"""The QPM APIs from Python: a typed client, and typed calls for a service.

The client binds the three APIs, control, admission and execution, and calls
the C stubs, so a Python caller and a C caller send the same bytes:

	qpm = defw2.QPM(rt, address)
	task = qpm.async_run(qasm, num_qubits=5, num_shots=1024,
			     reservation_id=rid)
	done = qpm.read_cq(cid=task.cid, reservation_id=rid,
			   result=numpy.empty(32, numpy.complex128))

A service answers the same methods. ServiceHost hands each one a Request of
plain Python values, read straight out of the C request structure with no
encoding in between, and writes the dictionary the method returns into the
C answer structure the same way:

	class FakeQPM:
		def is_ready(self, request):
			return {'state': 'running', 'ready': True}

A method with no typed form goes as a document, a dict of named arguments
in and the service's JSON out:

	answer = qpm.document(API_QPM_CONTROL, 'test')

Every answer has the typed fields and extra, a JSON object carrying the rest
of what the service said. Outcomes such as INVALID_RESERVATION are data and
come back in the answer; a call that fails raises DefwError with the status
category, and a service fails one by raising ServiceError.

A completion event is QPM_COMPLETION. Its payload is a Task, the record
read_cq answers with, its statevector described and never carried. A caller
registers a sink for them, and a service publishes one with the dict it
would answer read_cq with:

	qpm.register_event_notification(sink, type='done', tag='job-7',
					reservation_id=rid)
	publisher.publish(QPM_COMPLETION, request.target, task,
			  type=request.type)
"""

import json

from . import _doc
from ._defw2 import ffi, lib
from ._event import EventKind, EventSink, EventTarget
from ._runtime import DefwError, _status_out, _take_status, _text

__all__ = [
	'QPM', 'Request', 'Tensor', 'ServiceStatus', 'Decision',
	'Reservation', 'Task', 'API_QPM_CONTROL', 'API_QPM_ADMISSION',
	'API_QPM_EXECUTION', 'QPM_APIS', 'PROVIDER_QPM_CONTROL',
	'PROVIDER_QPM_ADMISSION', 'PROVIDER_QPM_EXECUTION', 'QPM_VERSION',
	'DTYPE', 'QPM_COMPLETION',
]

API_QPM_CONTROL = 'qfw.qpm.control'
API_QPM_ADMISSION = 'qfw.qpm.admission'
API_QPM_EXECUTION = 'qfw.qpm.execution'
QPM_APIS = (API_QPM_CONTROL, API_QPM_ADMISSION, API_QPM_EXECUTION)

PROVIDER_QPM_CONTROL = lib.DEFW2_PROVIDER_QPM_CONTROL
PROVIDER_QPM_ADMISSION = lib.DEFW2_PROVIDER_QPM_ADMISSION
PROVIDER_QPM_EXECUTION = lib.DEFW2_PROVIDER_QPM_EXECUTION
DEFAULT_PROVIDERS = {
	API_QPM_CONTROL: PROVIDER_QPM_CONTROL,
	API_QPM_ADMISSION: PROVIDER_QPM_ADMISSION,
	API_QPM_EXECUTION: PROVIDER_QPM_EXECUTION,
}
QPM_VERSION = lib.DEFW2_QPM_VERSION

# Element types by name, and the numpy type each one is, for a statevector.
DTYPE = {
	'u8': lib.DEFW2_DTYPE_U8, 'i32': lib.DEFW2_DTYPE_I32,
	'i64': lib.DEFW2_DTYPE_I64, 'f32': lib.DEFW2_DTYPE_F32,
	'f64': lib.DEFW2_DTYPE_F64, 'c64': lib.DEFW2_DTYPE_C64,
	'c128': lib.DEFW2_DTYPE_C128,
}
_NUMPY_TYPE = {
	lib.DEFW2_DTYPE_U8: 'u1', lib.DEFW2_DTYPE_I32: 'i4',
	lib.DEFW2_DTYPE_I64: 'i8', lib.DEFW2_DTYPE_F32: 'f4',
	lib.DEFW2_DTYPE_F64: 'f8', lib.DEFW2_DTYPE_C64: 'c8',
	lib.DEFW2_DTYPE_C128: 'c16',
}

try:
	import numpy as _np
except ImportError:  # the binding works without it, as bytes
	_np = None


def _json_text(value):
	"""extra as JSON text: a str is taken as already JSON."""
	if value is None or isinstance(value, str):
		return value
	return json.dumps(value, separators=(',', ':'))


def _parse(text):
	return None if text is None else json.loads(text)


# --- answers ---------------------------------------------------------


