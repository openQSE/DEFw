#!/usr/bin/env python3
"""The fake QPM of defw2_qpm_smoke.c, in Python.

It answers every method exactly as the C fake does, down to the
fingerprints of what each request carried, so the C checks in
defw2_qpm_smoke --remote and the Python checks in defw2_qpm_client_check.py
pass against either one. That is the cross-language claim made concrete: a
caller cannot tell which language the service is written in.

	defw2_qpm_fake.py [--slow-ms N] [--execution-workers N] [--register]

It prints its address and nothing else on stdout, serves until its stdin
closes, and with --register keeps itself in the directory DEFW2_DIRSVC
names. --slow-ms makes async_run take that long, which is how the
concurrency test builds an execution backlog.

It keeps registrations for completion events as the C fake does, and
publishes the same event for each completion it queues, so a sink in either
language cannot tell which fake sent it.
"""

import argparse
import sys
import threading
import time

import defw2

try:
	import numpy
except ImportError:
	numpy = None

BIG_EXTRA = 200 * 1024
QUEUED = 64
REGISTRATIONS = 8
STR_MAX = 64 * 1024


def s_or(value):
	return '(null)' if value is None else value


def fnv1a(data):
	h = 1469598103934665603
	for byte in data:
		h ^= byte
		h = (h * 1099511628211) & 0xffffffffffffffff
	return h


# --- fingerprints, the same text the C fake builds -------------------


def fp_ctx(r):
	return 'rid={} token={}'.format(r.reservation_id, s_or(r.token))


def fp_reserve(r):
	tc = r.task_class or {}
	return ('rid={} token={} req={} user={} job={} alloc={} dev={} '
		'scope={} kind={} nq={} wall={} ttl={} tc={}/{}/{}/{}/{}/{}/{}/{} '
		'extra={}').format(
		r.reservation_id, s_or(r.token), r.request_id, s_or(r.user),
		s_or(r.job_id), s_or(r.allocation_id), s_or(r.target_device_id),
		s_or(r.scope_id), s_or(r.workload_kind), r.num_qubits,
		r.walltime_ns, r.ttl_ns, int(r.task_class is not None),
		tc.get('count', 0), tc.get('qubit_count', 0), tc.get('depth', 0),
		tc.get('one_q_gate_count', 0), tc.get('two_q_gate_count', 0),
		tc.get('shots', 0), tc.get('measurement_count', 0),
		s_or(r.extra_json))


def fp_run(r):
	timed = r.run_timeout_ms is not None
	return ('rid={} token={} fmt={} len={} sum={:016x} nq={} shots={} '
		'comp={} sv={} to={}/{} cot={} extra={}').format(
		r.reservation_id, s_or(r.token), s_or(r.circuit_format),
		len(r.circuit), fnv1a(r.circuit), r.num_qubits, r.num_shots,
		s_or(r.compiler), int(r.return_statevector), int(timed),
		r.run_timeout_ms if timed else 0, int(r.cancel_on_timeout),
		s_or(r.extra_json))


def fp_task(r):
	return 'rid={} token={} cid={} qtask={} reason={}'.format(
		r.reservation_id, s_or(r.token), s_or(r.cid), r.qtask_id,
		s_or(r.reason))


def fp_notify(r):
	return ('rid={} token={} addr={} provider={} tag={} type={} '
		'extra={}').format(
		r.reservation_id, s_or(r.token), s_or(r.target.address),
		r.target.provider_id, s_or(r.target.tag), s_or(r.type),
		s_or(r.extra_json))


def statevector(count):
	"""Amplitude k is (k, -k), the C fake's pattern."""
	if numpy is not None:
		return numpy.arange(count, dtype=numpy.float64) * (1 - 1j)
	import struct
	return b''.join(struct.pack('<dd', k, -k) for k in range(count))


