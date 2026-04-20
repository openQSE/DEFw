import cdefw_global
from defw_exception import DEFwError, DEFwDumper, DEFwNotFound
import logging, os, yaml, shutil, threading, time, sys
import cdefw_global
from pathlib import Path
from collections import deque

FILE_HANDLER = None
CUSTOM_LEVELS = {}
CUSTOM_LEVEL_NAMES = set()
CUSTOM_LEVEL_GROUPS = {}

# DEFAULT LOG LEVELS
DEFW_LOG_LEVEL_CORE =			30
DEFW_LOG_LEVEL_WORKER =			31
DEFW_LOG_LEVEL_SERVICE =		32
DEFW_LOG_LEVEL_APP =			33
DEFW_LOG_LEVEL_RPC =			34
DEFW_LOG_LEVEL_STACKTRACE =		35

DEFW_LOG_LEVEL_CORE_NAME =				"DEFW_CORE"
DEFW_LOG_LEVEL_WORKER_NAME =			"DEFW_WORKER"
DEFW_LOG_LEVEL_SERVICE_NAME =			"DEFW_SERVICE"
DEFW_LOG_LEVEL_APP_NAME =				"DEFW_APP"
DEFW_LOG_LEVEL_RPC_NAME =				"DEFW_RPC"
DEFW_LOG_LEVEL_STACKTRACE_NAME =		"DEFW_STACKTRACE"

DEFW_STATUS_STRING = 'DEFw STATUS: '
DEFW_STATUS_SUCCESS = 'Success'
DEFW_STATUS_FAILURE = 'Failure'
DEFW_STATUS_IGNORE = 'Ignore'
DEFW_CODE_STRING = 'DEFw CODE: '
MASTER_PORT = 8494
MASTER_DAEMON_PORT = 8495
AGENT_DAEMON_PORT = 8094
DEFW_SCRIPT_PATHS = ['src/',
		     'python/',
		     'python/service-apis',
		     'python/service-apis/util',
		     'python/services',
		     'python/services/util',
		     'python/infra',
		     'python/config'
		     'python/experiments']
MIN_IFS_NUM_DEFAULT = 3
g_system_shutdown = False
# RPC statistics by endpoint. Contains Max/Min/Avg time taken for each RPC
# which is blocking and non-blocking separately

class RPCMetrics:
	def __init__(self, window_size=4096):
		self.lock = threading.Lock()
		self.window_size = window_size
		self.rpc_rsp_timing_db = {'window': deque(maxlen=self.window_size),
								  'avg': 0.0, 'min': sys.maxsize, 'max': 0.0,
								  'total': 0}
		self.rpc_req_timing_db = {'window': deque(maxlen=self.window_size),
								  'avg': 0.0, 'min': sys.maxsize, 'max': 0.0,
								  'total': 0}
		self.method_timing_db = {}

	def add_timing_locked(self, send_time, recv_time, db):
		rtt = recv_time - send_time
		db['total'] += 1
		db['window'].append(rtt)
		window_len = len(db['window'])
		if window_len > 0:
			db['avg'] = sum(db['window']) / window_len
		if rtt > db['max']:
			db['max'] = rtt
		if rtt < db['min']:
			db['min'] = rtt

	def add_rpc_req_time(self, send_time, recv_time):
		with self.lock:
			self.add_timing_locked(send_time, recv_time, self.rpc_req_timing_db)

	def add_rpc_rsp_time(self, send_time, recv_time):
		with self.lock:
			self.add_timing_locked(send_time, recv_time, self.rpc_rsp_timing_db)

	def add_method_time(self, start_time, end_time, method):
		with self.lock:
			if method not in self.method_timing_db:
				self.method_timing_db[method] = {'window': deque(maxlen=self.window_size),
												 'avg': 0.0, 'min': sys.maxsize, 'max': 0.0,
												 'total': 0}
			self.add_timing_locked(start_time, end_time, self.method_timing_db[method])

	def dump(self):
		import copy

		reqdb = copy.deepcopy(self.rpc_req_timing_db)
		rspdb = copy.deepcopy(self.rpc_rsp_timing_db)
		methodb = copy.deepcopy(self.method_timing_db)
		del(reqdb['window'])
		del(rspdb['window'])
		for k, v in methodb.items():
			del(v['window'])
		logging.critical("RPC request timing statistics")
		logging.critical(yaml.dump(reqdb,
						 Dumper=DEFwDumper, indent=2, sort_keys=False))
		logging.critical("RPC response timing statistics")
		logging.critical(yaml.dump(rspdb,
						 Dumper=DEFwDumper, indent=2, sort_keys=False))
		logging.critical("RPC method timing statistics")
		logging.critical(yaml.dump(methodb,
						 Dumper=DEFwDumper, indent=2, sort_keys=False))

