from defw_remote import BaseRemote


SERVICE_CONNECTED = 'SERVICE_CONNECTED'
SERVICE_DISCONNECTED = 'SERVICE_DISCONNECTED'


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

	def register_event_notification(self, endpoint, event_type, class_id,
					filters=None):
		pass

	def unregister_event_notification(self, registration_id):
		pass
