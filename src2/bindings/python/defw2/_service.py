"""Serving from Python.

No Margo thread ever executes Python. The C handler decodes a request,
puts it on the host's queue and parks on an Argobots eventual. The worker
threads here block in defw2_service_next_call with the interpreter lock
released, run the service method, and answer, which wakes the handler to
encode the reply. Python runs on Python threads, Margo runs on Margo
threads, and the queue is the only thing they share.

A host serves one service identity and any number of its APIs, each on its
own provider with its own queue and its own workers. So a QPM answers
is_ready from its control queue while its execution queue is full, which a
single queue could not promise.

Echo calls carry bytes, which a method takes and returns. A typed API's
calls, such as the QPM's, carry structures: a method takes a Request of
plain Python values and returns a dict, and both are read and written
straight from and into the C structures, with no encoding in between.

The cost is one hand-off per call, which the benchmark measures and the
server span reports as its queue event.
"""

import threading
import warnings

from ._defw2 import ffi, lib
from ._dir import build_record
from ._echo import API_ECHO, PROVIDER_ECHO
from ._qpm import (
	API_QPM_ADMISSION,
	API_QPM_CONTROL,
	API_QPM_EXECUTION,
	DEFAULT_PROVIDERS,
	QPM_APIS,
	QPM_VERSION,
	read_request,
	typed_api,
	write_answer,
)
from ._runtime import CATEGORY_CODE, DefwError, Runtime, _check, _Kept

__all__ = ['ServiceHost']

# How each API registers its methods on a provider, and what its binding
# is called in a directory record.
_BIND = {
	API_ECHO: lib.defw2_echo_bind,
	API_QPM_CONTROL: lib.defw2_qpm_control_bind,
	API_QPM_ADMISSION: lib.defw2_qpm_admission_bind,
	API_QPM_EXECUTION: lib.defw2_qpm_execution_bind,
}
_DEFAULT_PROVIDER = dict(DEFAULT_PROVIDERS, **{API_ECHO: PROVIDER_ECHO})
_BINDING_NAME = {
	API_ECHO: 'echo',
	API_QPM_CONTROL: 'control',
	API_QPM_ADMISSION: 'admission',
	API_QPM_EXECUTION: 'execution',
}
# The major version a binding declares in the directory.
_VERSION_MAJOR = dict({api: QPM_VERSION >> 16 for api in QPM_APIS},
		      **{API_ECHO: 0})


class _Endpoint:
	"""One API of the service: its provider, its queue, its workers."""

	def __init__(self, api, provider_id, depth, svc):
		self.api = api
		self.provider_id = provider_id
		self.depth = depth
		self.svc = svc
		self.workers = []