def give_statevector(answer, num_qubits, capacity, lie=False):
	"""The statevector, or only its description when it will not fit."""
	count = 1 << num_qubits
	if count * 16 > capacity:
		answer['statevector_shape'] = (count,)
		answer['statevector_dtype'] = 'c128'
		return answer
	answer['statevector'] = statevector(count)
	if lie:
		answer['statevector_shape'] = (count // 2,)
	return answer


class FakeQPM:
	"""Every QPM method but renew, which it leaves unserved on purpose."""

	def __init__(self, slow_ms=0, publisher=None):
		self.slow = slow_ms / 1000.0
		self.lock = threading.Lock()
		self.done = []
		self.next_qtask = 1
		self.big_extra = 'e' * (BIG_EXTRA - 1)
		self.publisher = publisher
		self.registrations = []

	# --- documents, the control API's, answered as the C fake answers

	def document(self, api, method, request, traceparent):
		if api == defw2.API_QPM_CONTROL and method == 'describe':
			return {'api': api, 'method': method,
				'request': request}
		if api == defw2.API_QPM_CONTROL and method == 'refuse':
			raise defw2.ServiceError('invalid-argument',
						 'the fake refused')
		raise defw2.ServiceError('not-found',
					 'the fake has no such document')

	# --- control

	def is_ready(self, r):
		return {'state': 'running', 'ready': True, 'initialized': True,
			'accepting_requests': True, 'provider_ready': False,
			'active_task_count': 3, 'active_reservation_count': 2,
			'extra': fp_ctx(r)}

	def get_service_status(self, r):
		return {'state': 'running', 'extra': self.big_extra}

	# --- admission

	def reserve(self, r):
		return {'decision': 'accepted', 'reservation_id': 7001,
			'request_id': r.request_id, 'reason': 'accepted',
			'reason_code': 1, 'retry_after_ns': 42,
			'extra': fp_reserve(r)}

	def release(self, r):
		if r.reason_code == 99:
			raise RuntimeError('a failure with no status of its own')
		return {}

	def cancel(self, r):
		raise defw2.ServiceError(
			'invalid-reservation',
			'no reservation named' if r.reservation_id == 0
			else 'no such reservation')

	def get_reservation(self, r):
		if r.reservation_id == 666:
			return {'reservation_id': 666, 'state': 'x' * STR_MAX}
		return {'reservation_id': r.reservation_id, 'state': 'active',
			'created_at_ns': 1000, 'expires_at_ns': 2000,
			'extra': fp_ctx(r)}

	# --- execution

	def async_run(self, r):
		if self.slow:
			time.sleep(self.slow)
		with self.lock:
			queued = [c for c in self.done if c['queued']]
			if len(queued) >= QUEUED:
				raise defw2.ServiceError(
					'pending-capacity',
					"the fake's queue is full")
			qtask = self.next_qtask
			self.next_qtask += 1
			self.done.append({
				'cid': 'cid-{}'.format(qtask), 'qtask_id': qtask,
				'num_qubits': r.num_qubits,
				'statevector': r.return_statevector,
				'queued': True,
			})
		# The fake completes a task as it queues it.
		self._publish(r, 'cid-{}'.format(qtask), qtask)
		return {'outcome': 'ACCEPTED', 'lifecycle_state': 'queued',
			'cid': 'cid-{}'.format(qtask), 'qtask_id': qtask,
			'reservation_id': r.reservation_id, 'extra': fp_run(r)}

	def sync_run(self, r):
		answer = {'outcome': 'COMPLETED', 'cid': 'cid-sync',
			  'extra': fp_run(r)}
		if r.return_statevector:
			give_statevector(answer, r.num_qubits, r.result_capacity)
		return answer

	def _find(self, r):
		for c in self.done:
			if not c['queued']:
				continue
			if r.cid is not None and r.cid != c['cid']:
				continue
			if r.qtask_id and r.qtask_id != c['qtask_id']:
				continue
			return c
		return None

	def _collect(self, r, consume):
		with self.lock:
			found = self._find(r)
			if found is not None:
				fits = (not found['statevector'] or
					(1 << found['num_qubits']) * 16 <=
					r.result_capacity)
				if consume and fits:
					found['queued'] = False
				found = dict(found)
		if found is None:
			return {'outcome': 'IN_PROGRESS', 'completion_ready': False,
				'message': fp_task(r)}
		answer = {'outcome': 'COMPLETED', 'completion_ready': True,
			  'cid': found['cid'], 'qtask_id': found['qtask_id'],
			  'message': fp_task(r)}
		if found['statevector']:
			give_statevector(answer, found['num_qubits'],
					 r.result_capacity, r.reason == 'lie')
		return answer

	def read_cq(self, r):
		return self._collect(r, True)

	def peek_cq(self, r):
		return self._collect(r, False)

	def _task_answer(self, r):
		return {'outcome': 'CANCELLED' if r.reason else 'ACCEPTED',
			'cid': r.cid, 'qtask_id': r.qtask_id,
			'message': fp_task(r)}

	task_status = _task_answer
	cancel_task = _task_answer
	delete_circuit = _task_answer

	# --- completion events, kept and sent as a QPM does

	def register_event_notification(self, r):
		with self.lock:
			if len(self.registrations) >= REGISTRATIONS:
				raise defw2.ServiceError(
					'pending-capacity',
					'the fake keeps no more registrations')
			self.registrations.append(
				(r.target, r.type, r.reservation_id))
		return {'decision': 'accepted',
			'reservation_id': r.reservation_id,
			'extra': fp_notify(r)}

	def _publish(self, r, cid, qtask):
		"""Send a queued completion to every registration that may hear
		of it, and drop one whose target the publisher reports gone."""
		if self.publisher is None:
			return
		task = {'outcome': 'COMPLETED', 'lifecycle_state': 'completed',
			'cid': cid, 'qtask_id': qtask,
			'reservation_id': r.reservation_id,
			'completion_ready': True, 'extra': fp_run(r)}
		if r.return_statevector:
			task['statevector_shape'] = (1 << r.num_qubits,)
			task['statevector_dtype'] = 'c128'
		with self.lock:
			for registration in list(self.registrations):
				target, event_type, rid = registration
				if rid and rid != r.reservation_id:
					continue
				try:
					self.publisher.publish(
						defw2.QPM_COMPLETION, target,
						task, type=event_type,
						traceparent=r.traceparent)
				except defw2.TargetGone:
					self.registrations.remove(registration)


def main(argv):
	parser = argparse.ArgumentParser(description='The fake QPM, in Python.')
	parser.add_argument('--slow-ms', type=int, default=0)
	parser.add_argument('--execution-workers', type=int, default=2)
	parser.add_argument('--register', action='store_true')
	parser.add_argument('--service-id', default='qpm:fake:fake-20q-py')
	args = parser.parse_args(argv)

	runtime = defw2.Runtime(role='server', node_name='py-fake-qpm')
	publisher = defw2.EventPublisher(runtime)
	host = defw2.ServiceHost(runtime, args.service_id, 'qfw.qpm',
				 apis=defw2.QPM_APIS)
	if args.register:
		host.register(
			selector={'name': 'fake-20q-py',
				  'aliases': ['fake-py'],
				  'resources': ['FAKE-20q']},
			properties={'provider': 'fake-py', 'num_qubits': 20})
	host.start(FakeQPM(args.slow_ms, publisher),
		   workers={defw2.API_QPM_CONTROL: 1,
			    defw2.API_QPM_ADMISSION: 1,
			    defw2.API_QPM_EXECUTION: args.execution_workers})

	print(host.address)
	sys.stdout.flush()
	sys.stdin.read()

	host.close()
	publisher.close()
	runtime.close()
	return 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
