#!/usr/bin/env python3
"""A connect this process can not complete fails with an error, not a crash.

A plain interpreter that imports the DEFw Python modules never runs
defw_start(), so it has no runtime id and no listener, and before
openQSE/DEFw#53 it had no initialized connection table either: the
outbound connect thread dereferenced NULL and the process died with
SIGSEGV. The connect now fails with EN_DEFW_RC_NOT_STARTED before any
thread starts, and DEFw raises DEFwError in the caller.

With DEFW_TEST_DEFWP set, the same connect is checked inside defwp started
with listen port 0, which has a runtime but no listener and so could never
complete the connection either. It fails with EN_DEFW_RC_NO_LISTENER.

The target is a socket that listens but never answers, so the kernel
completes the TCP handshake: without the checks, the plain interpreter
reaches the connection table and crashes, and defwp waits for a heartbeat
that never comes.
"""

import os
import socket
import subprocess
import sys
import time
import uuid

import defw
import defw_workers
from cdefw_agent import defw_connect_to_client, defw_send_req
from cdefw_global import EN_DEFW_DIRSVC, EN_DEFW_RC_KEEP_DATA, \
	EN_DEFW_RC_NOT_STARTED, EN_DEFW_RC_NO_LISTENER, EN_DEFW_RC_OK, \
	EN_DEFW_RC_UNKNOWN_MESSAGE, defw_rc2str
from defw_exception import DEFwError

ZERO_UUID = str(uuid.UUID(int=0))
# A refused connect returns at once. Anything near the work request's
# own timeout means a thread was started and waited for.
FAST_FAILURE_SEC = 5


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def directory_record(port):
	"""The record QFw's resolver builds for a directory known only by
	host and port: no runtime id, no pid."""
	return {
		'service_record': {
			'service_id': f'dirsvc:127.0.0.1:{port}',
			'service_name': 'DEFwDirSvc',
			'service_type': 'defw.dirsvc',
			'runtime_id': ZERO_UUID,
			'endpoint': {
				'address': '127.0.0.1',
				'listen_port': port,
				'pid': 0,
				'node_name': 'DEFwDirSvc',
				'hostname': '127.0.0.1',
				'runtime_id': ZERO_UUID,
			},
		},
		'selected_binding': {
			'client_module': 'api_dirsvc',
			'client_class': 'DEFwDirSvc',
			'service_module': 'svc_dirsvc.svc_dirsvc',
			'service_class': 'DEFwDirSvc',
		},
	}


def silent_listener():
	sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
	sock.bind(('127.0.0.1', 0))
	sock.listen(8)
	return sock


def expect_connect_error(record, hint):
	start = time.time()
	try:
		defw.connect_to_binding(record)
	except DEFwError as e:
		message = str(e)
	else:
		raise AssertionError("connect_to_binding returned without a peer")
	elapsed = time.time() - start
	expect(hint in message, f"error does not name the cause: {message}")
	expect(elapsed < FAST_FAILURE_SEC,
	       f"connect failed after {elapsed:.1f}s, not at once")
	expect(not defw_workers.worker_thread.req_db,
	       "a failed connect left its work request behind")
	return message


def check_rc_strings():
	expect(defw_rc2str(EN_DEFW_RC_OK) == 'RC_OK', "RC_OK string")
	# the last codes of the table used to fall past the range check
	expect(defw_rc2str(EN_DEFW_RC_KEEP_DATA) == 'RC_KEEP_DATA',
	       defw_rc2str(EN_DEFW_RC_KEEP_DATA))
	expect(defw_rc2str(EN_DEFW_RC_UNKNOWN_MESSAGE) == 'RC_UNKNOWN_MESSAGE',
	       defw_rc2str(EN_DEFW_RC_UNKNOWN_MESSAGE))
	expect(defw_rc2str(EN_DEFW_RC_NOT_STARTED) == 'RC_NOT_STARTED',
	       defw_rc2str(EN_DEFW_RC_NOT_STARTED))
	expect(defw_rc2str(EN_DEFW_RC_NO_LISTENER) == 'RC_NO_LISTENER',
	       defw_rc2str(EN_DEFW_RC_NO_LISTENER))


