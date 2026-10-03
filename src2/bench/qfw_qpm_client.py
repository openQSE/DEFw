#!/usr/bin/env python3
"""W5 and W6 through QFw's own client code.

QFw's QPMResolver finds the QPM and binds its APIs, as QFw's examples and
its Qiskit backend do, and every job is QFw's v1 calls: async_run with a
circuit dictionary, then read_cq until the completion is ready. A W6 job
also decodes the statevector, as QFw's Qiskit backend does, because an
application has not got its result until it has.

Run it with qfw-srun. That starts it on the run's DEFw: v1's defw-python, or
defw2-python, which runs the same code on v2 through defw2.compat. So one
script measures what a QFw application gets from each.

It takes the typed clients' arguments and writes their result file, so the
launcher runs it the way it runs them. --address and --service-id are
accepted and ignored, because QFw finds its directory and its QPM from the
run.
"""

import argparse
import json
import math
import os
import resource
import sys
import time

from defw_app_util import defw_get_directory_service
from qfw_qiskit.qpm_resolver import QPMResolver
from qfw_qiskit.qpm_selection import qpm_selection_for_provider
from util.qpm.statevector import decode_statevector_payload

PROVIDER = 'fake-iqm'
MAX_FAILURE_MESSAGES = 10
POLL_S = 0.005
# The points the typed clients check, so all three check the same thing.
QPM_CHECK_POINTS = 256
# Tries at the checked job, as in the typed clients.
QPM_CHECK_TRIES = 3
QPM_PHI = (math.sqrt(5.0) - 1.0) / 2.0
QASM = ('OPENQASM 2.0;\ninclude "qelib1.inc";\nqreg q[{0}];\n'
	'creg c[{0}];\nh q[0];\nmeasure q -> c;\n')


def parse_args(argv):
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--address')
	parser.add_argument('--service-id')
	parser.add_argument('--result', required=True)
	parser.add_argument('--index', type=int, default=0)
	parser.add_argument('--calls', type=int, default=1000)
	parser.add_argument('--warmup', type=int, default=20)
	parser.add_argument('--qubits', type=int, default=4)
	parser.add_argument('--shots', type=int, default=1024)
	parser.add_argument('--statevector', action='store_true')
	parser.add_argument('--qpm', action='store_true',
			    help='accepted for the launcher, always on')
	parser.add_argument('--traceparent')
	parser.add_argument('--ready')
	parser.add_argument('--go')
	parser.add_argument('--timeout-ms', type=int, default=60000)
	parser.add_argument('--wait-s', type=int, default=300)
	return parser.parse_args(argv)


def wait_for(path, seconds):
	if not path:
		return True
	deadline = time.monotonic() + seconds
	while not os.path.exists(path):
		if time.monotonic() > deadline:
			return False
		time.sleep(POLL_S)
	return True


def cpu_ns(usage):
	return int((usage.ru_utime + usage.ru_stime) * 1e9)


def statevector_matches(amplitudes, qubits):
	count = 1 << qubits
	if len(amplitudes) != count:
		return False
	stride = count // QPM_CHECK_POINTS + 1
	norm = 1.0 / math.sqrt(count)
	for k in [*range(0, count, stride), count - 1]:
		phase = 2.0 * math.pi * math.fmod(k * QPM_PHI, 1.0)
		value = complex(amplitudes[k])
		if abs(value.real - math.cos(phase) * norm) > 1e-9 * norm or \
		   abs(value.imag - math.sin(phase) * norm) > 1e-9 * norm:
			return False
	return True


def connect(timeout_s):
	"""The QPM's admission and execution APIs, found as QFw finds them."""
	selection = qpm_selection_for_provider(PROVIDER,
					       default_provider=PROVIDER)
	resolver = QPMResolver.from_environment(
		dirsvc=defw_get_directory_service())
	request = {
		'service_type': 'qfw.qpm',
		'qpm_type': selection['qpm_type'],
		'qpm_capabilities': selection['qpm_capabilities'],
		'provider': selection['provider'],
		'timeout': timeout_s,
	}
	resolved, admission = resolver.resolve_and_connect(
		binding_name='admission', **request)
	return (resolved.service_id, admission,
		resolver.connect(binding_name='execution', **request))


