#!/usr/bin/env python3
"""The echo service, in Python.

The same service defw2-echo serves in C, so a run against this one and a
run against that one differ only in the language the method runs in. That
difference is what Phase 0's Python criterion measures.

It prints its address on stdout and nothing else, so the launcher reads it
back the same way.

	defw2_echo_service.py [--workers N] [--depth N]
"""

import argparse
import signal
import sys

import defw2


def echo(method, request):
	"""The whole service. Its own cost is nothing, on purpose."""
	return request


def main(argv):
	parser = argparse.ArgumentParser(description='A Python echo service.')
	parser.add_argument(
		'--workers', type=int, default=2,
		help='threads draining the call queue (default: 2)')
	parser.add_argument(
		'--depth', type=int, default=0,
		help='calls that may wait at once, 0 for no limit')
	parser.add_argument(
		'--service-id', default='py-echo',
		help='service identifier (default: py-echo)')
	args = parser.parse_args(argv)

	runtime = defw2.Runtime(role='server', node_name=args.service_id)
	host = defw2.ServiceHost(runtime, args.service_id, depth=args.depth)

	def stop(signum, frame):
		host.stop()

	signal.signal(signal.SIGTERM, stop)
	signal.signal(signal.SIGINT, stop)

	print(host.address)
	sys.stdout.flush()

	# serve returns once a signal has closed the queue.
	host.serve(echo, workers=args.workers)
	host.close()
	runtime.close()
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
