import threading
import time
import uuid

import cdefw_global
from cdefw_agent import EN_DEFW_RESMGR, EN_DEFW_SERVICE
from defw_agent import Agent, Endpoint


PEER_READY = 'PEER_READY'
PEER_DEGRADED = 'PEER_DEGRADED'
PEER_LOST = 'PEER_LOST'
PEER_REMOVED = 'PEER_REMOVED'
ZERO_UUID = str(uuid.UUID(int=0))


def _endpoint_value(endpoint, attr, default=None):
	if isinstance(endpoint, dict):
		return endpoint.get(attr, default)
	return getattr(endpoint, attr, default)


def _endpoint_signature(endpoint):
	if not endpoint:
		return None
	return (
		_endpoint_value(endpoint, 'address',
				_endpoint_value(endpoint, 'addr', '')),
		_endpoint_value(endpoint, 'listen_port', 0),
		_endpoint_value(endpoint, 'node_name',
				_endpoint_value(endpoint, 'name', '')),
		_endpoint_value(endpoint, 'hostname', ''),
	)


class PeerTable:
	def __init__(self):
		self.__peers = {}
		self.__pending_targets = {}
		self.__lock = threading.Lock()

	def remember_connect_target(self, endpoint):
		record = self.__endpoint_record(endpoint)
		runtime_id = _endpoint_value(endpoint, 'remote_uuid', '')
		signature = _endpoint_signature(endpoint)
		with self.__lock:
			if runtime_id:
				self.__pending_targets[('runtime', runtime_id)] = record
			if signature:
				self.__pending_targets[('endpoint', signature)] = record

	def apply_event(self, event):
		peer_handle = event.get('peer_handle')
		if not peer_handle:
			return None

		event_type = event.get('event_type')
		timestamp = event.get('timestamp', time.time())
		with self.__lock:
			current = self.__peers.get(peer_handle)
			if current and timestamp < current.get('last_seen', 0):
				return current.copy()

			if event_type == PEER_REMOVED:
				removed = self.__peers.pop(peer_handle, None)
				return removed.copy() if removed else None

			if not current and event_type not in (PEER_READY, PEER_DEGRADED):
				return None

			endpoint = dict(event.get('endpoint') or {})
			if current:
				merged_endpoint = dict(current.get('endpoint') or {})
				merged_endpoint.update({
					key: value for key, value in endpoint.items()
					if value not in (None, '')
				})
				endpoint = merged_endpoint

			runtime_id = event.get('remote_runtime_id') or \
				(current or {}).get('runtime_id', '')
			pending = self.__pending_for(runtime_id, endpoint)
			record = dict(current or {})
			record.update({
				'peer_handle': peer_handle,
				'runtime_id': runtime_id,
				'endpoint': endpoint,
				'transport_context': event.get('transport_context') or
					record.get('transport_context', ''),
				'is_self': bool(event.get('is_self', False)),
				'last_seen': timestamp,
					'node_type': event.get('node_type') or
						endpoint.get('node_type') or
						record.get('node_type') or
						pending.get('node_type') or
						self.__infer_node_type(endpoint),
				})
			if event_type == PEER_READY:
				record['callable'] = True
				record['loss_reason'] = ''
			elif event_type == PEER_DEGRADED:
				record['callable'] = True
				record['loss_reason'] = event.get('reason', '')
			elif event_type == PEER_LOST:
				record['callable'] = False
				record['loss_reason'] = event.get('reason', '')
			self.__peers[peer_handle] = record
			return record.copy()

	def get_agent(self, target):
		with self.__lock:
			record = self.__find_record(target)
			if not record:
				return None
			return self.__record_to_agent(record, target)

	def get_dirsvc_agent(self):
		with self.__lock:
			for record in self.__peers.values():
				if not record.get('callable', False):
					continue
				if record.get('node_type') == EN_DEFW_RESMGR:
					return self.__record_to_agent(record)
			return None

	def get(self, peer_handle):
		with self.__lock:
			record = self.__peers.get(peer_handle)
			return record.copy() if record else None

	def snapshot(self):
		with self.__lock:
			return {key: value.copy() for key, value in self.__peers.items()}

	def dump(self):
		for record in self.snapshot().values():
			agent = self.__record_to_agent(record)
			if agent:
				agent.dump()

	def __endpoint_record(self, endpoint):
		return {
			'address': _endpoint_value(endpoint, 'address',
						   _endpoint_value(endpoint, 'addr', '')),
			'listen_port': _endpoint_value(endpoint, 'listen_port', 0),
			'node_name': _endpoint_value(endpoint, 'node_name',
						      _endpoint_value(endpoint, 'name', '')),
			'hostname': _endpoint_value(endpoint, 'hostname', ''),
			'pid': _endpoint_value(endpoint, 'pid', 0),
			'port': _endpoint_value(endpoint, 'port', 0),
			'node_type': _endpoint_value(endpoint, 'node_type',
						     EN_DEFW_SERVICE),
			'runtime_id': _endpoint_value(endpoint, 'remote_uuid', ''),
		}

	def __pending_for(self, runtime_id, endpoint):
		pending = {}
		if runtime_id:
			pending = self.__pending_targets.get(('runtime', runtime_id), {})
		if not pending:
			signature = _endpoint_signature(endpoint)
			if signature:
				pending = self.__pending_targets.get(('endpoint', signature), {})
		return pending

	def __infer_node_type(self, endpoint):
		node_name = endpoint.get('node_name', '')
		hostname = endpoint.get('hostname', '')
		listen_port = endpoint.get('listen_port', 0)
		parent_name = cdefw_global.get_parent_name()
		parent_hostname = cdefw_global.get_parent_hostname()
		parent_port = cdefw_global.get_parent_port()
		if parent_name and parent_name != 'None' and node_name == parent_name:
			return EN_DEFW_RESMGR
		if parent_hostname and parent_hostname != 'None' and \
		   hostname == parent_hostname and listen_port == parent_port:
			return EN_DEFW_RESMGR
		return None

	def __find_record(self, target):
		peer_handle = _endpoint_value(target, 'blk_uuid', '')
		runtime_id = _endpoint_value(target, 'remote_uuid', '')
		if peer_handle and peer_handle != ZERO_UUID:
			record = self.__peers.get(peer_handle)
			if self.__record_matches(record, target):
				return record
			return None

		for record in self.__peers.values():
			if self.__record_matches(record, target):
				return record
		return None

	def __record_matches(self, record, target):
		if not record or not record.get('callable', False):
			return False
		runtime_id = _endpoint_value(target, 'remote_uuid', '')
		if runtime_id:
			return runtime_id == record.get('runtime_id')
		signature = _endpoint_signature(target)
		record_signature = _endpoint_signature(record.get('endpoint'))
		if not runtime_id and signature and record_signature != signature:
			return False
		return True

	def __record_to_agent(self, record, target=None):
		endpoint = record.get('endpoint') or {}
		node_type = record.get('node_type') or \
			_endpoint_value(target, 'node_type', EN_DEFW_SERVICE)
		runtime_id = record.get('runtime_id') or \
			_endpoint_value(target, 'remote_uuid', '')
		if not runtime_id:
			return None
		ep = Endpoint(
			endpoint.get('address') or _endpoint_value(target, 'addr', ''),
			endpoint.get('port', _endpoint_value(target, 'port', 0)),
			endpoint.get('listen_port',
				     _endpoint_value(target, 'listen_port', 0)),
			endpoint.get('pid', _endpoint_value(target, 'pid', 0)),
			endpoint.get('node_name') or _endpoint_value(target, 'name', ''),
			endpoint.get('hostname') or _endpoint_value(target, 'hostname', ''),
			node_type,
			runtime_id,
			blk_uuid=record.get('peer_handle', ZERO_UUID),
		)
		return Agent(ep)


peer_table = PeerTable()


def apply_event(event):
	return peer_table.apply_event(event)


def remember_connect_target(endpoint):
	return peer_table.remember_connect_target(endpoint)


def get_agent(target):
	return peer_table.get_agent(target)


def get_dirsvc_agent():
	return peer_table.get_dirsvc_agent()


def get_peer(peer_handle):
	return peer_table.get(peer_handle)


def snapshot():
	return peer_table.snapshot()


def dump():
	return peer_table.dump()
