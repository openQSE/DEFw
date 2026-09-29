#!/usr/bin/env python3
"""
Run one DEFw v1 benchmark workload and report the results, for example:
defw1_bench.py W1 --transport tcp --clients 8

The launcher does not join DEFw itself. It prepares a run directory, then
starts a DEFw v1 directory service with defwp and runs defw1_bench_driver.py
inside it. The driver spawns the svc_test_echo service and the client
processes, releases the clients together, and writes the report once they
finish.

Run it where DEFw v1 is installed, such as a QFw-SLURM-Cluster node after
sourcing qfw-activate, so that DEFW_PATH names the installation and the
Python environment has PyYAML. ../README.md describes the output.
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

BENCH_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V1_DIR = os.path.join(BENCH_DIR, 'v1')
sys.path.insert(0, BENCH_DIR)

import defw_bench_common as common  # noqa: E402

DIRSVC_MODULES = 'svc_dirsvc,api_dirsvc,api_test_echo'

# Rough peak memory of one v1 echo per payload byte, client and service
# together: the payload, its base64 text in the YAML message, and the
# copies each side makes on the way in and out.
V1_MEMORY_PER_PAYLOAD_BYTE = 8
MEMORY_HEADROOM = 512 * common.MIB

# The driver gets this long beyond --timeout to write its report.
REPORT_GRACE_S = 120

DRIVER_BOOTSTRAP = (
	'import sys\n'
	'sys.path[:0] = {paths!r}\n'
	'import defw1_bench_driver\n'
	'raise SystemExit(defw1_bench_driver.main())\n'
)

# Variables from the caller's environment that would change what DEFw
# loads or how it talks, cleared so every run starts from the same place.
CLEARED_ENV = (
	'DEFW_TRANSPORT',
	'DEFW_OFI_PROVIDER',
	'DEFW_EXTERNAL_SERVICES_PATH',
	'DEFW_EXTERNAL_SERVICE_APIS_PATH',
	'DEFW_EXTERNAL_EXPERIMENTS_PATH',
	'DEFW_LOAD_NO_INIT',
)


def parse_args(argv):
	parser = argparse.ArgumentParser(
		description='Run a DEFw v1 benchmark workload.')
	parser.add_argument('workload', choices=sorted(common.WORKLOADS))
	parser.add_argument(
		'--payload', type=common.parse_size, dest='payload_bytes',
		help='payload size such as 64, 4KiB or 16MiB '
		'(default: set by the workload)')
	parser.add_argument(
		'--payload-kind', choices=('bytes', 'str'), default='bytes',
		help='send the payload as bytes or as an ASCII str '
		'(default: bytes)')
	parser.add_argument(
		'--calls', type=int, help='measured calls per client')
	parser.add_argument(
		'--warmup', type=int,
		help='unmeasured calls per client before measuring')
	parser.add_argument(
		'--clients', type=int, default=1,
		help='concurrent client processes (default: 1)')
	parser.add_argument(
		'--transport', default='tcp',
		help='tcp or ofi+<provider>, such as ofi+tcp or ofi+sm2 '
		'(default: tcp)')
	parser.add_argument(
		'--label', help='run label (default: built from the parameters)')
	parser.add_argument(
		'--out', default=os.environ.get('DEFW_BENCH_OUT', '/tmp/defw-bench'),
		help='parent of the run directory, keep it on node-local storage '
		'(default: $DEFW_BENCH_OUT or /tmp/defw-bench)')
	parser.add_argument(
		'--defw-path', default=os.environ.get('DEFW_PATH'),
		help='DEFw v1 installation (default: $DEFW_PATH)')
	parser.add_argument(
		'--defw-revision',
		help='git revision of that installation, recorded in the report')
	parser.add_argument(
		'--image', default=os.environ.get('QFW_IMAGE'),
		help='container image, recorded in the report (default: $QFW_IMAGE)')
	parser.add_argument(
		'--port-base', type=int, default=26100,
		help='first TCP port the run uses (default: 26100)')
	parser.add_argument(
		'--timeout', type=float, default=1800.0,
		help='seconds before the run is abandoned (default: 1800)')
	parser.add_argument(
		'--log-level', default='error',
		help='DEFw C runtime log level (default: error)')
	parser.add_argument(
		'--py-log-level', default='critical',
		help='DEFw Python log level (default: critical)')
	parser.add_argument(
		'--no-spans', action='store_true',
		help='skip the per-call spans and write only the summary')
	parser.add_argument(
		'--force', action='store_true',
		help='skip the checks for available memory and for libfabric '
		'support before the run')
	args = parser.parse_args(argv)

	workload = common.WORKLOADS[args.workload]
	if args.payload_bytes is None:
		args.payload_bytes = workload['payload_bytes']
	if args.calls is None:
		args.calls = common.default_calls(args.workload, args.payload_bytes)
	if args.warmup is None:
		args.warmup = workload['warmup']
	if args.payload_bytes < 1:
		parser.error('--payload must be at least one byte')
	if args.calls < 1 or args.warmup < 0 or args.clients < 1:
		parser.error('--calls and --clients must be positive, '
			     '--warmup must not be negative')
	try:
		args.transport_env = common.transport_env(args.transport)
	except ValueError as exc:
		parser.error(str(exc))
	if not args.defw_path:
		parser.error('no DEFw installation, set DEFW_PATH or --defw-path')
	return args


def find_defwp(defw_path):
	override = os.environ.get('DEFW_EXECUTABLE')
	candidates = [override] if override else [
		os.path.join(defw_path, 'src', 'defwp'),
		os.path.join(defw_path, 'bin', 'defwp'),
	]
	for candidate in candidates:
		if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
			return os.path.abspath(candidate)
	sys.exit(f'defw1_bench: no defwp executable under {defw_path}')


def find_defw_config(defw_path):
	for relative in ('share/defw/config/defw_generic.yaml',
			 'python/config/defw_generic.yaml'):
		candidate = os.path.join(defw_path, relative)
		if os.path.isfile(candidate):
			return candidate
	sys.exit(f'defw1_bench: no defw_generic.yaml under {defw_path}')


def defw_links_libfabric(defw_path):
	"""Whether the installation's C runtime links libfabric, or None when
	that cannot be told. Without it DEFw serves an ofi request over tcp,
	logging the fallback but carrying on."""
	for libdir in ('lib', 'lib64', 'src'):
		library = os.path.join(defw_path, libdir, 'libdefw.so')
		if not os.path.isfile(library):
			continue
		try:
			linked = subprocess.run(['ldd', library], capture_output=True,
						text=True, timeout=10).stdout
		except (OSError, subprocess.SubprocessError):
			return None
		return 'libfabric' in linked
	return None


def check_transport(args):
	if args.transport == 'tcp' or args.force:
		return
	if defw_links_libfabric(args.defw_path) is False:
		sys.exit(
			f'defw1_bench: {args.transport} needs DEFw built with '
			f'libfabric, and the one in {args.defw_path} is not. It would '
			f'fall back to tcp, so the run would measure tcp.')


def is_defw_module_dir(path):
	return any(os.path.isfile(os.path.join(path, name))
		   for name in ('defw.py', 'cdefw_global.py'))


def defw_pythonpath(defw_path, current):
	"""PYTHONPATH for the DEFw processes of this run.

	defwp appends its own DEFW_PATH directories to sys.path after whatever
	PYTHONPATH already holds. An activated environment for a different
	DEFw installation would therefore win, and the run would load that
	installation's Python modules against this one's C runtime. Drop any
	entry that holds DEFw modules and put this installation's first."""
	own = [os.path.join(defw_path, 'src'),
	       os.path.join(defw_path, 'python', 'infra')]
	kept = [entry for entry in (current or '').split(os.pathsep)
		if entry and not is_defw_module_dir(entry)]
	return os.pathsep.join(own + kept)


