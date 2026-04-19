from pathlib import Path
from cdefw_agent import *
from defw_common_def import *
import defw_common_def as common
from defw_exception import DEFwError, DEFwDumper, DEFwCommError, DEFwNotFound
from defw_cmd import defw_exec_local_cmd
import importlib, socket
import cdefw_global
from defw_agent import DEFwClientAgents, DEFwServiceAgents, \
	 DEFwActiveClientAgents, DEFwActiveServiceAgents, Endpoint
import netifaces, random
import atexit
import os, subprocess, sys, yaml, fnmatch, logging, csv, uuid, io, signal
import shutil, traceback, datetime, re, copy, threading, queue, time
from defw_util import prformat, fg, bg, generate_random_string, \
	 get_lscpu, get_today, get_now, print_all_thread_stack_traces_to_logger

preferences = {}
defw_tmp_dir = ''
defw_path = ''
only_load = []
noinit_load = []
g_yaml_blocks = []
client_agents = None
service_agents = None
active_client_agents = None
active_service_agents = None
defw_config_yaml = None
me = None
resmgr = None
updater_thread = None

def get_nearest_yaml_block():
	global g_yaml_blocks
	return g_yaml_blocks[-1]

def get_top_yaml_block():
	global g_yaml_blocks
	return g_yaml_blocks[0]

g_keywords = {'DATE': get_today,
			  'TIME': get_now,
			  'YNEAR': get_nearest_yaml_block,
			  'YTOP': get_top_yaml_block}

def split_external_paths(value):
	if not value:
		return []
	return [p for p in value.split(':') if p]

def clear_module_cache(module_name):
	for key in list(sys.modules.keys()):
		if key == module_name or key.startswith(module_name + '.'):
			del sys.modules[key]

def import_module_from_path(module_name, module_path):
	clear_module_cache(module_name)
	if module_path in sys.path:
		sys.path.remove(module_path)
	sys.path.insert(0, module_path)
	mod = importlib.import_module(module_name)
	importlib.reload(mod)
	return mod

class DEFwYaml:
	def __init__(self, y=None):
		if y is not None and (type(y) is not dict and type(y) is not list):
			raise DEFwError('This class takes dictionaries or lists only')
		self.__yaml = y

	def get(self):
		return self.__yaml

	def dump(self):
		return yaml.dump(self.__yaml)

	def load(self, stream):
		if self.__yaml:
			raise DEFwError('There exists a YAML instance')
		self.__yaml = yaml.load(stream, Loader=yaml.FullLoader)

	def unload(self):
		self.__yaml = None

class YamlResults:
	def __init__(self):
		self.__results = []
		self.__max = 0
		self.__n = 0

	def __setitem__(self, key, value):
		for i, e in enumerate(self.__results):
			if e.get()['name'] == key:
				value['name'] = key
				self.__results[i] = DEFwYaml(value)
				return
		value['name'] = key
		self.__results.append(DEFwYaml(value))
		self.__max = len(self.__results)

	def __getitem__(self, key):
		for entry in self.__results:
			if entry.get()['name'] == key:
				return entry
		return None

	def __iter__(self):
		self.__n = 0
		return self

	# needed for python 3.x
	def __next__(self):
		if self.__n < self.__max:
			rc = self.__results[self.__n]
			self.__n += 1
			return rc['name'], rc.get()
		else:
			raise StopIteration

	def get(self, status=None):
		shadow = []
		for entry in self.__results:
			e = entry.get()
			if status and type(status) == str:
				if e['status'] != status.upper():
					continue
			shadow.append(entry.get())
		return shadow

# subtest_result = YamlResults
# global_test_resutls['defw-dlc']['script-name'] = rc
class YamlGlobalTestResults:
	def __init__(self, desc=None):
		if not desc:
			self.desc = 'auster defw'
		self.__results = {'Tests': []}
		self.__max = 0
		self.__n = 0

	def __setitem__(self, key, value):
		if type(value) != dict:
			raise TypeError("This class only takes dict type")
		for i, e in enumerate(self.__results['Tests']):
			if e['name'] == key:
				self.__results['Tests'][i]['SubTests'][value['name']] = value
				self.finalize(key)
				return
		defw = {'name': key, 'description': self.desc, 'SubTests': YamlResults()}
		defw['SubTests'][value['name']] = value
		self.__results['Tests'].append(defw)
		self.__max = len(self.__results['Tests'])
		self.finalize(key)

	def __getitem__(self, key):
		for entry in self.__results['Tests']:
			if entry['name'] == key:
				return entry['SubTests']
		return None

	def __iter__(self):
		self.__n = 0
		return self

	# needed for python 3.x
	def __next__(self):
		if self.__n < self.__max:
			rc = self.__results['Tests'][self.__n]
			self.__n += 1
			return rc['name'], rc
		else:
			raise StopIteration

	def finalize(self, name):
		timefmt = datetime.datetime.utcnow().strftime('%a %b %d %H:%M:%S UTC %Y')
		for e in self.__results['Tests']:
			if e['name'] == name:
				total_duration = 0
				sstatus = 'PASS'
				subs = e['SubTests'].get()
				for r in subs:
					total_duration += r['duration']
					if r['status'] == 'FAIL':
						sstatus = 'FAIL'
					elif r['status'] == 'SKIP' and sstatus != 'FAIL':
						sstatus = 'SKIP'
				e['duration'] = total_duration
				e['status'] = sstatus
				e['submission'] = timefmt

	def get(self):
		rc = copy.deepcopy(self.__results)
		for t in rc['Tests']:
			t['SubTests'] = t['SubTests'].get()
		return rc


def normalize_experiment_result(result):
	from defw_remote import DEFwResult
	if isinstance(result, dict) and 'status' in result:
		rc = DEFwResult(copy.deepcopy(dict(result)))
	elif isinstance(result, bool):
		rc = DEFwResult({'status': 'PASS' if result else 'FAIL'})
	elif result is None:
		rc = DEFwResult({'status': 'PASS'})
	else:
		rc = DEFwResult({'status': 'PASS', 'args': [result]})
	return rc


