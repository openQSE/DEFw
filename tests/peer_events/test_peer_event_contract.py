#!/usr/bin/env python3

import importlib
import threading
import time
import uuid


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def wait_for_peer_event(defw_workers, peer_handle):
	deadline = time.time() + 2
	while time.time() < deadline:
		for event in defw_workers.get_peer_events():
			if event.get('peer_handle') == peer_handle:
				return event
		time.sleep(0.02)
	raise AssertionError("peer lifecycle event was not dispatched")


def wait_for_peer_callability(defw_peers, peer_handle, callable_state):
	deadline = time.time() + 2
	while time.time() < deadline:
		peer = defw_peers.get_peer(peer_handle)
		if peer and peer.get('callable') == callable_state:
			return peer
		time.sleep(0.02)
	raise AssertionError("peer callability did not reach expected state")


def wait_for_listener_event(events, peer_handle, event_type):
	deadline = time.time() + 2
	while time.time() < deadline:
		for event in events:
			if event.get('peer_handle') == peer_handle and \
			   event.get('event_type') == event_type:
				return event
		time.sleep(0.02)
	raise AssertionError("peer lifecycle listener was not notified")


def main():
	cdefw_agent = importlib.import_module("cdefw_agent")
	expect(cdefw_agent.DEFW_PEER_READY == 1,
	       "DEFW_PEER_READY constant changed")
	expect(cdefw_agent.defw_peer_event_type2str(
		cdefw_agent.DEFW_PEER_LOST) == "PEER_LOST",
	       "peer event string conversion failed")

	defw_workers = importlib.import_module("defw_workers")
	defw_peers = importlib.import_module("defw_peers")
	defw_agent = importlib.import_module("defw_agent")
	defw = importlib.import_module("defw")
	unknown_peer_handle = f"peer-{uuid.uuid4()}"
	unknown_runtime_id = f"runtime-{uuid.uuid4()}"
	unknown_event = {
		'event_type': 'PEER_READY',
		'peer_handle': unknown_peer_handle,
		'remote_runtime_id': '',
		'is_self': False,
		'transport_context': 'defw-tcp',
		'endpoint': {
			'address': '127.0.0.1',
			'listen_port': 8281,
			'node_name': 'peer-unknown-runtime',
			'hostname': 'localhost',
			'pid': 122,
		},
		'reason': 'unit-test',
		'timestamp': time.time(),
	}
	defw_workers.put_peer_event(unknown_event)
	wait_for_peer_callability(defw_peers, unknown_peer_handle, True)
	unknown_target = defw_agent.Endpoint(
		'127.0.0.1', 0, 8281, 122, 'peer-unknown-runtime',
		'localhost', cdefw_agent.EN_DEFW_SERVICE, unknown_runtime_id)
	expect(defw.get_agent(unknown_target) is None,
	       "runtime-scoped lookup matched a peer with unknown runtime id")

	peer_handle = f"peer-{uuid.uuid4()}"
	runtime_id = f"runtime-{uuid.uuid4()}"
	listener_events = []
	listener = defw_workers.add_peer_event_listener(
		lambda peer_event: listener_events.append(peer_event))
	event = {
		'event_type': 'PEER_READY',
		'peer_handle': peer_handle,
		'remote_runtime_id': runtime_id,
		'is_self': False,
		'transport_context': 'defw-tcp',
		'endpoint': {
			'address': '127.0.0.1',
			'listen_port': 8282,
			'node_name': 'peer-test',
			'hostname': 'localhost',
			'pid': 123,
		},
		'reason': 'unit-test',
		'timestamp': time.time(),
	}
	defw_workers.put_peer_event(event)
	dispatched = wait_for_peer_event(defw_workers, peer_handle)
	listener_ready = wait_for_listener_event(
		listener_events, peer_handle, 'PEER_READY')
	expect(dispatched['event_type'] == 'PEER_READY',
	       "wrong peer lifecycle event type")
	expect(listener_ready is not event,
	       "peer lifecycle listener received mutable internal event state")
	expect(dispatched['endpoint']['hostname'] == 'localhost',
	       "endpoint metadata was not preserved")
	ready_peer = wait_for_peer_callability(defw_peers, peer_handle, True)
	target = defw_agent.Endpoint('127.0.0.1', 0, 8282, 123, 'peer-test',
				    'localhost', cdefw_agent.EN_DEFW_SERVICE,
				    runtime_id)
	for name in ('active_service_agents', 'service_agents',
		     'active_client_agents', 'client_agents'):
		expect(not hasattr(defw, name),
		       f"legacy agent view {name} is still exported")
	agent = defw.get_agent(target)
	expect(agent is not None,
	       "get_agent did not resolve a callable peer table record")
	expect(agent.get_blk_uuid() == ready_peer['peer_handle'],
	       "get_agent did not use the peer table handle")
	lost = dict(event)
	lost['event_type'] = 'PEER_LOST'
	lost['reason'] = 'socket-close'
	lost['timestamp'] = time.time() + 1
	defw_workers.put_peer_event(lost)
	wait_for_peer_callability(defw_peers, peer_handle, False)
	wait_for_listener_event(listener_events, peer_handle, 'PEER_LOST')
	defw_workers.remove_peer_event_listener(listener)
	expect(defw.get_agent(target) is None,
	       "get_agent returned a peer after PEER_LOST")

	# Directory peer processing may synchronously notify remote subscribers.
	# It must not run on the sole worker that dispatches the corresponding RPC
	# response.
	defw_directory = importlib.import_module("defw_directory")
	original_apply_peer_event = defw_directory.apply_peer_event
	entered = threading.Event()
	release = threading.Event()

	def blocking_apply_peer_event(_event):
		entered.set()
		release.wait(timeout=2)

	defw_directory.apply_peer_event = blocking_apply_peer_event
	async_peer_handle = f"peer-{uuid.uuid4()}"
	async_event = dict(event)
	async_event.update({
		'peer_handle': async_peer_handle,
		'remote_runtime_id': f"runtime-{uuid.uuid4()}",
		'timestamp': time.time() + 2,
	})
	try:
		started = time.monotonic()
		defw_workers.worker_thread.handle_peer_event(async_event)
		elapsed = time.monotonic() - started
		expect(elapsed < 0.5,
		       "directory lifecycle processing blocked the DEFw worker")
		expect(entered.wait(timeout=1),
		       "directory lifecycle processing was not dispatched")
	finally:
		release.set()
		defw_directory.apply_peer_event = original_apply_peer_event

	# A late disconnect from an old directory connection must not clear the
	# proxy installed for its replacement.
	replacement_dirsvc = object()
	defw.dirsvc = replacement_dirsvc
	defw_workers.dirsvc_binding_runtime_id = "directory-new"
	defw_workers.dirsvc_binding_peer_handle = "peer-new"
	stale_directory_event = {
		'event_type': 'PEER_LOST',
		'peer_handle': 'peer-old',
		'remote_runtime_id': 'directory-old',
		'node_type': cdefw_agent.EN_DEFW_DIRSVC,
	}
	defw_workers.worker_thread.clear_dirsvc_peer(stale_directory_event)
	expect(defw.dirsvc is replacement_dirsvc,
	       "stale directory disconnect cleared the replacement proxy")
	current_directory_event = dict(stale_directory_event)
	current_directory_event.update({
		'peer_handle': 'peer-new',
		'remote_runtime_id': 'directory-new',
	})
	defw_workers.worker_thread.clear_dirsvc_peer(current_directory_event)
	expect(defw.dirsvc is None,
	       "current directory disconnect retained its proxy")


if __name__ == "__main__":
	main()
