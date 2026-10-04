"""Remote objects on v2, which defw.connect_to_binding hands a v1 caller.

v1's BaseRemote sent any method call to the remote object by name. On v2 a
remote QPM has typed methods, so compat's BaseRemote sends those over the
typed QPM APIs, taking the arguments by the names the API class declares,
and builds the v1 answer back from the typed one. Everything else fails,
naming the method, until v2 types it.

Fourteen QPM methods go over the typed APIs. The fifteenth,
register_event_notification, is emulated until compat's clients serve
sinks of their own. v1 pushed each completion to the caller as an event.
So once a caller registers, compat watches every task this process submits
through the same QPM, peeks the completion queue until each task completes,
and puts the completion on the caller's event queue, as v1's push would
have. Peeking leaves the completion queued, as v1's push did, so a read_cq
of it still finds it. What arrives is what peek_cq answers: the record v1
pushed, saying poll_operation peek_cq.

A failure comes back as the v1 exception the service raised, by name,
when it is a DEFw exception or a Python built-in one, and as the DEFw
exception its category suggests when not.

Each call carries the caller's trace context, from v1's defw_trace as v1
carried it, so the v2 spans and the service's own join the caller's trace.
"""

import builtins
import copy
import inspect
import logging
import os
import re
import sys
import threading

from .._qpm import QPM
from .._runtime import DefwError
from . import _mapping as m
from . import _state

try:
	import numpy as _np
except ImportError:
	_np = None

log = logging.getLogger('defw2.compat')

# The typed QPM methods, and the answer each has.
TYPED = {
	'is_ready': 'status',
	'get_service_status': 'status',
	'reserve': 'decision',
	'renew': 'decision',
	'release': 'decision',
	'cancel': 'decision',
	'get_reservation': 'reservation',
	'async_run': 'task',
	'sync_run': 'task',
	'read_cq': 'task',
	'peek_cq': 'task',
	'task_status': 'task',
	'cancel_task': 'task',
	'delete_circuit': 'task',
}

# How often the completion collector peeks, by default.
POLL_MS = int(os.environ.get('DEFW2_COMPAT_POLL_MS', '10'))

# Outcomes QFw's QPM answers a peek with when a completion will never be
# there, so the collector stops waiting for it.
_NEVER = ('INVALID_RESERVATION', 'MISSING_RESERVATION', 'NO_LONGER_RETAINED')

# The DEFw exception for a category, when the message names no class.
_BY_CATEGORY = {
	'pending-capacity': 'DEFwOutOfResources',
	'invalid-reservation': 'DEFwReserveError',
	'expired-reservation': 'DEFwReserveError',
	'not-found': 'DEFwNotFound',
	'transport': 'DEFwCommError',
	'timeout': 'DEFwCommError',
}
_NAMED = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)(?:: (.*))?\Z', re.S)


def v1_exception(error):
	"""The v1 exception for a failed typed call."""
	import defw_exception
	message = error.message or ''
	match = _NAMED.match(message)
	if match:
		name, text = match.group(1), match.group(2) or ''
		if name.startswith('DEFw'):
			cls = getattr(defw_exception, name, None)
		else:
			cls = getattr(builtins, name, None)
		if isinstance(cls, type) and issubclass(cls, Exception):
			try:
				return cls(text)
			except Exception:  # noqa: BLE001
				pass
	name = _BY_CATEGORY.get(error.category, 'DEFwRemoteError')
	return getattr(defw_exception, name)(message)


def unsupported(owner, name):
	import defw_exception
	text = ('defw2.compat: {}.{} is not a typed v2 method, so it cannot be '
		'called on v2 yet'.format(owner, name))
	log.warning(text)
	return defw_exception.DEFwError(text)


def _mapping_error(error):
	import defw_exception
	return defw_exception.DEFwError('defw2.compat: ' + str(error))


# --- connecting -----------------------------------------------------------


