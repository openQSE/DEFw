#!/usr/bin/env python3
"""Validate the OTLP JSON a DEFw v2 run leaves behind.

The C smoke test counts what it finds by reading the text. This reads the
files the way the report tooling will, with a JSON parser, and checks the
shape rather than the spelling: identifiers are hex of the right length,
times are decimal strings in order, histogram buckets add up, and the
server's spans really are children of the client's, which is the thing
that proves trace context crossed the wire.

	defw2_otlp_check.py <directory> [--tier TIER]

With --tier, the run must also have recorded that tier's spans on both
sides of a call. A document span must name the document's own method.
"""

import glob
import json
import os
import re
import sys

HEX32 = re.compile(r'^[0-9a-f]{32}$')
HEX16 = re.compile(r'^[0-9a-f]{16}$')

SPAN_CLIENT = 3
SPAN_SERVER = 2


class Problems:
	def __init__(self):
		self.messages = []

	def check(self, ok, message):
		if not ok:
			self.messages.append(message)
		return ok

	def report(self):
		for message in self.messages:
			print('FAILED  {}'.format(message))
		return len(self.messages)


def _text(path, number, raw):
	"""One line as text. Text that is not UTF-8 is a name read from
	memory after it was freed, which is why it is worth naming."""
	try:
		return raw.decode('utf-8').strip()
	except UnicodeDecodeError as error:
		raise SystemExit('{}:{}: not UTF-8, so a span named something '
				 'freed: {}'.format(path, number, error))


def requests(path):
	"""Each line is one OTLP export request."""
	with open(path, 'rb') as stream:
		for number, raw in enumerate(stream, 1):
			line = _text(path, number, raw)
			if not line:
				continue
			try:
				yield number, json.loads(line)
			except ValueError as error:
				raise SystemExit(
					'{}:{}: not JSON: {}'.format(path, number, error))


def attribute_keys(attributes):
	return {item['key'] for item in attributes}


def attribute(attributes, key):
	for item in attributes:
		if item['key'] == key:
			return item['value'].get('stringValue')
	return None


def check_spans(path, problems, seen):
	name = os.path.basename(path)
	count = 0

	for number, request in requests(path):
		where = '{}:{}'.format(name, number)
		resources = request.get('resourceSpans')
		if not problems.check(resources, where + ' has no resourceSpans'):
			continue
		for resource in resources:
			keys = attribute_keys(resource['resource']['attributes'])
			problems.check('service.name' in keys,
				       where + ' has no service.name')
			problems.check('qfw.transport.kind' in keys,
				       where + ' has no transport on the resource')
			for scope in resource['scopeSpans']:
				# The scope names the producer, and a run has
				# more than one: libdefw2 and the harness.
				problems.check(scope['scope'].get('name'),
					       where + ' has an unnamed scope')
				for span in scope['spans']:
					count += check_span(where, span,
							    problems, seen)
	return count


def check_span(where, span, problems, seen):
	problems.check(HEX32.match(span['traceId']),
		       where + ' has a malformed traceId')
	problems.check(HEX16.match(span['spanId']),
		       where + ' has a malformed spanId')
	start = int(span['startTimeUnixNano'])
	end = int(span['endTimeUnixNano'])
	problems.check(end >= start, where + ' ends before it starts')
	# A span older than 2020 means the clock or the units are wrong.
	problems.check(start > 1577836800 * 10 ** 9,
		       where + ' has an implausible start time')
	problems.check('status' in span, where + ' has no status')

	if span['name'] != 'qfw.transport.rpc':
		return 0

	keys = attribute_keys(span['attributes'])
	for required in ('qfw.rpc.api', 'qfw.rpc.method', 'qfw.transport.kind',
			 'qfw.rpc.tier', 'qfw.rpc.status.category'):
		problems.check(required in keys,
			       '{} is missing {}'.format(where, required))

	tier = attribute(span['attributes'], 'qfw.rpc.tier')
	seen['tiers'].add((tier, span['kind']))
	if tier == 'document':
		problems.check(attribute(span['attributes'], 'qfw.rpc.method')
			       not in (None, '', 'document'),
			       where + ' is a document span that does not name '
			       'its method')

	if span['kind'] == SPAN_CLIENT:
		seen['client'].add(span['spanId'])
	elif span['kind'] == SPAN_SERVER:
		seen['server_parents'].add(span.get('parentSpanId'))
		events = {event['name'] for event in span.get('events', [])}
		problems.check('decode' in events,
			       where + ' has a server span with no decode event')
	return 1


def check_metrics(path, problems):
	name = os.path.basename(path)
	found = set()

	for number, request in requests(path):
		where = '{}:{}'.format(name, number)
		for resource in request['resourceMetrics']:
			for scope in resource['scopeMetrics']:
				for metric in scope['metrics']:
					found.add(metric['name'])
					check_metric(where, metric, problems)
	return found


def check_metric(where, metric, problems):
	problems.check('unit' in metric, where + ' has a metric with no unit')
	histogram = metric.get('histogram')
	if histogram is None:
		problems.check('gauge' in metric,
			       where + ' is neither a histogram nor a gauge')
		return

	for point in histogram['dataPoints']:
		buckets = [int(value) for value in point['bucketCounts']]
		bounds = point['explicitBounds']
		problems.check(len(buckets) == len(bounds) + 1,
			       where + ' has a bucket count that does not fit')
		problems.check(sum(buckets) == int(point['count']),
			       where + ' has buckets that do not add up')
		problems.check(point['min'] <= point['max'],
			       where + ' has min above max')


def main():
	args = sys.argv[1:]
	tier = None
	if len(args) == 3 and args[1] == '--tier':
		tier = args[2]
	elif len(args) != 1:
		raise SystemExit('usage: defw2_otlp_check.py <directory> '
				 '[--tier TIER]')
	directory = args[0]
	problems = Problems()
	seen = {'client': set(), 'server_parents': set(), 'tiers': set()}

	span_files = sorted(glob.glob(os.path.join(directory, 'spans-*.jsonl')))
	metric_files = sorted(glob.glob(os.path.join(directory,
						     'metrics-*.jsonl')))
	problems.check(span_files, 'no span files in ' + directory)
	problems.check(metric_files, 'no metric files in ' + directory)

	spans = sum(check_spans(path, problems, seen) for path in span_files)
	problems.check(spans > 0, 'no transport spans were recorded')

	metrics = set()
	for path in metric_files:
		metrics |= check_metrics(path, problems)
	for required in ('qfw.transport.rpc.duration', 'qfw.transport.rpc.bytes',
			 'process.cpu.time', 'process.memory.peak_rss'):
		problems.check(required in metrics, 'no ' + required + ' metric')

	# The point of the traceparent: a service's span hangs off the call
	# that produced it, across two runtimes.
	joined = seen['client'] & seen['server_parents']
	problems.check(joined,
		       'no server span is a child of a client span, so trace '
		       'context did not cross the wire')

	if tier is not None:
		for kind, side in ((SPAN_CLIENT, 'client'),
				   (SPAN_SERVER, 'server')):
			problems.check((tier, kind) in seen['tiers'],
				       'no {} span of the {} tier'.format(
					       side, tier))

	failed = problems.report()
	print('{} transport spans, {} joined to their caller, {} metrics'.format(
		spans, len(joined), len(metrics)))
	print('OTLP CHECK ' + ('FAILED' if failed else 'PASSED'))
	return 1 if failed else 0


if __name__ == '__main__':
	sys.exit(main())
