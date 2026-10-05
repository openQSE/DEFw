"""The runtime, and the pieces every other module needs.

The binding contains no networking, no encoding and no lifecycle logic of
its own. It exposes the C API idiomatically and keeps the interpreter away
from Margo threads. cffi releases the interpreter lock around every C call,
so a blocking call here does not stall other Python threads.
"""

from ._defw2 import ffi, lib

__all__ = [
	'Runtime', 'Status', 'DefwError', 'ServiceError', 'CATEGORY',
	'CATEGORY_CODE', 'version', 'process_stats',
]

# The wire-visible outcome categories, as names a caller can branch on.
CATEGORY = {
	lib.DEFW2_CAT_OK: 'ok',
	lib.DEFW2_CAT_TRANSPORT: 'transport',
	lib.DEFW2_CAT_TIMEOUT: 'timeout',
	lib.DEFW2_CAT_CANCELLED: 'cancelled',
	lib.DEFW2_CAT_NOT_FOUND: 'not-found',
	lib.DEFW2_CAT_VERSION_MISMATCH: 'version-mismatch',
	lib.DEFW2_CAT_INVALID_ARGUMENT: 'invalid-argument',
	lib.DEFW2_CAT_INVALID_RESERVATION: 'invalid-reservation',
	lib.DEFW2_CAT_INSUFFICIENT_ALLOWANCE: 'insufficient-allowance',
	lib.DEFW2_CAT_PENDING_CAPACITY: 'pending-capacity',
	lib.DEFW2_CAT_POLICY_DELAYED: 'policy-delayed',
	lib.DEFW2_CAT_EXPIRED_RESERVATION: 'expired-reservation',
	lib.DEFW2_CAT_SCHEDULER_FAILURE: 'scheduler-failure',
	lib.DEFW2_CAT_PROVIDER_FAILURE: 'provider-failure',
}


# The same categories by name, for a service that answers with one.
CATEGORY_CODE = {name: code for code, name in CATEGORY.items()}


def version():
	return _text(lib.defw2_version())


def _text(pointer):
	"""A C string as str, or None."""
	if pointer == ffi.NULL:
		return None
	return ffi.string(pointer).decode('utf-8', 'replace')


class DefwError(Exception):
	"""A call that did not reach the service and come back.

	v1's exception classes map onto the status categories, so a caller
	branches on .category rather than on a class hierarchy.
	"""

	def __init__(self, code, category=None, message=None):
		self.code = int(code)
		self.category = category or CATEGORY.get(lib.DEFW2_CAT_OK)
		self.message = message or _text(lib.defw2_strerror(code))
		super().__init__('{} ({})'.format(self.message, self.category))


class ServiceError(DefwError):
	"""What a Python service raises to fail a call with a category.

	A service that raises anything else fails the call as a provider
	failure. This one says which category the caller should see, such as
	'invalid-reservation', and the message travels with it.
	"""

	def __init__(self, category, message=None, code=None):
		if category not in CATEGORY_CODE:
			raise ValueError('unknown category {!r}'.format(category))
		super().__init__(lib.DEFW2_ERR_INTERNAL if code is None else code,
				 category, message)


class Status:
	"""What a service made of a call, as opposed to whether it arrived."""

	__slots__ = ('code', 'category', 'message')

	def __init__(self, code, category, message):
		self.code = code
		self.category = category
		self.message = message

	@property
	def ok(self):
		return self.category == 'ok'

	def raise_for_status(self):
		if not self.ok:
			raise DefwError(self.code, self.category, self.message)

	def __repr__(self):
		return 'Status({}, {!r})'.format(self.category, self.message)


def _status_out():
	"""A defw2_status_t to hand to C, freed when Python drops it."""
	return ffi.new('defw2_status_t *')


def _take_status(holder):
	"""Read a filled status and release what C allocated inside it.

	Everything is read first, because defw2_status_free clears the
	struct as well as freeing the message.
	"""
	code = int(holder.code)
	category = CATEGORY.get(holder.category, 'unknown')
	message = _text(holder.message)
	lib.defw2_status_free(holder)
	return Status(code, category, message)


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


def _check(rc, what):
	if rc != lib.DEFW2_OK:
		raise DefwError(rc, 'transport', '{}: {}'.format(
			what, _text(lib.defw2_strerror(rc))))


