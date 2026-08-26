import threading, queue, time, uuid, logging, yaml, importlib, traceback, sys
import defw_common_def as common
import defw_trace
from cdefw_global import *
from defw_exception import DEFwCommError, DEFwError, DEFwInternalError, DEFwNotFound
from cdefw_agent import defw_send_req, defw_send_rsp, defw_connect_to_service, \
			defw_connect_to_client, EN_DEFW_DIRSVC
from defw_agent import Endpoint
from defw import me, preferences, service_apis
from defw_util import print_thread_stack_trace_to_logger
from defw_attachments import attach_encode, attach_load, attach_discard
import defw

from collections import deque
import time

INSTANCE_MODE_SINGLETON = 'singleton'
INSTANCE_MODE_PER_CONNECTION = 'per_connection'

PEER_EVENT_TYPES = {
	'PEER_READY',
	'PEER_DEGRADED',
	'PEER_LOST',
	'PEER_REMOVED',
}
ZERO_UUID = str(uuid.UUID(int=0))

peer_events = deque()
peer_events_lock = threading.Lock()


def is_ready_dirsvc_peer(event, peer_record):
	if event.get('event_type') != 'PEER_READY':
		return False
	if me.is_dirsvc() or getattr(defw, 'dirsvc', None):
		return False
	if not peer_record or not peer_record.get('callable', False):
		return False
	if peer_record.get('node_type') != EN_DEFW_DIRSVC:
		return False
	runtime_id = peer_record.get('runtime_id')
	return bool(runtime_id and runtime_id != ZERO_UUID)


def is_disconnected_dirsvc_peer(event):
	if event.get('event_type') not in ('PEER_LOST', 'PEER_REMOVED'):
		return False
	if me.is_dirsvc():
		return False
	return event.get('node_type') == EN_DEFW_DIRSVC


def dirsvc_peer_still_ready(peer_record):
	import defw_peers

	if not peer_record:
		return False
	current = defw_peers.get_peer(peer_record.get('peer_handle'))
	if not current or not current.get('callable', False):
		return False
	if current.get('node_type') != EN_DEFW_DIRSVC:
		return False
	if current.get('runtime_id') != peer_record.get('runtime_id'):
		return False
	return True


def _peer_endpoint(peer_record):
	endpoint = peer_record.get('endpoint') or {}
	return Endpoint(
		endpoint.get('address', ''),
		endpoint.get('port', 0),
		endpoint.get('listen_port', 0),
		endpoint.get('pid', 0),
		endpoint.get('node_name', ''),
		endpoint.get('hostname', ''),
		peer_record.get('node_type', EN_DEFW_DIRSVC),
		peer_record.get('runtime_id', ''),
		blk_uuid=peer_record.get('peer_handle', ZERO_UUID),
	)


def get_instance_mode(module):
	svc_info = getattr(module, 'svc_info', None)
	if svc_info and 'instance_mode' in svc_info:
		return svc_info['instance_mode']

	package_name = getattr(module, '__package__', None)
	if package_name and package_name != module.__name__:
		try:
			service_package = importlib.import_module(package_name)
			service_metadata = getattr(service_package, 'svc_info', None)
			if service_metadata and 'instance_mode' in service_metadata:
				return service_metadata['instance_mode']
		except Exception as exc:
			logging.defw_worker(
				f"Unable to load service package metadata for {module.__name__}: {exc}"
			)

	return INSTANCE_MODE_PER_CONNECTION


