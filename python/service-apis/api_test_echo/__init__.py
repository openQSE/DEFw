from .api_test_echo import TestEcho

svc_info = {
	'name': 'TestEcho',
	'description': 'Per-connection echo service API for DEFw self-tests',
	'version': 1.0,
}

service_classes = [TestEcho]


def initialize():
	return None


def uninitialize():
	return None
