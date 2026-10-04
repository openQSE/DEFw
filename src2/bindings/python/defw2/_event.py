"""Events from Python: sinks that take them, publishers that send them.

A caller that wants events serves a sink, which is a provider in its own
runtime, so the runtime must be a server. It registers the sink with a
service, and takes what arrives with next(), or by iterating:

	with defw2.Runtime(role='server') as rt:
		sink = defw2.EventSink(rt)
		qpm.register_event_notification(sink, type='done', tag='job-7',
						reservation_id=rid)
		for event in sink:
			task = event.payload	# a defw2.Task, for a completion

No thread runs Python for a sink. C queues each event as it arrives, and a
Python thread takes it with the interpreter lock released for the wait,
which is how a ServiceHost takes calls.

A service sends events through a publisher, which copies each one and
returns at once, and delivers from an execution stream of its own:

	publisher = defw2.EventPublisher(rt)
	try:
		publisher.publish(defw2.QPM_COMPLETION, request.target, task,
				  type=request.type)
	except defw2.TargetGone:
		forget(request.target)

Delivery is at most once. A full sink or a full queue loses an event, and
the service's own record, such as a completion queue, is how a caller
recovers.
"""

import collections
import math
import time

from ._defw2 import ffi, lib
from ._dir import _Kept
from ._runtime import DefwError, _text

__all__ = [
	'Event', 'EventKind', 'EventPublisher', 'EventSink', 'EventTarget',
	'PROVIDER_EVENT', 'TargetGone',
]

PROVIDER_EVENT = lib.DEFW2_PROVIDER_EVENT

# The longest one wait in C lasts while Python waits longer, so that an
# interrupt is seen between waits.
_WAIT_MS = 200

# What a code from the event calls means, as a category. The first four
# are what a publish raises TargetGone with.
_CATEGORY = {
	lib.DEFW2_ERR_TRANSPORT: 'transport',
	lib.DEFW2_ERR_TIMEOUT: 'timeout',
	lib.DEFW2_ERR_NOT_FOUND: 'not-found',
	lib.DEFW2_ERR_VERSION: 'version-mismatch',
	lib.DEFW2_ERR_INVALID: 'invalid-argument',
	lib.DEFW2_ERR_CONFIG: 'invalid-argument',
	lib.DEFW2_ERR_BUSY: 'pending-capacity',
	lib.DEFW2_ERR_CANCELLED: 'cancelled',
}

EventTarget = collections.namedtuple(
	'EventTarget', ('address', 'provider_id', 'tag'),
	defaults=(PROVIDER_EVENT, None))
EventTarget.__doc__ = """Where a service sends events.

A sink's address and provider, and the tag its owner registered with,
which comes back on every event, so one sink can tell its registrations
apart. A plain (address, provider_id, tag) tuple does as well.
"""


def _error(rc, what):
	return DefwError(rc, _CATEGORY.get(rc, 'transport'), '{}: {}'.format(
		what, _text(lib.defw2_strerror(rc))))


def _closed():
	return DefwError(lib.DEFW2_ERR_NOT_FOUND, 'not-found',
			 'the sink is closed')


class TargetGone(DefwError):
	"""An earlier event to this target was not delivered.

	It could not be reached, did not answer in time, has no sink, or does
	not take this kind of event. The service should drop the
	registration. The event being published was not sent, and the next
	one to the same target is tried afresh.
	"""


class EventKind:
	"""One API's kind of event, as the binding handles it.

	accept readies a sink for it, read turns one into its payload, and
	send publishes one. Each API defines its own, such as
	defw2.QPM_COMPLETION.
	"""

	def __init__(self, api, name, accept, read, send):
		self.api = api
		self.name = name
		self.accept = accept
		self.read = read
		self.send = send

	def __repr__(self):
		return 'EventKind({}.{})'.format(self.api, self.name)


