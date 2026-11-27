# The DEFw v2 Python binding

A thin package over the public C headers. It contains no networking, no
encoding and no lifecycle logic of its own. It exists to expose the C API
idiomatically and to keep the interpreter away from Margo threads.

## Why cffi

The design proposes cffi and the risk table leaves it open for review. This
is the prototype's answer, and it is reversible: the C API is the same
either way, which is the point of writing the headers for bindings.

- **The GIL rule falls out of it.** cffi releases the interpreter lock
  around every C call. That is exactly what the design requires of the
  client path, and it is what lets a service worker sit in
  `defw2_service_next_call` without stopping the rest of Python. The test
  measures it: 8.4 million Python iterations ran while two workers waited
  inside C. With SWIG the same property is a `%thread` directive per
  function, and one omission is a stall nobody sees until load.
- **The declarations are checked.** `cffi_build.py` compiles its
  declarations against the real headers, so a signature that drifts is a
  build error rather than a crash.
- **It says what the binding depends on.** The `cdef` block is the whole
  contract between Python and `libdefw2`, in one readable page.
- **v1's SWIG layer is what the prototype is trying to leave behind.** It
  generates thousands of lines nobody reads, and it is the part of the v1
  build that breaks first.

SWIG stays viable if the review prefers it. Nothing in `src2` except this
directory would change.

## Building

The build is automatic when cffi is present and skipped when it is not, so
a C-only build needs no Python at all.

```bash
cmake -S . -B build -DDEFW_BUILD_V2=ON \
      -DPython3_EXECUTABLE=/opt/openqse/qfw-venv/bin/python3
cmake --build build --target defw2-python
PYTHONPATH=build/src2/python python3 -c "import defw2; print(defw2.version())"
```

An install puts the package under `DEFW2_PYTHON_INSTALL_DIR`, the prefix's
`lib/pythonX.Y/site-packages` unless set, and `defw2-python` in `bin`. The
installed extension is built apart from the build tree's, with an RPATH
relative to itself and nothing else, so an install can move and never
loads a build tree's library by accident.

## Calling

```python
import defw2

with defw2.Runtime() as rt:
	with defw2.Echo(rt, address) as echo:
		reply = echo.echo(b'hello')
		payload, moved = echo.echo_bulk(big_buffer)
```

A call that reaches the service and comes back returns normally whatever the
service made of it, and a service's own failure raises `DefwError` carrying
the status category. A call that never arrived raises with the transport
category. That is the same two-step the C API has, with the second step
turned into an exception because that is what Python callers expect.

The QPM's three APIs work the same way through `defw2.QPM`, which calls the
C stubs, so a Python caller and a C caller send the same bytes:

```python
with defw2.Runtime() as rt:
	with defw2.Directory(rt) as directory:
		record = directory.resolve(service_type='qfw.qpm')[0]
	with defw2.QPM.from_record(rt, record) as qpm:
		task = qpm.async_run(qasm, num_qubits=20, num_shots=1024,
				     return_statevector=True,
				     reservation_id=rid)
		done = qpm.read_cq(cid=task.cid, reservation_id=rid,
				   result=numpy.empty(1 << 20, numpy.complex128))
		amplitudes = done.statevector_data
```

Answers are objects with the typed fields as attributes. `extra` is the
service's JSON, parsed when it is first read, and `extra_json` the text it
came as. An outcome such as `INVALID_RESERVATION` is in the answer, and a
failed call raises. A result buffer is anything writable, a numpy array
included, and the statevector lands in it directly: `statevector_data` is a
view of that buffer shaped by the answer's descriptor, so nothing is copied
after the push. A buffer that is too small comes back undelivered with
`statevector.nbytes` saying what a retry needs.

### Threads

Any thread may call. A client object keeps nothing per call on itself, so
one may be shared. This works because the runtime always runs Margo's
progress loop on its own execution stream: a thread Argobots has never seen
waits for its reply on an eventual, which Argobots 1.2 allows, while the
progress stream does the network work. Eight threads sharing one `QPM` are
part of the tests.

## Serving

```python
class Reverser:
	def echo(self, request):
		return request[::-1]

with defw2.Runtime(role='server') as rt:
	host = defw2.ServiceHost(rt, 'py-echo')
	host.serve(Reverser(), workers=2)
```

A handler is either an object whose method names match the API's or a
callable taking `(method, request)`. A handler that raises becomes a status
with the `provider-failure` category, so a caller is never left waiting on a
service that gave up. One that raises `defw2.ServiceError` chooses the
category instead, such as `'invalid-reservation'`.

`serve` blocks until `stop()`, which a signal handler may call. `start()` is
the same thing on background threads.

A host can serve several APIs of one service, each on its own provider with
its own queue and its own workers. That is how a QPM is served, and it is
why its `is_ready` is never stuck behind a backlog of `async_run`:

