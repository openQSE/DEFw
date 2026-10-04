#!/usr/bin/env python3
"""Do Python's sinks and publishers keep defw2_event.h's promises?

Two runtimes in one process over na+sm: a server whose sinks take events,
and a client whose publisher sends them. defw2_event_smoke.c holds the C
side to the same promises, and defw2_python_qpm_smoke.py shows a Python
sink taking a C QPM's events and a C sink a Python QPM's. This is what is
left for the Python classes themselves:

- a sink needs a runtime that listens, and a provider has one sink;
- a completion published from a dict arrives as the Task it describes,
  and that Task passes on unchanged;
- one target's events arrive in order, numbered from 1;
- a reader waits without the interpreter lock, and closing the sink ends
  a loop that reads it;
- a publish never waits: a full publisher drops the event and says so,
  and an event that cannot be sent raises;
- a target whose delivery failed is reported once, by TargetGone, and so
  is a sink that never accepted the kind.

	defw2_python_event_smoke.py
"""

import sys
import threading
import time

import defw2
from defw2._defw2 import lib

WAIT_MS = 10000
TRACE_ID = '0af7651916cd43dd8448eb211c80319c'
TRACEPARENT = '00-' + TRACE_ID + '-b7ad6b7169203331-01'

# A completion as a QPM answers read_cq with it: every typed field, extra
# as a dict, and a statevector described by shape alone.
TASK = {
	'outcome': 'COMPLETED', 'lifecycle_state': 'completed',
	'cid': 'cid-7', 'qtask_id': 7, 'reservation_id': 7002,
	'reason': 'done', 'message': 'all well', 'completion_ready': True,
	'statevector_shape': (4, 2), 'statevector_dtype': 'c64',
	'extra': {'counts': {'00': 3, '11': 5}, 'shots': 8},
}

failures = []


