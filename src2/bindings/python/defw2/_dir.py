"""The directory from Python.

A client resolves services the way a C one does, through the same calls,
and gets records back as plain dictionaries:

	with defw2.Directory(rt) as directory:
		for record in directory.resolve(service_type='qfw.qpm'):
			qpm = defw2.QPM.from_record(rt, record)

A service registers through ServiceHost.register, which starts the C agent
that keeps the record alive with heartbeats and deregisters it on close.

A client that wants to hear of services coming and going subscribes a sink,
and takes DIR_SERVICE events from it. Each one's type is SERVICE_CONNECTED
or SERVICE_DISCONNECTED, as v1 named them, and its payload a dict of
connected, reason and the record:

	sink = defw2.EventSink(rt)
	directory.subscribe(sink, service_type='qfw.qpm')
	for event in sink:
		record = event.payload['record']
"""

from ._defw2 import ffi, lib
from ._event import EventKind, EventSink, EventTarget
from ._runtime import DefwError, _Kept, _status_out, _take_status, _text

__all__ = ['Directory', 'STATE', 'API_DIR', 'DIR_SERVICE',
	   'SERVICE_CONNECTED', 'SERVICE_DISCONNECTED']

API_DIR = 'qfw.directory'
SERVICE_CONNECTED = 'SERVICE_CONNECTED'
SERVICE_DISCONNECTED = 'SERVICE_DISCONNECTED'

STATE = {
	lib.DEFW2_DIR_STATE_UP: 'UP',
	lib.DEFW2_DIR_STATE_DOWN: 'DOWN',
	lib.DEFW2_DIR_STATE_TIMED_OUT: 'TIMED_OUT',
	lib.DEFW2_DIR_STATE_DEREGISTERED: 'DEREGISTERED',
}

_MATCH = {
	'equal': lib.DEFW2_DIR_MATCH_EQUAL,
	'bits_all': lib.DEFW2_DIR_MATCH_BITS_ALL,
	'bits_any': lib.DEFW2_DIR_MATCH_BITS_ANY,
}


def _strings(array, count):
	return [_text(array[i]) for i in range(count)]


def _binding(cdata):
	return {
		'binding_name': _text(cdata.binding_name),
		'api_id': _text(cdata.api_id),
		'api_version': cdata.api_version,
		'provider_id': cdata.provider_id,
	}


def record_from_entry(entry):
	"""A resolve result's entry as a dictionary, copied out of C."""
	values = _record(entry.record)
	values['binding'] = (_binding(entry.binding)
			     if entry.binding.api_id != ffi.NULL else None)
	return values


def _record(record):
	"""A record as a dictionary, copied out of C."""
	selector = record.selector
	return {
		'service_id': _text(record.service_id),
		'service_type': _text(record.service_type),
		'runtime_id': _text(record.runtime_id),
		'generation': record.generation,
		'state': STATE.get(record.state, 'UNKNOWN'),
		'address': _text(record.address),
		'endpoint': {
			'node_name': _text(record.endpoint.node_name),
			'hostname': _text(record.endpoint.hostname),
			'pid': record.endpoint.pid,
		},
		'bindings': [_binding(record.bindings[i])
			     for i in range(record.binding_count)],
		'selector': {
			'name': _text(selector.name),
			'aliases': _strings(selector.aliases,
					    selector.alias_count),
			'resources': _strings(selector.resources,
					      selector.resource_count),
		},
		'properties': {
			_text(record.properties[i].name):
				_text(record.properties[i].value)
			for i in range(record.property_count)
		},
		'registered_at_ns': record.registered_at_ns,
		'last_heartbeat_ns': record.last_heartbeat_ns,
		'retention_deadline_ns': record.retention_deadline_ns,
	}


def _read_change(event):
	"""A directory event's payload: which way the record went, why, and
	the record as the directory now holds it."""
	change = lib.defw2_dir_event_change(event)
	if change == ffi.NULL:
		return None
	return {'connected': bool(change.connected),
		'reason': _text(change.reason),
		'record': _record(change.record)}