def get_result_details(result):
	if 'error' in result:
		return str(result['error']).splitlines()[0]
	if 'kwargs' in result and result['kwargs']:
		return ', '.join(f"{k}={v}" for k, v in result['kwargs'].items())
	if 'args' in result and result['args']:
		return ', '.join(str(v) for v in result['args'])
	return ''


def format_experiment_result(result):
	lines = [f"status: {result['status']}"]
	if 'kwargs' in result and result['kwargs']:
		for key, value in result['kwargs'].items():
			lines.append(f"{key}: {value}")
	if 'args' in result and result['args']:
		lines.append("args:")
		for value in result['args']:
			lines.append(f"  - {value}")
	if 'error' in result:
		lines.append("error:")
		lines.append(str(result['error']))
	return "\n".join(lines)


def formatGlobalTestResultsTable(status=None):
	global global_test_results

	status_filter = status.upper() if isinstance(status, str) else None
	rows = []
	for suite in global_test_results.get()['Tests']:
		for result in suite['SubTests']:
			if status_filter and result['status'] != status_filter:
				continue
			rows.append({
				'experiment': f"{suite['name']}::{result['name']}",
				'status': result['status'],
				'duration': f"{result.get('duration', 0):.3f}s",
				'details': get_result_details(result),
			})

	headers = ['Experiment', 'Status', 'Duration', 'Details']
	if not rows:
		return "No experiment results recorded."

	widths = [len(h) for h in headers]
	for row in rows:
		widths[0] = max(widths[0], len(row['experiment']))
		widths[1] = max(widths[1], len(row['status']))
		widths[2] = max(widths[2], len(row['duration']))
		widths[3] = max(widths[3], len(row['details']))

	def format_row(values):
		return " | ".join(value.ljust(widths[idx]) for idx, value in enumerate(values))

	sep = "-+-".join("-" * width for width in widths)
	lines = [
		format_row(headers),
		sep,
	]
	for row in rows:
		lines.append(format_row([
			row['experiment'],
			row['status'],
			row['duration'],
			row['details'],
		]))
	return "\n".join(lines)


def formatGlobalTestResultsReport(status=None):
	global global_test_results

	report = [formatGlobalTestResultsTable(status=status)]
	status_filter = status.upper() if isinstance(status, str) else None
	for suite in global_test_results.get()['Tests']:
		for result in suite['SubTests']:
			if status_filter and result['status'] != status_filter:
				continue
			if 'error' not in result:
				continue
			report.append("")
			report.append(f"[{suite['name']}::{result['name']}]")
			report.append(format_experiment_result(result))
	return "\n".join(report)

class Documentation:
	def __init__(self, base_name):
		doc_path = os.path.join(cdefw_global.get_defw_path(), 'documentation')
		Path(doc_path).mkdir(parents=True, exist_ok=True)
		self.__req = os.path.join(cdefw_global.get_defw_path(), 'documentation',
					  os.path.splitext(base_name)[0]+'_req.csv')
		self.__hld = os.path.join(cdefw_global.get_defw_path(), 'documentation',
					  os.path.splitext(base_name)[0]+'_hld.csv')
		self.__tp = os.path.join(cdefw_global.get_defw_path(), 'documentation',
					  os.path.splitext(base_name)[0]+'_tp.csv')
		self.__req_writeheader()
		self.__hld_writeheader()
		self.__tp_writeheader()

	def __req_writeheader(self):
		if not os.path.isfile(self.__req):
			header = ["Test Case ID", "Requirement Id", "Requirement Description"]
			with open(self.__req, 'w') as fcsv:
				writer = csv.writer(fcsv)
				writer.writerow(header)

	def __hld_writeheader(self):
		if not os.path.isfile(self.__req):
			header = ["Test Case ID", "Requirement Id", "Design Notes"]
			with open(self.__hld, 'w') as fcsv:
				writer = csv.writer(fcsv)
				writer.writerow(header)

	def __tp_writeheader(self):
		if not os.path.isfile(self.__req):
			header = ["Test Case ID", "Primary Requirement Id", "Secondary Requirement Id", "Test Case"]
			with open(self.__tp, 'w') as fcsv:
				writer = csv.writer(fcsv)
				writer.writerow(header)

	def req_writerow(self, req_id, req_desc, fname):
		with open(self.__req, 'a+') as fcsv:
			writer = csv.writer(fcsv)
			writer.writerow([fname, req_id, req_desc])

	def hld_writerow(self, req_id, design, fname):
		with open(self.__hld, 'a+') as fcsv:
			writer = csv.writer(fcsv)
			writer.writerow([fname, req_id, design])

	def tp_writerow(self, preq_id, sreq_id, tc, fname):
		with open(self.__tp, 'a+') as fcsv:
			writer = csv.writer(fcsv)
			writer.writerow([fname, preq_id, sreq_id, tc])

class MethodInterceptor(object):
	def __init__(self, child, disabled_methods):
		self.__disabled_methods = disabled_methods

	def __getattribute__(self, name):
		original_method = object.__getattribute__(self, name)
		if (callable(original_method) and name not in self.__disabled_methods) \
			or (not callable(original_method)):
			return original_method
		else:
			def noop(*args, **kwargs):
				print(f"Method '{name}' not allowed on this object.")
				return
			return noop

