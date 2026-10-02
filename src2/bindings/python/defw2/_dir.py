"""The directory from Python.

A client resolves services the way a C one does, through the same calls,
and gets records back as plain dictionaries:

	with defw2.Directory(rt) as directory:
		for record in directory.resolve(service_type='qfw.qpm'):
			qpm = defw2.QPM.from_record(rt, record)

A service registers through ServiceHost.register, which starts the C agent
that keeps the record alive with heartbeats and deregisters it on close.
"""

from ._defw2 import ffi, lib
from ._runtime import DefwError, _status_out, _take_status, _text

__all__ = ['Directory', 'STATE']

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
	record = entry.record
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
		'binding': (_binding(entry.binding)
			    if entry.binding.api_id != ffi.NULL else None),
	}


class _Kept:
	"""C strings and arrays a call borrows, alive until it returns."""

	def __init__(self):
		self.kept = []

	def str(self, value):
		if value is None:
			return ffi.NULL
		held = ffi.new('char[]', str(value).encode('utf-8'))
		self.kept.append(held)
		return held

	def strs(self, values):
		values = list(values or ())
		array = ffi.new('const char *[]', max(len(values), 1))
		for index, value in enumerate(values):
			array[index] = self.str(value)
		self.kept.append(array)
		return array, len(values)


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
