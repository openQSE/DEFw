"""A v1 QPM, served as the typed QPM APIs.

QFw's QPM service classes take v1 calls, async_run(info, reservation_id=...,
token=...) and the rest, and answer with dictionaries. QPMAdapter is a
handler for defw2.ServiceHost that turns each typed call back into that v1
call and the dictionary it returns into the typed answer, through _mapping,
so the service class runs unchanged:

	host = defw2.ServiceHost(rt, service_id, 'qfw.qpm', apis=defw2.QPM_APIS)
	host.start(QPMAdapter(qpm))

What a v1 method raises fails the call with a status category, and the
message names the v1 exception class, so a v1 caller can be given the same
exception back. A statevector in an answer goes back as the bulk result
when the caller lent room for it, and only its description when not.
read_cq peeks first, so a completion whose statevector does not fit stays
queued for the retry, as the typed API promises; v1's read_cq would have
consumed it.
"""

import inspect
import sys
import threading

from .._runtime import ServiceError
from . import _mapping as m

__all__ = ['QPMAdapter', 'service_error']

# The category a v1 exception class fails a call with, by class name, for
# a v2 caller to branch on. Anything else is the service's own failure.
_CATEGORY = {
	'DEFwOutOfResources': 'pending-capacity',
	'DEFwReserveError': 'invalid-reservation',
	'DEFwNotFound': 'not-found',
	'DEFwAgentNotFound': 'not-found',
}

# Parameters every QPM call carries, which a v1 method that has no use for
# them may leave out of its signature.
_CONTEXT = ('reservation_id', 'token')


def _message(exc):
	"""What a v1 exception says: a DEFwError's msg, or its str."""
	if any(cls.__name__ == 'DEFwError' for cls in type(exc).__mro__):
		msg = getattr(exc, 'msg', '')
		return str(msg) if msg not in (None, '') else ''
	return str(exc)


def service_error(exc):
	"""The ServiceError a v1 exception fails a typed call with.

	The message is "Class: message", which the calling side reads back
	into the same class.
	"""
	if isinstance(exc, ServiceError):
		return exc
	if isinstance(exc, m.MappingError):
		return ServiceError('invalid-argument', str(exc))
	category = 'provider-failure'
	for cls in type(exc).__mro__:
		if cls.__name__ in _CATEGORY:
			category = _CATEGORY[cls.__name__]
			break
	text = _message(exc)
	name = type(exc).__name__
	return ServiceError(category, '{}: {}'.format(name, text) if text
			    else name)


