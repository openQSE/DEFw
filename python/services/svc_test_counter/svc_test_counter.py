import uuid

import defw_common_def as common
from defw_agent_info import Capability, DEFwServiceInfo


class TestCounter:
	def __init__(self, start=True):
		self._instance_id = str(uuid.uuid4())
		self._count = 0
		self._ref_count = 0

	def query(self):
		cap = Capability(1, 1, "default test counter capability")
		return DEFwServiceInfo(
			"TestCounter",
			"Singleton counter service for DEFw self-tests",
			self.__class__.__name__,
			self.__class__.__module__,
			cap,
			-1,
		)

	def reserve(self, svc, client_ep, *args, **kwargs):
		self._ref_count += 1
		return None

	def release(self, services=None):
		if self._ref_count > 0:
			self._ref_count -= 1
		return None

	def get_instance_id(self):
		return self._instance_id

	def increment(self):
		self._count += 1
		return self._count

	def get_count(self):
		return self._count

	def shutdown(self):
		if self._ref_count > 0:
			self._ref_count -= 1
		if self._ref_count == 0:
			common.shutdown_service_instance(self)
			return True
		return False
