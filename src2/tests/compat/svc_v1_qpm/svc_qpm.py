"""A v1 QPM, shaped like QFw's, that answers the same calls the same way.

It is written against v1 alone: v1 imports, v1 signatures, v1 exceptions,
dictionaries shaped like QFw's UTIL_QPM answers, and statevectors encoded
the way QFw's util/qpm/statevector.py encodes them. Nothing in it knows
about v2. Every answer is a function of the calls made so far, so two
instances given the same calls give the same answers, which is how the
compat test compares the one it reaches over v2 with one it calls itself.

It keeps event registrations and puts each completion to the ones that
match, one after another, dropping one whose put fails, which is what
QFw's controller does. A run whose info asks for drop_event is completed
without its event, the way a lost one is, and one that asks for
delay_event has its event put that many seconds later.
"""

import base64
import struct
import threading
import zlib

import cdefw_global
from api_events import BaseEventAPI, Event
from defw_exception import (
	DEFwError,
	DEFwExecutionError,
	DEFwNotReady,
	DEFwOutOfResources,
	DEFwReserveError,
)
from defw_util import expand_host_list

QPM_SERVICE_TYPE = 'qfw.qpm'
BINDINGS = (
	('execution', 'api_v1_qpm', 'QPMExecution'),
	('admission', 'api_v1_qpm', 'QPMAdmissionControl'),
	('admission-policy', 'api_v1_qpm', 'QPMAdmissionPolicyConfig'),
	('scheduler', 'api_v1_qpm', 'QPMSchedulerControl'),
	('telemetry', 'api_v1_qpm', 'QPMTelemetry'),
	('control', 'api_v1_qpm', 'QPMControl'),
)
RESERVATION = 7001
TRACED = 4242
initialized = False
trace_seen = None


class AdmissionPolicyError(Exception):
	"""An exception class that is neither DEFw's nor a built-in one."""


def statevector_payload(num_qubits):
	"""Amplitude k is (k, -k), encoded as QFw encodes a statevector."""
	count = 1 << num_qubits
	raw = b''.join(struct.pack('<dd', k, -k) for k in range(count))
	compressed = zlib.compress(raw)
	encoded = base64.b64encode(compressed).decode('ascii')
	return {
		'type': 'statevector',
		'encoding': 'base64+zlib',
		'dtype': 'complex128',
		'byte_order': 'little',
		'num_qubits': num_qubits,
		'num_amplitudes': count,
		'raw_size_bytes': len(raw),
		'compressed_size_bytes': len(compressed),
		'base64_size_bytes': len(encoded),
		'compression_ratio': len(compressed) / len(raw),
		'encode_time_seconds': 0.25,
		'data': encoded,
		'source': 'v1-fake',
	}


