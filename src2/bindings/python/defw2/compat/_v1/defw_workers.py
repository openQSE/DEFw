"""v1's defw_workers, as far as peer events, on DEFw v2.

v1 told listeners when a peer connected, was lost or was removed. v2 has
no peer table, and a binding is valid while its directory record is. The
peer v1 code watches is the directory, so compat asks the directory which
runtime it is now and then, and tells listeners PEER_LOST when it stops
answering or turns out to be another runtime, and PEER_READY when it
answers again, as v1 would have. No other peer event fires.
"""

from defw2.compat import _events, _state


def add_peer_event_listener(listener):
	if not callable(listener):
		import defw_exception
		raise defw_exception.DEFwError(
			'Peer lifecycle listener is not callable')
	return _state.events().add_peer_listener(listener)


def remove_peer_event_listener(listener):
	events = _state.started_events()
	if events is not None:
		events.remove_peer_listener(listener)


def is_dirsvc_peer_event(event):
	return event.get('node_type') == _events.DIRSVC


def __getattr__(name):
	raise AttributeError(
		"defw2.compat's defw_workers has no {}: v1's workers are not on "
		'v2, only the peer event listeners are'.format(name))
