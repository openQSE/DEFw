import copy
import logging
import os
import shlex
import socket
import subprocess
import sys
import threading
from defw_exception import DEFwInProgress
sys.path.append(os.path.split(os.path.abspath(__file__))[0])
from defw_cmd import defw_exec_remote_cmd

class Process:
	def __init__(self, cmd, env, path):
		self.__cmd = shlex.split(cmd)
		if not self.__cmd:
			raise ValueError("process command must not be empty")
		if path:
			self.__cmd[0] = os.path.join(path, self.__cmd[0])
		self.__pid = 0
		self.__process = None
		self.__appended_env = env
		self.__env = copy.deepcopy(dict(os.environ))
		if env and type(env) == dict:
			for k, v in env.items():
				self.__env[k] = v

	def __str__(self):
		return f"Process(pid={self.__pid}, {self.__process}, env={self.__appended_env})"

	def __repr__(self):
		return f"Process(pid={self.__pid}, {self.__process}, env={self.__appended_env})"

	def launch(self):
		try:
			self.__process = subprocess.Popen(self.__cmd, env=self.__env,
							stdout=subprocess.PIPE, stderr=subprocess.PIPE,
							stdin=subprocess.PIPE, start_new_session=True)
			self.__pid = self.__process.pid
		except Exception as e:
			logging.defw_service(f"hit exception: {e}")
			raise e
		return self.__pid

	def get_result(self):
		output, error = self.__process.communicate()
		return output, error, self.__process.returncode

	def kill(self):
		logging.defw_service(f"Kill process with pid: {self.__pid}")
		self.__process.kill()
		self.__process.wait()

	def terminate(self):
		logging.defw_service(f"Terminate process with pid: {self.__pid}")
		self.__process.terminate()
		self.__process.wait()

	def poll(self):
		return self.__process.poll()

	def returncode(self):
		return self.__process.returncode

	def getpid(self):
		return self.__pid

class Launcher:
	def __init__(self, start=True):
		# No thread runs until launch() starts one for each process, so
		# start has no effect. The service loader still passes it.
		self.__proc_dict = {}
		self.__dead_procs = {}
		self.__lock_db = threading.Lock()

	def __wait_for_exit(self, pid, proc):
		# communicate() reads both pipes while the process runs, so a
		# process that writes more than a pipe buffer can still exit.
		stdout, stderr, rc = proc.get_result()
		logging.defw_service(f"{pid} terminated with rc {rc}")
		with self.__lock_db:
			# kill(), terminate() and shutdown() remove the process
			# first, and nothing asks for its status afterwards.
			if self.__proc_dict.get(pid) is proc:
				del self.__proc_dict[pid]
				self.__dead_procs[pid] = (stdout, stderr, rc)

	def compose_remote_cmd(self, exe, env, use, modules, python_env):
		cmd = ''
		if env:
			cmd = "; ".join([f"export {var_name}={var_value}" \
				for var_name, var_value in env.items()]) + ';'
		if use and modules:
			for u in use.split(':'):
				cmd += f"module use {u};"
			for m in modules.split(':'):
				cmd += f"module load {m};"
		if python_env:
			cmd += f"source {python_env};"
		if cmd:
			cmd += f" {exe}"
		else:
			cmd = exe
		return cmd

	def run_cmd_on_target(self, exe, env, use, modules, python_env, target):
		rcmd = self.compose_remote_cmd(exe, env, use, modules, python_env)
		defw_exec_remote_cmd(rcmd, target, deamonize=True)

	def launch(self, cmd, env=None, path='', wait=False,
			   target=None, muse='', modules='', python_env=''):
		logging.defw_service(f"Starting {cmd} on {target}")
		if target and target != socket.gethostname():
			self.run_cmd_on_target(cmd, env, muse, modules, python_env, target)
			return 0
		proc = Process(cmd, env, path)
		proc.launch()
		pid = proc.getpid()
		with self.__lock_db:
			self.__proc_dict[pid] = proc
		if wait:
			output, error, rc = proc.get_result()
			with self.__lock_db:
				if self.__proc_dict.get(pid) is proc:
					del self.__proc_dict[pid]
			return output, error, rc
		waiter = threading.Thread(target=self.__wait_for_exit,
					  args=(pid, proc), daemon=True)
		try:
			waiter.start()
		except Exception:
			self.kill(pid)
			raise
		return pid

	def kill(self, pid):
		with self.__lock_db:
			if pid in self.__proc_dict.keys():
				self.__proc_dict[pid].kill()
				del self.__proc_dict[pid]

	def terminate(self, pid):
		with self.__lock_db:
			if pid in self.__proc_dict.keys():
				self.__proc_dict[pid].kill()
				del self.__proc_dict[pid]

	def status(self, pid):
		with self.__lock_db:
			if pid in self.__dead_procs.keys():
				rc = self.__dead_procs[pid]
				del self.__dead_procs[pid]
				return rc
		raise DEFwInProgress(f"{pid} is still running")

	def shutdown(self, keep=False):
		rm_pid = []
		if not keep:
			with self.__lock_db:
				for pid, proc in self.__proc_dict.items():
					proc.kill()
					rm_pid.append(pid)
				for pid in rm_pid:
					del self.__proc_dict[pid]
		logging.defw_service("Launcher Service shutdown requested")

	def query(self):
		from . import svc_info
		return {
			'service_name': svc_info['name'],
			'service_type': 'defw.launcher',
			'api_bindings': [{
				'binding_name': 'default',
				'client_module': 'api_launcher',
				'client_class': 'Launcher',
				'service_module': self.__class__.__module__,
				'service_class': self.__class__.__name__,
				'version': 1,
			}],
			'selector': {'resources': [svc_info['name']]},
			'properties': {'description': svc_info['description']},
			'capability': {
				'type': 1,
				'caps': 1,
				'description': svc_info['description'],
			},
		}

	def test(self):
		logging.defw_service("Testing Launcher")