def check_plain_interpreter(port):
	rc = defw_connect_to_client('127.0.0.1', port, 'DEFwDirSvc',
				    '127.0.0.1', EN_DEFW_DIRSVC,
				    str(uuid.uuid4()), None)
	expect(rc == EN_DEFW_RC_NOT_STARTED,
	       f"connect without a runtime returned {defw_rc2str(rc)}")

	expect_connect_error(directory_record(port), 'defw-python')

	# a send walks the connection table too
	rc = defw_send_req(str(uuid.uuid4()), str(uuid.uuid4()), "rpc: {}\n")
	expect(rc != EN_DEFW_RC_OK, "a send to an unknown agent succeeded")


DEFWP_CHILD = """
import os
import sys
sys.path.insert(0, os.environ['DEFW_TEST_DIR'])
import test_connect_requires_runtime as t
t.expect_connect_error(
	t.directory_record(int(os.environ['DEFW_TEST_TARGET_PORT'])),
	'DEFW_LISTEN_PORT')
print('defwp connect refused: ok', flush=True)
"""


def check_defwp_without_listener(defwp, port):
	env = dict(os.environ)
	env.update({
		# the child imports this file; keep its bytecode out of the tree
		'PYTHONDONTWRITEBYTECODE': '1',
		'DEFW_TEST_DIR': os.path.dirname(os.path.abspath(__file__)),
		'DEFW_TEST_TARGET_PORT': str(port),
		'DEFW_AGENT_NAME': 'connect-no-listener',
		'DEFW_AGENT_TYPE': 'agent',
		'DEFW_SHELL_TYPE': 'cmdline',
		'DEFW_LISTEN_PORT': '0',
		'DEFW_TELNET_PORT': '0',
		'DEFW_PARENT_NAME': 'None',
		'DEFW_PARENT_HOSTNAME': 'None',
		'DEFW_PARENT_ADDR': '0.0.0.0',
		'DEFW_PARENT_PORT': '0',
		'DEFW_DISABLE_DIRSVC': 'yes',
		'DEFW_LOG_LEVEL': 'error',
		'DEFW_EXTERNAL_SERVICES_PATH': '',
		'DEFW_EXTERNAL_SERVICE_APIS_PATH': '',
		'DEFW_EXTERNAL_EXPERIMENTS_PATH': '',
	})
	if os.environ.get('DEFW_TEST_CONFIG_PATH'):
		env['DEFW_CONFIG_PATH'] = os.environ['DEFW_TEST_CONFIG_PATH']
	if os.environ.get('DEFW_TEST_LOG_DIR'):
		env['DEFW_LOG_DIR'] = os.environ['DEFW_TEST_LOG_DIR']
		os.makedirs(env['DEFW_LOG_DIR'], exist_ok=True)

	proc = subprocess.run([defwp, '-c', DEFWP_CHILD], env=env,
			      capture_output=True, text=True, timeout=60)
	output = proc.stdout + proc.stderr
	expect(proc.returncode >= 0,
	       f"defwp died with signal {-proc.returncode}:\n{output}")
	expect('defwp connect refused: ok' in proc.stdout,
	       f"defwp check failed (rc {proc.returncode}):\n{output}")
	print('defwp without a listener fails cleanly: ok')


def main():
	listener = silent_listener()
	port = listener.getsockname()[1]
	try:
		check_rc_strings()
		check_plain_interpreter(port)
		defwp = os.environ.get('DEFW_TEST_DEFWP')
		if defwp:
			check_defwp_without_listener(defwp, port)
	finally:
		listener.close()
	print('connect without a runtime or listener fails cleanly: ok')
	return 0


if __name__ == '__main__':
	sys.exit(main())
