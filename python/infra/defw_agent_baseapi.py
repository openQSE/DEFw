import defw
import defw_common_def as common
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
				logging.defw_stacktrace(
					"Failed to query service metadata for %s from %s",
					getattr(c, "__name__", c),
					getattr(module, "__name__", module),
					exc_info=True,
				)
		return svcs

	'''
	reserve the svc passed in from the agent described by info
	'''
	def reserve(self, svc_info, client_ep, *args, **kwargs):
		from defw import services
		from defw_workers import (
			INSTANCE_MODE_SINGLETON,
			get_instance_mode,
		)
		class_name = svc_info.get_class_name()
		mod_name = svc_info.get_module_name()
		if mod_name in services:
			mod = services[mod_name]
			instance_mode = get_instance_mode(mod)
			for c in mod.service_classes:
				if class_name == c.__name__:
					if instance_mode == INSTANCE_MODE_SINGLETON:
						obj = common.get_or_create_singleton_instance(
							mod_name,
							class_name,
							lambda: c(),
						)
						logging.defw_core(
							f"Reserving singleton service {class_name} "
							f"through shared instance {id(obj)}"
						)
					else:
						obj = c()
						logging.defw_core(
							f"Reserving per-connection service {class_name} "
							f"through temporary instance {id(obj)}"
						)
					return obj.reserve(svc_info, client_ep, *args, **kwargs)

	def release(self, services):
		prformat(fg.bold+fg.lightgrey+bg.red, "Client doesn't implement RELEASE API")
		pass

def query_service_info(ep, name=None):
	logging.defw_core(f"Query service on endpoint {ep}")
	client_api = BaseAgentAPI(target=ep)
	svcs = client_api.query()
	logging.defw_core(f"Got service infos: {svcs}")
	if name:
		for svc in svcs:
			logging.defw_core(f"SVC info ---{type(svc)}--- is {svc.get_service_name()} <-> {name}")
			if name == svc.get_service_name():
				return svc
		return []
	return svcs
