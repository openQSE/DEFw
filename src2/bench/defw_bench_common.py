"""
Shared pieces of the DEFw benchmark harnesses.

The v1 harness under v1/ measures DEFw v1 through its Python runtime. The
DEFw v2 harnesses will use the same workload table, statistics and output
format, so a v1 run and a v2 run can be compared directly. Nothing in this
module imports DEFw, so it loads the same way in either runtime.

Output follows the file profile of the QFw benchmarking design
(docs/design/benchmarking.md in openQSE/QFw): OTLP/JSON, one export request
per line, on node-local storage. One benchmark run is one trace. Its root
span is qfw.bench.run, and each measured call is a qfw.transport.rpc span
beneath it, carrying the client-side round-trip time.
"""

import json
import math
import os
import platform
import re
import socket
import subprocess

MIB = 1024 * 1024

CONFIG_ENV = 'DEFW_BENCH_CONFIG'
CLIENT_INDEX_ENV = 'DEFW_BENCH_CLIENT_INDEX'

# Workload defaults, from the Workloads table in docs/design_v2.md. W3's
# call count depends on its payload size, see default_calls().
#
# W5 and W6 measure whole QPM jobs rather than calls: async_run, then read_cq
# until the completion is ready. A W6 job returns a statevector of 16 bytes an
# amplitude, which is its payload.
WORKLOADS = {
	'W1': {'payload_bytes': 64, 'calls': 10000, 'warmup': 100},
	'W2': {'payload_bytes': 4 * 1024, 'calls': 10000, 'warmup': 100},
	'W3': {'payload_bytes': MIB, 'calls': None, 'warmup': 2},
	'W5': {'payload_bytes': 0, 'calls': 1000, 'warmup': 20,
	       'qpm': True, 'qubits': 4, 'shots': 1024, 'statevector': False},
	'W6': {'payload_bytes': 16 * MIB, 'calls': 20, 'warmup': 2,
	       'qpm': True, 'qubits': 20, 'shots': 1024, 'statevector': True},
}

# The echo workloads, which the v1 harness measures against DEFw's own echo
# service. W5 and W6 run QPM jobs through QFw instead, see the v2 launcher.
ECHO_WORKLOADS = sorted(name for name, workload in WORKLOADS.items()
			if not workload.get('qpm'))

# A W3 client moves about this much payload, within the call limits below,
# so a 256 MiB run takes minutes rather than hours.
W3_BYTES_PER_CLIENT = 1600 * MIB
W3_MIN_CALLS = 5
W3_MAX_CALLS = 100

SPAN_KIND_INTERNAL = 1
SPAN_KIND_CLIENT = 3
STATUS_OK = 1
STATUS_ERROR = 2

SPANS_PER_BATCH = 2000


def default_calls(workload, payload_bytes):
	calls = WORKLOADS[workload]['calls']
	if calls is not None:
		return calls
	calls = W3_BYTES_PER_CLIENT // max(payload_bytes, 1)
	return max(W3_MIN_CALLS, min(W3_MAX_CALLS, calls))


def parse_size(text):
	"""Parse a byte count written as 64, 4KiB, 16MiB or 1GiB."""
	match = re.fullmatch(r'\s*(\d+)\s*([KMG]iB)?\s*', text)
	if not match:
		raise ValueError(f'not a size: {text!r}')
	scale = {None: 1, 'KiB': 1024, 'MiB': MIB, 'GiB': 1024 * MIB}
	return int(match.group(1)) * scale[match.group(2)]


def format_size(nbytes):
	for unit, scale in (('GiB', 1024 * MIB), ('MiB', MIB), ('KiB', 1024)):
		if nbytes >= scale and nbytes % scale == 0:
			return f'{nbytes // scale} {unit}'
	return f'{nbytes} B'


def transport_env(kind):
	"""DEFw v1 environment for a transport named tcp or ofi+<provider>."""
	if kind == 'tcp':
		return {'DEFW_TRANSPORT': 'tcp'}
	if kind.startswith('ofi+') and len(kind) > len('ofi+'):
		return {'DEFW_TRANSPORT': 'ofi',
			'DEFW_OFI_PROVIDER': kind[len('ofi+'):]}
	raise ValueError(
		f'unknown transport {kind!r}, expected tcp or ofi+<provider>')


def build_payload(nbytes, kind):
	"""A deterministic payload of nbytes, as bytes or as an ASCII str."""
	if kind == 'bytes':
		pattern = bytes(range(256))
	elif kind == 'str':
		pattern = b'0123456789abcdef' * 16
	else:
		raise ValueError(f'unknown payload kind {kind!r}')
	whole, rest = divmod(nbytes, len(pattern))
	data = pattern * whole
	if rest:
		data += pattern[:rest]
	return data if kind == 'bytes' else data.decode('ascii')


# Files one run uses to coordinate its processes and hand back results.

