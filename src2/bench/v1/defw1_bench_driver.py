"""
Runs inside the DEFw v1 directory service that defw1_bench.py starts.

The driver spawns the svc_test_echo service with DEFw's own helper, starts
the client processes, and waits until every client has connected and warmed
up. It then releases them together and waits for them to finish. From their
measurements it writes the run's report: summary.json, OTLP/JSON spans under
otlp/, and a short table on stdout.

Service CPU time and peak memory are read from /proc around the measured
window. That works because defw_spawn_services starts the service on this
node. For W4 the service measured is the directory, which is this process,
so its figures include this driver's own waiting, a poll every 50 ms.
"""

import json
import os
import re
import signal
import socket
import subprocess
import sys
import time
import traceback

import defw
from defw_app_util import defw_shutdown_services, defw_spawn_services

import defw_bench_common as common

SERVICE_MODULE = 'svc_test_echo'
SERVICE_API = 'TestEcho'
SERVICE_METHOD = 'echo'
DIRECTORY_API = 'svc_dirsvc'
DIRECTORY_METHOD = 'resolve_services'
CLIENT_MODULES = 'api_dirsvc,api_test_echo'
SCOPE_NAME = 'defw.bench.v1'
SCOPE_VERSION = '0.1'
REPORT_SCHEMA = 'defw-bench-summary/1'
POLL_S = 0.05
STOP_GRACE_S = 5
STDERR_TAIL_LINES = 15

# DEFw logs this at error level when it cannot provide the requested
# transport and carries on over tcp instead.
FALLBACK_MARKER = 'requested but unavailable'
ANSI_ESCAPE = re.compile(r'\x1b\[[0-9;]*m')

CLIENT_BOOTSTRAP = (
	'import sys\n'
	'sys.path[:0] = {paths!r}\n'
	'import defw1_bench_client\n'
	'raise SystemExit(defw1_bench_client.main())\n'
)


class BenchError(Exception):
	pass


def proc_cpu_ns(pid):
	"""User plus system CPU time of a process, from /proc/<pid>/stat."""
	with open(f'/proc/{pid}/stat', encoding='ascii') as stream:
		stat = stream.read()
	# The command name can hold spaces, so split after its closing paren.
	# utime and stime are fields 14 and 15 of the whole line.
	fields = stat[stat.rindex(')') + 2:].split()
	ticks = int(fields[11]) + int(fields[12])
	return ticks * 1_000_000_000 // os.sysconf('SC_CLK_TCK')


def proc_peak_rss_kib(pid):
	with open(f'/proc/{pid}/status', encoding='ascii') as stream:
		for line in stream:
			if line.startswith('VmHWM:'):
				return int(line.split()[1])
	return None


def loaded_defw_module(config):
	"""The defw module this process loaded, which must come from the
	installation under test rather than another one on the Python path."""
	module = os.path.realpath(defw.__file__)
	root = os.path.realpath(config['defw_path'])
	if os.path.commonpath([module, root]) != root:
		raise BenchError(f'loaded DEFw from {module}, not from {root}')
	return module


def transport_fallbacks(run_dir):
	"""Processes of this run whose DEFw runtime fell back from the requested
	transport. Every process logs to a defw_out.log under the run
	directory, and the fallback is logged whatever the log level."""
	found = []
	for directory, _, files in os.walk(run_dir):
		if 'defw_out.log' not in files:
			continue
		path = os.path.join(directory, 'defw_out.log')
		with open(path, encoding='utf-8', errors='replace') as stream:
			for line in stream:
				if FALLBACK_MARKER in line:
					message = ANSI_ESCAPE.sub('', line).strip()
					where = os.path.relpath(directory, run_dir)
					found.append(f'{where}: {message}')
					break
	return found


def stderr_tail(index, run_dir):
	path = os.path.join(common.client_dir(run_dir, index), 'stderr.log')
	try:
		with open(path, encoding='utf-8', errors='replace') as stream:
			lines = stream.readlines()[-STDERR_TAIL_LINES:]
	except OSError:
		return ''
	return ''.join(lines).rstrip()


