import defw_common_def as common
from defw_exception import DEFwNotFound
import os, threading, uuid

class ConditionalLock:
	def __init__(self, thread_safe=True):
		self.thread_safe = thread_safe
		self._lock = threading.Lock() if thread_safe else None

	def __enter__(self):
		if self.thread_safe:
			self._lock.acquire()
		return self

	def __exit__(self, exc_type, exc_val, exc_tb):
		if self.thread_safe:
			self._lock.release()

	def acquire(self):
		if self.thread_safe:
			return self._lock.acquire()
		return True

	def release(self):
		if self.thread_safe:
			self._lock.release()

	def locked(self):
		if self.thread_safe:
			return self._lock.locked()
		return False

def equalto_noop(criteria, event):
	return True

def recordtime_noop(event):
	pass

class BaseEventAPI:
	def __init__(self, thread_safe=True, *args, **kwargs):
		self.__read_fd, self.__write_fd = os.pipe()
		self.__event_queue = []
		self.__event_lock = ConditionalLock(thread_safe=thread_safe)
		self.__class_id = str(uuid.uuid1())

	def class_id(self):
		return self.__class_id

	def put(self, event):
		with self.__event_lock:
			self.__event_queue.append(event)
			os.write(self.__write_fd, b"x")

	def get(self, criteria=None, equalto=equalto_noop,
			 recordtime=recordtime_noop):
		res = []
		with self.__event_lock:
			i = 0
			for e in self.__event_queue:
				if equalto(criteria, e):
					recordtime(e)
					res.append(e)
					i += 1
			# remove results from queue
			for e in res:
				self.__event_queue.remove(e)
			os.read(self.__read_fd, i)
		return res

	def fileno(self):
		return self.__read_fd

	def register_external(self):
		try:
			common.get_class_from_db(self.__class_id)
		except DEFwNotFound:
			common.add_to_class_db(self, self.__class_id)

	def unregister_external(self):
		common.del_entry_from_class_db(self.__class_id)