def mem_available():
	try:
		with open('/proc/meminfo', encoding='ascii') as stream:
			for line in stream:
				if line.startswith('MemAvailable:'):
					return int(line.split()[1]) * 1024
	except OSError:
		pass
	return None


def check_memory(args):
	per_client = V1_MEMORY_PER_PAYLOAD_BYTE * args.payload_bytes
	needed = per_client * args.clients + MEMORY_HEADROOM
	available = mem_available()
	if args.force or available is None or needed <= available:
		return
	sys.exit(
		f'defw1_bench: {args.clients} client(s) echoing '
		f'{common.format_size(args.payload_bytes)} through DEFw v1 need '
		f'about {needed // common.MIB} MiB, and '
		f'{available // common.MIB} MiB is available. Use fewer '
		f'clients or a smaller payload, or pass --force.')


def git_revision(path):
	def git(*arguments):
		return subprocess.run(['git', '-C', path, *arguments],
				      capture_output=True, text=True, timeout=10)
	try:
		head = git('rev-parse', 'HEAD')
		if head.returncode != 0:
			return None
		dirty = git('status', '--porcelain', '--', '.').stdout.strip()
	except (OSError, subprocess.SubprocessError):
		return None
	return head.stdout.strip() + ('-dirty' if dirty else '')


