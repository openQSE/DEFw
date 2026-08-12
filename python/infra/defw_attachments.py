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
import logging

# Reserved key identifying an extracted-attachment marker in the YAML message.
# Chosen to be extremely unlikely to collide with an application dict key.
ATTACH_MARKER_KEY = '__defw_attachment__'

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
