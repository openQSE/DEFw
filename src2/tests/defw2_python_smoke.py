#!/usr/bin/env python3
"""Does a Python service answer a Python client?

A server runtime with a queued service, a worker thread draining it, and a
client runtime calling in, all in one process over na+sm. It also checks
the property the whole design rests on: that a blocking C call releases the
interpreter lock, so a worker waiting for work does not stop the rest of
Python.

And the rules for documents: an API with no typed methods answers nothing
but documents, a document reaches a handler only through its document(),
and an answer JSON cannot carry fails the call.
"""

import sys
import threading
import time
import warnings

import defw2

try:
	import numpy
except ImportError:
	numpy = None

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


class Documents:
	"""A handler with documents, and a public method no caller may
	reach by naming it."""

	def __init__(self):
		self.closed = False

	def document(self, api, method, request, traceparent):
		if method == 'echo':
			return {'api': api, 'request': request}
		if method == 'trace':
			return traceparent
		if method == 'numpy':
			return {'f': numpy.float32(1.5), 'i': numpy.int64(7),
				'a': numpy.arange(3), 'got': request}
		if method == 'raw':
			return b'bytes'
		if method == 'raise':
			raise ValueError('the documents said no')
		raise defw2.ServiceError('not-found', 'no document ' + method)

	def close(self):
		self.closed = True


def check_documents():
	server = defw2.Runtime(role='server', node_name='py-docs')
	host = defw2.ServiceHost(server, 'py-docs', 'qfw.test',
				 apis=[('qfw.test.docs', 21)])
	handler = Documents()
	host.start(handler)
	bare = defw2.ServiceHost(server, 'py-bare', 'qfw.test',
				 apis=[('qfw.test.bare', 23)])
	bare.start(lambda method, request: request)
	echo = defw2.ServiceHost(server, 'py-docs-echo')
	echo.start(Reverser())

	client = defw2.Runtime(role='client', node_name='py-docs-client')
	where = host.address
	qpm = defw2.QPM(client, bindings={
		'qfw.test.docs': (where, 21), 'qfw.test.bare': (where, 23),
		defw2.API_ECHO: (where, defw2.PROVIDER_ECHO)})

	request = {'x': [1, 2.5, 'three', None, False], 'y': {'z': {}}}
	check('an API of documents alone answers one',
	      qpm.document('qfw.test.docs', 'echo', request) ==
	      {'api': 'qfw.test.docs', 'request': request})
	check('no request arrives as no arguments',
	      qpm.document('qfw.test.docs', 'echo') ==
	      {'api': 'qfw.test.docs', 'request': {}})
	trace = '00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01'
	check("the caller's trace context reaches the handler",
	      qpm.document('qfw.test.docs', 'trace', traceparent=trace) ==
	      trace)

	def category(*args):
		try:
			qpm.document(*args)
		except defw2.DefwError as error:
			return error.category, error.message
		return None, None

	if numpy is not None:
		try:
			got = qpm.document('qfw.test.docs', 'numpy',
					   {'n': numpy.uint16(9)})
		except (TypeError, defw2.DefwError) as error:
			got = error
		check('a numpy value goes as the Python value it holds',
		      got == {'f': 1.5, 'i': 7, 'a': [0, 1, 2],
			      'got': {'n': 9}})
	raised = category('qfw.test.docs', 'raw')
	check('an answer JSON cannot carry fails the call',
	      raised[0] == 'provider-failure' and
	      'JSON cannot carry' in raised[1])
	raised = category('qfw.test.docs', 'raise')
	check("a handler's exception becomes its status",
	      raised[0] == 'provider-failure' and
	      'the documents said no' in raised[1])
	check('a document names a method, never a handler attribute',
	      category('qfw.test.docs', 'close')[0] == 'not-found' and
	      not handler.closed)
	raised = category('qfw.test.bare', 'echo')
	check('a handler with no document() answers none',
	      raised[0] == 'not-found' and 'answers no documents' in raised[1])
	check('echo answers no documents at all',
	      category(defw2.API_ECHO, 'echo')[0] == 'not-found')

	try:
		defw2.ServiceHost(server, 'py-nowhere', 'qfw.test',
				  apis=['qfw.test.nowhere'])
		named = False
	except ValueError:
		named = True
	check('an API with no default provider needs one named', named)

	qpm.close()
	client.close()
	host.close()
	bare.close()
	echo.close()
	server.close()


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


def check_spindown():
	"""A runtime takes the spindown it is given, and only one there is.
	The C runtime test reads what Margo then runs with."""
	try:
		with defw2.Runtime(role='server', node_name='py-spin',
				   progress_spindown_ms=5) as spinning:
			started = spinning.handle is not None
	except defw2.DefwError:
		started = False
	check('a runtime starts with the spindown it is given', started)
	try:
		defw2.Runtime(role='server', progress_spindown_ms=-1)
		refused = False
	except ValueError:
		refused = True
	check('and refuses one that is not a spindown', refused)


def check_bulk_pool():
	"""A runtime takes the bulk pool budget it is given, and only one
	there is. The C pool test checks what the pool then does."""
	try:
		with defw2.Runtime(role='server', node_name='py-pool',
				   bulk_pool_mib=64) as pooled:
			started = pooled.handle is not None
	except defw2.DefwError:
		started = False
	check('a runtime starts with the bulk pool it is given', started)
	try:
		defw2.Runtime(role='server', bulk_pool_mib=-1)
		refused = False
	except ValueError:
		refused = True
	check('and refuses one that is not a budget', refused)


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

	check_documents()
	check_close_does_not_free_under_a_worker()
	check_host_names_resolve()
	check_spindown()
	check_bulk_pool()

	print('PYTHON SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main())
