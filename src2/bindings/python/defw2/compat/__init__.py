"""v1 Python code, unchanged, on DEFw v2.

v1 ran a Python script inside its own launcher, defw-python, which put the
v1 modules on the path and started the v1 runtime before the script ran.
defw2-python does the same for v2. It makes the v1 module names importable,
then runs the script, so QFw's QPM services and its Qiskit backend run on
v2 without an edit:

	defw2-python script.py ARGS            a v1 client, such as an app
	defw2-python --serve svc_fake_iqm_qpm  a v1 QPM service module

The v1 names come from three places.

- cdefw_global, defw, defw_app_util, defw_remote and defw_workers are
  compat's own, in _v1. They are v1's runtime: the process (defw.me), the
  directory (defw.dirsvc), remote objects (defw.connect_to_binding and
  defw_remote.BaseRemote) and peer events. Each says what it is on v2.
- api_events, defw_cmd, defw_common_def, defw_event_baseapi,
  defw_exception, defw_trace, defw_util and svc_launcher are v1's own
  modules, loaded unchanged from the v1 tree, because none of them touches
  the v1 runtime. A v1 exception raised here is v1's own class.
- Every other v1 module name fails to import, with an error that says so,
  rather than loading v1's runtime beside v2's.

A remote QPM's fifteen typed methods go over the typed QPM APIs. Any
other remote method fails, naming the method, until v2 types it. Every
compat process listens, and v1's events, a QPM's completions and the
directory's changes, reach the caller's own queue through one sink.
_remote has the remote objects, _events the events, and _mapping how a v1
dictionary crosses the typed APIs and comes back the same.
"""

import importlib.abc
import importlib.util
import logging
import os
import sys

__all__ = ['install', 'installed', 'v1_root', 'SHIMS', 'V1_OWN']

log = logging.getLogger('defw2.compat')

# v1 names compat provides itself.
SHIMS = ('cdefw_global', 'defw', 'defw_app_util', 'defw_remote',
	 'defw_workers')

# v1 modules loaded unchanged from the v1 tree.
V1_OWN = ('api_events', 'defw_cmd', 'defw_common_def', 'defw_event_baseapi',
	  'defw_exception', 'defw_trace', 'defw_util', 'svc_launcher')

# Where v1 keeps its Python, under its root, and the paths v1's launcher
# added from the environment, as v1's setup_paths did.
_V1_DIRS = ('infra', 'service-apis', 'services')
_EXTERNAL_PATHS = ('DEFW_EXTERNAL_SERVICE_APIS_PATH',
		   'DEFW_EXTERNAL_SERVICES_PATH',
		   'DEFW_EXTERNAL_EXPERIMENTS_PATH')

_HERE = os.path.dirname(os.path.abspath(__file__))
_finder = None


def v1_root():
	"""v1's Python tree: $DEFW_PATH/python, or this source tree's."""
	candidates = []
	if os.environ.get('DEFW_PATH'):
		candidates.append(os.path.join(os.environ['DEFW_PATH'], 'python'))
	# The source tree, src2/bindings/python/defw2/compat, is five levels
	# below the repository's python directory.
	candidates.append(os.path.normpath(os.path.join(
		_HERE, *(['..'] * 5), 'python')))
	for path in candidates:
		if os.path.isdir(os.path.join(path, 'infra')):
			return path
	return None


def _module_file(root, name):
	"""(path, package directory or None) of a v1 module in the v1 tree."""
	for sub in _V1_DIRS:
		base = os.path.join(root, sub, name)
		if os.path.isfile(os.path.join(base, '__init__.py')):
			return os.path.join(base, '__init__.py'), base
		if os.path.isfile(base + '.py'):
			return base + '.py', None
	return None, None


def _v1_names(root):
	"""Every top-level module name the v1 tree defines."""
	names = set()
	for sub in _V1_DIRS:
		path = os.path.join(root, sub)
		if not os.path.isdir(path):
			continue
		for entry in os.listdir(path):
			full = os.path.join(path, entry)
			if entry.endswith('.py'):
				names.add(entry[:-3])
			elif os.path.isfile(os.path.join(full, '__init__.py')):
				names.add(entry)
	return names


class _Finder(importlib.abc.MetaPathFinder):
	"""Answers for the v1 module names, ahead of everything on sys.path.

	It has to come first: QFw's environment puts v1's infra directory on
	PYTHONPATH, where v1's defw would start v1's runtime.
	"""

	def __init__(self, root):
		self.root = root
		self.v1_names = _v1_names(root)
		self.refused = set()

	def find_spec(self, name, path=None, target=None):
		if '.' in name:
			return None
		if name in SHIMS:
			return importlib.util.spec_from_file_location(
				name, os.path.join(_HERE, '_v1', name + '.py'))
		if name in V1_OWN:
			filename, package = _module_file(self.root, name)
			if filename is None:
				raise ImportError(
					"defw2.compat: v1's {} is not in {}".format(
						name, self.root), name=name)
			return importlib.util.spec_from_file_location(
				name, filename,
				submodule_search_locations=(
					[package] if package else None))
		if name in self.v1_names or name.startswith('cdefw') or \
		   name.startswith('_cdefw'):
			if name not in self.refused:
				self.refused.add(name)
				log.warning('refused v1 module %s, which has no '
					    'v2 counterpart', name)
			raise ImportError(
				"defw2.compat: v1's {} has no v2 counterpart, so it "
				"cannot be imported on v2".format(name), name=name)
		return None


def installed():
	return _finder is not None


def install(root=None):
	"""Make the v1 names importable in this process.

	Call it before anything imports a v1 module; defw2-python does. It
	also sets up v1's logging, as importing v1's defw did, so the DEFw
	log levels exist and go where v1 sent them.
	"""
	global _finder
	if _finder is not None:
		return
	root = root or v1_root()
	if root is None:
		raise RuntimeError(
			'defw2.compat needs the v1 Python tree; set DEFW_PATH to '
			'the DEFw installation')
	for variable in _EXTERNAL_PATHS:
		paths = [p for p in os.environ.get(variable, '').split(':') if p]
		for path in reversed(paths):
			if path in sys.path:
				sys.path.remove(path)
			sys.path.insert(0, path)
	for name in SHIMS + V1_OWN:
		loaded = sys.modules.get(name)
		if loaded is not None:
			raise RuntimeError(
				'defw2.compat must be installed before {} is '
				'imported, and {} already is'.format(name, loaded))
	_finder = _Finder(root)
	sys.meta_path.insert(0, _finder)

	import defw_common_def
	defw_common_def.setup_log_file()
	defw_common_def.setup_log_levels()
	defw_common_def.load_pref()
	log.info('v1 names from %s, runtime on DEFw v2', root)
