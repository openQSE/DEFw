"""v1's cdefw_global, the C runtime's globals, on DEFw v2.

v1's is a SWIG module over its C library. What v1's Python reads from it
is where the process keeps temporary files, its node name and the DEFw
path, which here come from the same environment v1 read them from. A
setter keeps what it is given, so reading it back gives the same value.
"""

import os
import socket

_values = {}


def _agent_name():
	return os.environ.get('DEFW_AGENT_NAME') or socket.gethostname()


def get_defw_tmp_dir():
	path = _values.get('tmp_dir') or os.environ.get('DEFW_LOG_DIR') or \
		os.path.join('/tmp', _agent_name())
	os.makedirs(path, exist_ok=True)
	return path


def set_defw_tmp_dir(path):
	_values['tmp_dir'] = path


def get_node_name():
	return _values.get('node_name') or _agent_name()


def set_node_name(name):
	_values['node_name'] = name


def get_defw_path():
	return os.environ.get('DEFW_PATH', '')


def get_defw_initialized():
	return True


def __getattr__(name):
	raise AttributeError(
		"defw2.compat's cdefw_global has no {}: v1's C runtime is not on "
		'v2'.format(name))
