#!/usr/bin/env python3

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
		properties=None):
	record = {
		'service_id': 'qpm-iqm-ornl',
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


def main():
	directory = defw_directory.Directory(retention_seconds=0.01)
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
