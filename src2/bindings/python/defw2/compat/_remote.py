"""Remote objects on v2, which defw.connect_to_binding hands a v1 caller.

v1's BaseRemote sent any method call to the remote object by name. On v2 a
remote QPM has typed methods, so compat's BaseRemote sends those over the
typed QPM APIs, taking the arguments by the names the API class declares,
and builds the v1 answer back from the typed one. Every other method goes
as a document to the API of the binding the object was connected through,
with the arguments the caller passed, by name, and its answer is the v1
answer, as JSON carried it.

Fifteen QPM methods go over the typed APIs. The fifteenth,
register_event_notification, registers this process's sink with the QPM,
and _events puts what arrives there on the caller's own event queue, as
v1's push did.

The other way round, a v1 QPM keeps a client's registration as the
api_events.BaseEventAPI it makes with the endpoint it was given, and puts
each completion to it. The endpoint compat gives it is the client's sink,
and a put to it publishes the completion and returns at once, so a v1 QPM
sends its events without waiting on any client, and without changing.

A failure comes back as the v1 exception the service raised, by name,
when it is a DEFw exception or a Python built-in one, and as the DEFw
exception its category suggests when not.

Each call carries the caller's trace context, from v1's defw_trace as v1
carried it, so the v2 spans and the service's own join the caller's trace.
"""

import builtins
import inspect
import logging
import os
import re
import sys
import threading

from .._event import EventTarget
from .._qpm import QPM, QPM_BINDING_NAMES, QPM_COMPLETION
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

# The v2 API each of QFw's QPM bindings is, for a record that does not say.
_API_OF_BINDING = {name: api for api, name in QPM_BINDING_NAMES.items()}

# Outcomes QFw's QPM answers a peek with when a completion will never be
# there, so the sweep stops waiting for it.
NEVER = ('INVALID_RESERVATION', 'MISSING_RESERVATION', 'NO_LONGER_RETAINED')

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


def unsupported(owner, name, why='belongs to no API this QPM serves'):
	import defw_exception
	text = 'defw2.compat: {}.{} {}, so it cannot be called on v2'.format(
		owner, name, why)
	log.warning(text)
	return defw_exception.DEFwError(text)


def _mapping_error(error):
	import defw_exception
	return defw_exception.DEFwError('defw2.compat: ' + str(error))


# --- connecting -----------------------------------------------------------


class Target:
	"""One remote QPM, shared by every API object bound to it.

	apis maps each binding name to the v2 API it is, and classes maps a
	v1 API class's name to its binding, for an object that was made
	without saying which binding it is for.
	"""

	def __init__(self, qpm, service_id, runtime_id, apis=None,
		     classes=None):
		self.qpm = qpm
		self.service_id = service_id
		self.runtime_id = runtime_id
		self.apis = dict(apis or _API_OF_BINDING)
		self.classes = dict(classes or {})
		self._lock = threading.Lock()
		self._statevectors = {}

	def api(self, binding_name, owner):
		"""The v2 API a call on an object of class owner goes to."""
		return self.apis.get(binding_name or self.classes.get(owner))

	def submitted(self, cid, info, reservation_id, token):
		"""Note a task this process submitted: the room its statevector
		needs, and, when a registration wants its completion, the task
		itself, for the sweep that recovers a lost event."""
		capacity = _statevector_bytes(info)
		with self._lock:
			if capacity:
				self._statevectors[cid] = capacity
		events = _state.started_events()
		if events is not None:
			events.watch(self, cid, reservation_id, token)

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
		apis = None
		if v2 is not None and v2.get('address') == address:
			qpm = QPM.from_record(rt, v2, timeout_ms=timeout_ms)
			apis = {b['binding_name']: b['api_id']
				for b in v2.get('bindings', [])}
		else:
			qpm = QPM(rt, address, timeout_ms=timeout_ms)
		classes = {b.get('client_class'): b.get('binding_name')
			   for b in record.get('api_bindings', [])}
		target = Target(qpm, record.get('service_id'),
				record.get('runtime_id'), apis, classes)
		_targets[key] = target
		_state.on_close(target.close)
		return target


# A v1 API module's name. It is one identifier, because finding a dotted
# name imports its package first, and that runs the package's code.
_MODULE_NAME = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')

# The directories v1 API modules come from, as v1's launcher put them on
# the path. QFw's are there.
_API_PATH = 'DEFW_EXTERNAL_SERVICE_APIS_PATH'


def _api_dirs():
	return [os.path.realpath(p)
		for p in os.environ.get(_API_PATH, '').split(':') if p]


def _refused(what):
	error = _mapping_error(ValueError(what))
	log.warning('%s', error)
	return error


def _client_class(binding):
	"""The v1 API class a binding names, from a v1 API module only.

	The binding comes from a directory record, and whatever can register
	in the directory writes those. v1 imported the module a record named
	and called the class it named there, so a record chose code for the
	client to run. Here the module has to be in one of the directories v1
	API modules come from, and the class has to be a BaseRemote, or
	nothing is imported and nothing is called.
	"""
	import importlib
	import importlib.util
	import defw_remote

	name = binding.get('client_module')
	if not isinstance(name, str) or not _MODULE_NAME.match(name):
		raise _refused('client module {!r} is not a plain module '
			       'name'.format(name))
	try:
		spec = importlib.util.find_spec(name)
	except ValueError:
		spec = None
	where = None
	if spec is not None and spec.has_location and spec.origin:
		where = os.path.dirname(spec.origin)
		if spec.submodule_search_locations is not None:
			where = os.path.dirname(where)
	if where is None or os.path.realpath(where) not in _api_dirs():
		raise _refused('client module {} is not a v1 API module from '
			       '{}'.format(name, _API_PATH))
	module = importlib.import_module(name)
	class_name = binding.get('client_class')
	cls = None
	if isinstance(class_name, str):
		cls = getattr(module, class_name, None)
	if not inspect.isclass(cls) or \
	   not issubclass(cls, defw_remote.BaseRemote):
		raise _refused('{} has no v1 API class {!r}'.format(
			name, class_name))
	return cls


