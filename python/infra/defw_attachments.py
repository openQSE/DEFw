"""
Transparent binary-attachment handling for DEFw RPCs.

Large buffer-protocol objects (NumPy arrays, and bytes/bytearray above a size
threshold) that appear inside RPC arguments or return values are pulled out of
the YAML-serialized message and carried as separate binary attachments, then
transparently restored on the receiving side. This keeps large payloads out of
the text YAML encoding so they can travel over an efficient binary path
(eager/inline today; an RMA fi_read rendezvous for RMA-capable transports in a
later slice). Application code is unchanged: it passes and returns NumPy arrays
(or bytes) normally.

The message body still serializes to YAML, but where a large buffer was found
it now holds a small marker dict recording the attachment index and enough
metadata (kind, and for arrays the shape and dtype) to reconstruct the object.
"""
import base64
import logging
import os

# Reserved key identifying an extracted-attachment marker in the YAML message.
# Chosen to be extremely unlikely to collide with an application dict key.
ATTACH_MARKER_KEY = '__defw_attachment__'

# Reserved top-level key carrying the attachment payloads, in marker-index
# order. Each entry either holds the data (a base64 string) or says where to
# read it (an RMA descriptor dict), so the two can be mixed freely within one
# message and each payload travels whichever way suits it.
ATTACH_DATA_KEY = '__defw_attachment_data__'

# Set DEFW_RMA_ATTACHMENTS=0 to keep every payload inline regardless of size.
# RMA is used by default where it is available.
RMA_ENV = 'DEFW_RMA_ATTACHMENTS'

# Payloads at or above this many bytes travel by RMA when the peer can serve
# it; smaller ones stay inline, where an extra round trip would cost more than
# the base64 encoding it saves. Overridable with DEFW_RMA_THRESHOLD, and
# setting it to 0 sends every attachment by RMA, which is how the RMA path is
# exercised with small payloads.
#
# The default is a judgement rather than a measurement: base64 inflates a
# payload by a third, and the fabric's eager receive buffer is 256 KiB, so
# inline payloads much above 192 KiB push the whole message off the eager path
# anyway. 64 KiB sits well below that and well above the small arrays that
# ride along with ordinary RPCs. Worth revisiting with numbers when the
# statevector path adopts this.
DEFAULT_RMA_THRESHOLD = 64 * 1024
RMA_THRESHOLD_ENV = 'DEFW_RMA_THRESHOLD'

# bytes/bytearray at or above this many bytes are extracted as attachments;
# smaller ones stay inline in YAML (which handles them fine). NumPy arrays are
# always extracted because YAML cannot represent them.
DEFAULT_ATTACH_THRESHOLD = 4096

try:
	import numpy as _np
except Exception:
	_np = None


def _is_ndarray(obj):
	return _np is not None and isinstance(obj, _np.ndarray)


def _marker(index, kind, **meta):
	m = {ATTACH_MARKER_KEY: index, 'kind': kind}
	m.update(meta)
	return m


def _extract(obj, attachments, threshold):
	# NumPy array: always an attachment (YAML cannot serialize it). Make it
	# contiguous first, and keep the resulting object alive by storing it in
	# the attachments list (the send path reads its buffer).
	if _is_ndarray(obj):
		arr = _np.ascontiguousarray(obj)
		index = len(attachments)
		attachments.append(arr)
		return _marker(index, 'ndarray',
			       shape=list(arr.shape), dtype=str(arr.dtype))

	# bytes/bytearray: attachment only above the threshold.
	if isinstance(obj, (bytes, bytearray)):
		if len(obj) >= threshold:
			index = len(attachments)
			attachments.append(bytes(obj))
			return _marker(index, 'bytes')
		return obj

	if isinstance(obj, dict):
		return {k: _extract(v, attachments, threshold)
			for k, v in obj.items()}
	if isinstance(obj, list):
		return [_extract(v, attachments, threshold) for v in obj]
	if isinstance(obj, tuple):
		return tuple(_extract(v, attachments, threshold) for v in obj)

	return obj