class Script(MethodInterceptor):
	def __init__(self, abs_path, collection):
		super().__init__(self, collection.get_disabled_methods())
		self.name = os.path.splitext(os.path.split(abs_path)[1])[0]
		self.__abs_path = abs_path
		self.__prefix = collection.get_prefix()
		self.__callbacks = collection.get_callbacks()
		self.__parent_suite = collection.get_suite_name().replace('suite_', '')
		self.__collection = collection

	def is_expected_failure(self, name):
		return self.__collection.in_expected_failures_list(name)

	def create_docs(self, csvfile):
		# open script and extract comment block. It is expected to
		# be at the beginning of the file
		doc = []
		start = False
		with open(self.__abs_path, 'r') as f:
			lines = f.readlines()
			for l in lines:
				if len(l.strip()) > 0 and l.strip() == '"""':
					if start:
						start = False
						break
					else:
						start = True
				elif start:
					doc.append(l.strip())
		if len(doc) == 0:
			return

		meta = {'prim': {'txt': [], 'st': False},
			'primd': {'txt': [], 'st': False},
			'sec': {'txt': [], 'st': False},
			'des': {'txt': [], 'st': False},
			'tc': {'txt': [], 'st': False}}

		for l in doc:
			if '@PRIMARY:' in l:
				meta['prim']['st'] = True
				meta['primd']['st'] = False
				meta['sec']['st'] = False
				meta['des']['st'] = False
				meta['tc']['st'] = False
				meta['prim']['txt'].append(l.split('@PRIMARY:')[1].strip())
			elif '@PRIMARY_DESC:' in l:
				meta['prim']['st'] = False
				meta['primd']['st'] = True
				meta['sec']['st'] = False
				meta['des']['st'] = False
				meta['tc']['st'] = False
				meta['primd']['txt'].append(l.split('@PRIMARY_DESC:')[1].strip())
			elif '@SECONDARY:' in l:
				meta['prim']['st'] = False
				meta['primd']['st'] = False
				meta['sec']['st'] = True
				meta['des']['st'] = False
				meta['tc']['st'] = False
				meta['sec']['txt'].append(l.split('@SECONDARY:')[1].strip())
			elif '@DESIGN:' in l:
				meta['prim']['st'] = False
				meta['primd']['st'] = False
				meta['sec']['st'] = False
				meta['des']['st'] = True
				meta['tc']['st'] = False
				meta['des']['txt'].append(l.split('@DESIGN:')[1].strip())
			elif '@TESTCASE:' in l:
				meta['prim']['st'] = False
				meta['primd']['st'] = False
				meta['sec']['st'] = False
				meta['des']['st'] = False
				meta['tc']['st'] = True
				meta['tc']['txt'].append(l.split('@TESTCASE:')[1].strip())
			elif meta['prim']['st']:
				meta['prim']['txt'].append('\n'+l)
			elif meta['primd']['st']:
				meta['primd']['txt'].append('\n'+l)
			elif meta['sec']['st']:
				meta['sec']['txt'].append('\n'+l)
			elif meta['des']['st']:
				meta['des']['txt'].append('\n'+l)
			elif meta['tc']['st']:
				meta['tc']['txt'].append('\n'+l)

		documentation = Documentation(csvfile)
		documentation.req_writerow("".join(meta['prim']['txt']),
					   "".join(meta['primd']['txt']),
					   self.name)
		documentation.hld_writerow("".join(meta['prim']['txt']),
					   "".join(meta['des']['txt']),
					   self.name)
		documentation.tp_writerow("".join(meta['prim']['txt']),
					  "".join(meta['sec']['txt']),
					  "".join(meta['tc']['txt']),
					   self.name)

	def initialize(self):
		name = self.name.replace(self.__prefix, '')

		preferences = common.global_pref

		module = __import__(self.name)
		# force a reload in case it has changed since it has
		# been previously be imported
		importlib.reload(module)
		try:
			module_run = getattr(module, 'initialize')
		except Exception as e:
			logging.critical(e)
			return
		# run the script
		if hasattr(module_run, '__call__'):
			try:
				logging.critical("Initializing Module: %s" % str(self.name))
				rc = module_run()
			except Exception as e:
				if preferences['halt_on_exception']:
					raise e
				else:
					# if the script went out of its way to say I want to halt all execution
					# then honor that.
					if type(e) == DEFwError and e.halt:
						raise e
					else:
						logging.critical("Initializing %s failed" % str(self.name))

	def execute_method_by_name(self, method_name):
		global global_test_results
		global preferences

		name = self.name.replace(self.__prefix, '')

		preferences = common.global_pref
		start_time = time.time()

		module = __import__(self.name)
		# force a reload in case it has changed since it has
		# been previously be imported
		importlib.reload(module)
		try:
			module_run = getattr(module, method_name)
		except Exception as e:
			logging.critical(e)
			return
			# run the script
		try:
			rc = module_run()
		except Exception as e:
			if preferences['halt_on_exception']:
				raise e
			else:
				# if the script went out of its way to say I want to halt all execution
				# then honor that.
				if type(e) == DEFwError and e.halt:
					raise e
				else:
					from defw_remote import DEFwResult
					rc = DEFwResult({
						'status': 'FAIL',
						'error': traceback.format_exc(),
					})
		duration = time.time() - start_time
		if method_name == 'run':
			result = normalize_experiment_result(rc)
			result['duration'] = duration
			global_test_results[self.__parent_suite] = {
				'name': name,
				'status': result['status'],
				'duration': result['duration'],
				**{k: v for k, v in result.items()
				   if k not in ['name', 'status', 'duration']},
			}
		return rc

	def run(self, progress=-1):
		self.execute_method_by_name('run')

	def initialize(self, progress=-1):
		self.execute_method_by_name('initialize')

	def show(self):
		with open(self.__abs_path, 'r') as f:
			for line in f:
				print(line.strip('\n'))

	def edit(self):
		global preferences
		preferences = common.global_pref

		try:
			subprocess.call(preferences['editor']+" "+self.__abs_path, shell=True)
		except:
			logging.critical("No editor available")
			print("No editor available")

