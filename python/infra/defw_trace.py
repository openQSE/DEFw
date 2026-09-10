"""
Trace context propagation across DEFw RPC.

A distributed trace only holds together if the caller's context reaches the
remote it invokes. HTTP and gRPC carry that in headers and are instrumented
automatically. DEFw uses its own RPC envelope, so it has to carry the context
itself.

DEFw does not know what a trace context is, and does not depend on any
tracing library. It moves an opaque dictionary from caller to remote and
scopes it around the dispatch. What goes in the dictionary, and what scoping
it means, are supplied by whoever is doing the tracing:

	import defw_trace

	def _inject(carrier):
		# fill carrier with whatever identifies the current context
		...

	def _attach(carrier):
		# make carrier current, return a token that undoes it
		...

	def _detach(token):
		...

	defw_trace.set_hooks(inject=_inject, attach=_attach, detach=_detach)

With no hooks registered, which is the default, injection produces an empty
carrier and attachment is a no-op. Nothing in DEFw changes behaviour and the
cost is a null check.

Wire compatibility runs both ways. A peer that predates this field simply
does not send one, and the receiver treats a missing or empty carrier as no
context. A peer that does send one to an older receiver is ignored. So the
two ends can be upgraded independently.

Hook failures are contained. Tracing must never break the RPC it is
observing, so an exception raised by a hook is swallowed and the call
proceeds untraced.
"""

CARRIER_KEY = 'trace_context'

_inject_hook = None
_attach_hook = None
_detach_hook = None


def set_hooks(inject=None, attach=None, detach=None):
	"""
	Register the propagation hooks.

	inject is called with an empty dict and fills it with the caller's
	current context. attach is called with a received dict, makes that
	context current, and returns a token. detach is called with that token
	once the dispatch finishes.
	"""
	global _inject_hook, _attach_hook, _detach_hook
	_inject_hook = inject
	_attach_hook = attach
	_detach_hook = detach


def clear_hooks():
	"""Drop the hooks and return to untraced behaviour."""
	global _inject_hook, _attach_hook, _detach_hook
	_inject_hook = None
	_attach_hook = None
	_detach_hook = None


def hooks_registered():
	return _inject_hook is not None or _attach_hook is not None


def inject():
	"""Build a carrier describing the caller's context, empty when untraced."""
	hook = _inject_hook
	if hook is None:
		return {}
	carrier = {}
	try:
		hook(carrier)
	except Exception:
		return {}
	return carrier


def attach(carrier):
	"""
	Make a received context current and return a token for detach().

	Returns None when nothing is tracing or the peer sent no context, which
	detach() accepts.
	"""
	hook = _attach_hook
	if hook is None or not carrier:
		return None
	try:
		return hook(carrier)
	except Exception:
		return None


def detach(token):
	"""Undo an attach(). Accepts None, so it is always safe in a finally."""
	if token is None or _detach_hook is None:
		return
	try:
		_detach_hook(token)
	except Exception:
		pass
