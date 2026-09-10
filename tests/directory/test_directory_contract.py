#!/usr/bin/env python3

import threading
import time

import defw_directory
from defw_exception import DEFwError


def expect(condition, message):
	if not condition:
		raise AssertionError(message)


def expect_raises(exc_type, func, *args, **kwargs):
	try:
		func(*args, **kwargs)
	except exc_type:
		return
	raise AssertionError(f"{exc_type.__name__} was not raised")


def make_record(runtime_id='runtime-1', peer_handle='peer-1',
		properties=None, service_id='qpm-iqm-ornl'):
	record = {
		'service_id': service_id,
		'service_name': 'IQM QPM',
		'service_type': 'qfw.qpm',
		'runtime_id': runtime_id,
		'peer_handle': peer_handle,
		'endpoint': {
			'address': '127.0.0.1',
			'listen_port': 8095,
			'node_name': 'qpm-iqm',
			'hostname': 'qpm.example',
			'pid': 12345,
		},
		'api_bindings': [
			{
				'binding_name': 'execution',
				'client_module': 'api_qpm_execution',
				'client_class': 'QPMExecution',
				'service_module': 'svc_iqm_qpm.svc_qpm',
				'service_class': 'QPM',
				'version': 1,
			},
			{
				'binding_name': 'telemetry',
				'client_module': 'api_qpm_telemetry',
				'client_class': 'QPMTelemetry',
				'service_module': 'svc_iqm_qpm.svc_qpm',
				'service_class': 'QPM',
				'version': 1,
			},
		],
		'selector': {
			'name': 'IQM-20q',
			'aliases': ['ornl-iqm-20q'],
			'resources': ['IQM-20q'],
		},
	}
	if properties is not None:
		record['properties'] = dict(properties)
	return record


class EventCallback:
	def __init__(self):
		self.events = []

	def put(self, event):
		self.events.append(dict(event))


class BlockingEventCallback:
	def __init__(self):
		self.entered = threading.Event()
		self.release = threading.Event()

	def put(self, _event):
		self.entered.set()
		self.release.wait(timeout=2)


def test_service_event_contract():
	directory = defw_directory.Directory(
		retention_seconds=10,
		runtime_id_provider=lambda: 'directory-runtime-1',
	)
	connected = EventCallback()
	disconnected = EventCallback()
	connected_id = directory.register_event_notification(
		connected,
		defw_directory.SERVICE_CONNECTED,
		'client-runtime-1',
		filters={'service_id': 'qpm-iqm-ornl'},
	)
	disconnected_id = directory.register_event_notification(
		disconnected,
		defw_directory.SERVICE_DISCONNECTED,
		'client-runtime-1',
		filters={'service_type': 'qfw.qpm'},
	)
	expect_raises(
		DEFwError,
		directory.register_event_notification,
		connected,
		'UNKNOWN_EVENT',
		'client-runtime-1',
	)
	expect_raises(
		DEFwError,
		directory.register_event_notification,
		connected,
		defw_directory.SERVICE_CONNECTED,
		'client-runtime-1',
		filters={'provider': 'iqm'},
	)

	registered = directory.register_service(make_record())
	expect(len(connected.events) == 1,
	       'initial registration did not publish a connected event')
	event = connected.events[0]
	expect(event['event'] == defw_directory.SERVICE_CONNECTED,
	       'connected event type is incorrect')
	expect(event['directory_runtime_id'] == 'directory-runtime-1',
	       'connected event lacks directory runtime identity')
	expect(event['runtime_id'] == 'runtime-1',
	       'connected event lacks service runtime identity')
	expect(event['peer_handle'] == 'peer-1',
	       'connected event lacks peer identity')
	expect(event['service_record']['generation'] == 1,
	       'connected event lacks the active service record')

	directory.register_service(make_record())
	expect(len(connected.events) == 1,
	       'duplicate active registration published another event')
	lost_at = time.time()
	directory.apply_peer_event({
		'event_type': 'PEER_LOST',
		'peer_handle': 'peer-1',
		'remote_runtime_id': 'runtime-1',
		'reason': 'socket-close',
		'timestamp': lost_at,
	})
	expect(len(disconnected.events) == 1,
	       'peer loss did not publish a disconnected event')
	event = disconnected.events[0]
	expect(event['event'] == defw_directory.SERVICE_DISCONNECTED,
	       'disconnected event type is incorrect')
	expect(event['reason'] == 'socket-close',
	       'disconnected event lacks the peer-loss reason')

	directory.register_service(make_record(
		runtime_id='runtime-2', peer_handle='peer-2'))
	expect(len(connected.events) == 2,
	       'replacement runtime did not publish a connected event')
	directory.apply_peer_event({
		'event_type': 'PEER_LOST',
		'peer_handle': 'peer-1',
		'remote_runtime_id': 'runtime-1',
		'reason': 'delayed-old-loss',
		'timestamp': lost_at + 2,
	})
	expect(len(disconnected.events) == 1,
	       'old runtime loss invalidated the replacement')

	expect(directory.unregister_event_notification(connected_id),
	       'connected registration was not removed')
	expect(directory.unregister_event_notification(disconnected_id),
	       'disconnected registration was not removed')
	expect(not directory.unregister_event_notification(disconnected_id),
	       'repeated unregistration should be idempotent')