class _Answer:
	"""Typed fields as attributes, and extra parsed when it is asked for."""

	_fields = ()

	def __init__(self, **values):
		for field in self._fields:
			setattr(self, field, values.get(field))
		self.extra_json = values.get('extra_json')
		self._extra = None

	@property
	def extra(self):
		if self._extra is None and self.extra_json is not None:
			self._extra = _parse(self.extra_json)
		return self._extra

	def as_dict(self):
		"""The answer as a service would return it."""
		values = {field: getattr(self, field) for field in self._fields}
		values['extra'] = self.extra
		return values

	def __repr__(self):
		shown = ', '.join('{}={!r}'.format(field, getattr(self, field))
				  for field in self._fields)
		return '{}({})'.format(type(self).__name__, shown)


class ServiceStatus(_Answer):
	_fields = ('state', 'ready', 'initialized', 'accepting_requests',
		   'provider_ready', 'active_task_count',
		   'active_reservation_count')


class Decision(_Answer):
	_fields = ('decision', 'reservation_id', 'request_id', 'reason',
		   'reason_code', 'retry_after_ns', 'message')


class Reservation(_Answer):
	_fields = ('reservation_id', 'state', 'created_at_ns', 'expires_at_ns')


class Tensor:
	"""What a bulk result is: element type, shape and length."""

	__slots__ = ('dtype', 'shape', 'nbytes')

	def __init__(self, dtype=0, shape=(), nbytes=0):
		self.dtype = dtype
		self.shape = tuple(shape)
		self.nbytes = nbytes

	def __repr__(self):
		return 'Tensor(dtype={}, shape={}, nbytes={})'.format(
			self.dtype, self.shape, self.nbytes)


class Task(_Answer):
	"""An execution answer. statevector_data is the delivered result.

	It is a view of the buffer the caller lent, shaped by the descriptor
	when numpy is there, so nothing is copied after the push.
	"""

	_fields = ('outcome', 'lifecycle_state', 'cid', 'qtask_id',
		   'reservation_id', 'reason', 'message', 'completion_ready',
		   'statevector', 'statevector_delivered')

	def __init__(self, **values):
		super().__init__(**values)
		self.statevector_data = values.get('statevector_data')


# --- the client --------------------------------------------------------


class _Call:
	"""What one call keeps alive: every C string it hands to a stub.

	Kept per call rather than on the client, so one client can be shared
	between threads.
	"""

	__slots__ = ('kept',)

	def __init__(self):
		self.kept = []

	def str(self, value):
		if value is None:
			return ffi.NULL
		if isinstance(value, str):
			value = value.encode('utf-8')
		held = ffi.new('char[]', bytes(value))
		self.kept.append(held)
		return held

	def json(self, value):
		return self.str(_json_text(value))

	def options(self, timeout_ms, traceparent):
		opts = ffi.new('defw2_call_opts_t *')
		opts.timeout_ms = timeout_ms
		opts.traceparent = self.str(traceparent)
		self.kept.append(opts)
		return opts

	def ctx(self, ctx, reservation_id, token):
		ctx.reservation_id = reservation_id or 0
		ctx.token = self.str(token)

	def lend(self, result):
		"""A result buffer: None, a byte count, or a writable buffer."""
		if result is None:
			return ffi.NULL, None
		buffer = bytearray(result) if isinstance(result, int) else result
		data = ffi.from_buffer(buffer, require_writable=True)
		lent = ffi.new('defw2_result_buffer_t *')
		lent.data = data
		lent.capacity = len(data)
		self.kept.extend((data, lent))
		return lent, buffer


def _tensor(cdata):
	rank = min(cdata.rank, lib.DEFW2_TENSOR_RANK_MAX)
	return Tensor(cdata.dtype, [cdata.shape[i] for i in range(rank)],
		      cdata.nbytes)


def _delivered(buffer, tensor):
	"""The delivered result, as a view of the buffer that holds it."""
	if _np is not None and tensor.dtype in _NUMPY_TYPE:
		array = _np.frombuffer(buffer, dtype=_NUMPY_TYPE[tensor.dtype],
				       count=tensor.nbytes //
				       _np.dtype(_NUMPY_TYPE[tensor.dtype]).itemsize)
		return array.reshape(tensor.shape)
	return memoryview(buffer)[:tensor.nbytes]


def _task_of(out, buffer=None):
	"""A Task from a C task, with its statevector in buffer when the C
	task says it was delivered there."""
	tensor = _tensor(out.statevector)
	delivered = bool(out.statevector_delivered)
	return Task(
		outcome=_text(out.outcome),
		lifecycle_state=_text(out.lifecycle_state),
		cid=_text(out.cid), qtask_id=out.qtask_id,
		reservation_id=out.reservation_id,
		reason=_text(out.reason),
		message=_text(out.message),
		completion_ready=bool(out.completion_ready),
		statevector=tensor,
		statevector_delivered=delivered,
		statevector_data=(_delivered(buffer, tensor)
				  if delivered else None),
		extra_json=_text(out.extra))


