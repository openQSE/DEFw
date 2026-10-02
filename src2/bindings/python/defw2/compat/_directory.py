"""v1's directory, defw.dirsvc, over the v2 directory.

A v1 service registers with register_service(endpoint, context) and a v1
client finds it with resolve_services(**filters), which answers with v1
records: a service_record, the selected_binding and latest_generation. The
v2 directory keeps typed records, so registering here keeps two things in
the v2 record. Its typed fields, the selector and the properties as
strings, are what a v2 client resolves by. And under the property
v1_record it keeps the v1 record's own fields as JSON, which resolve
puts back together with the v2 record's identity, generation, state and
address into the record v1's directory would have answered with. The
matching is v1's: the same filters, the same rules.

A v2 record that a v2 service registered has no v1_record, and still
resolves, as the v1 record its typed fields describe.

v1's directory events, SERVICE_CONNECTED and SERVICE_DISCONNECTED, need
events, which v2 does not have until phase 3. Registering for them is
accepted, and nothing is delivered: a client notices a restarted service
when it next resolves, not before.
"""

import copy
import json
import logging
import re
import uuid

from .._dir import Directory as V2Directory
from .._runtime import DefwError
from ._mapping import STR_MAX

log = logging.getLogger('defw2.compat')

V1_RECORD = 'v1_record'
SERVICE_EVENT_TYPES = ('SERVICE_CONNECTED', 'SERVICE_DISCONNECTED')
SERVICE_EVENT_FILTERS = ('service_id', 'service_type')

# The v1 record's own fields. The rest, identity and state, come from v2.
_STATIC = ('service_id', 'service_name', 'service_type', 'api_bindings',
	   'selector', 'properties', 'capability', 'qpm_type',
	   'qpm_capabilities')


def _v1_error(message):
	import defw_exception
	return defw_exception.DEFwError(message)


def _record_value(record, name):
	"""v1's _record_value: the record's value, else its property's."""
	value = record.get(name)
	if value not in (-1, None):
		return value
	value = (record.get('properties') or {}).get(name)
	if value not in (-1, None):
		return value
	return -1


def _bits_match(record_bits, requested_bits):
	"""v1's _bits_match, rule for rule."""
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


def record_matches(record, filters):
	"""v1's Directory.__record_matches."""
	for field in ('service_id', 'service_name', 'service_type'):
		value = filters.get(field)
		if value and record.get(field) != value:
			return False
	if not _bits_match(_record_value(record, 'qpm_type'),
			   filters.get('qpm_type', -1)):
		return False
	if not _bits_match(_record_value(record, 'qpm_capabilities'),
			   filters.get('qpm_capabilities', -1)):
		return False
	for key, value in (filters.get('properties') or {}).items():
		if record.get('properties', {}).get(key) != value:
			return False
	selector = record.get('selector') or {}
	if filters.get('selector_name') and \
	   selector.get('name') != filters['selector_name']:
		return False
	if filters.get('selector_alias') and \
	   filters['selector_alias'] not in selector.get('aliases', []):
		return False
	if filters.get('selector_resource') and \
	   filters['selector_resource'] not in selector.get('resources', []):
		return False
	return True


def binding_matches(binding, filters):
	"""v1's Directory.__binding_matches."""
	for name in ('binding_name', 'client_class', 'service_class'):
		value = filters.get(name)
		if value and binding.get(name) != value:
			return False
	return True


