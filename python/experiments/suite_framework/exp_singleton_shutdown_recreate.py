from defw_app_util import (
	defw_get_resource_mgr,
	defw_reserve_service_by_name,
	defw_shutdown_services,
	defw_spawn_services,
)
from defw_remote import defwrc

PASS = 0
FAIL = -1


def run():
	services = defw_spawn_services('svc_test_counter')
	try:
		resmgr = defw_get_resource_mgr()
		first = defw_reserve_service_by_name(resmgr, "TestCounter")[0]
		first_id = first.get_instance_id()
		first.shutdown()
		second = defw_reserve_service_by_name(resmgr, "TestCounter")[0]
		second_id = second.get_instance_id()
		status = PASS if first_id != second_id else FAIL
		return defwrc(
			status,
			first_instance_id=first_id,
			second_instance_id=second_id,
		)
	finally:
		defw_shutdown_services(services)
