#!/usr/bin/env python3
"""The measured client, in Python.

The same measurement defw2-bench makes in C, through the binding instead,
so a run with this one and a run with that one differ only in the language
the caller is written in. It takes the same arguments and writes the same
result file, so the launcher does not care which it started.
"""

import argparse
import json
import os
import resource
import sys
import time

import defw2

MAX_FAILURE_MESSAGES = 10
POLL_S = 0.005


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


def main(argv):
	args = parse_args(argv)
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

	partial = args.result + '.partial'
	with open(partial, 'w', encoding='utf-8') as stream:
		json.dump(results, stream)
		stream.write('\n')
	os.replace(partial, args.result)

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