class QPM:
	"""A client of one QPM's three APIs.

	Bound by address, with the default providers or the ones given, or
	from a directory record with from_record, which is what a resolved
	QPM gives. Safe to share between threads: every call keeps what it
	hands to C alive on its own.
	"""

	def __init__(self, runtime, address=None, providers=None,
		     timeout_ms=60000, bindings=None):
		self._runtime = runtime
		self._timeout_ms = timeout_ms
		self._bindings = {}
		if bindings is None:
			chosen = dict(DEFAULT_PROVIDERS)
			chosen.update(providers or {})
			bindings = {api: (address, provider)
				    for api, provider in chosen.items()}
		try:
			for api, (where, provider) in bindings.items():
				out = ffi.new('defw2_binding_t **')
				rc = lib.defw2_binding_create(
					runtime.handle, where.encode(),
					provider, out)
				if rc != lib.DEFW2_OK:
					raise DefwError(rc, 'transport',
							'binding {} at {}'.format(
								api, where))
				self._bindings[api] = out[0]
		except Exception:
			self.close()
			raise

	@classmethod
	def from_record(cls, runtime, record, timeout_ms=60000):
		"""A client for the QPM a directory record describes."""
		bindings = {
			binding['api_id']: (record['address'],
					    binding['provider_id'])
			for binding in record['bindings']
			if binding['api_id'] in QPM_APIS
		}
		if not bindings:
			raise DefwError(lib.DEFW2_ERR_NOT_FOUND, 'not-found',
					'{} serves no QPM API'.format(
						record.get('service_id')))
		return cls(runtime, timeout_ms=timeout_ms, bindings=bindings)

	def _binding(self, api):
		binding = self._bindings.get(api)
		if binding is None:
			raise DefwError(lib.DEFW2_ERR_NOT_FOUND, 'not-found',
					'this client has no binding for ' + api)
		return binding

	def _invoke(self, what, api, stub, call, req, out, free, lent,
		    timeout_ms, traceparent):
		"""lent is None for a stub with no result buffer, and the
		buffer, or ffi.NULL, for one that takes it."""
		opts = call.options(self._timeout_ms if timeout_ms is None
				    else timeout_ms, traceparent)
		holder = _status_out()
		binding = self._binding(api)
		if lent is None:
			rc = stub(binding, req, opts, out, holder)
		else:
			rc = stub(binding, req, lent, opts, out, holder)
		status = _take_status(holder)
		if rc != lib.DEFW2_OK:
			free(out)
			raise DefwError(rc, 'transport', '{} failed: {}'.format(
				what, _text(lib.defw2_strerror(rc))))
		if not status.ok:
			free(out)
			raise DefwError(status.code, status.category,
					status.message)

	# --- control

	def _control(self, what, stub, reservation_id, token, timeout_ms,
		     traceparent):
		call = _Call()
		req = ffi.new('defw2_qpm_ctx_t *')
		call.ctx(req, reservation_id, token)
		out = ffi.new('defw2_qpm_service_status_t *')
		self._invoke(what, API_QPM_CONTROL, stub, call, req, out,
			     lib.defw2_qpm_service_status_free, None,
			     timeout_ms, traceparent)
		try:
			return ServiceStatus(
				state=_text(out.state), ready=bool(out.ready),
				initialized=bool(out.initialized),
				accepting_requests=bool(out.accepting_requests),
				provider_ready=bool(out.provider_ready),
				active_task_count=out.active_task_count,
				active_reservation_count=(
					out.active_reservation_count),
				extra_json=_text(out.extra))
		finally:
			lib.defw2_qpm_service_status_free(out)

	def is_ready(self, reservation_id=0, token=None, timeout_ms=None,
		     traceparent=None):
		return self._control('is_ready', lib.defw2_qpm_is_ready,
				     reservation_id, token, timeout_ms,
				     traceparent)

	def get_service_status(self, reservation_id=0, token=None,
			       timeout_ms=None, traceparent=None):
		return self._control('get_service_status',
				     lib.defw2_qpm_get_service_status,
				     reservation_id, token, timeout_ms,
				     traceparent)

	# --- admission

	def _decision(self, what, stub, call, req, timeout_ms, traceparent,
		      api=API_QPM_ADMISSION):
		out = ffi.new('defw2_qpm_decision_t *')
		self._invoke(what, api, stub, call, req, out,
			     lib.defw2_qpm_decision_free, None, timeout_ms,
			     traceparent)
		try:
			return Decision(
				decision=_text(out.decision),
				reservation_id=out.reservation_id,
				request_id=out.request_id,
				reason=_text(out.reason),
				reason_code=out.reason_code,
				retry_after_ns=out.retry_after_ns,
				message=_text(out.message),
				extra_json=_text(out.extra))
		finally:
			lib.defw2_qpm_decision_free(out)

	def reserve(self, request_id=0, user=None, job_id=None,
		    allocation_id=None, target_device_id=None, scope_id=None,
		    workload_kind=None, num_qubits=0, walltime_ns=0, ttl_ns=0,
		    task_class=None, extra=None, reservation_id=0, token=None,
		    timeout_ms=None, traceparent=None):
		"""Reserve. task_class is a dict of the defw2_qpm_task_class_t
		fields, or None."""
		call = _Call()
		req = ffi.new('defw2_qpm_reserve_req_t *')
		call.ctx(req.ctx, reservation_id, token)
		req.request_id = request_id or 0
		req.user = call.str(user)
		req.job_id = call.str(job_id)
		req.allocation_id = call.str(allocation_id)
		req.target_device_id = call.str(target_device_id)
		req.scope_id = call.str(scope_id)
		req.workload_kind = call.str(workload_kind)
		req.num_qubits = num_qubits or 0
		req.walltime_ns = walltime_ns or 0
		req.ttl_ns = ttl_ns or 0
		req.has_task_class = task_class is not None
		for field, value in (task_class or {}).items():
			setattr(req.task_class, field, value or 0)
		req.extra = call.json(extra)
		return self._decision('reserve', lib.defw2_qpm_reserve, call,
				      req, timeout_ms, traceparent)

	def renew(self, reservation_id, ttl_ns=0, extra=None, token=None,
		  timeout_ms=None, traceparent=None):
		call = _Call()
		req = ffi.new('defw2_qpm_renew_req_t *')
		call.ctx(req.ctx, reservation_id, token)
		req.ttl_ns = ttl_ns or 0
		req.extra = call.json(extra)
		return self._decision('renew', lib.defw2_qpm_renew, call, req,
				      timeout_ms, traceparent)

	def _close(self, what, stub, reservation_id, reason_code, token,
		   timeout_ms, traceparent):
		call = _Call()
		req = ffi.new('defw2_qpm_close_req_t *')
		call.ctx(req.ctx, reservation_id, token)
		req.reason_code = reason_code or 0
		return self._decision(what, stub, call, req, timeout_ms,
				      traceparent)

	def release(self, reservation_id, reason_code=0, token=None,
		    timeout_ms=None, traceparent=None):
		return self._close('release', lib.defw2_qpm_release,
				   reservation_id, reason_code, token,
				   timeout_ms, traceparent)

	def cancel(self, reservation_id, reason_code=0, token=None,
		   timeout_ms=None, traceparent=None):
		return self._close('cancel', lib.defw2_qpm_cancel,
				   reservation_id, reason_code, token,
				   timeout_ms, traceparent)

	def get_reservation(self, reservation_id, token=None, timeout_ms=None,
			    traceparent=None):
		call = _Call()
		req = ffi.new('defw2_qpm_ctx_t *')
		call.ctx(req, reservation_id, token)
		out = ffi.new('defw2_qpm_reservation_t *')
		self._invoke('get_reservation', API_QPM_ADMISSION,
			     lib.defw2_qpm_get_reservation, call, req, out,
			     lib.defw2_qpm_reservation_free, None, timeout_ms,
			     traceparent)
		try:
			return Reservation(
				reservation_id=out.reservation_id,
				state=_text(out.state),
				created_at_ns=out.created_at_ns,
				expires_at_ns=out.expires_at_ns,
				extra_json=_text(out.extra))
		finally:
			lib.defw2_qpm_reservation_free(out)

	# --- execution

	def _task(self, what, stub, call, req, result, lends, timeout_ms,
		  traceparent):
		lent, buffer = call.lend(result)
		out = ffi.new('defw2_qpm_task_t *')
		self._invoke(what, API_QPM_EXECUTION, stub, call, req, out,
			     lib.defw2_qpm_task_free, lent if lends else None,
			     timeout_ms, traceparent)
		try:
			return _task_of(out, buffer)
		finally:
			lib.defw2_qpm_task_free(out)

	def _run_req(self, call, circuit, circuit_format, num_qubits,
		     num_shots, compiler, return_statevector, run_timeout_ms,
		     cancel_on_timeout, extra, reservation_id, token):
		if isinstance(circuit, str):
			circuit = circuit.encode('utf-8')
		data = ffi.from_buffer(circuit)
		call.kept.append(data)
		req = ffi.new('defw2_qpm_run_req_t *')
		call.ctx(req.ctx, reservation_id, token)
		req.circuit.format = call.str(circuit_format)
		req.circuit.data = data
		req.circuit.len = len(data)
		req.num_qubits = num_qubits or 0
		req.num_shots = num_shots or 0
		req.compiler = call.str(compiler)
		req.return_statevector = bool(return_statevector)
		req.has_timeout = run_timeout_ms is not None
		req.timeout_ms = run_timeout_ms or 0
		req.cancel_on_timeout = bool(cancel_on_timeout)
		req.extra = call.json(extra)
		return req

	def async_run(self, circuit, circuit_format='openqasm2', num_qubits=0,
		      num_shots=0, compiler=None, return_statevector=False,
		      run_timeout_ms=None, cancel_on_timeout=False, extra=None,
		      reservation_id=0, token=None, timeout_ms=None,
		      traceparent=None):
		"""Submit a circuit: OpenQASM text or QPY bytes."""
		call = _Call()
		req = self._run_req(call, circuit, circuit_format, num_qubits,
				    num_shots, compiler, return_statevector,
				    run_timeout_ms, cancel_on_timeout, extra,
				    reservation_id, token)
		return self._task('async_run', lib.defw2_qpm_async_run, call,
				  req, None, False, timeout_ms, traceparent)

	def sync_run(self, circuit, circuit_format='openqasm2', num_qubits=0,
		     num_shots=0, compiler=None, return_statevector=False,
		     run_timeout_ms=None, cancel_on_timeout=False, extra=None,
		     result=None, reservation_id=0, token=None,
		     timeout_ms=None, traceparent=None):
		"""Run a circuit and wait. result is a buffer to lend for the
		statevector, or its size, or None."""
		call = _Call()
		req = self._run_req(call, circuit, circuit_format, num_qubits,
				    num_shots, compiler, return_statevector,
				    run_timeout_ms, cancel_on_timeout, extra,
				    reservation_id, token)
		return self._task('sync_run', lib.defw2_qpm_sync_run, call, req,
				  result, True, timeout_ms, traceparent)

	def _task_req(self, call, cid, qtask_id, reason, reservation_id,
		      token):
		req = ffi.new('defw2_qpm_task_req_t *')
		call.ctx(req.ctx, reservation_id, token)
		req.cid = call.str(cid)
		req.qtask_id = qtask_id or 0
		req.reason = call.str(reason)
		return req

	def read_cq(self, cid=None, qtask_id=0, result=None, reservation_id=0,
		    token=None, reason=None, timeout_ms=None, traceparent=None):
		"""Collect a completion. A statevector that does not fit what was
		lent leaves the completion queued, and the answer says the size
		a retry needs."""
		call = _Call()
		req = self._task_req(call, cid, qtask_id, reason, reservation_id,
				     token)
		return self._task('read_cq', lib.defw2_qpm_read_cq, call, req,
				  result, True, timeout_ms, traceparent)

	def peek_cq(self, cid=None, qtask_id=0, result=None, reservation_id=0,
		    token=None, reason=None, timeout_ms=None, traceparent=None):
		call = _Call()
		req = self._task_req(call, cid, qtask_id, reason, reservation_id,
				     token)
		return self._task('peek_cq', lib.defw2_qpm_peek_cq, call, req,
				  result, True, timeout_ms, traceparent)

	def task_status(self, cid=None, qtask_id=0, reservation_id=0,
			token=None, timeout_ms=None, traceparent=None):
		call = _Call()
		req = self._task_req(call, cid, qtask_id, None, reservation_id,
				     token)
		return self._task('task_status', lib.defw2_qpm_task_status,
				  call, req, None, False, timeout_ms, traceparent)

	def cancel_task(self, cid=None, qtask_id=0, reason=None,
			reservation_id=0, token=None, timeout_ms=None,
			traceparent=None):
		call = _Call()
		req = self._task_req(call, cid, qtask_id, reason,
				     reservation_id, token)
		return self._task('cancel_task', lib.defw2_qpm_cancel_task,
				  call, req, None, False, timeout_ms, traceparent)

	def delete_circuit(self, cid=None, qtask_id=0, reservation_id=0,
			   token=None, timeout_ms=None, traceparent=None):
		call = _Call()
		req = self._task_req(call, cid, qtask_id, None, reservation_id,
				     token)
		return self._task('delete_circuit', lib.defw2_qpm_delete_circuit,
				  call, req, None, False, timeout_ms,
				  traceparent)

	def register_event_notification(self, target, type=None, tag=None,
					extra=None, reservation_id=0,
					token=None, timeout_ms=None,
					traceparent=None):
		"""Ask the QPM for completion events, and return its Decision.

		target is an EventSink of this process, which this readies for
		completions first, or an EventTarget naming any sink. tag comes
		back on every event, so one sink can tell its registrations
		apart, and so does type, the caller's name for the events. A
		reservation limits them to its tasks. extra is a JSON object
		for the rest, such as QFw's filters. The QPM keeps the
		registration until a delivery to the sink fails, so closing the
		sink ends it.
		"""
		if isinstance(target, EventSink):
			target.accept(QPM_COMPLETION)
			target = target.target(tag)
		elif tag is not None:
			target = EventTarget(target[0], target[1], tag)
		address, provider_id, tag = target
		call = _Call()
		req = ffi.new('defw2_qpm_notify_req_t *')
		call.ctx(req.ctx, reservation_id, token)
		req.target.address = call.str(address)
		req.target.provider_id = provider_id
		req.target.tag = call.str(tag)
		req.type = call.str(type)
		req.extra = call.json(extra)
		return self._decision('register_event_notification',
				      lib.defw2_qpm_register_event_notification,
				      call, req, timeout_ms, traceparent,
				      api=API_QPM_EXECUTION)

	# --- documents

	def document(self, api, method, request=None, timeout_ms=None,
		     traceparent=None):
		"""Any method of one of the QPM's APIs as a document: request
		is a dict of named arguments, and the answer is the JSON the
		service answered with. This is how a method with no typed
		form is called."""
		call = _Call()
		opts = call.options(self._timeout_ms if timeout_ms is None
				    else timeout_ms, traceparent)
		return _doc.call(self._binding(api), api, method, request, opts)

	def close(self):
		for binding in self._bindings.values():
			lib.defw2_binding_free(binding)
		self._bindings = {}

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False


