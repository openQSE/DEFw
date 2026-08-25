import atexit
import logging
import os
import signal
import subprocess
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

import cdefw_global
import defw
import defw_peers
from cdefw_agent import EN_DEFW_SERVICE
from defw_agent import Endpoint
from defw_exception import DEFwError, DEFwReserveError

SYSTEM_UP_TIMEOUT = 40
_PORT_LOCK = threading.Lock()
_NEXT_PORT = None
_SPAWNED_SERVICES = []


@dataclass
class DEFwServiceProcess:
	module: str
	agent_name: str
	listen_port: int
	telnet_port: int
	log_dir: str
	pid: int
	process: subprocess.Popen
	stdout_path: str
	stderr_path: str
	stdout_handle: object
	stderr_handle: object
	service_endpoint: object = None
	directory_records: object = None

	def shutdown(self, timeout=5):
		self.deregister()
		if self.pid > 0:
			try:
				os.kill(self.pid, signal.SIGTERM)
			except ProcessLookupError:
				self.pid = 0
			start = time.time()
			while self.pid > 0 and time.time() - start < timeout:
				try:
					os.kill(self.pid, 0)
				except ProcessLookupError:
					self.pid = 0
					break
				time.sleep(0.1)
			if self.pid > 0:
				try:
					os.kill(self.pid, signal.SIGKILL)
				except ProcessLookupError:
					pass
				self.pid = 0
		if self.process.poll() is None:
			self.process.wait(timeout=timeout)
		self.stdout_handle.close()
		self.stderr_handle.close()

	def deregister(self):
		if not self.directory_records:
			return
		try:
			dirsvc = defw_get_directory_service(timeout=1)
			for record in list(self.directory_records):
				dirsvc.deregister_service(
					record['service_id'],
					record['runtime_id'],
					record['generation'],
				)
		except Exception:
			logging.defw_stacktrace(
				f"Failed to deregister spawned service {self.agent_name}",
				exc_info=True,
			)
		finally:
			self.directory_records = []


def _normalize_service_specs(services):
	if isinstance(services, str):
		return [{'module': services}]
	if isinstance(services, dict):
		return [services]
	if isinstance(services, list):
		normalized = []
		for entry in services:
			if isinstance(entry, str):
				normalized.append({'module': entry})
			elif isinstance(entry, dict):
				normalized.append(entry)
			else:
				raise DEFwError(f"Unsupported service entry: {entry}")
		return normalized
	raise DEFwError(f"Unsupported service specification: {services}")


def _allocate_ports(count=2):
	global _NEXT_PORT

	with _PORT_LOCK:
		if _NEXT_PORT is None:
			base = int(os.environ.get(
				'DEFW_EXPERIMENT_PORT_BASE',
				str(defw.me.my_listenport() + 10),
			))
			_NEXT_PORT = base
		start = _NEXT_PORT
		_NEXT_PORT += count
	return list(range(start, start + count))


def _build_service_env(service_spec):
	module = service_spec['module']
	agent_name = service_spec.get(
		'agent_name',
		f"{module}-{str(uuid.uuid4())[:8]}",
	)
	listen_port, telnet_port = _allocate_ports()
	log_dir = os.path.join(cdefw_global.get_defw_tmp_dir(), agent_name)
	Path(log_dir).mkdir(parents=True, exist_ok=True)

	env = os.environ.copy()
	defw_path = cdefw_global.get_defw_path()
	config_path = os.environ.get('DEFW_CONFIG_PATH') or os.path.join(
		defw_path, 'share', 'defw', 'config', 'defw_generic.yaml')
	env.update({
		'DEFW_PATH': defw_path,
		'DEFW_CONFIG_PATH': config_path,
		'DEFW_AGENT_NAME': agent_name,
		'DEFW_AGENT_TYPE': 'service',
		'DEFW_SHELL_TYPE': 'daemon',
		'DEFW_LISTEN_PORT': str(listen_port),
		'DEFW_TELNET_PORT': str(telnet_port),
		'DEFW_PARENT_NAME': defw.me.my_name(),
		'DEFW_PARENT_HOSTNAME': defw.me.my_hostname(),
		'DEFW_PARENT_ADDR': defw.me.my_listenaddress(),
		'DEFW_PARENT_PORT': str(defw.me.my_listenport()),
		'DEFW_ONLY_LOAD_MODULE': module,
		'DEFW_LOG_DIR': log_dir,
	})
	if 'env' in service_spec and service_spec['env']:
		env.update({k: str(v) for k, v in service_spec['env'].items()})
	return env, agent_name, listen_port, telnet_port, log_dir


def _wait_for_daemon_pid(log_dir, timeout=5):
	pid_path = os.path.join(log_dir, 'pid')
	start = time.time()
	while time.time() - start < timeout:
		if os.path.isfile(pid_path):
			with open(pid_path, 'r', encoding='utf-8') as handle:
				return int(handle.read().strip())
		time.sleep(0.1)
	raise DEFwError(f"Timed out waiting for daemon pid in {log_dir}")


def _resolve_defwp():
	override = os.environ.get('DEFW_EXECUTABLE')
	if override:
		path = Path(override).resolve()
		if path.is_file() and os.access(path, os.X_OK):
			return str(path)
		raise DEFwError(f"DEFW_EXECUTABLE is not executable: {path}")

	defw_path = Path(cdefw_global.get_defw_path())
	for candidate in [
		defw_path / 'src' / 'defwp',
		defw_path / 'bin' / 'defwp',
		defw_path / 'src' / 'defwp-wrapper',
		defw_path / 'bin' / 'defwp-wrapper',
	]:
		if candidate.is_file() and os.access(candidate, os.X_OK):
			return str(candidate)
	raise DEFwError(f"Unable to find defwp under {defw_path}")


