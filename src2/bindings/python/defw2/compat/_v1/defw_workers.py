"""v1's defw_workers, as far as peer events, on DEFw v2.

v1 told listeners when a peer connected, was lost or was removed. v2 has
no peer table: a binding is valid while its directory record is, so no
peer event ever fires. Adding and removing listeners works, for code that
installs them, and they are never called.
"""

_listeners = []


def add_peer_event_listener(listener):
	if listener not in _listeners:
		_listeners.append(listener)
	return listener


def remove_peer_event_listener(listener):
	if listener in _listeners:
		_listeners.remove(listener)


def is_dirsvc_peer_event(event):
	return False


def __getattr__(name):
	raise AttributeError(
		"defw2.compat's defw_workers has no {}: v1's workers are not on "
		'v2, only the peer event listeners are'.format(name))