class Collection(MethodInterceptor):
	def __init__(self, base, name, callbacks, skip_list,
				 expected_failures, prefix, disabled_methods):
		super().__init__(self, disabled_methods)
		self.__suite_name = name
		self.__test_db = {}
		self.__prefix = prefix
		self.__max = 0
		self.__n = 0
		self.__abs_path = os.path.join(base, name)
		self.__callbacks = callbacks
		self.__skip_list = skip_list
		self.__expected_failures = expected_failures
		self.__disabled_methods = disabled_methods
		self.reload()

	def __getitem__(self, key):
		try:
			rc = self.__test_db[key]
		except:
			raise DEFwError('no entry for ' + str(key))
		return rc

	def __iter__(self):
		self.__n = 0
		return self

	# needed for python 3.x
	def __next__(self):
		if self.__n < self.__max:
			key = list(self.__test_db.keys())[self.__n]
			suite = self.__test_db[key]
			self.__n += 1
			return key, suite
		else:
			raise StopIteration

	def __generate_test_db(self, db):
		# defw/python/tests/suite_xxx has a list of tests
		# make a dictionary of each of these. Each test script
		# should start with "test_"
		for subdir, dirs, files in os.walk(self.__abs_path):
			added = False
			for f in files:
				if f.startswith(self.__prefix) and os.path.splitext(f)[1] == '.py':
					# add any subidrectories to the sys path
					if subdir != '.' and not added:
						subdirectory = os.path.join(self.__abs_path, subdir)
						if subdirectory not in sys.path:
							sys.path.append(subdirectory)
					added = True
					name = os.path.splitext(f.replace(self.__prefix, ''))[0]
					db[name] = Script(os.path.join(self.__abs_path, subdir, f), self)

		self.__max = len(self.__test_db)

	def in_expected_failures_list(self, name):
		return name in self.__expected_failures

	def __in_skip_list(self, name):
		return name in self.__skip_list

	def reload(self):
		self.__test_db = {}
		self.__generate_test_db(self.__test_db)

	def get_num_scripts(self, match='*'):
		num_scripts = 0
		for key in sorted(self.__test_db.keys()):
			if fnmatch.fnmatch(key, match) and not self.__in_skip_list(key):
				num_scripts += 1
		return num_scripts

	def get_disabled_methods(self):
		return self.__disabled_methods

	def get_prefix(self):
		return self.__prefix

	def get_callbacks(self):
		return self.__callbacks

	def get_suite_name(self):
		return self.__suite_name

	# run all the scripts in this test suite
	def execute_method_by_name(self, method_name, match, num_scripts):
		# get number of scripts
		if not num_scripts:
			num_scripts = self.get_num_scripts(match)

		executed = 0

		with open(me.get_test_progress_path(), 'a+') as f:
			out = '-----============= defw-' + self.__suite_name.replace('suite_', '') + "\n"
			f.write(out)
			f.flush()

		for key in sorted(self.__test_db.keys()):
			if fnmatch.fnmatch(key, match) and not self.__in_skip_list(key):
				executed += 1
				progress = int((executed / num_scripts) * 100)
				method = getattr(self.__test_db[key], method_name)
				if not callable(method):
					continue
				method(progress)

	def run(self, match='*', num_scripts=0):
		self.execute_method_by_name('run', match, num_scripts)

	def initialize(self, match='*', num_scripts=0):
		self.execute_method_by_name('initialize', match, num_scripts)

	def create_docs(self, csvfile, match='*'):
		for k, v in self.__test_db.items():
			if fnmatch.fnmatch(k, match):
				v.create_docs(csvfile)

	def list(self):
		return list(self.__test_db.keys())

	def dump(self, match='*'):
		scripts_dict = {'scripts': []}
		for k, v in self.__test_db.items():
			if fnmatch.fnmatch(k, match):
				if self.in_expected_failures_list(k):
					scripts_dict['scripts'].append(k+' (expected failure)')
				elif self.__in_skip_list(k):
					scripts_dict['scripts'].append(k+' (skip)')
				else:
					scripts_dict['scripts'].append(k)
		scripts_dict['scripts'].sort()
		print(yaml.dump(scripts_dict, Dumper=DEFwDumper, indent=2, sort_keys=True))

	def get_suite_name(self):
		return self.__suite_name

	def len(self):
		return len(self.__test_db)

class SuiteCallbacks:
	def __init__(self, **kwargs):
		if type(kwargs) is not dict:
			raise DEFwError("Must specify a dictionary")
		self.__callbacks = kwargs
	def __contains__(self, key):
		return key in self.__callbacks
	def __getitem__(self, key):
		try:
			rc = self.__callbacks[key]
		except:
			raise DEFwError('no entry for ' + str(key))
		return rc
	def dump(self):
		print(yaml.dump(self.__callbacks, Dumper=DEFwDumper, indent=2, sort_keys=True))

class ASuite(MethodInterceptor):
	def __init__(self, base, name, prefix, disabled_methods):
		super().__init__(self, disabled_methods)
		self.__base = base
		self.__prefix = prefix
		self.__callback_reg = False
		self.__callbacks = None
		self.name = name
		self.__abs_path = os.path.join(base, name)
		self.scripts = None
		self.__skip_list = []
		self.__expected_failures = []
		self.__disabled_methods = disabled_methods
		if self.__abs_path not in sys.path:
			sys.path.append(self.__abs_path)
		self.reload()

	def __register_callbacks(self):
		if self.__callback_reg:
			return
		# find callbacks module in this suite and get the callbacks
		for subdir, dirs, files in os.walk(self.__abs_path):
			break
		for f in files:
			if f == 'skip.py':
				mod_name = self.name+'.'+'skip'
				module = __import__(mod_name)
				importlib.reload(module)
				try:
					if type(module.skip.skip_list) != list:
						logging.critical('malformed skip list')
						continue
					try:
						self.__skip_list = module.skip.skip_list
					except:
						pass
					try:
						self.__expected_failures = module.skip.expected_failures
					except:
						pass
				except Exception as e:
					logging.critical(str(e))
					pass
				del(module)

	def reload(self):
		self.__callback_reg = False
		self.__register_callbacks()
		self.scripts = Collection(self.__base, self.name, self.__callbacks,
								  self.__skip_list, self.__expected_failures,
								  self.__prefix, self.__disabled_methods)

	def dump(self, match='*'):
		self.scripts.dump(match)

	def list(self):
		return self.scripts.list()

	def create_docs(self, csvfile, match='*'):
		self.scripts.create_docs(csvfile, match)

	def get_num_scripts(self, match='*'):
		return self.scripts.get_num_scripts(match)

	def initialize(self, match='*', num_scripts=0):
		self.scripts.initialize(match=match, num_scripts=num_scripts)

	def run(self, match='*', num_scripts=0):
		self.scripts.run(match=match, num_scripts=num_scripts)

	def get_abs_path(self):
		return self.__abs_path

