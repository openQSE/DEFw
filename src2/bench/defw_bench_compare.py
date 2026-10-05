#!/usr/bin/env python3
"""Compare DEFw benchmark runs.

Both harnesses write the same report, so a comparison is a join on the
workload. Give it run directories or summary files, in any mix:

	defw_bench_compare.py /tmp/defw-bench /mnt/defw2-baselines/2026-09-23-tcp

Runs are grouped by workload, payload and client count, and W5 and W6 in
event mode apart from polling. Within a group the v1 runs are the
baselines, and each v2 run is shown against the v1 run on the matching
provider: v2's ofi+tcp against v1's ofi+tcp, and Mercury's shared memory,
na+sm, against v1's, ofi+sm2. When the group has no v1 run on that
provider, the v2 run is shown against the first v1 run it has, and the row
says the pair is unmatched. A group with only one version is still listed,
because a missing half is worth seeing. Runs whose service and clients had
CPUs of their own are kept apart from runs where they shared them.

W5 and W6 compare what a job costs instead of what a call does: the
framework's overhead, jobs a second, polls and the collect.
"""

import argparse
import glob
import json
import os
import sys

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BENCH_DIR)

import defw_bench_common as common  # noqa: E402

# What is compared, and which way is better.
CALL_METRICS = (
	('p50 ms', 'lower', lambda r: r['latency']['p50_us'] / 1e3),
	('p99 ms', 'lower', lambda r: r['latency']['p99_us'] / 1e3),
	('calls/s', 'higher', lambda r: r['throughput']['calls_per_s']),
	('client CPU ms', 'lower', lambda r: r['cpu']['client_us_per_call'] / 1e3),
	('service CPU ms', 'lower', lambda r: r['cpu']['service_us_per_call'] / 1e3
	 if r['cpu']['service_us_per_call'] is not None else None),
	('peak RSS MiB', 'lower',
	 lambda r: r['memory']['service_max_rss_kib'] / 1024
	 if r['memory']['service_max_rss_kib'] else None),
	('bulk MiB/s', 'higher',
	 lambda r: r.get('bulk', {}).get('payload_mib_per_s')),
	('wire B/call', 'lower',
	 lambda r: (r.get('wire') or {}).get('bytes_per_call')),
)

# A QPM job's, where the overhead is the framework's cost per job.
JOB_METRICS = (
	('overhead p50 ms', 'lower',
	 lambda r: r['qpm']['overhead']['p50_us'] / 1e3),
	('overhead p99 ms', 'lower',
	 lambda r: r['qpm']['overhead']['p99_us'] / 1e3),
	('jobs/s', 'higher', lambda r: r['throughput']['calls_per_s']),
	('polls', 'lower', lambda r: r['qpm']['polls']['mean']),
	('collect ms', 'lower', lambda r: r['qpm']['collect']['p50_us'] / 1e3),
	('client CPU ms', 'lower',
	 lambda r: r['cpu']['client_us_per_call'] / 1e3),
	('wire B/job', 'lower',
	 lambda r: (r.get('wire') or {}).get('bytes_per_call')),
)


def measured(get, report):
	"""A metric of a report, or None for one it does not have, such as
	the latency of a run in which every call failed."""
	try:
		return get(report)
	except (KeyError, TypeError, ZeroDivisionError):
		return None


def metrics_of(reports):
	return JOB_METRICS if any('qpm' in r for r in reports) \
		else CALL_METRICS


def find_summaries(paths):
	found = []
	for path in paths:
		if os.path.isdir(path):
			found.extend(glob.glob(os.path.join(path, '**',
							    'summary.json'),
					       recursive=True))
		else:
			found.append(path)
	reports = []
	for path in sorted(set(found)):
		with open(path, encoding='utf-8') as stream:
			report = json.load(stream)
		if report.get('schema') != 'defw-bench-summary/1':
			print('skipping {}, schema {}'.format(
				path, report.get('schema')), file=sys.stderr)
			continue
		report['_path'] = path
		reports.append(report)
	return reports


def placement(report):
	"""'own CPUs' when the service and the clients each had their own,
	and 'shared CPUs' when the kernel placed them together."""
	where = report.get('environment', {}).get('placement') or {}
	return 'own CPUs' if where.get('client_cpus') else 'shared CPUs'


def group_key(report):
	workload = report['workload']
	name = workload['id'] + (' events' if workload.get('events') else '')
	return (name, workload['payload_bytes'], workload['clients'],
		placement(report))


# Which halves of a v2 run were Python. A report from before the binding
# existed names neither, and both were C.
FLAVOURS = {
	('c', 'c'): 'v2',
	('python', 'c'): 'v2 py-client',
	('c', 'python'): 'v2 py-service',
	('python', 'python'): 'v2 py-both',
	('c', 'qfw'): 'v2 c',
	('python', 'qfw'): 'v2 python',
	('qfw', 'qfw'): 'v2 qfw',
}


