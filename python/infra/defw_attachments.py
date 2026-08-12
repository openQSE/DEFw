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

# Reserved top-level key carrying the inline (base64) attachment payloads.
# This is the eager path: it works on every transport and stays the right
# choice for small payloads and for peers we cannot reach over the fabric.
ATTACH_DATA_KEY = '__defw_attachment_data__'

# Reserved top-level key carrying RMA descriptors instead of payloads. The
# message then names the data rather than containing it, and the receiver
# reads it directly out of the sender's memory over the fabric.
ATTACH_RMA_KEY = '__defw_attachment_rma__'

# Set DEFW_RMA_ATTACHMENTS=1 to move payloads by RMA where the peer supports
# it. Off by default: the rendezvous is in place but choosing it
# automatically (on payload size) is a separate step.
RMA_ENV = 'DEFW_RMA_ATTACHMENTS'

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


def rma_requested():
	return os.environ.get(RMA_ENV, '') not in ('', '0', 'no', 'false')


def _rma_usable(blk_uuid):
	"""Whether this message's payloads can travel by RMA to blk_uuid."""
	if not blk_uuid or not rma_requested():
		return False
	ops = _rma_ops()
	if not ops:
		return False
	try:
		return bool(ops.defw_rma_available(str(blk_uuid)))
	except Exception as e:
		logging.debug(f"RMA availability check failed: {e}")
		return False


def _rma_publish_all(attachments):
	"""Register every attachment for the peer to read and return the
	descriptors. Returns None if any registration fails, having released the
	ones already made, so the caller can fall back to sending inline."""
	ops = _rma_ops()
	descs = []
	for a in attachments:
		try:
			rc, desc = ops.defw_rma_publish(_to_bytes(a))
		except Exception as e:
			logging.warning(f"RMA publish raised, using inline: {e}")
			rc, desc = -1, None
		if rc or not desc:
			for d in descs:
				ops.defw_rma_discard(d['handle'])
			return None
		handle, key, addr, length = (int(v) for v in desc.split(':'))
		descs.append({'handle': handle, 'key': key,
			      'addr': addr, 'len': length})
	return descs


def attach_encode(msg, blk_uuid=None, threshold=DEFAULT_ATTACH_THRESHOLD):
	"""Serialize msg to a YAML string, carrying any large buffers as
	attachments rather than in the YAML itself.

	Attachments travel one of two ways. If RMA is enabled and the peer named
	by blk_uuid is reachable over the fabric, the message carries only
	descriptors and the receiver reads the data out of our memory directly;
	each region stays registered until the receiver acknowledges it.
	Otherwise the payloads are inlined as base64, which works everywhere.

	When msg contains no large buffers this is exactly yaml.dump(msg), so
	ordinary RPCs are unaffected either way.
	"""
	import yaml
	transformed, attachments = extract_attachments(msg, threshold)
	if attachments:
		descs = None
		if _rma_usable(blk_uuid):
			descs = _rma_publish_all(attachments)
		if descs is not None:
			transformed[ATTACH_RMA_KEY] = descs
		else:
			transformed[ATTACH_DATA_KEY] = [
				base64.b64encode(_to_bytes(a)).decode('ascii')
				for a in attachments]
	return yaml.dump(transformed)


def _rma_fetch_all(descs, blk_uuid):
	ops = _rma_ops()
	if not ops:
		raise RuntimeError("received RMA attachments but this DEFw build "
				   "has no RMA support to fetch them")
	if not blk_uuid:
		raise RuntimeError("received RMA attachments but the sending "
				   "agent is unknown, so they cannot be fetched")
	attachments = []
	for d in descs:
		rc, data = ops.defw_rma_fetch(str(blk_uuid), d['handle'],
					      d['key'], d['addr'], d['len'])
		if rc or data is None:
			raise RuntimeError(
				f"RMA fetch failed (rc={rc}) for {d['len']} bytes "
				f"from agent {blk_uuid}")
		attachments.append(data)
	return attachments


def attach_load(msg_str, blk_uuid=None):
	"""Inverse of attach_encode: yaml.load msg_str and restore any
	attachments it carries, reading them over the fabric when the message
	carries descriptors rather than inline data. blk_uuid identifies the
	sending agent and is required only for the RMA case. Equivalent to a
	plain yaml.load for messages without attachments."""
	import yaml
	obj = yaml.load(msg_str, Loader=yaml.Loader)
	if isinstance(obj, dict):
		if ATTACH_RMA_KEY in obj:
			descs = obj.pop(ATTACH_RMA_KEY)
			obj = reinject_attachments(
				obj, _rma_fetch_all(descs, blk_uuid))
		elif ATTACH_DATA_KEY in obj:
			encoded = obj.pop(ATTACH_DATA_KEY)
			attachments = [base64.b64decode(s) for s in encoded]
			obj = reinject_attachments(obj, attachments)
	return obj