class Target:
	"""One remote QPM, shared by every API object bound to it."""

	def __init__(self, qpm, service_id, runtime_id):
		self.qpm = qpm
		self.service_id = service_id
		self.runtime_id = runtime_id
		self._lock = threading.Lock()
		self._completions = None
		self._statevectors = {}

	def completions(self):
		with self._lock:
			if self._completions is None:
				self._completions = Completions(self)
				_state.on_close(self._completions.stop)
			return self._completions

	def submitted(self, cid, info, reservation_id, token):
		"""Note a task this process submitted: the room its statevector
		needs, and whether a completion listener wants it."""
		capacity = _statevector_bytes(info)
		with self._lock:
			if capacity:
				self._statevectors[cid] = capacity
			completions = self._completions
		if completions is not None:
			completions.watch(cid, reservation_id, token)

	def capacity(self, cid):
		with self._lock:
			return self._statevectors.get(cid, 0)

	def forget(self, cid):
		with self._lock:
			self._statevectors.pop(cid, None)

	def close(self):
		self.qpm.close()


_targets = {}
_targets_lock = threading.Lock()


def _target(record):
	"""The Target for a v1 service record, made on first use."""
	address = (record.get('endpoint') or {}).get('address')
	if not address:
		raise _mapping_error(ValueError(
			'the record for {} has no address'.format(
				record.get('service_id'))))
	key = (address, record.get('runtime_id'))
	with _targets_lock:
		target = _targets.get(key)
		if target is not None:
			return target
		rt = _state.runtime()
		directory = _state.directory()
		v2 = None
		if directory is not None:
			v2 = directory.seen.get((record.get('service_id'),
						 record.get('runtime_id')))
		# v1 waited as long as its RPC timeout preference said, 300
		# seconds unless set, and a QPM's sync_run can take that long.
		import defw_common_def
		timeout_ms = int(defw_common_def.get_rpc_timeout() * 1000)
		if v2 is not None and v2.get('address') == address:
			qpm = QPM.from_record(rt, v2, timeout_ms=timeout_ms)
		else:
			qpm = QPM(rt, address, timeout_ms=timeout_ms)
		target = Target(qpm, record.get('service_id'),
				record.get('runtime_id'))
		_targets[key] = target
		_state.on_close(target.close)
		return target


def connect_to_binding(resolved_binding):
	"""v1's connect_to_binding: the API object for a resolved binding."""
	import importlib
	record = resolved_binding['service_record']
	binding = resolved_binding['selected_binding']
	module = importlib.import_module(binding['client_module'])
	cls = getattr(module, binding['client_class'])
	return cls(target=_target(record),
		   remote_module=binding.get('service_module'),
		   remote_class=binding.get('service_class'))


# --- calling -------------------------------------------------------------


_signatures = {}


def _arguments(fn, args, kwargs):
	"""The call's arguments by name, defaults filled in, as the API
	class's method declares them."""
	func = getattr(fn, '__func__', fn)
	signature = _signatures.get(func)
	if signature is None:
		signature = inspect.signature(fn)
		_signatures[func] = signature
	bound = signature.bind(*args, **kwargs)
	bound.apply_defaults()
	return dict(bound.arguments)


def invoke(target, owner, fn, args, kwargs):
	"""A method call on a remote object, on v2."""
	name = fn.__name__
	if name in TYPED:
		try:
			return _typed(target, name, _arguments(fn, args, kwargs))
		except m.MappingError as error:
			raise _mapping_error(error) from None
		except DefwError as error:
			raise v1_exception(error) from error
	if name == 'register_event_notification':
		return target.completions().register(
			_arguments(fn, args, kwargs))
	raise unsupported(owner, name)


def _ctx(a):
	return {'reservation_id': m.context_reservation(a.get('reservation_id')),
		'token': m.context_token(a.get('token'))}


def _selectors(a):
	cid = a.get('cid')
	if cid is not None and not isinstance(cid, str):
		raise m.MappingError('a cid is a string, not {!r}'.format(cid))
	qtask_id = a.get('qtask_id')
	if qtask_id is not None and not m.fits('u64', qtask_id):
		raise m.MappingError('a qtask_id is an unsigned 64-bit integer '
				     'above 0, not {!r}'.format(qtask_id))
	values = _ctx(a)
	values.update(cid=cid, qtask_id=qtask_id or 0)
	return values