class BenchRun:
	def __init__(self, config):
		self.config = config
		self.run_dir = config['run_dir']
		self.services = []
		self.clients = []
		self.pids = {}
		self.seen = None

	def record_pid(self, role, pid):
		# The launcher reads this to clean up if the run is abandoned.
		self.pids[role] = pid
		common.write_json(
			os.path.join(self.run_dir, 'control', 'pids.json'), self.pids)

	def start_client(self, index):
		config = self.config
		log_dir = common.client_dir(self.run_dir, index)
		os.makedirs(log_dir, exist_ok=True)
		port = config['client_port_base'] + 2 * index
		env = dict(os.environ)
		env.pop('DEFW_EXPERIMENT_PORT_BASE', None)
		env.update({
			'DEFW_AGENT_NAME':
				f'bench-client-{index}-{config["trace_id"][:8]}',
			'DEFW_AGENT_TYPE': 'agent',
			'DEFW_SHELL_TYPE': 'cmdline',
			'DEFW_LISTEN_PORT': str(port),
			'DEFW_TELNET_PORT': str(port + 1),
			'DEFW_PARENT_NAME': defw.me.my_name(),
			'DEFW_PARENT_HOSTNAME': defw.me.my_hostname(),
			'DEFW_PARENT_ADDR': defw.me.my_listenaddress(),
			'DEFW_PARENT_PORT': str(defw.me.my_listenport()),
			'DEFW_DISABLE_DIRSVC': 'no',
			'DEFW_ONLY_LOAD_MODULE': CLIENT_MODULES,
			'DEFW_LOG_DIR': log_dir,
			common.CLIENT_INDEX_ENV: str(index),
		})
		bootstrap = CLIENT_BOOTSTRAP.format(
			paths=[config['v1_dir'], config['bench_dir']])
		stdout = open(os.path.join(log_dir, 'stdout.log'), 'w')
		stderr = open(os.path.join(log_dir, 'stderr.log'), 'w')
		try:
			pin = common.pinned_to(set(config['client_cpus']))
			process = subprocess.Popen(
				[config['defwp'], '-c', bootstrap], env=env, cwd=log_dir,
				stdout=stdout, stderr=stderr,
				start_new_session=True, preexec_fn=pin)
		finally:
			# The child has its own descriptors for both files.
			stdout.close()
			stderr.close()
		self.clients.append((index, process))
		self.record_pid(f'client-{index}', process.pid)

	def check_clients(self, indices):
		for index, process in self.clients:
			if index not in indices:
				continue
			rc = process.poll()
			if rc is not None:
				raise BenchError(
					f'client {index} exited with {rc}\n'
					f'{stderr_tail(index, self.run_dir)}')

	def wait_until_ready(self):
		deadline = time.monotonic() + self.config['timeout_s']
		pending = {index for index, _ in self.clients}
		while True:
			pending = {index for index in pending if not os.path.exists(
				common.ready_path(self.run_dir, index))}
			if not pending:
				return
			self.check_clients(pending)
			if time.monotonic() > deadline:
				raise BenchError(f'clients {sorted(pending)} were not '
						 'ready before the timeout')
			time.sleep(POLL_S)

	def wait_for_clients(self):
		deadline = time.monotonic() + self.config['timeout_s']
		running = dict(self.clients)
		while running:
			for index, process in list(running.items()):
				rc = process.poll()
				if rc is None:
					continue
				del running[index]
				if rc != 0:
					raise BenchError(
						f'client {index} failed with {rc}\n'
						f'{stderr_tail(index, self.run_dir)}')
			if running and time.monotonic() > deadline:
				raise BenchError(f'clients {sorted(running)} did not '
						 'finish before the timeout')
			if running:
				time.sleep(POLL_S)

	def load_results(self):
		results = []
		for index, _ in self.clients:
			path = common.result_path(self.run_dir, index)
			try:
				with open(path, encoding='utf-8') as stream:
					results.append(json.load(stream))
			except (OSError, ValueError) as exc:
				raise BenchError(f'no result from client {index}: {exc}')
		return results

	def check_transport(self):
		if self.config['transport'] == 'tcp':
			return
		fallbacks = transport_fallbacks(self.run_dir)
		if fallbacks:
			raise BenchError(
				f'DEFw did not provide {self.config["transport"]}, so the '
				f'run would measure tcp:\n' + '\n'.join(fallbacks))

	def stop_services(self):
		services, self.services = self.services, []
		if services:
			defw_shutdown_services(services)

	def stop(self):
		for _, process in self.clients:
			if process.poll() is None:
				try:
					os.killpg(process.pid, signal.SIGTERM)
				except ProcessLookupError:
					pass
		for _, process in self.clients:
			try:
				process.wait(timeout=STOP_GRACE_S)
			except subprocess.TimeoutExpired:
				try:
					os.killpg(process.pid, signal.SIGKILL)
				except ProcessLookupError:
					pass
		try:
			self.stop_services()
		except Exception as exc:
			print(f'defw1_bench: stopping the echo service failed: {exc}',
			      flush=True)

	def execute(self):
		config = self.config
		defw_module = loaded_defw_module(config)
		self.services = defw_spawn_services({
			'module': SERVICE_MODULE,
			'agent_name': f'bench-echo-{config["trace_id"][:8]}',
		})
		self.record_pid('service', self.services[0].pid)
		# W4's clients call the directory, which is this process.
		service_pid = (os.getpid() if config.get('resolve')
			       else self.services[0].pid)

		for index in range(config['clients']):
			self.start_client(index)
		self.wait_until_ready()
		# Checked here to stop a mislabelled run early, and again at the
		# end, once every process has flushed its log.
		self.check_transport()
		self.seen = common.seen(
			[os.getpid(), self.services[0].pid],
			[process.pid for _, process in self.clients])

		service_cpu_before = proc_cpu_ns(service_pid)
		go_unix_ns = time.time_ns()
		with open(common.go_path(self.run_dir), 'w', encoding='ascii'):
			pass
		self.wait_for_clients()
		end_unix_ns = time.time_ns()
		service = {
			'pid': service_pid,
			'cpu_ns': proc_cpu_ns(service_pid) - service_cpu_before,
			'max_rss_kib': proc_peak_rss_kib(service_pid),
		}
		self.stop_services()
		self.check_transport()

		results = self.load_results()
		for result in results:
			if result['defw_module'] != defw_module:
				raise BenchError(
					f'client {result["index"]} loaded DEFw from '
					f'{result["defw_module"]}, not {defw_module}')
		report = build_report(config, results, service,
				      go_unix_ns, end_unix_ns, defw_module,
				      self.seen)
		common.write_json(os.path.join(self.run_dir, 'summary.json'),
				  report, indent=2)
		write_spans(config, results, report, go_unix_ns, end_unix_ns)
		print_report(report, self.run_dir)
		return 0 if report['failed_calls'] == 0 else 1