g_rpc_metrics = RPCMetrics()

def get_rpc_rsp_base():
	return {'rpc': {'dst': None, 'src': None, 'type': 'results', 'rc': None,
			'statistics': {'send_time': None}}}

def get_rpc_req_base():
	return {'rpc': {'src': None, 'dst': None, 'type': None, 'script': None,
			'class': None, 'method': None, 'function': None,
			'parameters': {'args': None, 'kwargs': None},
			'statistics': {'send_time': None}}}

#
# Remote object identity in DEFw has two layers:
# - class_id is the caller handle used in RPC payloads and method dispatch
# - module_name:class_name is the identity of a shared singleton service
#
# For per-connection services a class_id maps to one remote object.
# For singleton services multiple caller-generated class_ids can alias the
# same underlying instance through global_singleton_alias_db.
#
global_class_db = {}
global_singleton_db = {}
global_singleton_alias_db = {}
global_class_db_lock = threading.Lock()
global_singleton_db_lock = threading.Lock()

def system_shutdown():
	global g_system_shutdown
	logging.debug("System Shutting down")
	g_system_shutdown = True

def is_system_up():
	global g_system_shutdown
	logging.debug(f"System is {not g_system_shutdown}")
	return not g_system_shutdown

def add_to_class_db(instance, class_id):
	with global_class_db_lock:
		if class_id in global_class_db:
			raise DEFwError("Duplicate class_id. Contention in timing")
		logging.debug(f"created instance for {type(instance).__name__} "\
				      f"with id {class_id}")
		global_class_db[class_id] = instance

def has_class_entry(class_id):
	with global_class_db_lock:
		return class_id in global_class_db

def get_class_from_db(class_id):
	with global_class_db_lock:
		if class_id in global_class_db:
			return global_class_db[class_id]
	logging.debug(f"Request for class not in the database {class_id}")
	raise DEFwNotFound(f'no {class_id} in database')

def del_entry_from_class_db(class_id):
	with global_class_db_lock:
		if class_id in global_class_db:
			instance = global_class_db[class_id]
			logging.debug(f"removing instance for {type(instance).__name__} "\
						"with id {class_id}")
			del global_class_db[class_id]
			global_singleton_alias_db.pop(class_id, None)

def get_singleton_key(module_name, class_name):
	return f"{module_name}:{class_name}"

def get_or_create_singleton_instance(module_name, class_name, factory):
	key = get_singleton_key(module_name, class_name)
	with global_singleton_db_lock:
		if key in global_singleton_db:
			return global_singleton_db[key]
		instance = factory()
		logging.debug(f"created singleton instance for {class_name} with key {key}")
		global_singleton_db[key] = instance
		return instance

def evict_singleton_instance(module_name, class_name):
	key = get_singleton_key(module_name, class_name)
	with global_singleton_db_lock:
		instance = global_singleton_db.pop(key, None)
		with global_class_db_lock:
			aliases = [
				class_id for class_id, alias_key in global_singleton_alias_db.items()
				if alias_key == key
			]
			for class_id in aliases:
				global_singleton_alias_db.pop(class_id, None)
				global_class_db.pop(class_id, None)
		if instance is not None or aliases:
			logging.debug(
				f"evicted singleton instance for {class_name} with key {key} "
				f"and removed {len(aliases)} alias entries"
			)
		return instance

def shutdown_service_instance(instance):
	# Service code should call this helper instead of reaching into the
	# singleton registry directly. The framework owns the mapping from the
	# live instance back to its singleton identity.
	try:
		import defw
		if getattr(defw, 'resmgr', None):
			defw.resmgr.deregister(defw.me.my_endpoint())
	except Exception as exc:
		logging.debug(
			f"Failed to deregister service {instance.__class__.__name__} "
			f"before shutdown: {exc}"
		)
	return evict_singleton_instance(
		instance.__class__.__module__,
		instance.__class__.__name__
	)