# --- typed calls, for a service --------------------------------------


class Request:
	"""A typed call's request, as plain Python values.

	Every request has reservation_id, token, result_capacity, the
	bytes the caller lent for a bulk result, 0 when it lent none, and
	traceparent, the W3C context the service's own work belongs under,
	None when the caller sent none. The rest depends on the method.
	extra is the request's JSON parsed, and extra_json the text it came
	as.
	"""

	def __init__(self, method, values, extra_json=None):
		self.method = method
		self.__dict__.update(values)
		self.extra_json = extra_json
		self._extra = None

	@property
	def extra(self):
		if self._extra is None and self.extra_json is not None:
			self._extra = _parse(self.extra_json)
		return self._extra

	def __repr__(self):
		shown = {key: value for key, value in self.__dict__.items()
			 if not key.startswith('_') and key != 'circuit'}
		return 'Request({})'.format(shown)


def _ctx_values(ctx):
	return {'reservation_id': ctx.reservation_id,
		'token': _text(ctx.token)}


def _read_ctx(pointer):
	req = ffi.cast('defw2_qpm_ctx_t *', pointer)
	return _ctx_values(req), None


def _read_reserve(pointer):
	req = ffi.cast('defw2_qpm_reserve_req_t *', pointer)
	values = _ctx_values(req.ctx)
	values.update(
		request_id=req.request_id, user=_text(req.user),
		job_id=_text(req.job_id),
		allocation_id=_text(req.allocation_id),
		target_device_id=_text(req.target_device_id),
		scope_id=_text(req.scope_id),
		workload_kind=_text(req.workload_kind),
		num_qubits=req.num_qubits, walltime_ns=req.walltime_ns,
		ttl_ns=req.ttl_ns, task_class=None)
	if req.has_task_class:
		tc = req.task_class
		values['task_class'] = {
			'count': tc.count, 'qubit_count': tc.qubit_count,
			'depth': tc.depth,
			'one_q_gate_count': tc.one_q_gate_count,
			'two_q_gate_count': tc.two_q_gate_count,
			'shots': tc.shots,
			'measurement_count': tc.measurement_count,
		}
	return values, _text(req.extra)


