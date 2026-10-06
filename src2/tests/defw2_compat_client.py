#!/usr/bin/env python3
"""A v1 client of a v1 QPM, run on v2 through defw2.compat.

Run under defw2-python, against tests/compat/svc_v1_qpm served by
defw2-python --serve and registered in the directory DEFW2_DIRSVC names:

	defw2-python defw2_compat_client.py

It is written the way QFw's client code is: it finds the QPM through
defw.dirsvc, connects with defw.connect_to_binding and calls v1 API
classes. Every answer, and every exception, is compared with what the same
call gives on a QPM object of the same class called directly in this
process, which is what a v1 caller got, since v1's remote calls returned
the service's own dictionaries and raised its own exceptions. The two
QPMs get the same calls in the same order, so they stay in step.

The directory's answers are compared with v1's own directory code, loaded
from the v1 tree, given the same record.

Events are the ones v1 delivered. The QPM puts each completion to the
caller's registration, and it arrives on the caller's own queue with its
statevector, or from the sweep when its event is lost. A registration
whose sink is gone is dropped by the QPM when its put fails. A service
coming and going reaches the caller as v1's directory events.
"""

import base64
import builtins
import importlib.util
import os
import select
import shutil
import struct
import sys
import tempfile
import time

import defw
import defw_exception
import defw_trace
from defw_app_util import defw_get_directory_service
from defw_event_baseapi import BaseEventAPI

from svc_v1_qpm import svc_qpm

import defw2
from defw2.compat import _state

try:
	import numpy
except ImportError:
	numpy = None

QASM = ('OPENQASM 2.0;\ninclude "qelib1.inc";\n'
	'qreg q[2];\nh q[0];\ncx q[0],q[1];\n')
RID = svc_qpm.RESERVATION

failures = []


