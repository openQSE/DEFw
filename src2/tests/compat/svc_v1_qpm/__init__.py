"""A v1 QPM service module, laid out as v1 loaded one.

initialize() registers the way QFw's QPMs do: it asks each loaded
service class for its advertisement and registers it with defw.dirsvc,
from the endpoint defw.me gives.
"""

from . import svc_qpm
from .svc_qpm import QPM

import defw
import defw_trace

svc_info = {
	'name': 'QPM',
	'module': __name__,
	'description': 'A v1 fake QPM for the compat test',
	'version': 1.0,
	'instance_mode': 'singleton',
}

service_classes = [QPM]


def _attach(carrier):
	svc_qpm.trace_seen = carrier.get('traceparent')
	return carrier


def initialize():
	# A service that traces, as QFw's do through their telemetry: v1
	# ran each call in the caller's trace through these hooks.
	defw_trace.set_hooks(attach=_attach, detach=lambda token: None)
	for _name, module in defw.services:
		for cls in module.service_classes:
			advertisement = cls(start=False).query()
			context = {key: advertisement.get(key) for key in (
				'service_id', 'service_name', 'service_type',
				'api_bindings', 'selector', 'properties',
				'capability', 'qpm_type', 'qpm_capabilities')}
			defw.dirsvc.register_service(defw.me.my_endpoint(),
						     context=context)
	svc_qpm.initialized = True


def uninitialize():
	svc_qpm.initialized = False