class Event:
	"""An event as a sink took it.

	api and name say what it is, and payload is what its kind carries, a
	defw2.Task for a QPM completion. type and tag are the registration's,
	source is the runtime that sent it, and seq counts that sender's
	events to this target from 1, so a gap shows a loss.
	"""

	__slots__ = ('api', 'name', 'type', 'tag', 'source', 'seq',
		     'traceparent', 'payload')

	def __init__(self, **values):
		for field in self.__slots__:
			setattr(self, field, values.get(field))

	def __repr__(self):
		return 'Event({}.{}, type={!r}, tag={!r}, seq={})'.format(
			self.api, self.name, self.type, self.tag, self.seq)


class EventSink:
	"""A sink in this process, for the events services send it.

	The runtime must be a server, since a sink is a provider. Close the
	sink before the runtime, or use it as a context manager. A sink takes
	only the kinds it has accepted. QPM.register_event_notification
	accepts completions for the sink it is given.
	"""

	def __init__(self, runtime, provider_id=PROVIDER_EVENT, depth=0,
		     max_bytes=0):
		opts = ffi.new('defw2_event_sink_opts_t *')
		opts.depth = depth
		opts.max_bytes = max_bytes
		out = ffi.new('defw2_event_sink_t **')
		rc = lib.defw2_event_sink_create(runtime.handle, provider_id,
						 opts, out)
		if rc == lib.DEFW2_ERR_CONFIG:
			raise _error(rc, 'a sink needs a runtime that listens, '
				     "one made with role='server'")
		if rc != lib.DEFW2_OK:
			raise _error(rc, 'serving a sink on provider {}'.format(
				provider_id))
		self._runtime = runtime
		self._sink = out[0]
		self._kinds = {}

	def _live(self):
		"""The C sink, until this sink or its runtime closes. The
		runtime closes an open sink as it stops, and then frees it."""
		if self._sink is not None and self._runtime.closed:
			self._sink = None
		if self._sink is None:
			raise _closed()
		return self._sink

	def accept(self, kind):
		"""Take events of this kind from now on. Asking again is
		harmless."""
		key = (kind.api, kind.name)
		known = key in self._kinds
		self._kinds[key] = kind
		rc = kind.accept(self._live())
		if rc != lib.DEFW2_OK:
			if not known:
				del self._kinds[key]
			raise _error(rc, 'accepting {!r}'.format(kind))

	@property
	def address(self):
		return _text(lib.defw2_event_sink_address(self._live()))

	@property
	def provider_id(self):
		return lib.defw2_event_sink_provider_id(self._live())

	def target(self, tag=None):
		"""Where a service sends this sink events, under tag."""
		return EventTarget(self.address, self.provider_id, tag)

	def next(self, timeout_ms=None):
		"""The next event, or None if timeout_ms passes first.

		With no timeout it waits for an event. Once the sink is closed
		it raises DefwError with the not-found category, which is how a
		reading loop learns to stop. The wait holds no lock, the
		interpreter's included, and an interrupt is seen within a fifth
		of a second.
		"""
		deadline = None
		if timeout_ms is not None:
			deadline = time.monotonic() + timeout_ms / 1000.0
		event = ffi.new('defw2_event_t *')
		while True:
			wait = _WAIT_MS
			if deadline is not None:
				left = deadline - time.monotonic()
				wait = max(0, min(wait, math.ceil(left * 1000)))
			sink = self._live()
			rc = lib.defw2_event_sink_next(sink, wait, event)
			if rc == lib.DEFW2_OK:
				try:
					return self._event(event)
				finally:
					lib.defw2_event_free(event)
			if rc == lib.DEFW2_ERR_NOT_FOUND:
				raise _closed()
			if rc != lib.DEFW2_ERR_TIMEOUT:
				raise _error(rc, 'taking an event')
			if deadline is not None and \
			   time.monotonic() >= deadline:
				return None

	def __iter__(self):
		"""Every event, until the sink closes."""
		while True:
			try:
				yield self.next()
			except DefwError as error:
				if error.code == lib.DEFW2_ERR_NOT_FOUND:
					return
				raise

	def _event(self, event):
		api = _text(event.api)
		name = _text(event.name)
		kind = self._kinds.get((api, name))
		return Event(api=api, name=name, type=_text(event.type),
			     tag=_text(event.tag), source=_text(event.source),
			     seq=event.seq,
			     traceparent=_text(event.traceparent),
			     payload=kind.read(event) if kind is not None
			     else None)

	def stats(self):
		"""received counts events queued on arrival, refused those a
		full sink turned away, and waiting those queued now."""
		stats = ffi.new('defw2_event_sink_stats_t *')
		lib.defw2_event_sink_stats(self._live(), stats)
		return {'received': stats.received, 'refused': stats.refused,
			'waiting': stats.waiting}

	def close(self):
		"""Stop taking events and drop those still waiting. A reader in
		next() wakes and hears the sink is closed, and senders hear
		that it is gone."""
		if self._sink is not None and not self._runtime.closed:
			lib.defw2_event_sink_destroy(self._sink)
		self._sink = None

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False


