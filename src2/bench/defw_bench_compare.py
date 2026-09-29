#!/usr/bin/env python3
"""Compare DEFw benchmark runs.

Both harnesses write the same report, so a comparison is a join on the
workload. Give it run directories or summary files, in any mix:

	defw_bench_compare.py /tmp/defw-bench /mnt/defw2-baselines/2026-09-23-tcp

Runs are grouped by workload, payload and client count. Within a group the
v1 runs are the baseline and each v2 run is shown against it. A group with
only one version is still listed, because a missing half is worth seeing.
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
METRICS = (
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
)


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


def group_key(report):
	workload = report['workload']
	return (workload['id'], workload['payload_bytes'], workload['clients'])


# Which halves of a v2 run were Python. A report from before the binding
# existed names neither, and both were C.
FLAVOURS = {
	('c', 'c'): 'v2',
	('python', 'c'): 'v2 py-client',
	('c', 'python'): 'v2 py-service',
	('python', 'python'): 'v2 py-both',
}


def describe(report):
	if report['run']['defw_major'] == 1:
		return 'v1 ' + report['transport']['kind']
	environment = report.get('environment', {})
	flavour = FLAVOURS[(environment.get('client_language', 'c'),
			    environment.get('service_language', 'c'))]
	return '{} {}'.format(flavour, report['transport']['kind'])


def ratio(value, baseline, better):
	"""How much better or worse than the baseline, as a plain number."""
	if value is None or baseline in (None, 0):
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
	workload, payload, clients = key
	baselines = [r for r in reports if r['run']['defw_major'] == 1]
	baseline = baselines[0] if baselines else None

	print()
	print('{}  {}  {} client{}'.format(
		workload, common.format_size(payload), clients,
		'' if clients == 1 else 's'))
	if baseline is not None and len(baselines) > 1:
		print('  baseline: {}'.format(os.path.dirname(baseline['_path'])))

	width = max(len(describe(r)) for r in reports)
	header = '  {:<{w}}'.format('', w=width)
	for name, _, _ in METRICS:
		header += '  {:>14}'.format(name)
	print(header)

	for report in sorted(reports, key=lambda r: r['run']['defw_major']):
		line = '  {:<{w}}'.format(describe(report), w=width)
		for name, better, get in METRICS:
			value = get(report)
			text = value_text(value)
			if baseline is not None and report is not baseline:
				against = ratio(value, get(baseline), better)
				if against:
					text += ' ' + against
			line += '  {:>14}'.format(text)
		print(line)
		if report['failed_calls']:
			print('  {:<{w}}  {} calls FAILED'.format(
				'', report['failed_calls'], w=width))


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
			workload, payload, clients = key
			out.append({
				'workload': workload,
				'payload_bytes': payload,
				'clients': clients,
				'runs': [{
					'version': r['run']['defw_major'],
					'transport': r['transport']['kind'],
					'label': r['run']['label'],
					'path': r['_path'],
					'metrics': {name: get(r)
						    for name, _, get in METRICS},
					'failed_calls': r['failed_calls'],
				} for r in groups[key]],
			})
		common.write_json(args.json, out, indent=2)
		print('wrote {}'.format(args.json))
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
