#!/usr/bin/env python3
"""The SPANK reserve and release flow in C, run against a QPM.

defw2-spank-flow is the C caller experience the design asks for: the
plugin's reserve and release, against the public headers, in under one
hundred lines of code. This runs it against the Python fake QPM, registered
in a directory of its own, as a plugin would run it against a site's QPM,
and counts its lines the way the comparison counts DEFw's.

	defw2_spank_flow_check.py --flow PATH --dirsvc PATH --fake PATH
"""

import argparse
import os
import subprocess
import sys
import time

import defw2

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'bench'))

import defw_loc  # noqa: E402

FAKE_ID = 'qpm:fake:fake-20q-py'
FLOW_SOURCE = os.path.join(os.path.dirname(HERE), 'examples',
			   'defw2_spank_flow.c')
failures = []


def check(what, ok):
	print('{:<64} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)


def start(command, env):
	"""A service that prints its address, and that address."""
	process = subprocess.Popen(command, stdin=subprocess.PIPE,
				   stdout=subprocess.PIPE, env=env, text=True)
	return process, process.stdout.readline().strip()


def flow(args, env, *argv):
	return subprocess.run([args.flow, *argv], env=env, capture_output=True,
			      text=True, timeout=60)


def registered(address):
	"""Wait for the fake to be in the directory."""
	runtime = defw2.Runtime(role='client', node_name='spank-check')
	try:
		with defw2.Directory(runtime, address) as directory:
			deadline = time.monotonic() + 30
			while time.monotonic() < deadline:
				if directory.resolve(service_id=FAKE_ID):
					return True
				time.sleep(0.05)
	finally:
		runtime.close()
	return False


def main(argv):
	parser = argparse.ArgumentParser()
	parser.add_argument('--flow', required=True)
	parser.add_argument('--dirsvc', required=True)
	parser.add_argument('--fake', required=True)
	args = parser.parse_args(argv)

	lines = defw_loc.count_file(FLOW_SOURCE)
	check('the flow is {} lines of code, under one hundred'.format(lines),
	      lines < 100)

	env = dict(os.environ)
	dirsvc, address = start([args.dirsvc], env)
	env['DEFW2_DIRSVC'] = address
	fake, _ = start([sys.executable, args.fake, '--register'], env)
	try:
		check('the QPM registers', registered(address))

		done = flow(args, env, 'reserve', FAKE_ID, 'slurm-42', 'alice',
			    '4', '1024')
		check('reserve prints the reservation the QPM granted',
		      done.returncode == 0 and done.stdout == '7001\n')
		if done.returncode != 0:
			print('  ' + done.stderr.strip())
		done = flow(args, env, 'release', FAKE_ID, '7001')
		check('release releases it', done.returncode == 0)
		if done.returncode != 0:
			print('  ' + done.stderr.strip())

		done = flow(args, env, 'reserve', 'qpm:none', 'slurm-42',
			    'alice', '4', '1024')
		check('a QPM the directory does not have is said so',
		      done.returncode == 1 and done.stdout == '' and
		      'no such QPM' in done.stderr)
		done = flow(args, env, 'release', 'qpm:none', '7001')
		check('for release too',
		      done.returncode == 1 and 'no such QPM' in done.stderr)
		done = flow(args, env, 'reserve', FAKE_ID)
		check('a wrong command line is a usage error',
		      done.returncode == 2 and 'usage' in done.stderr)
	finally:
		fake.stdin.close()
		fake.wait(timeout=30)
		dirsvc.stdin.close()
		dirsvc.terminate()
		dirsvc.wait(timeout=30)

	print('SPANK FLOW CHECK ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
