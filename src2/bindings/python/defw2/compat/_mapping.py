"""How a v1 QPM's dictionaries cross the typed QPM APIs, in both directions.

A v1 QPM takes open dictionaries, info for a run and request for a
reservation, and answers with one. The typed APIs carry typed fields and
extra, a JSON object. This module moves what fits a typed field into it and
leaves everything else in extra, and it moves a value only when the trip
back gives the same value: a string that is not empty, an integer from 1 to
2**64 - 1, or True. An empty string, a zero, a False, a None or a float
stays in extra under its own key. So the dictionary a v1 caller gets back is
the one the service returned, key for key, and a v2 caller still finds what
it branches on in the typed fields.

Two things in a v1 QPM's dictionaries are not JSON values at heart. A
circuit moves into the request's circuit bytes, and a statevector into the
bulk result. Each leaves a stub where it was, a copy of its dictionary with
data set to None, so the far side knows where to put it back and in which
encoding.

The service side of this is the adapter, and the calling side the remote
objects in _remote. Both use only what is here, so the two directions
cannot drift apart.
"""

import base64
import binascii
import json
import zlib

from .._defw2 import lib

__all__ = []

U64_MAX = (1 << 64) - 1
STR_MAX = lib.DEFW2_STR_MAX
TEXT_MAX = lib.DEFW2_EAGER_MAX

# A v1 answer that is not a dictionary travels whole under this key, the
# only key of extra, since the typed fields only describe dictionaries.
VALUE_KEY = 'defw2.compat.value'

# QFw's circuit formats whose v1 data is base64 text of binary bytes, as
# util/circuit_payload.py writes them. The typed request carries the bytes
# themselves. Every other format's data is text.
BINARY_FORMATS = ('qpy', 'qpy+gzip')
LEGACY_FORMAT = 'openqasm2'

# Where a v1 QPM puts a statevector in an answer, as QFw's QRCs do: in the
# result of a completion or a task status, or at the top.
STATEVECTOR_PATHS = (('result', 'statevector'), ('statevector',))
STATEVECTOR_ENCODING = 'base64+zlib'
RAW_ENCODING = 'raw'
C128_BYTES = 16

# Each typed answer's fields: (typed name, v1 key, kind). A v1 reservation
# decision says accepted or rejected under status, and the typed field that
# carries it is decision.
ANSWER_FIELDS = {
	'status': (
		('state', 'state', 'str'),
		('ready', 'ready', 'bool'),
		('initialized', 'initialized', 'bool'),
		('accepting_requests', 'accepting_requests', 'bool'),
		('provider_ready', 'provider_ready', 'bool'),
		('active_task_count', 'active_task_count', 'u64'),
		('active_reservation_count', 'active_reservation_count', 'u64'),
	),
	'decision': (
		('decision', 'status', 'str'),
		('reservation_id', 'reservation_id', 'u64'),
		('request_id', 'request_id', 'u64'),
		('reason', 'reason', 'str'),
		('reason_code', 'reason_code', 'u64'),
		('retry_after_ns', 'retry_after_ns', 'u64'),
		('message', 'message', 'str'),
	),
	'reservation': (
		('reservation_id', 'reservation_id', 'u64'),
		('state', 'state', 'str'),
		('created_at_ns', 'created_at_ns', 'u64'),
		('expires_at_ns', 'expires_at_ns', 'u64'),
	),
	'task': (
		('outcome', 'outcome', 'str'),
		('lifecycle_state', 'lifecycle_state', 'str'),
		('cid', 'cid', 'str'),
		('qtask_id', 'qtask_id', 'u64'),
		('reservation_id', 'reservation_id', 'u64'),
		('reason', 'reason', 'str'),
		('message', 'message', 'str'),
		('completion_ready', 'completion_ready', 'bool'),
	),
}

# The typed reservation request's fields, under the same names in v1.
RESERVE_FIELDS = (
	('request_id', 'u64'),
	('user', 'str'),
	('job_id', 'str'),
	('allocation_id', 'str'),
	('target_device_id', 'str'),
	('scope_id', 'str'),
	('workload_kind', 'str'),
	('num_qubits', 'u64'),
	('walltime_ns', 'u64'),
	('ttl_ns', 'u64'),
)
TASK_CLASS_FIELDS = ('count', 'qubit_count', 'depth', 'one_q_gate_count',
		     'two_q_gate_count', 'shots', 'measurement_count')

# The typed run request's fields that come from info.
RUN_FIELDS = (
	('num_qubits', 'u64'),
	('num_shots', 'u64'),
	('compiler', 'str'),
	('return_statevector', 'bool'),
)


class MappingError(ValueError):
	"""A value the typed APIs cannot carry the way v1 did."""