def check(what, ok, detail=None):
	print('{:<62} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)
		if detail:
			print('    ' + str(detail)[:2000])


def _diff(got, want):
	if isinstance(got, dict) and isinstance(want, dict):
		keys = sorted(set(got) | set(want), key=str)
		return {k: (got.get(k, '<absent>'), want.get(k, '<absent>'))
			for k in keys if got.get(k, '<absent>') !=
			want.get(k, '<absent>')}
	return (got, want)


def outcome(fn, *args, **kwargs):
	"""What a call gave: ('answer', value) or ('raised', class, message)."""
	try:
		return ('answer', fn(*args, **kwargs))
	except Exception as error:  # noqa: BLE001
		if isinstance(error, defw_exception.DEFwError):
			text = str(error.msg)
		else:
			text = str(error)
		return ('raised', type(error).__name__, text)


def same(what, remote, reference, method, *args, **kwargs):
	"""Call method on both QPMs and check they gave the same."""
	got = outcome(getattr(remote, method), *args, **kwargs)
	want = outcome(getattr(reference, method), *args, **kwargs)
	check(what, got == want, None if got == want else
	      _diff(got[1], want[1]) if got[0] == want[0] == 'answer'
	      else (got, want))
	return got[1] if got[0] == 'answer' else None


def resolve(dirsvc, **filters):
	deadline = time.monotonic() + 30
	while True:
		found = dirsvc.resolve_services(**filters)
		if found or time.monotonic() > deadline:
			return found
		time.sleep(0.1)


# --- the directory ---------------------------------------------------------


def v1_directory():
	path = os.path.join(os.environ['DEFW_PATH'], 'python', 'infra',
			    'defw_directory.py')
	spec = importlib.util.spec_from_file_location('v1_directory', path)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


def _steady(entries):
	"""Resolve answers without the two clocks the stores keep apart."""
	steady = []
	for entry in entries:
		entry = dict(entry)
		record = dict(entry['service_record'])
		record.pop('last_seen', None)
		record.pop('state_changed_at', None)
		entry['service_record'] = record
		steady.append(entry)
	return steady


def directory_checks(dirsvc, record):
	advertisement = svc_qpm.QPM(start=False).query()
	properties = dict(advertisement['properties'], qpm_type=2,
			  qpm_capabilities=2)
	check('the record is the one v1 would have built',
	      record['service_id'] == advertisement['service_id'] and
	      record['service_name'] == 'QPM' and
	      record['service_type'] == 'qfw.qpm' and
	      record['api_bindings'] == advertisement['api_bindings'] and
	      record['selector'] == advertisement['selector'] and
	      record['properties'] == properties and
	      record['capability'] == advertisement['capability'] and
	      record['state'] == 'UP' and record['generation'] == 1,
	      record)

	rt = _state.runtime()
	with defw2.Directory(rt, rt.dirsvc) as v2:
		found = v2.resolve(service_type='qfw.qpm')
	typed = found[0] if found else {}
	check('and a v2 client finds it by its typed fields',
	      len(found) == 1 and
	      typed['runtime_id'] == record['runtime_id'] and
	      typed['address'] == record['endpoint']['address'] and
	      typed['selector']['name'] == 'fake-v1-4q' and
	      typed['properties']['provider'] == 'v1-fake' and
	      typed['properties']['num_qubits'] == '4' and
	      typed['properties']['qpm_type'] == '2' and
	      typed['properties']['circuit_formats'] ==
	      '["openqasm2","qpy"]' and
	      {(b['binding_name'], b['provider_id'])
	       for b in typed['bindings']} == {
		      ('control', 2), ('admission', 3), ('execution', 4),
		      ('admission-policy', 6), ('scheduler', 7),
		      ('telemetry', 8)},
	      typed)

	oracle = v1_directory().Directory(runtime_id_provider=lambda: 'dir')
	oracle.register_service(dict(record))
	queries = (
		{'service_type': 'qfw.qpm'},
		{'service_type': 'qfw.qpm', 'binding_name': 'execution'},
		{'service_id': record['service_id'], 'binding_name': 'admission'},
		{'service_name': 'QPM', 'client_class': 'QPMControl'},
		{'qpm_type': 2},
		{'qpm_type': 1},
		{'qpm_capabilities': 2, 'binding_name': 'telemetry'},
		{'selector_resource': 'V1-FAKE-4q'},
		{'selector_alias': 'v1-fake', 'binding_name': 'control'},
		{'selector_alias': 'nobody'},
		{'properties': {'provider': 'v1-fake', 'num_qubits': 4}},
		{'properties': {'num_qubits': '4'}},
		{'provider': 'elsewhere', 'api_category': 'execution'},
	)
	for query in queries:
		got = _steady(dirsvc.resolve_services(**query))
		want = _steady(oracle.resolve_services(**query))
		check('resolve_services({}) as v1 answers'.format(
			', '.join(sorted(query))), got == want,
		      None if got == want else (len(got), len(want)))
	check('and the generation is the record\'s',
	      dirsvc.get_service_generation(record['service_id']) == 1 and
	      dirsvc.get_service_generation('nobody') is None)


# --- the typed methods --------------------------------------------------


def binding_checks(resolved):
	"""A binding comes from a directory record, which anything that can
	register in the directory writes. So compat imports a client module
	only from where v1 API modules come from, and calls only a v1 API
	class in it. Each module here would mark builtins when it runs."""
	probe = tempfile.mkdtemp(prefix='defw2-probe-')
	api = ('from defw_remote import BaseRemote\n\n\n'
	       'class Probe(BaseRemote):\n'
	       '\tpass\n')
	with open(os.path.join(probe, 'defw2_probe.py'), 'w') as f:
		f.write('import builtins\nbuiltins.defw2_probe_ran = True\n' +
			api)
	os.mkdir(os.path.join(probe, 'defw2_probe_pkg'))
	with open(os.path.join(probe, 'defw2_probe_pkg', '__init__.py'),
		  'w') as f:
		f.write('import builtins\n'
			'builtins.defw2_probe_pkg_ran = True\n')
	with open(os.path.join(probe, 'defw2_probe_pkg', 'api.py'), 'w') as f:
		f.write(api)
	sys.path.insert(0, probe)

	def refused(module, cls):
		binding = dict(resolved['selected_binding'],
			       client_module=module, client_class=cls)
		try:
			defw.connect_to_binding(
				{'service_record': resolved['service_record'],
				 'selected_binding': binding})
		except defw_exception.DEFwError as error:
			return 'defw2.compat' in str(error)
		return False

	try:
		check('a client module from anywhere else is refused, unrun',
		      refused('defw2_probe', 'Probe') and
		      'defw2_probe' not in sys.modules and
		      not hasattr(builtins, 'defw2_probe_ran'))
		check('a dotted one is refused before its package runs',
		      refused('defw2_probe_pkg.api', 'Probe') and
		      'defw2_probe_pkg' not in sys.modules and
		      not hasattr(builtins, 'defw2_probe_pkg_ran'))
		check('so is one from the standard library',
		      refused('subprocess', 'Popen'))
		# The API module's own type is a class, but no BaseRemote.
		check('and a name in an API module that is no v1 API class',
		      refused('api_v1_qpm', '__class__') and
		      refused('api_v1_qpm', 'NoSuchClass'))
	finally:
		sys.path.remove(probe)
		shutil.rmtree(probe, ignore_errors=True)


def control_checks(control, reference):
	same('is_ready answers as the v1 QPM does', control, reference,
	     'is_ready', token='tok-17')
	same('get_service_status, with a False and a None in it', control,
	     reference, 'get_service_status')


def admission_checks(admission, reference):
	request = {
		'owner': {'user': 'doug'},
		'request_id': 0x1234567890abcdef,
		'job_id': 'slurm-4242',
		'allocation_id': 'alloc-1',
		'num_qubits': 4,
		'workload_kind': 'quantum',
		'walltime_ns': 300000000000,
		'ttl_ns': 360000000000,
		'scope_id': '',
		'workload': {'example': 'w7', 'operation': 'async_run'},
		'run_context': {'operation': 'async_run'},
		'task_class': {'count': 1, 'qubit_count': 4, 'depth': 10,
			       'one_q_gate_count': 4, 'two_q_gate_count': 3,
			       'shots': 1024, 'measurement_count': 4},
	}
	same('reserve, and the request arrived key for key', admission,
	     reference, 'reserve', token='munge', request=request)
	partial = dict(request, task_class={'count': 2, 'shots': 100})
	same('a partial task_class stays as it was sent', admission,
	     reference, 'reserve', request=partial)
	same('reserve with no request raises v1\'s DEFwExecutionError',
	     admission, reference, 'reserve')
	got = outcome(admission.reserve, request={'workload_kind': 'refused'})
	check('an exception class v2 cannot name arrives as DEFwRemoteError',
	      got == ('raised', 'DEFwRemoteError',
		      'AdmissionPolicyError: policy refused the workload'), got)

	same('renew, with a request', admission, reference, 'renew',
	     reservation_id=RID, request={'ttl_ns': 5, 'now_ns': 0})
	same('renew, with none, which is not an empty one', admission,
	     reference, 'renew', reservation_id=RID)
	same('renew, with an empty one, which is not none', admission,
	     reference, 'renew', reservation_id=RID, request={})
	same('release with a reason code', admission, reference, 'release',
	     reservation_id=RID, reason=3)
	same('a DEFwReserveError comes back as one', admission, reference,
	     'release', reservation_id=RID, reason=99)
	same('release with no reservation raises as v1 did', admission,
	     reference, 'release')
	same('a DEFwOutOfResources comes back as one', admission, reference,
	     'cancel', reservation_id=RID)
	same('a built-in exception comes back as itself', admission,
	     reference, 'cancel', reservation_id=RID, reason=98)
	same('get_reservation', admission, reference, 'get_reservation',
	     reservation_id=RID, token='t')

	# A caller that traces, as QFw's telemetry does: v1 carried the
	# context its defw_trace hooks injected, and so does compat.
	traceparent = '00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01'
	defw_trace.set_hooks(inject=lambda carrier: carrier.update(
		traceparent=traceparent))
	try:
		traced = admission.get_reservation(reservation_id=svc_qpm.TRACED)
	finally:
		defw_trace.clear_hooks()
	seen = traced.get('trace_seen') or ''
	check('the service ran the call in the caller\'s trace',
	      seen.split('-')[1:2] == traceparent.split('-')[1:2], traced)


def execution_checks(execution, reference):
	info = {'qasm': QASM, 'num_qubits': 2, 'num_shots': 1024,
		'compiler': 'staq', 'qubit_mapping': {'0': 'QB1', '1': 'QB2'}}
	first = same('async_run, and info arrived key for key', execution,
		     reference, 'async_run', info, reservation_id=RID,
		     token='tok')

	qpy = bytes((i * 31) & 0xff for i in range(1024 * 1024))
	info = {'circuit': {'format': 'qpy',
			    'data': base64.b64encode(qpy).decode('ascii')},
		'num_qubits': 2, 'num_shots': 100, 'return_statevector': False}
	same('a 1 MiB QPY circuit, a whole-second timeout and a flag',
	     execution, reference, 'async_run', info, reservation_id=RID,
	     timeout=30, cancel_on_timeout=True)
	same('a fractional timeout stays fractional', execution, reference,
	     'async_run', {'qasm': QASM, 'num_shots': 8}, reservation_id=RID,
	     timeout=2.5)
	same('a run with no reservation raises as v1 did', execution,
	     reference, 'async_run', {'qasm': QASM})
	same('and so does one the QPM refuses', execution, reference,
	     'async_run', {'qasm': QASM, 'num_shots': 0}, reservation_id=RID)

	cid = first['cid'] if first else None
	same('peek_cq of a completion', execution, reference, 'peek_cq',
	     cid=cid, reservation_id=RID)
	same('read_cq of it', execution, reference, 'read_cq', cid=cid,
	     reservation_id=RID)
	same('and then it is gone', execution, reference, 'read_cq', cid=cid,
	     reservation_id=RID)
	same('read_cq with no reservation is an outcome, not an error',
	     execution, reference, 'read_cq')

	# Two completions are left. With no cid, read_cq collects them in
	# order, and then has nothing.
	for index in range(3):
		same('read_cq with no cid, call {}'.format(index + 1),
		     execution, reference, 'read_cq', reservation_id=RID)

	big = same('a 20-qubit statevector run', execution, reference,
		   'async_run', {'qasm': QASM, 'num_qubits': 20,
				 'return_statevector': True},
		   reservation_id=RID)
	start = time.monotonic()
	same('its 16 MiB statevector arrives as the v1 payload, byte for '
	     'byte', execution, reference, 'read_cq',
	     cid=big['cid'] if big else None, reservation_id=RID)
	print('    20-qubit completion through compat in {:.2f} s'.format(
		time.monotonic() - start))

	same('a statevector run whose size the client cannot know', execution,
	     reference, 'async_run', {'qasm': QASM, 'num_qubits': 12,
				      'return_statevector': True},
	     reservation_id=RID)
	same('arrives after the service says how much room it needs',
	     execution, reference, 'read_cq', reservation_id=RID)
	same('sync_run with a statevector', execution, reference, 'sync_run',
	     {'qasm': QASM, 'num_qubits': 10, 'return_statevector': True},
	     reservation_id=RID)

	same('task_status by cid', execution, reference, 'task_status',
	     cid=cid, reservation_id=RID)
	same('task_status by qtask_id', execution, reference, 'task_status',
	     qtask_id=2, reservation_id=RID)
	same('an unknown task keeps the None values v1 sent', execution,
	     reference, 'task_status', cid='nope', reservation_id=RID)
	same('cancel_task carries its reason', execution, reference,
	     'cancel_task', cid=cid, reservation_id=RID,
	     reason='user-requested')
	same('cancel_task of nothing raises v1\'s DEFwError', execution,
	     reference, 'cancel_task', cid='nope', reservation_id=RID)
	same('delete_circuit, whose answer has no typed field at all',
	     execution, reference, 'delete_circuit', cid,
	     reservation_id=RID)

	same('a method v2 has no typed form for goes as a document',
	     execution, reference, 'get_device_profile')


def document_checks(apis, reference):
	"""The methods v2 has no typed form for, on every API."""
	policy = apis['admission-policy']
	same('control answers a document as the v1 QPM does',
	     apis['control'], reference, 'test', token='tok-9')
	same('admission policy too, with its arguments by keyword', policy,
	     reference, 'get_device_profile', device_id='fake-v1-4q')
	same('and by position, named as the API class names them', policy,
	     reference, 'set_admission_policy', None, 'fake-v1-4q',
	     {'max_qubits': 2})
	same("a built-in exception arrives as the service's own",
	     policy, reference, 'set_admission_policy',
	     device_id='fake-v1-4q', policy={})
	same('and so does a DEFw one', policy, reference,
	     'set_admission_policy', device_id='fake-v1-4q',
	     policy={'max_qubits': 20})
	same('scheduler answers one', apis['scheduler'], reference,
	     'get_scheduler_status', device_id='other-4q')
	same("and an argument left out takes the service's default",
	     apis['scheduler'], reference, 'get_scheduler_status')
	same('telemetry answers nested data as it was',
	     apis['telemetry'], reference, 'get_backend_info', lib='qdmi')

	got = outcome(apis['telemetry'].capability_map)
	check('a tuple in an answer arrives as a list, as JSON carries it',
	      got == ('answer', {'gates': ['cx', 'h', 'rz']}), got)
	got = outcome(apis['telemetry'].get_device_info)
	check('an answer JSON cannot carry fails, rather than losing it',
	      got[0] == 'raised' and got[1] == 'DEFwRemoteError' and
	      'JSON cannot carry' in got[2], got)
	got = outcome(policy.set_admission_policy, device_id='x',
		      policy={'blob': b'\x00'})
	check('and so does a request JSON cannot carry',
	      got[0] == 'raised' and got[1] == 'DEFwError' and
	      'document carries JSON' in got[2], got)

	rt = _state.runtime()
	with defw2.Directory(rt, rt.dirsvc) as v2:
		found = v2.resolve(service_type='qfw.qpm')
	with defw2.QPM.from_record(rt, found[0]) as qpm:
		try:
			qpm.document(defw2.API_QPM_CONTROL, 'query')
			reached = True
		except defw2.DefwError as error:
			reached = error.category != 'not-found'
		check('a method no API class declares cannot be reached',
		      not reached)


def take(events, count, timeout=30):
	"""count events from a v1 event queue, waiting as v1 code does."""
	taken = []
	deadline = time.monotonic() + timeout
	while len(taken) < count:
		left = deadline - time.monotonic()
		if left <= 0:
			break
		ready, _, _ = select.select([events.fileno()], [], [], left)
		if ready:
			taken.extend(events.get())
	return taken


def completion_of(reference, cid):
	"""The completion record the reference QPM queued for cid."""
	for record in reference.completions:
		if record['cid'] == cid:
			return dict(record)
	return None


def event_checks(execution, reference):
	events = BaseEventAPI()
	events.register_external()
	registration = execution.register_event_notification(
		defw.me.my_endpoint(), 1, events.class_id())
	check('register_event_notification is the v1 QPM\'s own answer',
	      registration == {'status': 'accepted',
			       'class_id': events.class_id(),
			       'registration_count': 1}, registration)

	info = {'qasm': QASM, 'num_qubits': 16, 'return_statevector': True}
	task = same('a run after registering', execution, reference,
		    'async_run', info, reservation_id=RID)
	cid = task['cid'] if task else None
	delivered = take(events, 1)
	check('its completion arrives as an event on the caller\'s queue',
	      len(delivered) == 1 and delivered[0].get_evtype() == 1)
	payload = delivered[0].get_event() if delivered else None
	want = completion_of(reference, cid)
	check('carrying the completion record, statevector and all',
	      payload == want, _diff(payload, want))
	same('and the completion is still queued for read_cq', execution,
	     reference, 'read_cq', cid=cid, reservation_id=RID)

	lost = dict(info, num_qubits=4, drop_event=True)
	task = same('a run the QPM sends no event for', execution, reference,
		    'async_run', lost, reservation_id=RID)
	cid = task['cid'] if task else None
	want = reference.peek_cq(cid=cid, reservation_id=RID)
	delivered = take(events, 1)
	payload = delivered[0].get_event() if delivered else None
	check('the sweep recovers it from the completion queue',
	      len(delivered) == 1 and payload == want, _diff(payload, want))
	same('and leaves it queued as well', execution, reference, 'read_cq',
	     cid=cid, reservation_id=RID)

	late = dict(info, num_qubits=4, delay_event=1.5)
	task = same('a run whose event comes late', execution, reference,
		    'async_run', late, reservation_id=RID)
	delivered = take(events, 2, timeout=3)
	check('reaches the queue once, from the sweep, not again from it',
	      len(delivered) == 1 and
	      delivered[0].get_event().get('poll_operation') == 'peek_cq',
	      [d.get_event().get('cid') for d in delivered])
	same('and its completion is read', execution, reference, 'read_cq',
	     cid=task['cid'] if task else None, reservation_id=RID)
	return events


def dead_sink_checks(execution, reference, events, record):
	"""A sink that goes: the QPM's put to it fails and the QPM drops the
	registration, as v1 did, while the live sink hears every event."""
	rt = _state.runtime()
	dead = defw2.EventSink(rt, provider_id=9)
	target = dead.target('dead')
	dead.close()
	with defw2.QPM.from_record(rt, record) as qpm:
		decision = qpm.register_event_notification(
			target, type='1', reservation_id=RID)
	check('a typed caller registers a sink that is gone',
	      decision.decision == 'accepted' and
	      decision.extra.get('registration_count') == 2, decision)

	info = {'qasm': QASM, 'num_qubits': 2}
	for _ in range(5):
		execution.async_run(info, reservation_id=RID)
		reference.async_run(info, reservation_id=RID)
		time.sleep(0.2)
	check('the live sink hears every completion',
	      len(take(events, 5)) == 5)

	other = BaseEventAPI()
	other.register_external()
	registration = execution.register_event_notification(
		defw.me.my_endpoint(), 1, other.class_id())
	check('and the QPM dropped the dead one when its put failed',
	      registration.get('registration_count') == 2, registration)
	other.unregister_external()
	events.unregister_external()


def directory_event_checks(dirsvc):
	"""v1's directory events, from the v2 directory."""
	events = BaseEventAPI()
	events.register_external()
	ids = [dirsvc.register_event_notification(
		defw.me.my_endpoint(), kind, events.class_id(),
		filters={'service_type': 'test.compat.events'})
	       for kind in ('SERVICE_CONNECTED', 'SERVICE_DISCONNECTED')]
	check('the directory takes a registration for each event',
	      len(set(ids)) == 2)

	rt = _state.runtime()
	directory_id = _state.directory().v2.runtime_id()
	host = defw2.ServiceHost(rt, 'svc:compat-events', 'test.compat.events',
				 apis=[(defw2.API_ECHO, 11)])
	host.register()
	delivered = take(events, 1)
	event = delivered[0] if delivered else {}
	record = event.get('service_record') or {}
	check('a service registering is v1\'s SERVICE_CONNECTED',
	      event.get('event') == 'SERVICE_CONNECTED' and
	      event.get('service_id') == 'svc:compat-events' and
	      event.get('service_type') == 'test.compat.events' and
	      event.get('runtime_id') == rt.runtime_id and
	      event.get('peer_handle') == rt.runtime_id and
	      event.get('directory_runtime_id') == directory_id, event)
	check('with the v1 record a resolve would give',
	      record.get('service_id') == 'svc:compat-events' and
	      record.get('state') == 'UP' and record.get('generation') == 1 and
	      (record.get('endpoint') or {}).get('address') == host.address,
	      record)

	host.close()
	delivered = take(events, 1)
	event = delivered[0] if delivered else {}
	check('and deregistering is SERVICE_DISCONNECTED, with its reason',
	      event.get('event') == 'SERVICE_DISCONNECTED' and
	      event.get('reason') == 'deregistered' and
	      event.get('service_id') == 'svc:compat-events' and
	      'service_record' not in event, event)
	check('unregistering a directory event ends it, once',
	      dirsvc.unregister_event_notification(ids[0]) and
	      dirsvc.unregister_event_notification(ids[1]) and
	      not dirsvc.unregister_event_notification(ids[0]))
	events.unregister_external()


def peer_checks():
	"""The directory stays, so a peer event listener hears nothing."""
	import defw_workers
	heard = []
	defw_workers.add_peer_event_listener(heard.append)
	time.sleep(1)
	defw_workers.remove_peer_event_listener(heard.append)
	check('a peer event listener hears nothing while the directory stays',
	      heard == [], heard)


def typed_checks(record, reference):
	"""A v2 caller of the same v1 QPM, through the typed API."""
	rt = _state.runtime()
	with defw2.Directory(rt, rt.dirsvc) as v2:
		found = v2.resolve(service_type='qfw.qpm')
	with defw2.QPM.from_record(rt, found[0]) as qpm:
		task = qpm.async_run(QASM, num_qubits=20, return_statevector=True,
				     reservation_id=RID)
		reference.async_run({'qasm': QASM, 'num_qubits': 20,
				     'return_statevector': True},
				    reservation_id=RID)
		check('a v2 caller gets typed fields from a v1 QPM',
		      task.outcome == 'ACCEPTED' and
		      task.cid and task.cid.startswith('cid-') and
		      task.qtask_id > 0 and task.reservation_id == RID and
		      task.extra['echo']['info']['qasm'] == QASM, task)
		small = qpm.read_cq(cid=task.cid, result=1024,
				    reservation_id=RID)
		check('read_cq into too small a buffer says the size',
		      small.completion_ready and
		      not small.statevector_delivered and
		      small.statevector.nbytes == 16 << 20, small)
		check('and leaves it queued',
		      qpm.peek_cq(cid=task.cid,
				  reservation_id=RID).completion_ready)
		whole = (numpy.empty(1 << 20, dtype=numpy.complex128)
			 if numpy is not None else bytearray(16 << 20))
		done = qpm.read_cq(cid=task.cid, result=whole,
				   reservation_id=RID)
		data = done.statevector_data
		if numpy is not None and data is not None:
			ok = bool((data == numpy.arange(1 << 20) * (1 - 1j)).all())
		elif data is not None:
			ok = all(struct.unpack_from('<dd', data, 16 * k) == (k, -k)
				 for k in range(0, 1 << 20, 997))
		else:
			ok = False
		check('the retry gets all 16 MiB, raw, in the buffer lent',
		      done.statevector_delivered and ok and
		      done.statevector.dtype == defw2.DTYPE['c128'])
		reference.read_cq(cid=task.cid, reservation_id=RID)


def main():
	try:
		import defw_agent  # noqa: F401
		refused = None
	except ImportError as error:
		refused = str(error)
	check('a v1 module with no v2 counterpart refuses to import',
	      refused is not None and 'no v2 counterpart' in refused, refused)

	dirsvc = defw_get_directory_service()
	found = resolve(dirsvc, service_type='qfw.qpm', binding_name='execution')
	check('the v1 QPM is in the directory, as v1 records it', len(found) == 1,
	      found)
	if not found:
		return 1
	record = found[0]['service_record']
	directory_checks(dirsvc, record)

	apis = {}
	for name in ('control', 'admission', 'execution', 'admission-policy',
		     'scheduler', 'telemetry'):
		apis[name] = defw.connect_to_binding(
			resolve(dirsvc, service_type='qfw.qpm',
				binding_name=name)[0])
	check('connect_to_binding gives the v1 API classes',
	      [type(api).__name__ for api in apis.values()] ==
	      ['QPMControl', 'QPMAdmissionControl', 'QPMExecution',
	       'QPMAdmissionPolicyConfig', 'QPMSchedulerControl',
	       'QPMTelemetry'])
	binding_checks(found[0])

	svc_qpm.initialized = True
	reference = svc_qpm.QPM()
	control_checks(apis['control'], reference)
	admission_checks(apis['admission'], reference)
	execution_checks(apis['execution'], reference)
	document_checks(apis, reference)
	typed_checks(record, reference)
	events = event_checks(apis['execution'], reference)
	rt = _state.runtime()
	with defw2.Directory(rt, rt.dirsvc) as v2:
		v2_record = v2.resolve(service_type='qfw.qpm')[0]
	dead_sink_checks(apis['execution'], reference, events, v2_record)
	directory_event_checks(dirsvc)
	peer_checks()

	print('COMPAT CLIENT ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main())
