import uuid

from defw_agent_info import Capability, DEFwServiceInfo
from defw_exception import DEFwError


class TestEcho:
	def __init__(self, start=True):
		self._instance_id = str(uuid.uuid4())

	def query(self):
		cap = Capability(1, 1, "default test echo capability")
		return DEFwServiceInfo(
			"TestEcho",
			"Per-connection echo service for DEFw self-tests",
			self.__class__.__name__,
			self.__class__.__module__,
			cap,
			-1,
		)

	def reserve(self, svc, client_ep, *args, **kwargs):
		return None

	def release(self, services=None):
		return None

	def get_instance_id(self):
		return self._instance_id

	def echo(self, value):
		return value

	def raise_error(self):
		raise DEFwError("intentional self-test error")