def fits(kind, value):
	"""Whether value can travel in a typed field and come back the same."""
	if kind == 'str':
		return (type(value) is str and value != '' and
			'\0' not in value and
			len(value.encode('utf-8')) < STR_MAX)
	if kind == 'u64':
		return type(value) is int and 0 < value <= U64_MAX
	if kind == 'bool':
		return value is True
	raise ValueError('no field kind ' + kind)


def present(kind, value):
	"""Whether a typed field that arrived carries a value."""
	if kind == 'str':
		return value is not None
	if kind == 'u64':
		return bool(value)
	return value is True


def _plain(value):
	"""A numpy scalar or array as the Python value JSON can carry."""
	if getattr(value, 'shape', None) == () and callable(
			getattr(value, 'item', None)):
		return value.item()
	if callable(getattr(value, 'tolist', None)):
		return value.tolist()
	raise TypeError('{} is not a JSON value'.format(type(value).__name__))


def json_text(value, what):
	"""value as compact JSON, or MappingError naming what it was.

	JSON is what extra carries, so a tuple arrives as a list, a key that
	is not a string as a string, and a numpy value as a Python one.
	"""
	try:
		text = json.dumps(value, separators=(',', ':'),
				  default=_plain)
	except (TypeError, ValueError) as error:
		raise MappingError('{} is not JSON: {}'.format(what, error))
	if len(text.encode('utf-8')) >= TEXT_MAX:
		raise MappingError(
			'{} is {} bytes of JSON, more than the {} a typed call '
			'carries in extra'.format(what, len(text), TEXT_MAX - 1))
	return text


# --- answers: v1 dictionary to typed, on the service side --------------


def answer_to_typed(kind, answer):
	"""A v1 answer as the dict a typed method returns, minus statevector.

	The typed fields are moved out, everything else stays in extra.
	"""
	if not isinstance(answer, dict):
		return {'extra': json_text({VALUE_KEY: answer}, 'the answer')}
	rest = dict(answer)
	typed = {}
	for name, key, field in ANSWER_FIELDS[kind]:
		if key in rest and fits(field, rest[key]):
			typed[name] = rest.pop(key)
	typed['extra'] = json_text(rest, 'the answer') if rest else None
	return typed


# --- answers: typed back to the v1 dictionary, on the calling side ------


def typed_to_answer(kind, answer):
	"""The v1 dictionary a typed answer was made from.

	answer is a defw2 answer, such as a Task. A service that is not a v1
	QPM behind the adapter answers the same way, with its extra merged in
	when it is an object and kept under extra when it is not.
	"""
	extra = answer.extra
	if isinstance(extra, dict) and len(extra) == 1 and VALUE_KEY in extra:
		return extra[VALUE_KEY]
	if isinstance(extra, dict):
		result = dict(extra)
	else:
		result = {} if extra is None else {'extra': extra}
	for name, key, field in ANSWER_FIELDS[kind]:
		value = getattr(answer, name)
		if present(field, value):
			result[key] = value
	return result


# --- statevectors --------------------------------------------------------


def _holder(answer, path):
	"""The dictionary that holds path's last key, or None."""
	holder = answer
	for key in path[:-1]:
		holder = holder.get(key) if isinstance(holder, dict) else None
	return holder if isinstance(holder, dict) else None


def _is_payload(value):
	return isinstance(value, dict) and value.get('type') == 'statevector'


def payload_size(payload):
	"""The raw bytes a v1 statevector payload decodes to, or None when it
	is not one the bulk path carries."""
	if not _is_payload(payload) or payload.get('data') is None:
		return None
	if payload.get('dtype') != 'complex128' or \
	   payload.get('byte_order') not in (None, 'little'):
		return None
	encoding = payload.get('encoding')
	if encoding == STATEVECTOR_ENCODING:
		if not isinstance(payload.get('data'), str):
			return None
		size = payload.get('raw_size_bytes')
		if type(size) is int and size >= 0:
			return size
		count = payload.get('num_amplitudes')
		if type(count) is int and count >= 0:
			return count * C128_BYTES
		raw = _decode(payload)
		return None if raw is None else len(raw)
	if encoding == RAW_ENCODING:
		try:
			return memoryview(payload['data']).nbytes
		except TypeError:
			return None
	return None


def _decode(payload):
	"""A base64+zlib payload's bytes, or None when it does not decode, in
	which case the payload travels whole and the caller's own decode
	fails the way it would have in v1."""
	try:
		raw = zlib.decompress(base64.b64decode(
			payload['data'].encode('ascii'), validate=True))
	except (binascii.Error, ValueError, zlib.error, UnicodeError):
		return None
	if len(raw) % C128_BYTES:
		return None
	return raw