def client_dir(run_dir, index):
	return os.path.join(run_dir, f'client-{index}')


def ready_path(run_dir, index):
	return os.path.join(run_dir, 'control', f'ready-{index}')


def go_path(run_dir):
	return os.path.join(run_dir, 'control', 'go')


def result_path(run_dir, index):
	return os.path.join(run_dir, 'results', f'client-{index}.json')


def load_config():
	with open(os.environ[CONFIG_ENV], encoding='utf-8') as stream:
		return json.load(stream)


def write_json(path, value, indent=None):
	"""Write JSON atomically, so a reader never sees a partial file."""
	partial = f'{path}.partial'
	with open(partial, 'w', encoding='utf-8') as stream:
		json.dump(value, stream, indent=indent)
		stream.write('\n')
	os.replace(partial, path)


def percentile(ordered, fraction):
	"""Nearest-rank percentile of an ascending sequence."""
	if not ordered:
		return None
	rank = max(1, math.ceil(fraction * len(ordered)))
	return ordered[rank - 1]


def latency_summary(durations_ns):
	"""Round-trip statistics in microseconds."""
	ordered = sorted(durations_ns)
	count = len(ordered)
	if not count:
		return {'count': 0}
	mean = sum(ordered) / count
	variance = sum((value - mean) ** 2 for value in ordered) / count
	summary = {
		'count': count,
		'min_us': ordered[0] / 1e3,
		'p50_us': percentile(ordered, 0.50) / 1e3,
		'p90_us': percentile(ordered, 0.90) / 1e3,
		'p99_us': percentile(ordered, 0.99) / 1e3,
		'max_us': ordered[-1] / 1e3,
		'mean_us': mean / 1e3,
		'stdev_us': math.sqrt(variance) / 1e3,
	}
	# From fewer than 1000 samples a p99.9 is just the maximum.
	if count >= 1000:
		summary['p999_us'] = percentile(ordered, 0.999) / 1e3
	return summary


def libfabric_version():
	try:
		out = subprocess.run(['fi_info', '--version'], capture_output=True,
				     text=True, timeout=10).stdout
	except (OSError, subprocess.SubprocessError):
		return None
	match = re.search(r'libfabric:\s*(\S+)', out)
	return match.group(1) if match else None


def process_attributes(service_name):
	"""Per-process context, recorded as OTLP resource attributes."""
	return {
		'service.name': service_name,
		'host.name': socket.gethostname(),
		'host.arch': platform.machine(),
		'os.type': platform.system().lower(),
		'os.version': platform.release(),
		'process.pid': os.getpid(),
		'process.runtime.name': platform.python_implementation(),
		'process.runtime.version': platform.python_version(),
	}


def otlp_value(value):
	if isinstance(value, bool):
		return {'boolValue': value}
	if isinstance(value, int):
		# OTLP/JSON carries 64-bit integers as decimal strings.
		return {'intValue': str(value)}
	if isinstance(value, float):
		return {'doubleValue': value}
	return {'stringValue': str(value)}


def otlp_attributes(mapping):
	return [{'key': key, 'value': otlp_value(value)}
		for key, value in mapping.items() if value is not None]


def otlp_span(trace_id, span_id, name, start_ns, end_ns, attributes,
	      parent_span_id=None, kind=SPAN_KIND_CLIENT, error=None):
	"""One span. attributes is an OTLP attribute list rather than a dict,
	so a caller writing many similar spans can share one list."""
	span = {
		'traceId': trace_id,
		'spanId': span_id,
		'name': name,
		'kind': kind,
		'startTimeUnixNano': str(start_ns),
		'endTimeUnixNano': str(end_ns),
		'attributes': attributes,
	}
	if parent_span_id:
		span['parentSpanId'] = parent_span_id
	if error:
		span['status'] = {'code': STATUS_ERROR, 'message': error}
	else:
		span['status'] = {'code': STATUS_OK}
	return span


class OtlpJsonWriter:
	"""Writes OTLP/JSON trace export requests, one per line.

	Identifiers are lowercase hex and enums are integers, as OTLP/JSON
	requires. Each line repeats the resource once, however many spans
	it holds."""

	def __init__(self, path, scope_name, scope_version):
		self._stream = open(path, 'w', encoding='utf-8')
		self._scope = {'name': scope_name, 'version': scope_version}

	def write(self, resource, spans):
		attributes = otlp_attributes(resource)
		for start in range(0, len(spans), SPANS_PER_BATCH):
			request = {'resourceSpans': [{
				'resource': {'attributes': attributes},
				'scopeSpans': [{
					'scope': self._scope,
					'spans': spans[start:start + SPANS_PER_BATCH],
				}],
			}]}
			self._stream.write(json.dumps(request, separators=(',', ':')))
			self._stream.write('\n')

	def close(self):
		self._stream.close()