def _read_renew(pointer):
	req = ffi.cast('defw2_qpm_renew_req_t *', pointer)
	values = _ctx_values(req.ctx)
	values['ttl_ns'] = req.ttl_ns
	return values, _text(req.extra)


def _read_close(pointer):
	req = ffi.cast('defw2_qpm_close_req_t *', pointer)
	values = _ctx_values(req.ctx)
	values['reason_code'] = req.reason_code
	return values, None


def _read_run(pointer):
	req = ffi.cast('defw2_qpm_run_req_t *', pointer)
	values = _ctx_values(req.ctx)
	# Copied out, because the request goes when the call is answered and
	# a service may keep the circuit longer than that.
	circuit = (bytes(ffi.buffer(req.circuit.data, req.circuit.len))
		   if req.circuit.len else b'')
	values.update(
		circuit_format=_text(req.circuit.format), circuit=circuit,
		num_qubits=req.num_qubits, num_shots=req.num_shots,
		compiler=_text(req.compiler),
		return_statevector=bool(req.return_statevector),
		run_timeout_ms=req.timeout_ms if req.has_timeout else None,
		cancel_on_timeout=bool(req.cancel_on_timeout))
	return values, _text(req.extra)


def _read_task(pointer):
	req = ffi.cast('defw2_qpm_task_req_t *', pointer)
	values = _ctx_values(req.ctx)
	values.update(cid=_text(req.cid), qtask_id=req.qtask_id,
		      reason=_text(req.reason))
	return values, None