class EventPublisher:
	"""Sends a service's events, from an execution stream of its own.

	Deliveries go to every target at once, each target's in the order
	they were published, each within timeout_ms, one second unless
	given. depth is how many may wait per target and max_bytes how many
	bytes may wait in all, and 0 leaves either at its default. Close the
	publisher when the service stops, or leave it to the runtime.
	"""

	def __init__(self, runtime, timeout_ms=0, depth=0, max_bytes=0):
		opts = ffi.new('defw2_event_publisher_opts_t *')
		opts.timeout_ms = timeout_ms
		opts.depth = depth
		opts.max_bytes = max_bytes
		out = ffi.new('defw2_event_publisher_t **')
		rc = lib.defw2_event_publisher_create(runtime.handle, opts, out)
		if rc != lib.DEFW2_OK:
			raise _error(rc, 'starting a publisher')
		self._runtime = runtime
		self._pub = out[0]

	def _live(self):
		if self._pub is None or self._runtime.closed:
			raise DefwError(lib.DEFW2_ERR_CANCELLED, 'cancelled',
					'the publisher is closed')
		return self._pub

	def publish(self, kind, target, payload, type=None, traceparent=None):
		"""Queue one event for target, and return without waiting.

		True when it is queued, False when a full queue dropped it.
		Raises TargetGone when an earlier event to target was not
		delivered, and DefwError when this one cannot be sent at all,
		such as one with a field too long for the wire. type is the
		registration's, and traceparent the work the event belongs to.
		"""
		address, provider_id, tag = target
		kept = _Kept()
		where = ffi.new('defw2_event_target_t *')
		where.address = kept.str(address)
		where.provider_id = provider_id
		where.tag = kept.str(tag)
		rc = kind.send(self._live(), where, kept.str(type), payload,
			       kept.str(traceparent))
		if rc == lib.DEFW2_OK:
			return True
		if rc == lib.DEFW2_ERR_BUSY:
			return False
		if lib.defw2_event_target_gone(rc):
			raise TargetGone(rc, _CATEGORY.get(rc, 'transport'),
					 'an earlier event to {} was not '
					 'delivered: {}'.format(
						 address,
						 _text(lib.defw2_strerror(rc))))
		raise _error(rc, 'publishing to {}'.format(address))

	def stats(self):
		"""published counts events queued, delivered those a sink took,
		refused those a full sink turned away, failed the deliveries
		that failed, dropped those never sent, waiting those queued or
		on their way, and targets the targets it remembers."""
		stats = ffi.new('defw2_event_publisher_stats_t *')
		lib.defw2_event_publisher_stats(self._live(), stats)
		return {name: getattr(stats, name) for name in (
			'published', 'delivered', 'refused', 'failed',
			'dropped', 'waiting', 'targets')}

	def close(self):
		"""Drop what has not been sent, and wait for what is on its way,
		at most the time limit."""
		if self._pub is not None:
			lib.defw2_event_publisher_destroy(self._pub)
			self._pub = None

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False
