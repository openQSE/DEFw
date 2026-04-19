from defw_remote import BaseRemote


class TestCounter(BaseRemote):
	def __init__(self, si):
		super().__init__(service_info=si)

	def get_instance_id(self):
		pass

	def increment(self):
		pass

	def get_count(self):
		pass

	def shutdown(self):
		pass
