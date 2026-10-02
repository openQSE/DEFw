"""v1's defw_app_util, the helpers an application uses, on DEFw v2.

defw_get_directory_service and defw_connect_service_by_name are here,
with v1's waiting and v1's errors. Spawning services is not: that is the
launcher's job, and on v2 the launcher runs defw2-python --serve.
"""

import logging
import time

import defw
from defw_exception import DEFwReserveError

SYSTEM_UP_TIMEOUT = 40


def defw_get_directory_service(timeout=SYSTEM_UP_TIMEOUT):
	"""v1 waited for its agent to find the directory. A v2 process is
	told where the directory is, so there is nothing to wait for."""
	dirsvc = defw.dirsvc
	if dirsvc is None:
		logging.defw_app("Couldn't find a directory service")
		raise DEFwReserveError("Couldn't find a directory service")
	return dirsvc


def defw_connect_service_by_name(dirsvc, service_name,
				 timeout=SYSTEM_UP_TIMEOUT,
				 service_type=None,
				 binding_name=None,
				 selector_resource=None,
				 selector_alias=None,
				 properties=None):
	wait = 0
	bindings = []
	filters = {'service_name': service_name}
	for key, value in (
			('service_type', service_type),
			('binding_name', binding_name),
			('selector_resource', selector_resource),
			('selector_alias', selector_alias),
			('properties', properties),
	):
		if value:
			filters[key] = value
	while wait < timeout:
		bindings = dirsvc.resolve_services(**filters)
		if bindings:
			break
		wait += 1
		logging.defw_app(f"Waiting to connect to {service_name}")
		time.sleep(1)

	if not bindings:
		raise DEFwReserveError(f"Couldn't connect to a {service_name}")

	logging.defw_app(f"Received directory bindings: {bindings}")
	return [defw.connect_to_binding(binding) for binding in bindings]


def __getattr__(name):
	raise AttributeError(
		"defw2.compat's defw_app_util has no {}: only the directory "
		'helpers are on v2'.format(name))
