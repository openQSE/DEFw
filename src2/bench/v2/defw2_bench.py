#!/usr/bin/env python3
"""Run a DEFw v2 benchmark workload.

The launcher starts the echo service, runs the measured clients against it,
and writes the same report a v1 run writes, so the two can be compared
directly. The workload table, the statistics and the output layout are
shared with the v1 harness in defw_bench_common.

The per-call spans come from libdefw2 itself. This writes only the
qfw.bench.run span the calls hang beneath, and passes its trace context to
every client, so one run is one trace across every process in it.

	defw2_bench.py W1 --transport ofi+tcp --clients 8

W5 and W6 run QPM jobs against QFw's fake IQM QPM instead, which a QFw run
serves. The launcher starts no service for them. It finds the run's
directory, and measures the C client, the Python client, or QFw's own client
code on the run's DEFw, which may be v1:

	defw2_bench.py W5 --qfw-run-dir RUN --client qfw --clients 8
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

V2_DIR = os.path.dirname(os.path.abspath(__file__))
BENCH_DIR = os.path.dirname(V2_DIR)
sys.path.insert(0, BENCH_DIR)

import defw_bench_common as common  # noqa: E402

SCOPE_NAME = 'defw.bench.v2'
SCOPE_VERSION = '0.1'
REPORT_SCHEMA = 'defw-bench-summary/1'
SERVICE_API = 'qfw.echo'
QPM_API = 'qfw.qpm.execution'
POLL_S = 0.05
STOP_GRACE_S = 10
STDERR_TAIL_LINES = 15


def parse_args(argv):
	parser = argparse.ArgumentParser(
		description='Run a DEFw v2 benchmark workload.')
	parser.add_argument('workload', choices=sorted(common.WORKLOADS))
	parser.add_argument(
		'--payload', type=common.parse_size, dest='payload_bytes',
		help='payload size such as 64, 4KiB or 16MiB '
		'(default: set by the workload)')
	parser.add_argument(
		'--calls', type=int, help='measured calls per client')
	parser.add_argument(
		'--warmup', type=int,
		help='unmeasured calls per client before measuring')
	parser.add_argument(
		'--clients', type=int, default=1,
		help='concurrent client processes (default: 1)')
	parser.add_argument(
		'--transport', default='ofi+tcp',
		help='Margo provider, such as ofi+tcp, na+sm or ofi+cxi '
		'(default: ofi+tcp)')
	parser.add_argument(
		'--rpc-threads', type=int,
		help='handler execution streams in the service '
		'(default: the runtime default)')
	parser.add_argument(
		'--client', choices=('c', 'python', 'qfw'), default='c',
		help='which client to measure: c, python, or for W5 and W6 '
		'QFw\'s own client code under qfw-srun (default: c)')
	parser.add_argument(
		'--qfw-run-dir',
		help='W5 and W6: the QFw run whose plane serves the QPM')
	parser.add_argument(
		'--directory',
		help='W5 and W6: the directory to find the QPM in '
		'(default: the QFw run\'s)')
	parser.add_argument(
		'--service-id',
		help='W5 and W6: the QPM\'s service_id (default: the only QPM '
		'in the directory, as in a QFw run, which names its own)')
	parser.add_argument(
		'--qubits', type=int,
		help='W5 and W6: qubits per job (default: set by the workload)')
	parser.add_argument(
		'--shots', type=int,
		help='W5 and W6: shots per job (default: set by the workload)')
	parser.add_argument(
		'--service', choices=('c', 'python'), default='c',
		help='which echo service to measure against (default: c)')
	parser.add_argument(
		'--service-workers', type=int, default=2,
		help='queue workers in the python service (default: 2)')
	parser.add_argument(
		'--label', help='run label (default: built from the parameters)')
	parser.add_argument(
		'--out',
		default=os.environ.get('DEFW_BENCH_OUT', '/tmp/defw-bench'),
		help='parent of the run directory, keep it on node-local '
		'storage (default: $DEFW_BENCH_OUT or /tmp/defw-bench)')
	parser.add_argument(
		'--bin-dir', default=os.environ.get('DEFW2_BIN_DIR'),
		help='where defw2-echo and defw2-bench are '
		'(default: $DEFW2_BIN_DIR, then $PATH)')
	parser.add_argument(
		'--defw-revision',
		help='git revision of the build under test, kept in the report')
	parser.add_argument(
		'--image', default=os.environ.get('QFW_IMAGE'),
		help='container image, kept in the report (default: $QFW_IMAGE)')
	parser.add_argument(
		'--timeout', type=float, default=1800.0,
		help='seconds before the run is abandoned (default: 1800)')
	parser.add_argument(
		'--log-level', default='error',
		help='DEFw v2 log level (default: error)')
	parser.add_argument(
		'--no-spans', action='store_true',
		help='leave profiling off, so only the summary is produced')
	parser.add_argument(
		'--force', action='store_true',
		help='skip the check for available memory before the run')
	args = parser.parse_args(argv)

	workload = common.WORKLOADS[args.workload]
	args.qpm = workload.get('qpm', False)
	if args.qpm:
		args.qubits = args.qubits or workload['qubits']
		args.shots = args.shots or workload['shots']
		args.statevector = workload['statevector']
		# A W6 job's payload is its statevector, set by the qubits.
		args.payload_bytes = (16 << args.qubits
				      if args.statevector else 0)
		if not args.qfw_run_dir and not (args.directory and
						 args.client != 'qfw'):
			parser.error('{} needs --qfw-run-dir, or --directory '
				     'for the c and python clients'.format(
					     args.workload))
	elif args.client == 'qfw':
		parser.error('the qfw client runs only W5 and W6')
	if args.payload_bytes is None:
		args.payload_bytes = workload['payload_bytes']
	if args.calls is None:
		args.calls = common.default_calls(args.workload,
						  args.payload_bytes)
	if args.warmup is None:
		args.warmup = workload['warmup']
	if args.clients < 1:
		parser.error('--clients must be at least 1')
	# W3 is the bulk workload. Anything too large to ride inside a
	# message has to go through registered memory in any case. A W6
	# statevector does too, but the QPM's API decides that, not this.
	args.bulk = not args.qpm and (args.workload == 'W3' or
				      args.payload_bytes > 4 * 1024 * 1024)
	return args


def fail(message):
	print('error: ' + message, file=sys.stderr)
	raise SystemExit(1)


def find_binary(name, bin_dir):
	if bin_dir:
		path = os.path.join(os.path.abspath(bin_dir), name)
		if os.access(path, os.X_OK):
			return path
		fail('{} is not in {}'.format(name, bin_dir))
	for directory in os.environ.get('PATH', '').split(os.pathsep):
		path = os.path.join(directory, name)
		if directory and os.access(path, os.X_OK):
			return path
	fail('{} is not in PATH, pass --bin-dir'.format(name))


def git_revision(path):
	def git(*arguments):
		return subprocess.run(['git', '-C', path, *arguments],
				      capture_output=True, text=True,
				      timeout=10)
	try:
		head = git('rev-parse', 'HEAD')
		if head.returncode != 0:
			return None
		dirty = git('status', '--porcelain', '--', '.').stdout.strip()
	except (OSError, subprocess.SubprocessError):
		return None
	return head.stdout.strip() + ('-dirty' if dirty else '')


def check_memory(args):
	"""Each client holds a payload, and a bulk client holds two."""
	per_client = args.payload_bytes * (2 if args.bulk else 1)
	needed = per_client * args.clients + args.payload_bytes
	try:
		pages = os.sysconf('SC_AVPHYS_PAGES')
		available = pages * os.sysconf('SC_PAGE_SIZE')
	except (ValueError, OSError):
		return
	if needed > available * 0.8:
		fail('{} clients of {} need about {}, and {} is free. '
		     'Use --force to run anyway.'.format(
			     args.clients, common.format_size(args.payload_bytes),
			     common.format_size(needed),
			     common.format_size(int(available))))


def proc_cpu_ns(pid):
	"""User plus system CPU time of a process, from /proc/<pid>/stat."""
	try:
		with open('/proc/{}/stat'.format(pid), encoding='ascii') as f:
			stat = f.read()
	except OSError:
		return None
	# The command name can hold spaces, so split after its closing paren.
	fields = stat[stat.rindex(')') + 2:].split()
	ticks = int(fields[11]) + int(fields[12])
	return ticks * 1_000_000_000 // os.sysconf('SC_CLK_TCK')


def proc_peak_rss_kib(pid):
	try:
		with open('/proc/{}/status'.format(pid), encoding='ascii') as f:
			for line in f:
				if line.startswith('VmHWM:'):
					return int(line.split()[1])
	except OSError:
		return None
	return None


def _flavour(args):
	"""What a run's name says about which halves were Python."""
	if args.client == 'qfw':
		return 'qfw'
	if args.client == 'python' and args.service == 'python':
		return 'pypy'
	if args.service == 'python':
		return 'pysvc'
	if args.client == 'python':
		return 'pycli'
	return ''