def environment(config, defw_module, seen=None):
	return {
		'hostname': socket.gethostname(),
		'cpu_count': os.cpu_count(),
		'python': sys.version.split()[0],
		'libfabric': common.libfabric_version(),
		'image': config['image'],
		'defw_path': config['defw_path'],
		'defw_module': defw_module,
		'defw_revision': config['defw_revision'],
		'harness_revision': config['harness_revision'],
		'slurm_job_id': os.environ.get('SLURM_JOB_ID'),
		'slurm_nodelist': os.environ.get('SLURM_JOB_NODELIST'),
		'defw_log_level': config['log_level'],
		'defw_py_log_level': config['py_log_level'],
		'rma_attachments': os.environ.get('DEFW_RMA_ATTACHMENTS'),
		'rma_threshold': os.environ.get('DEFW_RMA_THRESHOLD'),
		'placement': dict(config.get('placement') or {}, seen=seen),
	}


def build_report(config, results, service, go_unix_ns, end_unix_ns,
		 defw_module, seen=None):
	ok_durations = []
	clients = []
	failed = 0
	for result in results:
		failed_calls = set(result['failed_calls'])
		durations = [duration for call, duration
			     in enumerate(result['durations_ns'])
			     if call not in failed_calls]
		ok_durations.extend(durations)
		failed += len(failed_calls)
		loop_s = result['loop_ns'] / 1e9
		clients.append({
			'index': result['index'],
			'pid': result['resource']['process.pid'],
			'calls': result['calls'],
			'failed_calls': len(failed_calls),
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
			'defw_major': config['defw_major'],
			'go_unix_ns': go_unix_ns,
			'end_unix_ns': end_unix_ns,
		},
		'workload': {
			'id': config['workload'],
			'payload_bytes': config['payload_bytes'],
			'payload_kind': config['payload_kind'],
			'calls_per_client': config['calls'],
			'warmup_per_client': config['warmup'],
			'clients': config['clients'],
		},
		'transport': {
			'kind': config['transport'],
			'env': config['transport_env'],
			# Only reached when no process logged a fallback to tcp.
			'fallback_check': ('not needed' if config['transport'] == 'tcp'
					   else 'passed'),
		},
		'environment': environment(config, defw_module, seen),
		'latency': common.latency_summary(ok_durations),
		'throughput': {
			'calls_per_s': total_calls / window_s,
			'window_s': window_s,
		},
		'cpu': {
			'client_us_per_call': sum(client['cpu_us_per_call']
						  for client in clients) / len(clients),
			'service_us_per_call': service['cpu_ns'] / total_calls / 1e3,
		},
		'memory': {
			'client_max_rss_kib': max(client['max_rss_kib']
						  for client in clients),
			'service_max_rss_kib': service['max_rss_kib'],
		},
		'failed_calls': failed,
		'clients': clients,
	}
	if config['workload'] == 'W3' and ok_durations:
		payload_mib = config['payload_bytes'] / common.MIB
		busy_s = sum(ok_durations) / 1e9
		report['bulk'] = {
			'payload_mib_per_s': payload_mib * len(ok_durations) / busy_s,
		}
	counts = {}
	for result in results:
		common.add_counts(counts, result['wire'])
	report['wire'] = common.wire_summary(counts, total_calls,
					     'client message counts')
	return report