class QPMAdapter:
	"""A defw2.ServiceHost handler over one v1 QPM object.

	Give it the object, or a factory that makes it, which is how v1
	served a singleton: made by the first call that needs it, and made
	again by the next call when making it failed.
	"""

	def __init__(self, qpm=None, factory=None):
		if (qpm is None) == (factory is None):
			raise ValueError('give it a QPM object or a factory, one of '
					 'the two')
		self._qpm = qpm
		self._factory = factory
		self._lock = threading.Lock()
		self._signatures = {}

	@property
	def qpm(self):
		"""The v1 object, once it has been made."""
		return self._qpm

	def _object(self):
		if self._qpm is None:
			with self._lock:
				if self._qpm is None:
					self._qpm = self._factory()
		return self._qpm

	def _serves(self, method):
		try:
			return getattr(self._object(), method, None) is not None
		except Exception as exc:  # noqa: BLE001
			raise service_error(exc)

	def _signature(self, method, fn):
		signature = self._signatures.get(method)
		if signature is None:
			try:
				signature = inspect.signature(fn)
			except (TypeError, ValueError):
				signature = False
			self._signatures[method] = signature
		return signature

	def _invoke(self, method, *args, **kwargs):
		"""Call a v1 method with the keywords it takes.

		A reservation or token it has no parameter for is left out,
		since v1 never sent one, and so is a None or a False, which
		is what a v1 caller's default would have been. Any other
		value it cannot take fails the call, rather than being
		dropped. The call runs in the caller's trace, through v1's
		defw_trace, as v1 ran it, so a service that records spans puts
		them in the caller's trace.
		"""
		tracing = sys.modules.get('defw_trace')
		traceparent = kwargs.pop('_traceparent', None)
		token = (tracing.attach({'traceparent': traceparent})
			 if tracing is not None and traceparent else None)
		try:
			fn = getattr(self._object(), method, None)
			if fn is None:
				raise ServiceError('not-found',
						   'this QPM does not serve that '
						   'method')
			signature = self._signature(method, fn)
			if signature:
				params = signature.parameters
				anything = any(
					p.kind is inspect.Parameter.VAR_KEYWORD
					for p in params.values())
				for name in list(kwargs):
					if anything or name in params:
						continue
					value = kwargs.pop(name)
					if name in _CONTEXT or value is None or \
					   value is False:
						continue
					raise ServiceError(
						'invalid-argument',
						"this QPM's {} takes no {}".format(
							method, name))
			return fn(*args, **kwargs)
		except Exception as exc:  # noqa: BLE001
			raise service_error(exc)
		finally:
			if token is not None:
				tracing.detach(token)

	@staticmethod
	def _context(request):
		return {'reservation_id': request.reservation_id or None,
			'token': request.token,
			'_traceparent': getattr(request, 'traceparent', None)}

	@staticmethod
	def _task(answer, capacity):
		"""A v1 task answer as a typed one, with its statevector as the
		bulk result when it fits capacity and as a description when
		not."""
		try:
			answer, data = m.take_statevector(answer)
			typed = m.answer_to_typed('task', answer)
		except m.MappingError as exc:
			raise service_error(exc)
		if data is None:
			return typed
		view = memoryview(data)
		if not view.c_contiguous:
			view = memoryview(view.tobytes())
		view = view.cast('B')
		if view.nbytes <= capacity:
			typed['statevector'] = view
			typed['statevector_dtype'] = 'c128'
		else:
			typed['statevector_shape'] = (view.nbytes // m.C128_BYTES,)
			typed['statevector_dtype'] = 'c128'
		return typed

	@staticmethod
	def _typed(kind, answer):
		try:
			return m.answer_to_typed(kind, answer)
		except m.MappingError as exc:
			raise service_error(exc)

	# --- control

	def is_ready(self, request):
		return self._typed('status', self._invoke(
			'is_ready', **self._context(request)))

	def get_service_status(self, request):
		return self._typed('status', self._invoke(
			'get_service_status', **self._context(request)))

	# --- admission

	def reserve(self, request):
		try:
			values = m.reserve_from_typed(request)
		except m.MappingError as exc:
			raise service_error(exc)
		return self._typed('decision', self._invoke(
			'reserve', token=request.token, request=values,
			_traceparent=getattr(request, 'traceparent', None)))

	def renew(self, request):
		try:
			values = m.renew_from_typed(request)
		except m.MappingError as exc:
			raise service_error(exc)
		return self._typed('decision', self._invoke(
			'renew', request=values, **self._context(request)))

	def release(self, request):
		return self._typed('decision', self._invoke(
			'release', reason=request.reason_code or None,
			**self._context(request)))

	def cancel(self, request):
		return self._typed('decision', self._invoke(
			'cancel', reason=request.reason_code or None,
			**self._context(request)))

	def get_reservation(self, request):
		return self._typed('reservation', self._invoke(
			'get_reservation', **self._context(request)))

	# --- execution

	def _run(self, method, request):
		try:
			info = m.info_from_run(request)
		except m.MappingError as exc:
			raise service_error(exc)
		return self._invoke(
			method, info=info,
			timeout=m.timeout_seconds(request.run_timeout_ms),
			cancel_on_timeout=request.cancel_on_timeout,
			**self._context(request))

	def async_run(self, request):
		return self._task(self._run('async_run', request), 0)

	def sync_run(self, request):
		return self._task(self._run('sync_run', request),
				  request.result_capacity)

	def _selectors(self, request, cid=None):
		values = self._context(request)
		values.update(cid=cid or request.cid,
			      qtask_id=request.qtask_id or None,
			      reason=request.reason)
		return values

	def read_cq(self, request):
		cid = None
		if self._serves('peek_cq'):
			peeked = self._invoke('peek_cq', **self._selectors(request))
			if isinstance(peeked, dict) and \
			   peeked.get('completion_ready'):
				_path, payload = m.find_statevector(peeked)
				need = m.payload_size(payload) or 0
				if need > request.result_capacity:
					# Leave it queued for a retry with room.
					return self._task(peeked, 0)
				cid = peeked.get('cid')
		return self._task(self._invoke(
			'read_cq', **self._selectors(request, cid)),
			request.result_capacity)

	def peek_cq(self, request):
		return self._task(self._invoke(
			'peek_cq', **self._selectors(request)),
			request.result_capacity)

	def task_status(self, request):
		return self._task(self._invoke(
			'task_status', **self._selectors(request)), 0)

	def cancel_task(self, request):
		return self._task(self._invoke(
			'cancel_task', **self._selectors(request)), 0)

	def delete_circuit(self, request):
		return self._task(self._invoke(
			'delete_circuit', **self._selectors(request)), 0)
