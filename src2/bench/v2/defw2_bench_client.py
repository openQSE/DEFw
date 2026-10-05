#!/usr/bin/env python3
"""The measured client, in Python.

The same measurement defw2-bench makes in C, through the binding instead,
so a run with this one and a run with that one differ only in the language
the caller is written in. It takes the same arguments and writes the same
result file, so the launcher does not care which it started.

W4 resolves through the directory at --address instead, as the C client
does. W5 and W6 time whole QPM jobs, as the C client does: async_run, then
read_cq until the completion is ready, against a QPM found by its
service_id in the directory at --address, or the directory's only one.
"""

import argparse
import json
import math
import os
import resource
import struct
import sys
import time

import defw2

MAX_FAILURE_MESSAGES = 10
POLL_S = 0.005
# Points of a statevector checked after each W6 job, as in the C client.
QPM_CHECK_POINTS = 256
# Tries at the checked job, as in the C client.
QPM_CHECK_TRIES = 3
QPM_PHI = (math.sqrt(5.0) - 1.0) / 2.0
QASM = ('OPENQASM 2.0;\ninclude "qelib1.inc";\nqreg q[{0}];\n'
	'creg c[{0}];\nh q[0];\nmeasure q -> c;\n')


def parse_args(argv):
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument('--address', required=True)
	parser.add_argument('--result', required=True)
	parser.add_argument('--provider', type=int,
			    default=defw2.PROVIDER_ECHO)
	parser.add_argument('--index', type=int, default=0)
	parser.add_argument('--calls', type=int, default=1000)
	parser.add_argument('--warmup', type=int, default=100)
	parser.add_argument('--payload', type=int, default=64)
	parser.add_argument('--traceparent')
	parser.add_argument('--ready')
	parser.add_argument('--go')
	parser.add_argument('--timeout-ms', type=int, default=60000)
	parser.add_argument('--wait-s', type=int, default=300)
	parser.add_argument('--bulk', action='store_true')
	parser.add_argument('--resolve', action='store_true')
	parser.add_argument('--resolve-type')
	parser.add_argument('--qpm', action='store_true')
	parser.add_argument('--service-id')
	parser.add_argument('--qubits', type=int, default=4)
	parser.add_argument('--shots', type=int, default=1024)
	parser.add_argument('--statevector', action='store_true')
	return parser.parse_args(argv)


def build_payload(length):
	"""The same bytes the C client sends."""
	return bytes(i % 256 for i in range(length))


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


def measure(echo, args, payload, sink_check):
	clock = time.perf_counter_ns
	durations = [0] * args.calls
	failed = []
	messages = {}

	before = resource.getrusage(resource.RUSAGE_SELF)
	loop_start_unix_ns = time.time_ns()
	loop_start = clock()
	for call in range(args.calls):
		started = clock()
		try:
			value = echo(payload)
		except Exception as exc:			# noqa: BLE001
			durations[call] = clock() - started
			failed.append(call)
			if len(messages) < MAX_FAILURE_MESSAGES:
				messages[str(call)] = '{}: {}'.format(
					type(exc).__name__, exc)
			continue
		# Checked after the clock stops, as the C client does.
		durations[call] = clock() - started
		if not sink_check(value, payload):
			failed.append(call)
			if len(messages) < MAX_FAILURE_MESSAGES:
				messages[str(call)] = 'the echo returned ' \
						      'different data'
	loop_ns = clock() - loop_start
	after = resource.getrusage(resource.RUSAGE_SELF)

	return {
		'loop_start_unix_ns': loop_start_unix_ns,
		'loop_ns': loop_ns,
		'durations_ns': durations,
		'failed_calls': failed,
		'failed_call_count': len(failed),
		'failure_messages': messages,
		'cpu_ns': cpu_ns(after) - cpu_ns(before),
		'max_rss_kib': after.ru_maxrss,
	}


def write_results(args, results):
	partial = args.result + '.partial'
	with open(partial, 'w', encoding='utf-8') as stream:
		json.dump(results, stream)
		stream.write('\n')
	os.replace(partial, args.result)


def statevector_matches(data, qubits):
	"""The fake IQM QPM's statevector, checked at the points the C client
	checks: amplitude k is exp(2 pi i frac(k phi)) / sqrt(2^n)."""
	count = 1 << qubits
	stride = count // QPM_CHECK_POINTS + 1
	norm = 1.0 / math.sqrt(count)
	for k in [*range(0, count, stride), count - 1]:
		phase = 2.0 * math.pi * math.fmod(k * QPM_PHI, 1.0)
		real, imag = struct.unpack_from('<dd', data, 16 * k)
		if abs(real - math.cos(phase) * norm) > 1e-9 * norm or \
		   abs(imag - math.sin(phase) * norm) > 1e-9 * norm:
			return False
	return True