def extract_attachments(msg, threshold=DEFAULT_ATTACH_THRESHOLD):
	"""Walk msg and pull large buffer-protocol objects out into attachments.

	Returns (transformed_msg, attachments) where transformed_msg is a copy of
	msg with each extracted buffer replaced by a marker dict, and attachments
	is a list of buffer-protocol objects (bytes or contiguous NumPy arrays) in
	marker-index order. msg itself is not mutated. attachments is empty when
	nothing qualified, in which case transformed_msg is equivalent to msg.
	"""
	attachments = []
	transformed = _extract(msg, attachments, threshold)
	return transformed, attachments


def _reinject(obj, attachments):
	if isinstance(obj, dict):
		if ATTACH_MARKER_KEY in obj:
			index = obj[ATTACH_MARKER_KEY]
			buf = attachments[index]
			kind = obj.get('kind')
			if kind == 'ndarray':
				if _np is None:
					raise RuntimeError(
						"received a NumPy array attachment but "
						"numpy is not available to restore it")
				arr = _np.frombuffer(buf, dtype=obj['dtype'])
				return arr.reshape(obj['shape'])
			# kind == 'bytes' (or unknown): hand back raw bytes
			return bytes(buf)
		return {k: _reinject(v, attachments) for k, v in obj.items()}
	if isinstance(obj, list):
		return [_reinject(v, attachments) for v in obj]
	if isinstance(obj, tuple):
		return tuple(_reinject(v, attachments) for v in obj)
	return obj


def reinject_attachments(msg, attachments):
	"""Inverse of extract_attachments: replace markers in msg with the
	corresponding attachment buffers (reconstructing NumPy arrays from their
	recorded shape/dtype). Returns msg unchanged when there are no
	attachments."""
	if not attachments:
		return msg
	return _reinject(msg, attachments)


def _to_bytes(buf):
	if isinstance(buf, (bytes, bytearray)):
		return bytes(buf)
	# NumPy: tobytes() is unambiguous and works for every dtype (memoryview
	# cast to 'B' can reject non-standard formats such as complex).
	if _is_ndarray(buf):
		return buf.tobytes()
	# any other buffer-protocol object
	return memoryview(buf).cast('B').tobytes()


_agent_mod = None
_agent_mod_tried = False


def _rma_ops():
	"""The C entry points backing the RMA path, or None when they are not
	available -- a DEFw built without them, or this module imported on its
	own for testing. Callers fall back to the inline path."""
	global _agent_mod, _agent_mod_tried
	if not _agent_mod_tried:
		_agent_mod_tried = True
		try:
			import cdefw_agent
			_agent_mod = cdefw_agent
		except Exception:
			_agent_mod = None
	return _agent_mod


def rma_enabled():
	return os.environ.get(RMA_ENV, '') not in ('0', 'no', 'false', 'off')


def rma_threshold():
	try:
		return int(os.environ[RMA_THRESHOLD_ENV])
	except (KeyError, ValueError):
		return DEFAULT_RMA_THRESHOLD


def _rma_usable(blk_uuid):
	"""Whether payloads in this message can travel by RMA to blk_uuid."""
	if not blk_uuid or not rma_enabled():
		return False
	ops = _rma_ops()
	if not ops:
		return False
	try:
		return bool(ops.defw_rma_available(str(blk_uuid)))
	except Exception as e:
		logging.debug(f"RMA availability check failed: {e}")
		return False


def _rma_publish(ops, data):
	"""Register one payload for the peer to read, or return None to let the
	caller fall back to sending it inline."""
	try:
		rc, desc = ops.defw_rma_publish(data)
	except Exception as e:
		logging.warning(f"RMA publish raised, sending inline instead: {e}")
		return None
	if rc or not desc:
		logging.warning(f"RMA publish failed (rc={rc}), "
				f"sending {len(data)} bytes inline instead")
		return None
	handle, key, addr, length = (int(v) for v in desc.split(':'))
	return {'handle': handle, 'key': key, 'addr': addr, 'len': length}


