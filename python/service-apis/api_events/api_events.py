from defw_remote import BaseRemote

VERSION = 0.1

class Event:
	def __init__(self, evtype, ev):
		self.__evtype = evtype
		self.__ev = ev

	def get_evtype(self):
		return self.__evtype

	def get_event(self):
		return self.__ev

# Allow the remote end to put events only
class BaseEventAPI(BaseRemote):
	def __init__(self, class_id=None, target=None, thread_safe=True, *args, **kwargs):
		super().__init__(
			class_id=class_id,
			target=target,
			remote_module="defw_event_baseapi",
			remote_class="BaseEventAPI",
			*args,
			**kwargs)

	def put(self, event):
		pass