def qpm_job(qpm, args, reservation_id, lent, traceparent=None):
	"""One job: async_run, then read_cq back to back until the completion
	is ready, within the call timeout. Returns what went wrong, or None,
	with the read_cq that collected the job, the QPM's own run time and
	the number of polls."""
	clock = time.perf_counter_ns
	task = qpm.async_run(QASM.format(args.qubits), num_qubits=args.qubits,
			     num_shots=args.shots,
			     return_statevector=args.statevector,
			     reservation_id=reservation_id,
			     traceparent=traceparent)
	polls = 0
	deadline = clock() + args.timeout_ms * 1000000
	while True:
		started = clock()
		done = qpm.read_cq(cid=task.cid, result=lent,
				   reservation_id=reservation_id,
				   traceparent=traceparent)
		collect = clock() - started
		polls += 1
		if done.completion_ready:
			break
		if clock() > deadline:
			return ('the job did not complete within the call '
				'timeout', collect, 0, polls)
	why = None
	if done.outcome != 'COMPLETED':
		why = 'the job did not complete'
	elif args.statevector and (not done.statevector_delivered or
				   done.statevector.nbytes !=
				   16 << args.qubits):
		why = 'the statevector was not delivered'
	# A QPM that does not say how long it ran counts as no time.
	backend = int((done.extra or {}).get('observed_fake_runtime_ns') or 0)
	return why, collect, backend, polls


def run_qpm(args):
	"""W5 and W6, shaped like the echo run: one checked job and the warmup
	outside the run's trace, then the measured jobs."""
	clock = time.perf_counter_ns
	runtime = defw2.Runtime(role='client',
				node_name='bench-py-client-{}'.format(args.index))
	with defw2.Directory(runtime, args.address) as directory:
		records = directory.resolve(service_id=args.service_id,
					    service_type='qfw.qpm')
	if not records:
		print('no QPM {} in the directory at {}'.format(
			args.service_id or 'at all', args.address),
		      file=sys.stderr)
		return 1
	if len(records) > 1:
		print('{} QPMs in the directory, measuring {}'.format(
			len(records), records[0]['service_id']), file=sys.stderr)
	qpm = defw2.QPM.from_record(runtime, records[0],
				    timeout_ms=args.timeout_ms)
	decision = qpm.reserve(
		job_id='defw2-bench-py-{}'.format(args.index),
		allocation_id='defw2-bench-py-{}'.format(args.index),
		num_qubits=args.qubits, workload_kind='quantum',
		walltime_ns=3600 * 10**9, ttl_ns=3600 * 10**9,
		task_class={'count': args.calls + args.warmup + 1,
			    'qubit_count': args.qubits, 'depth': 1,
			    'one_q_gate_count': 1, 'two_q_gate_count': 0,
			    'shots': args.shots,
			    'measurement_count': args.qubits},
		extra={'owner': {'user': 'defw2-bench'},
		       'run_context': {'operation': 'async_run'}})
	if decision.decision != 'accepted' or not decision.reservation_id:
		print('the reservation was not accepted: {}'.format(
			decision.message or decision.reason), file=sys.stderr)
		return 1
	rid = decision.reservation_id
	lent = None
	blank = None
	if args.statevector:
		lent = bytearray(16 << args.qubits)
		blank = bytes(len(lent))

	try:
		for _ in range(QPM_CHECK_TRIES):
			try:
				why = qpm_job(qpm, args, rid, lent)[0]
			except Exception as exc:		# noqa: BLE001
				why = '{}: {}'.format(type(exc).__name__, exc)
			if why is None and args.statevector and \
			   not statevector_matches(lent, args.qubits):
				why = "the statevector is not the fake's"
			if why is None:
				break
			print('the checked job failed: {}'.format(why),
			      file=sys.stderr)
		if why is not None:
			return 1
		for _ in range(args.warmup):
			try:
				qpm_job(qpm, args, rid, lent)
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
			# Cleared so the check proves this job's statevector
			# arrived. Outside the clock, inside the loop, as in C.
			if lent is not None:
				lent[:] = blank
			started = clock()
			try:
				why, collects[job], backends[job], polls[job] = \
					qpm_job(qpm, args, rid, lent,
						args.traceparent)
			except Exception as exc:		# noqa: BLE001
				why = '{}: {}'.format(type(exc).__name__, exc)
			durations[job] = clock() - started
			# Checked after the clock stops.
			if why is None and lent is not None and \
			   not statevector_matches(lent, args.qubits):
				why = "the statevector is not the fake's"
			if why is not None:
				failed.append(job)
				if len(messages) < MAX_FAILURE_MESSAGES:
					messages[str(job)] = why
			elif lent is not None:
				moved += len(lent)
		loop_ns = clock() - loop_start
		after = resource.getrusage(resource.RUSAGE_SELF)
	finally:
		qpm.release(rid)
		qpm.close()
		runtime.close()

	write_results(args, {
		'index': args.index,
		'calls': args.calls,
		'resource': {'process.pid': os.getpid()},
		'loop_start_unix_ns': loop_start_unix_ns,
		'loop_ns': loop_ns,
		'cpu_ns': cpu_ns(after) - cpu_ns(before),
		'max_rss_kib': after.ru_maxrss,
		'bytes_moved': moved,
		'service_id': records[0]['service_id'],
		'durations_ns': durations,
		'backend_ns': backends,
		'collect_ns': collects,
		'polls': polls,
		'failed_calls': failed,
		'failed_call_count': len(failed),
		'failure_messages': messages,
	})
	if failed:
		print('client {}: {} of {} jobs failed'.format(
			args.index, len(failed), args.calls), file=sys.stderr)
		return 1
	return 0