class Suites(MethodInterceptor):
	'''
	This class stores all the available suites in the provided path.
	The following methods are available for the suites:
		list() - list all the suites
		run() - run all the suites
		dump() - YAML output of the suites available
		create_docs() - create document for all suites
	These methods can be overridden if the class is inherited
	A single suite can be accessed as follows:
		suites['name of suite']
	A single suite provides the following methods:
		list() - list all the scripts in the suite
		run() - Run all the scripts in the suite
		dump() - YAML output of the scripts available
		create_docs() - create document for this suite
	The available methods are the same ones available on the suite
	A single script can be accessed as follows:
		suites['name of suite'].scripts['name of script']
	A single script provides the following methods:
		edit() - edit the script
		show() - show the script
		run() - run the script
	These methods can be disabled if need be. For example if we don't want
	to edit or run the script, we can just disable these methods.
	'''
	def __init__(self, path, suite_prefix='suite_', prefix="", disabled_methods=[]):
		# iterate over the test scripts directory and generate
		# An internal database
		global defw_path

		super().__init__(self, disabled_methods)
		self.test_db = {}
		self.__prefix = prefix
		self.max = 0
		self.n = 0
		self.suites_path = path
		self.__disabled_methods = disabled_methods
		self.suite_prefix = suite_prefix
		self.generate_test_db(self.test_db)

	def __getitem__(self, key):
		try:
			rc = self.test_db[key]
		except:
			raise DEFwError('no entry for ' + str(key))
		return rc

	def __iter__(self):
		self.n = 0
		return self

	# needed for python 3.x
	def __next__(self):
		if self.n < self.max:
			key = list(self.test_db.keys())[self.n]
			suite = self.test_db[key]
			self.n += 1
			return key, suite
		else:
			raise StopIteration

	def __contains__(self, item):
		return item in self.test_db

	def generate_test_db(self, db):
		# There should be a directory for each suite in the path provided.
		# Make a dictionary of each of these. The provided path
		# is one level hierarchy. Each directory suite should start
		# with self.suite_prefix
		for path in self.suites_path:
			for subdir, dirs, files in os.walk(path):
				break
			for d in dirs:
				if d.startswith(self.suite_prefix):
					name = d.replace(self.suite_prefix, '')
					db[name] = ASuite(path, d, self.__prefix,
									self.__disabled_methods)

		self.max = len(self.test_db)

	def create_docs(self, csvfile, match='*'):
		for k, v in self.test_db.items():
			if fnmatch.fnmatch(k, match):
				v.create_docs(csvfile)

	# run all the test suites
	def execute_method_by_name(self, method_name, suite_list, match):
		numscripts = {}
		if suite_list == '*':
			sl = list(self.test_db.keys())
		else:
			sl = [item for item in re.split(',| ', suite_list) if len(item.strip()) > 0]
		num_scripts = 0
		for k, v in self.test_db.items():
			if k in sl:
				numscripts[k] = v.get_num_scripts('*')

		for k, v in self.test_db.items():
			if k in sl:
				method = getattr(v, method_name)
				if not callable(method):
					continue
				method(num_scripts=numscripts[k])

	# run all the test suites
	def run(self, suite_list='*', match='*'):
		self.execute_method_by_name('run', suite_list, match)

	def initialize(self, suite_list='*', match='*'):
		self.execute_method_by_name('initialize', suite_list, match)

	def reload(self):
		self.test_db = {}
		self.generate_test_db(self.test_db)

	def len(self):
		return len(self.test_db)

	def list(self):
		return list(self.test_db.keys())

	def dump(self, match='*'):
		suites_dict = {'suites': []}
		for k, v in self.test_db.items():
			if fnmatch.fnmatch(k, match):
				suites_dict['suites'].append(k)
		suites_dict['suites'].sort()
		print(yaml.dump(suites_dict, Dumper=DEFwDumper, indent=2, sort_keys=True))

	def finalize(self):
		for k, v in self.test_db.items():
			try:
				v.uninitialize()
			except:
				pass

class ServiceSuitesBase(Suites):
	def __init__(self, path, prefix="", disabled_methods=[], noload_resmgr=True, suite_prefix='suite_'):
		self.noload_resmgr = noload_resmgr
		self.suite_prefix = suite_prefix
		super().__init__(path, prefix=prefix, suite_prefix=suite_prefix, disabled_methods=disabled_methods)

	def generate_test_db(self, db):
		# There should be a directory for each suite in the path provided.
		# Make a dictionary of each of these. The provided path
		# is one level hierarchy. Each directory suite should start
		# with self.suite_prefix
		for path in self.suites_path:
			for subdir, dirs, files in os.walk(path):
				break
			for d in dirs:
				#import the directory as a package
				if d.startswith(self.suite_prefix):
					name = d.replace(self.suite_prefix, '')
					mod_path = d
					if noinit_load and d in noinit_load:
						sys.path.append(mod_path)
						continue
					if only_load and d not in only_load and name != 'resmgr':
						continue
					# TODO for now disable loading the resmgr if you're not
					# the resmgr. is there a better way of handling this?
					if not me.is_resmgr() and name == 'resmgr' and self.noload_resmgr:
						continue
					mod = import_module_from_path(mod_path, path)
					mname = mod.svc_info['name']
					db[mname] = mod
					try:
						db[mname].initialize()
					except:
						pass

		self.max = len(self.test_db)

class ServiceSuites(ServiceSuitesBase):
	def __init__(self):
		global defw_config_yaml

		paths = [os.path.join(defw_path, "python", "services")]
		try:
			v = defw_config_yaml['defw']['external-services']
			tmp_paths = split_external_paths(v)
			setup_external_paths(tmp_paths)
			paths += tmp_paths
		except:
			pass
		super().__init__(paths,
						 prefix="svc_", disabled_methods=['run', 'edit'],
						 suite_prefix="svc_")

class ServiceSuiteAPIs(ServiceSuitesBase):
	def __init__(self):
		global defw_config_yaml

		paths = [os.path.join(defw_path, "python", "service-apis")]
		try:
			v = defw_config_yaml['defw']['external-service-apis']
			tmp_paths = split_external_paths(v)
			setup_external_paths(tmp_paths)
			paths += tmp_paths
		except:
			pass
		super().__init__(paths,
						 prefix="api_", disabled_methods=['run', 'edit'],
						 noload_resmgr=False, suite_prefix="api_")