def make_run_dir(args, trace_id):
	stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
	name = ('{}-v{}{}-{}-{}-c{}-{}'.format(stamp, args.defw_major,
					    _flavour(args),
					    args.workload,
					    args.transport.replace('+', ''),
					    args.clients, trace_id[:8]))
	run_dir = os.path.join(os.path.abspath(args.out), name)
	for sub in ('control', 'results', 'logs', 'otlp'):
		os.makedirs(os.path.join(run_dir, sub))
	return run_dir


def write_config(args, run_dir, trace_id, root_span_id, binaries):
	label = args.label or 'v{}{}-{}-{}-{}-c{}'.format(
		args.defw_major, _flavour(args),
		args.workload, args.transport,
		common.format_size(args.payload_bytes).replace(' ', ''),
		args.clients)
	config = {
		'schema': 1,
		'defw_major': args.defw_major,
		'trace_id': trace_id,
		'root_span_id': root_span_id,
		'label': label,
		'workload': args.workload,
		'payload_bytes': args.payload_bytes,
		'payload_kind': payload_kind(args),
		'calls': args.calls,
		'warmup': args.warmup,
		'clients': args.clients,
		'transport': args.transport,
		'transport_env': transport_env(args),
		'rpc_threads': args.rpc_threads,
		'run_dir': run_dir,
		'binaries': binaries,
		'defw_revision': args.defw_revision,
		'image': args.image,
		'harness_revision': git_revision(BENCH_DIR),
		'timeout_s': args.timeout,
		'spans': not args.no_spans,
		'log_level': args.log_level,
		'launched_unix_ns': time.time_ns(),
	}
	common.write_json(os.path.join(run_dir, 'config.json'), config,
			  indent=2)
	return config


