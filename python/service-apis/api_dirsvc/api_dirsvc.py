from defw_remote import BaseRemote


class DEFwDirSvc(BaseRemote):
	def register_agent(self, client_ep, context=None):
		pass

	def deregister_agent(self, ep):
		pass

	def ready_agents(self):
		pass

	def wait_agents(self, timeout=10):
		pass

	def get_agents_context(self):
		pass

	def wait_agents_deregistration(self, timeout=10):
		pass

	def register_service(self, service_ep, context=None):
		pass

	def deregister(self, agent_ep):
		pass

	def resolve_services(self, **filters):
		pass

	def query_directory(self, include_inactive=False):
		pass

	def deregister_service(self, service_id, runtime_id, generation):
		pass

	def get_service_generation(self, service_id):
		pass

	def get_generation(self, service_id):
		pass
