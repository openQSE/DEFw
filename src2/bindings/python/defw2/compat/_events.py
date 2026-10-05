"""v1's events on v2: one sink per compat process, and what feeds it.

v1 pushed an event to a process by calling put on its event queue, an
ordinary remote call. On v2 every compat process listens, as every v1
process did, and serves one sink. A remote QPM's completions and the
directory's changes arrive there, and a reader thread puts each on the
caller's own queue, the defw_event_baseapi.BaseEventAPI it registered, in
the shape v1 put it there:

- a completion as api_events.Event(evtype, record), the record read_cq
  answers with. An event describes a statevector and never carries it, so
  the reader fetches it with peek_cq first, since v1's record held it;
- a change in the directory as v1's dictionary, with the v1 service record
  when the service connected.

Events are delivered at most once. A slow sweep peeks at the tasks this
process submitted, and delivers any completion that is ready and whose
event never came, so the completion queue really is the fallback. A
recovered completion is what peek_cq answers, which says so in its
poll_operation.

v1 also told a process when its link to the directory came and went, as
peer events. v2 has no links to watch, so compat asks the directory which
runtime it is now and then, and makes the peer events v1 would have:
PEER_LOST when it stops answering or turns out to be another runtime, and
PEER_READY when it answers again. A restarted directory holds no
subscriptions, and on PEER_READY the v1 code that made them makes them
again, as it did on v1.
"""

import collections
import copy
import json
import logging
import os
import threading
import time
import uuid

from .._defw2 import lib
from .._dir import DIR_SERVICE
from .._event import EventSink
from .._qpm import API_QPM_EXECUTION, QPM_COMPLETION
from .._runtime import DefwError
from . import _mapping as m
from . import _remote, _state

log = logging.getLogger('defw2.compat')

# How often the sweep looks for completions whose events never came, and
# how often the directory is asked which runtime it is.
SWEEP_MS = int(os.environ.get('DEFW2_COMPAT_SWEEP_MS', '5000'))
CHECK_MS = int(os.environ.get('DEFW2_COMPAT_DIRSVC_CHECK_MS', '2000'))

# How long the directory has to answer. Over ofi+tcp a call to a process
# that has gone fails only when its time is up, so this, not a call's usual
# 10 s, decides how soon a directory that has stopped is seen to be gone.
CHECK_TIMEOUT_MS = int(os.environ.get('DEFW2_COMPAT_DIRSVC_TIMEOUT_MS',
				      '2000'))

# How many deliveries to remember, so a completion that both its event and
# the sweep found reaches each queue once.
DELIVERED_MAX = 4096

# What v1's defw_workers.is_dirsvc_peer_event looks for.
DIRSVC = 'dirsvc'

_NO_QUEUE = ('no local event queue {!r}: register_external() it before '
	     'registering for events')


def type_text(evtype):
	"""A v1 evtype as an event's type: JSON, so it comes back as itself."""
	try:
		return json.dumps(evtype)
	except (TypeError, ValueError):
		return str(evtype)


def evtype_of(text):
	"""The v1 evtype an event's type stands for."""
	if text is None:
		return None
	try:
		return json.loads(text)
	except ValueError:
		return text


def peer_event(kind, runtime_id, reason):
	"""A v1 peer event about the directory."""
	return {'event_type': kind, 'peer_handle': runtime_id,
		'remote_runtime_id': runtime_id, 'node_type': DIRSVC,
		'reason': reason, 'timestamp': time.time()}


def _queue(class_id):
	"""The caller's own event queue, the BaseEventAPI it registered, or
	None when it has gone."""
	import defw_common_def
	try:
		return defw_common_def.get_class_from_db(class_id)
	except Exception:  # noqa: BLE001
		log.debug('the event queue %s is gone', class_id)
		return None


def _peek(target, cid, reservation_id, token, capacity):
	"""peek_cq of one completion, with room for its statevector."""
	return _remote.collect(target, 'peek_cq', {
		'cid': cid, 'qtask_id': 0,
		'reservation_id': m.context_reservation(reservation_id),
		'token': m.context_token(token)}, capacity)