def _traceparent():
	"""The caller's trace context, when something registered v1's
	defw_trace hooks, as QFw's telemetry does."""
	tracing = sys.modules.get('defw_trace')
	if tracing is None:
		return None
	return tracing.inject().get('traceparent')


def _typed(target, name, a):
	qpm = target.qpm
	kind = TYPED[name]
	trace = {'traceparent': _traceparent()}
	if name in ('is_ready', 'get_service_status'):
		answer = getattr(qpm, name)(**_ctx(a), **trace)
	elif name == 'reserve':
		answer = qpm.reserve(**m.reserve_request(a.get('token'),
							 a.get('request')),
				     **trace)
	elif name == 'renew':
		answer = qpm.renew(**m.renew_request(
			a.get('token'), a.get('reservation_id'), a.get('request')),
			**trace)
	elif name in ('release', 'cancel'):
		ctx = _ctx(a)
		answer = getattr(qpm, name)(
			ctx['reservation_id'], reason_code=m.reason_code(
				a.get('reason')), token=ctx['token'], **trace)
	elif name == 'get_reservation':
		ctx = _ctx(a)
		answer = qpm.get_reservation(ctx['reservation_id'],
					     token=ctx['token'], **trace)
	elif name in ('async_run', 'sync_run'):
		call = m.run_request(a.get('info'), a.get('reservation_id'),
				     a.get('token'), a.get('timeout'),
				     a.get('cancel_on_timeout'))
		if name == 'sync_run':
			call['result'] = _buffer(_statevector_bytes(a.get('info')))
		answer = getattr(qpm, name)(**call, **trace)
	elif name in ('read_cq', 'peek_cq'):
		values = _selectors(a)
		answer = collect(target, name, values,
				 target.capacity(values['cid']), **trace)
	else:
		values = _selectors(a)
		if name == 'cancel_task':
			reason = a.get('reason')
			if reason is not None and not isinstance(reason, str):
				raise m.MappingError('a cancel reason is a string, '
						     'not {!r}'.format(reason))
			values['reason'] = reason
		answer = getattr(qpm, name)(**values, **trace)

	result = m.typed_to_answer(kind, answer)
	if kind == 'task':
		_fill_statevector(result, answer)
	if name == 'async_run' and isinstance(result, dict) and \
	   isinstance(result.get('cid'), str):
		target.submitted(result['cid'], a.get('info'),
				 a.get('reservation_id'), a.get('token'))
	if name == 'read_cq' and isinstance(result, dict) and \
	   result.get('completion_ready'):
		target.forget(result.get('cid'))
	return result


def _statevector_bytes(info):
	"""The room a run's statevector needs, from what it asked for."""
	if not isinstance(info, dict) or info.get('return_statevector') is not \
	   True:
		return 0
	num_qubits = info.get('num_qubits')
	if type(num_qubits) is not int or not 0 < num_qubits <= 34:
		return 0
	return m.C128_BYTES << num_qubits