def _read_notify(pointer):
	req = ffi.cast('defw2_qpm_notify_req_t *', pointer)
	values = _ctx_values(req.ctx)
	values.update(target=EventTarget(_text(req.target.address),
					 req.target.provider_id,
					 _text(req.target.tag)),
		      type=_text(req.type))
	return values, _text(req.extra)


class _Writer:
	"""Writes a Python answer into the call's C answer.

	Every string comes from the call, which the provider frees once the
	reply is on the wire, so nothing here outlives the call.
	"""

	def __init__(self, call):
		self.call = call

	def str(self, value):
		if value is None:
			return ffi.NULL
		if not isinstance(value, bytes):
			value = str(value).encode('utf-8')
		copy = lib.defw2_call_strndup(self.call, value, len(value))
		if copy == ffi.NULL:
			raise MemoryError('no memory for the answer')
		return copy

	def json(self, value):
		return self.str(_json_text(value))

	def carry(self, source, nbytes):
		"""Hand the call a result, which the provider pushes into the
		caller's buffer."""
		reply = lib.defw2_call_bulk_reply(self.call, nbytes)
		if reply == ffi.NULL:
			raise MemoryError('no room for a {} byte result'.format(
				nbytes))
		ffi.memmove(reply, source, nbytes)


class _EventWriter(_Call):
	"""Writes a task into a completion event, as _Writer writes one into
	an answer. Its strings are Python's, kept until the publish has
	copied them, and a statevector is described and never carried, so
	its data stays behind."""

	def str(self, value):
		if value is not None and not isinstance(value, (bytes, str)):
			value = str(value)
		return super().str(value)

	def carry(self, source, nbytes):
		pass