class WorkerEvent:
	EVENT_INCOMING_REQUEST = 1
	EVENT_INCOMING_RESPONSE = 2
	EVENT_CONN_COMPLETE = 3
	EVENT_SHUTDOWN = 4
	EVENT_PEER_LIFECYCLE = 5

	def __init__(self, ev_type, connect_status=EN_DEFW_RC_OK, uuid=None, msg=None,
				 peer_event=None):
		self.__check_type(ev_type)
		self.ev_type = ev_type
		self.uuid = uuid
		if ev_type == WorkerEvent.EVENT_CONN_COMPLETE:
			self.connect_status = connect_status
		elif ev_type == WorkerEvent.EVENT_PEER_LIFECYCLE:
			self.peer_event = dict(peer_event)
		else:
			self.msg_yaml = None
			if msg:
				# uuid identifies the sending agent, which the RMA
				# path needs in order to read attachments back
				self.msg_yaml = attach_load(msg, uuid)
		stack_trace_str = "".join(traceback.format_stack())
		logging.defw_stacktrace(
			f"workerEvent generated from:\n{stack_trace_str}"
		)

	def __check_type(self, we_type):
		if we_type != WorkerEvent.EVENT_INCOMING_REQUEST and \
		   we_type != WorkerEvent.EVENT_INCOMING_RESPONSE and \
		   we_type != WorkerEvent.EVENT_CONN_COMPLETE and \
		   we_type != WorkerEvent.EVENT_SHUTDOWN and \
		   we_type != WorkerEvent.EVENT_PEER_LIFECYCLE:
			   raise DEFwError(f"Bad WorkerEvent type {we_type}")

	def type2str(self, we):
		events = []
		for e in we:
			if e == WorkerEvent.EVENT_INCOMING_REQUEST:
				events.append('EVENT_INCOMING_REQUEST')
			elif e == WorkerEvent.EVENT_INCOMING_RESPONSE:
				events.append('EVENT_INCOMING_RESPONSE')
			elif e == WorkerEvent.EVENT_CONN_COMPLETE:
				events.append('EVENT_CONN_COMPLETE')
			elif e == WorkerEvent.EVENT_SHUTDOWN:
				events.append('EVENT_SHUTDOWN')
			elif e == WorkerEvent.EVENT_PEER_LIFECYCLE:
				events.append('EVENT_PEER_LIFECYCLE')
			else:
				events.append("UNKNOWN_WORKEREVENT")
		return ",".join(events)

class WorkerRequest:
	WR_SEND_MSG = 1
	WR_CONNECT = 2

	def __init__(self, wr_type, remote_uuid=None,
				 blk_uuid=None, msg=None, ep=None, blocking=True,
				 timeout=preferences['RPC timeout']):
		self.__check_type(wr_type)
		self.wr_type = wr_type
		self.req_uuid = uuid.uuid4()
		self.deadline = time.time() + timeout
		self.connect_status = -1
		self.expected_events_lock = threading.Lock()
		if wr_type == WorkerRequest.WR_SEND_MSG:
			self.remote_uuid = remote_uuid
			self.blk_uuid = blk_uuid
			self.msg = msg
			if not 'req-uuid' in self.msg['rpc']:
				self.msg['rpc']['req-uuid'] = self.req_uuid
			self.expected_events = [WorkerEvent.EVENT_INCOMING_RESPONSE]
		elif wr_type == WorkerRequest.WR_CONNECT:
			self.ep = ep
			self.expected_events = [WorkerEvent.EVENT_CONN_COMPLETE]
		else:
			raise DEFwInternalError(f"Unexpected WR type {wr_type}")
		self.blocking = blocking
		if blocking:
			self.queue = queue.Queue()
		else:
			self.queue = None
		logging.defw_worker(f"WorkRequest({self.type2str(self.wr_type)}, " \
					  f"{self.blocking}, {self.req_uuid})")
		stack_trace_str = "".join(traceback.format_stack())
		logging.defw_stacktrace(
			f"WorkRequest stack trace:\n{stack_trace_str}"
		)

	def __check_type(self, wr_type):
		if wr_type != WorkerRequest.WR_SEND_MSG and \
		   wr_type != WorkerRequest.WR_CONNECT:
			   raise DEFwError(f"Bad Request type {wr_type}")

	def type2str(self, wr_type):
		if wr_type == WorkerRequest.WR_SEND_MSG:
			return 'WR_SEND_SMG'
		if wr_type == WorkerRequest.WR_CONNECT:
			return 'WR_CONNECT'
		return 'UNKNOWN_WORKREQUEST'

	def wait(self):
		if not self.queue:
			return None
		logging.defw_worker(f"Waiting for WorkRequest({self.type2str(self.wr_type)}) " \
					  f"{self.req_uuid} to complete")

		t = time.time()
		while t < self.deadline:
			if not common.is_system_up():
				return None
			event = None
			try:
				event = self.queue.get(timeout=1)
			except queue.Empty:
				pass
			t = time.time()
			logging.defw_worker(f"cur time {str(t)}, deadline {str(self.deadline)}")
			if event:
				logging.defw_worker(f"Completed {self.type2str(self.wr_type)} " \
							  f"ev: {event.type2str([event.ev_type])} " \
							  f"WorkRequest {self.req_uuid} exp " \
							  f"{event.type2str(self.expected_events)}")
				if event.ev_type == WorkerEvent.EVENT_CONN_COMPLETE:
					with self.expected_events_lock:
						ev = self.expected_events[0]
					if ev == WorkerEvent.EVENT_CONN_COMPLETE:
						with self.expected_events_lock:
							self.expected_events.remove(ev)
							remaining = len(self.expected_events)
						self.connect_status = event.connect_status
						if remaining == 0:
							return self.connect_status
					else:
						raise DEFwCommError(f"expected CONN_COMPLETE got " \
								f"{event.type2str([event.ev_type])}")
				elif event.ev_type == WorkerEvent.EVENT_SHUTDOWN:
					return None
				elif event.ev_type == WorkerEvent.EVENT_PEER_LIFECYCLE:
					raise DEFwCommError(
						"Peer disconnected during request: "
						f"{event.peer_event.get('event_type')} "
						f"{event.peer_event.get('reason', '')}")
				else:
					return event.msg_yaml
		raise DEFwCommError('Response timed out')

	def get_uuid(self):
		return self.req_uuid

	def get_uuid_str(self):
		return str(self.req_uuid)

