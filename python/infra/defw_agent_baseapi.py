import defw
from defw_remote import BaseRemote
from defw_util import prformat, fg, bg
import os, traceback, logging

class BaseAgentAPI(BaseRemote):
	def __init__(self, target=None, *args, **kwargs):
		super().__init__(target=target, *args, **kwargs)

	def query(self):
		# go over each of the service in each of the services module and
		# call their query function. If they don't have a query function
		# then they won't be picked up or advertised.
		#
		from defw import services
		svcs = []
		for svc, module in services:
			if module.svc_info['name'] == 'Resource Manager':
				if defw.me.is_resmgr():
					svcs.append(defw.resmgr.query())
				continue
			try:
				for c in module.service_classes:
					obj = c(start=False)
					svcs.append(obj.query())
			except Exception:
				logging.exception(
					"Failed to query service metadata for %s from %s",
					getattr(c, "__name__", c),
					getattr(module, "__name__", module),
				)
		return svcs

	'''
	reserve the svc passed in from the agent described by info
	'''
	def reserve(self, svc_info, client_ep, *args, **kwargs):
		from defw import services
		class_name = svc_info.get_class_name()
		mod_name = svc_info.get_module_name()
		if mod_name in services:
			mod = services[mod_name]
			for c in mod.service_classes:
				if class_name == c.__name__:
					obj = c()
					return obj.reserve(svc_info, client_ep, *args, **kwargs)

	def release(self, services):
		prformat(fg.bold+fg.lightgrey+bg.red, "Client doesn't implement RELEASE API")
		pass

def query_service_info(ep, name=None):
	logging.debug(f"Query service on endpoint {ep}")
	client_api = BaseAgentAPI(target=ep)
	svcs = client_api.query()
	logging.debug(f"Got service infos: {svcs}")
	if name:
		for svc in svcs:
			logging.debug(f"SVC info ---{type(svc)}--- is {svc.get_service_name()} <-> {name}")
			if name == svc.get_service_name():
				return svc
		return []
	return svcs