def bind_singleton_alias(class_id, module_name, class_name, instance):
	# Bind the caller-visible class_id to an existing singleton instance.
	# The class_id remains the dispatch handle on later method calls even
	# though the object's real identity is module_name:class_name.
	key = get_singleton_key(module_name, class_name)
	with global_class_db_lock:
		if class_id in global_class_db:
			raise DEFwError("Duplicate class_id. Contention in timing")
		logging.debug(f"created instance for {type(instance).__name__} "\
				      f"with id {class_id}")
		global_class_db[class_id] = instance
		global_singleton_alias_db[class_id] = key

def is_singleton_alias(class_id):
	with global_class_db_lock:
		return class_id in global_singleton_alias_db

def dump_class_db():
	with global_class_db_lock:
		for k, v in global_class_db.items():
			logging.debug("id = %f, name = %s" % (k, type(v).__name__))

def populate_rpc_req(src, dst, req_type, module, cname,
		     mname, class_id, *args, **kwargs):
	rpc = get_rpc_req_base()
	rpc['rpc']['src'] = src
	rpc['rpc']['dst'] = dst
	rpc['rpc']['type'] = req_type
	rpc['rpc']['module'] = module
	rpc['rpc']['class'] = cname
	rpc['rpc']['method'] = mname
	rpc['rpc']['class_id'] = class_id
	rpc['rpc']['parameters']['args'] = args
	rpc['rpc']['parameters']['kwargs'] = kwargs
	rpc['rpc']['statistics']['send_time'] = time.time()
	rpc['rpc']['statistics']['recv_time'] = 0
	return rpc

def populate_rpc_rsp(src, dst, rc, exception=None):
	rpc = get_rpc_rsp_base()
	rpc['rpc']['src'] = src
	rpc['rpc']['dst'] = dst
	if exception:
		rpc['rpc']['type'] = 'exception'
		rpc['rpc']['exception'] = exception
	else:
		rpc['rpc']['type'] = 'response'
	rpc['rpc']['rc'] = rc
	rpc['rpc']['statistics']['send_time'] = time.time()
	rpc['rpc']['statistics']['recv_time'] = 0
	return rpc

GLOBAL_PREF_DEF = {'editor': shutil.which('vim'), 'loglevel': 'critical',
		   'halt_on_exception': False, 'remote copy': False,
		   'RPC timeout': 300, 'num_intfs': MIN_IFS_NUM_DEFAULT,
		   'cmd verbosity': True,
		   'debug module reload': False}

global_pref = GLOBAL_PREF_DEF

def set_editor(editor):
	'''
	Set the text base editor to use for editing scripts
	'''
	global global_pref
	if shutil.which(editor):
		global_pref['editor'] = shutil.which(editor)
	else:
		logging.critical("%s is not found" % (str(editor)))
	save_pref()

def set_halt_on_exception(exc):
	'''
	Set halt_on_exception.
		True for raising exception and halting test progress
		False for continuing test progress
	'''
	global global_pref

	if type(exc) is not bool:
		logging.critical("Must be True or False")
		global_pref['halt_on_exception'] = False
		return
	global_pref['halt_on_exception'] = exc
	save_pref()

def set_rpc_timeout(timeout):
	'''
	Set the RPC timeout in seconds.
	That's the timeout to wait for the operation to complete on the remote end.
	'''
	global global_pref
	global_pref['RPC timeout'] = timeout
	save_pref()

def get_rpc_timeout():
	'''
	Get the RPC timeout in seconds.
	That's the timeout to wait for the operation to complete on the remote end.
	'''
	global global_pref
	return global_pref['RPC timeout']

def set_script_remote_cp(enable):
	'''
	set the remote copy feature
	If True then scripts will be remote copied to the agent prior to execution
	'''
	global global_pref
	global_pref['remote copy'] = enable
	save_pref()

def set_debug_module_reload(enable):
	'''
	Enable or disable module reloads on remote RPC dispatch for debugging.
	'''
	global global_pref
	global_pref['debug module reload'] = bool(enable)
	save_pref()

def get_debug_module_reload():
	'''
	Return whether debug module reload is enabled for remote RPC dispatch.
	'''
	global global_pref
	return global_pref['debug module reload']

