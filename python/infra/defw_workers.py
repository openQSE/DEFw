import threading, queue, time, uuid, logging, yaml, importlib, traceback, sys
import defw_common_def as common
from cdefw_global import *
from defw_exception import DEFwCommError, DEFwError, DEFwInternalError, DEFwNotFound
from cdefw_agent import defw_send_req, defw_send_rsp, defw_connect_to_service, \
			defw_connect_to_client
from defw import client_agents, service_agents, \
				active_client_agents, active_service_agents, \
				me, preferences, service_apis
from defw_util import print_thread_stack_trace_to_logger
import defw

from collections import deque
import time

INSTANCE_MODE_SINGLETON = 'singleton'
INSTANCE_MODE_PER_CONNECTION = 'per_connection'


def get_instance_mode(module):
	svc_info = getattr(module, 'svc_info', {})
	return svc_info.get('instance_mode', INSTANCE_MODE_PER_CONNECTION)


class WorkerEvent:
	EVENT_INCOMING_REQUEST = 1
	EVENT_INCOMING_RESPONSE = 2
	EVENT_CONN_COMPLETE = 3
	EVENT_REFRESH = 4
	EVENT_REFRESH_COMPLETE = 5
	EVENT_SHUTDOWN = 6

	def __init__(self, ev_type, connect_status=EN_DEFW_RC_OK, uuid=None, msg=None):
		self.__check_type(ev_type)
		self.ev_type = ev_type
		self.uuid = uuid
		if ev_type == WorkerEvent.EVENT_CONN_COMPLETE:
			self.connect_status = connect_status
		else:
			self.msg_yaml = None
			if msg:
				self.msg_yaml = yaml.load(msg, Loader=yaml.Loader)
		logging.debug("workerEvent generated from: ")
		stack_trace_str = "".join(traceback.format_stack())
		logging.debug(f"{stack_trace_str}")

	def __check_type(self, we_type):
		if we_type != WorkerEvent.EVENT_INCOMING_REQUEST and \
		   we_type != WorkerEvent.EVENT_INCOMING_RESPONSE and \
		   we_type != WorkerEvent.EVENT_CONN_COMPLETE and \
		   we_type != WorkerEvent.EVENT_REFRESH and \
		   we_type != WorkerEvent.EVENT_REFRESH_COMPLETE and \
		   we_type != WorkerEvent.EVENT_SHUTDOWN:
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
			elif e == WorkerEvent.EVENT_REFRESH:
				events.append('EVENT_REFRESH')
			elif e == WorkerEvent.EVENT_REFRESH_COMPLETE:
				events.append('EVENT_REFRESH_COMPLETE')
			elif e == WorkerEvent.EVENT_SHUTDOWN:
				events.append('EVENT_SHUTDOWN')
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
			self.expected_events = [WorkerEvent.EVENT_CONN_COMPLETE,
									WorkerEvent.EVENT_REFRESH]
		else:
			raise DEFwInternalError(f"Unexpected WR type {wr_type}")
		self.blocking = blocking
		if blocking:
			self.queue = queue.Queue()
		else:
			self.queue = None
		logging.debug(f"WorkRequest({self.type2str(self.wr_type)}, " \
					  f"{self.blocking}, {self.req_uuid})")
		stack_trace_str = "".join(traceback.format_stack())
		logging.debug(f"{stack_trace_str}")

	def __check_type(self, wr_type):
		if wr_type != WorkerRequest.WR_SEND_MSG and \
		   wr_type != WorkerRequest.WR_CONNECT:
			   raise DEFwError(f"Bad Request type {wr_type}")

	def type2str(self, wr_type):
		if WorkerRequest.WR_SEND_MSG:
			return 'WR_SEND_SMG'
		if WorkerRequest.WR_CONNECT:
			return 'WR_CONNECT'
		return 'UNKNOWN_WORKREQUEST'

	def wait(self):
		if not self.queue:
			return None
		logging.debug(f"Waiting for WorkRequest({self.type2str(self.wr_type)}) " \
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
			logging.debug(f"cur time {str(t)}, deadline {str(self.deadline)}")
			if event:
				logging.debug(f"Completed {self.type2str(self.wr_type)} " \
							  f"ev: {event.type2str([event.ev_type])} " \
							  f"WorkRequest {self.req_uuid} exp " \
							  f"{event.type2str(self.expected_events)}")
				if event.ev_type == WorkerEvent.EVENT_CONN_COMPLETE:
					with self.expected_events_lock:
						ev = self.expected_events[0]
					if ev == WorkerEvent.EVENT_CONN_COMPLETE:
						with self.expected_events_lock:
							self.expected_events.remove(ev)
						if len(self.expected_events) > 0:
							self.connect_status = event.connect_status
						else:
							raise DEFwCommError("Expected to wait for a REFRESH COMPLETE")
					else:
						raise DEFwCommError(f"expected REFRESH_COMPLETE got " \
								f"{event.type2str([event.ev_type])}")
				elif event.ev_type == WorkerEvent.EVENT_REFRESH_COMPLETE:
					with self.expected_events_lock:
						if len(self.expected_events) > 0:
							raise DEFwCommError(f"Unexpected pending Events: {self.expected_events}")
					return self.connect_status
				elif event.ev_type == WorkerEvent.EVENT_SHUTDOWN:
					return None
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
			logging.debug("Waiting for Worker thread to shutdown")
			self.thread.join()

	def add_work_request(self, work_request):
		with self.req_db_lock:
			self.req_db[work_request.get_uuid()] = work_request

	def refresh_agents(self, *args, **kwargs):
		try:
			client_agents.reload()
			service_agents.reload()
			active_client_agents.reload()
			active_service_agents.reload()

			# TODO: If the resource manager dies and comes up again, we'll
			# still use the old resource manager. So we need a better way
			# to notify python that the resource manager is dead and we
			# need to recreate the API
			#if not me.is_resmgr() and not defw.resmgr:
			if not me.is_resmgr():
				if 'Resource Manager' in service_apis:
					from defw_agent_baseapi import query_service_info
					si = query_service_info(active_service_agents.get_resmgr(),
							 'DEFwResMgr')
					logging.debug(f"Querying Resource Manager returned: {si}")
					if si:
						defw.resmgr = service_apis['Resource Manager'].service_classes[0](si)
						logging.debug(f"Created resource manager API: {defw.resmgr}")
						from defw import updater_queue
						updater_queue.put({'type': 'resmgr', 'resmgr': defw.resmgr})
					else:
						raise DEFwNotFound("Couldn't Query resource manager")
		except Exception as e:
			logging.debug("Calling system up")
			if common.is_system_up():
				logging.critical("Couldn't refresh agents")
				raise e
			pass
		logging.debug("Feeding worker thread EVENT_REFRESH_COMPLETE")
		we = WorkerEvent(WorkerEvent.EVENT_REFRESH_COMPLETE)
		worker_thread.put_ev(we)

	def spawn_temporary_worker(self, cb, *args, **kwargs):
		tmp_thread = threading.Thread(target=cb, args=args, kwargs=kwargs)
		tmp_thread.daemon = True
		tmp_thread.start()

	# This thread should never do any blocking calls
	def handle(self):
		shutdown = False
		while not shutdown:
			try:
				we = self.queue.get(timeout=1)
			except queue.Empty:
				continue

			logging.debug(f"Received event {we.type2str([we.ev_type])}")

			if we.ev_type == WorkerEvent.EVENT_INCOMING_REQUEST:
				logging.debug(f"handling request {we.msg_yaml}")
				self.spawn_temporary_worker(self.handle_rpc_req, we.msg_yaml, we.uuid)
			elif we.ev_type == WorkerEvent.EVENT_INCOMING_RESPONSE:
				# find request
				logging.debug(f"handling response {we.msg_yaml}")
				try:
					with self.req_db_lock:
						wr = self.req_db[we.msg_yaml['rpc']['req-uuid']]
						del self.req_db[we.msg_yaml['rpc']['req-uuid']]
					wr.queue.put(we)
				except:
					logging.critical(f"Unmatched response. DB = {self.req_db}")
			elif we.ev_type == WorkerEvent.EVENT_REFRESH:
				logging.debug("Refreshing Agents")
				self.spawn_temporary_worker(self.refresh_agents)
			elif we.ev_type == WorkerEvent.EVENT_REFRESH_COMPLETE:
				del_entries = []
				with self.req_db_lock:
					for k, v in self.req_db.items():
						logging.debug(f"Got a refresh event. looking at {k}:{v}")
						# satisfy the event in order to avoid out of order
						# refresh events which get misinterpreted
						# TODO: Is there a bug here?
						with v.expected_events_lock:
							ev = v.expected_events[0]
						if WorkerEvent.EVENT_REFRESH == ev:
							v.queue.put(we)
							with v.expected_events_lock:
								v.expected_events.remove(ev)
								if len(v.expected_events) == 0:
									del_entries.append(k)
						elif WorkerEvent.EVENT_REFRESH in v.expected_events:
							raise DEFwCommError(f"Unordered events {v.expected_events}")
					logging.debug(f"deleting entries from req_db {del_entries}")
					for k in del_entries:
						del self.req_db[k]
				logging.debug("Finished handling refresh")
			elif we.ev_type == WorkerEvent.EVENT_CONN_COMPLETE:
				try:
					with self.req_db_lock:
						wr = self.req_db[we.uuid]
					logging.debug(f"Queuing Event Complete on WR {we.uuid}")
					wr.queue.put(we)
				except:
					logging.critical(f"Unmatched response. DB = {self.req_db}")
			elif we.ev_type == WorkerEvent.EVENT_SHUTDOWN:
				shutdown = True
				# shutdown any waiting events
				with self.req_db_lock:
					for k, v in self.req_db.items():
						v.queue.put(we)
				logging.debug("Worker thread shutdown")
			else:
				logging.critical(f"Bug. Unknown event {we.ev_type}")

	def handle_rpc_req(self, y, blk_uuid):
		function_name = ''
		class_name = ''
		method_name = ''
		rc = {}

		start_rep_req_handle = time.time()

		logging.debug("Calling handle_rpc_type")

		common.g_rpc_metrics.add_rpc_req_time(y['rpc']['statistics']['send_time'],
								   time.time())

		# check to see if this is for me
		target = y['rpc']['dst']
		if not target == me.my_endpoint():
			logging.debug("Message is not for me")
			logging.debug(target)
			logging.debug(me.my_endpoint())
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
			#TODO: If you're the resource manager don't instantiate
			# a new class return the object which is already instantiated
			# on startup. This way all the state is maintained there.
			# We can add the resmgr instance with with key class_id.
			# Don't ever delete the resmgr.
			#
			# is this true of all services? Or are services stateless. So
			# if multiple clients connect to it, then do you want
			# a separate instance per service, or do you want one instance
			# for all clients trying to request work?
			#
		elif rpc_type == 'instantiate_class' or rpc_type == 'destroy_class':
			class_name = y['rpc']['class']
			class_id = y['rpc']['class_id']
			logging.debug(f"instantiate_class {class_name} with {class_id}")
		else:
			raise DEFwError('Unexpected rpc')

		# any remote invocation implies that module which needs to be
		# imported is in the python/icpa-be/
		logging.debug("module name is: %s " % mname)
		logging.debug("rpc type is: %s " % rpc_type)
		module = importlib.import_module(mname)
		if common.get_debug_module_reload():
			importlib.reload(module)
		logging.debug(f"module is: {module.__name__}")
		args = y['rpc']['parameters']['args']
		kwargs = y['rpc']['parameters']['kwargs']
		defw_exception_string = None
		try:
			if rpc_type == 'function_call':
				logging.debug(f'remote call to function {function_name}')
				module_func = getattr(module, function_name)
				if hasattr(module_func, '__call__'):
					rc = module_func(*args, **kwargs)
			elif rpc_type == 'instantiate_class':
				logging.debug(f'remote call to instantiate class {class_name}')
				if me.is_resmgr() and class_name == 'DEFwResMgr':
					if not common.has_class_entry(class_id):
						common.add_to_class_db(defw.resmgr, class_id)
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
				logging.debug(f'remote call to destroy class {class_name}')
				if me.is_resmgr() and class_name == 'DEFwResMgr':
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
				logging.debug(f'remote call to method call {class_name}.{method_name} took '\
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
	logging.debug("Putting Shutdown")

def put_request(msg, uuid):
	try:
		we = WorkerEvent(WorkerEvent.EVENT_INCOMING_REQUEST,
						 uuid=uuid, msg=msg)
		worker_thread.put_ev(we)
	except:
		logging.critical(f"Recieved a bad request:\n{msg}")
	logging.debug("Putting request")

def put_response(msg, uuid):
	try:
		we = WorkerEvent(WorkerEvent.EVENT_INCOMING_RESPONSE,
						 uuid=uuid, msg=msg)
		worker_thread.put_ev(we)
	except:
		logging.critical(f"Recieved a bad response:\n{msg}")
	logging.debug("Putting response")

def put_refresh():
	we = WorkerEvent(WorkerEvent.EVENT_REFRESH)
	worker_thread.put_ev(we)
	logging.debug("Putting refresh")

def put_connect_complete(status, uuid_str):
	we = WorkerEvent(WorkerEvent.EVENT_CONN_COMPLETE,
					 connect_status=status, uuid=uuid.UUID(uuid_str))
	worker_thread.put_ev(we)
	logging.debug("Putting connect complete")

def send_rsp(wr):
	rc = defw_send_rsp(wr.remote_uuid,
					  wr.blk_uuid,
					  yaml.dump(wr.msg))
	return rc

def send_req(wr):
	if wr.blocking:
		worker_thread.add_work_request(wr)

	# non-blocking send
	rc = defw_send_req(wr.remote_uuid,
					  wr.blk_uuid,
					  yaml.dump(wr.msg))

	if rc:
		raise DEFwCommError(f"Sending failed with {defw_rc2str(rc)}, " \
							f"{wr.remote_uuid}, {wr.blk_uuid}")

	if wr.blocking:
		return wr.wait()

	return rc, None

def connect_to_agent(wr):
	if wr.blocking:
		worker_thread.add_work_request(wr)

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