def write_spans(config, results, report, go_unix_ns, end_unix_ns):
	"""One trace for the run: a qfw.bench.run root span, and beneath it a
	qfw.transport.rpc span for every measured call."""
	trace_id = config['trace_id']
	root_span_id = config['root_span_id']
	writer = common.OtlpJsonWriter(
		os.path.join(config['run_dir'], 'otlp', 'spans.jsonl'),
		SCOPE_NAME, SCOPE_VERSION)
	try:
		failed = report['failed_calls']
		root = common.otlp_span(
			trace_id, root_span_id, 'qfw.bench.run', go_unix_ns, end_unix_ns,
			common.otlp_attributes({
				'qfw.bench.label': config['label'],
				'qfw.bench.workload': config['workload'],
				'qfw.bench.payload.bytes': config['payload_bytes'],
				'qfw.bench.payload.kind': config['payload_kind'],
				'qfw.bench.calls': config['calls'],
				'qfw.bench.warmup': config['warmup'],
				'qfw.bench.clients': config['clients'],
				'qfw.transport.kind': config['transport'],
			}),
			kind=common.SPAN_KIND_INTERNAL,
			error=f'{failed} calls failed' if failed else None)
		resource = common.process_attributes('defw1-bench-driver')
		resource.update({
			'qfw.defw.major': config['defw_major'],
			'qfw.defw.revision': config['defw_revision'],
			'qfw.bench.harness.revision': config['harness_revision'],
			'container.image.name': config['image'],
			'qfw.slurm.job_id': os.environ.get('SLURM_JOB_ID'),
			'qfw.libfabric.version': report['environment']['libfabric'],
		})
		writer.write(resource, [root])
		if not config['spans']:
			return

		resolve = config.get('resolve')
		call_attributes = common.otlp_attributes({
			'qfw.rpc.api': (DIRECTORY_API if resolve
					else SERVICE_API),
			'qfw.rpc.method': (DIRECTORY_METHOD if resolve
					   else SERVICE_METHOD),
			'qfw.transport.kind': config['transport'],
			'qfw.bench.payload.bytes': config['payload_bytes'],
		})
		for result in results:
			index = result['index']
			failed_calls = set(result['failed_calls'])
			messages = result['failure_messages']
			loop_start = result['loop_start_unix_ns']
			spans = []
			timings = zip(result['start_offsets_ns'], result['durations_ns'])
			for call, (offset, duration) in enumerate(timings):
				start = loop_start + offset
				error = None
				if call in failed_calls:
					error = messages.get(str(call), 'call failed')
				spans.append(common.otlp_span(
					trace_id, f'{index + 1:04x}{call + 1:012x}',
					'qfw.transport.rpc', start, start + duration,
					call_attributes, parent_span_id=root_span_id,
					error=error))
			writer.write(result['resource'], spans)
	finally:
		writer.close()