def find_statevector(answer):
	"""(path, payload) of the first statevector in a v1 answer that the
	bulk path can carry, or (None, None)."""
	if not isinstance(answer, dict):
		return None, None
	for path in STATEVECTOR_PATHS:
		holder = _holder(answer, path)
		if holder is None:
			continue
		payload = holder.get(path[-1])
		if payload_size(payload) is not None:
			return path, payload
	return None, None


def take_statevector(answer):
	"""Take a statevector's data out of a v1 answer.

	Returns (answer, data), where answer is a copy with the payload's data
	set to None, copied along the path so the service's own dictionaries
	are untouched, and data is the raw little-endian complex128 bytes.
	data is None when there is no statevector the bulk path carries.
	"""
	path, payload = find_statevector(answer)
	if path is None:
		return answer, None
	if payload['encoding'] == STATEVECTOR_ENCODING:
		data = _decode(payload)
		if data is None:
			return answer, None
	else:
		data = payload['data']

	answer = dict(answer)
	holder = answer
	for key in path[:-1]:
		holder[key] = dict(holder[key])
		holder = holder[key]
	stub = dict(payload)
	stub['data'] = None
	holder[path[-1]] = stub
	return answer, data


def find_stub(answer):
	"""The stub a statevector left in a v1 answer, or None."""
	if not isinstance(answer, dict):
		return None
	for path in STATEVECTOR_PATHS:
		holder = _holder(answer, path)
		if holder is None:
			continue
		stub = holder.get(path[-1])
		if _is_payload(stub) and stub.get('data', 0) is None:
			return stub
	return None


def restore_statevector(stub, data):
	"""Put a delivered statevector back into its stub, in its encoding.

	base64+zlib is encoded again from the bytes, with the sizes that
	encoding gives. On the same zlib that is the payload the service
	made, byte for byte. raw gets the delivered buffer itself.
	"""
	if stub.get('encoding') != STATEVECTOR_ENCODING:
		stub['data'] = data
		return stub
	raw = bytes(memoryview(data).cast('B'))
	compressed = zlib.compress(raw)
	encoded = base64.b64encode(compressed).decode('ascii')
	stub['data'] = encoded
	if 'compressed_size_bytes' in stub:
		stub['compressed_size_bytes'] = len(compressed)
	if 'base64_size_bytes' in stub:
		stub['base64_size_bytes'] = len(encoded)
	if 'compression_ratio' in stub and raw:
		stub['compression_ratio'] = len(compressed) / len(raw)
	return stub


# --- requests: v1 call to typed, on the calling side --------------------


def _move(rest, fields, out):
	for key, field in fields:
		if key in rest and fits(field, rest[key]):
			out[key] = rest.pop(key)


def _circuit_bytes(fmt, data):
	"""A circuit's data as the bytes the typed request carries, or None
	when it cannot be carried exactly, in which case it stays in extra."""
	if not isinstance(data, str):
		return None
	if fmt in BINARY_FORMATS:
		try:
			raw = base64.b64decode(data.encode('ascii'), validate=True)
		except (binascii.Error, ValueError, UnicodeError):
			return None
		# Only canonical base64 comes back as the same text.
		if base64.b64encode(raw).decode('ascii') != data:
			return None
		return raw
	return data.encode('utf-8')


def run_request(info, reservation_id=None, token=None, timeout=None,
		cancel_on_timeout=False):
	"""A v1 async_run or sync_run call as defw2.QPM keyword arguments."""
	call = {
		'circuit': b'', 'circuit_format': None,
		'reservation_id': context_reservation(reservation_id),
		'token': context_token(token),
		'run_timeout_ms': timeout_ms(timeout),
		'cancel_on_timeout': bool(cancel_on_timeout),
		'extra': None,
	}
	if info is None:
		return call
	if not isinstance(info, dict):
		raise MappingError('info is a dict, not {}'.format(
			type(info).__name__))
	rest = dict(info)
	circuit = rest.get('circuit')
	if isinstance(circuit, dict) and fits('str', circuit.get('format')):
		raw = _circuit_bytes(circuit['format'], circuit.get('data'))
		if raw is not None:
			stub = dict(circuit)
			stub['data'] = None
			rest['circuit'] = stub
			call['circuit'] = raw
			call['circuit_format'] = circuit['format']
	elif fits('str', rest.get('qasm')):
		call['circuit'] = rest.pop('qasm').encode('utf-8')
		call['circuit_format'] = LEGACY_FORMAT
	_move(rest, RUN_FIELDS, call)
	call['extra'] = json_text(rest, 'info')
	return call


