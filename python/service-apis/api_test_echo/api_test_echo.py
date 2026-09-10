from defw_remote import BaseRemote


class TestEcho(BaseRemote):
	def get_instance_id(self):
		pass

	def echo(self, value):
		pass

	def raise_error(self):
		pass

	def shutdown(self):
		pass
