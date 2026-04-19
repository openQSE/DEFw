from .api_test_counter import TestCounter

svc_info = {
	'name': 'TestCounter',
	'description': 'Singleton counter service API for DEFw self-tests',
	'version': 1.0,
}

service_classes = [TestCounter]


def initialize():
	return None


def uninitialize():
	return None
