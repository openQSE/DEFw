#!/usr/bin/env python3
"""The QPM checks of defw2_qpm_smoke.c, through the Python client.

Run against either fake, the C one served by defw2_qpm_smoke --serve or the
Python one in defw2_qpm_fake.py. Every check expects exactly what the C
checks expect, so a pass here and a pass there mean a Python caller and a C
caller see the same QPM.

	defw2_qpm_client_check.py ADDRESS

It ends with the same calls from many threads at once, which is how a
Python application calls, and which the binding has to allow, and then
with completion events, taken from a sink in the client's own runtime,
which is why that runtime is a server.
"""

import sys
import threading

import defw2
from defw2._defw2 import lib

from defw2_qpm_fake import (
	BIG_EXTRA,
	fp_ctx,
	fp_notify,
	fp_reserve,
	fp_run,
	fp_task,
)

try:
	import numpy
except ImportError:
	numpy = None

SV_QUBITS = 20
SV_COUNT = 1 << SV_QUBITS
SV_BYTES = SV_COUNT * 16
QASM = ('OPENQASM 2.0;\ninclude "qelib1.inc";\n'
	'qreg q[2];\nh q[0];\ncx q[0],q[1];\n')
QASM3 = 'OPENQASM 2.0;\nqreg q[3];\nh q[0];\n'
EVENT_WAIT_MS = 10000

# The W3C example, so the event's trace can be told from any other.
TRACE_ID = '0af7651916cd43dd8448eb211c80319c'
TRACEPARENT = '00-' + TRACE_ID + '-b7ad6b7169203331-01'

failures = []


