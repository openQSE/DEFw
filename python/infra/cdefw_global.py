"""A process's DEFw globals: where it keeps temporary files, its node name
and the DEFw path.

DEFw v1 kept these in its C library and read them through a SWIG module of
this name. DEFw's Python still reads them here, in a defw2-python process
and in plain Python alike, and they come from the environment v1 read them
from. A setter keeps what it is given, so reading it back gives the same
value.
"""

import os
import socket
import tempfile

_values = {}


def _agent_name():
	return os.environ.get('DEFW_AGENT_NAME') or socket.gethostname()


def get_defw_tmp_dir():
	"""Where this process keeps its files: what it was told, then
	DEFW_LOG_DIR, then a directory under /tmp.

	v1 kept them under /tmp in a directory named for the agent, and
	named an agent with no DEFW_AGENT_NAME defw_python_ and eight hex
	digits, so two processes never shared a directory. An unnamed
	agent now goes by its host's name, which every process on the
	host shares. The first user to run a client would own that
	directory, and the next user's client could not write its log
	there. So an unnamed agent gets a directory of its own, with v1's
	prefix.
	"""
	path = _values.get('tmp_dir') or os.environ.get('DEFW_LOG_DIR')
	if not path:
		name = os.environ.get('DEFW_AGENT_NAME')
		if not name:
			_values['tmp_dir'] = tempfile.mkdtemp(
				prefix='defw_python_', dir='/tmp')
			return _values['tmp_dir']
		path = os.path.join('/tmp', name)
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
		'cdefw_global has no {}: DEFw v1\'s C runtime, which had it, is '
		'gone'.format(name))
