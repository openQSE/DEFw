import uuid

import defw_common_def as common


class TestCounter:
	def __init__(self, start=True):
		self._instance_id = str(uuid.uuid4())
		self._count = 0

	def query(self):
		return {
			'service_name': 'TestCounter',
			'service_type': 'defw.test.counter',
			'api_bindings': [{
				'binding_name': 'default',
				'client_module': 'api_test_counter',
				'client_class': 'TestCounter',
				'service_module': self.__class__.__module__,
				'service_class': self.__class__.__name__,
				'version': 1,
			}],
			'selector': {'resources': ['TestCounter']},
			'properties': {
				'description': (
					'Singleton counter service for DEFw self-tests'),
			},
			'capability': {
				'type': 1,
				'caps': 1,
				'description': 'default test counter capability',
			},
		}

	def get_instance_id(self):
		return self._instance_id

	def increment(self):
		self._count += 1
		return self._count

	def get_count(self):
		return self._count

	def shutdown(self):
		common.shutdown_service_instance(self)
		return True