# Can add a req
class WorkerThread:
	def __init__(self):
		self.queue = queue.Queue()
		self.thread = threading.Thread(target=self.handle, args=())
		self.thread.daemon = True
		self.thread.start()
		self.req_db = {}
		self.req_db_lock = threading.Lock()

	def put_ev(self, we):
		self.queue.put(we)
		if we.ev_type == we.EVENT_SHUTDOWN:
			logging.defw_worker("Waiting for Worker thread to shutdown")
			self.thread.join()

	def add_work_request(self, work_request):
		with self.req_db_lock:
			self.req_db[work_request.get_uuid()] = work_request

	def _work_request_targets_peer(self, work_request, event):
		if work_request.wr_type != WorkerRequest.WR_SEND_MSG:
			return False
		remote_uuid = event.get('remote_runtime_id') or ''
		if remote_uuid and str(work_request.remote_uuid) != remote_uuid:
			return False
		peer_handle = event.get('peer_handle') or ''
		blk_uuid = str(work_request.blk_uuid or '')
		return not peer_handle or blk_uuid in ('', ZERO_UUID, peer_handle)

	def fail_pending_peer_requests(self, event):
		if event.get('event_type') not in ('PEER_LOST', 'PEER_REMOVED'):
			return
		failed = []
		with self.req_db_lock:
			for req_uuid, work_request in list(self.req_db.items()):
				if not self._work_request_targets_peer(work_request, event):
					continue
				failed.append(work_request)
				del self.req_db[req_uuid]
		for work_request in failed:
			if not work_request.queue:
				continue
			work_request.queue.put(
				WorkerEvent(
					WorkerEvent.EVENT_PEER_LIFECYCLE,
					peer_event=event))

	def clear_dirsvc_peer(self, event):
		if not is_disconnected_dirsvc_peer(event):
			return
		import defw_peers

		if defw_peers.get_dirsvc_agent():
			return
		if not getattr(defw, 'dirsvc', None):
			return
		defw.dirsvc = None
		logging.defw_worker(
			f"Cleared directory service API after {event.get('event_type')}")

	def bind_dirsvc_peer(self, peer_record):
		try:
			if me.is_dirsvc() or getattr(defw, 'dirsvc', None):
				return
			if 'Directory Service' not in service_apis:
				raise DEFwNotFound("Directory service API not loaded")
			if not dirsvc_peer_still_ready(peer_record):
				logging.defw_worker(
					"Skipping stale directory service peer binding")
				return
			dirsvc_class = service_apis[
				'Directory Service'].service_classes[0]
			dirsvc = dirsvc_class(
				target=_peer_endpoint(peer_record),
				remote_module='svc_dirsvc.svc_dirsvc',
				remote_class='DEFwDirSvc',
			)
			if not dirsvc_peer_still_ready(peer_record):
				logging.defw_worker(
					"Discarding directory service API for stale peer")
				return
			defw.dirsvc = dirsvc
			logging.defw_worker(
				f"Created directory service API: {defw.dirsvc}")
		except Exception as e:
			if common.is_system_up():
				logging.defw_worker("Couldn't bind directory service peer")
				raise e

	def spawn_temporary_worker(self, cb, *args, **kwargs):
		tmp_thread = threading.Thread(target=cb, args=args, kwargs=kwargs)
		tmp_thread.daemon = True
		tmp_thread.start()

	def handle_peer_event(self, event):
		import defw_peers
		import defw_directory

		peer_record = defw_peers.apply_event(event)
		defw_directory.apply_peer_event(event)
		with peer_events_lock:
			peer_events.append(event)
		logging.defw_worker(f"Recorded peer lifecycle event: {event}")
		self.fail_pending_peer_requests(event)
		self.clear_dirsvc_peer(event)
		if is_ready_dirsvc_peer(event, peer_record):
			logging.defw_worker(
				"Directory service peer is ready; binding dirsvc API")
			self.spawn_temporary_worker(
				self.bind_dirsvc_peer, peer_record)

	# This thread should never do any blocking calls
	def handle(self):
		shutdown = False
		while not shutdown:
			try:
				we = self.queue.get(timeout=1)
			except queue.Empty:
				continue

			logging.defw_worker(f"Received event {we.type2str([we.ev_type])}")

			if we.ev_type == WorkerEvent.EVENT_INCOMING_REQUEST:
				logging.defw_rpc(f"handling request {we.msg_yaml}")
				self.spawn_temporary_worker(self.handle_rpc_req, we.msg_yaml, we.uuid)
			elif we.ev_type == WorkerEvent.EVENT_INCOMING_RESPONSE:
				# find request
				logging.defw_rpc(f"handling response {we.msg_yaml}")
				try:
					with self.req_db_lock:
						wr = self.req_db[we.msg_yaml['rpc']['req-uuid']]
						del self.req_db[we.msg_yaml['rpc']['req-uuid']]
					wr.queue.put(we)
				except:
					logging.defw_rpc(f"Unmatched response. DB = {self.req_db}")
			elif we.ev_type == WorkerEvent.EVENT_CONN_COMPLETE:
				try:
					with self.req_db_lock:
						wr = self.req_db[we.uuid]
					logging.defw_worker(f"Queuing Event Complete on WR {we.uuid}")
					wr.queue.put(we)
				except:
					logging.defw_rpc(f"Unmatched response. DB = {self.req_db}")
			elif we.ev_type == WorkerEvent.EVENT_PEER_LIFECYCLE:
				self.handle_peer_event(we.peer_event)
			elif we.ev_type == WorkerEvent.EVENT_SHUTDOWN:
				shutdown = True
				# shutdown any waiting events
				with self.req_db_lock:
					for k, v in self.req_db.items():
						v.queue.put(we)
				logging.defw_worker("Worker thread shutdown")
			else:
				logging.defw_worker(f"Bug. Unknown event {we.ev_type}")

	def handle_rpc_req(self, y, blk_uuid):
		function_name = ''
		class_name = ''
		method_name = ''
		rc = {}

		start_rep_req_handle = time.time()

		logging.defw_rpc("Calling handle_rpc_type")

		common.g_rpc_metrics.add_rpc_req_time(y['rpc']['statistics']['send_time'],
								   time.time())

		# check to see if this is for me
		target = y['rpc']['dst']
		if not target == me.my_endpoint():
			logging.defw_rpc("Message is not for me")
			logging.defw_rpc(target)
			logging.defw_rpc(me.my_endpoint())
			return
		source = y['rpc']['src']
		hostname = source.hostname
		mname = y['rpc']['module']
		rpc_type = y['rpc']['type']
		if rpc_type == 'function_call':
			function_name = y['rpc']['function']
		elif rpc_type == 'method_call':
			class_name = y['rpc']['class']
			method_name = y['rpc']['method']
			class_id = y['rpc']['class_id']
			#TODO: If you're the directory service don't instantiate
			# a new class return the object which is already instantiated
			# on startup. This way all the state is maintained there.
			# We can add the dirsvc instance with with key class_id.
			# Don't ever delete the dirsvc.
			#
			# is this true of all services? Or are services stateless. So
			# if multiple clients connect to it, then do you want
			# a separate instance per service, or do you want one instance
			# for all clients trying to request work?
			#
		elif rpc_type == 'instantiate_class' or rpc_type == 'destroy_class':
			class_name = y['rpc']['class']
			class_id = y['rpc']['class_id']
			logging.defw_rpc(f"instantiate_class {class_name} with {class_id}")
		else:
			raise DEFwError('Unexpected rpc')

		# any remote invocation implies that module which needs to be
		# imported is in the python/icpa-be/
		logging.defw_rpc("module name is: %s " % mname)
		logging.defw_rpc("rpc type is: %s " % rpc_type)
		module = importlib.import_module(mname)
		if common.get_debug_module_reload():
			importlib.reload(module)
		logging.defw_rpc(f"module is: {module.__name__}")
		args = y['rpc']['parameters']['args']
		kwargs = y['rpc']['parameters']['kwargs']
		defw_exception_string = None
		# Scope the caller's trace context to the dispatch, so work done on
		# this side joins the caller's trace instead of starting its own. A
		# no-op when nothing is tracing, or when the peer sent no context.
		trace_token = defw_trace.attach(
			y['rpc'].get(defw_trace.CARRIER_KEY))
		try:
			if rpc_type == 'function_call':
				logging.defw_rpc(f'remote call to function {function_name}')
				module_func = getattr(module, function_name)
				if hasattr(module_func, '__call__'):
					rc = module_func(*args, **kwargs)
			elif rpc_type == 'instantiate_class':
				logging.defw_rpc(f'remote call to instantiate class {class_name}')
				if me.is_dirsvc() and class_name == 'DEFwDirSvc':
					if not common.has_class_entry(class_id):
						common.add_to_class_db(defw.dirsvc, class_id)
				else:
					if get_instance_mode(module) == INSTANCE_MODE_SINGLETON:
						my_class = getattr(module, class_name)
						# For singleton services the caller-provided class_id is
						# only an alias. The shared object identity is derived
						# from the service module and class name so independent
						# callers reuse one server-side instance.
						instance = common.get_or_create_singleton_instance(
							mname, class_name,
							lambda: my_class(*args, **kwargs)
						)
						if not common.has_class_entry(class_id):
							common.bind_singleton_alias(class_id, mname, class_name, instance)
					else:
						try:
							instance = common.get_class_from_db(class_id)
						except DEFwNotFound:
							my_class = getattr(module, class_name)
							# TODO: Instantiating a class can result in a blocking
							# call
							instance = my_class(*args, **kwargs)
							common.add_to_class_db(instance, class_id)
			elif rpc_type == 'destroy_class':
				logging.defw_rpc(f'remote call to destroy class {class_name}')
				if me.is_dirsvc() and class_name == 'DEFwDirSvc':
					common.del_entry_from_class_db(class_id)
				else:
					instance = common.get_class_from_db(class_id)
					if common.is_singleton_alias(class_id):
						common.del_entry_from_class_db(class_id)
					else:
						del(instance)
						common.del_entry_from_class_db(class_id)
			elif rpc_type == 'method_call':
				instance = common.get_class_from_db(class_id)
				if type(instance).__name__ != class_name:
					raise DEFwError(f"requested class {class_name}, "  \
								   f"but id refers to class {type(instance).__name__}")
				start = time.time()
				rc = getattr(instance, method_name)(*args, **kwargs)
				logging.defw_rpc(f'remote call to method call {class_name}.{method_name} took '\
							  f'{time.time() - start}')
		except Exception as e:
			# NOTE: I can just send the exception as is to the other end, however,
			# it won't have a backtrace. I put the back trace in the DEFwError representation
			# but other exceptions will not have a backtrace from the remote end.
			# TODO: Maybe we can toggle this behavior through some config. I can see that it
			# might be cleaner to just print the message from the remote side instead of the
			# back trace
			if issubclass(type(e), DEFwError):
				defw_exception_string = e
			else:
				exception_list = traceback.format_stack()
				exception_list = exception_list[:-2]
				exception_list.extend(traceback.format_tb(sys.exc_info()[2]))
				exception_list.extend(traceback.format_exception_only(sys.exc_info()[0],
														sys.exc_info()[1]))
				header = "Traceback (most recent call last):\n"
				stacktrace = "".join(exception_list)
				defw_exception_string = header+stacktrace
		finally:
			defw_trace.detach(trace_token)
		if defw_exception_string:
			rc_yaml = common.populate_rpc_rsp(target, source, rc, defw_exception_string)
		else:
			rc_yaml = common.populate_rpc_rsp(target, source, rc)
		rc_yaml['rpc']['req-uuid'] = y['rpc']['req-uuid']

		wr = WorkerRequest(WorkerRequest.WR_SEND_MSG,
						   remote_uuid=source.remote_uuid,
						   blk_uuid=blk_uuid, msg=rc_yaml, blocking=False)
		rc = send_rsp(wr)
		if rpc_type == 'method_call':
			common.g_rpc_metrics.add_method_time(start_rep_req_handle, time.time(),
											f'{class_name}.{method_name}')
		return rc

