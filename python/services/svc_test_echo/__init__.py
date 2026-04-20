from .svc_test_echo import TestEcho

SERVICE_NAME = 'TestEcho'
SERVICE_DESC = 'Per-connection echo service for DEFw self-tests'

svc_info = {
	'name': SERVICE_NAME,
	'module': __name__,
	'description': SERVICE_DESC,
	'version': 1.0,
	'instance_mode': 'per_connection',
}

service_classes = [TestEcho]


def initialize():
	return None


def uninitialize():
	return None
