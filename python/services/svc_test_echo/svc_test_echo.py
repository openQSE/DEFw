import uuid

import defw_common_def as common
from defw_exception import DEFwError


class TestEcho:
	def __init__(self, start=True):
		self._instance_id = str(uuid.uuid4())

	def query(self):
		return {
			'service_name': 'TestEcho',
			'service_type': 'defw.test.echo',
			'api_bindings': [{
				'binding_name': 'default',
				'client_module': 'api_test_echo',
				'client_class': 'TestEcho',
				'service_module': self.__class__.__module__,
				'service_class': self.__class__.__name__,
				'version': 1,
			}],
			'selector': {'resources': ['TestEcho']},
			'properties': {
				'description': (
					'Per-connection echo service for DEFw self-tests'),
			},
			'capability': {
				'type': 1,
				'caps': 1,
				'description': 'default test echo capability',
			},
		}

	def get_instance_id(self):
		return self._instance_id

	def echo(self, value):
		return value

	def raise_error(self):
		raise DEFwError("intentional self-test error")

	def shutdown(self):
		common.shutdown_service_instance(self)
		return True
