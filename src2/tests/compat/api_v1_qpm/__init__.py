"""v1 API classes for the fake QPM, as QFw's service-apis declare theirs.

Each declares v1's signatures with empty bodies, and v1's BaseRemote sent
a call to any of them to the remote object. They are not changed for v2.
"""

from defw_remote import BaseRemote


class QPMRemoteBase(BaseRemote):
	pass


class QPMExecution(QPMRemoteBase):
	def delete_circuit(self, cid, reservation_id=None, token=None):
		pass

	def sync_run(self, info, reservation_id=None, token=None, timeout=None,
		     cancel_on_timeout=False):
		pass

	def async_run(self, info, reservation_id=None, token=None,
		      timeout=None, cancel_on_timeout=False):
		pass

	def read_cq(self, cid=None, reservation_id=None, token=None):
		pass

	def peek_cq(self, cid=None, reservation_id=None, token=None):
		pass

	def register_event_notification(self, ep, evtype, class_id,
					token=None, reservation_id=None,
					filters=None):
		pass

	def cancel_task(self, cid=None, reservation_id=None, token=None,
			reason=None, qtask_id=None):
		pass

	def task_status(self, cid=None, reservation_id=None, token=None,
			qtask_id=None):
		pass

	def get_device_profile(self, token=None, device_id=None):
		pass


class QPMAdmissionControl(QPMRemoteBase):
	def reserve(self, token=None, request=None):
		pass

	def renew(self, token=None, reservation_id=None, request=None):
		pass

	def release(self, token=None, reservation_id=None, reason=None):
		pass

	def cancel(self, token=None, reservation_id=None, reason=None):
		pass

	def get_reservation(self, token=None, reservation_id=None):
		pass


class QPMControl(QPMRemoteBase):
	def is_ready(self, token=None):
		pass

	def get_service_status(self, token=None):
		pass

	def test(self, token=None):
		pass


class QPMAdmissionPolicyConfig(QPMRemoteBase):
	def get_device_profile(self, token=None, device_id=None):
		pass

	def set_admission_policy(self, token=None, device_id=None,
				 policy=None):
		pass


class QPMSchedulerControl(QPMRemoteBase):
	def get_scheduler_status(self, token=None, device_id=None):
		pass


class QPMTelemetry(QPMRemoteBase):
	def get_backend_info(self, lib=None, token=None):
		pass

	def get_device_info(self, lib=None, token=None):
		pass

	def capability_map(self, token=None):
		pass
