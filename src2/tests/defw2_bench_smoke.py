#!/usr/bin/env python3
"""The v2 benchmark harness, run end to end with few calls.

The comparison's numbers come from the harness, so this runs its launcher
the way a campaign does and checks what the reports say: W1 against the
echo service, W4 against a directory, and W5 against the Python fake QPM,
by polling and on completion events, from the C and the Python clients. It
checks the wire bytes each counts, the polls and events a job takes, and
that the processes ran where they were put, with transparent huge pages
off, and then compares the reports as a campaign does. The pieces that
need no DEFw are checked on their own first.

	defw2_bench_smoke.py --bin-dir DIR --fake PATH
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.join(os.path.dirname(HERE), 'bench')
sys.path.insert(0, BENCH)

import defw_bench_common as common  # noqa: E402

FAKE_ID = 'qpm:fake:fake-20q-py'
failures = []


def check(what, ok, found=None):
	print('{:<64} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)
		if found is not None:
			print('  found {}'.format(found))


def pieces():
	"""What the harness computes without DEFw."""
	check('CPU lists read and print as Linux lists them',
	      common.cpu_set('0-2,5') == {0, 1, 2, 5} and
	      common.cpu_text({0, 1, 2, 5, 7, 8}) == '0-2,5,7-8')

	# v1's four message paths, wrapped as a v1 client wraps them.
	sent = []
	workers = types.SimpleNamespace(
		defw_send_req=lambda *a: sent.append('req') or 0,
		defw_send_rsp=lambda *a: sent.append('rsp') or 0,
		put_request=lambda *a: sent.append('in-req'),
		put_response=lambda *a: sent.append('in-rsp'))
	messages = common.V1Messages(workers)
	workers.defw_send_req('peer', 'blk', 'before')
	messages.start()
	workers.defw_send_req('peer', 'blk', 'abc')
	workers.put_response('de', 'uuid')
	workers.put_request('fghi', 'uuid')
	workers.defw_send_rsp('peer', 'blk', 'j')
	messages.stop()
	workers.put_response('after', 'uuid')
	check('v1 counts both ways while counting, and only then',
	      messages.counts() == {'rpcs': 2, 'request_bytes': 4 + 5,
				    'responses': 2, 'response_bytes': 3 + 2} and
	      len(sent) == 6)


def launch(args, out, *argv):
	"""One launcher run, and its report."""
	command = [sys.executable, os.path.join(BENCH, 'v2', 'defw2_bench.py'),
		   *argv, '--bin-dir', args.bin_dir, '--out', out,
		   '--timeout', '120']
	done = subprocess.run(command, capture_output=True, text=True,
			      timeout=300)
	runs = sorted(glob.glob(os.path.join(out, '*', 'summary.json')),
		      key=os.path.getmtime)
	if done.returncode != 0 or not runs:
		print(done.stdout[-2000:] + done.stderr[-2000:])
		return None
	with open(runs[-1], encoding='utf-8') as stream:
		return json.load(stream)


def wire(report):
	return (report or {}).get('wire') or {}


def placed(report):
	return (report or {}).get('environment', {}).get('placement') or {}


def echo_and_directory(args, out):
	cpus = sorted(os.sched_getaffinity(0))
	pinned = ['--service-cpus', str(cpus[-1]), '--client-cpus',
		  str(cpus[0])]
	report = launch(args, out, 'W1', '--transport', 'na+sm', '--calls',
			'200', '--warmup', '10', *pinned)
	check('W1 runs', report is not None and report['failed_calls'] == 0)
	check('W1 counts one RPC a call, and bytes both ways',
	      wire(report).get('rpcs_per_call') == 1 and
	      wire(report).get('request_bytes_per_call', 0) > 64 and
	      wire(report).get('response_bytes_per_call', 0) > 64)
	seen = placed(report).get('seen') or {}
	check('the service and the client ran on the CPUs they were given',
	      seen.get('service_cpus') == str(cpus[-1]) and
	      seen.get('client_cpus') == [str(cpus[0])])
	check('with transparent huge pages off',
	      placed(report).get('thp_disabled') is True and
	      seen.get('thp_disabled') is True)

	for client in ('c', 'python'):
		report = launch(args, out, 'W4', '--transport', 'na+sm',
				'--client', client, '--calls', '100',
				'--warmup', '5')
		check('W4 from {} resolves'.format(client),
		      report is not None and report['failed_calls'] == 0)
		check('and counts the resolves\' bytes both ways',
		      wire(report).get('rpcs_per_call') == 1 and
		      wire(report).get('request_bytes_per_call', 0) > 0 and
		      wire(report).get('response_bytes_per_call', 0) > 0,
		      wire(report))


def job(args, out, address, client, events):
	"""W5 from client, polling or on events, and what its report says
	about a job."""
	mode = ['--events'] if events else []
	report = launch(args, out, 'W5', '--transport', 'na+sm',
			'--directory', address, '--service-id', FAKE_ID,
			'--client', client, '--calls', '50', '--warmup', '2',
			*mode)
	check('W5 from {}, {}'.format(client, 'events' if events else
				       'polling'),
	      report is not None and report['failed_calls'] == 0)
	qpm = (report or {}).get('qpm') or {}
	polls = qpm.get('polls') or {}
	taken = (qpm.get('events') or {}).get('mean')
	if events:
		check('one event a job, and one read_cq after it',
		      taken == 1 and polls.get('max') == 1, (taken, polls))
		check('three RPCs a job, the event among them',
		      wire(report).get('rpcs_per_call') == 3, wire(report))
	else:
		check('polls until the job is done, and takes no event',
		      polls.get('mean', 0) >= 1 and taken == 0,
		      (taken, polls))


def jobs(args, out):
	"""W5 against the Python fake QPM, found through a directory."""
	env = dict(os.environ)
	dirsvc = subprocess.Popen([os.path.join(args.bin_dir, 'defw2-dirsvc')],
				  stdout=subprocess.PIPE, text=True, env=env)
	address = dirsvc.stdout.readline().strip()
	env['DEFW2_DIRSVC'] = address
	fake = subprocess.Popen([sys.executable, args.fake, '--register'],
				stdin=subprocess.PIPE, stdout=subprocess.PIPE,
				text=True, env=env)
	fake.stdout.readline()
	try:
		for client in ('c', 'python'):
			for events in (False, True):
				job(args, out, address, client, events)
	finally:
		fake.stdin.close()
		fake.wait(timeout=30)
		dirsvc.terminate()
		dirsvc.wait(timeout=30)


def compare(out):
	"""The reports, compared as a campaign compares them."""
	path = os.path.join(out, 'compare.json')
	done = subprocess.run(
		[sys.executable, os.path.join(BENCH, 'defw_bench_compare.py'),
		 out, '--json', path], capture_output=True, text=True,
		timeout=60)
	groups = {}
	if done.returncode == 0:
		with open(path, encoding='utf-8') as stream:
			groups = {group['workload']: group
				  for group in json.load(stream)}
	check('the comparison reads every report',
	      {'W1', 'W4', 'W5', 'W5 events'} <= set(groups))
	runs = (groups.get('W5 events') or {}).get('runs') or [{}]
	check('and compares a job by what the framework costs',
	      'overhead p50 ms' in runs[0].get('metrics', {}))


def main(argv):
	parser = argparse.ArgumentParser()
	parser.add_argument('--bin-dir', required=True)
	parser.add_argument('--fake', required=True)
	args = parser.parse_args(argv)

	pieces()
	with tempfile.TemporaryDirectory(prefix='defw2-bench-smoke-') as out:
		echo_and_directory(args, out)
		jobs(args, out)
		compare(out)
	print('BENCH SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
