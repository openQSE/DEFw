"""DEFw v2 from Python.

A thin package over the public C headers, built with cffi. It contains no
networking, no encoding and no lifecycle logic of its own. It exists to
expose the C API idiomatically and to keep the interpreter away from Margo
threads.

	import defw2

	with defw2.Runtime() as rt:
		with defw2.Echo(rt, address) as echo:
			print(echo.echo(b'hello'))

and on the other side:

	with defw2.Runtime(role='server') as rt:
		host = defw2.ServiceHost(rt, 'echo')
		host.serve(lambda method, request: request)
"""

from ._echo import API_ECHO, PROVIDER_ECHO, Echo
from ._runtime import (
	CATEGORY,
	DefwError,
	Runtime,
	Status,
	process_stats,
	version,
)
from ._service import ServiceHost

__all__ = [
	'API_ECHO',
	'CATEGORY',
	'DefwError',
	'Echo',
	'PROVIDER_ECHO',
	'Runtime',
	'ServiceHost',
	'Status',
	'process_stats',
	'version',
]
