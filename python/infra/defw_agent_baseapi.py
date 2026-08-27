import defw
from defw_remote import BaseRemote
import logging

class BaseAgentAPI(BaseRemote):
	def __init__(self, target=None, *args, **kwargs):
		super().__init__(
			target=target,
			remote_module="defw_agent_baseapi",
			remote_class="BaseAgentAPI",
			*args,
			**kwargs)

	def query(self):
		# Query each service class for its metadata advertisement.
		from defw import services
		svcs = []
		for svc, module in services:
			if module.svc_info['name'] == 'Directory Service':
				if defw.me.is_dirsvc():
					svcs.append(defw.dirsvc.query())
				continue
			try:
				for c in module.service_classes:
					obj = c(start=False)
					svcs.append(obj.query())
			except Exception:
				logging.defw_stacktrace(
					"Failed to query service metadata for %s from %s",
					getattr(c, "__name__", c),
					getattr(module, "__name__", module),
					exc_info=True,
				)
		return svcs

def query_service_info(ep, name=None):
	logging.defw_core(f"Query service on endpoint {ep}")
	client_api = BaseAgentAPI(target=ep)
	svcs = client_api.query()
	logging.defw_core(f"Got service infos: {svcs}")
	if name:
		for svc in svcs:
			service_name = svc.get('service_name')
			logging.defw_core(
				f"Service metadata is {service_name} <-> {name}")
			if name == service_name:
				return svc
		return []
	return svcs