# Only the directory sends these, so Python has nothing to send them with.
DIR_SERVICE = EventKind(API_DIR, 'service', lib.defw2_dir_event_accept,
			_read_change, None)


def build_record(kept, service_id, service_type, bindings, selector=None,
		 properties=None):
	"""A defw2_dir_record_t for a service to register.

	bindings is a list of (binding_name, api_id, api_version,
	provider_id). The address, endpoint and runtime identity are the
	runtime's, which the agent fills in.
	"""
	record = ffi.new('defw2_dir_record_t *')
	kept.kept.append(record)
	record.service_id = kept.str(service_id)
	record.service_type = kept.str(service_type)

	array = ffi.new('defw2_dir_binding_t[]', max(len(bindings), 1))
	kept.kept.append(array)
	for index, (name, api_id, version, provider) in enumerate(bindings):
		array[index].binding_name = kept.str(name)
		array[index].api_id = kept.str(api_id)
		array[index].api_version = version
		array[index].provider_id = provider
	record.bindings = array
	record.binding_count = len(bindings)

	selector = selector or {}
	record.selector.name = kept.str(selector.get('name'))
	record.selector.aliases, record.selector.alias_count = kept.strs(
		selector.get('aliases'))
	record.selector.resources, record.selector.resource_count = \
		kept.strs(selector.get('resources'))

	items = sorted((properties or {}).items())
	props = ffi.new('defw2_dir_property_t[]', max(len(items), 1))
	kept.kept.append(props)
	for index, (name, value) in enumerate(items):
		props[index].name = kept.str(name)
		props[index].value = kept.str(value)
	record.properties = props
	record.property_count = len(items)
	return record