class ServiceHost:
	"""A service whose APIs are answered by Python.

	With apis left out it serves the one API service_type names on
	provider_id, which is how an echo service is written. Given apis, a
	list of API names or (api, provider_id) or (api, provider_id, depth)
	tuples, it serves each of them on its own provider:

		host = defw2.ServiceHost(rt, 'qpm:fake:fake-20q', 'qfw.qpm',
					 apis=defw2.QPM_APIS)

	A handler is a callable taking (method, request) or an object whose
	method names match the APIs', each taking the request. For echo the
	request is bytes; for a typed API it is a Request and the method
	returns a dict.

	CLOSE_TIMEOUT is how long close() waits for a worker to leave a
	handler before it gives up and leaks the service. Raise it on a host
	whose handlers are legitimately slow.
	"""

	CLOSE_TIMEOUT = 5

	def __init__(self, runtime, service_id, service_type=API_ECHO,
		     provider_id=None, depth=0, apis=None):
		self._runtime = runtime
		self._owned_runtime = None
		self.service_id = service_id
		self.service_type = service_type
		self._stopping = threading.Event()
		self._agent = None
		self._endpoints = []

		if apis is None:
			apis = [(service_type, provider_id, depth)]
		try:
			for spec in apis:
				self._add(spec, depth)
		except Exception:
			self._destroy()
			raise

	def _add(self, spec, default_depth):
		if isinstance(spec, str):
			spec = (spec,)
		api = spec[0]
		provider = spec[1] if len(spec) > 1 else None
		depth = spec[2] if len(spec) > 2 else default_depth
		if api not in _BIND:
			raise ValueError('no binding for API ' + api)
		if provider is None:
			provider = _DEFAULT_PROVIDER[api]

		out = ffi.new('defw2_service_t **')
		_check(lib.defw2_service_create(self._runtime.handle,
						self.service_id.encode(),
						self.service_type.encode(),
						provider, out),
		       'creating {} for {}'.format(api, self.service_id))
		endpoint = _Endpoint(api, provider, depth, out[0])
		self._endpoints.append(endpoint)
		_check(lib.defw2_service_queue_open(endpoint.svc, depth),
		       'opening the call queue for ' + api)
		_check(_BIND[api](endpoint.svc, ffi.NULL), 'binding ' + api)

	@classmethod
	def from_environment(cls, service_id, service_type, apis=None,
			     selector=None, properties=None, node_name=None,
			     register=True):
		"""A host on a server runtime of its own, from the environment.

		The runtime reads the DEFW_ and DEFW2_ names, and the host
		registers with the directory they point at, when there is one.
		close() then deregisters and finalizes the runtime too.
		"""
		runtime = Runtime(role='server', node_name=node_name)
		try:
			host = cls(runtime, service_id, service_type, apis=apis)
		except Exception:
			runtime.close()
			raise
		host._owned_runtime = runtime
		if register and runtime.dirsvc:
			try:
				host.register(selector=selector,
					      properties=properties)
			except Exception:
				host.close()
				raise
		return host

	@property
	def runtime(self):
		return self._runtime

	@property
	def address(self):
		return ffi.string(
			lib.defw2_service_address(self._endpoints[0].svc)).decode()

	@property
	def providers(self):
		"""Which provider serves each API."""
		return {e.api: e.provider_id for e in self._endpoints}

	# --- the directory

	def register(self, directory=None, selector=None, properties=None,
		     binding_names=None, interval_ms=0, service_id=None):
		"""Register in the directory and stay there until close().

		The C agent registers the record, heartbeats on a Margo timer,
		registers again if the directory forgets it, and deregisters
		on close. properties are stringified, since the directory
		keeps names and values and defines neither. service_id names
		the record when it is not the host's own, for a service that
		only knows its name once it is serving, as a v1 QPM does.
		"""
		if self._agent is not None:
			raise RuntimeError('this host is already registered')
		address = directory or self._runtime.dirsvc
		if not address:
			raise DefwError(lib.DEFW2_ERR_CONFIG, 'not-found',
					'no directory to register with')
		names = dict(_BINDING_NAME, **(binding_names or {}))
		bindings = [(names[e.api], e.api, _VERSION_MAJOR[e.api],
			     e.provider_id) for e in self._endpoints]
		kept = _Kept()
		record = build_record(
			kept, service_id or self.service_id, self.service_type,
			bindings, selector,
			{key: str(value)
			 for key, value in (properties or {}).items()})
		out = ffi.new('defw2_dir_agent_t **')
		_check(lib.defw2_dir_agent_start(self._endpoints[0].svc,
						 address.encode(), record,
						 interval_ms, out),
		       'registering {} at {}'.format(self.service_id, address))
		self._agent = out[0]

	@property
	def generation(self):
		"""What the directory assigned, 0 before a registration."""
		if self._agent is None:
			return 0
		return lib.defw2_dir_agent_generation(self._agent)

	def _deregister(self):
		# Before anything else stops: deregistering is an RPC, and an
		# agent stopped after Margo has gone cannot say goodbye.
		if self._agent is not None:
			lib.defw2_dir_agent_stop(self._agent)
			self._agent = None

	# --- serving

	def _worker_count(self, workers, endpoint):
		if isinstance(workers, dict):
			return workers.get(endpoint.api, 1)
		return workers

	def start(self, handler, workers=1, poll_ms=200):
		"""Begin serving on background threads and return.

		workers is a count per API, or a dict of counts by API name.
		"""
		for endpoint in self._endpoints:
			for index in range(self._worker_count(workers, endpoint)):
				thread = threading.Thread(
					target=self._drain,
					args=(endpoint, handler, poll_ms),
					name='defw2-{}-{}'.format(
						endpoint.api.rsplit('.', 1)[-1],
						index),
					daemon=True)
				thread.start()
				endpoint.workers.append(thread)

	def serve(self, handler, workers=1, poll_ms=200):
		"""Serve until stop(). Blocks the calling thread, which a
		signal handler may interrupt to call stop()."""
		self.start(handler, workers, poll_ms)
		while not self._stopping.wait(0.5):
			pass
		self.join()

	def _drain(self, endpoint, handler, poll_ms):
		holder = ffi.new('defw2_call_t **')
		length = ffi.new('size_t *')
		typed = typed_api(endpoint.api)

		while not self._stopping.is_set():
			# The interpreter lock is released for this wait, so
			# other Python threads keep running.
			rc = lib.defw2_service_next_call(endpoint.svc, poll_ms,
							 holder)
			if rc == lib.DEFW2_ERR_NOT_FOUND:
				break  # the queue closed
			if rc != lib.DEFW2_OK:
				continue  # nothing arrived in time

			call = holder[0]
			method = ffi.string(
				lib.defw2_call_method(call)).decode()
			if typed:
				self._answer_typed(endpoint.api, method, call,
						   handler)
			else:
				self._answer_bytes(method, call, length,
						   handler)

	def _answer_bytes(self, method, call, length, handler):
		data = lib.defw2_call_request(call, length)
		request = bytes(ffi.buffer(data, length[0])) if length[0] else b''
		try:
			reply = self._invoke(handler, method, request)
		except Exception as exc:  # noqa: BLE001
			_fail(call, exc)
			return
		if reply is None:
			reply = b''
		elif isinstance(reply, str):
			reply = reply.encode()
		lib.defw2_service_respond(call, reply, len(reply))

	def _answer_typed(self, api, method, call, handler):
		try:
			request = read_request(api, method, call)
			if request is None:
				raise AttributeError(method)
			answer = self._invoke(handler, method, request)
			write_answer(api, method, call, answer)
		except _Unserved:
			# The same words the C provider uses for a method an
			# operations table leaves out.
			lib.defw2_service_fail(call, lib.DEFW2_ERR_NOT_FOUND,
					       lib.DEFW2_CAT_NOT_FOUND,
					       b'this QPM does not serve that '
					       b'method')
			return
		except Exception as exc:  # noqa: BLE001
			_fail(call, exc)
			return
		lib.defw2_service_respond(call, ffi.NULL, 0)

	@staticmethod
	def _invoke(handler, method, request):
		if callable(handler) and not hasattr(handler, method):
			return handler(method, request)
		bound = getattr(handler, method, None)
		if bound is None:
			raise _Unserved(method)
		return bound(request)

	def stop(self):
		"""Release the workers. Calls already waiting are failed."""
		self._stopping.set()
		for endpoint in self._endpoints:
			lib.defw2_service_queue_close(endpoint.svc)

	def join(self, timeout=None):
		"""Wait for the workers. True when every one of them has left."""
		alive = False
		for endpoint in self._endpoints:
			for thread in endpoint.workers:
				thread.join(timeout)
			endpoint.workers = [t for t in endpoint.workers
					    if t.is_alive()]
			alive = alive or bool(endpoint.workers)
		return not alive

	def close(self):
		if not self._endpoints:
			return
		self._deregister()
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
			self._endpoints = []
			return
		self._destroy()
		if self._owned_runtime is not None:
			self._owned_runtime.close()
			self._owned_runtime = None

	def _destroy(self):
		self._deregister()
		for endpoint in self._endpoints:
			lib.defw2_service_destroy(endpoint.svc)
		self._endpoints = []

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False


class _Unserved(Exception):
	"""A method the service object does not have."""

	def __str__(self):
		return 'the service has no method {}'.format(self.args[0])


def _fail(call, exc):
	"""Answer a call with what the service raised.

	A ServiceError, or any DefwError, says which category the caller
	sees. Anything else is the service's own failure.
	"""
	if isinstance(exc, DefwError) and exc.category in CATEGORY_CODE:
		code = exc.code if exc.code != lib.DEFW2_OK else \
			lib.DEFW2_ERR_INTERNAL
		category = CATEGORY_CODE[exc.category]
		message = exc.message or ''
	else:
		code = lib.DEFW2_ERR_INTERNAL
		category = lib.DEFW2_CAT_PROVIDER_FAILURE
		message = '{}: {}'.format(type(exc).__name__, exc)
	lib.defw2_service_fail(call, code, category, message.encode())