worker_thread = WorkerThread()

def put_shutdown():
	we = WorkerEvent(WorkerEvent.EVENT_SHUTDOWN)
	worker_thread.put_ev(we)
	from defw import updater_queue
	updater_queue.put({'type': 'shutdown'})
	# TODO need to uninitialize all active services
	logging.defw_worker("Putting Shutdown")

def put_request(msg, uuid):
	try:
		we = WorkerEvent(WorkerEvent.EVENT_INCOMING_REQUEST,
						 uuid=uuid, msg=msg)
		worker_thread.put_ev(we)
	except:
		logging.defw_rpc(f"Recieved a bad request:\n{msg}")
	logging.defw_rpc("Putting request")

def put_response(msg, uuid):
	try:
		we = WorkerEvent(WorkerEvent.EVENT_INCOMING_RESPONSE,
						 uuid=uuid, msg=msg)
		worker_thread.put_ev(we)
	except:
		logging.defw_rpc(f"Recieved a bad response:\n{msg}")
	logging.defw_rpc("Putting response")

def put_connect_complete(status, uuid_str):
	we = WorkerEvent(WorkerEvent.EVENT_CONN_COMPLETE,
					 connect_status=status, uuid=uuid.UUID(uuid_str))
	worker_thread.put_ev(we)
	logging.defw_worker("Putting connect complete")

