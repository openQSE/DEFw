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
from time import sleep

import cdefw_global
import defw
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
	process: subprocess.Popen
	stdout_path: str
	stderr_path: str
	stdout_handle: object
	stderr_handle: object

	def shutdown(self, timeout=5):
		if self.process.poll() is None:
			self.process.terminate()
			try:
				self.process.wait(timeout=timeout)
			except subprocess.TimeoutExpired:
				self.process.kill()
				self.process.wait(timeout=timeout)
		self.stdout_handle.close()
		self.stderr_handle.close()


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
			base = int(os.environ.get('DEFW_EXPERIMENT_PORT_BASE', '28000'))
			_NEXT_PORT = max(base, defw.me.my_listenport() + 10)
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
	env.update({
		'DEFW_PATH': cdefw_global.get_defw_path(),
		'DEFW_CONFIG_PATH': os.path.join(
			cdefw_global.get_defw_path(),
			'python',
			'config',
			'defw_generic.yaml',
		),
		'DEFW_AGENT_NAME': agent_name,
		'DEFW_AGENT_TYPE': 'agent',
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


def defw_spawn_services(services):
	specs = _normalize_service_specs(services)
	defwp = os.path.join(cdefw_global.get_defw_path(), 'src', 'defwp')
	spawned = []

	for spec in specs:
		env, agent_name, listen_port, telnet_port, log_dir = _build_service_env(spec)
		stdout_path = os.path.join(log_dir, 'stdout.log')
		stderr_path = os.path.join(log_dir, 'stderr.log')
		stdout_handle = open(stdout_path, 'w', encoding='utf-8')
		stderr_handle = open(stderr_path, 'w', encoding='utf-8')
		process = subprocess.Popen(
			[defwp],
			env=env,
			stdout=stdout_handle,
			stderr=stderr_handle,
			start_new_session=True,
		)
		time.sleep(0.2)
		if process.poll() is not None:
			stdout_handle.close()
			stderr_handle.close()
			raise DEFwError(
				f"Service {spec['module']} exited early with rc={process.returncode}"
			)
		handle = DEFwServiceProcess(
			module=spec['module'],
			agent_name=agent_name,
			listen_port=listen_port,
			telnet_port=telnet_port,
			log_dir=log_dir,
			process=process,
			stdout_path=stdout_path,
			stderr_path=stderr_path,
			stdout_handle=stdout_handle,
			stderr_handle=stderr_handle,
		)
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
		logging.exception("Failed to shut down spawned DEFw services")


atexit.register(_shutdown_spawned_services)


def defw_get_resource_mgr(timeout=SYSTEM_UP_TIMEOUT):
	if not defw.wait_resmgr(timeout):
		logging.debug("Couldn't find a resmgr")
		raise DEFwReserveError("Couldn't find a resmgr")

	return defw.resmgr


def defw_reserve_service_by_name(resmgr, svc_name, svc_type=-1,
								 svc_cap=-1, timeout=SYSTEM_UP_TIMEOUT):
	wait = 0
	while wait < timeout:
		service_infos = resmgr.get_services(svc_name, svc_type, svc_cap)
		if service_infos and len(service_infos) > 0:
			break
		wait += 1
		logging.debug(f"Waiting to connect to {svc_name}")
		sleep(1)

	if len(service_infos) == 0:
		raise DEFwReserveError(f"Couldn't connect to a {svc_name}, {svc_type}, {svc_cap}")

	logging.debug(f"Received service_infos: {service_infos}")

	svc_apis = defw.connect_to_resource(service_infos, svc_name)

	return svc_apis