def payload_kind(args):
	if args.qpm:
		return 'statevector' if args.statevector else 'none'
	return 'bulk' if args.bulk else 'bytes'


def transport_env(args):
	"""What picks the transport. QFw's client gets its own from the run,
	except that v1 takes DEFW_TRANSPORT from the caller."""
	if args.client == 'qfw':
		if args.defw_major == 1:
			return common.transport_env(args.transport)
		return {}
	return {'DEFW2_ADDRESS': args.transport + '://'}


def read_qfw_run(run_dir):
	"""The directory and the DEFw of a QFw run, from its state file."""
	path = os.path.join(run_dir, 'state', 'runtime-state.json')
	try:
		with open(path, encoding='utf-8') as stream:
			state = json.load(stream)
	except (OSError, ValueError) as exc:
		fail('cannot read the QFw run at {}: {}'.format(run_dir, exc))
	endpoint = (state.get('local_dirsvc') or {}).get('endpoint')
	if not endpoint:
		fail('the QFw run at {} names no directory'.format(run_dir))
	major = str((state.get('environment') or {}).get(
		'QFW_DEFW_VERSION') or '1').strip()
	return {'directory': 'ofi+tcp://' + endpoint, 'defw_major': int(major)}


def process_env(args, run_dir, agent):
	env = dict(os.environ)
	env.update({
		'DEFW2_TELEMETRY_DIR': os.path.join(run_dir, 'otlp'),
		'DEFW_AGENT_NAME': agent,
		'DEFW_LOG_LEVEL': args.log_level,
	})
	env.update(transport_env(args))
	if args.no_spans:
		env.pop('DEFW2_PROFILE', None)
	else:
		env['DEFW2_PROFILE'] = '1'
	return env