def test_subscriber_cleanup():
	directory = defw_directory.Directory(
		runtime_id_provider=lambda: 'directory-runtime-1')
	callback = EventCallback()
	directory.register_event_notification(
		callback,
		defw_directory.SERVICE_CONNECTED,
		'client-runtime-1',
	)
	directory.apply_peer_event({
		'event_type': 'PEER_LOST',
		'peer_handle': 'client-peer-1',
		'remote_runtime_id': 'client-runtime-1',
		'reason': 'socket-close',
		'timestamp': time.time(),
	})
	directory.register_service(make_record())
	expect(callback.events == [],
	       'lost subscriber retained its callback registration')


def test_callback_delivery_does_not_block_queries():
	directory = defw_directory.Directory(
		runtime_id_provider=lambda: 'directory-runtime-1')
	callback = BlockingEventCallback()
	directory.register_event_notification(
		callback,
		defw_directory.SERVICE_CONNECTED,
		'client-runtime-1',
	)
	thread = threading.Thread(
		target=directory.register_service,
		args=(make_record(),),
	)
	thread.start()
	expect(callback.entered.wait(timeout=1),
	       'blocking callback was not invoked')
	started = time.monotonic()
	matches = directory.resolve_services(service_type='qfw.qpm')
	elapsed = time.monotonic() - started
	callback.release.set()
	thread.join(timeout=1)
	expect(len(matches) == 2,
	       'directory query did not return both API bindings')
	expect(elapsed < 0.5,
	       'remote callback held the directory record lock')
	expect(not thread.is_alive(), 'callback delivery thread did not finish')


def main():
	test_service_event_contract()
	test_subscriber_cleanup()
	test_callback_delivery_does_not_block_queries()
	directory = defw_directory.Directory(
		retention_seconds=0.01,
		runtime_id_provider=lambda: 'directory-runtime-1',
	)
	lifecycle_events = []

	def record_lifecycle(event_type, service_record=None, peer_event=None,
			     reason=None, details=None):
		lifecycle_events.append({
			'event_type': event_type,
			'service_record': dict(service_record or {}),
			'peer_event': dict(peer_event or {}),
			'reason': reason,
			'details': dict(details or {}),
		})

	directory.add_lifecycle_listener(record_lifecycle)
	missing_bindings = make_record()
	del missing_bindings['api_bindings']
	expect_raises(
		DEFwError,
		directory.register_service,
		missing_bindings,
	)
	expect_raises(
		DEFwError,
		directory.register_service,
		make_record(properties={
			'provider': 'iqm',
			'credential_store': '/protected/qpu-users.json',
		}),
	)
	record = directory.register_service(make_record())
	expect(record['generation'] == 1, "new service generation should start at 1")
	expect(record['state'] == defw_directory.STATE_UP, "record should be UP")

	matches = directory.resolve_services(
		service_type='qfw.qpm',
		selector_resource='IQM-20q',
		binding_name='telemetry',
	)
	expect(len(matches) == 1, f"unexpected matches: {matches!r}")
	expect(matches[0]['selected_binding']['client_class'] == 'QPMTelemetry',
	       "selected binding was not returned")

	expect_raises(
		DEFwError,
		directory.register_service,
		make_record(runtime_id='runtime-2', peer_handle='peer-2'),
	)

	directory.apply_peer_event({
		'event_type': 'PEER_LOST',
		'peer_handle': 'peer-1',
		'remote_runtime_id': 'runtime-1',
		'reason': 'heartbeat-timeout',
		'timestamp': time.time(),
	})
	expect(directory.resolve_services(service_type='qfw.qpm') == [],
	       "lost service should not be discoverable")
	inactive = directory.query(include_inactive=True)
	expect(inactive[0]['state'] == defw_directory.STATE_TIMED_OUT,
	       f"wrong inactive state: {inactive!r}")

	directory.apply_peer_event({
		'event_type': 'PEER_READY',
		'peer_handle': 'peer-1',
		'remote_runtime_id': 'runtime-1',
		'timestamp': inactive[0]['last_seen'] - 1,
	})
	stale = directory.query(include_inactive=True)[0]
	expect(stale['state'] == defw_directory.STATE_TIMED_OUT,
	       "stale ready event should not revive a newer loss")

	restarted = directory.register_service(make_record(
		runtime_id='runtime-2',
		peer_handle='peer-2',
	))
	expect(restarted['generation'] == 2,
	       "inactive restart should increment generation")
	deregistered = directory.deregister_service(
		restarted['service_id'],
		restarted['runtime_id'],
		restarted['generation'],
	)
	expect(deregistered['state'] == defw_directory.STATE_DEREGISTERED,
	       "deregister should mark inactive")
	time.sleep(0.02)
	directory.purge_expired()
	expect(directory.query(include_inactive=True) == [],
	       "expired inactive record should be purged")
	event_types = [event['event_type'] for event in lifecycle_events]
	for event_type in (
			'registration',
			'peer-lost',
			'deregistration',
			'retention-purge'):
		expect(event_type in event_types,
		       f"missing lifecycle event {event_type}")
	peer_lost = next(
		event for event in lifecycle_events
		if event['event_type'] == 'peer-lost')
	restart = next(
		event for event in lifecycle_events
		if event['event_type'] == 'registration' and
		event['service_record']['generation'] == 2)
	expect(peer_lost['reason'] == 'heartbeat-timeout',
	       "peer loss reason was not reported")
	expect(restart['details']['previous_generation'] == 1,
	       "restart previous generation was not reported")


if __name__ == "__main__":
	main()