def check(what, ok):
	print('{:<58} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)


def raises(category, fn, *args, **kwargs):
	"""The DefwError fn raised, when its category is the one expected."""
	try:
		fn(*args, **kwargs)
	except defw2.DefwError as error:
		return error if error.category == category else None
	return None


class Values:
	"""A request as the fake sees it, for computing a fingerprint."""

	def __init__(self, **values):
		self.__dict__.update(values)


def sv_ok(data, count=SV_COUNT):
	if numpy is not None:
		expect = numpy.arange(count, dtype=numpy.float64) * (1 - 1j)
		return data.shape == (count,) and bool((data == expect).all())
	import struct
	return all(struct.unpack_from('<dd', data, 16 * k) == (k, -k)
		   for k in range(0, count, 997 if count > 997 else 1))


def control_checks(qpm):
	status = qpm.is_ready(reservation_id=17, token='tok-17')
	check('is_ready answers', status.state == 'running')
	check("is_ready's typed fields cross the wire",
	      status.ready and status.initialized and
	      status.accepting_requests and not status.provider_ready and
	      status.active_task_count == 3 and
	      status.active_reservation_count == 2)
	check("is_ready's request arrived whole",
	      status.extra_json == fp_ctx(Values(reservation_id=17,
						 token='tok-17')))

	status = qpm.get_service_status(reservation_id=17, token='tok-17')
	check('a 200 KiB extra, longer than any string, arrives intact',
	      status.extra_json == 'e' * (BIG_EXTRA - 1))


def admission_checks(qpm):
	reserve = dict(
		request_id=0x1234567890abcdef, user='doug', job_id='slurm-4242',
		allocation_id='alloc-1', target_device_id='fake-iqm-20q',
		scope_id=None, workload_kind='quantum', num_qubits=20,
		walltime_ns=300000000000, ttl_ns=360000000000,
		task_class={'count': 4, 'qubit_count': 20, 'depth': 10,
			    'one_q_gate_count': 100, 'two_q_gate_count': 50,
			    'shots': 1024, 'measurement_count': 20},
		extra='{"workload":{"example":"w5"},"run_context":{}}')
	decision = qpm.reserve(token='munge-cred', **reserve)
	check("reserve's decision crosses the wire",
	      decision.decision == 'accepted' and
	      decision.reservation_id == 7001 and
	      decision.request_id == reserve['request_id'] and
	      decision.reason_code == 1 and decision.retry_after_ns == 42 and
	      decision.message is None)
	expect = fp_reserve(Values(reservation_id=0, token='munge-cred',
				   extra_json=reserve.pop('extra'), **reserve))
	check('every reserve field arrived, an absent one as absent',
	      decision.extra_json == expect)

	check('an unserved method is not found',
	      raises('not-found', qpm.renew, 7001) is not None)
	error = raises('invalid-reservation', qpm.cancel, 7001)
	check("a service's own status category reaches the caller",
	      error is not None and error.message == 'no such reservation')
	check('a failure with no status is a provider failure',
	      raises('provider-failure', qpm.release, 7001,
		     reason_code=99) is not None)

	reservation = qpm.get_reservation(7001, token='t')
	check('get_reservation answers',
	      reservation.reservation_id == 7001 and
	      reservation.state == 'active' and
	      reservation.created_at_ns == 1000 and
	      reservation.expires_at_ns == 2000 and
	      reservation.extra_json == fp_ctx(Values(reservation_id=7001,
						      token='t')))
	check('an answer too long to send fails rather than hangs',
	      raises('provider-failure', qpm.get_reservation, 666)
	      is not None)


def execution_checks(qpm):
	run = dict(circuit_format='openqasm2', num_qubits=2, num_shots=1024,
		   compiler='staq', return_statevector=False, run_timeout_ms=0,
		   cancel_on_timeout=True,
		   extra='{"qubit_mapping":{"0":"QB1","1":"QB2"}}')
	task = qpm.async_run(QASM, reservation_id=7001, token='tok', **run)
	check('async_run answers',
	      task.outcome == 'ACCEPTED' and task.cid and task.qtask_id)
	expect = fp_run(Values(reservation_id=7001, token='tok',
			       circuit=QASM.encode(),
			       extra_json=run.pop('extra'), **run))
	check('every run field arrived, a zero timeout as a timeout',
	      task.extra_json == expect)

	binary = bytes((i * 31) & 0xff for i in range(1024 * 1024))
	task = qpm.async_run(binary, circuit_format='qpy', num_qubits=2,
			     num_shots=1024, compiler='staq',
			     cancel_on_timeout=True, reservation_id=7001,
			     token='tok')
	expect = fp_run(Values(reservation_id=7001, token='tok', circuit=binary,
			       circuit_format='qpy', num_qubits=2,
			       num_shots=1024, compiler='staq',
			       return_statevector=False, run_timeout_ms=None,
			       cancel_on_timeout=True, extra_json=None))
	check('a 1 MiB binary circuit arrives byte for byte',
	      task.extra_json == expect)

	check('a run with no circuit is an invalid argument',
	      raises('invalid-argument', qpm.async_run, b'',
		     circuit_format=None) is not None)

	# --- the result path
	whole = (numpy.empty(SV_COUNT, dtype=numpy.complex128)
		 if numpy is not None else bytearray(SV_BYTES))
	task = qpm.sync_run(QASM, num_qubits=SV_QUBITS, num_shots=1024,
			    return_statevector=True, result=whole,
			    reservation_id=7001)
	check('sync_run pushes a 20-qubit statevector',
	      task.statevector_delivered and
	      task.statevector.dtype == defw2.DTYPE['c128'] and
	      task.statevector.shape == (SV_COUNT,) and
	      task.statevector.nbytes == SV_BYTES)
	check('and all 16 MiB of it is right, in the buffer lent',
	      task.statevector_data is not None and
	      sv_ok(task.statevector_data))

	task = qpm.sync_run(QASM, num_qubits=SV_QUBITS, num_shots=1024,
			    return_statevector=True, reservation_id=7001)
	check('sync_run with no buffer says how much it needed',
	      not task.statevector_delivered and
	      task.statevector.nbytes == SV_BYTES)

	cid = qpm.async_run(QASM, num_qubits=SV_QUBITS, num_shots=1024,
			    return_statevector=True, reservation_id=7001).cid
	task = qpm.read_cq(cid=cid, result=1024, reservation_id=7001)
	check('read_cq into a buffer that is too small',
	      task.completion_ready and not task.statevector_delivered and
	      task.statevector.nbytes == SV_BYTES)
	task = qpm.peek_cq(cid=cid, reservation_id=7001)
	check('peek_cq still sees it, so nothing was consumed',
	      task.completion_ready and task.cid == cid)

	check('a statevector that contradicts its description is refused',
	      raises('provider-failure', qpm.peek_cq, cid=cid, result=whole,
		     reason='lie', reservation_id=7001) is not None)

	task = qpm.read_cq(cid=cid, result=whole, reservation_id=7001)
	check('the retry with a big enough buffer gets it',
	      task.statevector_delivered and
	      sv_ok(task.statevector_data))
	task = qpm.read_cq(cid=cid, result=whole, reservation_id=7001)
	check('and then it is gone',
	      not task.completion_ready and not task.statevector_delivered)

	ref = dict(cid='cid-x', qtask_id=9, reservation_id=7001)
	task = qpm.task_status(**ref)
	check('task_status names its task both ways',
	      task.qtask_id == 9 and task.cid == 'cid-x' and
	      task.message == fp_task(Values(token=None, reason=None, **ref)))
	task = qpm.cancel_task(reason='user-requested', **ref)
	check('cancel_task carries its reason',
	      task.outcome == 'CANCELLED' and
	      task.message == fp_task(Values(token=None,
					     reason='user-requested', **ref)))
	task = qpm.delete_circuit(cid='cid-x', reservation_id=7001)
	check('delete_circuit answers',
	      task.message == fp_task(Values(token=None, reason=None,
					     cid='cid-x', qtask_id=0,
					     reservation_id=7001)))


def threaded_checks(qpm, threads=8, calls=50):
	"""The same client, called from many Python threads at once."""
	errors = []
	done = []

	def worker(index):
		try:
			for _ in range(calls):
				qpm.is_ready(reservation_id=index)
				task = qpm.async_run(QASM, num_qubits=2,
						     num_shots=16,
						     reservation_id=7001)
				collected = qpm.read_cq(cid=task.cid,
							reservation_id=7001)
				if not collected.completion_ready:
					raise AssertionError(task.cid)
			done.append(index)
		except Exception as error:  # noqa: BLE001
			errors.append(repr(error))

	workers = [threading.Thread(target=worker, args=(index,))
		   for index in range(threads)]
	for thread in workers:
		thread.start()
	for thread in workers:
		thread.join(120)
	check('{} threads share one client, {} calls each'.format(
		threads, 3 * calls),
	      len(done) == threads and not errors)
	if errors:
		print('  ' + errors[0])


def event_checks(rt, qpm):
	"""Completion events. Two registrations of one sink, told apart by
	their tags, for reservation 7002. A run under 7001 goes first, so an
	event for it would arrive ahead of the 7002 run's on each tag."""
	sink = defw2.EventSink(rt)
	check('a listening client serves a sink for completions',
	      sink.address == rt.address and
	      sink.provider_id == defw2.PROVIDER_EVENT)

	notify = dict(type='circuit-result', tag='py-tag', reservation_id=7002,
		      token='tok-ev', extra='{"filters":{"user":"doug"}}')
	decision = qpm.register_event_notification(sink, **notify)
	check('register_event_notification is accepted',
	      decision.decision == 'accepted' and
	      decision.reservation_id == 7002)
	expect = fp_notify(Values(
		reservation_id=7002, token='tok-ev',
		target=sink.target('py-tag'), type='circuit-result',
		extra_json=notify['extra']))
	check('every registration field arrived', decision.extra_json == expect)
	decision = qpm.register_event_notification(
		sink, type='other', tag='py-other', reservation_id=7002,
		token='tok-ev')
	check('and the same sink again under another tag',
	      decision.decision == 'accepted')

	run = dict(circuit_format='openqasm2', num_qubits=3, num_shots=64,
		   return_statevector=True)
	first = qpm.async_run(QASM3, reservation_id=7001, **run)
	check('a run under another reservation is accepted',
	      first.outcome == 'ACCEPTED')
	task = qpm.async_run(QASM3, reservation_id=7002,
			     traceparent=TRACEPARENT, **run)
	check('and so is one under the registered reservation',
	      task.cid is not None)

	events = [sink.next(timeout_ms=EVENT_WAIT_MS) for _ in range(2)]
	tagged = {event.tag: event for event in events if event is not None}
	mine = tagged.get('py-tag')
	other = tagged.get('py-other')
	check('its completion reaches the sink once per registration',
	      mine is not None and other is not None)
	check("and the other reservation's never does",
	      mine is not None and mine.seq == 1 and other is not None and
	      other.seq == 1 and sink.next(timeout_ms=250) is None)
	check('each event says what it is and whose it is',
	      mine is not None and mine.api == defw2.API_QPM_EXECUTION and
	      mine.name == 'completion' and mine.type == 'circuit-result' and
	      bool(mine.source) and other is not None and
	      other.type == 'other')

	got = mine.payload if mine is not None else None
	expect = fp_run(Values(reservation_id=7002, token=None,
			       circuit=QASM3.encode(), compiler=None,
			       run_timeout_ms=None, cancel_on_timeout=False,
			       extra_json=None, **run))
	check('it carries the task read_cq answers with',
	      isinstance(got, defw2.Task) and got.outcome == 'COMPLETED' and
	      got.lifecycle_state == 'completed' and got.cid == task.cid and
	      got.qtask_id == task.qtask_id and got.reservation_id == 7002 and
	      got.completion_ready and got.reason is None and
	      got.message is None and got.extra_json == expect)
	check('and describes its statevector without carrying it',
	      got is not None and not got.statevector_delivered and
	      got.statevector_data is None and
	      got.statevector.dtype == defw2.DTYPE['c128'] and
	      got.statevector.shape == (8,) and got.statevector.nbytes == 128)
	check("the event joins the run's trace",
	      mine is not None and mine.traceparent is not None and
	      len(mine.traceparent) == len(TRACEPARENT) and
	      mine.traceparent[3:3 + len(TRACE_ID)] == TRACE_ID)

	# What the event described is what a collecting read needs.
	done = qpm.read_cq(cid=task.cid, reservation_id=7002,
			   result=got.statevector.nbytes if got else 0)
	check('read_cq into a buffer of the size it gave collects it',
	      done.statevector_delivered and
	      sv_ok(done.statevector_data, 8))

	check('a registration that names no sink is refused',
	      raises('invalid-argument', qpm.register_event_notification,
		     defw2.EventTarget(None), reservation_id=7002) is not None)
	check("and so is one on the directory's provider",
	      raises('invalid-argument', qpm.register_event_notification,
		     defw2.EventTarget(sink.address, 0),
		     reservation_id=7002) is not None)

	# The service hears it is gone on its next completion for 7002.
	sink.close()


def main(argv):
	if len(argv) != 1:
		raise SystemExit('usage: defw2_qpm_client_check.py ADDRESS')
	address = argv[0]
	with defw2.Runtime(role='server', node_name='qpm-py-client') as rt:
		with defw2.QPM(rt, address) as qpm:
			control_checks(qpm)
			admission_checks(qpm)
			execution_checks(qpm)
			threaded_checks(qpm)
			event_checks(rt, qpm)

		# A QPM method on another API's provider: Mercury refuses it
		# before any handler runs, so it is the transport that says.
		with defw2.QPM(rt, address, providers={
				defw2.API_QPM_CONTROL:
				defw2.PROVIDER_QPM_EXECUTION}) as wrong:
			try:
				wrong.is_ready(timeout_ms=5000)
				error = None
			except defw2.DefwError as raised:
				error = raised
			check('a QPM method on another API\'s provider is not found',
			      error is not None and
			      error.code == lib.DEFW2_ERR_NOT_FOUND)

	print('QPM CLIENT CHECK ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
