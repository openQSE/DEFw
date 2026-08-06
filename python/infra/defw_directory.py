import logging
import threading
import time

from defw_exception import DEFwError, DEFwNotFound


STATE_UP = 'UP'
STATE_DOWN = 'DOWN'
STATE_TIMED_OUT = 'TIMED_OUT'
STATE_DEREGISTERED = 'DEREGISTERED'
DEFAULT_RETENTION_SECONDS = 300


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


def _default_binding(record):
	service_name = record.get('service_name') or record.get('name')
	client_module = record.get('client_module') or record.get('module')
	client_class = record.get('client_class') or service_name
	service_module = record.get('service_module') or record.get('module')
	service_class = record.get('service_class') or record.get('class_name')
	return {
		'binding_name': record.get('binding_name', 'default'),
		'client_module': client_module,
		'client_class': client_class,
		'service_module': service_module,
		'service_class': service_class,
		'version': record.get('version', 1),
	}


def _normalize_bindings(record):
	bindings = record.get('api_bindings') or []
	if not bindings:
		bindings = [_default_binding(record)]
	return [dict(binding) for binding in bindings]


class Directory:
	def __init__(self, retention_seconds=DEFAULT_RETENTION_SECONDS):
		self.__records = {}
		self.__lock = threading.Lock()
		self.__retention_seconds = retention_seconds
		self.__lifecycle_listeners = []

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

	def register_service(self, record, peer=None):
		now = time.time()
		service_id = record.get('service_id') or record.get('name')
		if not service_id:
			raise DEFwError("Directory registration missing service_id")

		peer = peer or {}
		runtime_id = record.get('runtime_id') or peer.get('runtime_id')
		peer_handle = record.get('peer_handle') or peer.get('peer_handle')
		if not runtime_id or not peer_handle:
			raise DEFwError("Directory registration missing peer binding")

		previous_generation = None
		with self.__lock:
			current = self.__records.get(service_id)
			if current and current['state'] == STATE_UP:
				if current['runtime_id'] != runtime_id:
					raise DEFwError(
						f"service_id {service_id} already has a live runtime"
					)
				generation = current['generation']
			elif current:
				previous_generation = current['generation']
				generation = current['generation'] + 1
			else:
				generation = 1

			registered = {
				'service_id': service_id,
				'service_name': record.get('service_name') or service_id,
				'service_type': record.get('service_type', 'defw.service'),
				'runtime_id': runtime_id,
				'peer_handle': peer_handle,
				'generation': generation,
					'endpoint': dict(record.get('endpoint') or peer.get('endpoint') or {}),
					'api_bindings': _normalize_bindings(record),
					'selector': dict(record.get('selector') or {}),
					'properties': dict(record.get('properties') or {}),
					'capability': dict(record.get('capability') or {}),
					'legacy_type': record.get('legacy_type', -1),
					'legacy_capabilities': record.get(
						'legacy_capabilities', -1
					),
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
		return registered

	def deregister_service(self, service_id, runtime_id, generation):
		now = time.time()
		with self.__lock:
			record = self.__records.get(service_id)
			if not record:
				raise DEFwNotFound(f"service_id {service_id} not registered")
			if record['runtime_id'] != runtime_id or \
			   record['generation'] != generation:
				raise DEFwError("stale deregistration request")
			record['state'] = STATE_DEREGISTERED
			record['endpoint'] = {}
			record['state_changed_at'] = now
			record['retention_deadline'] = now + self.__retention_seconds
			record = _copy_record(record)
		self.__notify_lifecycle('deregistration', service_record=record)
		return record

	def apply_peer_event(self, event):
		event_type = event.get('event_type')
		peer_handle = event.get('peer_handle')
		runtime_id = event.get('remote_runtime_id') or event.get('runtime_id')
		now = event.get('timestamp', time.time())
		notifications = []
		with self.__lock:
			for record in self.__records.values():
				if record['peer_handle'] != peer_handle:
					continue
				if runtime_id and record['runtime_id'] != runtime_id:
					continue
				if now < record['last_seen']:
					continue
				record['last_seen'] = now
				if event_type == 'PEER_LOST':
					reason = event.get('reason', '')
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
				service_id for service_id, record in self.__records.items()
				if record['retention_deadline'] and
				record['retention_deadline'] <= now
			]
			for service_id in expired:
				purged.append(_copy_record(self.__records[service_id]))
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

	def get_generation(self, service_id):
		return self.get_service_generation(service_id)

	def __record_matches(self, record, filters):
		for field in ('service_id', 'service_name', 'service_type'):
			value = filters.get(field)
			if value and record.get(field) != value:
				return False
		svc_type = filters.get('svc_type',
				       filters.get('legacy_type',
						   filters.get('qpm_type', -1)))
		svc_caps = filters.get('svc_caps',
				       filters.get('legacy_capabilities',
						   filters.get('qpm_capability',
							       filters.get('qpm_cap',
									   -1))))
		if not self.__legacy_bits_match(
			record.get('legacy_type', -1), svc_type
		):
			return False
		if not self.__legacy_bits_match(
			record.get('legacy_capabilities', -1), svc_caps
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

	def __legacy_bits_match(self, record_bits, requested_bits):
		if requested_bits in (-1, None):
			return True
		if record_bits in (-1, None):
			return False
		return (requested_bits & record_bits) == requested_bits

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


def get_generation(service_id):
	return directory.get_generation(service_id)