def service_command(args, binaries):
	"""The C service, or the same service written in Python."""
	if args.service == 'c':
		return [binaries['defw2-echo'], 'serve']
	return [sys.executable, os.path.join(V2_DIR, 'defw2_echo_service.py'),
		'--workers', str(args.service_workers),
		'--service-id', 'bench-echo']


def start_service(args, config, binaries):
	"""Start the echo service and read back the address it prints."""
	run_dir = config['run_dir']
	env = process_env(args, run_dir, 'bench-echo')
	if args.rpc_threads is not None:
		env['DEFW2_RPC_THREADS'] = str(args.rpc_threads)
	log = open(os.path.join(run_dir, 'logs', 'echo.log'), 'w',
		   encoding='utf-8')
	service = subprocess.Popen(service_command(args, binaries),
				   stdout=subprocess.PIPE, stderr=log,
				   env=env, text=True)
	address = service.stdout.readline().strip()
	if not address:
		service.wait(timeout=STOP_GRACE_S)
		fail('the echo service did not start, see logs/echo.log')
	return service, address


def client_command(args, binaries):
	if args.client == 'qfw':
		return ['qfw-srun', '--run-dir', args.qfw_run_dir,
			os.path.join(BENCH_DIR, 'qfw_qpm_client.py')]
	if args.client == 'python':
		return [sys.executable,
			os.path.join(V2_DIR, 'defw2_bench_client.py')]
	return [binaries['defw2-bench']]


def start_clients(args, config, binaries, address):
	run_dir = config['run_dir']
	traceparent = '00-{}-{}-01'.format(config['trace_id'],
					   config['root_span_id'])
	clients = []
	for index in range(args.clients):
		command = client_command(args, binaries)
		command += [
			'--address', address,
			'--index', str(index),
			'--calls', str(args.calls),
			'--warmup', str(args.warmup),
			'--traceparent', traceparent,
			'--ready', common.ready_path(run_dir, index),
			'--go', common.go_path(run_dir),
			'--result', common.result_path(run_dir, index),
			'--wait-s', str(int(args.timeout)),
		]
		if args.client == 'qfw':
			command += ['--defw-major', str(args.defw_major)]
		if args.qpm:
			command += ['--qpm', '--qubits', str(args.qubits),
				    '--shots', str(args.shots)]
			if args.service_id:
				command += ['--service-id', args.service_id]
			if args.statevector:
				command.append('--statevector')
		else:
			command += ['--payload', str(args.payload_bytes)]
		if args.bulk:
			command.append('--bulk')
		log = open(os.path.join(run_dir, 'logs',
					'client-{}.log'.format(index)),
			   'w', encoding='utf-8')
		env = process_env(args, run_dir, 'bench-client-{}'.format(index))
		client = subprocess.Popen(command, stdout=log, stderr=log,
					  env=env, text=True,
					  start_new_session=True)
		client.group = True
		clients.append(client)
	return clients


def check_service(service, run_dir):
	"""A service that stops takes the run with it, and every client then
	sits in its own timeout, so say so at once. W5 and W6 start none."""
	if service is None:
		return
	code = service.poll()
	if code is None:
		return
	for line in tail(os.path.join(run_dir, 'logs', 'echo.log')):
		print('  ' + line, file=sys.stderr)
	fail('the echo service stopped with {} {}'.format(
		'signal' if code < 0 else 'status', abs(code)))