def _buffer(nbytes):
	if not nbytes:
		return None
	if _np is not None:
		return _np.empty(nbytes // m.C128_BYTES, dtype=_np.complex128)
	return bytearray(nbytes)


def collect(target, name, values, capacity, traceparent=None):
	"""read_cq or peek_cq, again with room when a statevector did not
	fit what was lent. The service leaves such a completion queued."""
	qpm = target.qpm
	answer = getattr(qpm, name)(result=_buffer(capacity),
				    traceparent=traceparent, **values)
	for _ in range(3):
		if answer.statevector_delivered or not answer.completion_ready \
		   or not answer.statevector.nbytes:
			break
		retry = dict(values, cid=answer.cid or values['cid'])
		answer = getattr(qpm, name)(
			result=_buffer(answer.statevector.nbytes),
			traceparent=traceparent, **retry)
	return answer


def _fill_statevector(result, answer):
	stub = m.find_stub(result)
	if stub is None:
		return
	if not answer.statevector_delivered:
		raise _mapping_error(ValueError(
			'the statevector of {} is {} bytes and was not delivered'
			.format(answer.cid, answer.statevector.nbytes)))
	m.restore_statevector(stub, answer.statevector_data)


# --- completion events --------------------------------------------------


class Completions:
	"""v1's completion events from one remote QPM, collected by peeking.

	register() is register_event_notification. Each task this process
	submits through the QPM afterwards is watched until peek_cq says it
	completed, and its completion is put on every matching registration's
	event queue, the caller's own defw_event_baseapi.BaseEventAPI.
	"""

	def __init__(self, target):
		self._target = target
		self._lock = threading.Lock()
		self._registrations = []
		self._watched = {}
		self._wake = threading.Event()
		self._stop = threading.Event()
		self._thread = None

	def register(self, a):
		import defw_common_def
		class_id = a.get('class_id')
		try:
			queue = defw_common_def.get_class_from_db(class_id)
		except Exception:  # noqa: BLE001
			raise _mapping_error(ValueError(
				'no local event queue {!r}: register_external() '
				'it before registering for events'.format(class_id)))
		registration = {
			'evtype': a.get('evtype'),
			'class_id': class_id,
			'queue': queue,
			'reservation_id': a.get('reservation_id'),
			'filters': dict(a.get('filters') or {}),
		}
		with self._lock:
			self._registrations.append(registration)
			count = len(self._registrations)
			if self._thread is None:
				self._thread = threading.Thread(
					target=self._run, daemon=True,
					name='defw2-compat-completions')
				self._thread.start()
		log.info('completion events for %s are collected by peek_cq '
			 'every %d ms', self._target.service_id, POLL_MS)
		return {'status': 'accepted', 'class_id': class_id,
			'registration_count': count}

	def watch(self, cid, reservation_id, token):
		with self._lock:
			if not self._registrations:
				return
			self._watched[cid] = (reservation_id, token)
		self._wake.set()

	def stop(self):
		self._stop.set()
		self._wake.set()
		if self._thread is not None and \
		   self._thread is not threading.current_thread():
			self._thread.join(10)

	def _run(self):
		while not self._stop.is_set():
			with self._lock:
				watched = list(self._watched.items())
			if not watched:
				self._wake.wait(0.5)
				self._wake.clear()
				continue
			for cid, (reservation_id, token) in watched:
				if self._stop.is_set():
					return
				self._peek(cid, reservation_id, token)
			self._stop.wait(POLL_MS / 1000.0)

	def _peek(self, cid, reservation_id, token):
		try:
			answer = collect(self._target, 'peek_cq', {
				'cid': cid, 'qtask_id': 0,
				'reservation_id': m.context_reservation(
					reservation_id),
				'token': m.context_token(token)},
				self._target.capacity(cid))
			if not answer.completion_ready:
				if answer.outcome in _NEVER:
					log.warning('no completion will come for %s: '
						    '%s', cid, answer.outcome)
					self._done(cid)
				return
			record = m.typed_to_answer('task', answer)
			_fill_statevector(record, answer)
		except Exception:  # noqa: BLE001
			if not self._stop.is_set():
				log.exception('collecting the completion of %s',
					      cid)
			return
		self._deliver(record)
		self._done(cid)

	def _done(self, cid):
		with self._lock:
			self._watched.pop(cid, None)
		self._target.forget(cid)

	def _deliver(self, record):
		import api_events
		with self._lock:
			registrations = list(self._registrations)
		matching = [r for r in registrations if self._matches(r, record)]
		for index, registration in enumerate(matching):
			payload = record if index == 0 else copy.deepcopy(record)
			try:
				registration['queue'].put(api_events.Event(
					registration['evtype'], payload))
			except Exception:  # noqa: BLE001
				log.exception('delivering a completion to %s',
					      registration['class_id'])

	@staticmethod
	def _matches(registration, record):
		reservation_id = registration['reservation_id']
		if reservation_id is not None and \
		   record.get('reservation_id') != reservation_id:
			return False
		return all(record.get(key) == value
			   for key, value in registration['filters'].items())
