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
	services = defw_spawn_services('svc_test_echo')
	echo = None
	try:
		resmgr = defw_get_resource_mgr()
		echo = defw_reserve_service_by_name(resmgr, "TestEcho")[0]
		try:
			echo.raise_error()
		except Exception as exc:
			rendered = str(exc)
			has_real_newlines = "\n" in rendered
			has_escaped_newlines = "\\n" in rendered
			status = PASS if has_real_newlines and not has_escaped_newlines else FAIL
			return defwrc(
				status,
				has_real_newlines=has_real_newlines,
				has_escaped_newlines=has_escaped_newlines,
				exception_type=type(exc).__name__,
				rendered_exception=rendered,
			)
		return defwrc(FAIL, msg="remote exception was not raised")
	finally:
		if echo is not None:
			try:
				echo.shutdown()
			except Exception:
				pass
		defw_shutdown_services(services)