def v1_static(endpoint, advertisement, context):
	"""The fields v1's directory service built a record from, as its
	__directory_record did from a service's advertisement and the
	context it registered with."""
	if not isinstance(advertisement, dict):
		raise _v1_error('Service query must return metadata dictionaries')
	context = context if isinstance(context, dict) else {}
	service_name = (context.get('service_name') or
			advertisement.get('service_name'))
	if not service_name:
		raise _v1_error('Service metadata missing service_name')
	properties = dict(context.get('properties') or {})
	properties.update(advertisement.get('properties') or {})
	capability = dict(context.get('capability') or
			  advertisement.get('capability') or {})
	qpm_type = context.get('qpm_type', advertisement.get(
		'qpm_type', properties.get('qpm_type', -1)))
	qpm_capabilities = context.get('qpm_capabilities', advertisement.get(
		'qpm_capabilities', properties.get('qpm_capabilities', -1)))
	if qpm_type != -1:
		properties.setdefault('qpm_type', qpm_type)
	if qpm_capabilities != -1:
		properties.setdefault('qpm_capabilities', qpm_capabilities)
	service_id = (context.get('service_id') or
		      advertisement.get('service_id') or
		      properties.get('service_id') or
		      '{}:{}:{}'.format(service_name, endpoint.hostname,
					endpoint.name))
	selector = (context.get('selector') or
		    advertisement.get('selector') or
		    properties.get('selector') or
		    {'resources': [service_name]})
	service_type = context.get('service_type')
	if service_type is None:
		service_type = advertisement.get(
			'service_type', properties.get('service_type',
						       'defw.service'))
	api_bindings = (context.get('api_bindings') or
			advertisement.get('api_bindings'))
	if not api_bindings:
		raise _v1_error('Service metadata missing api_bindings')
	return {
		'service_id': service_id,
		'service_name': service_name,
		'service_type': service_type,
		'api_bindings': [dict(binding) for binding in api_bindings],
		'selector': selector,
		'properties': properties,
		'capability': capability,
		'qpm_type': qpm_type,
		'qpm_capabilities': qpm_capabilities,
	}


def _property_text(value):
	"""A v1 property as the string a v2 property is."""
	if isinstance(value, str):
		return value
	try:
		return json.dumps(value, separators=(',', ':'), sort_keys=True)
	except (TypeError, ValueError):
		return str(value)


def _selector_strings(values):
	if values is None:
		return []
	if isinstance(values, (list, tuple, set)):
		return [str(v) for v in values if v is not None]
	return [str(values)]


def v2_registration(static):
	"""(selector, properties) of the v2 record for a v1 one."""
	selector = static.get('selector') or {}
	v2_selector = {
		'name': selector.get('name') or static['service_name'],
		'aliases': _selector_strings(selector.get('aliases')),
		'resources': _selector_strings(selector.get('resources')),
	}
	properties = {name: _property_text(value)
		      for name, value in static['properties'].items()}
	properties[V1_RECORD] = json.dumps(static, separators=(',', ':'),
					   sort_keys=True)
	for name, value in properties.items():
		if len(value.encode('utf-8')) >= STR_MAX:
			raise _v1_error(
				'defw2.compat: the {} property of {} is {} bytes, and a '
				'v2 property holds less than {}'.format(
					name, static['service_id'], len(value), STR_MAX))
	return v2_selector, properties


def _endpoint_record(v2):
	"""The v1 endpoint block of a v2 record, with listen_port as compat's
	Endpoint gives it: a TCP address's port, or else the process ID."""
	endpoint = v2.get('endpoint') or {}
	address = v2.get('address') or ''
	match = re.search(r':(\d+)$', address)
	listen_port = int(match.group(1)) if match else endpoint.get('pid')
	return {
		'address': address,
		'listen_port': listen_port,
		'node_name': endpoint.get('node_name'),
		'hostname': endpoint.get('hostname'),
		'pid': endpoint.get('pid'),
	}


def v1_record(v2):
	"""The v1 record a v2 record stands for."""
	text = (v2.get('properties') or {}).get(V1_RECORD)
	if text is not None:
		static = json.loads(text)
	else:
		static = _static_from_v2(v2)
	record = dict(static)
	record.update({
		'runtime_id': v2['runtime_id'],
		'peer_handle': v2['runtime_id'],
		'generation': v2['generation'],
		'endpoint': (_endpoint_record(v2) if v2.get('address')
			     else {}),
		'state': v2['state'],
		'last_seen': v2['last_heartbeat_ns'] / 1e9,
		'state_changed_at': v2['registered_at_ns'] / 1e9,
		'down_reason': '',
		'retention_deadline': (v2['retention_deadline_ns'] / 1e9
				       if v2['retention_deadline_ns'] else None),
	})
	return record


def _static_from_v2(v2):
	"""A v1 record's fields for a service v2 registered."""
	properties = dict(v2.get('properties') or {})
	for name in ('qpm_type', 'qpm_capabilities'):
		if str(properties.get(name, '')).lstrip('-').isdigit():
			properties[name] = int(properties[name])
	selector = v2.get('selector') or {}
	return {
		'service_id': v2['service_id'],
		'service_name': v2['service_id'],
		'service_type': v2['service_type'],
		'api_bindings': [{'binding_name': b['binding_name'],
				  'version': b['api_version']}
				 for b in v2.get('bindings', [])],
		'selector': {'name': selector.get('name'),
			     'aliases': list(selector.get('aliases') or []),
			     'resources': list(selector.get('resources') or [])},
		'properties': properties,
		'capability': {},
		'qpm_type': properties.get('qpm_type', -1),
		'qpm_capabilities': properties.get('qpm_capabilities', -1),
	}


