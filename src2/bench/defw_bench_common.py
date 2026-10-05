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
import threading

MIB = 1024 * 1024

CONFIG_ENV = 'DEFW_BENCH_CONFIG'
CLIENT_INDEX_ENV = 'DEFW_BENCH_CLIENT_INDEX'

# Workload defaults, from the Workloads table in docs/design_v2.md. W3's
# call count depends on its payload size, see default_calls().
#
# W4 resolves through the directory, with one record there to find, and
# carries no payload.
#
# W5 and W6 measure whole QPM jobs rather than calls: async_run, then read_cq
# until the completion is ready. A W6 job returns a statevector of 16 bytes an
# amplitude, which is its payload.
WORKLOADS = {
	'W1': {'payload_bytes': 64, 'calls': 10000, 'warmup': 100},
	'W2': {'payload_bytes': 4 * 1024, 'calls': 10000, 'warmup': 100},
	'W3': {'payload_bytes': MIB, 'calls': None, 'warmup': 2},
	'W4': {'payload_bytes': 0, 'calls': 1000, 'warmup': 100,
	       'resolve': True},
	'W5': {'payload_bytes': 0, 'calls': 1000, 'warmup': 20,
	       'qpm': True, 'qubits': 4, 'shots': 1024, 'statevector': False},
	'W6': {'payload_bytes': 16 * MIB, 'calls': 20, 'warmup': 2,
	       'qpm': True, 'qubits': 20, 'shots': 1024, 'statevector': True},
}

# What the v1 harness measures, against DEFw's own echo service and
# directory. W5 and W6 run QPM jobs through QFw instead, see the v2 launcher.
V1_WORKLOADS = sorted(name for name, workload in WORKLOADS.items()
		      if not workload.get('qpm'))

# A W3 client moves about this much payload, within the call limits below,
# so a 256 MiB run takes minutes rather than hours.
W3_BYTES_PER_CLIENT = 1600 * MIB
W3_MIN_CALLS = 5
W3_MAX_CALLS = 100

SPAN_KIND_INTERNAL = 1
SPAN_KIND_SERVER = 2
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


# Wire bytes per call count each message's body as the framework encoded
# it: v1's YAML text and the NUL that ends it, and the message Mercury
# encodes for v2, which carries v2's own header and trace context. Neither
# version's fixed transport header is counted, 28 bytes a message for v1
# and Mercury's own for v2, so what is compared is the two encodings. Both
# directions count: the requests a client sends and their answers, and
# the events a service sends it and its acknowledgements.

def _message_bytes(text):
	"""A v1 message's body: its text, and the NUL that ends it."""
	return len(text.encode('utf-8')) + 1


class V1Messages:
	"""Counts the RPC messages a DEFw v1 process sends and receives.

	Every v1 request leaves through defw_workers.defw_send_req, the C call
	that sends its YAML text, and every answer through defw_send_rsp. The
	C runtime hands each request and answer it receives to
	defw_workers.put_request and put_response, looking them up by name for
	every message. Wrapping the four sees every call, the events a service
	sends included. Counting is off until start(), so a run counts only
	its measured calls."""

	def __init__(self, workers):
		self.counting = False
		self.lock = threading.Lock()
		self.totals = {'rpcs': 0, 'request_bytes': 0, 'responses': 0,
			       'response_bytes': 0}
		send_req = workers.defw_send_req
		send_rsp = workers.defw_send_rsp
		put_req = workers.put_request
		put_rsp = workers.put_response

		def count(messages, nbytes, text):
			if not self.counting:
				return
			size = _message_bytes(text)
			with self.lock:
				self.totals[messages] += 1
				self.totals[nbytes] += size

		def counted_send_req(remote_uuid, blk_uuid, msg):
			count('rpcs', 'request_bytes', msg)
			return send_req(remote_uuid, blk_uuid, msg)

		def counted_send_rsp(remote_uuid, blk_uuid, msg):
			count('responses', 'response_bytes', msg)
			return send_rsp(remote_uuid, blk_uuid, msg)

		def counted_put_req(msg, uuid):
			count('rpcs', 'request_bytes', msg)
			return put_req(msg, uuid)

		def counted_put_rsp(msg, uuid):
			count('responses', 'response_bytes', msg)
			return put_rsp(msg, uuid)

		workers.defw_send_req = counted_send_req
		workers.defw_send_rsp = counted_send_rsp
		workers.put_request = counted_put_req
		workers.put_response = counted_put_rsp

	def start(self):
		self.counting = True

	def stop(self):
		self.counting = False

	def counts(self):
		with self.lock:
			return dict(self.totals)


def run_wire_bytes(otlp_dir, trace_id, start_ns, end_ns):
	"""What a v2 run's clients sent and received, from the span files of
	the client processes, whose names the launcher gives them: each
	client span of the run's trace, which counts one call's request and
	answer, and each span a client served while the run was measured,
	which is an event its sink took in. A client serves nothing else.
	Whether an event carries the job's trace is the sender's choice, so
	the window decides rather than the trace. The services' files are
	left alone, since they count the same calls again."""
	counts = {'rpcs': 0, 'request_bytes': 0, 'responses': 0,
		  'response_bytes': 0}
	for name in sorted(os.listdir(otlp_dir)):
		if not re.match(r'spans-bench(-py)?-client-\d+\.jsonl$',
				name):
			continue
		for span in _file_spans(os.path.join(otlp_dir, name)):
			if span['name'] != 'qfw.transport.rpc':
				continue
			started = int(span['startTimeUnixNano'])
			if (span['kind'] == SPAN_KIND_CLIENT and
			    span['traceId'] == trace_id) or \
			   (span['kind'] == SPAN_KIND_SERVER and
			    start_ns <= started <= end_ns):
				_count_span(span, counts)
	return counts


def _file_spans(path):
	"""Every span of one OTLP/JSON file."""
	with open(path, encoding='utf-8') as stream:
		for line in stream:
			for resource in json.loads(line)['resourceSpans']:
				for scope in resource['scopeSpans']:
					yield from scope['spans']


def _count_span(span, counts):
	sizes = {item['key']: int(item['value'].get('intValue', 0))
		 for item in span['attributes']
		 if item['key'] in ('qfw.rpc.request.bytes',
				    'qfw.rpc.response.bytes')}
	counts['rpcs'] += 1
	counts['request_bytes'] += sizes.get('qfw.rpc.request.bytes', 0)
	response = sizes.get('qfw.rpc.response.bytes', 0)
	if response:
		counts['responses'] += 1
		counts['response_bytes'] += response


def wire_summary(counts, calls, source):
	"""Wire bytes per measured call, or per job for a QPM workload, from
	counts that add up every client's."""
	if not calls or not counts['rpcs']:
		return None
	return {
		'source': source,
		'rpcs_per_call': counts['rpcs'] / calls,
		'request_bytes_per_call': counts['request_bytes'] / calls,
		'response_bytes_per_call': counts['response_bytes'] / calls,
		'bytes_per_call': (counts['request_bytes'] +
				   counts['response_bytes']) / calls,
	}


def add_counts(total, counts):
	for key, value in counts.items():
		total[key] = total.get(key, 0) + value
	return total


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
