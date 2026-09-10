#!/usr/bin/env python3

import logging
import threading

from svc_launcher.svc_launcher import Launcher


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def main():
	logging.defw_service = lambda *args, **kwargs: None
	baseline = len(threading.enumerate())
	metadata_launcher = Launcher(start=False)
	expect(metadata_launcher._Launcher__monitor_thr is None,
	       "metadata-only launcher created a monitor thread")
	expect(len(threading.enumerate()) == baseline,
	       "metadata-only launcher changed the active thread count")

	runtime_launcher = Launcher()
	monitor = runtime_launcher._Launcher__monitor_thr
	expect(monitor is not None and monitor.is_alive(),
	       "runtime launcher did not start its monitor thread")
	runtime_launcher.shutdown()
	monitor.join(timeout=2)
	expect(not monitor.is_alive(),
	       "runtime launcher monitor did not stop")
	print("launcher contract tests passed")


if __name__ == '__main__':
	main()
