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

The QPM's typed APIs work the same way, through defw2.QPM on the calling
side and a ServiceHost serving defw2.QPM_APIS on the other, and
defw2.Directory resolves what a host registers. A caller takes events, such
as a QPM's completions or the directory's word of a service coming and going,
from a defw2.EventSink, and a service sends them with a defw2.EventPublisher.
"""

from ._dir import (
	API_DIR,
	DIR_SERVICE,
	SERVICE_CONNECTED,
	SERVICE_DISCONNECTED,
	STATE,
	Directory,
)
from ._echo import API_ECHO, PROVIDER_ECHO, Echo
from ._event import (
	PROVIDER_EVENT,
	Event,
	EventKind,
	EventPublisher,
	EventSink,
	EventTarget,
	TargetGone,
)
from ._qpm import (
	API_QPM_ADMISSION,
	API_QPM_CONTROL,
	API_QPM_EXECUTION,
	DTYPE,
	PROVIDER_QPM_ADMISSION,
	PROVIDER_QPM_CONTROL,
	PROVIDER_QPM_EXECUTION,
	QPM,
	QPM_APIS,
	QPM_COMPLETION,
	QPM_VERSION,
	Decision,
	Request,
	Reservation,
	ServiceStatus,
	Task,
	Tensor,
)
from ._runtime import (
	CATEGORY,
	CATEGORY_CODE,
	DefwError,
	Runtime,
	ServiceError,
	Status,
	process_stats,
	version,
)
from ._service import ServiceHost

__all__ = [
	'API_DIR',
	'API_ECHO',
	'API_QPM_ADMISSION',
	'API_QPM_CONTROL',
	'API_QPM_EXECUTION',
	'CATEGORY',
	'CATEGORY_CODE',
	'DIR_SERVICE',
	'DTYPE',
	'Decision',
	'DefwError',
	'Directory',
	'Echo',
	'Event',
	'EventKind',
	'EventPublisher',
	'EventSink',
	'EventTarget',
	'PROVIDER_ECHO',
	'PROVIDER_EVENT',
	'PROVIDER_QPM_ADMISSION',
	'PROVIDER_QPM_CONTROL',
	'PROVIDER_QPM_EXECUTION',
	'QPM',
	'QPM_APIS',
	'QPM_COMPLETION',
	'QPM_VERSION',
	'Request',
	'Reservation',
	'Runtime',
	'SERVICE_CONNECTED',
	'SERVICE_DISCONNECTED',
	'STATE',
	'ServiceError',
	'ServiceHost',
	'ServiceStatus',
	'Status',
	'TargetGone',
	'Task',
	'Tensor',
	'process_stats',
	'version',
]
