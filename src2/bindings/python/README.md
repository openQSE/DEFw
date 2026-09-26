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
service that gave up.

`serve` blocks until `stop()`, which a signal handler may call. `start()` is
the same thing on background threads.

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

## Layout

| Path | Contents |
| --- | --- |
| `cffi_build.py` | The declarations, compiled against the real headers |
| `defw2/_runtime.py` | `Runtime`, `Status`, `DefwError`, telemetry, process stats |
| `defw2/_echo.py` | The `qfw.echo` client |
| `defw2/_service.py` | `ServiceHost` and the queue workers |

`src2/tests/defw2_python_smoke.py` exercises all of it, including the
interpreter-lock property, and runs under ctest when cffi is present.
