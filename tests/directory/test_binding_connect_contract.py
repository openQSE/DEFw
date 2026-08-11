#!/usr/bin/env python3

import sys
import types
import uuid

import defw
import defw_remote
import defw_workers
from api_dirsvc import DEFwDirSvc
from defw_exception import DEFwReserveError


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


class BindingClient:
	def __init__(self, target=None, remote_module=None, remote_class=None):
		self.target = target
		self.remote_module = remote_module
		self.remote_class = remote_class


class StubRemoteAgent:
	def __init__(self):
		self.requests = []

	def send_req(self, *args, **kwargs):
		self.requests.append((args, kwargs))
		return None


class StubEndpoint:
	def __init__(self, runtime_id, peer_handle):
		self.addr = '127.0.0.1'
		self.listen_port = 8095
		self.name = 'qpm-iqm'
		self.hostname = 'qpm.example'
		self.pid = 12345
		self.remote_uuid = runtime_id
		self.blk_uuid = peer_handle

	def get_id(self):
		return self.remote_uuid


class StubAgent:
	def __init__(self, endpoint):
		self.__endpoint = endpoint

	def get_ep(self):
		return self.__endpoint


class StubMe:
	def __init__(self, endpoint):
		self.__endpoint = endpoint

	def my_endpoint(self):
		return self.__endpoint


class StubServiceAPI:
	def __init__(self, service_infos):
		self.__service_infos = service_infos

	def query(self):
		return list(self.__service_infos)

	def unregister(self):
		pass


class StubCapability:
	def __init__(self, cap_type, caps):
		self.__cap_type = cap_type
		self.__caps = caps

	def get_capability_dict(self):
		return {
			'type': self.__cap_type,
			'caps': self.__caps,
			'description': 'stub capability',
		}

	def get_cap_type(self):
		return self.__cap_type

	def get_caps(self):
		return self.__caps


class StubServiceInfo:
	def __init__(self, service_name, cap_type=1, caps=1, properties=None):
		self.__service_name = service_name
		self.__capability = StubCapability(cap_type, caps)
		self.__properties = properties or {}
		self.key = None
		self.loc_db = None

	def get_service_name(self):
		return self.__service_name

	def add_key(self, key):
		self.key = key

	def add_loc_db(self, loc_db):
		self.loc_db = loc_db

	def get_property(self, key, default=None):
		return self.__properties.get(key, default)

	def get_properties(self):
		return self.__properties

	def get_capabilities(self):
		return self.__capability

	def get_class_name(self):
		return self.__properties.get('service_class', 'QPM')

	def get_module_name(self):
		return self.__properties.get('service_module', 'svc_qpm')


def make_resolved_binding(client_module, client_class, service_module,
			  service_class):
	return {
		'service_record': {
			'service_id': 'qpm-iqm-ornl',
			'runtime_id': str(uuid.uuid4()),
			'peer_handle': 'directory-peer-handle',
			'endpoint': {
				'address': '127.0.0.1',
				'listen_port': 8095,
				'node_name': 'qpm-iqm',
				'hostname': 'qpm.example',
				'pid': 12345,
			},
		},
		'selected_binding': {
			'client_module': client_module,
			'client_class': client_class,
			'service_module': service_module,
			'service_class': service_class,
		},
	}