def reserve_request(token=None, request=None):
	"""A v1 reserve call as defw2.QPM.reserve keyword arguments."""
	call = {'token': context_token(token)}
	if request is None:
		return call
	if not isinstance(request, dict):
		raise MappingError('a reservation request is a dict, not '
				   '{}'.format(type(request).__name__))
	rest = dict(request)
	_move(rest, RESERVE_FIELDS, call)
	task_class = rest.get('task_class')
	if isinstance(task_class, dict) and \
	   sorted(task_class) == sorted(TASK_CLASS_FIELDS) and \
	   all(type(v) is int and 0 <= v <= U64_MAX
	       for v in task_class.values()):
		call['task_class'] = rest.pop('task_class')
	call['extra'] = json_text(rest, 'the reservation request')
	return call


def renew_request(token=None, reservation_id=None, request=None):
	call = {'reservation_id': context_reservation(reservation_id),
		'token': context_token(token)}
	if request is None:
		return call
	if not isinstance(request, dict):
		raise MappingError('a renewal request is a dict, not '
				   '{}'.format(type(request).__name__))
	rest = dict(request)
	_move(rest, (('ttl_ns', 'u64'),), call)
	call['extra'] = json_text(rest, 'the renewal request')
	return call


def reason_code(reason):
	"""A v1 release or cancel reason, which QFw reads as reason or 0."""
	if reason is None or reason == 0:
		return 0
	if fits('u64', reason):
		return reason
	raise MappingError('a release reason is an integer code, not '
			   '{!r}'.format(reason))


def context_reservation(reservation_id):
	"""A reservation ID for the typed context: 0 when there is none."""
	if reservation_id is None:
		return 0
	if isinstance(reservation_id, str) and reservation_id.isdecimal():
		reservation_id = int(reservation_id, 10)
	if fits('u64', reservation_id):
		return reservation_id
	raise MappingError('reservation_id is an unsigned 64-bit integer '
			   'above 0, not {!r}'.format(reservation_id))


def context_token(token):
	if token is None or fits('str', token):
		return token
	raise MappingError('a token is a string, not {!r}'.format(
		type(token).__name__))


def timeout_ms(timeout):
	"""A v1 run timeout, in seconds, as whole milliseconds or None."""
	if timeout is None:
		return None
	if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) \
	   or timeout < 0:
		raise MappingError('a run timeout is a number of seconds, not '
				   '{!r}'.format(timeout))
	return int(round(timeout * 1000))


def timeout_seconds(milliseconds):
	"""The v1 timeout a typed one came from: whole seconds stay whole."""
	if milliseconds is None:
		return None
	if milliseconds % 1000 == 0:
		return milliseconds // 1000
	return milliseconds / 1000.0


# --- requests: typed back to the v1 call, on the service side ----------


def _extra_object(request, what):
	"""A request's extra as a dict, or None when it carried none."""
	if request.extra_json is None:
		return None
	extra = request.extra
	if not isinstance(extra, dict):
		raise MappingError(
			'{} needs extra to be a JSON object for a v1 QPM'.format(what))
	return dict(extra)


def info_from_run(request):
	"""The info dict of a v1 run call, from a typed run request."""
	info = _extra_object(request, 'a run')
	typed = (request.circuit or request.circuit_format or
		 any(present(field, getattr(request, key))
		     for key, field in RUN_FIELDS))
	if info is None:
		if not typed:
			return None
		info = {}

	if request.circuit or request.circuit_format:
		fmt = request.circuit_format or LEGACY_FORMAT
		if fmt in BINARY_FORMATS:
			data = base64.b64encode(request.circuit).decode('ascii')
		else:
			try:
				data = request.circuit.decode('utf-8')
			except UnicodeDecodeError:
				raise MappingError(
					'a {} circuit is text, and this one is not '
					'UTF-8'.format(fmt))
		stub = info.get('circuit')
		if _is_circuit_stub(stub):
			stub = dict(stub)
			stub['data'] = data
			info['circuit'] = stub
		elif fmt == LEGACY_FORMAT:
			info['qasm'] = data
		else:
			info['circuit'] = {'format': fmt, 'data': data}

	for key, field in RUN_FIELDS:
		value = getattr(request, key)
		if present(field, value):
			info[key] = value
	return info


def _is_circuit_stub(value):
	return isinstance(value, dict) and 'data' in value and \
		value['data'] is None


def reserve_from_typed(request):
	"""The request dict of a v1 reserve call, from a typed one."""
	values = _extra_object(request, 'a reservation')
	typed = any(present(field, getattr(request, key))
		    for key, field in RESERVE_FIELDS) or \
		request.task_class is not None
	if values is None:
		if not typed:
			return None
		values = {}
	for key, field in RESERVE_FIELDS:
		value = getattr(request, key)
		if present(field, value):
			values[key] = value
	if request.task_class is not None:
		values['task_class'] = dict(request.task_class)
	return values


def renew_from_typed(request):
	values = _extra_object(request, 'a renewal')
	if values is None:
		if not request.ttl_ns:
			return None
		values = {}
	if request.ttl_ns:
		values['ttl_ns'] = request.ttl_ns
	return values