def print_report(report, run_dir):
	workload = report['workload']
	latency = report['latency']
	cpu = report['cpu']
	memory = report['memory']
	print(f'DEFw v1 {workload["id"]}, {report["transport"]["kind"]}, '
	      f'{workload["clients"]} client(s), '
	      f'{common.format_size(workload["payload_bytes"])} '
	      f'{workload["payload_kind"]} payload, '
	      f'{workload["calls_per_client"]} calls each')
	if latency['count']:
		print(f'  round trip (us)  p50 {latency["p50_us"]:.1f}  '
		      f'p90 {latency["p90_us"]:.1f}  p99 {latency["p99_us"]:.1f}  '
		      f'max {latency["max_us"]:.1f}  mean {latency["mean_us"]:.1f}')
	print(f'  throughput       '
	      f'{report["throughput"]["calls_per_s"]:.1f} calls/s')
	if 'bulk' in report:
		print(f'  bulk             '
		      f'{report["bulk"]["payload_mib_per_s"]:.1f} MiB/s')
	print(f'  CPU per call (us) client {cpu["client_us_per_call"]:.1f}  '
	      f'service {cpu["service_us_per_call"]:.1f}')
	if report.get('wire'):
		print(f'  wire bytes/call  '
		      f'{report["wire"]["bytes_per_call"]:.0f}, '
		      f'{report["wire"]["request_bytes_per_call"]:.0f} in '
		      f'requests and '
		      f'{report["wire"]["response_bytes_per_call"]:.0f} in '
		      f'answers')
	service_rss = memory['service_max_rss_kib']
	service_mib = f'{service_rss / 1024:.1f}' if service_rss else 'unknown'
	print(f'  peak RSS (MiB)   client '
	      f'{memory["client_max_rss_kib"] / 1024:.1f}  service {service_mib}')
	print(f'  failed calls     {report["failed_calls"]}')
	print(f'  report           {os.path.join(run_dir, "summary.json")}',
	      flush=True)


def main():
	run = None
	try:
		run = BenchRun(common.load_config())
		return run.execute()
	except BenchError as exc:
		print(f'defw1_bench: {exc}', flush=True)
		return 1
	except Exception:
		# defwp exits 0 when code it runs as a directory service raises,
		# so every failure has to become an explicit exit status here.
		traceback.print_exc()
		return 1
	finally:
		if run:
			run.stop()