def connect_to_binding(resolved_binding):
	"""v1's connect_to_binding: the API object for a resolved binding,
	whose class must be a v1 API class, as _client_class says."""
	record = resolved_binding['service_record']
	binding = resolved_binding['selected_binding']
	cls = _client_class(binding)
	return cls(target=_target(record),
		   remote_module=binding.get('service_module'),
		   remote_class=binding.get('service_class'),
		   binding_name=binding.get('binding_name'))


# --- calling -------------------------------------------------------------


_signatures = {}


def _signature(fn):
	func = getattr(fn, '__func__', fn)
	signature = _signatures.get(func)
	if signature is None:
		signature = inspect.signature(fn)
		_signatures[func] = signature
	return signature


def _arguments(fn, args, kwargs):
	"""The call's arguments by name, defaults filled in, as the API
	class's method declares them."""
	bound = _signature(fn).bind(*args, **kwargs)
	bound.apply_defaults()
	return dict(bound.arguments)


def _named(fn, args, kwargs):
	"""The arguments a caller passed, by the names the API class's method
	gives them, and no defaults, so the service applies its own, as it
	did on v1."""
	signature = _signature(fn)
	named = {}
	for name, value in signature.bind(*args, **kwargs).arguments.items():
		kind = signature.parameters[name].kind
		if kind is inspect.Parameter.VAR_KEYWORD:
			named.update(value)
		elif kind is inspect.Parameter.VAR_POSITIONAL:
			if value:
				raise m.MappingError(
					'a document names its arguments, so it '
					'cannot carry *{}'.format(name))
		else:
			named[name] = value
	return named


def invoke(target, owner, fn, args, kwargs, binding_name=None):
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
		try:
			return _state.events().register_completions(
				target, _arguments(fn, args, kwargs))
		except m.MappingError as error:
			raise _mapping_error(error) from None
		except DefwError as error:
			raise v1_exception(error) from error
	api = target.api(binding_name, owner)
	if api is None:
		raise unsupported(owner, name)
	try:
		request = _named(fn, args, kwargs)
	except m.MappingError as error:
		raise _mapping_error(error) from None
	try:
		return target.qpm.document(api, name, request,
					   traceparent=traceparent())
	except (TypeError, ValueError) as error:
		text = 'a document carries JSON, and {}'.format(error)
		raise _mapping_error(m.MappingError(text)) from None
	except DefwError as error:
		raise v1_exception(error) from error


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


def traceparent():
	"""The caller's trace context, when something registered v1's
	defw_trace hooks, as QFw's telemetry does."""
	tracing = sys.modules.get('defw_trace')
	if tracing is None:
		return None
	return tracing.inject().get('traceparent')


def _typed(target, name, a):
	qpm = target.qpm
	kind = TYPED[name]
	trace = {'traceparent': traceparent()}
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
		fill_statevector(result, answer)
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


def fill_statevector(result, answer):
	stub = m.find_stub(result)
	if stub is None:
		return
	if not answer.statevector_delivered:
		raise _mapping_error(ValueError(
			'the statevector of {} is {} bytes and was not delivered'
			.format(answer.cid, answer.statevector.nbytes)))
	m.restore_statevector(stub, answer.statevector_data)


# --- events to a client ---------------------------------------------------


class EventEndpoint:
	"""A client's sink, as the endpoint a v1 QPM keeps for its
	registration. The QPM makes an api_events.BaseEventAPI of it, and a
	put to that publishes the event to the sink."""

	def __init__(self, address, provider_id):
		self.address = address
		self.provider_id = provider_id

	def get_id(self):
		return self.address

	def get(self):
		return {'addr': self.address, 'provider_id': self.provider_id}

	def __repr__(self):
		return 'EventEndpoint({}, {})'.format(self.address,
						      self.provider_id)


def event_call(endpoint, class_id, name, args, kwargs):
	"""A call on a v1 event API object whose target is a client's sink.
	put is the only method v1 called on one."""
	if name != 'put':
		raise unsupported('BaseEventAPI', name,
				  'is not put, the one method a sink takes')
	event = args[0] if args else kwargs.get('event')
	return publish_event(endpoint, class_id, event)


def publish_event(endpoint, class_id, event):
	"""v1's put of a completion event to a client, as a v2 completion.

	It returns once the event is queued, and never waits on the client.
	The record goes as the typed task it is, its statevector described
	rather than carried, since the client fetches that from the
	completion queue. A full queue drops the event, which the client's
	sweep recovers. A client that is gone raises, so the QPM drops the
	registration, as v1 dropped one whose put failed.
	"""
	from . import _events
	record, nbytes = m.describe_statevector(event.get_event())
	try:
		task = m.answer_to_typed('task', record)
	except m.MappingError as error:
		raise _mapping_error(error) from None
	if nbytes:
		task['statevector_shape'] = (nbytes // m.C128_BYTES,)
		task['statevector_dtype'] = 'c128'
	try:
		queued = _state.publisher().publish(
			QPM_COMPLETION,
			EventTarget(endpoint.address, endpoint.provider_id,
				    class_id),
			task, type=_events.type_text(event.get_evtype()),
			traceparent=traceparent())
	except DefwError as error:
		raise v1_exception(error) from error
	if not queued:
		log.warning('dropped the completion event of %s to %s, whose '
			    'queue is full; its sweep will find it',
			    record.get('cid') if isinstance(record, dict)
			    else None, endpoint.address)