```python
class FakeQPM:
	def is_ready(self, request):
		return {'state': 'running', 'ready': True}

	def async_run(self, request):
		# request.circuit, request.num_qubits, request.extra, ...
		return {'outcome': 'ACCEPTED', 'cid': 'cid-1', 'qtask_id': 1}

host = defw2.ServiceHost.from_environment(
	'qpm:fake:fake-20q', 'qfw.qpm', apis=defw2.QPM_APIS,
	selector={'name': 'fake-20q', 'resources': ['FAKE-20q']})
host.serve(FakeQPM(), workers={defw2.API_QPM_EXECUTION: 4})
```

A typed method takes a `Request`, the C request structure read into plain
Python values with no encoding in between, and returns a dict that is
written straight into the C answer the same way. A statevector answers as
`statevector`, bytes or a numpy array, and a service that will not consume
a completion its caller cannot hold answers with `statevector_shape` alone.
`request.result_capacity` says what the caller lent.

`register`, which `from_environment` calls when `DEFW2_DIRSVC` names a
directory, starts the C agent that keeps the record alive with heartbeats.
`close` stops it first, so the service deregisters before anything else
goes.

### How a call reaches Python

No Margo thread ever executes Python.

1. The C handler decodes the request, puts it on the host's queue and parks
   on an Argobots eventual, freeing its execution stream for other calls.
2. A worker thread blocked in `defw2_service_next_call` takes it, with the
   interpreter lock released for the wait.
3. The worker runs the service method and calls `defw2_service_respond`,
   which sets the eventual.
4. The handler wakes and encodes the reply.

The cost is one hand-off per call. The server's span reports it as its
`queue` event, separately from the time the service itself took, so the two
can be told apart in a report.

## Events

A caller that wants events serves a sink, a provider in its own runtime,
so the runtime is a server. `QPM.register_event_notification` readies the
sink for completions and registers it:

```python
with defw2.Runtime(role='server') as rt:
	with defw2.EventSink(rt) as sink, defw2.QPM(rt, address) as qpm:
		qpm.register_event_notification(sink, type='circuit-result',
						tag='job-7', reservation_id=rid)
		task = qpm.async_run(qasm, num_qubits=20,
				     return_statevector=True,
				     reservation_id=rid)
		event = sink.next(timeout_ms=60000)
		done = qpm.read_cq(cid=event.payload.cid, reservation_id=rid,
				   result=event.payload.statevector.nbytes)
```

`next()` returns the next event, or None when the timeout passes first, and
raises `DefwError` once the sink is closed. Iterating the sink yields every
event until it closes, so a reader thread can be a plain loop, and closing
the sink from another thread ends it. No thread runs Python for a sink: C
queues each event as it arrives, and `next()` takes it with the interpreter
lock released, as a `ServiceHost` worker takes calls.

An event says what it is, `api` and `name`, and whose it is, `type` and
`tag`, which are the registration's. `payload` is what its kind carries,
a `Task` for a QPM completion: the record `read_cq` answers with, its
statevector described and never carried, so `statevector.nbytes` is the
buffer to lend `read_cq` for it. `seq` counts one sender's events to one
target from 1, so a gap shows a loss.

A service sends events through a publisher. `publish` copies the event and
returns at once, and the publisher's own execution stream delivers it.
For a completion, the payload is the dict the service would answer
`read_cq` with:

```python
class QPM:
	def __init__(self, rt):
		self.publisher = defw2.EventPublisher(rt)
		self.registrations = []

	def register_event_notification(self, request):
		# request.target, .type, .reservation_id, .extra
		self.registrations.append((request.target, request.type))
		return {'decision': 'accepted'}

	def completed(self, completion):
		for target, kind in list(self.registrations):
			try:
				self.publisher.publish(defw2.QPM_COMPLETION,
						       target, completion,
						       type=kind)
			except defw2.TargetGone:
				self.registrations.remove((target, kind))
```

`publish` returns False when a full queue dropped the event, and raises
`TargetGone` when an earlier event to that target was not delivered, which
means the registration should go. The next event to the same target is
tried afresh. Delivery is at most once, and the completion queue is how a
caller recovers a lost event.

The directory sends events too, of services coming and going:

```python
with defw2.Directory(rt) as directory:
	subscription = directory.subscribe(sink, service_type='qfw.qpm')
	for event in sink:
		# event.type is SERVICE_CONNECTED or SERVICE_DISCONNECTED
		change = event.payload
		print(change['reason'], change['record']['service_id'])
```

A change says whether the record `connected`, the `reason`, which is
`registered`, `heartbeat-resumed`, `deregistered` or `heartbeat-timeout`,
and the `record` as the directory then holds it, a dict like the ones
`resolve` returns. `unsubscribe` ends a subscription, and returns False for
one the directory no longer holds. `runtime_id` says which runtime the
directory is, the `source` of every event it sends, which is new each time
it starts. Its `timeout_ms` limits that one call, for a caller asking
whether the directory is still there.

## Documents

A method with no typed form goes as a document, a dict of named arguments
in and JSON out:

```python
answer = qpm.document(defw2.API_QPM_CONTROL, 'test')
```

