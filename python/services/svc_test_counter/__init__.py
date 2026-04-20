from .svc_test_counter import TestCounter

SERVICE_NAME = 'TestCounter'
SERVICE_DESC = 'Singleton counter service for DEFw self-tests'

svc_info = {
	'name': SERVICE_NAME,
	'module': __name__,
	'description': SERVICE_DESC,
	'version': 1.0,
	'instance_mode': 'singleton',
}

service_classes = [TestCounter]


def initialize():
	return None


def uninitialize():
	return None
