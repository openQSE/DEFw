"""The qfw.echo client.

One typed method, bound one to one, which is the shape every typed method
takes. Anything without a typed stub goes through the document tier, which
is not built yet. A client may be shared between threads.
"""

from ._defw2 import ffi, lib
from ._runtime import DefwError, _check, _status_out, _take_status

__all__ = ['Echo', 'PROVIDER_ECHO', 'API_ECHO']

API_ECHO = 'qfw.echo'
PROVIDER_ECHO = 1


class Echo:
	"""A binding on one echo service.

	Resolving an address is the expensive part of reaching a peer, so it
	happens once here and every call reuses it. Safe to share between
	threads.
	"""

	def __init__(self, runtime, address, provider_id=PROVIDER_ECHO,
		     timeout_ms=60000):
		self._runtime = runtime
		self._timeout_ms = timeout_ms
		out = ffi.new('defw2_binding_t **')
		_check(lib.defw2_binding_create(runtime.handle,
						address.encode(), provider_id,
						out),
		       'binding to ' + address)
		self._binding = out[0]

	@property
	def address(self):
		return ffi.string(
			lib.defw2_binding_address(self._binding)).decode()

	def _options(self, timeout_ms, traceparent):
		"""The call's options, and the string they point at.

		The caller holds both until the call returns. Keeping the
		string on the client instead would let a second thread's call
		replace it, and free it, while the first is still using it.
		"""
		opts = ffi.new('defw2_call_opts_t *')
		opts.timeout_ms = (self._timeout_ms if timeout_ms is None
				   else timeout_ms)
		trace = ffi.NULL
		if traceparent:
			trace = ffi.new('char[]', traceparent.encode())
		opts.traceparent = trace
		return opts, trace

	def echo(self, payload, timeout_ms=None, traceparent=None):
		"""Send bytes and get them back. Raises on a failed call."""
		if isinstance(payload, str):
			payload = payload.encode()
		opts, trace = self._options(timeout_ms, traceparent)
		reply = ffi.new('defw2_buffer_t *')
		holder = _status_out()

		rc = lib.defw2_echo(self._binding, payload, len(payload), opts,
				    reply, holder)
		status = _take_status(holder)
		if rc != lib.DEFW2_OK:
			lib.defw2_buffer_free(reply)
			raise DefwError(rc, 'transport', 'echo failed')
		try:
			status.raise_for_status()
			if reply.len == 0:
				return b''
			return bytes(ffi.buffer(reply.data, reply.len))
		finally:
			lib.defw2_buffer_free(reply)

	def echo_bulk(self, payload, timeout_ms=None, traceparent=None):
		"""Move bytes through registered memory and get them back.

		The payload never travels inside the message, so this is the
		path a statevector takes.
		"""
		if isinstance(payload, str):
			payload = payload.encode()
		opts, trace = self._options(timeout_ms, traceparent)
		source = ffi.new('char[]', payload)
		sink = ffi.new('char[]', len(payload))
		moved = ffi.new('uint64_t *')
		holder = _status_out()

		rc = lib.defw2_echo_bulk(self._binding, source, sink,
					 len(payload), opts, moved, holder)
		status = _take_status(holder)
		if rc != lib.DEFW2_OK:
			raise DefwError(rc, 'transport', 'echo_bulk failed')
		status.raise_for_status()
		return bytes(ffi.buffer(sink, len(payload))), int(moved[0])

	def close(self):
		if self._binding is not None:
			lib.defw2_binding_free(self._binding)
			self._binding = None

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False