def _resolve_log_levels(level):
	if isinstance(level, int):
		return [level]
	if not isinstance(level, str):
		raise ValueError(f"Unsupported log level specification: {level}")

	resolved = []
	seen = set()
	for token in level.split(','):
		name = token.strip()
		if not name:
			continue
		name_upper = name.upper()
		if name_upper in CUSTOM_LEVEL_GROUPS:
			for group_level in CUSTOM_LEVEL_GROUPS[name_upper]:
				if group_level not in seen:
					resolved.append(group_level)
					seen.add(group_level)
		elif name_upper in CUSTOM_LEVELS:
			levelno = CUSTOM_LEVELS[name_upper]
			if levelno not in seen:
				resolved.append(levelno)
				seen.add(levelno)
		else:
			levelno = getattr(logging, name_upper)
			if levelno not in seen:
				resolved.append(levelno)
				seen.add(levelno)
	if not resolved:
		raise ValueError("At least one log level must be provided")
	return resolved


def set_logging_level_helper(levelnos):
	global FILE_HANDLER
	global CUSTOM_LEVEL_NAMES

	root_logger = logging.getLogger('')
	for handler in root_logger.handlers[:]:
		root_logger.removeHandler(handler)

	if isinstance(levelnos, int):
		levelnos = [levelnos]

	standard_levels = [levelno for levelno in levelnos
				   if levelno not in CUSTOM_LEVEL_NAMES]
	custom_levels = [levelno for levelno in levelnos
				 if levelno in CUSTOM_LEVEL_NAMES]
	root_levels = list(levelnos) if levelnos else [logging.CRITICAL]
	root_logger.setLevel(min(root_levels))

	FILE_HANDLER.setLevel(min(root_levels))
	for filt in FILE_HANDLER.filters[:]:
		FILE_HANDLER.removeFilter(filt)
	if custom_levels:
		FILE_HANDLER.addFilter(SelectedLevelsFilter(custom_levels,
									 standard_levels))

	root_logger.addHandler(FILE_HANDLER)

class SelectedLevelsFilter(logging.Filter):
	def __init__(self, custom_levels, standard_levels):
		super().__init__()
		self.custom_levels = set(custom_levels)
		self.standard_levels = list(standard_levels)

	def filter(self, record):
		if record.levelno in self.custom_levels:
			return True
		if record.levelno in CUSTOM_LEVEL_NAMES:
			return False
		if not self.standard_levels:
			return record.levelno == logging.CRITICAL
		return record.levelno >= min(self.standard_levels)

def add_logging_level(log_level, level_name, alias_names=None):
	global CUSTOM_LEVELS
	global CUSTOM_LEVEL_NAMES

	func_name = level_name.lower()
	logging.addLevelName(log_level, level_name.upper())

	def custom_level_logger(message, *args, **kwargs):
		if logging.getLogger().isEnabledFor(log_level):
			logging.getLogger()._log(log_level, message, args, **kwargs)

	CUSTOM_LEVELS[level_name.upper()] = log_level
	CUSTOM_LEVEL_NAMES.add(log_level)

	setattr(logging, func_name, custom_level_logger)
	if alias_names:
		for alias_name in alias_names:
			CUSTOM_LEVELS[alias_name.upper()] = log_level
			setattr(logging, alias_name.lower(), custom_level_logger)

def add_logging_group(group_name, level_names):
	global CUSTOM_LEVEL_GROUPS

	CUSTOM_LEVEL_GROUPS[group_name.upper()] = [
		CUSTOM_LEVELS[level_name.upper()] for level_name in level_names
	]

def set_logging_level(level, save=True):
	'''
	Set Python logging selection string.
	Examples: critical, debug, DEFW_ALL, DEFW_CORE,DEFW_RPC
	'''
	global global_pref
	global CUSTOM_LEVELS

	try:
		log_levels = _resolve_log_levels(level)
		set_logging_level_helper(log_levels)
		if save:
			global_pref['loglevel'] = level
	except Exception as e:
		logging.critical(f"error encountered {e}")
		logging.critical("Log level must be one or more comma-separated standard or DEFw log levels")
	if save:
		save_pref()