def _answer_values(answer):
	if answer is None:
		return {}
	if isinstance(answer, dict):
		return answer
	if hasattr(answer, 'as_dict'):
		return answer.as_dict()
	raise TypeError('a QPM answer is a dict, not {}'.format(
		type(answer).__name__))


def _write_status(writer, pointer, answer):
	out = ffi.cast('defw2_qpm_service_status_t *', pointer)
	out.state = writer.str(answer.get('state'))
	out.ready = bool(answer.get('ready'))
	out.initialized = bool(answer.get('initialized'))
	out.accepting_requests = bool(answer.get('accepting_requests'))
	out.provider_ready = bool(answer.get('provider_ready'))
	out.active_task_count = answer.get('active_task_count') or 0
	out.active_reservation_count = (
		answer.get('active_reservation_count') or 0)
	out.extra = writer.json(answer.get('extra'))


def _write_decision(writer, pointer, answer):
	out = ffi.cast('defw2_qpm_decision_t *', pointer)
	out.decision = writer.str(answer.get('decision'))
	out.reservation_id = answer.get('reservation_id') or 0
	out.request_id = answer.get('request_id') or 0
	out.reason = writer.str(answer.get('reason'))
	out.reason_code = answer.get('reason_code') or 0
	out.retry_after_ns = answer.get('retry_after_ns') or 0
	out.message = writer.str(answer.get('message'))
	out.extra = writer.json(answer.get('extra'))


def _write_reservation(writer, pointer, answer):
	out = ffi.cast('defw2_qpm_reservation_t *', pointer)
	out.reservation_id = answer.get('reservation_id') or 0
	out.state = writer.str(answer.get('state'))
	out.created_at_ns = answer.get('created_at_ns') or 0
	out.expires_at_ns = answer.get('expires_at_ns') or 0
	out.extra = writer.json(answer.get('extra'))


def _dtype_of(data, given):
	if given is not None:
		return DTYPE.get(given, given)
	if _np is not None and isinstance(data, _np.ndarray):
		for code, name in _NUMPY_TYPE.items():
			if data.dtype == _np.dtype(name):
				return code
		raise TypeError('no wire type for {}'.format(data.dtype))
	return lib.DEFW2_DTYPE_C128


