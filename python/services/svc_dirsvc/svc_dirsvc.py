"""
Directory service implementation.
"""
import logging
import threading

from defw import dump_all_agents, get_agent
from defw_agent_baseapi import BaseAgentAPI
from defw_exception import (
	DEFwAgentNotFound,
	DEFwError,
)
import defw_directory


class DEFwDirSvc:
	def __init__(self):
		self.__db_lock = threading.Lock()
		self.__services_db = {}

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

	def __endpoint_record(self, ep):
		return {
			'address': ep.addr,
			'listen_port': ep.listen_port,
			'node_name': ep.name,
			'hostname': ep.hostname,
			'pid': ep.pid,
		}

	def __directory_record(self, entry, advertisement, context=None):
		if not isinstance(advertisement, dict):
			raise DEFwError(
				"Service query must return metadata dictionaries")
		ep = entry['agent'].get_ep()
		context = context if isinstance(context, dict) else {}
		service_name = (
			context.get('service_name') or
			advertisement.get('service_name'))
		if not service_name:
			raise DEFwError("Service metadata missing service_name")
		properties = dict(context.get('properties') or {})
		properties.update(advertisement.get('properties') or {})
		capability = dict(
			context.get('capability') or
			advertisement.get('capability') or {})
		qpm_type = context.get(
			'qpm_type',
			advertisement.get(
				'qpm_type', properties.get('qpm_type', -1)))
		qpm_capabilities = context.get(
			'qpm_capabilities',
			advertisement.get(
				'qpm_capabilities',
				properties.get('qpm_capabilities', -1)))
		if qpm_type != -1:
			properties.setdefault('qpm_type', qpm_type)
		if qpm_capabilities != -1:
			properties.setdefault('qpm_capabilities', qpm_capabilities)
		service_id = (
			context.get('service_id') or
			advertisement.get('service_id') or
			properties.get('service_id') or
			f"{service_name}:{ep.hostname}:{ep.name}")
		selector = (
			context.get('selector') or
			advertisement.get('selector') or
			properties.get('selector') or
			{'resources': [service_name]})
		service_type = context.get('service_type')
		if service_type is None:
			service_type = advertisement.get(
				'service_type', properties.get(
					'service_type', 'defw.service'))
		api_bindings = (
			context.get('api_bindings') or
			advertisement.get('api_bindings'))
		if not api_bindings:
			raise DEFwError("Service metadata missing api_bindings")
		return {
			'service_id': service_id,
			'service_name': service_name,
			'service_type': service_type,
			'runtime_id': ep.remote_uuid,
			'peer_handle': ep.blk_uuid,
			'endpoint': self.__endpoint_record(ep),
			'api_bindings': [dict(binding) for binding in api_bindings],
			'selector': selector,
			'properties': properties,
			'capability': capability,
			'qpm_type': qpm_type,
			'qpm_capabilities': qpm_capabilities,
		}

	def __register_directory_entries(self, agent_id, context=None):
		entry = self.__services_db.get(agent_id)
		if not entry:
			return []
		if not entry.get('info'):
			entry['info'] = entry['api'].query()
		records = []
		for advertisement in entry.get('info') or []:
			record = self.__directory_record(
				entry, advertisement, context)
			records.append(defw_directory.register_service(record))
		return records

	def __register(self, ep, context):
		agent_id = ep.get_id()
		logging.defw_service(
			f"register(name={ep.name}, id={agent_id})")
		self.__prune_db(self.__services_db)
		agent = get_agent(ep)
		if not agent:
			logging.defw_service(
				f"Unknown agent during register: name={ep.name}, "
				f"id={agent_id}, known keys={list(self.__services_db.keys())}")
			dump_all_agents()
			raise DEFwAgentNotFound(
				f"Registration from an unknown client {ep}")
		client_api = BaseAgentAPI(target=ep)
		advertisements = client_api.query()
		with self.__db_lock:
			if agent_id not in self.__services_db:
				self.__services_db[agent_id] = {
					'agent': agent,
					'api': client_api,
					'info': advertisements,
				}
			else:
				self.__services_db[agent_id]['agent'] = agent
				self.__services_db[agent_id]['api'] = client_api
				self.__services_db[agent_id]['info'] = advertisements
			if context is not None:
				self.__services_db[agent_id]['context'] = context
		logging.defw_service(
			f"Registered agent: name={ep.name}, id={agent_id}, "
			f"local keys={list(self.__services_db.keys())}")

	def register_service(self, service_ep, context=None):
		agent_id = service_ep.get_id()
		self.__register(service_ep, context)
		return self.__register_directory_entries(agent_id, context)

	def deregister(self, ep):
		agent_id = ep.get_id()
		logging.defw_service(
			f"dirsvc.deregister(name={ep.name}, id={agent_id})")
		if agent_id not in self.__services_db:
			raise DEFwAgentNotFound(
				f"agent {ep.name} ({agent_id}) not found")
		logging.defw_service(
			f"Deregistering service entry name={ep.name}, id={agent_id}")
		self.__services_db[agent_id]['api'].unregister()
		for record in defw_directory.query(include_inactive=True):
			if record['runtime_id'] == agent_id:
				defw_directory.deregister_service(
					record['service_id'],
					record['runtime_id'],
					record['generation'])
		del self.__services_db[agent_id]

	def resolve_services(self, **filters):
		return defw_directory.resolve_services(**filters)

	def deregister_service(self, service_id, runtime_id, generation):
		return defw_directory.deregister_service(
			service_id, runtime_id, generation)

	def get_service_generation(self, service_id):
		return defw_directory.get_service_generation(service_id)

	def query(self):
		from . import SERVICE_DESC, SERVICE_NAME

		return {
			'service_name': SERVICE_NAME,
			'service_type': 'defw.dirsvc',
			'api_bindings': [{
				'binding_name': 'directory',
				'client_module': 'api_dirsvc',
				'client_class': 'DEFwDirSvc',
				'service_module': self.__class__.__module__,
				'service_class': self.__class__.__name__,
				'version': 1,
			}],
			'selector': {'resources': [SERVICE_NAME]},
			'properties': {'description': SERVICE_DESC},
			'capability': {
				'type': 1,
				'caps': 1,
				'description': 'directory service',
			},
		}