class ExpSuites(Suites):
	def __init__(self):
		global defw_config_yaml

		paths = [os.path.join(defw_path, "python", "experiments")]
		try:
			v = defw_config_yaml['defw']['external-experiments']
			if(v):
				tmp_paths = split_external_paths(v)
				setup_external_paths(tmp_paths)
				paths += tmp_paths
		except:
			pass
		super().__init__(paths, prefix="exp_")

import builtins
_original_exit = builtins.exit

class Myself:
	'''
	Class which represents this DEFw instance.
	It allows extraction of:
		- interfaces available
		- listen port
		- telnet port
		- name
		- hostname
		- DEFw type
	It provides an exit method to exit the DEFw instance
	'''
	def __init__(self, cy):
		global preferences
		preferences = common.global_pref
		self.__cpuinfo = get_lscpu()
		self.defw_cfg = cy
		listen_address = cdefw_global.get_listen_address()
		if listen_address == "0.0.0.0":
			listen_address = socket.gethostbyname('localhost')
		self.__my_endpoint = Endpoint(listen_address,
									cdefw_global.get_listen_port(),
									cdefw_global.get_listen_port(),
									os.getpid(),
									cdefw_global.get_node_name(),
									socket.gethostname(),
									cdefw_global.get_defw_type(),
									cdefw_global.get_defw_uuid())
		# Write the pid of the process in the file so it can be used to
		# monitor my life
		pid_path = os.path.join(cdefw_global.get_defw_tmp_dir(), 'pid')
		logging.debug(f"Path to PID file is {pid_path}")
		with open(pid_path, 'w') as f:
			logging.debug(f"WRITING PID TO FILE: {os.getpid()}")
			f.write(str(os.getpid()))


	def is_self(self, target):
		rc = target.name.upper() == self.my_name().upper() and \
			   target.hostname.upper() == self.my_hostname().upper() and \
			   target.node_type == self.my_type() and \
			   os.getpid() == target.pid and \
			   target.addr == self.my_listenaddress() and \
			   target.port == self.my_listenport()
		return rc

	def is_resmgr(self):
		return self.__my_endpoint.is_resmgr()

	def import_env_vars(self, fpath):
		with open(fpath, 'r') as f:
			for line in f.readlines():
				if 'export ' in line:
					s = line.replace('export ', '')
					kv = s.split('=')
					os.environ[kv[0].strip()] = kv[1].strip().strip('"')

	def get_test_progress_path(self):
		if 'test-progress' in self.defw_cfg['defw']:
			path = self.defw_cfg['defw']['test-progress']
		else:
			path = cdefw_global.get_defw_tmp_dir()
			path = os.path.join(path, 'defw_test_progress.out')
		return path

	def get_local_interface_names(self):
		return netifaces.interfaces()

	def get_local_interface_ip(self, name):
		return netifaces.ifaddresses(name)[netifaces.AF_INET][0]['addr']

	def get_local_interface_nm(self, name):
		return netifaces.ifaddresses(name)[netifaces.AF_INET][0]['netmask']

	def get_local_interface_bc(self, name):
		return netifaces.ifaddresses(name)[netifaces.AF_INET][0]['broadcast']

	def exit(self):
		'''
		Shutdown the DEFw
		'''
		global _original_exit

		common.g_rpc_metrics.dump()
		services.finalize()
		service_apis.finalize()
		common.system_shutdown()
		from defw_workers import put_shutdown
		put_shutdown()
		updater_thread.join()
		logging.critical("Shutting down the DEFw")
		print_all_thread_stack_traces_to_logger()
		# if we are in the context of the telnet server, we can not tell
		# it to stop directly, because that'll cause a deadlock. However
		# if we call exit, everything shutsdown anyway.
		#from defw_telnet_sr import g_tns
		#if g_tns:
		#	g_tns.stop()
		_original_exit()

	def get_cpuinfo(self):
		return self.__cpuinfo

	def get_num_cpus(self):
		return int(self.__cpuinfo['CPU(s)'])

	def get_num_numa_nodes(self):
		return int(self.__cpuinfo['NUMA node(s)'])

	def list_intfs(self):
		'''
		Return a list of all the interfaces available on this node
		'''
		intfs = {'interfaces': {}}
		for intf in self.get_local_interface_names():
			try:
				intfs['interfaces'][intf] = {'ip': self.get_local_interface_ip(intf),
							     'netmask': self.get_local_interface_nm(intf),
							     'broadcast': self.get_local_interface_bc(intf)}
			except:
				pass
		return intfs

	def dump_intfs(self):
		'''
		Dump the interfaces in YAML format
		'''
		print(yaml.dump(self.list_intfs(), sort_keys=False))

	def dump_cpuinfo(self):
		'''
		Dump CPU information
		'''
		for k, v in self.__cpuinfo.items():
			print(k, ': ', v)

	def my_name(self):
		'''
		Return the symbolic name assigned to this DEFw instance
		'''
		return self.__my_endpoint.name

	def my_hostname(self):
		'''
		Return the hostname of this node
		'''
		return self.__my_endpoint.hostname

	def my_type(self):
		'''
		Return the type of this DEFw instance
		'''
		self.__my_endpoint.node_type2str()

	def my_listenport(self):
		'''
		Return the listen port of this DEFw instance
		'''
		return self.__my_endpoint.port

	def my_listenaddress(self):
		'''
		Return the listen port of this DEFw instance
		'''
		return self.__my_endpoint.addr

	def my_pid(self):
		'''
		Return the pid of this DEFw instance
		'''
		return self.__my_endpoint.pid

	def my_uuid(self):
		'''
		Return the global UUID of this instance
		'''
		return self.__my_endpoint.remote_uuid

	def my_endpoint(self):
		'''
		Return the agent information of the node
		'''
		return self.__my_endpoint

	def dump_endpoint(self):
		'''
		Return the agent information of the node
		'''
		return print(self.__my_endpoint)

