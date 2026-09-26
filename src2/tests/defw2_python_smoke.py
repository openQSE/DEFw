#!/usr/bin/env python3
"""Does a Python service answer a Python client?

A server runtime with a queued service, a worker thread draining it, and a
client runtime calling in, all in one process over na+sm. It also checks
the property the whole design rests on: that a blocking C call releases the
interpreter lock, so a worker waiting for work does not stop the rest of
Python.
"""

import sys
import time

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

	print('PYTHON SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main())
