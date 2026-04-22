import os, shlex, subprocess, threading, ctypes, time, logging
from defw_exception import DEFwError
import defw_common_def

def exec_cmd(cmd, exception=True):
	if defw_common_def.is_cmd_verbosity():
		logging.defw_core("executing -> " + cmd)
	args = shlex.split(cmd)
	try:
		out = subprocess.Popen(args, stderr=subprocess.STDOUT,
			stdout=subprocess.PIPE)
	except Exception as e:
		logging.defw_core("Failed to execute cmd: " + cmd)
		logging.defw_core(e)
		return [None, -1]
	t = out.communicate()[0],out.returncode
	if t[1] != 0 and exception:
		raise DEFwError(cmd+"\n"+"rc = "+str(t[1])+"\n"+t[0].decode("utf-8"))
	elif t[1] != 0:
		logging.defw_core("Failed to execute cmd: " + cmd + " with rc = " + str(t[1]))
	return t

class DEFwCmd(threading.Thread):
	def __init__(self, name, cmd, exception=False):
		threading.Thread.__init__(self)
		self.name = name
		self.cmd = cmd
		self.thread_id = threading.get_ident()
		self.rc = None
		self.exception = exception

	def run(self):
		self.rc = exec_cmd(self.cmd, exception=self.exception)

	def raise_exception(self):
		res = ctypes.pythonapi.PyThreadState_SetAsyncExc(self.thread_id,
				ctypes.py_object(SystemExit))
		if res > 1:
			ctypes.pythonapi.PyThreadState_SetAsyncExc(self.thread_id, 0)

def defw_exec_local_cmd(cmd, expire=0, exception=True):
	if expire <= 0 or not type(expire) == int:
		return exec_cmd(cmd, exception=exception)
	cmd_thrd = DEFwCmd('defw_cmd', cmd, exception=exception)
	cmd_thrd.start()
	time.sleep(expire)
	if cmd_thrd.isAlive():
		cmd_thrd.raise_exception()
	if not cmd_thrd.rc:
		raise StopIteration(cmd+"\nExpired")
	return cmd_thrd.rc

def read_from_stream(stream):
	return '\n'.join(stream.read().decode('utf-8').splitlines())

def defw_exec_remote_cmd(cmd, host, username='', ignore_err=False, deamonize=False):
	import getpass
	import paramiko

	if deamonize:
		cmd += " &"

	if len(username) == 0:
		username = getpass.getuser()

	ssh = paramiko.SSHClient()
	ssh.load_system_host_keys()
	ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
	ssh.connect(hostname=host, timeout=3, banner_timeout=3, username=username)
	stdin, stdout, stderr = ssh.exec_command(cmd)

	logging.defw_core(f"Run:\n\t{cmd}\non\n\t{host}")

	out = ''
	err = ''
	if stdout.channel.recv_ready():
		out = read_from_stream(stdout)
	if stderr.channel.recv_ready():
		err = read_from_stream(stderr)
	stdin.close()
	stdout.close()
	stderr.close()
	ssh.close()
	return out, err
