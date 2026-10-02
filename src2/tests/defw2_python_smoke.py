#!/usr/bin/env python3
"""Does a Python service answer a Python client?

A server runtime with a queued service, a worker thread draining it, and a
client runtime calling in, all in one process over na+sm. It also checks
the property the whole design rests on: that a blocking C call releases the
interpreter lock, so a worker waiting for work does not stop the rest of
Python.
"""

import sys
import threading
import time
import warnings

import defw2

PAYLOAD = bytes(range(64))
CALLS = 200

failures = []


def check(what, ok):
	print('{:<44} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)


class Reverser:
	"""A service class, the shape the design's Python services take."""

	def __init__(self):
		self.served = 0

	def echo(self, request):
		self.served += 1
		if request == b'refuse':
			raise ValueError('the service said no')
		return request[::-1]


def count_while_workers_wait(seconds):
	"""How much Python runs while every worker is blocked inside C.

	The workers are idle, so both are sitting in defw2_service_next_call
	waiting for the poll to expire. If that call held the interpreter
	lock this loop would barely advance.
	"""
	ticks = 0
	deadline = time.monotonic() + seconds
	while time.monotonic() < deadline:
		ticks += 1
	return ticks


def check_close_does_not_free_under_a_worker():
	"""A worker still in a handler must outlive close(), not be freed under.

	close() frees the queue, the mutex and the condition variable that a
	worker uses. A handler that outlasts the wait used to be freed under,
	which corrupts memory instead of leaking it. The handler here is held
	open until close() has returned, so nothing about this races.
	"""
	entered = threading.Event()
	release = threading.Event()

	class Slow:
		def echo(self, request):
			entered.set()
			release.wait(30)
			return request

	server = defw2.Runtime(role='server', node_name='py-slow')
	host = defw2.ServiceHost(server, 'py-slow-echo')
	host.CLOSE_TIMEOUT = 0.2
	host.start(Slow(), workers=1)

	client = defw2.Runtime(role='client', node_name='py-slow-client')
	echo = defw2.Echo(client, host.address)

	def call():
		try:
			echo.echo(b'slow')
		except defw2.DefwError:
			# the queue closes under it, which is expected
			pass

	caller = threading.Thread(target=call, daemon=True)
	caller.start()
	check('the slow handler is running', entered.wait(10))

	with warnings.catch_warnings(record=True) as caught:
		warnings.simplefilter('always')
		host.close()
	warned = [w for w in caught if w.category is RuntimeWarning]
	check('close warns rather than freeing under a worker',
	      len(warned) == 1)
	check('the warning says the service was left allocated',
	      bool(warned) and 'left allocated' in str(warned[0].message))

	release.set()
	caller.join(10)
	check('the slow caller finished', not caller.is_alive())

	echo.close()
	client.close()
	server.close()


def check_host_names_resolve():
	"""An ofi+tcp address may name its host, as v1 deployments do.

	Mercury's OFI plugins look up numeric addresses only, so the binding
	resolves a name first. Without that, the directory address v2 composes
	from DEFW_PARENT_HOSTNAME cannot be reached.
	"""
	server = defw2.Runtime(role='server', address='ofi+tcp://127.0.0.1',
			       node_name='py-tcp-service')
	host = defw2.ServiceHost(server, 'py-tcp-echo')
	host.start(Reverser())
	named = host.address.replace('127.0.0.1', 'localhost')
	client = defw2.Runtime(role='client', address='ofi+tcp://127.0.0.1',
			       node_name='py-tcp-client')
	try:
		with defw2.Echo(client, named) as echo:
			ok = echo.echo(PAYLOAD) == PAYLOAD[::-1]
	except defw2.DefwError:
		ok = False
	check('an ofi+tcp address may name its host, {}'.format(named), ok)
	client.close()
	host.close()
	server.close()


def main():
	print('defw2', defw2.version())

	server = defw2.Runtime(role='server', node_name='py-service')
	host = defw2.ServiceHost(server, 'py-echo')
	service = Reverser()
	host.start(service, workers=2)
	print('  serving at {}'.format(host.address))

	client = defw2.Runtime(role='client', node_name='py-client')
	echo = defw2.Echo(client, host.address)

	check('a python service answers a python client',
	      echo.echo(PAYLOAD) == PAYLOAD[::-1])
	check('empty payloads work', echo.echo(b'') == b'')

	ok = all(echo.echo(PAYLOAD) == PAYLOAD[::-1] for _ in range(CALLS))
	check('{} calls in a row'.format(CALLS), ok)

	try:
		echo.echo(b'refuse')
		raised = None
	except defw2.DefwError as error:
		raised = error
	check('a raising service becomes a status',
	      raised is not None and raised.category == 'provider-failure')
	check('the message survives the trip',
	      raised is not None and 'the service said no' in raised.message)

	# echo_bulk stays in C even on a queued service, so it comes back
	# unchanged rather than reversed.
	payload, moved = echo.echo_bulk(PAYLOAD)
	check('bulk stays on the C path', payload == PAYLOAD)
	check('bulk counts both directions', moved == 2 * len(PAYLOAD))

	check('the service saw every call',
	      service.served == CALLS + 3)

	# Both workers are idle now, so both are blocked inside C.
	ticks = count_while_workers_wait(0.5)
	check('a blocking call releases the interpreter lock', ticks > 50000)
	print('  {} python iterations while 2 workers waited in C'.format(
		ticks))

	stats = defw2.process_stats()
	check('process stats are readable', stats['peak_rss_kib'] > 0)

	echo.close()
	client.close()
	host.close()
	server.close()

	check_close_does_not_free_under_a_worker()
	check_host_names_resolve()

	print('PYTHON SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main())
