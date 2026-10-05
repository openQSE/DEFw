#!/usr/bin/env python3
"""Does a Python QPM answer like a C one, from both languages?

Three things, each in processes of their own over na+sm.

The language matrix. The C fake QPM is served by defw2_qpm_smoke --serve
and the Python fake by defw2_qpm_fake.py. The C checks (defw2_qpm_smoke
--remote) and the Python checks (defw2_qpm_client_check.py) run against
each. The C client against the C service is defw2_qpm_smoke's own test, so
all four pairings pass.

Control is never stuck behind execution. A Python QPM with one slow
execution worker gets a backlog of async_run calls from eight threads,
while another thread times is_ready. Each API has its own provider and
queue, so is_ready should not wait for any of them.

The directory. A Python QPM registers itself, a client resolves it and
calls it through the record's bindings, and closing the service takes it
out of the directory again. A Python sink subscribed to the C directory
hears of both, with the record each time.

	defw2_python_qpm_smoke.py --qpm-smoke PATH --dirsvc PATH
"""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time

import defw2

HERE = os.path.dirname(os.path.abspath(__file__))
QASM = 'OPENQASM 2.0;\nqreg q[1];\nh q[0];\n'

failures = []


def check(what, ok):
	print('{:<58} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)


class Server:
	"""A process that prints its address and serves until stdin closes."""

	def __init__(self, name, command, env=None):
		self.name = name
		self.log = open(os.path.join(os.getcwd(),
					     'qpm-{}.log'.format(name)), 'w')
		self.process = subprocess.Popen(
			command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
			stderr=self.log, text=True, env=env)
		self.address = self.process.stdout.readline().strip()

	def stop(self):
		if self.process.stdin is not None:
			self.process.stdin.close()
		try:
			code = self.process.wait(timeout=30)
		except subprocess.TimeoutExpired:
			self.process.kill()
			code = self.process.wait()
		self.log.close()
		return code


def run(command, env=None):
	result = subprocess.run(command, capture_output=True, text=True,
				timeout=300, env=env)
	failed = [line for line in result.stdout.splitlines()
		  if 'FAILED' in line]
	if result.returncode != 0:
		sys.stdout.write(result.stdout[-2000:])
		sys.stdout.write(result.stderr[-2000:])
	return result.returncode == 0 and not failed


def python_env():
	env = dict(os.environ)
	env['PYTHONPATH'] = os.pathsep.join(
		[HERE] + [p for p in env.get('PYTHONPATH', '').split(os.pathsep)
			  if p])
	return env


def fake_command(*extra):
	return [sys.executable, os.path.join(HERE, 'defw2_qpm_fake.py')] + \
		list(extra)


def client_check_command(address):
	return [sys.executable, os.path.join(HERE, 'defw2_qpm_client_check.py'),
		address]


def matrix(qpm_smoke):
	env = python_env()

	server = Server('c-fake', [qpm_smoke, '--serve'], env)
	check('the C fake serves', server.address.startswith('na+sm'))
	check('the Python client passes against the C QPM',
	      run(client_check_command(server.address), env))
	check('the C fake stops cleanly', server.stop() == 0)

	server = Server('py-fake', fake_command(), env)
	check('the Python fake serves', server.address.startswith('na+sm'))
	check('the C client passes against the Python QPM',
	      run([qpm_smoke, '--remote', server.address], env))
	check('the Python client passes against the Python QPM',
	      run(client_check_command(server.address), env))
	check('the Python fake stops cleanly', server.stop() == 0)


def control_not_behind_execution(runtime):
	slow_ms = 100
	server = Server('backlog', fake_command(
		'--slow-ms', str(slow_ms), '--execution-workers', '1'),
		python_env())
	qpm = defw2.QPM(runtime, server.address)
	latencies = []
	errors = []
	running = threading.Event()
	running.set()

	def submit():
		try:
			for _ in range(3):
				qpm.async_run(QASM, num_qubits=1, num_shots=8,
					      reservation_id=7001)
		except Exception as error:  # noqa: BLE001
			errors.append(repr(error))

	def watch():
		while running.is_set():
			start = time.monotonic()
			try:
				qpm.is_ready()
			except Exception as error:  # noqa: BLE001
				errors.append(repr(error))
			latencies.append(time.monotonic() - start)
			time.sleep(0.02)

	watcher = threading.Thread(target=watch)
	submitters = [threading.Thread(target=submit) for _ in range(8)]
	started = time.monotonic()
	watcher.start()
	for thread in submitters:
		thread.start()
	for thread in submitters:
		thread.join(120)
	backlog = time.monotonic() - started
	running.clear()
	watcher.join(30)

	latencies.sort()
	worst = latencies[-1] if latencies else float('inf')
	print('    {} async_run took {:.2f} s through one worker; is_ready '
	      'answered {} times, slowest {:.1f} ms, median {:.2f} ms'.format(
		      24, backlog, len(latencies), worst * 1000,
		      latencies[len(latencies) // 2] * 1000 if latencies else 0))
	check('24 slow async_run calls all completed', not errors)
	check('they really did queue behind one worker',
	      backlog >= 24 * slow_ms / 1000.0 * 0.9)
	check('is_ready answered all through the backlog',
	      len(latencies) >= 20)
	check('and never waited behind async_run',
	      worst < slow_ms / 1000.0)
	qpm.close()
	server.stop()


def next_change(sink, expect_type):
	"""The next directory event, when it is the change expected."""
	event = sink.next(timeout_ms=10000)
	if event is None or event.type != expect_type or event.tag != 'qpms':
		print('    took {!r}'.format(event))
		return None
	return event.payload


def directory(runtime, dirsvc_binary):
	env = python_env()
	dirsvc = Server('dirsvc', [dirsvc_binary], env)
	check('the directory serves', dirsvc.address.startswith('na+sm'))
	env['DEFW2_DIRSVC'] = dirsvc.address

	# A runtime that listens, for the sink the directory tells.
	listener = defw2.Runtime(role='server', node_name='py-dir-listener')
	sink = defw2.EventSink(listener)
	watching = defw2.Directory(listener, dirsvc.address)
	subscription = watching.subscribe(sink, service_type='qfw.qpm',
					  tag='qpms')
	check('a Python sink subscribes to the C directory',
	      subscription > 0)

	fake = Server('registered-fake', fake_command('--register'), env)
	event = sink.next(timeout_ms=10000)
	change = event.payload if event is not None and \
		event.type == defw2.SERVICE_CONNECTED else None
	check('the event comes from the runtime the directory says it is',
	      event is not None and event.source == watching.runtime_id())
	record = (change or {}).get('record', {})
	check('and hears the QPM register, with its record',
	      change is not None and change['connected'] and
	      change['reason'] == 'registered' and
	      record.get('service_id') == 'qpm:fake:fake-20q-py' and
	      record.get('state') == 'UP' and
	      record.get('address') == fake.address and
	      len(record.get('bindings', [])) == 6 and
	      record.get('properties') == {'provider': 'fake-py',
					   'num_qubits': '20'})

	with defw2.Directory(runtime, dirsvc.address) as found:
		records = []
		deadline = time.monotonic() + 10
		while not records and time.monotonic() < deadline:
			records = found.resolve(service_type='qfw.qpm')
			if not records:
				time.sleep(0.05)
		check('a Python QPM registers itself', len(records) == 1)
		record = records[0] if records else {}
		bindings = {b['binding_name']: b
			    for b in record.get('bindings', [])}
		check('with a binding per API, on its own provider',
		      {name: (b['api_id'], b['provider_id'], b['api_version'])
		       for name, b in bindings.items()} == {
			      'control': ('qfw.qpm.control', 2, 1),
			      'admission': ('qfw.qpm.admission', 3, 1),
			      'execution': ('qfw.qpm.execution', 4, 1),
			      'admission-policy':
				      ('qfw.qpm.admission-policy', 6, 1),
			      'scheduler': ('qfw.qpm.scheduler', 7, 1),
			      'telemetry': ('qfw.qpm.telemetry', 8, 1)})
		check('and its selector and properties',
		      record.get('selector', {}).get('name') == 'fake-20q-py'
		      and record.get('properties') == {
			      'provider': 'fake-py', 'num_qubits': '20'}
		      and record.get('state') == 'UP')

		chosen = found.resolve(service_type='qfw.qpm',
				       binding_name='execution',
				       resource='FAKE-20q')
		check('resolving one binding selects it',
		      len(chosen) == 1 and
		      chosen[0]['binding']['provider_id'] == 4)
		traceparent = ('00-4bf92f3577b34da6a3ce929d0e0e4736-'
			       '00f067aa0ba902b7-01')
		traced = found.resolve(service_type='qfw.qpm',
				       traceparent=traceparent)
		check('a resolve can carry the caller\'s trace context',
		      traced == records)

		if record:
			with defw2.QPM.from_record(runtime, record) as qpm:
				status = qpm.is_ready(reservation_id=1)
				check('a client calls it through the record',
				      status.ready and
				      status.extra_json == 'rid=1 token=(null)')

		check('the service stops cleanly', fake.stop() == 0)
		change = next_change(sink, defw2.SERVICE_DISCONNECTED)
		record = (change or {}).get('record', {})
		check('the sink hears it deregister',
		      change is not None and not change['connected'] and
		      change['reason'] == 'deregistered' and
		      record.get('state') == 'DEREGISTERED' and
		      record.get('address') is None)
		check('and an unsubscribe ends the subscription',
		      watching.unsubscribe(subscription) and
		      not watching.unsubscribe(subscription))
		check('and is gone from what clients resolve',
		      found.resolve(service_type='qfw.qpm') == [])
		gone = found.query(service_type='qfw.qpm')
		check('though an operator still sees it deregistered',
		      len(gone) == 1 and gone[0]['state'] == 'DEREGISTERED'
		      and gone[0]['address'] is None)

	# A directory that stops answering, as one whose node went would. A
	# question with a limit of its own hears so at that limit, not at the
	# handle's 10 s.
	before = watching.runtime_id()
	dirsvc.process.send_signal(signal.SIGSTOP)
	os.waitpid(dirsvc.process.pid, os.WUNTRACED)
	start = time.monotonic()
	try:
		watching.runtime_id(timeout_ms=300)
		answered = True
	except defw2.DefwError:
		answered = False
	waited = time.monotonic() - start
	dirsvc.process.send_signal(signal.SIGCONT)
	check('a stopped directory fails a question at its own limit',
	      not answered and 0.25 < waited < 3)
	print('    it took {:.2f} s'.format(waited))
	check('and the same runtime answers once it goes on',
	      watching.runtime_id() == before)
	watching.close()
	sink.close()
	listener.close()
	dirsvc.process.terminate()
	dirsvc.process.wait(timeout=30)
	dirsvc.log.close()


class JsonQPM:
	"""A service whose answers carry real JSON in extra."""

	def is_ready(self, request):
		return {'state': 'running', 'ready': True,
			'extra': {'shutdown': None, 'seen': request.extra,
				  'qubits': [1, 2, 3]}}

	def reserve(self, request):
		return {'decision': 'accepted', 'reservation_id': 9,
			'extra': {'workload': request.extra['workload'],
				  'task_class': request.task_class}}


def extra_as_json(runtime):
	"""Dictionaries in, dictionaries out, through extra."""
	server = defw2.Runtime(role='server', node_name='py-json-qpm')
	host = defw2.ServiceHost(server, 'qpm:json', 'qfw.qpm',
				 apis=defw2.QPM_APIS)
	host.start(JsonQPM())
	with defw2.QPM(runtime, host.address) as qpm:
		status = qpm.is_ready()
		check('a dict a service answers with arrives as a dict',
		      status.extra == {'shutdown': None, 'seen': None,
				       'qubits': [1, 2, 3]})
		decision = qpm.reserve(
			task_class={'count': 2, 'shots': 100},
			extra={'workload': {'example': 'json'}})
		check('and a dict a caller sends arrives as one too',
		      decision.reservation_id == 9 and decision.extra == {
			      'workload': {'example': 'json'},
			      'task_class': {
				      'count': 2, 'qubit_count': 0, 'depth': 0,
				      'one_q_gate_count': 0,
				      'two_q_gate_count': 0, 'shots': 100,
				      'measurement_count': 0}})
		try:
			qpm.async_run(QASM)
			raised = None
		except defw2.DefwError as error:
			raised = error
		check('a method the service lacks is not found',
		      raised is not None and raised.category == 'not-found')
	host.close()
	server.close()


def main(argv):
	parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	parser.add_argument('--qpm-smoke', required=True)
	parser.add_argument('--dirsvc', required=True)
	args = parser.parse_args(argv)

	matrix(args.qpm_smoke)
	with defw2.Runtime(node_name='py-qpm-smoke') as runtime:
		control_not_behind_execution(runtime)
		directory(runtime, args.dirsvc)
		extra_as_json(runtime)

	print('PYTHON QPM SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