# Dump the global results to console or to file
def dumpGlobalTestResults(fname=None, status=None, desc=None):
	'''
	Dump the YAML results for tests which ran so far
	'''
	global global_test_results

	results = global_test_results.get()
	report = formatGlobalTestResultsReport(status=status)

	if fname:
		fpath = fname
		# if this is path then use it as is, otherwise put it in the tmp dir
		if os.sep not in fname:
			fpath = os.path.join(cdefw_global.get_defw_tmp_dir(), fname)
		with open(fpath, 'w') as f:
			f.write(yaml.dump(results,
				Dumper=DEFwDumper, indent=2,
				sort_keys=False))
	else:
		print(report)

	return report

def setup_external_paths(paths, prepend=False):
	global defw_tmp_dir

	if prepend:
		for p in reversed(paths):
			if p in sys.path:
				sys.path.remove(p)
			sys.path.insert(0, p)
	else:
		for p in paths:
			if p not in sys.path:
				sys.path.append(p)

def setup_paths():
	global defw_tmp_dir

	for p in DEFW_SCRIPT_PATHS:
		path = os.path.join(cdefw_global.get_defw_path(),p)
		if path not in sys.path:
			sys.path.append(path)
	for env_var in ['DEFW_EXTERNAL_SERVICE_APIS_PATH',
					'DEFW_EXTERNAL_SERVICES_PATH',
					'DEFW_EXTERNAL_EXPERIMENTS_PATH']:
		setup_external_paths(split_external_paths(os.environ.get(env_var, '')),
							 prepend=True)
	defw_tmp_dir = cdefw_global.get_defw_tmp_dir()
	Path(defw_tmp_dir).mkdir(parents=True, exist_ok=True)

def recurse_dictionary(d, key, value, str_cb):
	global g_yaml_blocks

	if type(value) is dict:
		for k, v in value.items():
			g_yaml_blocks.append(k)
			recurse_dictionary(value, k, v, str_cb)
			g_yaml_blocks.pop()
	elif type(value) is list:
		i = 0
		for l in value:
			if type(l) is dict:
				recurse_dictionary(d, key, l, str_cb)
			elif type(l) is str:
				d[key][i] = str_cb(l)
			i += 1
	elif type(value) is str:
		d[key] = str_cb(value)

def resolve_env_var(s):
	global g_keywords

	segments = s.split("${")
	if len(segments) == 1:
		return s

	resolved = ''
	for seg in segments:
		idx = seg.find('}')
		if idx == -1:
			resolved += seg
		else:
			try:
				var = seg[:idx]
				if var in g_keywords.keys():
					if callable(g_keywords[var]):
						env = g_keywords[var]()
					else:
						env = g_keywords[var]
				else:
					env = os.environ[var]
				resolved += env+seg[idx+1:]
			except:
				resolved += ""
				continue

	return resolved

def set_env_vars(env):
	recurse_dictionary(env, "", env, resolve_env_var)
	for k, v in env.items():
		if k == 'PATH' or k == "LD_LIBRARY_PATH" and k in os.environ:
			os.environ[k] += ":"+resolve_env_var(str(v))
		else:
			os.environ[k] = resolve_env_var(str(v))

def resolve_environment_vars(config):
		# Make sure to set all the environment variables in the block
		# before resolving the env vars in the rest of the block. This way
		# user can set the environment variables then use them in with the
		# syntax ${ENV_VAR}
		if 'environment' in config:
			set_env_vars(config['environment'])

		recurse_dictionary(config, "", config, resolve_env_var)

def configure_defw():
	global defw_path
	global only_load
	global noinit_load
	global defw_config_yaml

	if 'DEFW_DISABLE_RESMGR' in os.environ and \
		os.environ['DEFW_DISABLE_RESMGR'].upper() == 'YES':
			cdefw_global.disable_resmgr()

	if 'DEFW_ONLY_LOAD_MODULE' in os.environ:
		only_load = os.environ['DEFW_ONLY_LOAD_MODULE'].split(',')
	else:
		only_load = []

	if 'DEFW_LOAD_NO_INIT' in os.environ:
		noinit_load = os.environ['DEFW_LOAD_NO_INIT'].split(',')
	else:
		noinit_load = []

	if 'DEFW_PATH' not in os.environ:
		defw_path = os.getcwd()
	else:
		defw_path = os.environ['DEFW_PATH']

	if 'DEFW_CONFIG_PATH' not in os.environ:
		config = os.path.join(defw_path, "python", "config", "defw_generic.yaml")
	else:
		config = os.environ['DEFW_CONFIG_PATH']

	cy = None
	if os.path.isfile(config):
		with open(config, "r") as f:
			cy = yaml.load(f, Loader=yaml.FullLoader)
			resolve_environment_vars(cy)
			defw_config_yaml = cy
			cdefw_global.set_defw_path(cy['defw']['path'])
			if not cdefw_global.resmgr_disabled():
				cdefw_global.set_parent_name(cy['defw']['parent-name'])
				cdefw_global.set_parent_port(int(cy['defw']['parent-port']))
				if 'parent-address' not in cy['defw'] and 'parent-hostname' not in cy['defw']:
					raise DEFwError("No Parent configured for this process. Can not proceed")
				try:
					cdefw_global.set_parent_address(cy['defw']['parent-address'])
				except:
					pass
				try:
					cdefw_global.set_parent_hostname(cy['defw']['parent-hostname'])
				except:
					pass
				cdefw_global.set_hostname(socket.gethostname())
			else:
				cdefw_global.set_parent_name('None')
				cdefw_global.set_parent_port(0)
				cdefw_global.set_parent_address('0.0.0.0')
				cdefw_global.set_parent_hostname('None')
				cdefw_global.set_hostname('None')

			cdefw_global.set_defw_mode(cy['defw']['shell'])
			cdefw_global.set_defw_type(cy['defw']['type'])
			try:
				cdefw_global.set_defw_tmp_dir(cy['defw']['tmp'])
			except:
				pass
			try:
				cdefw_global.set_listen_address(int(cy['defw']['listen-address']))
			except:
				cdefw_global.set_listen_address("")
			try:
				cdefw_global.set_listen_port(int(cy['defw']['listen-port']))
			except:
				if cy['defw']['type'].upper() == 'AGENT':
					cdefw_global.set_listen_port(8091)
				else:
					cdefw_global.set_listen_port(8090)
			try:
				cdefw_global.set_agent_telnet_port(int(cy['defw']['telnet-port']))
			except:
				cdefw_global.set_agent_telnet_port(random.randint(10000,20000))
			try:
				cdefw_global.set_node_name(cy['defw']['name'])
			except:
				cdefw_global.set_node_name(generate_random_string(5))
			try:
				if cy['defw']['loglevel'].upper() == 'ERROR':
					cdefw_global.set_log_level(EN_LOG_LEVEL_ERROR)
				elif cy['defw']['loglevel'].upper() == 'DEBUG':
					cdefw_global.set_log_level(EN_LOG_LEVEL_DEBUG)
				elif cy['defw']['loglevel'].upper() == 'MESSAGE':
					cdefw_global.set_log_level(EN_LOG_LEVEL_MSG)
				elif cy['defw']['loglevel'].upper() == 'ALL':
					cdefw_global.set_log_level(EN_LOG_LEVEL_ALL)
				else:
					cdefw_global.set_log_level(EN_LOG_LEVEL_ERROR)
			except:
				cdefw_global.set_log_level(EN_LOG_LEVEL_ERROR)
			try:
				if cy['defw']['shutdown'] == 'SAFE':
					cdefw_global.set_defw_safe_shutdown(True)
				else:
					cdefw_global.set_defw_safe_shutdown(False)
			except:
				cdefw_global.set_defw_safe_shutdown(False)
				pass
	else:
		raise DEFwError('Failed to find a configuration (%s) file. Aborting' % config)

	return cy

