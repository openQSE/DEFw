"""Documents: any method of any API, as JSON.

A method with no typed form goes as a document. The request is a dict of
named arguments, sent as a JSON object, and the answer is whatever JSON
the service answered with:

	answer = qpm.document('qfw.qpm.telemetry', 'get_backend_info',
			      {'lib': 'qdmi'})

A service answers documents through ServiceHost, with a handler that has
document(api, method, request, traceparent). That is the only way a
document reaches Python. A document's method is the caller's to name, so it never picks a
handler's method by name on its own.

The differences are JSON's. A tuple arrives as a list, a key that is not a
string arrives as a string, a numpy value arrives as the Python value it
holds, and a value JSON cannot carry, such as bytes, fails the call rather
than being dropped.
"""

import json

from ._defw2 import ffi, lib
from ._runtime import DefwError, _status_out, _take_status, _text

# What the client's own refusals mean to a caller.
_REFUSED = {
	lib.DEFW2_ERR_INVALID: 'invalid-argument',
	lib.DEFW2_ERR_NOT_FOUND: 'not-found',
}


def _plain(value):
	"""A numpy scalar or array as the Python value JSON can carry."""
	if getattr(value, 'shape', None) == () and callable(
			getattr(value, 'item', None)):
		return value.item()
	if callable(getattr(value, 'tolist', None)):
		return value.tolist()
	raise TypeError('{} is not a JSON value'.format(type(value).__name__))


def dumps(value):
	"""value as a document's JSON text. TypeError or ValueError for a
	value JSON cannot carry."""
	return json.dumps(value, default=_plain)


def request_text(request):
	"""A request as the JSON text of an object of named arguments."""
	if request is None:
		return '{}'
	if not isinstance(request, dict):
		raise TypeError('a document is a dict of named arguments, not '
				'{}'.format(type(request).__name__))
	return dumps(request)


def call(binding, api, method, request, opts):
	"""One document call over binding, a defw2_binding_t *, with opts,
	a defw2_call_opts_t *. Raises DefwError when the call or the
	service fails."""
	text = request_text(request).encode('utf-8')
	answer = ffi.new('char **')
	holder = _status_out()
	rc = lib.defw2_doc_call(binding, api.encode('utf-8'),
				method.encode('utf-8'), text, opts, answer,
				holder)
	status = _take_status(holder)
	try:
		if rc != lib.DEFW2_OK:
			raise DefwError(rc, _REFUSED.get(rc, 'transport'),
					'{}.{} failed: {}'.format(
						api, method,
						_text(lib.defw2_strerror(rc))))
		if not status.ok:
			raise DefwError(status.code, status.category,
					status.message)
		text = ffi.string(answer[0]).decode('utf-8')
	finally:
		if answer[0] != ffi.NULL:
			lib.free(answer[0])
	try:
		return json.loads(text)
	except ValueError as error:
		raise DefwError(lib.DEFW2_ERR_INVALID, 'provider-failure',
				'{}.{} answered with something that is not '
				'JSON: {}'.format(api, method, error)) from None