def exercise_active_service_registration():
	import defw_directory
	import svc_dirsvc.svc_dirsvc as svc_dirsvc

	runtime_id = str(uuid.uuid4())
	peer_handle = str(uuid.uuid4())
	other_runtime_id = str(uuid.uuid4())
	other_peer_handle = str(uuid.uuid4())
	service_ep = StubEndpoint(runtime_id, peer_handle)
	other_service_ep = StubEndpoint(other_runtime_id, other_peer_handle)
	self_ep = StubEndpoint(str(uuid.uuid4()), str(uuid.uuid4()))
	service_info = StubServiceInfo('QPM', cap_type=0b0011, caps=0b0100,
				       properties={
					       'backend': 'iqm',
					       'service_type': 'qfw.qpm',
					       'qpm_type': 0b0011,
					       'qpm_capabilities': 0b0100,
				       })
	other_service_info = StubServiceInfo('QPM', cap_type=0b1000, caps=0b0010,
					     properties={
						     'backend': 'sim',
						     'service_type': 'qfw.qpm',
						     'qpm_type': 0b1000,
						     'qpm_capabilities': 0b0010,
					     })
	service_infos = {
		runtime_id: service_info,
		other_runtime_id: other_service_info,
	}
	originals = {
		'me': svc_dirsvc.me,
		'get_agent': svc_dirsvc.get_agent,
		'BaseAgentAPI': svc_dirsvc.BaseAgentAPI,
		'directory': defw_directory.directory,
	}
	try:
		defw_directory.directory = defw_directory.Directory()
		svc_dirsvc.me = StubMe(self_ep)
		svc_dirsvc.get_agent = lambda ep: (
			StubAgent(ep) if ep.get_id() in service_infos else None
		)
		svc_dirsvc.BaseAgentAPI = \
			lambda target: StubServiceAPI([
				service_infos[target.remote_uuid]
			])

		directory = svc_dirsvc.DEFwDirSvc('/tmp')
		records = directory.register_service(service_ep, context={
			'service_id': 'qpm-iqm-ornl',
			'service_type': 'qfw.qpm',
			'api_bindings': [
				{
					'binding_name': 'execution',
					'client_module': 'api_dirsvc',
					'client_class': 'DEFwDirSvc',
					'service_module': 'svc_dirsvc.svc_dirsvc',
					'service_class': 'DEFwDirSvc',
					'version': 1,
				},
			],
			'selector': {'resources': ['IQM-20q']},
		})
		expect(len(records) == 1,
		       "active service startup did not register one directory record")
		record = records[0]
		expect(record['runtime_id'] == runtime_id,
		       "registered service did not carry runtime identity")
		expect(record['peer_handle'] == peer_handle,
		       "registered service did not carry peer handle")
		expect(record['properties']['backend'] == 'iqm',
		       "registered service did not preserve properties")
		expect(record['qpm_type'] == 0b0011,
		       "registered service did not preserve QPM type")
		expect(record['qpm_capabilities'] == 0b0100,
		       "registered service did not preserve QPM capabilities")
		directory.register_service(other_service_ep, context={
			'service_id': 'qpm-sim-ornl',
			'service_type': 'qfw.qpm',
			'api_bindings': [
				{
					'binding_name': 'execution',
					'client_module': 'api_dirsvc',
					'client_class': 'DEFwDirSvc',
					'service_module': 'svc_dirsvc.svc_dirsvc',
					'service_class': 'DEFwDirSvc',
					'version': 1,
				},
			],
			'selector': {'resources': ['simulator']},
		})
		matches = defw_directory.resolve_services(
			service_type='qfw.qpm',
			selector_resource='IQM-20q',
			binding_name='execution',
		)
		expect(len(matches) == 1,
		       "normal discovery did not return active startup record")
		filtered_matches = directory.resolve_services(
			service_name='QPM',
			qpm_type=0b0001,
			qpm_capabilities=0b0100,
		)
		expect(len(filtered_matches) == 1,
		       f"type/capability filter returned {filtered_matches!r}")
		expect(filtered_matches[0]['service_record']['properties']['backend']
		       == 'iqm',
		       "type/capability filter selected wrong QPM")
	finally:
		defw_directory.directory = originals['directory']
		svc_dirsvc.me = originals['me']
		svc_dirsvc.get_agent = originals['get_agent']
		svc_dirsvc.BaseAgentAPI = originals['BaseAgentAPI']


def main():
	connected = []
	original_connect_to_agent = defw_workers.connect_to_agent
	original_wait_for_bound_agent = defw._wait_for_bound_agent
	defw_workers.connect_to_agent = lambda wr: connected.append(wr.ep)
	defw._wait_for_bound_agent = lambda ep, timeout=5: StubAgent(ep)
	try:
		module = types.ModuleType("binding_client_fixture")
		module.BindingClient = BindingClient
		sys.modules[module.__name__] = module

		resolved_binding = make_resolved_binding(
			module.__name__, 'BindingClient', 'svc_iqm_qpm.svc_qpm', 'QPM'
		)

		api = defw.connect_to_binding(resolved_binding)
		zero_uuid = str(uuid.UUID(int=0))

		expect(len(connected) == 1,
		       "binding connect did not request one transport connection")
		expect(connected[0].blk_uuid == zero_uuid,
		       "binding connect reused the directory peer handle")
		expect(api.target.blk_uuid == zero_uuid,
		       "binding proxy target reused the directory peer handle")
		expect(api.target.remote_uuid ==
		       resolved_binding['service_record']['runtime_id'],
		       "binding proxy target did not keep runtime identity")
		expect(api.remote_module == 'svc_iqm_qpm.svc_qpm',
		       "binding proxy did not receive remote module override")
		expect(api.remote_class == 'QPM',
		       "binding proxy did not receive remote class override")

		remote_agent = StubRemoteAgent()
		original_defw_get_agent = defw.get_agent
		original_remote_get_agent = defw_remote.get_agent
		defw.get_agent = lambda target: remote_agent
		defw_remote.get_agent = lambda target: remote_agent
		dirsvc_api = None
		try:
			dirsvc_binding = make_resolved_binding(
				'api_dirsvc', 'DEFwDirSvc',
				'svc_dirsvc.svc_dirsvc', 'DEFwDirSvc'
			)
			dirsvc_api = defw.connect_to_binding(dirsvc_binding)
		finally:
			defw.get_agent = original_defw_get_agent
			defw_remote.get_agent = original_remote_get_agent
	finally:
		defw_workers.connect_to_agent = original_connect_to_agent
		defw._wait_for_bound_agent = original_wait_for_bound_agent
	expect(dirsvc_api is not None,
	       "real dirsvc proxy was not returned")
	expect(len(remote_agent.requests) >= 1,
	       "real dirsvc proxy did not instantiate a remote class")
	args, _ = remote_agent.requests[0]
	expect(args[0] == 'instantiate_class',
	       "real dirsvc proxy sent the wrong RPC")
	expect(args[2] == 'svc_dirsvc.svc_dirsvc',
	       "real dirsvc proxy did not use selected service module")
	expect(args[3] == 'DEFwDirSvc',
	       "real dirsvc proxy did not use selected service class")

	exercise_active_service_registration()


if __name__ == "__main__":
	main()