class Directory:
	"""A client of the directory.

	address defaults to the runtime's own idea of where the directory
	is, which defw2_config_from_env took from DEFW2_DIRSVC.
	"""

	def __init__(self, runtime, address=None, timeout_ms=10000):
		self._runtime = runtime
		self._timeout_ms = timeout_ms
		address = address or runtime.dirsvc
		if not address:
			raise DefwError(lib.DEFW2_ERR_CONFIG, 'not-found',
					'this process was not told where the '
					'directory is')
		self.address = address
		out = ffi.new('defw2_dir_t **')
		rc = lib.defw2_dir_open(runtime.handle, address.encode(), out)
		if rc != lib.DEFW2_OK:
			raise DefwError(rc, 'transport',
					'opening the directory at ' + address)
		self._dir = out[0]

	def _options(self, kept):
		opts = ffi.new('defw2_call_opts_t *')
		opts.timeout_ms = self._timeout_ms
		kept.kept.append(opts)
		return opts

	def _query(self, kept, service_id, service_type, selector_name,
		   resource, binding_name, api_version, filters, limit):
		query = ffi.new('defw2_dir_query_t *')
		kept.kept.append(query)
		query.service_id = kept.str(service_id)
		query.service_type = kept.str(service_type)
		query.selector_name = kept.str(selector_name)
		query.resource = kept.str(resource)
		query.binding_name = kept.str(binding_name)
		query.api_version = api_version or 0
		query.limit = limit or 0
		filters = list(filters or ())
		if filters:
			array = ffi.new('defw2_dir_filter_t[]', len(filters))
			kept.kept.append(array)
			for index, item in enumerate(filters):
				name, value = item[0], item[1]
				match = item[2] if len(item) > 2 else 'equal'
				array[index].name = kept.str(name)
				array[index].value = kept.str(value)
				array[index].match = _MATCH[match]
			query.filters = array
			query.filter_count = len(filters)
		return query

	def _records(self, call, query, kept):
		result = ffi.new('defw2_dir_result_t *')
		holder = _status_out()
		rc = call(self._dir, query, self._options(kept), result, holder)
		status = _take_status(holder)
		try:
			if rc != lib.DEFW2_OK:
				raise DefwError(rc, 'transport',
						'the directory did not answer')
			status.raise_for_status()
			return [record_from_entry(result.entries[i])
				for i in range(result.entry_count)]
		finally:
			lib.defw2_dir_result_free(result)

	def resolve(self, service_type=None, service_id=None,
		    selector_name=None, resource=None, binding_name=None,
		    api_version=0, filters=None, limit=0):
		"""UP records that match, each with its selected binding.

		filters are (name, value) or (name, value, match) where match
		is 'equal', 'bits_all' or 'bits_any'. An empty list is an
		answer, not an error: nothing serves that yet.
		"""
		kept = _Kept()
		query = self._query(kept, service_id, service_type,
				    selector_name, resource, binding_name,
				    api_version, filters, limit)
		return self._records(lib.defw2_dir_resolve, query, kept)

	def query(self, service_type=None, service_id=None,
		  selector_name=None, resource=None, binding_name=None,
		  api_version=0, filters=None, limit=0):
		"""Every matching record, including ones that are not UP."""
		kept = _Kept()
		query = self._query(kept, service_id, service_type,
				    selector_name, resource, binding_name,
				    api_version, filters, limit)
		return self._records(lib.defw2_dir_query, query, kept)

	def subscribe(self, target, service_id=None, service_type=None,
		      connected=True, disconnected=True, tag=None):
		"""Have the directory send target the records that come and go,
		and return the subscription's id.

		target is an EventSink of this process, which this readies for
		directory events first, or an EventTarget naming any sink.
		service_id and service_type narrow it to the records that match
		exactly. connected and disconnected say which changes it hears
		of. It lasts until unsubscribe, or until a delivery to the sink
		fails.
		"""
		changes = ((lib.DEFW2_DIR_CONNECTED if connected else 0) |
			   (lib.DEFW2_DIR_DISCONNECTED if disconnected else 0))
		if changes == 0:
			raise ValueError('a subscription has to hear of '
					 'something')
		if isinstance(target, EventSink):
			target.accept(DIR_SERVICE)
			target = target.target(tag)
		elif tag is not None:
			target = EventTarget(target[0], target[1], tag)
		address, provider_id, tag = target
		kept = _Kept()
		req = ffi.new('defw2_dir_subscribe_req_t *')
		kept.kept.append(req)
		req.target.address = kept.str(address)
		req.target.provider_id = provider_id
		req.target.tag = kept.str(tag)
		req.service_id = kept.str(service_id)
		req.service_type = kept.str(service_type)
		req.changes = changes
		out = ffi.new('uint64_t *')
		holder = _status_out()
		opts = self._options(kept)
		rc = lib.defw2_dir_subscribe(self._dir, req, opts, out, holder)
		status = _take_status(holder)
		if rc != lib.DEFW2_OK:
			raise DefwError(rc, 'transport',
					'the directory did not answer')
		status.raise_for_status()
		return out[0]

	def unsubscribe(self, subscription_id):
		"""End a subscription. False when the directory held no such
		one, which is also what a subscription whose sink went
		becomes."""
		kept = _Kept()
		holder = _status_out()
		rc = lib.defw2_dir_unsubscribe(self._dir, subscription_id,
					       self._options(kept), holder)
		status = _take_status(holder)
		if rc != lib.DEFW2_OK:
			raise DefwError(rc, 'transport',
					'the directory did not answer')
		if status.category == 'not-found':
			return False
		status.raise_for_status()
		return True

	def generation(self, service_id):
		kept = _Kept()
		out = ffi.new('uint64_t *')
		holder = _status_out()
		rc = lib.defw2_dir_generation(self._dir, kept.str(service_id),
					      self._options(kept), out, holder)
		status = _take_status(holder)
		if rc != lib.DEFW2_OK:
			raise DefwError(rc, 'transport',
					'the directory did not answer')
		status.raise_for_status()
		return out[0]

	def close(self):
		if self._dir is not None:
			lib.defw2_dir_close(self._dir)
			self._dir = None

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False
