from defw_app_util import (
	defw_get_directory_service,
	defw_bind_service_by_name,
	defw_shutdown_services,
	defw_spawn_services,
)
from defw_remote import defwrc

PASS = 0
FAIL = -1


def run():
	services = defw_spawn_services('svc_test_counter')
	first = None
	second = None
	try:
		dirsvc = defw_get_directory_service()
		first = defw_bind_service_by_name(dirsvc, "TestCounter")[0]
		first_id = first.get_instance_id()
		first.shutdown()
		second = defw_bind_service_by_name(dirsvc, "TestCounter")[0]
		second_id = second.get_instance_id()
		status = PASS if first_id != second_id else FAIL
		return defwrc(
			status,
			first_instance_id=first_id,
			second_instance_id=second_id,
		)
	finally:
		if second is not None:
			try:
				second.shutdown()
			except Exception:
				pass
		defw_shutdown_services(services)