def make_run_dir(args, trace_id):
	stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
	name = (f'{stamp}-v1-{args.workload}-{args.transport}-'
		f'c{args.clients}-{trace_id[:8]}')
	run_dir = os.path.join(os.path.abspath(args.out), name)
	for sub in ('control', 'results', 'dirsvc', 'otlp'):
		os.makedirs(os.path.join(run_dir, sub))
	return run_dir


def write_config(args, run_dir, trace_id, defwp, defw_config):
	label = args.label or (
		f'v1-{args.workload}-{args.transport}-'
		f'{common.format_size(args.payload_bytes).replace(" ", "")}-'
		f'{args.payload_kind}-c{args.clients}')
	config = {
		'schema': 1,
		'defw_major': 1,
		'trace_id': trace_id,
		'root_span_id': os.urandom(8).hex(),
		'label': label,
		'workload': args.workload,
		'payload_bytes': args.payload_bytes,
		'payload_kind': args.payload_kind,
		'calls': args.calls,
		'warmup': args.warmup,
		'clients': args.clients,
		'transport': args.transport,
		'transport_env': args.transport_env,
		'run_dir': run_dir,
		'defw_path': os.path.abspath(args.defw_path),
		'defwp': defwp,
		'defw_config': defw_config,
		'defw_revision': args.defw_revision,
		'image': args.image,
		'harness_revision': git_revision(BENCH_DIR),
		'bench_dir': BENCH_DIR,
		'v1_dir': V1_DIR,
		'port_base': args.port_base,
		'client_port_base': args.port_base + 100,
		'timeout_s': args.timeout,
		'spans': not args.no_spans,
		'log_level': args.log_level,
		'py_log_level': args.py_log_level,
		'launched_unix_ns': time.time_ns(),
	}
	path = os.path.join(run_dir, 'config.json')
	common.write_json(path, config, indent=2)
	return config, path


def write_pref(config, run_dir):
	# DEFw reads its preferences as YAML. JSON is valid YAML, so the
	# launcher needs no YAML library of its own.
	pref = {
		'editor': None,
		'py_loglevel': config['py_log_level'],
		'halt_on_exception': False,
		'RPC timeout': max(300, int(config['timeout_s'])),
		'num_intfs': 3,
		'cmd verbosity': True,
		'debug module reload': False,
	}
	path = os.path.join(run_dir, 'defw_pref.yaml')
	common.write_json(path, pref, indent=2)
	return path


