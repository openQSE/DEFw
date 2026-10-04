#!/usr/bin/env python3
"""Does v1 QFw code run unchanged on v2 through defw2.compat?

Two parts.

The mapping, in this process. Every kind of value a v1 QPM's dictionaries
hold crosses the typed APIs through defw2.compat._mapping and comes back
the same: answers in both directions, run and reservation requests,
circuits and statevectors, and the values that must stay in extra because
a typed field would change them. And the directory watch, given a
directory that answers as the test says, makes the peer events v1 made.

End to end, in processes of their own over na+sm. The directory runs as
defw2-dirsvc. tests/compat/svc_v1_qpm, a v1 QPM service module written
against v1 alone, is served by defw2-python --serve. defw2_compat_client.py
runs under defw2-python and calls it the way QFw's client code does,
comparing every answer with v1's. Then the service is told to stop, and
must leave cleanly and leave the directory.

	defw2_compat_smoke.py --dirsvc PATH --launcher PATH
"""

import argparse
import base64
import json
import os
import signal
import struct
import subprocess
import sys
import time
import zlib

import defw2
from defw2._qpm import Request
from defw2.compat import _mapping as m

HERE = os.path.dirname(os.path.abspath(__file__))
COMPAT = os.path.join(HERE, 'compat')

failures = []


def check(what, ok, detail=None):
	print('{:<62} {}'.format(what, 'ok' if ok else 'FAILED'))
	if not ok:
		failures.append(what)
		if detail is not None:
			print('    ' + str(detail)[:2000])


# --- the mapping ----------------------------------------------------------


class Answer:
	"""A typed answer as the client binding gives one."""

	def __init__(self, kind, typed):
		for name, _key, _field in m.ANSWER_FIELDS[kind]:
			value = typed.get(name)
			if value is None:
				value = {'str': None, 'u64': 0, 'bool': False}[_field]
			setattr(self, name, value)
		self.extra_json = typed.get('extra')
		self.extra = (None if self.extra_json is None
			      else json.loads(self.extra_json))


def round_trip(kind, answer):
	return m.typed_to_answer(kind, Answer(kind, m.answer_to_typed(kind,
								    answer)))


def run_values(call):
	"""A run call's keywords as the service reads them off the wire."""
	return {
		'circuit': call['circuit'], 'circuit_format':
			call['circuit_format'],
		'num_qubits': call.get('num_qubits', 0),
		'num_shots': call.get('num_shots', 0),
		'compiler': call.get('compiler'),
		'return_statevector': call.get('return_statevector', False),
		'run_timeout_ms': call['run_timeout_ms'],
		'cancel_on_timeout': call['cancel_on_timeout'],
		'reservation_id': call['reservation_id'],
		'token': call['token'], 'result_capacity': 0,
	}


