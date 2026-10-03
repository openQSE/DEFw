"""Serving a v1 QPM service module on DEFw v2.

v1 loaded a service module, a package with svc_info, service_classes,
initialize() and uninitialize(), called initialize(), made the service
class's one instance on the first call that reached it, and called
uninitialize() at shutdown. This does the same over a defw2.ServiceHost
that serves the three QPM APIs through QPMAdapter.

The module registers itself, as QFw's QPMs do from initialize(), through
defw.dirsvc.register_service. The host serves from the start under a
provisional service ID, and the registration names the record with the
service's own.
"""

import argparse
import importlib
import logging
import os
import signal
import socket
import sys

from .._qpm import (
	API_QPM_ADMISSION,
	API_QPM_CONTROL,
	API_QPM_EXECUTION,
	QPM_APIS,
)
from .._service import ServiceHost
from . import _state, install
from ._adapter import QPMAdapter

log = logging.getLogger('defw2.compat')

SERVICE_TYPE = 'qfw.qpm'


def _service_module(name):
	module = importlib.import_module(name)
	info = getattr(module, 'svc_info', None)
	classes = getattr(module, 'service_classes', None)
	if not isinstance(info, dict) or not classes:
		raise SystemExit('defw2-python: {} is not a v1 service module: it '
				 'needs svc_info and service_classes'.format(name))
	if len(classes) != 1:
		raise SystemExit('defw2-python: {} has {} service classes, and a '
				 'v2 host serves one'.format(name, len(classes)))
	mode = info.get('instance_mode', 'singleton')
	if mode != 'singleton':
		log.warning('%s asks for %s instances; v2 serves one', name, mode)
	return module, info, classes[0]


def main(argv):
	parser = argparse.ArgumentParser(
		prog='defw2-python --serve',
		description='Serve a v1 QPM service module on DEFw v2.')
	parser.add_argument('module')
	parser.add_argument('--execution-workers', type=int, default=4,
			    help='threads answering execution calls')
	parser.add_argument('--node-name')
	args = parser.parse_args(argv)

	_state.role = 'server'
	_state.node_name = args.node_name
	install()
	module, info, cls = _service_module(args.module)

	import defw
	defw.services.append((info['name'], module))

	provisional = os.environ.get('QFW_QPM_SERVICE_ID') or '{}:{}:{}'.format(
		args.module, socket.gethostname(), os.getpid())
	host = ServiceHost(_state.runtime(), provisional, SERVICE_TYPE,
			   apis=QPM_APIS)
	_state.host = host
	host.start(QPMAdapter(factory=cls), workers={
		API_QPM_CONTROL: 1,
		API_QPM_ADMISSION: 1,
		API_QPM_EXECUTION: args.execution_workers,
	})
	log.info('serving %s at %s', args.module, host.address)

	def stop(signum, frame):
		_state.stopping.set()
	signal.signal(signal.SIGTERM, stop)
	signal.signal(signal.SIGINT, stop)

	try:
		initialize = getattr(module, 'initialize', None)
		if initialize is not None:
			initialize()
		while not _state.stopping.wait(0.5):
			pass
	finally:
		uninitialize = getattr(module, 'uninitialize', None)
		if uninitialize is not None:
			try:
				uninitialize()
			except Exception:  # noqa: BLE001
				log.exception('uninitializing %s', args.module)
		_state.close()
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