def _write_statevector(writer, out, answer):
	"""A statevector answers with data, or with a description alone.

	With data, from bytes or a numpy array, the call holds a copy that
	the provider pushes into the caller's buffer. With only a shape, the
	answer says what the result is without sending it, which is how a
	service tells a caller its buffer is too small. A Tensor, such as a
	Task's, is a description too.
	"""
	data = answer.get('statevector')
	shape = answer.get('statevector_shape')
	given = answer.get('statevector_dtype')
	if isinstance(data, Tensor):
		data, shape, given = None, data.shape, data.dtype
	dtype = _dtype_of(data, given)
	size = lib.defw2_dtype_size(dtype)
	if data is None and shape is None:
		return
	if data is not None:
		if _np is not None and isinstance(data, _np.ndarray):
			data = _np.ascontiguousarray(data)
			if shape is None:
				shape = data.shape
		# from_buffer takes any buffer, a complex array included,
		# which memoryview.cast will not.
		source = ffi.from_buffer(data)
		nbytes = len(source)
		if shape is None:
			shape = (nbytes // size if size else 0,)
		writer.carry(source, nbytes)
	else:
		nbytes = size
		for extent in shape:
			nbytes *= extent
	if len(shape) > lib.DEFW2_TENSOR_RANK_MAX:
		raise ValueError('a statevector has at most {} dimensions'.format(
			lib.DEFW2_TENSOR_RANK_MAX))
	out.statevector.dtype = dtype
	out.statevector.rank = len(shape)
	for index, extent in enumerate(shape):
		out.statevector.shape[index] = extent
	out.statevector.nbytes = nbytes


def _write_task(writer, pointer, answer):
	out = ffi.cast('defw2_qpm_task_t *', pointer)
	out.outcome = writer.str(answer.get('outcome'))
	out.lifecycle_state = writer.str(answer.get('lifecycle_state'))
	out.cid = writer.str(answer.get('cid'))
	out.qtask_id = answer.get('qtask_id') or 0
	out.reservation_id = answer.get('reservation_id') or 0
	out.reason = writer.str(answer.get('reason'))
	out.message = writer.str(answer.get('message'))
	out.completion_ready = bool(answer.get('completion_ready'))
	out.extra = writer.json(answer.get('extra'))
	_write_statevector(writer, out, answer)


# Every typed method: how to read its request and write its answer.
_METHODS = {
	API_QPM_CONTROL: {
		'is_ready': (_read_ctx, _write_status),
		'get_service_status': (_read_ctx, _write_status),
	},
	API_QPM_ADMISSION: {
		'reserve': (_read_reserve, _write_decision),
		'renew': (_read_renew, _write_decision),
		'release': (_read_close, _write_decision),
		'cancel': (_read_close, _write_decision),
		'get_reservation': (_read_ctx, _write_reservation),
	},
	API_QPM_EXECUTION: {
		'async_run': (_read_run, _write_task),
		'sync_run': (_read_run, _write_task),
		'read_cq': (_read_task, _write_task),
		'peek_cq': (_read_task, _write_task),
		'task_status': (_read_task, _write_task),
		'cancel_task': (_read_task, _write_task),
		'delete_circuit': (_read_task, _write_task),
		'register_event_notification': (_read_notify, _write_decision),
	},
}


def typed_api(api):
	"""Whether calls to this API carry structures rather than bytes."""
	return api in _METHODS


def read_request(api, method, call):
	"""The call's request as a Request, or None for a method not typed."""
	codec = _METHODS.get(api, {}).get(method)
	if codec is None:
		return None
	length = ffi.new('size_t *')
	pointer = lib.defw2_call_request(call, length)
	values, extra_json = codec[0](pointer)
	values['result_capacity'] = lib.defw2_call_result_capacity(call)
	values['traceparent'] = _text(lib.defw2_call_traceparent(call))
	return Request(method, values, extra_json)


def write_answer(api, method, call, answer):
	"""Write what the service returned into the call's answer."""
	codec = _METHODS[api][method]
	codec[1](_Writer(call), lib.defw2_call_response(call),
		 _answer_values(answer))


# --- the completion event ----------------------------------------------


def _read_completion(event):
	"""A completion event's payload, as a Task."""
	task = lib.defw2_qpm_event_task(event)
	return None if task == ffi.NULL else _task_of(task)


def _send_completion(publisher, target, type, task, traceparent):
	"""Publish a completion from the dict a service would answer read_cq
	with, or from a Task."""
	writer = _EventWriter()
	out = ffi.new('defw2_qpm_task_t *')
	_write_task(writer, out, _answer_values(task))
	return lib.defw2_qpm_publish_completion(publisher, target, type, out,
						traceparent)


QPM_COMPLETION = EventKind(API_QPM_EXECUTION, 'completion',
			   lib.defw2_qpm_event_accept, _read_completion,
			   _send_completion)