def process_stats():
	"""CPU time and peak memory, the same numbers the report carries."""
	stats = ffi.new('defw2_process_stats_t *')
	lib.defw2_process_stats(stats)
	return {
		'user_us': stats.user_us,
		'system_us': stats.system_us,
		'peak_rss_kib': stats.peak_rss_kib,
	}


class Runtime:
	"""One process's membership of a v2 deployment.

	Use it as a context manager, or call close(). Everything else in the
	binding takes one of these.
	"""

	def __init__(self, role='client', address=None, node_name=None,
		     profile=None, rpc_threads=None, progress_spindown_ms=None):
		"""progress_spindown_ms is how long Margo's progress loop spins
		after it has handled something. By default a server does not
		spin and a client keeps Margo's own default, and
		DEFW2_PROGRESS_SPINDOWN_MS changes that for both."""
		config = ffi.new('defw2_config_t *')
		_check(lib.defw2_config_from_env(config), 'reading the environment')

		# Keep every string alive for as long as the config points at
		# it, which is until defw2_init has copied what it needs.
		self._kept = []
		config.role = (lib.DEFW2_ROLE_SERVER if role == 'server'
			       else lib.DEFW2_ROLE_CLIENT)
		if address is not None:
			config.address = self._keep(address)
		if node_name is not None:
			config.node_name = self._keep(node_name)
		if profile is not None:
			config.profile = bool(profile)
		if rpc_threads is not None:
			config.rpc_thread_count = int(rpc_threads)
		if progress_spindown_ms is not None:
			spindown = int(progress_spindown_ms)
			if spindown < 0:
				raise ValueError('a spindown is 0 ms or more, '
						 'not {}'.format(spindown))
			config.has_progress_spindown = True
			config.progress_spindown_ms = spindown

		out = ffi.new('defw2_rt_t **')
		_check(lib.defw2_init(config, out), 'starting the runtime')
		self._rt = out[0]
		# init has copied everything it keeps, so the strings can go.
		self._kept = []

	def _keep(self, text):
		held = ffi.new('char[]', text.encode('utf-8'))
		self._kept.append(held)
		return held

	@property
	def handle(self):
		if self._rt is None:
			raise RuntimeError('this runtime has been closed')
		return self._rt

	@property
	def closed(self):
		return self._rt is None

	@property
	def runtime_id(self):
		return _text(lib.defw2_runtime_id(self.handle))

	@property
	def address(self):
		return _text(lib.defw2_address(self.handle))

	@property
	def node_name(self):
		return _text(lib.defw2_node_name(self.handle))

	@property
	def hostname(self):
		return _text(lib.defw2_hostname(self.handle))

	@property
	def dirsvc(self):
		"""Where the directory is, or None when this process has none."""
		return _text(lib.defw2_dirsvc(self.handle))

	@property
	def profiling(self):
		return bool(lib.defw2_profiling(self.handle))

	@property
	def trace_id(self):
		return _text(lib.defw2_trace_id(self.handle))

	def resource(self, key, value):
		"""Describe this process in the telemetry it writes."""
		lib.defw2_telemetry_resource(self.handle, key.encode(),
					     str(value).encode())

	def run_span(self, name, **attributes):
		"""A benchmark's qfw.bench.run, as a context manager."""
		return _RunSpan(self, name, attributes)

	def flush_telemetry(self):
		lib.defw2_telemetry_flush(self.handle)

	def close(self):
		if self._rt is not None:
			lib.defw2_finalize(self._rt)
			self._rt = None

	def __enter__(self):
		return self

	def __exit__(self, *exc):
		self.close()
		return False


class _RunSpan:
	def __init__(self, runtime, name, attributes):
		self._runtime = runtime
		self._name = name
		self._attributes = attributes

	def __enter__(self):
		handle = self._runtime.handle
		lib.defw2_telemetry_run_begin(handle, self._name.encode())
		for key, value in self._attributes.items():
			key = key.encode()
			if isinstance(value, int) and not isinstance(value, bool):
				lib.defw2_telemetry_run_attr_int(handle, key,
								 value)
			else:
				lib.defw2_telemetry_run_attr(handle, key,
							     str(value).encode())
		return self

	def __exit__(self, kind, value, traceback):
		error = ffi.NULL
		if value is not None:
			error = ffi.new('char[]', str(value).encode())
		lib.defw2_telemetry_run_end(self._runtime.handle, error)
		return False
