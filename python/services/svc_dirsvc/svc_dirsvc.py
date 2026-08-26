"""
Directory service implementation.
"""
import logging
import threading
import time
import yaml

from defw import defw_config_yaml, dump_all_agents, get_agent, me
from defw_agent_baseapi import BaseAgentAPI
from defw_agent_info import Capability, DEFwServiceInfo
from defw_exception import (
	DEFwAgentNotFound,
	DEFwCommError,
	DEFwInternalError,
	DEFwInProgress,
	DEFwNotFound,
)
import defw_directory


AGENT_STATE_CONNECTED = 1 << 0
AGENT_STATE_REGISTERED = 1 << 1
AGENT_STATE_UNREGISTERED = 1 << 2
AGENT_STATE_ERROR = 1 << 3


class DEFwDirSvc:
	SVC = 'services'
	CLT = 'clients'

	def __init__(self, sql_path):
		self.__db_lock = threading.Lock()
		self.__services_db = {}
		self.__clients_db = {}
		self.__my_ep = me.my_endpoint()
		self.__reload_entries()

	def __live_agent_ids(self):
		import defw_peers

		live_ids = set()
		for record in defw_peers.snapshot().values():
			if record.get('callable') and record.get('runtime_id'):
				live_ids.add(record['runtime_id'])
		return live_ids

	def __prune_db(self, db):
		current_ids = self.__live_agent_ids()
		with self.__db_lock:
			stale_ids = [
				agent_id for agent_id in db.keys()
				if agent_id not in current_ids
			]
			for agent_id in stale_ids:
				logging.defw_service(
					f"Pruning stale directory entry {agent_id}")
				del db[agent_id]

	def __reload_entries(self):
		self.__prune_db(self.__clients_db)
		self.__prune_db(self.__services_db)

	def __endpoint_record(self, ep):
		return {
			'address': ep.addr,
			'listen_port': ep.listen_port,
			'node_name': ep.name,
			'hostname': ep.hostname,
			'pid': ep.pid,
		}

	def __binding_for_service_info(self, service_info):
		import defw

		service_name = service_info.get_service_name()
		client_module = None
		client_class = service_name
		if service_name in defw.service_apis:
			api_class = defw.service_apis[service_name].service_classes[0]
			client_module = api_class.__module__
			client_class = api_class.__name__
		return {
			'binding_name': service_info.get_property(
				'binding_name', 'default'),
			'client_module': service_info.get_property(
				'client_module', client_module),
			'client_class': service_info.get_property(
				'client_class', client_class),
			'service_module': service_info.get_property(
				'service_module', service_info.get_module_name()),
			'service_class': service_info.get_property(
				'service_class', service_info.get_class_name()),
			'version': service_info.get_property('binding_version', 1),
		}

	def __service_info_properties(self, service_info):
		get_properties = getattr(service_info, 'get_properties', None)
		if not callable(get_properties):
			return {}
		return dict(get_properties() or {})

	def __service_info_capability(self, service_info):
		get_capabilities = getattr(service_info, 'get_capabilities', None)
		if not callable(get_capabilities):
			return {}, -1, -1
		capabilities = get_capabilities()
		if not capabilities:
			return {}, -1, -1
		capability = {}
		get_capability_dict = getattr(
			capabilities, 'get_capability_dict', None)
		if callable(get_capability_dict):
			capability = dict(get_capability_dict() or {})
		cap_type = capabilities.get_cap_type()
		caps = capabilities.get_caps()
		return capability, cap_type, caps

	def __directory_record(self, entry, service_info, context=None):
		ep = entry['agent'].get_ep()
		context = context if isinstance(context, dict) else {}
		service_name = service_info.get_service_name()
		properties = self.__service_info_properties(service_info)
		if isinstance(context.get('properties'), dict):
			context_properties = dict(context.get('properties') or {})
			context_properties.update(properties)
			properties = context_properties
		capability, cap_type, caps = (
			self.__service_info_capability(service_info))
		qpm_type = context.get('qpm_type', properties.get('qpm_type', -1))
		qpm_capabilities = context.get(
			'qpm_capabilities',
			properties.get('qpm_capabilities', -1))
		if qpm_type != -1:
			properties.setdefault('qpm_type', qpm_type)
		if qpm_capabilities != -1:
			properties.setdefault('qpm_capabilities', qpm_capabilities)
		service_id = (
			context.get('service_id') or
			service_info.get_property(
				'service_id',
				f"{service_name}:{ep.hostname}:{ep.name}"))
		selector = (
			context.get('selector') or
			service_info.get_property(
				'selector', {'resources': [service_name]}))
		service_type = context.get('service_type')
		if service_type is None:
			service_type = service_info.get_property(
				'service_type', 'defw.service')
		api_bindings = (
			context.get('api_bindings') or
			service_info.get_property('api_bindings', None))
		return {
			'service_id': service_id,
			'service_name': service_name,
			'service_type': service_type,
			'runtime_id': ep.remote_uuid,
			'peer_handle': ep.blk_uuid,
			'endpoint': self.__endpoint_record(ep),
			'api_bindings': api_bindings or [
				self.__binding_for_service_info(service_info)
			],
			'selector': selector,
			'properties': properties,
			'capability': capability,
			'qpm_type': qpm_type,
			'qpm_capabilities': qpm_capabilities,
		}

	def __register_directory_entries(self, db, agent_id, context=None):
		entry = db.get(agent_id)
		if not entry:
			return []
		if not entry.get('info'):
			entry['info'] = entry['api'].query()
		records = []
		for service_info in entry.get('info') or []:
			record = self.__directory_record(entry, service_info, context)
			records.append(defw_directory.register_service(record))
		return records

	def unset_state(self, db, aid, state):
		with self.__db_lock:
			db[aid]['state'] = db[aid]['state'] & ~state

	def set_state(self, db, aid, state):
		with self.__db_lock:
			db[aid]['state'] = db[aid]['state'] | state

	def get_state(self, db, aid):
		with self.__db_lock:
			return db[aid]['state']

	def __register(self, local_agent_dict, ep, context, query=True):
		agent_id = ep.get_id()
		logging.defw_service(
			f"register(name={ep.name}, id={agent_id}, query={query})")
		self.__reload_entries()
		agent = get_agent(ep)
		if not agent:
			logging.defw_service(
				f"Unknown agent during register: name={ep.name}, "
				f"id={agent_id}, known keys={list(local_agent_dict.keys())}")
			if agent_id in local_agent_dict:
				self.set_state(local_agent_dict, agent_id,
					       AGENT_STATE_ERROR)
			dump_all_agents()
			raise DEFwAgentNotFound(
				f"Registration from an unknown client {ep}")
		client_api = BaseAgentAPI(target=ep)
		svc_info = client_api.query() if query else []
		with self.__db_lock:
			if agent_id not in local_agent_dict:
				logging.defw_service(
					f"Setting {agent_id} state to CONNECTED")
				local_agent_dict[agent_id] = {
					'agent': agent,
					'api': client_api,
					'info': svc_info,
					'state': AGENT_STATE_CONNECTED,
				}
			else:
				local_agent_dict[agent_id]['agent'] = agent
				local_agent_dict[agent_id]['api'] = client_api
				if query:
					local_agent_dict[agent_id]['info'] = svc_info
			if context is not None:
				local_agent_dict[agent_id]['context'] = context
			local_agent_dict[agent_id]['state'] |= AGENT_STATE_REGISTERED
			loc_db = (
				DEFwDirSvc.SVC
				if local_agent_dict is self.__services_db
				else DEFwDirSvc.CLT)
			for service_info in local_agent_dict[agent_id]['info']:
				service_info.add_key(agent_id)
				service_info.add_loc_db(loc_db)
		logging.defw_service(
			f"Registered agent: name={ep.name}, id={agent_id}, "
			f"local keys={list(local_agent_dict.keys())}")

	def __deregister(self, local_agent_dict, ep):
		agent = get_agent(ep)
		if not agent:
			raise DEFwAgentNotFound(
				f"Deregistration from an unknown client {ep}")
		self.unset_state(
			local_agent_dict, ep.get_id(), AGENT_STATE_REGISTERED)

	def register_agent(self, ep, context=None):
		logging.defw_service(
			f"Agent with ep {ep} registering with directory service")
		dump_all_agents()
		self.__register(self.__clients_db, ep, context, query=False)
		self.__clients_db[ep.get_id()]['context'] = context
		state = self.get_state(self.__clients_db, ep.get_id())
		logging.defw_service(
			f"Agent with ep {ep} registered. Now in state {state}")

	def deregister_agent(self, ep):
		logging.defw_service(f"Agent with ep {ep} deregistering")
		self.__deregister(self.__clients_db, ep)

	def ready_agents(self):
		try:
			total = int(defw_config_yaml['defw']['expected-agent-count'])
		except Exception:
			raise DEFwInternalError(
				f"Bad configuration: {yaml.dump(defw_config_yaml)}")
		registered = 0
		with self.__db_lock:
			for agent, info in self.__clients_db.items():
				logging.defw_service(
					f"{agent} is in state {info['state']}")
				if info['state'] & AGENT_STATE_REGISTERED:
					registered += 1
		if total <= registered:
			return True
		raise DEFwInProgress(
			f"Missing clients. Expected {total}, registered {registered}")

	def wait_agents(self, timeout=10):
		start = time.time()
		while True:
			if time.time() - start > timeout:
				raise DEFwCommError(
					"Agents failed to connect to directory service")
			try:
				if self.ready_agents():
					break
			except Exception as error:
				if type(error) == DEFwInProgress:
					continue
				raise error
		logging.defw_service(
			f"wait_agents complete: {self.__clients_db}")

	def dereg_agents(self):
		registered = 0
		with self.__db_lock:
			for agent, info in self.__clients_db.items():
				if info['state'] & AGENT_STATE_REGISTERED:
					registered += 1
		logging.defw_service(f"Agents still registered = {registered}")
		if registered > 0:
			raise DEFwInProgress(f"Clients still registered {registered}")

	def wait_agents_deregistration(self, timeout=10):
		start = time.time()
		while True:
			if time.time() - start > timeout:
				raise DEFwCommError(
					"Agents failed to deregister from directory service")
			try:
				self.dereg_agents()
				break
			except Exception as error:
				if type(error) == DEFwInProgress:
					continue
				raise error
		logging.defw_service(
			f"wait for agent deregistration complete: {self.__clients_db}")

	def get_agents_context(self):
		contexts = {}
		logging.defw_service(f"Currently registered: {self.__clients_db}")
		with self.__db_lock:
			num_clients = len(self.__clients_db)
			for _, value in self.__clients_db.items():
				agent = value['agent']
				contexts[agent.get_pid()] = value['context']
		num_contexts = len(contexts)
		if num_contexts != num_clients:
			raise DEFwNotFound(
				"Clients didn't register properly. "
				f"Found {num_contexts}. Expected {num_clients}")
		return dict(sorted(contexts.items()))

	def register_service(self, service_ep, context=None):
		agent_id = service_ep.get_id()
		self.__register(self.__services_db, service_ep, context)
		return self.__register_directory_entries(
			self.__services_db, agent_id, context)

	def deregister(self, ep):
		agent_id = ep.get_id()
		logging.defw_service(
			f"dirsvc.deregister(name={ep.name}, id={agent_id})")
		if agent_id not in self.__clients_db and \
		   agent_id not in self.__services_db:
			raise DEFwAgentNotFound(
				f"agent {ep.name} ({agent_id}) not found")
		if agent_id in self.__services_db:
			logging.defw_service(
				f"Deregistering service entry name={ep.name}, "
				f"id={agent_id}")
			self.__services_db[agent_id]['api'].unregister()
			for record in defw_directory.query(include_inactive=True):
				if record['runtime_id'] == agent_id:
					defw_directory.deregister_service(
						record['service_id'],
						record['runtime_id'],
						record['generation'])
			del self.__services_db[agent_id]
		if agent_id in self.__clients_db:
			logging.defw_service(
				f"Deregistering client entry name={ep.name}, "
				f"id={agent_id}")
			self.__clients_db[agent_id]['api'].unregister()
			del self.__clients_db[agent_id]

	def resolve_services(self, **filters):
		return defw_directory.resolve_services(**filters)

	def query_directory(self, include_inactive=False):
		return defw_directory.query(include_inactive=include_inactive)

	def deregister_service(self, service_id, runtime_id, generation):
		return defw_directory.deregister_service(
			service_id, runtime_id, generation)

	def get_service_generation(self, service_id):
		return defw_directory.get_service_generation(service_id)

	def query(self):
		from . import SERVICE_DESC, SERVICE_NAME

		cap = Capability(1, 1, "directory service")
		return DEFwServiceInfo(
			SERVICE_NAME,
			SERVICE_DESC,
			self.__class__.__name__,
			self.__class__.__module__,
			cap,
			-1,
			properties={
				'service_type': 'defw.dirsvc',
				'api_bindings': [{
					'binding_name': 'directory',
					'client_module': 'api_dirsvc',
					'client_class': 'DEFwDirSvc',
					'service_module': self.__class__.__module__,
					'service_class': self.__class__.__name__,
					'version': 1,
				}],
			},
		)
