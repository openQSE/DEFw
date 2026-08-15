from cdefw_agent import *
from defw_common_def import *
from defw_exception import *
import yaml, logging, sys, ctypes, uuid
import ipaddress, traceback, time, threading

class Endpoint:
	def __init__(self, addr, port, listen_port, pid, name, hostname,
				 node_type, remote_uuid, blk_uuid=str(uuid.UUID(int=0))):
		if not (node_type == EN_DEFW_DIRSVC or \
				node_type == EN_DEFW_SERVICE or \
				node_type == EN_DEFW_AGENT):
			raise DEFwError("Unknown node type provided: ", node_type)
		self.addr = addr
		self.port = port
		self.listen_port = listen_port
		self.pid = pid
		self.name = name
		self.hostname = hostname
		self.node_type = node_type
		self.remote_uuid = remote_uuid
		self.blk_uuid = blk_uuid

	def __repr__(self):
		return yaml.dump(self.get())

	def __eq__(self, other):
		if not isinstance(other, Endpoint):
			return False
		return defw_agent_uuid_compare(self.remote_uuid, other.remote_uuid)

	def is_service(self):
		return self.node_type == EN_DEFW_SERVICE

	def is_dirsvc(self):
		return self.node_type == EN_DEFW_DIRSVC

	def get_id(self):
		return self.remote_uuid

	def get(self):
		info = {self.name: {'remote uuid': self.remote_uuid,
					'block uuid': self.blk_uuid,
					'hostname': self.hostname,
					'addr': self.addr,
					'listen port': self.listen_port,
					'connection port': self.port,
					'pid': self.pid,
					'node-type': self.node_type2str()}
				}
		return info

	def node_type2str(self):
		if self.node_type == EN_DEFW_DIRSVC:
			nt = 'DIRSVC'
		elif self.node_type == EN_DEFW_AGENT:
			nt = 'AGENT'
		elif self.node_type == EN_DEFW_SERVICE:
			nt = 'SERVICE'
		else:
			raise DEFwError("Unknown node type provided: ", self.node_type)

		return nt

	def dump(self):
		logging.defw_core(yaml.dump(self.get(), sort_keys=False))
		print(yaml.dump(self.get(), sort_keys=False))

class Agent:
	def __init__(self, endpoint):
		self.__endpoint = endpoint
		self.name = endpoint.name
		pref = load_pref()
		self.timeout = pref['RPC timeout']

	def get_ep(self):
		return self.__endpoint

	def get_remote_uuid(self):
		return self.__endpoint.remote_uuid

	def get_blk_uuid(self):
		return self.__endpoint.blk_uuid

	def is_dirsvc(self):
		return self.__endpoint.is_dirsvc()

	def dump(self):
		self.__endpoint.dump()

	def get(self):
		return self.__endpoint.get()

	def get_name(self):
		return self.name

	def get_node_type(self):
		return self.__endpoint.node_type

	def get_addr(self):
		return self.__endpoint.addr

	def get_hostname(self):
		return self.__endpoint.hostname

	def get_pid(self):
		return self.__endpoint.pid

	def get_port(self):
		return self.__endpoint.port

	def set_rpc_timeout(self, timeout):
		self.timeout = timeout

	def send_req(self, rpc_type, src, module, cname,
				 mname, class_id, blocking, *args, **kwargs):
		import defw_workers

		if not mname:
			raise DEFwError("A method or a function name need to be specified")

		start = time.time()
		rpc = populate_rpc_req(src, self.__endpoint, rpc_type, module, cname,
				       mname, class_id, *args, **kwargs)
		wr = defw_workers.WorkerRequest(defw_workers.WorkerRequest.WR_SEND_MSG,
									   remote_uuid=self.__endpoint.remote_uuid,
									   blk_uuid=self.__endpoint.blk_uuid,
									   msg=rpc,
									   blocking=blocking)
		y = defw_workers.send_req(wr)

		g_rpc_metrics.add_rpc_rsp_time(y['rpc']['statistics']['send_time'],
										 time.time())

		target = y['rpc']['dst']
		if not target == src:
			raise DEFwError("MSG intended to %s but I am %s" % (target, src))

		source = y['rpc']['src']
		if not source == self.__endpoint:
			raise DEFwError("MSG originated from %s but expected from %s" %
					 (source, self.name))

		if y['rpc']['type'] == 'failure':
			raise DEFwRemoteError('RPC failure')

		if y['rpc']['type'] == 'exception':
			if type(y['rpc']['exception']) == str:
				raise DEFwRemoteError(nname=source, msg=y['rpc']['exception'])
			else:
				raise y['rpc']['exception']

		return y['rpc']['rc']
