import logging
import threading
import time
import uuid

from defw_exception import DEFwError, DEFwNotFound


STATE_UP = 'UP'
STATE_DOWN = 'DOWN'
STATE_TIMED_OUT = 'TIMED_OUT'
STATE_DEREGISTERED = 'DEREGISTERED'
DEFAULT_RETENTION_SECONDS = 300
QPM_SERVICE_TYPE = 'qfw.qpm'
SERVICE_CONNECTED = 'SERVICE_CONNECTED'
SERVICE_DISCONNECTED = 'SERVICE_DISCONNECTED'
SERVICE_EVENT_TYPES = frozenset({
	SERVICE_CONNECTED,
	SERVICE_DISCONNECTED,
})
SERVICE_EVENT_FILTERS = frozenset({
	'service_id',
	'service_type',
})
QPM_CATALOG_PROPERTIES = frozenset({
	'backend',
	'controller_target_id',
	'device_id',
	'hardware',
	'max_shots',
	'num_qubits',
	'provider',
	'qpm_capabilities',
	'qpm_type',
	'service_id',
	'service_type',
	'simulator',
	'target_id',
})


def _copy_record(record):
	copied = dict(record)
	copied['endpoint'] = dict(record.get('endpoint') or {})
	copied['selector'] = dict(record.get('selector') or {})
	copied['properties'] = dict(record.get('properties') or {})
	copied['capability'] = dict(record.get('capability') or {})
	copied['api_bindings'] = [
		dict(binding) for binding in record.get('api_bindings', [])
	]
	return copied


def _normalize_bindings(record):
	bindings = record.get('api_bindings') or []
	if not bindings:
		raise DEFwError("Directory registration missing api_bindings")
	return [dict(binding) for binding in bindings]


def _catalog_properties(record):
	properties = dict(record.get('properties') or {})
	if record.get('service_type') != QPM_SERVICE_TYPE:
		return properties
	unsupported = sorted(set(properties) - QPM_CATALOG_PROPERTIES)
	if unsupported:
		raise DEFwError(
			"QPM directory registration contains non-catalog properties: "
			f"{', '.join(unsupported)}")
	return properties


def _record_value(record, *names):
	for name in names:
		value = record.get(name)
		if value not in (-1, None):
			return value
	properties = record.get('properties') or {}
	for name in names:
		value = properties.get(name)
		if value not in (-1, None):
			return value
	return -1


def _bits_match(record_bits, requested_bits):
	if requested_bits in (-1, None):
		return True
	if record_bits in (-1, None):
		return False
	try:
		record_bits = int(record_bits)
		requested_bits = int(requested_bits)
	except (TypeError, ValueError):
		return False
	if requested_bits == 0:
		return True
	if record_bits == 0:
		return False
	return (requested_bits & record_bits) == requested_bits


def _local_runtime_id():
	try:
		import defw
		return str(defw.me.my_endpoint().get_id())
	except Exception:
		return ''