def run_resolve(args):
	"""W4, as the C client runs it: one checked resolve, which waits for
	the service the run resolves to register, then the warmup and the
	measured resolves. An empty answer is still an answer, so only a
	failed call counts as a failure."""
	name = 'bench-py-client-{}'.format(args.index)
	runtime = defw2.Runtime(role='client', node_name=name)
	directory = defw2.Directory(runtime, args.address,
				    timeout_ms=args.timeout_ms)
	try:
		deadline = time.monotonic() + args.wait_s
		while not directory.resolve(service_type=args.resolve_type):
			if time.monotonic() > deadline:
				print('the directory at {} has no {}'.format(
					args.address,
					args.resolve_type or 'record'),
				      file=sys.stderr)
				return 1
			time.sleep(0.01)
		for _ in range(args.warmup):
			directory.resolve(service_type=args.resolve_type)

		if args.ready:
			with open(args.ready, 'w', encoding='ascii'):
				pass
		if not wait_for(args.go, args.wait_s):
			print('no go signal at {}'.format(args.go),
			      file=sys.stderr)
			return 1

		def resolve(_):
			return directory.resolve(
				service_type=args.resolve_type,
				traceparent=args.traceparent)

		results = measure(resolve, args, None, lambda value, sent: True)
	finally:
		directory.close()
		runtime.close()
	results.update({
		'index': args.index,
		'calls': args.calls,
		'resource': {'process.pid': os.getpid()},
		'bytes_moved': 0,
	})
	write_results(args, results)
	if results['failed_call_count']:
		print('client {}: {} of {} resolves failed'.format(
			args.index, results['failed_call_count'], args.calls),
		      file=sys.stderr)
		return 1
	return 0


def main(argv):
	args = parse_args(argv)
	if args.qpm:
		return run_qpm(args)
	if args.resolve:
		return run_resolve(args)
	payload = build_payload(args.payload)

	runtime = defw2.Runtime(role='client',
				node_name='bench-py-client-{}'.format(args.index))
	echo = defw2.Echo(runtime, args.address, provider_id=args.provider,
			  timeout_ms=args.timeout_ms)

	if args.bulk:
		def call(data, traceparent=None):
			return echo.echo_bulk(data, traceparent=traceparent)[0]
	else:
		def call(data, traceparent=None):
			return echo.echo(data, traceparent=traceparent)

	# The check and the warmup stay out of the run's trace, so one run is
	# one trace holding exactly the calls that were measured.
	if call(payload) != payload:
		print('the echo service returned different data',
		      file=sys.stderr)
		return 1
	for _ in range(args.warmup):
		call(payload)

	if args.ready:
		with open(args.ready, 'w', encoding='ascii'):
			pass
	if not wait_for(args.go, args.wait_s):
		print('no go signal at {}'.format(args.go), file=sys.stderr)
		return 1

	def measured(data):
		return call(data, traceparent=args.traceparent)

	results = measure(measured, args, payload,
			  lambda value, sent: value == sent)
	results.update({
		'index': args.index,
		'calls': args.calls,
		'resource': {'process.pid': os.getpid()},
		'bytes_moved': 2 * args.payload * (
			args.calls - results['failed_call_count']),
	})

	write_results(args, results)

	echo.close()
	runtime.close()
	if results['failed_call_count']:
		print('client {}: {} of {} calls failed'.format(
			args.index, results['failed_call_count'], args.calls),
		      file=sys.stderr)
		return 1
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