A service answers documents through its handler's `document(api, method,
request, traceparent)`, where `traceparent` is the caller's trace context
or None. Every API a `ServiceHost` serves takes documents, except echo,
and an API with no typed methods takes nothing else. A handler with no
`document` answers none, so a caller can never reach a handler's other
methods by naming them. A document travels inside the message, so it is at
most 4 MiB. The differences are JSON's: a tuple arrives as a list, a key
that is not a string arrives as a string, a numpy value arrives as the
Python value it holds, and a value JSON cannot carry fails the call.

## v1 code on v2

QFw's QPM services and its Qiskit backend are written against v1. They run
on v2 unchanged under `defw2-python`, the launcher `defw2.compat` provides,
which does for v2 what v1's `defw-python` did for v1:

```bash
defw2-python test_qiskit_simple.py 4 fake-iqm   # a v1 client
defw2-python --serve svc_fake_iqm_qpm           # a v1 QPM service module
```

Before anything else loads, it makes the v1 module names importable.
`cdefw_global`, `defw`, `defw_app_util`, `defw_remote` and `defw_workers`
are compat's own, because they were v1's runtime. `api_events`, `defw_cmd`,
`defw_common_def`, `defw_event_baseapi`, `defw_exception`, `defw_trace`,
`defw_util` and `svc_launcher` are v1's own files, loaded unchanged from the
v1 tree that `DEFW_PATH` names. Any other v1 name fails to import with an
error that says it has no v2 counterpart.

A v1 client's API classes come from the directories
`DEFW_EXTERNAL_SERVICE_APIS_PATH` names, as they did on v1, and
`defw.connect_to_binding` takes them from nowhere else. A directory record
that names a module anywhere else fails the connect with a `DEFwError`
before the module is imported. One that names a class that is not a
`BaseRemote` fails before the class is called.

A served v1 QPM answers the typed QPM APIs through `QPMAdapter`, so C and
Python v2 callers reach it as they reach any QPM. A v1 client's API classes
send the fifteen typed QPM methods over the same APIs, and the dictionary
the service returned comes back key for key, its exceptions as the same v1
classes, and a statevector through the bulk path, put back into the v1
payload. Every other method goes as a document, with the arguments the
caller passed, and `QPMAdapter` calls it only when the v1 API class
declares it. `_mapping.py` says exactly what moves into a typed field and what
stays in `extra`, and the design document's Python section says why.

Every compat process listens and serves one sink, as `_events.py`
describes. A v1 client's `register_event_notification` registers that sink
with the QPM, and the QPM's `put` to it publishes the completion without
waiting. The completion arrives on the caller's own queue as v1's `Event`,
its statevector fetched from the completion queue, and a slow sweep
recovers one whose event was lost. Directory events arrive as v1's
dictionaries, and compat asks the directory which runtime it is now and
then, to give v1 code the peer events it follows a restart by.

## Layout

| Path | Contents |
| --- | --- |
| `cffi_build.py` | The declarations, compiled against the real headers |
| `defw2/_runtime.py` | `Runtime`, `Status`, `DefwError`, telemetry, process stats |
| `defw2/_echo.py` | The `qfw.echo` client |
| `defw2/_qpm.py` | The QPM client, its answers, the codecs that read a typed call's request and write its answer, and the completion event |
| `defw2/_event.py` | `EventSink`, `EventPublisher`, `EventTarget` and `TargetGone` |
| `defw2/_doc.py` | Documents: any method of any API as JSON |
| `defw2/_dir.py` | The directory client, records as dictionaries, and the record a host registers |
| `defw2/_service.py` | `ServiceHost`: one provider, queue and set of workers per API, and directory registration |
| `defw2/compat/__init__.py` | `install`, which makes the v1 names importable, and the finder that refuses the rest |
| `defw2/compat/_mapping.py` | How a v1 QPM's dictionaries cross the typed APIs, both ways |
| `defw2/compat/_adapter.py` | `QPMAdapter`, a `ServiceHost` handler over a v1 QPM object |
| `defw2/compat/_remote.py` | Remote objects for v1 callers, v1 exceptions, and a v1 QPM's put to a client's sink |
| `defw2/compat/_events.py` | v1's events: the process's sink, the sweep that recovers a lost completion, and the directory watch behind peer events |
| `defw2/compat/_directory.py` | `defw.dirsvc`: v1 registration and resolution over the v2 directory |
| `defw2/compat/_serve.py`, `__main__.py` | `defw2-python` and its `--serve` |
| `defw2/compat/_v1/` | The five v1 modules compat provides itself |
| `defw2-python.in` | The launcher, configured beside the built package and for the install |

`src2/tests/defw2_python_smoke.py` exercises echo and the host, including
the interpreter-lock property. `src2/tests/defw2_python_qpm_smoke.py` holds
the QPM to the same checks as the C test from both languages, against a C
service and a Python one, with the control queue under an execution backlog
and a Python QPM in the directory. `src2/tests/defw2_compat_smoke.py` runs
a v1 QPM and a v1 client, both written against v1 alone, on v2, and holds
every answer to what v1 gave. All three run under ctest when cffi is
present.
