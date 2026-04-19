from defw_remote import BaseRemote


class TestEcho(BaseRemote):
	def __init__(self, si):
		super().__init__(service_info=si)

	def get_instance_id(self):
		pass

	def echo(self, value):
		pass

	def raise_error(self):
		pass
