import time, yaml, string, io, traceback, math
import random, os, threading, sys, logging
from defw_cmd import defw_exec_local_cmd
from defw_exception import DEFwError

reset = '\033[0m'
bold = '\033[01m'
disable = '\033[02m'
underline = '\033[04m'
reverse = '\033[07m'
strikethrough = '\033[09m'
invisible = '\033[08m'

class fg:
	black = '\033[30m'
	red = '\033[31m'
	green = '\033[32m'
	orange = '\033[33m'
	blue = '\033[34m'
	purple = '\033[35m'
	cyan = '\033[36m'
	lightgrey = '\033[37m'
	darkgrey = '\033[90m'
	lightred = '\033[91m'
	lightgreen = '\033[92m'
	yellow = '\033[93m'
	lightblue = '\033[94m'
	pink = '\033[95m'
	lightcyan = '\033[96m'
	bold = '\033[1m'

class bg:
	black = '\033[40m'
	red = '\033[41m'
	green = '\033[42m'
	orange = '\033[43m'
	blue = '\033[44m'
	purple = '\033[45m'
	cyan = '\033[46m'
	lightgrey = '\033[47m'

def get_today():
	info = time.localtime()
	today = "%d-%d-%d" % (info.tm_year, info.tm_mon, info.tm_mday)
	return today

def get_now():
	info = time.localtime()
	time_info = "%d.%d" % (info.tm_hour, info.tm_min)
	return time_info

def prformat(color, *args, **kwargs):
	print(color, *args, **kwargs)
	print(reset)

class IfwThread(threading.Thread):
	def __init__(self, name, function, exception=False, *args, **kwargs):
		threading.Thread.__init__(self)
		self.name = name
		self.thread_id = threading.get_ident()
		self.rc = None
		self.exception = exception
		self.args = args
		self.kwargs = kwargs
		self.function = function

	def run(self):
		self.rc = self.function(*self.args, **self.kwargs)

	def raise_exception(self):
		res = ctypes.pythonapi.PyThreadState_SetAsyncExc(self.thread_id,
				ctypes.py_object(SystemExit))
		if res > 1:
			ctypes.pythonapi.PyThreadState_SetAsyncExc(self.thread_id, 0)

def generate_random_int_array(size, minimum=1, maximum=3000):
	return random.sample(range(minimum, maximum), size)

def generate_random_bytes(size):
	return os.urandom(size)

def generate_random_string(length):
	characters = string.ascii_letters + string.digits  # Includes both letters and digits
	return ''.join(random.choice(characters) for _ in range(length))

def get_lscpu():
	lscpu = defw_exec_local_cmd('/usr/bin/lscpu')
	lscpu = lscpu[0].decode('utf-8')
	cpuinfo = {}
	for line in lscpu.splitlines():
		if ':' in line:
			key, value = line.split(':', 1)
			cpuinfo[key.strip()] = value.strip()
	return cpuinfo

def split_on_commas(expr):
	l = []
	c = o = 0
	s = expr
	stop = False
	while not stop and c != -1 and o != -1:
		o = s.find('[')
		b = s.find(']')
		c = s.find(',')
		while c > o and c < b:
			c = s.find(',', c+1)
		if len(l):
			l.pop()
		if c < o or c > b:
			l += [s[:s.find(',', c)], s[s.find(',', c)+1:]]
			s = l[-1]
		else:
			l += s.split(',')
			stop = True
	for i in range(0, len(l)):
		l[i] = l[i].strip()
	return l

def expand_host_list_sub(expr):
	host_list = []

	# first phase split on the commas first
	open_br = expr.find('[')
	close_br = expr.find(']', open_br)
	if open_br == -1 and close_br == -1:
		return expr.split(',')

	if open_br == -1 or close_br == -1:
		return []

	rangestr = expr[open_br+1 : close_br]

	node = expr[:open_br]

	ranges = rangestr.split(',')

	for r in ranges:
		cur = r.split('-')
		if len(cur) == 2:
			pre = "{:0%dd}" % len(cur[0])
			for idx in range(int(cur[0]), int(cur[1])+1):
				host_list.append(f'{node}{pre.format(idx)}')
		elif len(cur) == 1:
			pre = "{:0%dd}" % len(cur[0])
			host_list.append(f'{node}{pre.format(int(cur[0]))}')

	return host_list

def expand_host_list(expr):
	l = split_on_commas(expr)
	host_list = []
	for e in l:
		host_list += expand_host_list_sub(e)
	return host_list

def get_thread_names():
	thread_names = {}
	for thread in threading.enumerate():
		thread_names[thread.ident] = thread.name
	return thread_names

def print_thread_stack_trace_to_logger(level='debug'):
	stack_trace_str = "".join(traceback.format_stack())
	logging.defw_stacktrace(f"{stack_trace_str}")

def print_all_thread_stack_traces_to_logger():
	frames = sys._current_frames()
	thread_names = get_thread_names()
	for thread_id, frame in frames.items():
		# Redirect stderr to a StringIO object
		temp_stderr = io.StringIO()
		sys.stderr = temp_stderr

		# Print stack trace to temporary stderr
		traceback.print_stack(frame)

		# Get stack trace from StringIO object
		stack_trace = temp_stderr.getvalue()

		# Get thread name
		thread_name = thread_names.get(thread_id, "Unknown Thread")

		# Log the stack trace with thread name
		logging.defw_stacktrace(
			f"Thread Name: {thread_name}, ID: {thread_id}\n{stack_trace}"
		)

		# Reset stderr
		temp_stderr.close()
		sys.stderr = sys.__stderr__

def print_thread_stack_traces():
	frames = sys._current_frames()
	tnames = get_thread_names()
	for thread_id, frame in frames.items():
		print(f"Thread = {thread_id}: {tnames[thread_id]}")
		traceback.print_stack(frame)

def round_half_up(number):
	whole_part = math.floor(number)
	fractional_part = number - whole_part
	if fractional_part >= 0.5:
		return whole_part + 1
	else:
		return whole_part

def round_to_nearest_power_of_two(number):
	if number <= 0:
		return 0  # or handle the case as needed

	nearest_power_of_two = 2 ** int(math.log2(number) + 0.5)
	return nearest_power_of_two