def wait_for_ready(args, config, clients, service):
	run_dir = config['run_dir']
	deadline = time.monotonic() + args.timeout
	pending = set(range(args.clients))
	while pending:
		for index in sorted(pending):
			if os.path.exists(common.ready_path(run_dir, index)):
				pending.discard(index)
		check_service(service, run_dir)
		for index, client in enumerate(clients):
			if client.poll() is not None and index in pending:
				fail('client {} stopped before it was ready, '
				     'see logs/client-{}.log'.format(index, index))
		if time.monotonic() > deadline:
			fail('clients {} never became ready'.format(
				sorted(pending)))
		if pending:
			time.sleep(POLL_S)


def wait_for_clients(args, config, clients, service):
	deadline = time.monotonic() + args.timeout
	pending = list(enumerate(clients))
	while pending:
		check_service(service, config['run_dir'])
		still = []
		for index, client in pending:
			if client.poll() is None:
				still.append((index, client))
		pending = still
		if pending and time.monotonic() > deadline:
			fail('clients {} did not finish within {} s'.format(
				[index for index, _ in pending], args.timeout))
		if pending:
			time.sleep(POLL_S)


def signal_process(process, signum):
	"""Signal a process, or the whole of its process group when it leads
	one, which is how every client is started."""
	try:
		if getattr(process, 'group', False):
			os.killpg(process.pid, signum)
		else:
			process.send_signal(signum)
	except ProcessLookupError:
		pass


def stop(process):
	"""Stop a process and anything it started. QFw's client runs under
	qfw-srun, which passes no signal on to the client it starts, so
	stopping qfw-srun alone left the client running and holding its
	reservation."""
	if process is None:
		return
	group = getattr(process, 'group', False)
	if process.poll() is None:
		signal_process(process, signal.SIGTERM)
		try:
			process.wait(timeout=STOP_GRACE_S)
		except subprocess.TimeoutExpired:
			signal_process(process, signal.SIGKILL)
			process.wait(timeout=STOP_GRACE_S)
	if group:
		signal_process(process, signal.SIGKILL)


def tail(path):
	try:
		with open(path, encoding='utf-8', errors='replace') as stream:
			lines = stream.read().splitlines()
	except OSError:
		return []
	return lines[-STDERR_TAIL_LINES:]


def read_results(args, config):
	run_dir = config['run_dir']
	results = []
	for index in range(args.clients):
		path = common.result_path(run_dir, index)
		if not os.path.exists(path):
			for line in tail(os.path.join(run_dir, 'logs',
						      'client-{}.log'.format(index))):
				print('  ' + line, file=sys.stderr)
			fail('client {} wrote no results'.format(index))
		with open(path, encoding='utf-8') as stream:
			results.append(json.load(stream))
	return results


def environment(args, config):
	return {
		'hostname': os.uname().nodename,
		'cpu_count': os.cpu_count(),
		'python': sys.version.split()[0],
		'libfabric': common.libfabric_version(),
		'image': args.image,
		'defw_revision': args.defw_revision,
		'harness_revision': config['harness_revision'],
		'binaries': config['binaries'],
		'rpc_threads': args.rpc_threads,
		'slurm_job_id': os.environ.get('SLURM_JOB_ID'),
		'slurm_nodelist': os.environ.get('SLURM_JOB_NODELIST'),
		'defw_log_level': args.log_level,
		'profiling': not args.no_spans,
		'client_language': args.client,
		'service_language': 'qfw' if args.qpm else args.service,
		'service_workers': (args.service_workers
				    if args.service == 'python' and
				    not args.qpm else None),
		'qfw_run_dir': args.qfw_run_dir,
	}