class QPM:
	def __init__(self, start=True):
		self.lock = threading.Lock()
		self.next_qtask = 1
		self.tasks = {}
		self.completions = []
		self.hosts = expand_host_list('node[1-2]') if start else []
		self.tmp_dir = cdefw_global.get_defw_tmp_dir()
		self.events = []

	# --- what the directory sees

	def query(self):
		service_id = 'qpm:v1-fake:fake-v1-4q'
		return {
			'service_id': service_id,
			'service_name': 'QPM',
			'service_type': QPM_SERVICE_TYPE,
			'api_bindings': [{
				'binding_name': name,
				'client_module': module,
				'client_class': cls,
				'service_module': 'svc_v1_qpm.svc_qpm',
				'service_class': 'QPM',
				'version': 1,
			} for name, module, cls in BINDINGS],
			'selector': {
				'name': 'fake-v1-4q',
				'resources': ['fake-v1-4q', 'V1-FAKE-4q'],
				'aliases': ['v1-fake', 'QPM'],
			},
			'properties': {
				'provider': 'v1-fake',
				'num_qubits': 4,
				'max_shots': 10000,
				'circuit_formats': ['openqasm2', 'qpy'],
				'simulator': True,
				'hardware': False,
				'service_id': service_id,
				'service_type': QPM_SERVICE_TYPE,
			},
			'capability': {'type': 2, 'caps': 2,
				       'description': 'QPM_TYPE_SIMULATOR -> '
						      'QPM_CAP_STATEVECTOR'},
			'qpm_type': 2,
			'qpm_capabilities': 2,
			'description': 'a v1 fake QPM',
		}

	# --- control

	def is_ready(self, token=None):
		return self.get_service_status(token=token)

	def get_service_status(self, token=None):
		if not initialized:
			raise DEFwNotReady('QPM has not initialized properly')
		return {
			'state': 'running',
			'initialized': True,
			'ready': True,
			'accepting_requests': True,
			'provider_ready': False,
			'active_task_count': len(self.tasks),
			'active_reservation_count': 2,
			'shutdown': None,
			'directory_registered': True,
			'token_seen': token,
		}

	# --- admission

	def reserve(self, token=None, request=None):
		if not isinstance(request, dict):
			raise DEFwExecutionError(
				'legacy service reservation is not supported by the '
				'QPM admission API')
		if request.get('workload_kind') == 'refused':
			raise AdmissionPolicyError('policy refused the workload')
		return {
			'status': 'accepted',
			'decision': 1,
			'request_id': request.get('request_id'),
			'device_id': None,
			'scope_id': request.get('scope_id'),
			'reservation_id': RESERVATION,
			'reason': 'accepted',
			'reason_code': 1,
			'credits_required': 12,
			'rate_required': 0,
			'capacity_available': 64,
			'estimated_total_ns': 123456,
			'estimated_start_ns': 0,
			'estimated_finish_ns': 123456,
			'retry_after_ns': 0,
			'message': None,
			'echo': {'token': token, 'request': request},
		}

	def renew(self, token=None, reservation_id=None, request=None):
		return {'status': 'accepted', 'reservation_id': reservation_id,
			'echo': {'token': token, 'request': request}}

	def release(self, token=None, reservation_id=None, reason=None):
		if reservation_id is None:
			raise DEFwExecutionError(
				'legacy service release is not supported by the QPM '
				'admission API')
		if reason == 99:
			raise DEFwReserveError('no such reservation')
		return {'status': 'accepted', 'reservation_id': reservation_id,
			'reason_code': reason or 0}

	def cancel(self, token=None, reservation_id=None, reason=None):
		if reason == 98:
			raise ValueError('reason 98 is not a cancel reason')
		raise DEFwOutOfResources('cancel is out of credits')

	def get_reservation(self, token=None, reservation_id=None):
		if reservation_id == TRACED:
			return {'reservation_id': TRACED, 'state': 'traced',
				'trace_seen': trace_seen}
		return {
			'reservation_id': reservation_id,
			'request_id': None,
			'device_id': 'fake-v1-4q',
			'state': 'active',
			'state_code': 2,
			'credits_reserved': 12,
			'credits_consumed': 0,
			'created_at_ns': 1000,
			'expires_at_ns': 2000,
			'token_seen': token,
		}

	# --- execution

	def _status(self, task, outcome=None, **extra):
		status = {
			'outcome': outcome or task['outcome'],
			'lifecycle_state': task['state'],
			'cid': task['cid'],
			'qtask_id': task['qtask_id'],
			'reservation_id': task['reservation_id'],
			'scheduler_task_id': None,
			'provider_handle': 'fake-{}'.format(task['qtask_id']),
			'state': task['state'],
			'telemetry': {'access_class': 'caller-owned',
				      'object': 'managed-qtask'},
		}
		status.update(extra)
		return {key: value for key, value in status.items()
			if value is not None}

	def _require(self, reservation_id):
		if reservation_id is None:
			raise DEFwExecutionError(
				'reservation_id is required for resource-affecting '
				'QPM execution')

	def _result(self, info):
		result = {'counts': {'00': 509, '11': 515}}
		num_qubits = info.get('num_qubits') or 2
		if info.get('return_statevector'):
			result['statevector'] = statevector_payload(num_qubits)
		return result

	def _record(self, task, result):
		qtask = task['qtask_id']
		return {
			'cid': task['cid'],
			'qtask_id': qtask,
			'reservation_id': task['reservation_id'],
			'result': result,
			'rc': 0,
			'creation_time': 100.0 + qtask,
			'launch_time': 100.5 + qtask,
			'resources_consumed_time': 100.25 + qtask,
			'exec_time': 101.0 + qtask,
			'completion_time': 102.0 + qtask,
			'cq_enqueue_time': 102.5 + qtask,
			'cq_dequeue_time': -1,
			'completion_ready': True,
		}

	def _submit(self, info, reservation_id, token, timeout,
		    cancel_on_timeout):
		with self.lock:
			qtask = self.next_qtask
			self.next_qtask += 1
			task = {
				'cid': 'cid-{:04d}'.format(qtask),
				'qtask_id': qtask,
				'reservation_id': reservation_id,
				'outcome': 'ACCEPTED',
				'state': 'queued',
			}
			self.tasks[task['cid']] = task
		echo = {'info': info, 'token': token, 'timeout': timeout,
			'cancel_on_timeout': cancel_on_timeout}
		return task, echo

	def async_run(self, info, reservation_id=None, token=None,
		      timeout=None, cancel_on_timeout=False):
		self._require(reservation_id)
		if info.get('num_shots') == 0:
			raise DEFwOutOfResources('no shots, no run')
		task, echo = self._submit(info, reservation_id, token, timeout,
					  cancel_on_timeout)
		with self.lock:
			task['state'] = 'completed'
			task['outcome'] = 'COMPLETED'
			record = self._record(task, self._result(info))
			self.completions.append(record)
		if info.get('delay_event'):
			threading.Timer(info['delay_event'], self._push,
					(record,)).start()
		elif not info.get('drop_event'):
			self._push(record)
		return self._status(task, outcome='ACCEPTED', echo=echo)

	def sync_run(self, info, reservation_id=None, token=None, timeout=None,
		     cancel_on_timeout=False):
		self._require(reservation_id)
		task, echo = self._submit(info, reservation_id, token, timeout,
					  cancel_on_timeout)
		task['state'] = 'completed'
		task['outcome'] = 'COMPLETED'
		return self._status(task, result=self._result(info), echo=echo)

	def _poll(self, cid, reservation_id, consume, operation):
		if reservation_id is None:
			return {
				'outcome': 'INVALID_RESERVATION',
				'lifecycle_state': 'invalid-reservation',
				'reason': 'reservation-required',
				'message': ('reservation_id is required for managed '
					    'completion polling'),
				'completion_ready': False,
				'poll_operation': operation,
			}
		with self.lock:
			for record in self.completions:
				if record['reservation_id'] != reservation_id:
					continue
				if cid is not None and record['cid'] != cid:
					continue
				if consume:
					self.completions.remove(record)
				found = dict(record)
				if consume:
					found['cq_dequeue_time'] = 200.0
				found['poll_operation'] = operation
				return found
		return {
			'outcome': 'IN_PROGRESS',
			'lifecycle_state': 'no-ready-completion',
			'reservation_id': reservation_id,
			'cid': cid,
			'reason': 'completion-not-ready',
			'completion_ready': False,
			'poll_operation': operation,
		}

	def read_cq(self, cid=None, reservation_id=None, token=None):
		return self._poll(cid, reservation_id, True, 'read_cq')

	def peek_cq(self, cid=None, reservation_id=None, token=None):
		return self._poll(cid, reservation_id, False, 'peek_cq')

	def _task(self, cid, qtask_id):
		with self.lock:
			if cid is not None:
				return self.tasks.get(cid)
			for task in self.tasks.values():
				if task['qtask_id'] == qtask_id:
					return task
		return None

	def task_status(self, cid=None, reservation_id=None, token=None,
			qtask_id=None):
		task = self._task(cid, qtask_id)
		if task is None:
			return {'outcome': 'UNKNOWN', 'lifecycle_state': 'unknown',
				'cid': cid, 'reason': None, 'message': None}
		return self._status(task)

	def cancel_task(self, cid=None, reservation_id=None, token=None,
			reason=None, qtask_id=None):
		task = self._task(cid, qtask_id)
		if task is None:
			raise DEFwError('no task {}'.format(cid or qtask_id))
		return self._status(task, outcome='CANCELLED', reason=reason)

	def delete_circuit(self, cid, reservation_id=None, token=None):
		with self.lock:
			task = self.tasks.pop(cid, None)
		return {'deleted': task is not None, 'cid': cid,
			'reservation_id': reservation_id}

	# --- what v2 does not type, called by name only in v1

	def register_event_notification(self, ep, evtype, class_id,
					token=None, reservation_id=None,
					filters=None):
		registration = {
			'class': BaseEventAPI(class_id=class_id, target=ep),
			'evtype': evtype,
			'reservation_id': reservation_id,
			'filters': dict(filters or {}),
		}
		with self.lock:
			self.events.append(registration)
			count = len(self.events)
		return {'status': 'accepted', 'class_id': class_id,
			'registration_count': count}

	@staticmethod
	def _matches(registration, record):
		if registration['reservation_id'] not in (
				None, record['reservation_id']):
			return False
		return all(record.get(key) == value
			   for key, value in registration['filters'].items())

	def _push(self, record):
		with self.lock:
			matching = [r for r in self.events
				    if self._matches(r, record)]
		stale = []
		for r in matching:
			try:
				r['class'].put(Event(r['evtype'], dict(record)))
			except Exception:  # noqa: BLE001
				stale.append(r)
		if stale:
			with self.lock:
				self.events = [r for r in self.events
					       if r not in stale]

	# --- methods v2 has no typed form for, which go as documents

	def get_device_profile(self, token=None, device_id=None):
		return {'device_id': 'fake-v1-4q', 'max_qubits': 4}

	def test(self, token=None):
		return {'test': 'passed', 'token_seen': token}

	def set_admission_policy(self, token=None, device_id=None,
				 policy=None):
		if not isinstance(policy, dict) or 'max_qubits' not in policy:
			raise ValueError('a policy names max_qubits')
		qubits = policy['max_qubits']
		if qubits > 4:
			raise DEFwReserveError(
				'no device here has {} qubits'.format(qubits))
		return {'device_id': device_id, 'policy': policy,
			'applied': True}

	# Its default is not the API class's, which only the service's own
	# default being used, as on v1, gives back.
	def get_scheduler_status(self, token=None, device_id='fake-v1-4q'):
		return {'device_id': device_id, 'paused': False,
			'queued': len(self.tasks),
			'dispatch': {'limit': None, 'ratio': 0.5}}

	def get_backend_info(self, lib=None, token=None):
		return {'lib': lib, 'calibrated': None,
			'qubits': [{'id': i, 't1': 1e-4 * (i + 1), 'ok': i != 2}
				   for i in range(4)],
			'coupling': [[0, 1], [1, 2], [2, 3]]}

	def get_device_info(self, lib=None, token=None):
		# Bytes, which v1 carried and JSON cannot.
		return {'raw': b'\x00\x01'}

	def capability_map(self, token=None):
		# A tuple, which JSON carries as a list.
		return {'gates': ('cx', 'h', 'rz')}
