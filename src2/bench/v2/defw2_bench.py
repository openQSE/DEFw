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
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone

BENCH_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, BENCH_DIR)

import defw_bench_common as common  # noqa: E402

SCOPE_NAME = 'defw.bench.v2'
SCOPE_VERSION = '0.1'
REPORT_SCHEMA = 'defw-bench-summary/1'
SERVICE_API = 'qfw.echo'
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
	# message has to go through registered memory in any case.
	args.bulk = args.workload == 'W3' or args.payload_bytes > 4 * 1024 * 1024
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
		available = (os.sysconf('SC_AVPHYS_PAGES') *
			     os.sysconf('SC_PAGE_SIZE'))
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


def make_run_dir(args, trace_id):
	stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
	name = ('{}-v2-{}-{}-c{}-{}'.format(stamp, args.workload,
					    args.transport.replace('+', ''),
					    args.clients, trace_id[:8]))
	run_dir = os.path.join(os.path.abspath(args.out), name)
	for sub in ('control', 'results', 'logs', 'otlp'):
		os.makedirs(os.path.join(run_dir, sub))
	return run_dir


def write_config(args, run_dir, trace_id, root_span_id, binaries):
	label = args.label or 'v2-{}-{}-{}-c{}'.format(
		args.workload, args.transport,
		common.format_size(args.payload_bytes).replace(' ', ''),
		args.clients)
	config = {
		'schema': 1,
		'defw_major': 2,
		'trace_id': trace_id,
		'root_span_id': root_span_id,
		'label': label,
		'workload': args.workload,
		'payload_bytes': args.payload_bytes,
		'payload_kind': 'bulk' if args.bulk else 'bytes',
		'calls': args.calls,
		'warmup': args.warmup,
		'clients': args.clients,
		'transport': args.transport,
		'transport_env': {'DEFW2_ADDRESS': args.transport + '://'},
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


def process_env(args, run_dir, agent):
	env = dict(os.environ)
	env.update({
		'DEFW2_ADDRESS': args.transport + '://',
		'DEFW2_TELEMETRY_DIR': os.path.join(run_dir, 'otlp'),
		'DEFW_AGENT_NAME': agent,
		'DEFW_LOG_LEVEL': args.log_level,
	})
	if args.no_spans:
		env.pop('DEFW2_PROFILE', None)
	else:
		env['DEFW2_PROFILE'] = '1'
	return env


def start_service(args, config, binaries):
	"""Start the echo service and read back the address it prints."""
	run_dir = config['run_dir']
	env = process_env(args, run_dir, 'bench-echo')
	if args.rpc_threads is not None:
		env['DEFW2_RPC_THREADS'] = str(args.rpc_threads)
	log = open(os.path.join(run_dir, 'logs', 'echo.log'), 'w',
		   encoding='utf-8')
	service = subprocess.Popen([binaries['defw2-echo'], 'serve'],
				   stdout=subprocess.PIPE, stderr=log,
				   env=env, text=True)
	address = service.stdout.readline().strip()
	if not address:
		service.wait(timeout=STOP_GRACE_S)
		fail('the echo service did not start, see logs/echo.log')
	return service, address


def start_clients(args, config, binaries, address):
	run_dir = config['run_dir']
	traceparent = '00-{}-{}-01'.format(config['trace_id'],
					   config['root_span_id'])
	clients = []
	for index in range(args.clients):
		command = [
			binaries['defw2-bench'],
			'--address', address,
			'--index', str(index),
			'--calls', str(args.calls),
			'--warmup', str(args.warmup),
			'--payload', str(args.payload_bytes),
			'--traceparent', traceparent,
			'--ready', common.ready_path(run_dir, index),
			'--go', common.go_path(run_dir),
			'--result', common.result_path(run_dir, index),
			'--wait-s', str(int(args.timeout)),
		]
		if args.bulk:
			command.append('--bulk')
		log = open(os.path.join(run_dir, 'logs',
					'client-{}.log'.format(index)),
			   'w', encoding='utf-8')
		env = process_env(args, run_dir, 'bench-client-{}'.format(index))
		clients.append(subprocess.Popen(command, stdout=log, stderr=log,
						env=env, text=True))
	return clients


def check_service(service, run_dir):
	"""A service that stops takes the run with it, and every client then
	sits in its own timeout, so say so at once."""
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


def stop(process):
	if process is None or process.poll() is not None:
		return
	process.terminate()
	try:
		process.wait(timeout=STOP_GRACE_S)
	except subprocess.TimeoutExpired:
		process.kill()
		process.wait(timeout=STOP_GRACE_S)


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
			'defw_major': 2,
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
			'fallback_check': 'not applicable',
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
	return report


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
				'qfw.rpc.api': SERVICE_API,
			}),
			kind=common.SPAN_KIND_INTERNAL,
			error='{} calls failed'.format(failed) if failed else None)
		resource = common.process_attributes('defw2-bench')
		resource.update({
			'qfw.transport.kind': args.transport,
			'qfw.defw.major': 2,
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
	if report['failed_calls']:
		print('  {} calls FAILED'.format(report['failed_calls']))


def main(argv):
	args = parse_args(argv)
	if not args.force:
		check_memory(args)

	binaries = {name: find_binary(name, args.bin_dir)
		    for name in ('defw2-echo', 'defw2-bench')}
	trace_id = os.urandom(16).hex()
	root_span_id = os.urandom(8).hex()
	run_dir = make_run_dir(args, trace_id)
	config = write_config(args, run_dir, trace_id, root_span_id, binaries)
	print('run {}'.format(run_dir))

	service = None
	clients = []
	try:
		service, address = start_service(args, config, binaries)
		print('echo service at {}'.format(address))
		clients = start_clients(args, config, binaries, address)
		wait_for_ready(args, config, clients, service)

		service_cpu_before = proc_cpu_ns(service.pid)
		go_unix_ns = time.time_ns()
		with open(common.go_path(run_dir), 'w', encoding='ascii'):
			pass
		wait_for_clients(args, config, clients, service)
		end_unix_ns = time.time_ns()

		service_cpu_after = proc_cpu_ns(service.pid)
		usage = {
			'cpu_ns': (None if service_cpu_after is None
				   else service_cpu_after - service_cpu_before),
			'max_rss_kib': proc_peak_rss_kib(service.pid),
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