def qpm_job(execution, args, reservation_id):
	"""One job, within the call timeout. Returns what went wrong, or None,
	with the read_cq that collected the job and its decode, the QPM's own
	run time, the number of polls and the decoded statevector."""
	clock = time.perf_counter_ns
	info = {'qasm': QASM.format(args.qubits), 'num_qubits': args.qubits,
		'num_shots': args.shots, 'shots': args.shots}
	if args.statevector:
		info['return_statevector'] = True
	submitted = execution.async_run(info, reservation_id=reservation_id)
	polls = 0
	deadline = clock() + args.timeout_ms * 1000000
	while True:
		started = clock()
		done = execution.read_cq(cid=submitted['cid'],
					 reservation_id=reservation_id)
		polls += 1
		if done.get('completion_ready'):
			break
		if clock() > deadline:
			return ('the job did not complete within the call '
				'timeout', clock() - started, 0, polls, None)
	amplitudes = None
	if args.statevector:
		payload = (done.get('result') or {}).get('statevector')
		amplitudes = decode_statevector_payload(payload)
	collect = clock() - started
	why = None
	if done.get('outcome') != 'COMPLETED':
		why = 'the job did not complete'
	# A QPM that does not say how long it ran counts as no time.
	backend = int(done.get('observed_fake_runtime_ns') or 0)
	return why, collect, backend, polls, amplitudes


def main(argv):
	args = parse_args(argv)
	clock = time.perf_counter_ns
	service_id, admission, execution = connect(args.timeout_ms / 1000)
	decision = admission.reserve(request={
		'owner': {'user': 'qfw-bench'},
		'job_id': 'qfw-bench-{}'.format(args.index),
		'allocation_id': 'qfw-bench-{}'.format(args.index),
		'num_qubits': args.qubits,
		'workload_kind': 'quantum',
		'walltime_ns': 3600 * 10**9,
		'ttl_ns': 3600 * 10**9,
		'run_context': {'operation': 'async_run'},
		'task_class': {'count': args.calls + args.warmup + 1,
			       'qubit_count': args.qubits, 'depth': 1,
			       'one_q_gate_count': 1, 'two_q_gate_count': 0,
			       'shots': args.shots,
			       'measurement_count': args.qubits},
	})
	rid = decision.get('reservation_id')
	if decision.get('status') != 'accepted' or not rid:
		print('the reservation was not accepted: {}'.format(decision),
		      file=sys.stderr)
		return 1

	try:
		for _ in range(QPM_CHECK_TRIES):
			try:
				why, _, _, _, amplitudes = qpm_job(
					execution, args, rid)
			except Exception as exc:		# noqa: BLE001
				why = '{}: {}'.format(type(exc).__name__, exc)
			if why is None and args.statevector and \
			   not statevector_matches(amplitudes, args.qubits):
				why = "the statevector is not the fake's"
			if why is None:
				break
			print('the checked job failed: {}'.format(why),
			      file=sys.stderr)
		if why is not None:
			return 1
		for _ in range(args.warmup):
			try:
				qpm_job(execution, args, rid)
			except Exception:			# noqa: BLE001
				pass

		if args.ready:
			with open(args.ready, 'w', encoding='ascii'):
				pass
		if not wait_for(args.go, args.wait_s):
			print('no go signal at {}'.format(args.go),
			      file=sys.stderr)
			return 1

		durations = [0] * args.calls
		collects = [0] * args.calls
		backends = [0] * args.calls
		polls = [0] * args.calls
		failed = []
		messages = {}
		moved = 0
		before = resource.getrusage(resource.RUSAGE_SELF)
		loop_start_unix_ns = time.time_ns()
		loop_start = clock()
		for job in range(args.calls):
			started = clock()
			amplitudes = None
			try:
				why, collects[job], backends[job], polls[job], \
					amplitudes = qpm_job(execution, args, rid)
			except Exception as exc:		# noqa: BLE001
				why = '{}: {}'.format(type(exc).__name__, exc)
			durations[job] = clock() - started
			# Checked after the clock stops.
			if why is None and args.statevector and \
			   not statevector_matches(amplitudes, args.qubits):
				why = "the statevector is not the fake's"
			if why is not None:
				failed.append(job)
				if len(messages) < MAX_FAILURE_MESSAGES:
					messages[str(job)] = why
			elif args.statevector:
				moved += 16 << args.qubits
		loop_ns = clock() - loop_start
		after = resource.getrusage(resource.RUSAGE_SELF)
	finally:
		admission.release(reservation_id=rid)

	results = {
		'index': args.index,
		'calls': args.calls,
		'resource': {'process.pid': os.getpid()},
		'loop_start_unix_ns': loop_start_unix_ns,
		'loop_ns': loop_ns,
		'cpu_ns': cpu_ns(after) - cpu_ns(before),
		'max_rss_kib': after.ru_maxrss,
		'bytes_moved': moved,
		'service_id': service_id,
		'durations_ns': durations,
		'backend_ns': backends,
		'collect_ns': collects,
		'polls': polls,
		'failed_calls': failed,
		'failed_call_count': len(failed),
		'failure_messages': messages,
	}
	partial = args.result + '.partial'
	with open(partial, 'w', encoding='utf-8') as stream:
		json.dump(results, stream)
		stream.write('\n')
	os.replace(partial, args.result)
	if failed:
		print('client {}: {} of {} jobs failed'.format(
			args.index, len(failed), args.calls), file=sys.stderr)
		return 1
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