class Directory:
	def __init__(self, retention_seconds=DEFAULT_RETENTION_SECONDS,
		     runtime_id_provider=None):
		self.__records = {}
		self.__lock = threading.Lock()
		self.__transition_lock = threading.Lock()
		self.__retention_seconds = retention_seconds
		self.__lifecycle_listeners = []
		self.__event_registrations = {}
		self.__event_lock = threading.Lock()
		self.__runtime_id_provider = \
			runtime_id_provider or _local_runtime_id

	def add_lifecycle_listener(self, listener):
		if not callable(listener):
			raise DEFwError("Directory lifecycle listener is not callable")
		with self.__lock:
			if listener not in self.__lifecycle_listeners:
				self.__lifecycle_listeners.append(listener)
		return listener

	def remove_lifecycle_listener(self, listener):
		with self.__lock:
			if listener in self.__lifecycle_listeners:
				self.__lifecycle_listeners.remove(listener)

	def register_event_notification(self, callback, event_type,
					    subscriber_runtime_id, filters=None):
		if event_type not in SERVICE_EVENT_TYPES:
			raise DEFwError(
				f"Unsupported directory event type {event_type!r}")
		if not subscriber_runtime_id:
			raise DEFwError(
				"Directory event registration missing subscriber runtime ID")
		if callback is None or not callable(getattr(callback, 'put', None)):
			raise DEFwError("Directory event callback must expose put(event)")
		filters = dict(filters or {})
		unsupported = sorted(set(filters) - SERVICE_EVENT_FILTERS)
		if unsupported:
			raise DEFwError(
				"Unsupported directory event filters: "
				f"{', '.join(unsupported)}")
		registration_id = str(uuid.uuid4())
		registration = {
			'registration_id': registration_id,
			'event_type': event_type,
			'subscriber_runtime_id': str(subscriber_runtime_id),
			'callback': callback,
			'filters': filters,
		}
		with self.__event_lock:
			self.__event_registrations.setdefault(
				event_type, {})[registration_id] = registration
		return registration_id

	def unregister_event_notification(self, registration_id):
		with self.__event_lock:
			for registrations in self.__event_registrations.values():
				if registrations.pop(registration_id, None) is not None:
					return True
		return False

	def register_service(self, record, peer=None):
		with self.__transition_lock:
			now = time.time()
			service_id = record.get('service_id') or record.get('name')
			if not service_id:
				raise DEFwError("Directory registration missing service_id")

			peer = peer or {}
			runtime_id = record.get('runtime_id') or peer.get('runtime_id')
			peer_handle = record.get('peer_handle') or peer.get('peer_handle')
			if not runtime_id or not peer_handle:
				raise DEFwError(
					"Directory registration missing peer binding")

			previous_generation = None
			became_active = False
			with self.__lock:
				current = self.__records.get(service_id)
				if current and current['state'] == STATE_UP:
					if current['runtime_id'] != runtime_id:
						raise DEFwError(
							f"service_id {service_id} already has a live "
							"runtime")
					generation = current['generation']
				else:
					became_active = True
					if current:
						previous_generation = current['generation']
						generation = current['generation'] + 1
					else:
						generation = 1

				properties = _catalog_properties(record)
				qpm_type = _record_value(record, 'qpm_type')
				qpm_capabilities = _record_value(
					record, 'qpm_capabilities')
				if qpm_type != -1:
					properties.setdefault('qpm_type', qpm_type)
				if qpm_capabilities != -1:
					properties.setdefault(
						'qpm_capabilities', qpm_capabilities)
				registered = {
					'service_id': service_id,
					'service_name': record.get('service_name') or service_id,
					'service_type': record.get(
						'service_type', 'defw.service'),
					'runtime_id': runtime_id,
					'peer_handle': peer_handle,
					'generation': generation,
					'endpoint': dict(
						record.get('endpoint') or
						peer.get('endpoint') or {}),
					'api_bindings': _normalize_bindings(record),
					'selector': dict(record.get('selector') or {}),
					'properties': properties,
					'capability': dict(record.get('capability') or {}),
					'qpm_type': qpm_type,
					'qpm_capabilities': qpm_capabilities,
					'state': STATE_UP,
					'last_seen': now,
					'state_changed_at': now,
					'down_reason': '',
					'retention_deadline': None,
				}
				self.__records[service_id] = registered
				registered = _copy_record(registered)

			details = {}
			if previous_generation is not None:
				details['previous_generation'] = previous_generation
			self.__notify_lifecycle(
				'registration', service_record=registered,
				details=details)
			if became_active:
				self.__publish_service_event(
					SERVICE_CONNECTED, registered)
			return registered

	def deregister_service(self, service_id, runtime_id, generation):
		with self.__transition_lock:
			now = time.time()
			with self.__lock:
				record = self.__records.get(service_id)
				if not record:
					raise DEFwNotFound(
						f"service_id {service_id} not registered")
				if record['runtime_id'] != runtime_id or \
				   record['generation'] != generation:
					raise DEFwError("stale deregistration request")
				record['state'] = STATE_DEREGISTERED
				record['endpoint'] = {}
				record['state_changed_at'] = now
				record['retention_deadline'] = \
					now + self.__retention_seconds
				record = _copy_record(record)
			self.__notify_lifecycle(
				'deregistration', service_record=record)
			self.__publish_service_event(
				SERVICE_DISCONNECTED, record,
				reason='deregistered')
			return record

	def apply_peer_event(self, event):
		event_type = event.get('event_type')
		peer_handle = event.get('peer_handle')
		runtime_id = event.get('remote_runtime_id') or event.get('runtime_id')
		now = event.get('timestamp', time.time())
		notifications = []
		with self.__transition_lock:
			if event_type in ('PEER_LOST', 'PEER_REMOVED') and runtime_id:
				self.__remove_subscriber(runtime_id)
			with self.__lock:
				for record in self.__records.values():
					if record['peer_handle'] != peer_handle:
						continue
					if runtime_id and record['runtime_id'] != runtime_id:
						continue
					if now < record['last_seen']:
						continue
					record['last_seen'] = now
					if event_type in ('PEER_LOST', 'PEER_REMOVED'):
						reason = event.get('reason', '') or \
							'peer-removed'
						record['state'] = STATE_TIMED_OUT \
							if reason == 'heartbeat-timeout' else STATE_DOWN
						record['down_reason'] = reason
						record['state_changed_at'] = now
						record['retention_deadline'] = \
							now + self.__retention_seconds
						notifications.append((
							'peer-lost', _copy_record(record), reason))
					elif event_type == 'PEER_READY' and \
					     record['state'] in (STATE_DOWN, STATE_TIMED_OUT):
						record['state'] = STATE_UP
						record['down_reason'] = ''
						record['state_changed_at'] = now
						record['retention_deadline'] = None
						notifications.append((
							'peer-ready', _copy_record(record),
							event.get('reason', '')))
			for lifecycle_event, record, reason in notifications:
				self.__notify_lifecycle(
					lifecycle_event, service_record=record,
					peer_event=event, reason=reason)
				service_event = SERVICE_CONNECTED \
					if lifecycle_event == 'peer-ready' \
					else SERVICE_DISCONNECTED
				self.__publish_service_event(
					service_event, record, reason=reason)

	def resolve_services(self, **filters):
		self.purge_expired()
		with self.__lock:
			matches = []
			for record in self.__records.values():
				if record['state'] != STATE_UP:
					continue
				if not self.__record_matches(record, filters):
					continue
				for binding in record['api_bindings']:
					if self.__binding_matches(binding, filters):
						matches.append({
							'service_record': _copy_record(record),
							'selected_binding': dict(binding),
							'latest_generation': record['generation'],
						})
			return matches

	def query(self, include_inactive=False):
		self.purge_expired()
		with self.__lock:
			return [
				_copy_record(record)
				for record in self.__records.values()
				if include_inactive or record['state'] == STATE_UP
			]

	def purge_expired(self, now=None):
		now = now or time.time()
		purged = []
		with self.__lock:
			expired = [
				service_id
				for service_id, record in self.__records.items()
				if record['retention_deadline'] and
				record['retention_deadline'] <= now
			]
			for service_id in expired:
				purged.append(
					_copy_record(self.__records[service_id]))
				del self.__records[service_id]
		for record in purged:
			self.__notify_lifecycle(
				'retention-purge', service_record=record,
				details={'purged_at': now})

	def get_service_generation(self, service_id):
		self.purge_expired()
		with self.__lock:
			record = self.__records.get(service_id)
			if record is None:
				return None
			return record['generation']

	def __record_matches(self, record, filters):
		for field in ('service_id', 'service_name', 'service_type'):
			value = filters.get(field)
			if value and record.get(field) != value:
				return False
		if not _bits_match(
			_record_value(record, 'qpm_type'),
			filters.get('qpm_type', -1)
		):
			return False
		qpm_capabilities = filters.get('qpm_capabilities', -1)
		if not _bits_match(
			_record_value(record, 'qpm_capabilities'),
			qpm_capabilities
		):
			return False
		properties = filters.get('properties') or {}
		for key, value in properties.items():
			if record.get('properties', {}).get(key) != value:
				return False
		selector = record.get('selector') or {}
		selector_name = filters.get('selector_name')
		if selector_name and selector.get('name') != selector_name:
			return False
		selector_alias = filters.get('selector_alias')
		if selector_alias and selector_alias not in selector.get('aliases', []):
			return False
		selector_resource = filters.get('selector_resource')
		if selector_resource and \
		   selector_resource not in selector.get('resources', []):
			return False
		return True

	def __binding_matches(self, binding, filters):
		for src, dst in (
			('binding_name', 'binding_name'),
			('client_class', 'client_class'),
			('service_class', 'service_class'),
		):
			value = filters.get(src)
			if value and binding.get(dst) != value:
				return False
		return True

	def __notify_lifecycle(self, event_type, service_record=None,
			       peer_event=None, reason=None, details=None):
		with self.__lock:
			listeners = list(self.__lifecycle_listeners)
		for listener in listeners:
			try:
				listener(
					event_type,
					service_record=dict(service_record or {}),
					peer_event=dict(peer_event or {}),
					reason=reason,
					details=dict(details or {}))
			except Exception:
				logging.exception(
					"Directory lifecycle listener failed")

	def __remove_subscriber(self, runtime_id):
		with self.__event_lock:
			for event_type in list(self.__event_registrations):
				registrations = self.__event_registrations[event_type]
				for registration_id in list(registrations):
					registration = registrations[registration_id]
					if registration['subscriber_runtime_id'] == runtime_id:
						del registrations[registration_id]
				if not registrations:
					del self.__event_registrations[event_type]

	def __publish_service_event(self, event_type, record, reason=''):
		with self.__event_lock:
			registrations = [
				dict(registration)
				for registration in self.__event_registrations.get(
					event_type, {}).values()
				if self.__event_matches(
					registration.get('filters', {}), record)
			]
		if not registrations:
			return
		event = {
			'event': event_type,
			'directory_runtime_id': str(self.__runtime_id_provider()),
			'service_id': record['service_id'],
			'service_type': record['service_type'],
			'runtime_id': record['runtime_id'],
			'peer_handle': record['peer_handle'],
		}
		if event_type == SERVICE_CONNECTED:
			event['service_record'] = _copy_record(record)
		else:
			event['reason'] = reason or record.get('down_reason', '')
		for registration in registrations:
			try:
				registration['callback'].put(dict(event))
			except Exception:
				logging.exception(
					"Directory service lifecycle callback failed")

	def __event_matches(self, filters, record):
		return all(
			not value or record.get(field) == value
			for field, value in filters.items())


directory = Directory()


def register_service(record, peer=None):
	return directory.register_service(record, peer)


def add_lifecycle_listener(listener):
	return directory.add_lifecycle_listener(listener)


def remove_lifecycle_listener(listener):
	return directory.remove_lifecycle_listener(listener)


def deregister_service(service_id, runtime_id, generation):
	return directory.deregister_service(service_id, runtime_id, generation)


def apply_peer_event(event):
	return directory.apply_peer_event(event)


def resolve_services(**filters):
	return directory.resolve_services(**filters)


def query(include_inactive=False):
	return directory.query(include_inactive=include_inactive)


def purge_expired(now=None):
	return directory.purge_expired(now=now)


def get_service_generation(service_id):
	return directory.get_service_generation(service_id)


def register_event_notification(callback, event_type,
					subscriber_runtime_id, filters=None):
	return directory.register_event_notification(
		callback, event_type, subscriber_runtime_id, filters=filters)


def unregister_event_notification(registration_id):
	return directory.unregister_event_notification(registration_id)