class Directory:
	"""defw.dirsvc on v2."""

	def __init__(self, runtime, address):
		self._runtime = runtime
		self.address = address
		self._v2 = V2Directory(runtime, address)
		self._events = {}
		self._told = False
		self._registered = None
		# v2 records by (service_id, runtime_id), so a connect can use
		# the bindings the record it came from declared.
		self.seen = {}

	def close(self):
		self._v2.close()

	# --- registration, for a process that serves

	def register_service(self, service_ep, context=None):
		"""Register what this process serves, as v1's register_service
		did: with the advertisement each service class gives from
		query(), and the context the caller sent."""
		import defw
		from . import _state
		host = _state.host
		if host is None:
			raise _v1_error('defw2.compat: this process serves '
					'nothing, so it has nothing to register')
		advertisements = []
		for _name, module in defw.services:
			for cls in getattr(module, 'service_classes', []):
				advertisements.append(cls(start=False).query())
		if len(advertisements) != 1:
			raise _v1_error(
				'defw2.compat: a v2 host registers one record, '
				'and this process advertises {}'.format(
					len(advertisements)))
		static = v1_static(_state.endpoint(), advertisements[0],
				   context)
		selector, properties = v2_registration(static)
		# QFw registers again whenever it thinks the directory forgot
		# it. The v2 agent already re-registers on its own, so the same
		# registration again is answered from what is there.
		with _state.lock:
			if self._registered is None:
				host.register(self.address, selector=selector,
					      properties=properties,
					      service_id=static['service_id'])
				self._registered = static
			elif self._registered != static:
				raise _v1_error(
					'defw2.compat: this process already '
					'registered {}, differently'.format(
						self._registered['service_id']))
		records = self._resolve(service_id=static['service_id'],
					service_type=static['service_type'])
		return [v1_record(v2) for v2 in records
			if v2['runtime_id'] == self._runtime.runtime_id]

	# --- resolution, for a client

	def _resolve(self, service_id=None, service_type=None):
		records = self._v2.resolve(service_type=service_type or None,
					   service_id=service_id or None)
		for v2 in records:
			self.seen[(v2['service_id'], v2['runtime_id'])] = v2
		return records

	def resolve_services(self, **filters):
		matches = []
		for v2 in self._resolve(filters.get('service_id'),
					filters.get('service_type')):
			record = v1_record(v2)
			if not record_matches(record, filters):
				continue
			for binding in record['api_bindings']:
				if binding_matches(binding, filters):
					matches.append({
						'service_record':
							copy.deepcopy(record),
						'selected_binding': dict(binding),
						'latest_generation':
							record['generation'],
					})
		return matches

	def get_service_generation(self, service_id):
		try:
			generation = self._v2.generation(service_id)
		except DefwError as error:
			if error.category == 'not-found':
				return None
			raise
		return generation or None

	# --- directory events, which v2 does not deliver yet

	def register_event_notification(self, endpoint, event_type, class_id,
					filters=None):
		if event_type not in SERVICE_EVENT_TYPES:
			raise _v1_error('Unsupported directory event type '
					'{!r}'.format(event_type))
		unsupported = sorted(set(filters or {}) -
				     set(SERVICE_EVENT_FILTERS))
		if unsupported:
			raise _v1_error('Unsupported directory event filters: '
					'{}'.format(', '.join(unsupported)))
		if not self._told:
			self._told = True
			log.warning('directory events are accepted and not '
				    'delivered: v2 has no events until phase 3')
		registration_id = str(uuid.uuid4())
		self._events[registration_id] = (event_type, class_id,
						 dict(filters or {}))
		return registration_id

	def unregister_event_notification(self, registration_id):
		return self._events.pop(registration_id, None) is not None

	def __getattr__(self, name):
		raise AttributeError(
			"defw2.compat's dirsvc has no {}: v1 directory calls "
			'other than register_service, resolve_services, '
			'get_service_generation and the event registrations '
			'are not on v2'.format(name))