def describe(report):
	environment = report.get('environment', {})
	client = environment.get('client_language', 'c')
	if report['run']['defw_major'] == 1:
		prefix = 'v1 qfw' if client == 'qfw' else 'v1'
		return '{} {}'.format(prefix, report['transport']['kind'])
	flavour = FLAVOURS[(client, environment.get('service_language', 'c'))]
	return '{} {}'.format(flavour, report['transport']['kind'])


# The v1 provider each v2 provider is measured against. The design compares
# like with like, so the two shared-memory transports pair with each other.
MATCHING_V1 = {
	'ofi+tcp': 'ofi+tcp',
	'ofi+cxi': 'ofi+cxi',
	'na+sm': 'ofi+sm2',
}


def baseline_for(report, baselines):
	"""The v1 run a v2 run is compared against, and whether it is on the
	matching provider."""
	wanted = MATCHING_V1.get(report['transport']['kind'])
	for candidate in baselines:
		if candidate['transport']['kind'] == wanted:
			return candidate, True
	return (baselines[0], False) if baselines else (None, False)


def row_order(report):
	return (report['run']['defw_major'], report['transport']['kind'])


def against_text(baseline, matched):
	if baseline is None:
		return ''
	text = 'v1 ' + baseline['transport']['kind']
	return text if matched else text + ', unmatched'


def ratio(value, baseline, better):
	"""How much better or worse than the baseline, as a plain number."""
	if not value or not baseline:
		return ''
	times = value / baseline if better == 'higher' else baseline / value
	return '{:.1f}x'.format(times) if times >= 1 else '{:.2f}x'.format(times)


def value_text(value):
	if value is None:
		return '-'
	if value >= 1000:
		return '{:.0f}'.format(value)
	return '{:.3f}'.format(value)


def print_group(key, reports):
	workload, payload, clients, where = key
	baselines = [r for r in reports if r['run']['defw_major'] == 1]
	metrics = metrics_of(reports)

	print()
	print('{}  {}  {} client{}  {}'.format(
		workload, common.format_size(payload), clients,
		'' if clients == 1 else 's', where))
	kinds = [b['transport']['kind'] for b in baselines]
	for kind in sorted(set(kinds)):
		if kinds.count(kind) > 1:
			first = baselines[kinds.index(kind)]
			print('  v1 {} has {} runs, compared against {}'.format(
				kind, kinds.count(kind),
				os.path.dirname(first['_path'])))

	width = max(len(describe(r)) for r in reports)
	header = '  {:<{w}}'.format('', w=width)
	for name, _, _ in metrics:
		header += '  {:>15}'.format(name)
	if baselines and len(baselines) < len(reports):
		header += '  against'
	print(header)

	for report in sorted(reports, key=row_order):
		baseline, matched = None, False
		if report['run']['defw_major'] != 1:
			baseline, matched = baseline_for(report, baselines)
		line = '  {:<{w}}'.format(describe(report), w=width)
		for name, better, get in metrics:
			value = measured(get, report)
			text = value_text(value)
			if baseline is not None:
				against = ratio(value, measured(get, baseline),
						better)
				if against:
					text += ' ' + against
			line += '  {:>15}'.format(text)
		if baseline is not None:
			line += '  ' + against_text(baseline, matched)
		print(line)
		if report['failed_calls']:
			print('  {:<{w}}  {} calls FAILED'.format(
				'', report['failed_calls'], w=width))


def json_run(report, baselines, metrics):
	run = {
		'version': report['run']['defw_major'],
		'flavour': describe(report),
		'transport': report['transport']['kind'],
		'label': report['run']['label'],
		'path': report['_path'],
		'metrics': {name: measured(get, report)
			    for name, _, get in metrics},
		'failed_calls': report['failed_calls'],
	}
	if report['run']['defw_major'] != 1:
		baseline, matched = baseline_for(report, baselines)
		run['against'] = None if baseline is None else {
			'transport': baseline['transport']['kind'],
			'path': baseline['_path'],
			'matched': matched,
		}
	return run


def main(argv):
	parser = argparse.ArgumentParser(
		description='Compare DEFw v1 and v2 benchmark reports.')
	parser.add_argument('paths', nargs='+',
			    help='run directories or summary.json files')
	parser.add_argument('--json', help='also write the comparison here')
	args = parser.parse_args(argv)

	reports = find_summaries(args.paths)
	if not reports:
		raise SystemExit('no reports found')

	groups = {}
	for report in reports:
		groups.setdefault(group_key(report), []).append(report)

	print('{} reports in {} workloads'.format(len(reports), len(groups)))
	for key in sorted(groups):
		print_group(key, groups[key])
	print()

	if args.json:
		out = []
		for key in sorted(groups):
			workload, payload, clients, where = key
			baselines = [r for r in groups[key]
				     if r['run']['defw_major'] == 1]
			metrics = metrics_of(groups[key])
			out.append({
				'workload': workload,
				'payload_bytes': payload,
				'clients': clients,
				'placement': where,
				'runs': [json_run(r, baselines, metrics)
					 for r in groups[key]],
			})
		common.write_json(args.json, out, indent=2)
		print('wrote {}'.format(args.json))
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
