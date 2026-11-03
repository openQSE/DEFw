#!/usr/bin/env python3

import logging
import os
import shlex
import sys
import threading
import time

from defw_exception import DEFwInProgress
from svc_launcher.svc_launcher import Launcher

# More than a 64 KiB pipe buffer on each stream.
BIG = 256 * 1024
WRITER = (
	"import sys; sys.stdout.write('o' * {0}); sys.stdout.flush(); "
	"sys.stderr.write('e' * {0}); sys.exit({1})")


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def python_cmd(code):
	return f"{shlex.quote(sys.executable)} -c {shlex.quote(code)}"


def wait_for(check, message, timeout=10):
	deadline = time.monotonic() + timeout
	while time.monotonic() < deadline:
		result = check()
		if result:
			return result
		time.sleep(0.02)
	raise AssertionError(message)


def status_or_none(launcher, pid):
	try:
		return launcher.status(pid)
	except DEFwInProgress:
		return None


def gone(pid):
	try:
		os.kill(pid, 0)
	except ProcessLookupError:
		return True
	return False


def main():
	logging.defw_service = lambda *args, **kwargs: None
	baseline = threading.active_count()

	def idle():
		return threading.active_count() == baseline

	Launcher(start=False)
	launcher = Launcher()
	expect(idle(), "creating a launcher started a thread")

	pid = launcher.launch(python_cmd(WRITER.format(BIG, 3)))
	stdout, stderr, rc = wait_for(lambda: status_or_none(launcher, pid),
				      "a process writing more than a pipe "
				      "buffer did not finish")
	expect(rc == 3, f"expected rc 3, got {rc}")
	expect(stdout == b'o' * BIG and stderr == b'e' * BIG,
	       "output was lost")
	wait_for(idle, "the waiter thread outlived its process")
	try:
		launcher.status(pid)
		raise AssertionError("status was reported twice")
	except DEFwInProgress:
		pass

	stdout, stderr, rc = launcher.launch(python_cmd(WRITER.format(BIG, 0)),
					     wait=True)
	expect(rc == 0 and len(stdout) == BIG and len(stderr) == BIG,
	       "launch(wait=True) lost output")
	expect(idle(), "launch(wait=True) left a thread behind")

	sleeper = python_cmd("import time; time.sleep(60)")
	pid = launcher.launch(sleeper)
	expect(status_or_none(launcher, pid) is None,
	       "a running process reported a status")
	launcher.kill(pid)
	expect(gone(pid), "kill() returned before the process was reaped")
	wait_for(idle, "the waiter thread outlived a killed process")

	pids = [launcher.launch(sleeper) for _ in range(3)]
	launcher.shutdown()
	expect(all(gone(pid) for pid in pids),
	       "shutdown() left a process running")
	wait_for(idle, "waiter threads outlived shutdown()")
	print("launcher contract tests passed")


if __name__ == '__main__':
	main()
