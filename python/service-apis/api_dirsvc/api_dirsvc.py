from defw_remote import BaseRemote


class DEFwDirSvc(BaseRemote):
	def register_service(self, service_ep, context=None):
		pass

	def deregister(self, agent_ep):
		pass

	def resolve_services(self, **filters):
		pass

	def deregister_service(self, service_id, runtime_id, generation):
		pass

	def get_service_generation(self, service_id):
		pass