def mapping_checks():
	status = {'state': 'running', 'ready': True, 'initialized': True,
		  'accepting_requests': True, 'provider_ready': False,
		  'active_task_count': 0, 'active_reservation_count': 2,
		  'shutdown': None, 'directory_registered': True}
	typed = m.answer_to_typed('status', status)
	check('typed fields carry only what comes back the same',
	      typed['state'] == 'running' and typed['ready'] is True and
	      typed['active_reservation_count'] == 2 and
	      'provider_ready' not in typed and
	      'active_task_count' not in typed, typed)
	check('a False, a zero and a None stay in extra, and return',
	      round_trip('status', status) == status)

	decision = {'status': 'accepted', 'decision': 1, 'reason_code': 1,
		    'reservation_id': 7001, 'request_id': None, 'message': '',
		    'retry_after_ns': 0, 'estimated_total_ns': 12.5}
	typed = m.answer_to_typed('decision', decision)
	check('a decision\'s status is the typed decision',
	      typed.get('decision') == 'accepted' and
	      typed.get('reservation_id') == 7001)
	check('and an int decision, an empty message and a float return',
	      round_trip('decision', decision) == decision)

	task = {'outcome': 'UNKNOWN', 'lifecycle_state': 'unknown',
		'cid': 'nope', 'reason': None, 'message': None,
		'qtask_id': 2 ** 64, 'tuple': (1, 2)}
	back = round_trip('task', task)
	check('a None kept, an out-of-range int kept, a tuple as a list',
	      back == dict(task, tuple=[1, 2]), back)
	check('an answer that is not a dict comes back as itself',
	      round_trip('task', 'cid-0001') == 'cid-0001' and
	      round_trip('status', None) is None)
	try:
		m.answer_to_typed('task', {'outcome': 'x', 'bad': {1, 2}})
		raised = False
	except m.MappingError:
		raised = True
	check('a value JSON cannot carry fails, rather than being dropped',
	      raised)

	info = {'qasm': 'OPENQASM 2.0;', 'num_qubits': 3, 'num_shots': 0,
		'compiler': 'staq', 'return_statevector': False,
		'qubit_mapping': {'0': 'QB1'}}
	call = m.run_request(info, reservation_id=7001, token='t',
			     timeout=30, cancel_on_timeout=True)
	request = Request('async_run', run_values(call), call['extra'])
	check('a legacy qasm run moves the circuit and returns as qasm',
	      call['circuit_format'] == 'openqasm2' and
	      m.info_from_run(request) == info and
	      m.timeout_seconds(request.run_timeout_ms) == 30)

	qpy = bytes(range(256)) * 8
	info = {'circuit': {'format': 'qpy', 'data':
			    base64.b64encode(qpy).decode('ascii')},
		'num_shots': 10}
	call = m.run_request(info, reservation_id='7001', timeout=0.25)
	request = Request('async_run', run_values(call), call['extra'])
	check('a QPY circuit travels as its bytes and returns as base64',
	      call['circuit'] == qpy and call['reservation_id'] == 7001 and
	      m.info_from_run(request) == info and
	      m.timeout_seconds(request.run_timeout_ms) == 0.25)
	odd = {'circuit': {'format': 'qpy', 'data': 'QQ'}, 'num_shots': 10}
	call = m.run_request(odd, reservation_id=1)
	request = Request('async_run', run_values(call), call['extra'])
	check('non-canonical base64 stays in extra, exactly as it was',
	      call['circuit'] == b'' and m.info_from_run(request) == odd)
	native = Request('async_run', run_values({
		'circuit': b'OPENQASM 2.0;', 'circuit_format': 'openqasm2',
		'run_timeout_ms': None, 'cancel_on_timeout': False,
		'reservation_id': 1, 'token': None, 'num_qubits': 2}), None)
	check('a v2 caller\'s run reads as a v1 info dict',
	      m.info_from_run(native) == {'qasm': 'OPENQASM 2.0;',
					  'num_qubits': 2})

	for bad, why in ((dict(reservation_id=0), 'a reservation of 0'),
			 (dict(token=7), 'a token that is not a string'),
			 (dict(timeout=-1), 'a negative timeout')):
		try:
			m.run_request({'qasm': 'x'}, **bad)
			raised = False
		except m.MappingError:
			raised = True
		check('{} is refused before it is sent'.format(why), raised)

	payload = {'type': 'statevector', 'encoding': 'base64+zlib',
		   'dtype': 'complex128', 'byte_order': 'little',
		   'num_amplitudes': 4, 'raw_size_bytes': 64}
	raw = b''.join(struct.pack('<dd', k, -k) for k in range(4))
	compressed = zlib.compress(raw)
	payload.update(compressed_size_bytes=len(compressed),
		       data=base64.b64encode(compressed).decode('ascii'))
	payload['base64_size_bytes'] = len(payload['data'])
	answer = {'outcome': 'COMPLETED', 'result': {
		'counts': {'00': 1}, 'statevector': payload}}
	taken, data = m.take_statevector(answer)
	check('a statevector moves out as its raw bytes',
	      data == raw and answer['result']['statevector'] is payload and
	      taken['result']['statevector']['data'] is None)
	stub = m.find_stub(taken)
	m.restore_statevector(stub, data)
	check('and goes back in, the payload the service made',
	      taken == answer, taken)

	described, nbytes = m.describe_statevector(answer)
	check('an event describes it by its size, with the same stub',
	      nbytes == 64 and m.find_stub(described) is not None and
	      answer['result']['statevector'] is payload)
	broken = dict(answer, result=dict(answer['result'], statevector=dict(
		payload, data='not base64')))
	check('and decodes nothing, taking the size the payload gives',
	      m.describe_statevector(broken)[1] == 64)


def address_checks():
	"""Where a compat client listens, by what its environment asks."""
	import socket
	from defw2.compat import _state

	saved = {name: os.environ.get(name)
		 for name in ('DEFW2_ADDRESS', 'DEFW_LISTEN_PORT')}
	seen = {}
	try:
		for name, address, port in (
				('unset', None, None),
				('ofi+tcp alone', 'ofi+tcp://', None),
				('a host', 'ofi+tcp://somewhere', None),
				('a port', 'ofi+tcp://', '4242'),
				('another provider', 'na+sm://', None)):
			for variable, value in (('DEFW2_ADDRESS', address),
						('DEFW_LISTEN_PORT', port)):
				if value is None:
					os.environ.pop(variable, None)
				else:
					os.environ[variable] = value
			seen[name] = _state._host_address()
	finally:
		for name, value in saved.items():
			if value is None:
				os.environ.pop(name, None)
			else:
				os.environ[name] = value
	mine = 'ofi+tcp://' + socket.gethostname()
	check('a client asked for nothing in particular listens on its host',
	      seen['unset'] == mine and seen['ofi+tcp alone'] == mine, seen)
	check('and one asked for a host, a port or a provider gets that',
	      seen['a host'] is None and seen['a port'] is None and
	      seen['another provider'] is None, seen)


