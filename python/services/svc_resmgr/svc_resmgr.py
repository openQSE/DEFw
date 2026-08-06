"""
Interface module for the Resource Manager
"""
from defw_agent_info import *
from defw_agent import Endpoint
from defw import me, defw_config_yaml, dump_all_agents, get_agent
from defw_agent_baseapi import BaseAgentAPI
from defw_exception import DEFwError,DEFwCommError,DEFwAgentNotFound,\
						  DEFwInternalError,DEFwRemoteError,DEFwReserveError, \
						  DEFwInProgress
from defw_util import prformat, fg, bg
from cdefw_agent import EN_DEFW_SERVICE
import defw_directory
import logging, uuid, time, yaml, threading

# Agent states

# Agent has connected but not registered
AGENT_STATE_CONNECTED = 1 << 0
# Agent connected and registered
AGENT_STATE_REGISTERED = 1 << 1
# Agent connected has registered previously but now has unregistered
AGENT_STATE_UNREGISTERED = 1 << 2
# Agent is in error state
AGENT_STATE_ERROR = 1 << 3

class DEFwResMgr:
	SVC = 'services'
	CLT = 'clients'
	def __init__(self, sql_path):
		self.__db_lock = threading.Lock()
		self.__services_db = {}
		self.__clients_db = {}
		self.__dbs = {DEFwResMgr.SVC: self.__services_db,
					  DEFwResMgr.CLT: self.__clients_db}
		self.__my_ep = me.my_endpoint()
		self.__reload_resources(query=True)

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
			stale_ids = [agent_id for agent_id in db.keys() if agent_id not in current_ids]
			for agent_id in stale_ids:
				logging.defw_service(f"Pruning stale resource manager entry {agent_id}")
				del db[agent_id]

	def __reload_resources(self, query=True):
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
			'binding_name': service_info.get_property('binding_name', 'default'),
			'client_module': service_info.get_property('client_module',
								      client_module),
			'client_class': service_info.get_property('client_class',
								     client_class),
			'service_module': service_info.get_property('service_module',
								       service_info.get_module_name()),
			'service_class': service_info.get_property('service_class',
								      service_info.get_class_name()),
			'version': service_info.get_property('binding_version', 1),
		}

	def __service_info_properties(self, service_info):
		get_properties = getattr(service_info, 'get_properties', None)
		if not callable(get_properties):
			return {}
		properties = get_properties() or {}
		return dict(properties)

	def __service_info_capability(self, service_info):
		get_capabilities = getattr(service_info, 'get_capabilities', None)
		if not callable(get_capabilities):
			return {}, -1, -1
		capabilities = get_capabilities()
		if not capabilities:
			return {}, -1, -1
		capability = {}
		get_capability_dict = getattr(
			capabilities, 'get_capability_dict', None
		)
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
		capability, cap_type, caps = \
			self.__service_info_capability(service_info)
		service_id = context.get('service_id') or \
			service_info.get_property('service_id',
						  f"{service_name}:{ep.hostname}:{ep.name}")
		selector = context.get('selector') or \
			service_info.get_property('selector', {'resources': [service_name]})
		service_type = context.get('service_type')
		if service_type is None:
			service_type = service_info.get_property(
				'service_type', 'defw.service'
			)
		api_bindings = context.get('api_bindings') or \
			service_info.get_property('api_bindings', None)
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
			'legacy_type': cap_type,
			'legacy_capabilities': caps,
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
			db[aid]['state'] = \
				db[aid]['state'] & ~state

	def set_state(self, db, aid, state):
		with self.__db_lock:
			db[aid]['state'] = \
				db[aid]['state'] | state

	def get_state(self, db, aid):
		with self.__db_lock:
			return db[aid]['state']

	def __register(self, local_agent_dict, ep, context, query=True):
		agent_id = ep.get_id()
		logging.defw_service(
			f"__register(name={ep.name}, id={agent_id}, query={query})"
		)
		self.__reload_resources(query)
		agent = get_agent(ep)
		if not agent:
			logging.defw_service(
				f"Unknown agent during register: name={ep.name}, id={agent_id}, "
				f"known local keys={list(local_agent_dict.keys())}"
			)
			if agent_id in local_agent_dict:
				self.set_state(local_agent_dict, agent_id, AGENT_STATE_ERROR)
			dump_all_agents()
			raise DEFwAgentNotFound(
				f"Registration from an unknown client {ep}")
		try:
			client_api = BaseAgentAPI(target=ep)
		except:
			logging.defw_service(f"Agent with bad EP: {agent_id}")
			raise
		svc_info = client_api.query() if query else []
		with self.__db_lock:
			if agent_id not in local_agent_dict:
				logging.defw_service(f"Setting {agent_id} state to CONNECTED")
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
			loc_db = (DEFwResMgr.SVC if local_agent_dict is self.__services_db
				  else DEFwResMgr.CLT)
			for service_info in local_agent_dict[agent_id]['info']:
				service_info.add_key(agent_id)
				service_info.add_loc_db(loc_db)
		logging.defw_service(
			f"Registered agent: name={ep.name}, id={agent_id}, "
			f"local keys={list(local_agent_dict.keys())}"
		)
		return

	def __deregister(self, local_agent_dict, ep):
		agent = get_agent(ep)
		if not agent:
			raise DEFwAgentNotFound(
				f"Deregistration from an unknown client {ep}")
		else:
			self.unset_state(local_agent_dict, ep.get_id(), AGENT_STATE_REGISTERED)

	"""
	Register a client with the Resource Manager

	Args:
		client_ep (endpoint): Client endpoint

	Returns:
		None

	Raises:
		DEFwCommError: If Resource Manager is not reachable
	"""
	def register_agent(self, ep, context=None):
		logging.defw_service(f"Agent with ep {ep} registering. Current Agents in the system")
		dump_all_agents()
		self.__register(self.__clients_db, ep, context, query=False)
		self.__clients_db[ep.get_id()]['context'] = context
		state = self.get_state(self.__clients_db, ep.get_id())
		logging.defw_service(f"Agent with ep {ep} has registered. Now in State {state}")

	def deregister_agent(self, ep):
		logging.defw_service(f"Agent with ep {ep} deregistering")
		self.__deregister(self.__clients_db, ep)

	def ready_agents(self):
		try:
			total = int(defw_config_yaml['defw']['expected-agent-count'])
		except Exception as e:
			raise DEFwInternalError(f"Bad configuration: {yaml.dump(defw_config_yaml)}")
		registered = 0
		with self.__db_lock:
			for agent, info in self.__clients_db.items():
				logging.defw_service(f"{agent} is in state {info['state']}")
				if info['state'] & AGENT_STATE_REGISTERED:
					registered += 1
		if (total <= registered):
			return True
		raise DEFwInProgress(f"Missing clients. Expected {total}, registered {registered}")

	def wait_agents(self, timeout = 10):
		start = time.time()
		while True:
			if time.time() - start > timeout:
				raise DEFwCommError("Agents failed to connect to resource manager")
			try:
				if self.ready_agents():
					break
			except Exception as e:
				if type(e) == DEFwInProgress:
					continue
				else:
					raise e
		logging.defw_service(f"wait_agents complete: {self.__clients_db}")

	def dereg_agents(self):
		registered = 0
		with self.__db_lock:
			for agent, info in self.__clients_db.items():
				if info['state'] & AGENT_STATE_REGISTERED:
					registered += 1
		logging.defw_service(f"Agents still registered = {registered}")
		if (registered > 0):
			raise DEFwInProgress(f"Clients still registered {registered}")

	def wait_agents_deregistration(self, timeout = 10):
		start = time.time()
		while True:
			if time.time() - start > timeout:
				raise DEFwCommError("Agents failed to deregister from resource manager")
			try:
				self.dereg_agents()
				break
			except Exception as e:
				if type(e) == DEFwInProgress:
					continue
				else:
					raise e
		logging.defw_service(f"wait for agent deregistration complete: {self.__clients_db}")

	def get_agents_context(self):
		contexts = {}
		logging.defw_service(f"Currently registered: {self.__clients_db}")
		num_clients = 0
		with self.__db_lock:
			num_clients = len(self.__clients_db)
			for k, v in self.__clients_db.items():
				agent = v['agent']
				contexts[agent.get_pid()] = v['context']
		num_contexts = len(contexts)
		if num_contexts != num_clients:
			raise DEFwNotFound("Clients didn't register properly. "\
					"Found {num_contexts}. Expected {num_clients}")
		return dict(sorted(contexts.items()))

	"""
	Register a service with the Resource Manager

	Args:
		client_ep (endpoint): service endpoint

	Returns:
		agent: An agent class instance which references the service

	Raises:
		DEFwCommError: If Resource Manager is not reachable
	"""
	def register_service(self, service_ep, context=None):
		agent_id = service_ep.get_id()
		self.__register(self.__services_db, service_ep, context)
		records = self.__register_directory_entries(
			self.__services_db, agent_id, context
		)
		return records

	"""
	De-register an agent

	Args:
		agent (Agent): Agent instance to deregister

	Returns:
		None

	Raises:
		DEFwCommError: If Resource Manager is not reachable
		DEFwAgentNotFound: If agent is not registered
	"""
	def deregister(self, ep):
		agent_id = ep.get_id()
		logging.defw_service(
			f"resmgr.deregister(name={ep.name}, id={agent_id})"
		)
		logging.defw_service(
			f"resmgr.deregister clients keys={list(self.__clients_db.keys())}, "
			f"services keys={list(self.__services_db.keys())}"
		)
		if agent_id not in self.__clients_db and \
		   agent_id not in self.__services_db:
			   raise DEFwAgentNotFound(
				   f"agent {ep.name} ({agent_id}) not found"
			   )
		if agent_id in self.__services_db:
			logging.defw_service(
				f"Deregistering service entry name={ep.name}, id={agent_id} "
				f"from services db"
			)
			self.__services_db[agent_id]['api'].unregister()
			for record in defw_directory.query(include_inactive=True):
				if record['runtime_id'] == agent_id:
					defw_directory.deregister_service(
						record['service_id'],
						record['runtime_id'],
						record['generation']
					)
			del self.__services_db[agent_id]
		if agent_id in self.__clients_db:
			logging.defw_service(
				f"Deregistering client entry name={ep.name}, id={agent_id} "
				f"from clients db"
			)
			self.__clients_db[agent_id]['api'].unregister()
			del self.__clients_db[agent_id]
		return

	def get_info(self, db, svc_name, svc_type, svc_caps):
		r = []
		for k, v in db.items():
			if not v['info']:
				continue

			for i in v['info']:
				if i.is_match(svc_name, svc_type, svc_caps):
					r.append(i)
				else:
					logging.defw_service(f"No match found with ({svc_name}, {svc_type}, {svc_caps}")

		return r

	def __dedup_service_infos(self, service_infos):
		unique_infos = []
		seen = set()
		for info in service_infos:
			info_key = (
				info.get_key(),
				info.get_service_name(),
				info.get_class_name(),
				info.get_module_name(),
			)
			if info_key in seen:
				logging.defw_service(
					f"Skipping duplicate service info for key={info_key}: {info}"
				)
				continue
			seen.add(info_key)
			unique_infos.append(info)
		return unique_infos

	"""
	List all available Agents in the DEFw Network

	Args:
		service_filter: a string to filter services on

	Returns:
		dict: dictionary of services available on each agent

	Raises:
		DEFwCommError: If Resource Manager is not reachable
	"""
	def get_services(self, svc_name, svc_type=-1, svc_caps=-1):
		logging.defw_service(f"get_services({svc_name}, {svc_type}, {svc_caps})")
		return defw_directory.resolve_services(
			service_name=svc_name,
			svc_type=svc_type,
			svc_caps=svc_caps
		)

	def resolve_services(self, **filters):
		return defw_directory.resolve_services(**filters)

	def query_directory(self, include_inactive=False):
		return defw_directory.query(include_inactive=include_inactive)

	def deregister_service(self, service_id, runtime_id, generation):
		return defw_directory.deregister_service(
			service_id, runtime_id, generation
		)

	def get_service_generation(self, service_id):
		return defw_directory.get_service_generation(service_id)

	def get_generation(self, service_id):
		return defw_directory.get_generation(service_id)

	"""
	Reserve an Agent which exists on the DEFw Network

	Args:
		servics (dict): Dictionary of services to reserve

	Returns:
		endpoint list of all services reserved

	Raises:
		DEFwCommError: If Resource Manager is not reachable
		DEFwReserveError: If there is an error in the reservation process
	"""
	def reserve(self, client_ep, service_infos, *args, **kwargs):
		svc_eps = []
		for service_info in service_infos:
			if isinstance(service_info, dict):
				record = service_info['service_record']
				ep = Endpoint(record['endpoint']['address'], 0,
					      record['endpoint']['listen_port'],
					      record['endpoint']['pid'],
					      record['endpoint']['node_name'],
					      record['endpoint']['hostname'],
					      EN_DEFW_SERVICE,
					      record['runtime_id'],
					      blk_uuid=record['peer_handle'])
				svc_eps.append(ep)
				continue
			db = self.__dbs[service_info.get_loc_db()]
			with self.__db_lock:
				db_key = service_info.get_key()
				entry = db[db_key]
			if not entry['state'] & AGENT_STATE_REGISTERED:
				DEFwReserveError(f"Agent {db_key} is not registered")
			api = entry['api']
			try:
				api.reserve(service_info,  client_ep, *args, **kwargs)
			except Exception as e:
				raise DEFwReserveError(str(e))
			ep = entry['agent'].get_ep()
			# if this is a remote endpoint we should NULL out the blk_uuid
			# because it wouldn't mean anything here.
			if ep.remote_uuid != me.my_uuid():
				ep.blk_uuid = str(uuid.UUID(int=0))
			svc_eps.append(entry['agent'].get_ep())
		return svc_eps

	"""
	Release a reserved Agent

	Args:
		servics (dict): Dictionary of services to release

	Returns:
		None

	Raises:
		DEFwCommError: If Resource Manager is not reachable
		DEFwReserveError: If there is an error in the release process
	"""
	def release(self, service_infos):
		for service_info in service_infos:
			db = self.__dbs[service_info.get_loc_db()]
			with self.__db_lock:
				db_key = service_info.get_key()
				entry = db[db_key]
			if not entry['state'] & AGENT_STATE_REGISTERED:
				DEFwReserveError("Agent is not registered")
			api = entry['api']
			try:
				api.release()
			except Exception as e:
				raise DEFwReserveError(str(e))

	def query(self):
		from . import SERVICE_NAME, SERVICE_DESC
		from api_resmgr import ResMgrType, ResMgrCapability
		from defw_agent_info import get_bit_list, get_bit_desc, \
									Capability, DEFwServiceInfo
		t = get_bit_list(ResMgrType.RESMGR_TYPE_DEFW, ResMgrType)
		c = get_bit_list(ResMgrCapability.RESMGR_CAP_DEFW, ResMgrCapability)
		cap = Capability(ResMgrType.RESMGR_TYPE_DEFW,
						ResMgrCapability.RESMGR_CAP_DEFW, get_bit_desc(t, c))
		info = DEFwServiceInfo(SERVICE_NAME, SERVICE_DESC,
							   self.__class__.__name__,
							   self.__class__.__module__,
							   cap, -1)
		return info