def _encode_payloads(attachments, blk_uuid, published):
	"""Turn attachments into the wire payload list, choosing per payload
	between an RMA descriptor and inline base64."""
	use_rma = _rma_usable(blk_uuid)
	threshold = rma_threshold()
	ops = _rma_ops() if use_rma else None
	payloads = []

	for a in attachments:
		data = _to_bytes(a)
		desc = None
		if use_rma and len(data) >= threshold:
			desc = _rma_publish(ops, data)
		if desc is not None:
			published.append(desc['handle'])
			payloads.append(desc)
		else:
			payloads.append(base64.b64encode(data).decode('ascii'))

	return payloads


def attach_encode(msg, blk_uuid=None, threshold=DEFAULT_ATTACH_THRESHOLD,
		  published=None):
	"""Serialize msg to a YAML string, carrying any large buffers as
	attachments rather than in the YAML itself.

	Each attachment travels whichever way suits it. One at or above the RMA
	threshold, going to a peer reachable over the fabric, is left in our
	memory and named in the message by a descriptor; the receiver reads it
	directly and the region stays registered until it acknowledges. Anything
	smaller, or bound for a peer that cannot do RMA, is inlined as base64.
	The two mix freely within a message, and a payload that cannot be
	registered simply goes inline instead.

	published, if given, is extended with the registration handle of every
	payload left in our memory. A caller that fails to send the message
	should pass it to attach_discard, or those registrations stay live until
	the endpoint is torn down.

	When msg contains no large buffers this is exactly yaml.dump(msg), so
	ordinary RPCs are unaffected either way.
	"""
	import yaml
	transformed, attachments = extract_attachments(msg, threshold)
	if attachments:
		transformed[ATTACH_DATA_KEY] = _encode_payloads(
			attachments, blk_uuid,
			published if published is not None else [])
	return yaml.dump(transformed)


def attach_discard(published):
	"""Release registrations made for a message that was never sent."""
	ops = _rma_ops()
	if not ops:
		return
	for handle in published:
		try:
			ops.defw_rma_discard(handle)
		except Exception as e:
			logging.warning(f"RMA discard of handle {handle} failed: {e}")


def _rma_fetch(desc, blk_uuid):
	ops = _rma_ops()
	if not ops:
		raise RuntimeError("message carries an RMA attachment but this "
				   "DEFw build has no RMA support to fetch it")
	if not blk_uuid:
		raise RuntimeError("message carries an RMA attachment but the "
				   "sending agent is unknown, so it cannot be "
				   "fetched")
	rc, data = ops.defw_rma_fetch(str(blk_uuid), desc['handle'],
				      desc['key'], desc['addr'], desc['len'])
	if rc or data is None:
		raise RuntimeError(
			f"RMA fetch failed (rc={rc}) for {desc['len']} bytes "
			f"from agent {blk_uuid}")
	return data


def _decode_payloads(payloads, blk_uuid):
	"""Inverse of _encode_payloads: an entry either holds the data or names
	a region to read it from."""
	return [_rma_fetch(p, blk_uuid) if isinstance(p, dict)
		else base64.b64decode(p)
		for p in payloads]


def attach_load(msg_str, blk_uuid=None):
	"""Inverse of attach_encode: yaml.load msg_str and restore any
	attachments it carries, reading over the fabric those the message names
	rather than contains. blk_uuid identifies the sending agent and is
	needed only for the latter. Equivalent to a plain yaml.load for messages
	without attachments."""
	import yaml
	obj = yaml.load(msg_str, Loader=yaml.Loader)
	if isinstance(obj, dict) and ATTACH_DATA_KEY in obj:
		payloads = obj.pop(ATTACH_DATA_KEY)
		obj = reinject_attachments(obj,
					   _decode_payloads(payloads, blk_uuid))
	return obj