def watch_checks():
	"""The directory watch, against a directory that answers as told."""
	from defw2._defw2 import lib
	from defw2.compat._events import DIRSVC, DirectoryWatch

	answers = []
	emitted = []
	lost = []

	def ask():
		answer = answers.pop(0)
		if isinstance(answer, Exception):
			raise answer
		return answer

	def kinds():
		taken = [(e['event_type'], e['remote_runtime_id'], e['reason'])
			 for e in emitted]
		emitted.clear()
		return taken

	down = defw2.DefwError(lib.DEFW2_ERR_TRANSPORT, 'transport', 'gone')
	watch = DirectoryWatch(ask, emitted.append, lost.append)
	answers[:] = ['dir-1', 'dir-1']
	watch.check()
	watch.check()
	check('the directory\'s first answer is a start, not a change',
	      kinds() == [] and watch.up and lost == [])
	answers[:] = [down, down]
	watch.check()
	first = list(emitted)
	watch.check()
	check('a directory that stops answering is PEER_LOST, once',
	      kinds() == [('PEER_LOST', 'dir-1', 'unreachable')] and
	      lost == ['dir-1'] and first[0]['node_type'] == DIRSVC and
	      first[0]['peer_handle'] == 'dir-1')
	answers[:] = ['dir-1']
	watch.check()
	check('and PEER_READY when it answers again',
	      kinds() == [('PEER_READY', 'dir-1', 'reconnected')])
	answers[:] = ['dir-2']
	watch.check()
	check('a directory that restarted unseen is lost, then ready',
	      kinds() == [('PEER_LOST', 'dir-1', 'restarted'),
			  ('PEER_READY', 'dir-2', 'reconnected')] and
	      lost == ['dir-1', 'dir-1'] and watch.runtime_id == 'dir-2')


# --- end to end ------------------------------------------------------------


class Process:
	def __init__(self, name, command, env, address=False):
		self.name = name
		self.log = open(os.path.join(os.getcwd(),
					     'compat-{}.log'.format(name)), 'w')
		self.process = subprocess.Popen(
			command, stdin=subprocess.PIPE,
			stdout=subprocess.PIPE if address else self.log,
			stderr=self.log, text=True, env=env)
		self.address = (self.process.stdout.readline().strip()
				if address else None)

	def stop(self, sig=None, timeout=30):
		if sig is not None:
			self.process.send_signal(sig)
		elif self.process.stdin is not None:
			self.process.stdin.close()
		try:
			code = self.process.wait(timeout=timeout)
		except subprocess.TimeoutExpired:
			self.process.kill()
			code = self.process.wait()
		self.log.close()
		return code


def environment(dirsvc):
	env = dict(os.environ)
	env['PYTHONPATH'] = os.pathsep.join(
		[COMPAT] + [p for p in env.get('PYTHONPATH', '').split(os.pathsep)
			    if p])
	env.setdefault('DEFW2_PYTHON', sys.executable)
	env.setdefault('DEFW_PATH', os.path.normpath(
		os.path.join(HERE, '..', '..')))
	env['DEFW_LOG_DIR'] = os.path.join(os.getcwd(), 'compat-logs')
	# A sweep and a directory check every fifth of a second, so the
	# client sees what they do while it waits.
	env['DEFW2_COMPAT_SWEEP_MS'] = '200'
	env['DEFW2_COMPAT_DIRSVC_CHECK_MS'] = '200'
	if dirsvc:
		env['DEFW2_DIRSVC'] = dirsvc
	return env


def end_to_end(dirsvc_binary, launcher):
	dirsvc = Process('dirsvc', [dirsvc_binary], environment(None),
			 address=True)
	check('the directory serves', dirsvc.address.startswith('na+sm'))
	env = environment(dirsvc.address)

	service = Process('service', [launcher, '--serve', 'svc_v1_qpm'], env)
	result = subprocess.run(
		[launcher, os.path.join(HERE, 'defw2_compat_client.py')],
		capture_output=True, text=True, timeout=600, env=env)
	sys.stdout.write(result.stdout)
	if result.returncode != 0:
		sys.stdout.write(result.stderr[-4000:])
	check('the v1 client passes against the v1 QPM, on v2',
	      result.returncode == 0 and
	      'COMPAT CLIENT PASSED' in result.stdout)

	start = time.monotonic()
	code = service.stop(signal.SIGTERM)
	check('the served QPM stops cleanly when told to', code == 0, code)
	print('    it took {:.2f} s'.format(time.monotonic() - start))
	with defw2.Runtime(node_name='compat-smoke') as rt:
		with defw2.Directory(rt, dirsvc.address) as directory:
			records = directory.query(service_type='qfw.qpm')
	check('and it left the directory',
	      len(records) == 1 and records[0]['state'] == 'DEREGISTERED',
	      records)
	dirsvc.process.terminate()
	dirsvc.stop()


def main(argv):
	parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
	parser.add_argument('--dirsvc', required=True)
	parser.add_argument('--launcher', required=True)
	args = parser.parse_args(argv)

	mapping_checks()
	address_checks()
	watch_checks()
	end_to_end(args.dirsvc, args.launcher)

	print('COMPAT SMOKE ' + ('FAILED' if failures else 'PASSED'))
	return 1 if failures else 0


if __name__ == '__main__':
	sys.exit(main(sys.argv[1:]))