def build_report(args, config, results, service, go_unix_ns, end_unix_ns):
	"""The v1 harness's report shape, so the two runs compare directly."""
	ok_durations = []
	clients = []
	failed = 0
	for result in results:
		skipped = set(result['failed_calls'])
		durations = [duration for call, duration
			     in enumerate(result['durations_ns'])
			     if call not in skipped]
		ok_durations.extend(durations)
		failed += result['failed_call_count']
		loop_s = result['loop_ns'] / 1e9
		clients.append({
			'index': result['index'],
			'pid': result['resource']['process.pid'],
			'calls': result['calls'],
			'failed_calls': result['failed_call_count'],
			'calls_per_s': result['calls'] / loop_s,
			'cpu_us_per_call': result['cpu_ns'] / result['calls'] / 1e3,
			'max_rss_kib': result['max_rss_kib'],
			'latency': common.latency_summary(durations),
		})

	total_calls = sum(result['calls'] for result in results)
	window_start = min(result['loop_start_unix_ns'] for result in results)
	window_end = max(result['loop_start_unix_ns'] + result['loop_ns']
			 for result in results)
	window_s = (window_end - window_start) / 1e9
	report = {
		'schema': REPORT_SCHEMA,
		'run': {
			'trace_id': config['trace_id'],
			'label': config['label'],
			'defw_major': args.defw_major,
			'go_unix_ns': go_unix_ns,
			'end_unix_ns': end_unix_ns,
		},
		'workload': {
			'id': args.workload,
			'payload_bytes': args.payload_bytes,
			'payload_kind': config['payload_kind'],
			'calls_per_client': args.calls,
			'warmup_per_client': args.warmup,
			'clients': args.clients,
		},
		'transport': {
			'kind': args.transport,
			'env': config['transport_env'],
			# v2 asks Margo for one provider and fails if it
			# cannot have it, so there is nothing to fall back to.
			# v1 under QFw is not checked here.
			'fallback_check': ('not checked' if args.defw_major == 1
					   else 'not applicable'),
		},
		'environment': environment(args, config),
		'latency': common.latency_summary(ok_durations),
		'throughput': {
			'calls_per_s': total_calls / window_s,
			'window_s': window_s,
		},
		'cpu': {
			'client_us_per_call': sum(client['cpu_us_per_call']
						  for client in clients) / len(clients),
			'service_us_per_call': (service['cpu_ns'] / total_calls / 1e3
						if service['cpu_ns'] is not None
						else None),
		},
		'memory': {
			'client_max_rss_kib': max(client['max_rss_kib']
						  for client in clients),
			'service_max_rss_kib': service['max_rss_kib'],
		},
		'failed_calls': failed,
		'clients': clients,
	}
	if args.bulk and ok_durations:
		payload_mib = args.payload_bytes / common.MIB
		busy_s = sum(ok_durations) / 1e9
		report['bulk'] = {
			'payload_mib_per_s': payload_mib * len(ok_durations) / busy_s,
		}
	if args.qpm:
		report['workload'].update({
			'qubits': args.qubits,
			'shots': args.shots,
			'statevector': args.statevector,
			# The one the clients found, which a QFw run names.
			'service_id': results[0].get('service_id'),
		})
		report['qpm'] = qpm_summary(args, results)
	report['wire'] = wire_summary(args, config, results, total_calls,
				      go_unix_ns, end_unix_ns)
	return report


def wire_summary(args, config, results, total_calls, go_unix_ns,
		 end_unix_ns):
	"""Wire bytes per call, or per job. On v2 every client's calls are
	libdefw2's spans in the run's trace. v1 records none, so QFw's client
	counts its own messages there."""
	if args.defw_major == 1:
		counts = {}
		for result in results:
			common.add_counts(counts, result.get('wire') or {})
		return common.wire_summary(counts, total_calls,
					   'client message counts')
	if not config['spans']:
		return None
	counts = common.run_wire_bytes(
		os.path.join(config['run_dir'], 'otlp'), config['trace_id'],
		go_unix_ns, end_unix_ns)
	return common.wire_summary(counts, total_calls, 'client spans')