def dump_all_agents():
	agents = [active_service_agents, service_agents, active_client_agents,
			  client_agents]
	for agent_dict in agents:
		agent_dict.dump()

def get_agent(target):
	agents = [active_service_agents, service_agents, active_client_agents,
			  client_agents]
	for agent_dict in agents:
		agent_dict.reload()
		agidx = target.get_id()
		#print(f"Attempting to find "\
		#	  f"{agidx}:{agidx in agent_dict} ")
		#if agidx in agent_dict:
			#print(f"target id: {target.remote_uuid} " \
			#	  f"agent id: {agent_dict[agidx].get_remote_uuid()} " \
			#	  f"target blkuuid: {target.blk_uuid} " \
			#	  f"agent blkuuid: {agent_dict[agidx].get_blk_uuid()}")
		if agidx in agent_dict and \
		   target.remote_uuid == agent_dict[agidx].get_remote_uuid() and \
		   (target.blk_uuid ==  agent_dict[agidx].get_blk_uuid() or \
			target.blk_uuid == str(uuid.UUID(int=0))):
			#print(f"Returning {agidx}:{agent_dict[agidx]}")
			return agent_dict[agidx]
	#print(f"get_agent didn't find {target}")
	return None

def updater_thread():
	global resmgr

	shutdown = False
	while not shutdown:
		try:
			event = updater_queue.get(timeout = 1)
			if event['type'] == 'shutdown':
				shutdown = True
				continue
			if event['type'] == 'resmgr':
				cdefw_global.update_py_interactive_shell()
		except queue.Empty:
			continue

def connect_to_services(endpoints):
	for ep in endpoints:
		active_service_agents.connect(ep)
		logging.debug(f"Connection request finished: {ep}")

def connect_to_resource(service_infos, res_name):
	ep = resmgr.reserve(me.my_endpoint(), service_infos)
	connect_to_services(ep)
	apis = []
	for service_info in service_infos:
		class_obj = getattr(service_apis[res_name], res_name)
		api = class_obj(service_info)
		logging.debug(f"API created: {res_name}: {api}")
		apis.append(api)

	logging.debug(f"Returning API array: {apis}")
	return apis

def wait_resmgr(timeout):
	global resmgr

	wait = 0
	if not resmgr:
		while wait < timeout:
			if resmgr:
				return True
			wait += 1
			logging.debug("waiting to connect to resource manager")
			time.sleep(1)
	else:
		return True

	return False

# TODO: We need a way to disconnect endpoint

def get_resmgr():
	return resmgr

def get_self():
	return me

if not cdefw_global.get_defw_initialized():
	updater_queue = queue.Queue()

	defw_cfg = configure_defw()

	setup_log_file()
	setup_log_levels()

	setup_paths()

	# All test results are stored in here
	# Access functions can be used to dump it.
	global_test_results = YamlGlobalTestResults()

	client_agents = DEFwClientAgents()
	service_agents = DEFwServiceAgents()
	active_client_agents = DEFwActiveClientAgents()
	active_service_agents = DEFwActiveServiceAgents()

	# Create an instance of the resource manager because we have
	# a connection to it.

	logging.debug("INSTANTIATING myself")
	me = Myself(defw_cfg)

	# build up a database of all the icpa back ends
	#    The intent here is to ensure that when we get remote requests
	#    we're able to instantiate the objects defined in these icpa's.
	# build up a database of all the icpa front ends
	experiments = ExpSuites()
	services = ServiceSuites()
	service_apis = ServiceSuiteAPIs()

	if me.is_resmgr():
		if 'Resource Manager' in services:
			if 'DEFW_SQL_PATH' in os.environ:
				sql_path = os.enviorn['DEFW_SQL_PATH']
			else:
				sql_path = '/tmp'
			resmgr = services['Resource Manager'].service_classes[0](sql_path)

	# Convenience Variables
	R = dumpGlobalTestResults
	C = client_agents.dump
	S = service_agents.dump
	AC = active_client_agents.dump
	AS = active_service_agents.dump
	I = me.dump_intfs
	X = me.exit

	preferences = load_pref()
	# set debug level
	#set_logging_level('debug')

	updater_thread = threading.Thread(target=updater_thread, args=())
	updater_thread.daemon = True
	updater_thread.start()

	builtins.exit = me.exit

	def sigkill_handler(signum, frame):
		logging.critical("DEFw received a SIGKILL")
		me.exit()

	signal.signal(signal.SIGABRT, sigkill_handler)

	cdefw_global.set_defw_initialized(True)
