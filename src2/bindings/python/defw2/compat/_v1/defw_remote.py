"""v1's defw_remote.BaseRemote, on DEFw v2.

A v1 API class, such as QFw's QPMExecution, derives from BaseRemote and
declares its methods with v1's signatures and empty bodies. v1 sent a call
to any of them to the remote object by name. Here a call goes through
defw2.compat._remote, which sends a typed QPM method over the typed APIs,
emulates register_event_notification, and fails anything else, naming it.

A BaseRemote made with no target is a local object, as in v1, and one
made with a v1 endpoint instead of a compat target, as a service does to
call back a client, fails when called: v2 cannot reach Python objects in
other processes.
"""

from defw2.compat import _remote


class BaseRemote(object):
	def __init__(self, class_id=None, blocking=True, target=None,
		     remote_module=None, remote_class=None, *args, **kwargs):
		self.__target = target
		self.__remote_module = remote_module
		self.__remote_class = remote_class

	def __copy__(self):
		return self

	def __deepcopy__(self, memo):
		return self

	def __reduce_ex__(self, protocol):
		raise TypeError('DEFw remote proxies are not pickleable')

	def __getattribute__(self, name):
		if name.startswith('__') and name.endswith('__'):
			return object.__getattribute__(self, name)
		attr = object.__getattribute__(self, name)
		target = object.__getattribute__(self, '_BaseRemote__target')
		if target is None or not callable(attr):
			return attr
		owner = type(self).__name__

		def call(*args, **kwargs):
			if not isinstance(target, _remote.Target):
				raise _remote.unsupported(owner, attr.__name__)
			return _remote.invoke(target, owner, attr, args, kwargs)
		call.__name__ = attr.__name__
		return call


def __getattr__(name):
	raise AttributeError(
		"defw2.compat's defw_remote has no {}: only BaseRemote is on "
		'v2'.format(name))