def qpm_summary(args, results):
	"""A job is async_run plus the read_cq calls it took. Overhead is the
	job less the QPM's own run time, which is what the framework costs:
	the design's qfw.app.job minus backend time. Collect is the read_cq
	that found the completion, which carries a W6 statevector."""
	backend, overhead, collect, polls = [], [], [], []
	for result in results:
		skipped = set(result['failed_calls'])
		for job, duration in enumerate(result['durations_ns']):
			if job in skipped:
				continue
			backend.append(result['backend_ns'][job])
			overhead.append(duration - result['backend_ns'][job])
			collect.append(result['collect_ns'][job])
			polls.append(result['polls'][job])
	summary = {
		'backend': common.latency_summary(backend),
		'overhead': common.latency_summary(overhead),
		'collect': common.latency_summary(collect),
		'polls': {
			'mean': sum(polls) / len(polls) if polls else None,
			'p50': common.percentile(sorted(polls), 0.50),
			'max': max(polls) if polls else None,
		},
	}
	if args.statevector and collect:
		statevector_mib = args.payload_bytes / common.MIB
		summary['statevector_mib_per_s'] = (
			statevector_mib * len(collect) / (sum(collect) / 1e9))
	return summary


def write_run_span(args, config, report, go_unix_ns, end_unix_ns):
	"""The span every call in the run hangs beneath. The calls themselves
	are libdefw2's spans, in its own files beside this one."""
	path = os.path.join(config['run_dir'], 'otlp', 'spans-run.jsonl')
	writer = common.OtlpJsonWriter(path, SCOPE_NAME, SCOPE_VERSION)
	try:
		failed = report['failed_calls']
		root = common.otlp_span(
			config['trace_id'], config['root_span_id'],
			'qfw.bench.run', go_unix_ns, end_unix_ns,
			common.otlp_attributes({
				'qfw.bench.label': config['label'],
				'qfw.bench.workload': args.workload,
				'qfw.bench.payload.bytes': args.payload_bytes,
				'qfw.bench.payload.kind': config['payload_kind'],
				'qfw.bench.calls': args.calls,
				'qfw.bench.warmup': args.warmup,
				'qfw.bench.clients': args.clients,
				'qfw.transport.kind': args.transport,
				'qfw.rpc.api': QPM_API if args.qpm else SERVICE_API,
			}),
			kind=common.SPAN_KIND_INTERNAL,
			error='{} calls failed'.format(failed) if failed else None)
		resource = common.process_attributes('defw2-bench')
		resource.update({
			'qfw.transport.kind': args.transport,
			'qfw.defw.major': args.defw_major,
			'qfw.defw.revision': args.defw_revision,
			'qfw.bench.harness.revision': config['harness_revision'],
			'container.image.name': args.image,
			'qfw.slurm.job_id': os.environ.get('SLURM_JOB_ID'),
			'qfw.libfabric.version': report['environment']['libfabric'],
		})
		writer.write(resource, [root])
	finally:
		writer.close()