def put_peer_event(event):
	if event.get('event_type') not in PEER_EVENT_TYPES:
		raise DEFwError(f"Bad peer event type {event.get('event_type')}")
	if 'peer_handle' not in event:
		raise DEFwError("Peer lifecycle event missing peer_handle")
	we = WorkerEvent(WorkerEvent.EVENT_PEER_LIFECYCLE, peer_event=event)
	worker_thread.put_ev(we)
	logging.defw_worker("Putting peer lifecycle event")

def get_peer_events():
	with peer_events_lock:
		return list(peer_events)

def send_rsp(wr):
	# published collects the payloads left in our memory for the peer to
	# read; if the message never goes out, nobody will ever acknowledge
	# them, so release them here rather than pinning them until shutdown
	published = []
	msg = attach_encode(wr.msg, wr.blk_uuid, published=published)
	rc = defw_send_rsp(wr.remote_uuid, wr.blk_uuid, msg)
	if rc and published:
		attach_discard(published)
	return rc

def send_req(wr):
	if wr.blocking:
		worker_thread.add_work_request(wr)

	# non-blocking send
	published = []
	msg = attach_encode(wr.msg, wr.blk_uuid, published=published)
	rc = defw_send_req(wr.remote_uuid, wr.blk_uuid, msg)

	if rc:
		if published:
			attach_discard(published)
		raise DEFwCommError(f"Sending failed with {defw_rc2str(rc)}, " \
							f"{wr.remote_uuid}, {wr.blk_uuid}")

	if wr.blocking:
		return wr.wait()

	return rc, None

def connect_to_agent(wr):
	if wr.blocking:
		worker_thread.add_work_request(wr)

	import defw_peers
	defw_peers.remember_connect_target(
		wr.ep,
		connection_direction=defw_peers.CONNECTION_OUTBOUND,
	)

	if wr.ep.is_service():
		func = defw_connect_to_service
	else:
		func = defw_connect_to_client
	# TODO: need to figure out how to pass function pointers
	rc = func(wr.ep.addr,
			  wr.ep.listen_port,
			  wr.ep.name,
			  wr.ep.hostname,
			  wr.ep.node_type,
			  wr.get_uuid_str(),
			  None)
	if rc and rc != EN_DEFW_RC_IN_PROGRESS:
		raise DEFwError("Failed to connect:", defw_rc2str(rc))

	if wr.blocking:
		return wr.wait()

	return rc, None
