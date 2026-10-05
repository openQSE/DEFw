"""
Runs inside one DEFw v1 client process started by defw1_bench_driver.py.

The client connects to the echo service through the directory service,
checks that an echo comes back intact, and warms up. It then waits for the
driver's go signal, so that all clients measure the same window.

Each measured call is timed with perf_counter_ns around the public proxy
call, echo.echo(payload). That is the boundary a DEFw application sees, so
the round trip includes everything v1 does for a call: building the request,
YAML encoding, the transport, dispatch in the service, and the response's
way back. Correctness is checked after the clock stops. The measured calls'
messages are counted too, for wire bytes per call. Results go to one JSON
file for the driver.

The client never calls the service's shutdown method. In v1 that
deregisters the whole service, which other clients may still be using.

W4 resolves the echo service through the directory instead, with
dirsvc.resolve_services, which is how v1 finds any service. The first
resolve waits for the echo service to be there, so every measured resolve
finds one record.
"""

import os
import resource
import time
import traceback

import defw
import defw_workers
from defw_app_util import (
	defw_connect_service_by_name,
	defw_get_directory_service,
)

import defw_bench_common as common

SERVICE_NAME = 'TestEcho'
SERVICE_TYPE = 'defw.test.echo'
GO_POLL_S = 0.001
RESOLVE_POLL_S = 0.01
MAX_FAILURE_MESSAGES = 20
MAX_MESSAGE_LEN = 500


def wait_for(path, timeout_s):
	deadline = time.monotonic() + timeout_s
	while not os.path.exists(path):
		if time.monotonic() > deadline:
			raise TimeoutError(f'no go signal after {timeout_s:.0f}s')
		time.sleep(GO_POLL_S)


def cpu_ns(usage):
	return round((usage.ru_utime + usage.ru_stime) * 1e9)


def describe(exc):
	return f'{type(exc).__name__}: {exc}'[:MAX_MESSAGE_LEN]


def main():
	try:
		return measure()
	except Exception:
		traceback.print_exc()
		return 1


def echo_calls(dirsvc, payload):
	"""W1 to W3: echo calls, and what checks each answer. The service is
	found by name and checked once before anything is measured."""
	echo = defw_connect_service_by_name(dirsvc, SERVICE_NAME)[0]
	if echo.echo(payload) != payload:
		raise RuntimeError('the echo service returned different data')

	def verify(value):
		if value != payload:
			return 'the echo returned different data'
		return None
	return (lambda: echo.echo(payload)), verify


def resolve_calls(dirsvc, timeout_s):
	"""W4: resolves of the echo service. The first waits for the service
	to be there, as v1's own lookup helper does, and an answer is never
	wrong, since an empty one is an answer too."""
	deadline = time.monotonic() + timeout_s
	while not dirsvc.resolve_services(service_type=SERVICE_TYPE):
		if time.monotonic() > deadline:
			raise TimeoutError(f'no {SERVICE_TYPE} in the '
					   f'directory after {timeout_s:.0f}s')
		time.sleep(RESOLVE_POLL_S)
	return (lambda: dirsvc.resolve_services(service_type=SERVICE_TYPE),
		lambda value: None)


def measure():
	config = common.load_config()
	index = int(os.environ[common.CLIENT_INDEX_ENV])
	run_dir = config['run_dir']

	dirsvc = defw_get_directory_service()
	if config.get('resolve'):
		operation, verify = resolve_calls(dirsvc, config['timeout_s'])
	else:
		operation, verify = echo_calls(
			dirsvc, common.build_payload(config['payload_bytes'],
						     config['payload_kind']))
	for _ in range(config['warmup']):
		operation()

	with open(common.ready_path(run_dir, index), 'w', encoding='ascii'):
		pass
	wait_for(common.go_path(run_dir), config['timeout_s'])

	calls = config['calls']
	start_offsets = [0] * calls
	durations = [0] * calls
	failures = {}
	clock = time.perf_counter_ns
	messages = common.V1Messages(defw_workers)

	usage_before = resource.getrusage(resource.RUSAGE_SELF)
	loop_start_unix_ns = time.time_ns()
	messages.start()
	loop_start = clock()
	for call in range(calls):
		started = clock()
		try:
			value = operation()
		except Exception as exc:
			ended = clock()
			failures[call] = describe(exc)
		else:
			ended = clock()
			problem = verify(value)
			if problem:
				failures[call] = problem
		start_offsets[call] = started - loop_start
		durations[call] = ended - started
	loop_ns = clock() - loop_start
	messages.stop()
	usage_after = resource.getrusage(resource.RUSAGE_SELF)

	attributes = common.process_attributes('defw1-bench-client')
	attributes['qfw.bench.client.index'] = index
	first_failures = sorted(failures)[:MAX_FAILURE_MESSAGES]
	common.write_json(common.result_path(run_dir, index), {
		'index': index,
		'agent_name': os.environ.get('DEFW_AGENT_NAME'),
		'defw_module': os.path.realpath(defw.__file__),
		'resource': attributes,
		'calls': calls,
		'loop_start_unix_ns': loop_start_unix_ns,
		'loop_ns': loop_ns,
		'start_offsets_ns': start_offsets,
		'durations_ns': durations,
		'failed_calls': sorted(failures),
		'failure_messages': {str(call): failures[call]
				     for call in first_failures},
		'cpu_ns': cpu_ns(usage_after) - cpu_ns(usage_before),
		'max_rss_kib': usage_after.ru_maxrss,
		'wire': messages.counts(),
	})
	return 0