def dirsvc_env(config, config_path, pref_path):
	env = dict(os.environ)
	for key in CLEARED_ENV:
		env.pop(key, None)
	defw_path = config['defw_path']
	name = f'bench-dirsvc-{config["trace_id"][:8]}'
	port = config['port_base']
	# A standalone DEFw build installs to lib64 on Rocky, one bundled
	# with QFw installs to lib.
	libs = os.pathsep.join(os.path.join(defw_path, libdir)
			       for libdir in ('lib', 'lib64', 'src'))
	if env.get('LD_LIBRARY_PATH'):
		libs = libs + os.pathsep + env['LD_LIBRARY_PATH']
	env.update({
		'LD_LIBRARY_PATH': libs,
		'PYTHONPATH': defw_pythonpath(defw_path, env.get('PYTHONPATH')),
		'DEFW_PATH': defw_path,
		'DEFW_CONFIG_PATH': config['defw_config'],
		'DEFW_AGENT_NAME': name,
		'DEFW_AGENT_TYPE': 'dirsvc',
		'DEFW_SHELL_TYPE': 'cmdline',
		'DEFW_LISTEN_PORT': str(port),
		'DEFW_TELNET_PORT': str(port + 1),
		'DEFW_PARENT_NAME': name,
		'DEFW_PARENT_HOSTNAME': '127.0.0.1',
		'DEFW_PARENT_ADDR': '127.0.0.1',
		'DEFW_PARENT_PORT': str(port),
		'DEFW_DISABLE_DIRSVC': 'no',
		'DEFW_LOG_DIR': os.path.join(config['run_dir'], 'dirsvc'),
		'DEFW_LOG_LEVEL': config['log_level'],
		'DEFW_PY_LOGLEVEL': config['py_log_level'],
		'DEFW_PREF_PATH': pref_path,
		'DEFW_EXPERIMENT_PORT_BASE': str(port + 10),
		'DEFW_ONLY_LOAD_MODULE': DIRSVC_MODULES,
		'DEFW_EXTERNAL_SERVICES_PATH': '',
		'DEFW_EXTERNAL_SERVICE_APIS_PATH': '',
		'DEFW_EXTERNAL_EXPERIMENTS_PATH': '',
		common.CONFIG_ENV: config_path,
	})
	env.update(config['transport_env'])
	return env


class Interrupted(Exception):
	def __init__(self, signum):
		super().__init__(signum)
		self.signum = signum


def raise_interrupted(signum, frame):
	raise Interrupted(signum)


def tee(stream, log):
	for line in stream:
		sys.stdout.write(line)
		sys.stdout.flush()
		log.write(line)
		log.flush()


def signal_group(pid, sig):
	try:
		os.killpg(pid, sig)
	except (ProcessLookupError, PermissionError):
		pass


def stop_run(driver, run_dir):
	"""Stop the driver and anything it left running."""
	signal_group(driver.pid, signal.SIGTERM)
	try:
		driver.wait(timeout=10)
	except subprocess.TimeoutExpired:
		signal_group(driver.pid, signal.SIGKILL)
	try:
		with open(os.path.join(run_dir, 'control', 'pids.json'),
			  encoding='utf-8') as stream:
			pids = json.load(stream)
	except (OSError, ValueError):
		pids = {}
	for pid in pids.values():
		signal_group(pid, signal.SIGKILL)


def run_driver(config, env):
	run_dir = config['run_dir']
	command = [config['defwp'], '-c',
		   DRIVER_BOOTSTRAP.format(paths=[V1_DIR, BENCH_DIR])]
	with open(os.path.join(run_dir, 'driver.log'), 'w',
		  encoding='utf-8') as log:
		driver = subprocess.Popen(
			command, env=env, cwd=run_dir, text=True, bufsize=1,
			stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
			start_new_session=True)
		pump = threading.Thread(target=tee, args=(driver.stdout, log),
					daemon=True)
		pump.start()
		try:
			rc = driver.wait(timeout=config['timeout_s'] + REPORT_GRACE_S)
		except subprocess.TimeoutExpired:
			print('defw1_bench: the run timed out, stopping it',
			      file=sys.stderr)
			stop_run(driver, run_dir)
			rc = 124
		except Interrupted as exc:
			print('defw1_bench: interrupted, stopping the run',
			      file=sys.stderr)
			stop_run(driver, run_dir)
			rc = 128 + exc.signum
		pump.join(timeout=10)
	return rc


def main(argv=None):
	# Stop the processes the run started, whichever way the launcher is
	# asked to stop. Without this, SIGTERM leaves defwp processes behind.
	for signum in (signal.SIGINT, signal.SIGTERM):
		signal.signal(signum, raise_interrupted)
	args = parse_args(sys.argv[1:] if argv is None else argv)
	defwp = find_defwp(args.defw_path)
	defw_config = find_defw_config(args.defw_path)
	check_transport(args)
	check_memory(args)
	trace_id = os.urandom(16).hex()
	run_dir = make_run_dir(args, trace_id)
	config, config_path = write_config(args, run_dir, trace_id, defwp,
					   defw_config)
	pref_path = write_pref(config, run_dir)
	rc = run_driver(config, dirsvc_env(config, config_path, pref_path))
	print(f'run directory: {run_dir}')
	return rc


if __name__ == '__main__':
	sys.exit(main())