def setup_log_file():
	global FILE_HANDLER

	py_log_path = cdefw_global.get_defw_tmp_dir()
	Path(py_log_path).mkdir(parents=True, exist_ok=True)
	flog_name = os.path.join(py_log_path, "defw_py.log")
	flog_mode = 'w'
	printformat = "[%(asctime)s:%(filename)s:%(lineno)s:%(funcName)s():Thread-%(thread)d]-> %(message)s"

	logging.basicConfig(filename=flog_name, filemode='w',
						format=printformat)

	FILE_HANDLER = logging.FileHandler(flog_name, mode=flog_mode)
	FILE_HANDLER.setFormatter(logging.Formatter(printformat))

def setup_log_levels():
	add_logging_level(
		DEFW_LOG_LEVEL_CORE,
		DEFW_LOG_LEVEL_CORE_NAME,
		alias_names=["DEFW_INFRA"],
	)
	add_logging_level(
		DEFW_LOG_LEVEL_WORKER,
		DEFW_LOG_LEVEL_WORKER_NAME,
		alias_names=["DEFW_WORKERS"],
	)
	add_logging_level(
		DEFW_LOG_LEVEL_SERVICE,
		DEFW_LOG_LEVEL_SERVICE_NAME,
		alias_names=["DEFW_SERVICES"],
	)
	add_logging_level(
		DEFW_LOG_LEVEL_APP,
		DEFW_LOG_LEVEL_APP_NAME,
		alias_names=["DEFW_EXPERIMENTS"],
	)
	add_logging_level(DEFW_LOG_LEVEL_RPC, DEFW_LOG_LEVEL_RPC_NAME)
	add_logging_level(DEFW_LOG_LEVEL_STACKTRACE, DEFW_LOG_LEVEL_STACKTRACE_NAME)
	add_logging_group(
		"DEFW_ALL",
		[
			DEFW_LOG_LEVEL_CORE_NAME,
			DEFW_LOG_LEVEL_WORKER_NAME,
			DEFW_LOG_LEVEL_SERVICE_NAME,
			DEFW_LOG_LEVEL_APP_NAME,
			DEFW_LOG_LEVEL_RPC_NAME,
			DEFW_LOG_LEVEL_STACKTRACE_NAME,
		],
	)

def set_cmd_verbosity(value):
	'''
	Set the shell command verbosity to either on or off. If on, then
	all the shell commands will be written to the debug logging.
	'''
	global global_pref
	if value.upper() == 'ON':
		global_pref['cmd verbosity'] = True
	else:
		global_pref['cmd verbosity'] = False
	save_pref()

def is_cmd_verbosity():
	'''
	True if command verbosity is set, False otherwise.
	'''
	global global_pref
	return global_pref['cmd verbosity']

def load_pref():
	'''
	Load the DEFw preferences.
		editor - the editor of choice to use for editing scripts
		halt_on_exception - True to throw an exception on first error
				    False to continue running scripts
		log_level - Python log level. One of: critical, debug, error, fatal
	'''
	global GLOBAL_PREF_DEF
	global global_pref

	try:
		global_pref_file = os.environ['DEFW_PREF_PATH']
	except:
		global_pref_file = os.path.join(cdefw_global.get_defw_tmp_dir(), 'defw_pref.yaml')

	if os.path.isfile(global_pref_file):
		with open(global_pref_file, 'r') as f:
			global_pref = yaml.load(f, Loader=yaml.FullLoader)
			if not global_pref:
				global_pref = GLOBAL_PREF_DEF
			else:
				#compare with the default and fill in any entries
				#which might not be there.
				for k, v in GLOBAL_PREF_DEF.items():
					if not k in global_pref:
						global_pref[k] = v
	save_pref()
	return global_pref

def save_pref():
	'''
	Save the DEFw preferences.
		editor - the editor of choice to use for editing scripts
		halt_on_exception - True to throw an exception on first error
				    False to continue running scripts
		log_level - Python log level. One of: critical, debug, error, fatal
	'''
	global global_pref

	try:
		global_pref_file = os.environ['DEFW_PREF_PATH']
	except:
		global_pref_file = os.path.join(cdefw_global.get_defw_tmp_dir(), 'defw_pref.yaml')

	with open(global_pref_file, 'w') as f:
		f.write(yaml.dump(global_pref, Dumper=DEFwDumper, indent=2, sort_keys=False))

	with open(global_pref_file, 'r') as f:
		p = yaml.load(f, Loader=yaml.FullLoader)
		set_logging_level(p['loglevel'], save=False)

def dump_pref():
	global global_pref
	print(yaml.dump(global_pref, Dumper=DEFwDumper, indent=2, sort_keys=True))