def _endpoint_from_peer_record(record, agent_name, listen_port):
	if not record.get('callable'):
		return None
	endpoint = record.get('endpoint') or {}
	if endpoint.get('node_name') != agent_name:
		return None
	try:
		if int(endpoint.get('listen_port', 0)) != int(listen_port):
			return None
	except (TypeError, ValueError):
		return None
	runtime_id = record.get('runtime_id') or endpoint.get('runtime_id')
	peer_handle = record.get('peer_handle')
	if not runtime_id or not peer_handle:
		return None
	return Endpoint(
		endpoint.get('address', ''),
		endpoint.get('port', 0),
		endpoint.get('listen_port', 0),
		endpoint.get('pid', 0),
		endpoint.get('node_name', agent_name),
		endpoint.get('hostname', ''),
		endpoint.get('node_type') or record.get('node_type') or
		EN_DEFW_SERVICE,
		runtime_id,
		blk_uuid=peer_handle,
	)


def _wait_for_service_endpoint(agent_name, listen_port,
			       timeout=SYSTEM_UP_TIMEOUT):
	deadline = time.time() + timeout
	while time.time() < deadline:
		for record in defw_peers.snapshot().values():
			endpoint = _endpoint_from_peer_record(
				record, agent_name, listen_port)
			if endpoint:
				return endpoint
		time.sleep(0.1)
	raise DEFwReserveError(
		f"Timed out waiting for service {agent_name} to connect")


def _register_spawned_service(agent_name, listen_port):
	service_endpoint = _wait_for_service_endpoint(agent_name, listen_port)
	dirsvc = defw_get_directory_service()
	return service_endpoint, dirsvc.register_service(service_endpoint)


def defw_spawn_services(services):
	specs = _normalize_service_specs(services)
	defwp = _resolve_defwp()
	spawned = []

	for spec in specs:
		env, agent_name, listen_port, telnet_port, log_dir = _build_service_env(spec)
		stdout_path = os.path.join(log_dir, 'stdout.log')
		stderr_path = os.path.join(log_dir, 'stderr.log')
		stdout_handle = open(stdout_path, 'w', encoding='utf-8')
		stderr_handle = open(stderr_path, 'w', encoding='utf-8')
		process = subprocess.Popen(
			[defwp, '-d'],
			env=env,
			stdout=stdout_handle,
			stderr=stderr_handle,
			start_new_session=True,
		)
		try:
			pid = _wait_for_daemon_pid(log_dir)
		except Exception:
			process.wait(timeout=5)
			stdout_handle.close()
			stderr_handle.close()
			raise DEFwError(
				f"Service {spec['module']} exited early with rc={process.returncode}"
			)
		process.wait(timeout=5)
		handle = DEFwServiceProcess(
			module=spec['module'],
			agent_name=agent_name,
			listen_port=listen_port,
			telnet_port=telnet_port,
			log_dir=log_dir,
			pid=pid,
			process=process,
			stdout_path=stdout_path,
			stderr_path=stderr_path,
			stdout_handle=stdout_handle,
			stderr_handle=stderr_handle,
		)
		try:
			handle.service_endpoint, handle.directory_records = (
				_register_spawned_service(agent_name, listen_port)
			)
		except Exception:
			handle.shutdown()
			raise
		_SPAWNED_SERVICES.append(handle)
		spawned.append(handle)

	return spawned


def defw_shutdown_services(services=None, timeout=5):
	targets = list(services) if services is not None else list(_SPAWNED_SERVICES)
	for handle in reversed(targets):
		try:
			handle.shutdown(timeout=timeout)
		finally:
			if handle in _SPAWNED_SERVICES:
				_SPAWNED_SERVICES.remove(handle)


def _shutdown_spawned_services():
	try:
		defw_shutdown_services()
	except Exception:
		logging.defw_stacktrace(
			"Failed to shut down spawned DEFw services",
			exc_info=True,
		)


atexit.register(_shutdown_spawned_services)


def defw_get_directory_service(timeout=SYSTEM_UP_TIMEOUT):
	if not defw.wait_dirsvc(timeout):
		logging.defw_app("Couldn't find a directory service")
		raise DEFwReserveError("Couldn't find a directory service")

	return defw.dirsvc


def defw_connect_service_by_name(dirsvc, service_name,
				 timeout=SYSTEM_UP_TIMEOUT,
				 service_type=None,
				 binding_name=None,
				 selector_resource=None,
				 selector_alias=None,
				 properties=None):
	wait = 0
	bindings = []
	filters = {'service_name': service_name}
	for key, value in (
			('service_type', service_type),
			('binding_name', binding_name),
			('selector_resource', selector_resource),
			('selector_alias', selector_alias),
			('properties', properties),
	):
		if value:
			filters[key] = value
	while wait < timeout:
		bindings = dirsvc.resolve_services(**filters)
		if bindings and len(bindings) > 0:
			break
		wait += 1
		logging.defw_app(f"Waiting to connect to {service_name}")
		time.sleep(1)

	if len(bindings) == 0:
		raise DEFwReserveError(
			f"Couldn't connect to a {service_name}")

	logging.defw_app(f"Received directory bindings: {bindings}")

	return [defw.connect_to_binding(binding) for binding in bindings]