def check(what, ok):
	print('{:<58} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)


def raises(fn, *args, **kwargs):
	"""The DefwError fn raised, or None."""
	try:
		fn(*args, **kwargs)
	except defw2.DefwError as error:
		return error
	return None


def wait_for(predicate, seconds=10):
	deadline = time.monotonic() + seconds
	while not predicate():
		if time.monotonic() > deadline:
			return False
		time.sleep(0.01)
	return True


def fields(task):
	"""What a Task says, for comparing two of them."""
	tensor = task.statevector
	return (task.outcome, task.lifecycle_state, task.cid, task.qtask_id,
		task.reservation_id, task.reason, task.message,
		task.completion_ready, task.statevector_delivered,
		tensor.dtype, tensor.shape, tensor.nbytes, task.extra)


def creation(server, client):
	error = raises(defw2.EventSink, client)
	check('a sink needs a runtime that listens',
	      error is not None and error.code == lib.DEFW2_ERR_CONFIG)
	sink = defw2.EventSink(server)
	check('a sink serves on its runtime, on the event provider',
	      sink.address == server.address and
	      sink.provider_id == defw2.PROVIDER_EVENT and
	      sink.target('t') == (server.address, defw2.PROVIDER_EVENT, 't'))
	error = raises(defw2.EventSink, server)
	check('a provider has one sink',
	      error is not None and error.code == lib.DEFW2_ERR_BUSY)
	error = raises(defw2.EventSink, server, provider_id=0)
	check("and none serves the directory's provider",
	      error is not None and error.code == lib.DEFW2_ERR_INVALID)
	return sink


def round_trip(sink, publisher, client):
	sink.accept(defw2.QPM_COMPLETION)
	sink.accept(defw2.QPM_COMPLETION)
	queued = publisher.publish(defw2.QPM_COMPLETION, sink.target('t1'),
				   TASK, type='k1', traceparent=TRACEPARENT)
	event = sink.next(timeout_ms=WAIT_MS)
	check('a completion published from a dict arrives',
	      queued is True and event is not None)
	check('saying what it is, whose it is and who sent it',
	      event is not None and event.api == defw2.API_QPM_EXECUTION and
	      event.name == 'completion' and event.type == 'k1' and
	      event.tag == 't1' and event.seq == 1 and
	      event.source == client.runtime_id and
	      event.traceparent is not None and
	      event.traceparent[3:3 + len(TRACE_ID)] == TRACE_ID)
	task = event.payload if event is not None else None
	check('as the Task the dict describes',
	      isinstance(task, defw2.Task) and
	      fields(task)[:9] == ('COMPLETED', 'completed', 'cid-7', 7, 7002,
				   'done', 'all well', True, False) and
	      task.extra == TASK['extra'])
	check('its statevector described and not carried',
	      task is not None and task.statevector_data is None and
	      task.statevector.dtype == defw2.DTYPE['c64'] and
	      task.statevector.shape == (4, 2) and
	      task.statevector.nbytes == 64)

	# Passing a Task on as it came is how a service forwards one.
	publisher.publish(defw2.QPM_COMPLETION, sink.target('t2'), task)
	again = sink.next(timeout_ms=WAIT_MS)
	check('and that Task passes on unchanged',
	      again is not None and again.tag == 't2' and
	      again.type is None and again.traceparent is None and
	      again.seq == 1 and task is not None and
	      fields(again.payload) == fields(task))

	stats = sink.stats()
	check('the sink counts what it took',
	      stats['received'] == 2 and stats['waiting'] == 0 and
	      stats['refused'] == 0)


def in_order(sink, publisher):
	# A plain tuple is a target as well as an EventTarget is.
	target = (sink.address, sink.provider_id, 'order')
	queued = [publisher.publish(defw2.QPM_COMPLETION, target,
				    dict(TASK, qtask_id=qtask))
		  for qtask in range(1, 101)]
	got = [sink.next(timeout_ms=WAIT_MS) for _ in range(100)]
	check('a hundred events to one target arrive in order',
	      all(queued) and all(event is not None for event in got) and
	      [event.seq for event in got] == list(range(1, 101)) and
	      [event.payload.qtask_id for event in got] ==
	      list(range(1, 101)))


def waiting(sink):
	started = time.monotonic()
	check('next gives None when nothing arrives in time',
	      sink.next(timeout_ms=300) is None and
	      time.monotonic() - started >= 0.29)

	# The reader is parked in next() while this thread counts. If its
	# wait held the interpreter lock, the count would hardly move.
	reader = threading.Thread(target=sink.next, kwargs={'timeout_ms': 1000})
	reader.start()
	ticks = 0
	deadline = time.monotonic() + 0.5
	while time.monotonic() < deadline:
		ticks += 1
	reader.join()
	check('a reader waits without the interpreter lock', ticks > 50000)
	print('    {} ticks in 0.5 s while a reader waited'.format(ticks))


def closing(server):
	sink = defw2.EventSink(server, provider_id=7)
	sink.accept(defw2.QPM_COMPLETION)
	seen = []
	reader = threading.Thread(target=lambda: seen.extend(sink))
	reader.start()
	time.sleep(0.2)
	started = time.monotonic()
	sink.close()
	reader.join(5)
	check('closing a sink ends a loop that reads it',
	      not reader.is_alive() and time.monotonic() - started < 1 and
	      seen == [])
	error = raises(sink.next, timeout_ms=0)
	check('and next then says it is closed',
	      error is not None and error.category == 'not-found')


def refusals(sink, publisher, client):
	with defw2.EventPublisher(client, max_bytes=1) as tiny:
		queued = tiny.publish(defw2.QPM_COMPLETION, sink.target('tiny'),
				      TASK)
		stats = tiny.stats()
	check('a full publisher drops an event rather than wait',
	      queued is False and stats['dropped'] == 1 and
	      stats['published'] == 0)

	error = raises(publisher.publish, defw2.QPM_COMPLETION,
		       sink.target('long'), dict(TASK, cid='c' * (64 * 1024)))
	check('an event that cannot be sent raises rather than queues',
	      error is not None and not isinstance(error, defw2.TargetGone) and
	      error.category == 'invalid-argument')


def gone(server, publisher):
	sink = defw2.EventSink(server, provider_id=8)
	sink.accept(defw2.QPM_COMPLETION)
	target = sink.target('gone')
	sink.close()
	failed = publisher.stats()['failed']
	check('an event to a closed sink is still queued',
	      publisher.publish(defw2.QPM_COMPLETION, target, TASK) is True)
	check('its delivery fails',
	      wait_for(lambda: publisher.stats()['failed'] > failed))
	error = raises(publisher.publish, defw2.QPM_COMPLETION, target, TASK)
	check('so the next publish to it raises TargetGone',
	      isinstance(error, defw2.TargetGone) and
	      error.category == 'not-found')
	check('once: the one after that is tried afresh',
	      publisher.publish(defw2.QPM_COMPLETION, target, TASK) is True)

	# A sink takes only what it accepted, and refuses the rest.
	other = defw2.EventSink(server, provider_id=9)
	target = other.target('unaccepted')
	failed = publisher.stats()['failed']
	publisher.publish(defw2.QPM_COMPLETION, target, TASK)
	wait_for(lambda: publisher.stats()['failed'] > failed)
	error = raises(publisher.publish, defw2.QPM_COMPLETION, target, TASK)
	check('a sink that never accepted completions is gone to them',
	      isinstance(error, defw2.TargetGone) and
	      other.stats()['received'] == 0)
	other.close()


def main():
	server = defw2.Runtime(role='server', node_name='py-event-sink')
	client = defw2.Runtime(node_name='py-event-sender')
	publisher = defw2.EventPublisher(client)

	sink = creation(server, client)
	round_trip(sink, publisher, client)
	in_order(sink, publisher)
	waiting(sink)
	closing(server)
	refusals(sink, publisher, client)
	gone(server, publisher)
	sink.close()

	publisher.close()
	error = raises(publisher.publish, defw2.QPM_COMPLETION,
		       (server.address, defw2.PROVIDER_EVENT, 'late'), TASK)
	check('a closed publisher sends nothing',
	      error is not None and error.category == 'cancelled')
	client.close()
	server.close()

	print('PYTHON EVENT SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main())