def print_table(report):
	latency = report['latency']
	print()
	print('{}  {} clients  {}'.format(report['run']['label'],
					  report['workload']['clients'],
					  report['transport']['kind']))
	print('  latency p50 {:.3f} ms, p99 {:.3f} ms, max {:.3f} ms'.format(
		latency['p50_us'] / 1e3, latency['p99_us'] / 1e3,
		latency['max_us'] / 1e3))
	print('  throughput  {:.1f} calls/s over {:.3f} s'.format(
		report['throughput']['calls_per_s'],
		report['throughput']['window_s']))
	service_cpu = report['cpu']['service_us_per_call']
	print('  cpu per call  client {:.3f} ms, service {}'.format(
		report['cpu']['client_us_per_call'] / 1e3,
		'{:.3f} ms'.format(service_cpu / 1e3)
		if service_cpu is not None else 'unknown'))
	print('  peak rss  client {} MiB, service {} MiB'.format(
		report['memory']['client_max_rss_kib'] // 1024,
		(report['memory']['service_max_rss_kib'] or 0) // 1024))
	if 'bulk' in report:
		print('  bulk  {:.1f} MiB/s'.format(
			report['bulk']['payload_mib_per_s']))
	if report.get('wire'):
		wire = report['wire']
		print('  wire bytes per {}  {:.0f}, {:.0f} in requests and '
		      '{:.0f} in answers, {:.1f} RPCs'.format(
			      'job' if 'qpm' in report else 'call',
			      wire['bytes_per_call'],
			      wire['request_bytes_per_call'],
			      wire['response_bytes_per_call'],
			      wire['rpcs_per_call']))
	if 'qpm' in report:
		qpm = report['qpm']
		print('  per job  overhead p50 {:.3f} ms, p99 {:.3f} ms, '
		      'backend p50 {:.3f} ms'.format(
			      qpm['overhead']['p50_us'] / 1e3,
			      qpm['overhead']['p99_us'] / 1e3,
			      qpm['backend']['p50_us'] / 1e3))
		print('  collect p50 {:.3f} ms, polls p50 {}, max {}'.format(
			qpm['collect']['p50_us'] / 1e3, qpm['polls']['p50'],
			qpm['polls']['max']))
		if 'statevector_mib_per_s' in qpm:
			print('  statevector  {:.1f} MiB/s on collect'.format(
				qpm['statevector_mib_per_s']))
	if report['failed_calls']:
		print('  {} calls FAILED'.format(report['failed_calls']))


def main(argv):
	args = parse_args(argv)
	if not args.force:
		check_memory(args)

	args.defw_major = 2
	directory = args.directory
	if args.qpm and args.qfw_run_dir:
		qfw_run = read_qfw_run(args.qfw_run_dir)
		directory = directory or qfw_run['directory']
		if args.client == 'qfw':
			args.defw_major = qfw_run['defw_major']
	needed = []
	if args.client == 'c':
		needed.append('defw2-bench')
	if not args.qpm:
		needed.append('defw2-echo')
	binaries = {name: find_binary(name, args.bin_dir) for name in needed}
	trace_id = os.urandom(16).hex()
	root_span_id = os.urandom(8).hex()
	run_dir = make_run_dir(args, trace_id)
	config = write_config(args, run_dir, trace_id, root_span_id, binaries)
	print('run {}'.format(run_dir))

	service = None
	clients = []
	try:
		if args.qpm:
			# QFw serves the QPM, so there is nothing to start.
			address = directory
			print('QPM {} in the directory at {}'.format(
				args.service_id or 'of the run', address))
		else:
			service, address = start_service(args, config,
							 binaries)
			print('echo service at {}'.format(address))
		clients = start_clients(args, config, binaries, address)
		wait_for_ready(args, config, clients, service)

		service_cpu_before = (proc_cpu_ns(service.pid)
				      if service is not None else None)
		go_unix_ns = time.time_ns()
		with open(common.go_path(run_dir), 'w', encoding='ascii'):
			pass
		wait_for_clients(args, config, clients, service)
		end_unix_ns = time.time_ns()

		# The QPM runs on another node, so its usage is not this
		# launcher's to read.
		service_cpu_after = (proc_cpu_ns(service.pid)
				     if service is not None else None)
		usage = {
			'cpu_ns': (None if service_cpu_after is None
				   else service_cpu_after - service_cpu_before),
			'max_rss_kib': (proc_peak_rss_kib(service.pid)
					if service is not None else None),
		}
		stop(service)

		results = read_results(args, config)
		report = build_report(args, config, results, usage,
				      go_unix_ns, end_unix_ns)
		common.write_json(os.path.join(run_dir, 'summary.json'), report,
				  indent=2)
		if config['spans']:
			write_run_span(args, config, report, go_unix_ns,
				       end_unix_ns)
		print_table(report)
		return 1 if report['failed_calls'] else 0
	finally:
		for client in clients:
			stop(client)
		stop(service)


if __name__ == '__main__':
	try:
		sys.exit(main(sys.argv[1:]))
	except KeyboardInterrupt:
		sys.exit(128 + signal.SIGINT)
