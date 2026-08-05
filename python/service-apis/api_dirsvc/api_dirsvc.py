from api_resmgr.api_resmgr import DEFwResMgr


class DEFwDirSvc(DEFwResMgr):
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