class DirectoryWatch:
	"""Peer events for the directory, from asking it which runtime it is.

	ask returns the directory's runtime ID and raises DefwError when the
	directory does not answer. emit is given each peer event. lost is
	told when the directory's subscriptions stop being this process's,
	because the directory went or because it is another one now.
	"""

	def __init__(self, ask, emit, lost):
		self._ask = ask
		self._emit = emit
		self._lost = lost
		self.runtime_id = None
		self.up = False

	def check(self):
		try:
			runtime_id = self._ask()
		except DefwError as error:
			if error.category not in ('transport', 'timeout'):
				raise
			if self.up:
				self._down('unreachable')
			return
		if self.runtime_id is None:
			# The first answer is where it starts, not a change.
			self.runtime_id = runtime_id
			self.up = True
			return
		if runtime_id != self.runtime_id:
			if self.up:
				self._down('restarted')
			self.runtime_id = runtime_id
		if not self.up:
			self.up = True
			self._emit(peer_event('PEER_READY', runtime_id,
					      'reconnected'))

	def _down(self, reason):
		self.up = False
		self._lost(self.runtime_id)
		self._emit(peer_event('PEER_LOST', self.runtime_id, reason))


class Hub:
	"""This process's sink, the thread that reads it, and the thread that
	sweeps for lost completions and watches the directory."""

	def __init__(self, runtime):
		self._sink = EventSink(runtime)
		self._sink.accept(QPM_COMPLETION)
		self._sink.accept(DIR_SERVICE)
		self._lock = threading.Lock()
		self._completions = []
		self._watched = {}
		self._delivered = collections.OrderedDict()
		self._subscriptions = {}
		self._orphans = []
		self._listeners = []
		self._watch = DirectoryWatch(self._ask_directory,
					     self._notify, self._orphan)
		self._watch_lock = threading.Lock()
		self._stop = threading.Event()
		self._threads = [
			threading.Thread(target=self._read, daemon=True,
					 name='defw2-compat-events'),
			threading.Thread(target=self._keep, daemon=True,
					 name='defw2-compat-sweep'),
		]
		for thread in self._threads:
			thread.start()

	def close(self):
		self._stop.set()
		self._sink.close()
		for thread in self._threads:
			if thread is not threading.current_thread():
				thread.join(10)

	# --- completions from a remote QPM

	def register_completions(self, target, a):
		"""register_event_notification on a remote QPM: register this
		process's sink there, under the caller's class_id."""
		class_id = a.get('class_id')
		if _queue(class_id) is None:
			raise m.MappingError(_NO_QUEUE.format(class_id))
		filters = dict(a.get('filters') or {})
		registration = {
			'target': target,
			'class_id': class_id,
			'evtype': a.get('evtype'),
			'type': type_text(a.get('evtype')),
			'reservation_id': a.get('reservation_id'),
			'token': a.get('token'),
			'filters': filters,
		}
		# Kept before the QPM hears of it, so its first event, which may
		# come before the answer does, finds it.
		with self._lock:
			self._completions.append(registration)
		try:
			decision = target.qpm.register_event_notification(
				self._sink, type=registration['type'],
				tag=class_id,
				extra={'filters': filters} if filters else None,
				reservation_id=m.context_reservation(
					a.get('reservation_id')),
				token=m.context_token(a.get('token')),
				traceparent=_remote.traceparent())
		except Exception:
			with self._lock:
				self._completions.remove(registration)
			raise
		return m.typed_to_answer('decision', decision)

	def watch(self, target, cid, reservation_id, token):
		"""A task this process submitted, for the sweep to look after
		while a registration with its QPM wants its completion."""
		entry = {'target': target, 'reservation_id': reservation_id,
			 'token': token, 'since': time.monotonic(),
			 'ready_at': None}
		with self._lock:
			wanted = any(r['target'] is target
				     for r in self._completions)
			if wanted:
				self._watched[cid] = entry

	def _registration(self, source, class_id, kind):
		with self._lock:
			named = [r for r in self._completions
				 if r['class_id'] == class_id and
				 r['type'] == kind]
		for registration in named:
			if registration['target'].runtime_id == source:
				return registration
		return named[0] if named else None

	def _completion(self, event):
		task = event.payload
		registration = self._registration(event.source, event.tag,
						  event.type)
		if task is None or registration is None:
			log.debug('a completion for %s, which nothing here '
				  'registered for', event.tag)
			return
		record = m.typed_to_answer('task', task)
		if m.find_stub(record) is not None:
			self._fetch_statevector(registration, task, record)
		self._deliver(registration, task.cid, record)
		self._unwatch(task.cid)

	def _fetch_statevector(self, registration, task, record):
		"""The statevector the event described, from the completion
		queue, into the record, as v1's event carried it."""
		try:
			answer = _peek(registration['target'], task.cid,
				       task.reservation_id or
				       registration['reservation_id'],
				       registration['token'],
				       task.statevector.nbytes)
			_remote.fill_statevector(record, answer)
		except Exception:  # noqa: BLE001
			log.exception('the statevector of %s is not in the '
				      'completion queue any more, so its event '
				      'arrives without it', task.cid)

	def _deliver(self, registration, cid, record):
		"""Put a completion on a registration's queue, once."""
		import api_events
		key = (cid, registration['class_id'], registration['type'])
		with self._lock:
			if cid is not None and key in self._delivered:
				return False
			self._delivered[key] = True
			while len(self._delivered) > DELIVERED_MAX:
				self._delivered.popitem(last=False)
		queue = _queue(registration['class_id'])
		if queue is None:
			return False
		queue.put(api_events.Event(registration['evtype'], record))
		return True

	def _unwatch(self, cid):
		with self._lock:
			self._watched.pop(cid, None)

	@staticmethod
	def _matches(registration, record):
		"""Whether a recovered completion is one the registration's QPM
		would have sent it, by v1's rules."""
		reservation_id = registration['reservation_id']
		if reservation_id is not None and \
		   record.get('reservation_id') != reservation_id:
			return False
		for key, value in registration['filters'].items():
			actual = record.get(key)
			if isinstance(value, (list, tuple, set)):
				if actual not in value:
					return False
			elif actual != value:
				return False
		return True

	def _sweep(self):
		"""Deliver each watched completion that is ready and whose event
		has had a whole sweep to arrive and did not."""
		now = time.monotonic()
		with self._lock:
			watched = list(self._watched.items())
		for cid, entry in watched:
			if self._stop.is_set():
				return
			if now - entry['since'] >= SWEEP_MS / 1000.0:
				self._sweep_one(cid, entry, now)

	def _sweep_one(self, cid, entry, now):
		target = entry['target']
		try:
			answer = _peek(target, cid, entry['reservation_id'],
				       entry['token'], target.capacity(cid))
		except Exception:  # noqa: BLE001
			log.exception('sweeping %s', cid)
			return
		if not answer.completion_ready:
			if answer.outcome in _remote.NEVER:
				self._unwatch(cid)
			return
		if entry['ready_at'] is None:
			# Ready now, so its event gets one more sweep to come.
			entry['ready_at'] = now
			return
		record = m.typed_to_answer('task', answer)
		_remote.fill_statevector(record, answer)
		with self._lock:
			matching = [r for r in self._completions
				    if r['target'] is target and
				    self._matches(r, record)]
		for registration in matching:
			delivered = self._deliver(registration, cid,
						  copy.deepcopy(record))
			if delivered:
				log.warning('recovered the completion of %s, '
					    'whose event never came', cid)
		self._unwatch(cid)

	# --- the directory

	def subscribe_directory(self, directory, event_type, class_id, filters):
		"""v1's directory register_event_notification: subscribe this
		process's sink, and name the registration as v1 did."""
		registration_id = str(uuid.uuid4())
		filters = dict(filters or {})
		self._baseline(directory)
		# Kept first, for the reason register_completions says.
		entry = {'event_type': event_type, 'class_id': class_id,
			 'subscription': None,
			 'directory_id': self._watch.runtime_id}
		with self._lock:
			self._subscriptions[registration_id] = entry
		connected = event_type == 'SERVICE_CONNECTED'
		try:
			entry['subscription'] = directory.v2.subscribe(
				self._sink,
				service_id=filters.get('service_id') or None,
				service_type=filters.get('service_type') or
				None, connected=connected,
				disconnected=not connected, tag=registration_id)
		except Exception:
			with self._lock:
				self._subscriptions.pop(registration_id, None)
			raise
		return registration_id

	def unsubscribe_directory(self, directory, registration_id):
		with self._lock:
			entry = self._subscriptions.pop(registration_id, None)
		if entry is None:
			return False
		if entry['subscription'] is not None:
			try:
				directory.v2.unsubscribe(entry['subscription'])
			except DefwError:
				# The directory is gone, and the subscription
				# with it.
				pass
		return True

	def _change(self, event):
		with self._lock:
			entry = self._subscriptions.get(event.tag)
		change = event.payload
		if entry is None or change is None:
			log.debug('a directory event for %s, which nothing '
				  'here subscribed to', event.tag)
			return
		from ._directory import v1_record
		v2 = change['record']
		v1 = {
			'event': event.type,
			'directory_runtime_id': event.source,
			'service_id': v2['service_id'],
			'service_type': v2['service_type'],
			'runtime_id': v2['runtime_id'],
			'peer_handle': v2['runtime_id'],
		}
		if change['connected']:
			v1['service_record'] = v1_record(v2)
			directory = _state.directory()
			if directory is not None:
				directory.seen[(v2['service_id'],
						v2['runtime_id'])] = v2
		else:
			v1['reason'] = change['reason'] or ''
		queue = _queue(entry['class_id'])
		if queue is not None:
			queue.put(v1)

	def add_peer_listener(self, listener):
		with self._lock:
			if listener not in self._listeners:
				self._listeners.append(listener)
		directory = _state.directory()
		if directory is not None:
			self._baseline(directory)
		return listener

	def remove_peer_listener(self, listener):
		with self._lock:
			if listener in self._listeners:
				self._listeners.remove(listener)

	def _baseline(self, directory):
		"""Learn which directory this is before anything depends on it,
		so that a restart before the first check is still seen."""
		with self._watch_lock:
			if self._watch.runtime_id is None:
				self._ask_safely()

	def _ask_safely(self):
		"""One check, with the watch lock held."""
		try:
			self._watch.check()
		except DefwError:
			log.exception('asking the directory its runtime')

	def _ask_directory(self):
		directory = _state.directory()
		if directory is None:
			raise DefwError(lib.DEFW2_ERR_CONFIG, 'transport',
					'no directory')
		return directory.v2.runtime_id(timeout_ms=CHECK_TIMEOUT_MS)

	def _orphan(self, runtime_id):
		"""The directory's subscriptions are not this process's any
		more. v1 code makes new ones when it hears PEER_READY. Ones the
		same directory still holds are cancelled once it answers
		again."""
		with self._lock:
			for entry in self._subscriptions.values():
				held = entry['directory_id'] or runtime_id
				if entry['subscription'] is not None:
					self._orphans.append(
						(held, entry['subscription']))
			self._subscriptions.clear()

	def _cancel_orphans(self):
		with self._lock:
			if not self._orphans or not self._watch.up:
				return
			orphans, self._orphans = self._orphans, []
		directory = _state.directory()
		for directory_id, subscription in orphans:
			if directory is None or \
			   directory_id != self._watch.runtime_id:
				continue
			try:
				directory.v2.unsubscribe(subscription)
			except DefwError:
				pass

	def _notify(self, event):
		with self._lock:
			listeners = list(self._listeners)
		log.warning('the directory: %s, runtime %s, %s',
			    event['event_type'], event['remote_runtime_id'],
			    event['reason'])
		for listener in listeners:
			try:
				listener(dict(event))
			except Exception:  # noqa: BLE001
				log.exception('a peer event listener failed')

	def _check(self):
		with self._lock:
			wanted = bool(self._listeners or self._subscriptions or
				      self._orphans)
		if not wanted:
			return
		with self._watch_lock:
			self._ask_safely()
		self._cancel_orphans()

	# --- the threads

	def _read(self):
		for event in self._sink:
			try:
				if event.api == API_QPM_EXECUTION:
					self._completion(event)
				elif event.api == DIR_SERVICE.api:
					self._change(event)
			except Exception:  # noqa: BLE001
				log.exception('delivering %r', event)

	def _keep(self):
		next_sweep = time.monotonic() + SWEEP_MS / 1000.0
		next_check = time.monotonic() + CHECK_MS / 1000.0
		tick = min(SWEEP_MS, CHECK_MS, 250) / 1000.0
		while not self._stop.wait(tick):
			now = time.monotonic()
			try:
				if now >= next_check:
					next_check = now + CHECK_MS / 1000.0
					self._check()
				if now >= next_sweep:
					next_sweep = now + SWEEP_MS / 1000.0
					self._sweep()
			except Exception:  # noqa: BLE001
				log.exception('the event sweep')
