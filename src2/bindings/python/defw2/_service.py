"""Serving from Python.

No Margo thread ever executes Python. The C handler decodes a request,
puts it on the host's queue and parks on an Argobots eventual. The worker
threads here block in defw2_service_next_call with the interpreter lock
released, run the service method, and answer, which wakes the handler to
encode the reply. Python runs on Python threads, Margo runs on Margo
threads, and the queue is the only thing they share.

The cost is one hand-off per call, which the benchmark measures and the
server span reports as its queue event.
"""

import threading
import warnings

from ._defw2 import ffi, lib
from ._echo import API_ECHO, PROVIDER_ECHO
from ._runtime import _check

__all__ = ['ServiceHost']


class ServiceHost:
	"""A provider whose methods are answered by Python.

	A handler is either a callable taking (method, request) or an object
	whose method names match the API's, each taking the request bytes.

	CLOSE_TIMEOUT is how long close() waits for a worker to leave a
	handler before it gives up and leaks the service. Raise it on a host
	whose handlers are legitimately slow.
	"""

	CLOSE_TIMEOUT = 5

	def __init__(self, runtime, service_id, service_type=API_ECHO,
		     provider_id=PROVIDER_ECHO, depth=0):
		self._runtime = runtime
		self._workers = []
		self._stopping = threading.Event()

		out = ffi.new('defw2_service_t **')
		_check(lib.defw2_service_create(runtime.handle,
						service_id.encode(),
						service_type.encode(),
						provider_id, out),
		       'creating service ' + service_id)
		self._svc = out[0]
		_check(lib.defw2_service_queue_open(self._svc, depth),
		       'opening the call queue')
		_check(lib.defw2_echo_bind(self._svc, ffi.NULL),
		       'binding ' + service_type)

	@property
	def address(self):
		return ffi.string(
			lib.defw2_service_address(self._svc)).decode()

	def start(self, handler, workers=1, poll_ms=200):
		"""Begin serving on background threads and return."""
		for index in range(workers):
			thread = threading.Thread(
				target=self._drain, args=(handler, poll_ms),
				name='defw2-worker-{}'.format(index),
				daemon=True)
			thread.start()
			self._workers.append(thread)

	def serve(self, handler, workers=1, poll_ms=200):
		"""Serve until stop(), on this thread and workers - 1 others."""
		self.start(handler, max(0, workers - 1), poll_ms)
		self._drain(handler, poll_ms)
		self.join()

	def _drain(self, handler, poll_ms):
		holder = ffi.new('defw2_call_t **')
		length = ffi.new('size_t *')

		while not self._stopping.is_set():
			# The interpreter lock is released for this wait, so
			# other Python threads keep running.
			rc = lib.defw2_service_next_call(self._svc, poll_ms,
							 holder)
			if rc == lib.DEFW2_ERR_NOT_FOUND:
				break		# the queue closed
			if rc != lib.DEFW2_OK:
				continue	# nothing arrived in time

			call = holder[0]
			data = lib.defw2_call_request(call, length)
			request = (bytes(ffi.buffer(data, length[0]))
				   if length[0] else b'')
			method = ffi.string(
				lib.defw2_call_method(call)).decode()
			try:
				reply = self._invoke(handler, method, request)
			except Exception as exc:	# noqa: BLE001
				# A service that raises answers with a status
				# rather than leaving its caller waiting.
				lib.defw2_service_fail(
					call, lib.DEFW2_ERR_INTERNAL,
					lib.DEFW2_CAT_PROVIDER_FAILURE,
					'{}: {}'.format(type(exc).__name__,
							exc).encode())
				continue
			if reply is None:
				reply = b''
			elif isinstance(reply, str):
				reply = reply.encode()
			lib.defw2_service_respond(call, reply, len(reply))

	@staticmethod
	def _invoke(handler, method, request):
		if callable(handler):
			return handler(method, request)
		bound = getattr(handler, method, None)
		if bound is None:
			raise AttributeError(
				'{} has no method {}'.format(
					type(handler).__name__, method))
		return bound(request)

	def stop(self):
		"""Release the workers. Calls already waiting are failed."""
		self._stopping.set()
		lib.defw2_service_queue_close(self._svc)

	def join(self, timeout=None):
		"""Wait for the workers. True when every one of them has left."""
		for thread in self._workers:
			thread.join(timeout)
		self._workers = [t for t in self._workers if t.is_alive()]
		return not self._workers

	def close(self):
		if self._svc is None:
			return
		self.stop()
		if not self.join(timeout=self.CLOSE_TIMEOUT):
			# A worker still inside a handler owns the queue that
			# defw2_service_destroy would free, along with the
			# mutex and condition variable it is waiting on.
			# Freeing them under a live thread corrupts memory,
			# so the allocation is given up instead. The workers
			# are daemons, so this does not hold up exit.
			warnings.warn(
				'defw2: a service worker outlasted the {}s '
				'close, so the service was left allocated '
				'rather than freed under it'.format(
					self.CLOSE_TIMEOUT),
				RuntimeWarning, stacklevel=2)
			self._svc = None
			return
		lib.defw2_service_destroy(self._svc)
		self._svc = None

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False
