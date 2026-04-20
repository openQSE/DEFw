from defw_agent_info import *
from defw_util import prformat, fg, bg
from defw import me
import os, subprocess, copy, yaml, logging, sys, threading, socket, psutil, traceback
from time import sleep
from defw_exception import DEFwError, DEFwInProgress
sys.path.append(os.path.split(os.path.abspath(__file__))[0])
import launcher_common as common
from defw_cmd import defw_exec_remote_cmd

class Process:
	def __init__(self, cmd, env, path):
		if path:
			self.__cmd = os.path.join(path, proc).split()
		else:
			self.__cmd = cmd.split()
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
		try:
			os.waitpid(self.__pid, 0)
		except:
			pass

	def terminate(self):
		logging.defw_service(f"Terminate process with pid: {self.__pid}")
		self.__process.terminate()
		try:
			os.waitpid(self.__pid, 0)
		except:
			pass

	def poll(self):
		return self.__process.poll()

	def returncode(self):
		return self.__process.returncode

	def getpid(self):
		return self.__pid

class Launcher:
	def __init__(self, start=False):
		self.__proc_dict = {}
		self.__dead_procs = {}
		self.__shutdown = False
		self.__lock_db = threading.Lock()
		self.__monitor_thr = threading.Thread(target=self.monitor_thr)
		self.__monitor_thr.daemon = True
		self.__monitor_thr.start()

	def monitor_thr(self):
		while not self.__shutdown:
			with self.__lock_db:
				for pid, proc in self.__proc_dict.items():
					exists = psutil.pid_exists(pid)
					if proc.poll() is not None:
						logging.defw_service(f"{pid} terminated with rc {proc.returncode()}")
						stdout, stderr, rc = proc.get_result()
						proc.kill()
						self.__dead_procs[pid] = (stdout, stderr, rc)
				for pid in self.__dead_procs.keys():
					if pid in self.__proc_dict.keys():
						del self.__proc_dict[pid]
			sleep(0.0001)
		logging.defw_service("Monitor thread shutdown")

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
		# if we're going to wait for it keep it around until we get
		# the result
		proc.launch()
		pid = proc.getpid()
		with self.__lock_db:
			self.__proc_dict[pid] = proc
		if not wait:
			return pid
		psutilproc = psutil.Process(pid)
		output, error, rc = proc.get_result()
		proc.kill()
		return output, error, rc

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
		self.__shutdown = True

	def blocking_wait(self, pid=-1):
		while True:
			with self.__lock_db:
				if pid == -1:
					if len(self.__proc_dict) == 0:
						break;
					rm_pid = []
					for pid, proc in self.__proc_dict.items():
						if proc.poll():
							rm_pid.append(pid)
					for pid in rm_pid:
						del self.__proc_dict[pid]
				else:
					if pid in self.__proc_dict.keys() and \
					self.__proc_dict[pid].poll():
						self.__proc_dict[pid].terminate()
						del self.__proc_dict[pid]
						break
			sleep(0.0001)

	def query(self):
		from . import svc_info
		cap = Capability(svc_info['name'], svc_info['description'], 1)
		svc = ServiceDescr(svc_info['name'], svc_info['description'], [cap], 1)
		info = DEFwServiceInfo(self.__class__.__name__,
						  self.__class__.__module__, [svc])
		return info

	def test(self):
		logging.defw_service("Testing Launcher")

	def reserve(self, svc, client_ep, *args, **kwargs):
		logging.defw_service(f"{client_ep} reserved the {svc}")

	def release(self, services):
		self.runner_shutdown = True
