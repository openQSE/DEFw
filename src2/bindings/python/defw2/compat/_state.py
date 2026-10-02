"""The one v2 runtime a compat process has, and what hangs off it.

v1 started its runtime when the process started. compat starts a v2 one
the first time something needs it, as a client, or as a server when the
process serves, and closes everything at exit in an order that is safe:
the completion collectors first, then the remote QPMs, the directory, the
service and its registration, and the runtime last.
"""

import atexit
import logging
import os
import re
import threading

from .._runtime import Runtime

log = logging.getLogger('defw2.compat')

lock = threading.RLock()
role = 'client'
node_name = None
host = None
_runtime = None
_directory = None
_endpoint = None
_closers = []
_closed = False

# Set by me.exit(), which a served v1 service calls to stop its process.
stopping = threading.Event()


def runtime():
	global _runtime
	with lock:
		if _closed:
			raise RuntimeError('defw2.compat has shut down')
		if _runtime is None:
			_runtime = Runtime(role=role, node_name=node_name or
					   os.environ.get('DEFW_AGENT_NAME'))
			atexit.register(close)
			log.info('runtime %s at %s', _runtime.runtime_id,
				 _runtime.address)
		return _runtime


def started():
	return _runtime is not None


def on_close(closer):
	"""Run closer at shutdown, before the runtime closes; last in, first
	out."""
	with lock:
		_closers.append(closer)


def directory():
	"""v1's dirsvc for this process, or None when it was told of none."""
	global _directory
	with lock:
		if _directory is None:
			rt = runtime()
			if not rt.dirsvc:
				return None
			from ._directory import Directory
			_directory = Directory(rt, rt.dirsvc)
			on_close(_directory.close)
		return _directory


class Endpoint:
	"""This process as v1 saw it: the fields v1 code reads off an endpoint.

	A v2 address has no port, so listen_port is the port of a TCP address
	and the process ID otherwise, because v1 records insist on one.
	"""

	def __init__(self, rt):
		self.addr = rt.address
		match = re.search(r':(\d+)$', self.addr or '')
		self.pid = os.getpid()
		self.listen_port = int(match.group(1)) if match else self.pid
		self.name = rt.node_name
		self.hostname = rt.hostname
		self.remote_uuid = rt.runtime_id
		self.blk_uuid = rt.runtime_id

	def get_id(self):
		return self.remote_uuid

	def get(self):
		return {'name': self.name, 'hostname': self.hostname,
			'addr': self.addr, 'listen_port': self.listen_port,
			'pid': self.pid, 'remote uuid': self.remote_uuid}

	def __repr__(self):
		return 'Endpoint({})'.format(self.get())


def endpoint():
	global _endpoint
	with lock:
		if _endpoint is None:
			_endpoint = Endpoint(runtime())
		return _endpoint


def close():
	"""Shut everything down, once."""
	global _runtime, _directory, _closed, host
	with lock:
		if _closed:
			return
		_closed = True
		closers = list(reversed(_closers))
		_closers.clear()
	stopping.set()
	for closer in closers:
		try:
			closer()
		except Exception:  # noqa: BLE001
			log.exception('closing %s', closer)
	if host is not None:
		host.close()
		host = None
	if _runtime is not None:
		_runtime.close()
		_runtime = None
	_directory = None


def exit():
	"""What v1's me.exit() did: stop this process's DEFw and leave.

	From the main thread that is everything, then SystemExit, as v1 did.
	From another thread, such as a service's shutdown thread, it stops the
	service, and the main thread closes the rest.
	"""
	stopping.set()
	if threading.current_thread() is threading.main_thread():
		close()
	raise SystemExit
