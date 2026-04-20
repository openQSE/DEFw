from defw_agent_info import *
import logging, os, copy, subprocess, traceback

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
		cmd = " ".join(self.__cmd)
		try:
#			self.__process = subprocess.Popen(cmd, env=self.__env,
#							stdout=subprocess.PIPE, stderr=subprocess.PIPE,
#							stdin=subprocess.PIPE, shell=True, start_new_session=False)
			self.__process = subprocess.Popen(self.__cmd, env=None, #self.__env,
							stdout=subprocess.PIPE, stderr=subprocess.PIPE,
							stdin=subprocess.PIPE, start_new_session=True)
			self.__pid = self.__process.pid
		except Exception as e:
			logging.defw_core(f"hit exception: {e}")
			raise e
		return self.__pid

	def run(self):
		cmd = " ".join(self.__cmd)
		rc = os.system(cmd)
		return rc

	def get_result(self):
		output, error = self.__process.communicate()
#		while True:
#			output += self.__process.stdout.readline()
#			logging.debug(f"Got output: {output}")
#			if output == b'' and self.__process.poll() is not None:
#				break
#		error = self.__process.stderr.read()

		return output, error, self.__process.returncode

	def kill(self):
		logging.defw_core(f"Kill process with pid: {self.__pid}")
		stack_trace_str = "".join(traceback.format_stack())
		logging.defw_stacktrace(f"{stack_trace_str}")
		self.__process.kill()
		try:
			os.waitpid(self.__pid, 0)
		except:
			pass

	def terminate(self):
		logging.defw_core(f"Terminate process with pid: {self.__pid}")
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
