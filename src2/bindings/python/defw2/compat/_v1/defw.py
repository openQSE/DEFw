"""v1's defw, the module a v1 script imports, on DEFw v2.

What v1 code reads from it: me, this process, with my_endpoint() and
exit(); dirsvc, the directory, None when this process was told of none;
services, the (name, module) pairs it serves; and connect_to_binding, which
turns a resolved directory record into a remote object. Importing it starts
nothing. The v2 runtime starts when one of these is first used.

Anything else v1's defw had fails, naming itself, rather than being
emulated: v2 has no telnet shell, suites or agent table.
"""

import logging

from defw2.compat import _remote, _state

log = logging.getLogger('defw2.compat')

# The service modules this process serves, as v1 kept them.
services = []


class Myself:
	"""v1's me."""

	def my_endpoint(self):
		return _state.endpoint()

	def my_name(self):
		return _state.runtime().node_name

	def my_hostname(self):
		return _state.runtime().hostname

	def is_dirsvc(self):
		return False

	def exit(self):
		_state.exit()

	def __getattr__(self, name):
		raise AttributeError(
			"defw2.compat's defw.me has no {}: only my_endpoint, "
			'my_name, my_hostname, is_dirsvc and exit are on v2'.format(
				name))


me = Myself()


def connect_to_binding(resolved_binding):
	return _remote.connect_to_binding(resolved_binding)


def __getattr__(name):
	if name == 'dirsvc':
		return _state.directory()
	raise AttributeError(
		"defw2.compat's defw has no {}: v1's runtime is not on v2, only "
		'me, dirsvc, services and connect_to_binding are'.format(name))
