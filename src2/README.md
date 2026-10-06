# DEFw v2 prototype

v2 is built on Mercury and Margo and lives here, beside v1 in `src/`, which
it does not touch. `docs/design_v2.md` is the design. This directory is the
prototype that the go or no-go decision is made on.

What exists so far is the runtime core, the typed RPC tier, the telemetry,
the directory and the Python binding: configuration from the environment,
`defw2_init` and `defw2_finalize`, identity, the status model, the logging
sink, bindings and typed stubs, the service host with its call queue,
`qfw.echo` as the reference service with its eager and bulk methods, the
directory service with its client, agent and binding cache, the QPM's
control, admission and execution APIs typed in C, the document tier that
carries every method v2 has not typed, events, the spans and histograms the
comparison reads, a `defw2` Python package that calls and serves all of it,
and the benchmarks that measure the lot against v1.

## Building

The build is off by default, so every existing DEFw build and CI job is
unchanged until it is asked for.

In the QFw-SLURM-Cluster image, from the repository root:

```bash
export PKG_CONFIG_PATH=/opt/qfw/mochi/lib/pkgconfig:/opt/qfw/libfabric/lib/pkgconfig
export LD_LIBRARY_PATH=/opt/qfw/mochi/lib:/opt/qfw/libfabric/lib

cmake -S . -B build -DDEFW_BUILD_V2=ON \
	-DPython3_EXECUTABLE=/workspace/qfw-container-base/qfw-venv/bin/python
cmake --build build -j "$(nproc)" --target defw2 defw2-echo defw2-dirsvc \
	defw2-bench defw2-python defw2_runtime_smoke defw2_echo_smoke \
	defw2_queue_smoke defw2_telemetry_smoke defw2_dir_store_smoke \
	defw2_dir_rpc_smoke defw2_dir_agent_smoke defw2_dir_cache_smoke \
	defw2_self_call_smoke defw2_qpm_smoke defw2_wire_smoke
ctest --test-dir build -R defw2
```

That prints seventeen passing tests. Each line of it is doing something, so
changing one of them tends to be how a build goes wrong:

- **The paths are set by hand rather than with `module load`.** The image
  ships Mochi under `/opt/qfw/mochi` and libfabric under
  `/opt/qfw/libfabric`, but their modulefiles are not on `MODULEPATH`, so
  `module load libfabric mochi` leaves `PKG_CONFIG_PATH` empty and
  pkg-config then cannot find Margo. Mercury, Margo and Argobots are found
  through pkg-config, so any of the image's copy, a Spack `mochi-margo`, or
  a local build works as long as `PKG_CONFIG_PATH` points at it.
- **The targets are named.** `cmake --build build` with no target also
  builds v1, which fails on a SWIG fixture that needs PyYAML, and the
  system Python has none. Naming the targets keeps the v2 build independent
  of the v1 tree's own dependencies.
- **The test binaries are among them.** They are separate targets, so a
  build of `defw2` and `defw2-echo` alone leaves `ctest` reporting every
  test as Not Run, which reads like a broken build rather than a missing
  target.
- **The Python interpreter is named.** The binding needs cffi, the
  container's default `python3` has none, and a configure that does not
  find it says so once and then carries on without the binding. So the
  build looks successful while omitting the `defw2` package and its smoke
  test.

Outside that image, point `PKG_CONFIG_PATH` at whatever provides Margo and
`-DPython3_EXECUTABLE` at a Python with cffi.

`cmake --install build` installs `libdefw2`, the headers, `defw2-dirsvc`,
`defw2-echo`, `defw2-bench`, the `defw2` package under
`DEFW2_PYTHON_INSTALL_DIR` (the prefix's `lib/pythonX.Y/site-packages` by
default) and `defw2-python`. An install runs with nothing on
`LD_LIBRARY_PATH` or `PYTHONPATH`: the binaries carry RPATHs to Margo and
libfabric, the extension finds `libdefw2` relative to itself, and the
launcher finds the package the install put under the prefix. A process
started over ssh gets that bare environment. `tests/defw2_install_smoke.py`
installs into a scratch prefix and runs the compat test from there to hold
it to that.

Build trees are not relocatable: a configured `build/` holds absolute
paths, so copying a source tree that contains one and building in the copy
writes back into the original. Configure a fresh one instead.

## Trying it

`defw2-echo` serves the reference service and calls it. The server prints its
address on stdout and nothing else, so a script can read it back.

```bash
DEFW2_ADDRESS=ofi+tcp:// defw2-echo serve
DEFW2_ADDRESS=ofi+tcp:// defw2-echo ping ofi+tcp://10.0.0.5:45817 -n 5000 -s 64
DEFW2_ADDRESS=ofi+tcp:// defw2-echo ping ofi+tcp://10.0.0.5:45817 -b 16777216
```

`-s` is the payload carried inside the message, `-b` switches to the bulk
path, `-n` is the call count, `-p` the provider and `-t` the timeout in
milliseconds. It is a development tool and reports nothing but latency and
rate. The benchmark client that records spans and writes the OTLP files the
comparison reads is `defw2-bench`, under `bench/`.

## Layout

| Path | Contents |
| --- | --- |
| `include/defw2/` | The public headers, written for bindings: opaque handles, fixed-width fields, explicit ownership, no Mercury |
| `core/` | Runtime, configuration, identity and logging |
| `rpc/` | The wire structures, the checked string and bulk procs, the header and status helpers, bindings, the bulk buffer pool, the typed client stubs, and the one client path and one provider path every typed method takes |
| `telemetry/` | Spans, histograms and the OTLP JSON writer |
| `host/` | The service host: identity, provider registration and the run loop |
| `services/echo/` | `qfw.echo`, the reference service, and the `defw2-echo` tool |
| `dir/`, `services/dirsvc/` | The directory: store, wire, service, client, agent, binding cache and events, and the `defw2-dirsvc` daemon |
| `qpm/` | The QPM's control, admission and execution APIs: wire, client stubs and provider, and its completion event. Nothing outside this directory and `defw2_qpm.h` knows what a QPM is |
| `rpc/defw2_doc.c` | The document tier, one RPC per API on the typed path |
| `event/` | Event sinks, publishers and `defw2.event.deliver`, the one RPC that carries every API's events. Each API supplies its own events' payload, so nothing here knows an event by name |
| `bindings/python/` | The `defw2` package, built with cffi. See its own README |
| `tests/` | C tests, which run over `na+sm`, so they need no network, and the Python checker that reads the OTLP files back |
| `bench/` | The benchmarks, the v1 side of the comparison, and the line counter |
| `examples/` | `defw2-spank-flow`, the Slurm plugin's reserve and release in C |

## Calling and serving

A caller resolves a peer once and keeps the binding.

```c
defw2_binding_t *echo = NULL;
defw2_call_opts_t opts = { .timeout_ms = 5000 };
defw2_buffer_t reply = { 0 };
defw2_status_t status = { 0 };

defw2_binding_create(rt, address, DEFW2_PROVIDER_ECHO, &echo);
defw2_echo(echo, "hello", 5, &opts, &reply, &status);
defw2_buffer_free(&reply);
defw2_status_free(&status);
```

A service is a provider, and each API binds its own operations.

```c
defw2_service_t *svc = NULL;

defw2_service_create(rt, "echo", DEFW2_API_ECHO, DEFW2_PROVIDER_ECHO, &svc);
defw2_echo_bind(svc, NULL);            /* NULL is the built-in echo */
defw2_service_run(svc);                /* until defw2_service_shutdown */
defw2_service_destroy(svc);
```

The return code is the transport outcome and the status is the service's
own. A call that reached the service and came back returns `DEFW2_OK`
whatever the service made of it, so a caller checks the code and then the
status. Everything a stub hands back has one free function.

The design's stub signature passes the wire input structure and a timeout.
These stubs take the payload and a `defw2_call_opts_t` instead, which keeps
Mercury out of the public headers. The header, including the caller's
`traceparent`, is filled from the options.

## The QPM APIs

`defw2_qpm.h` types the QPM hot path, so a C caller such as the Slurm plugin
can reserve, run and collect without an interpreter in its process. Each of
`qfw.qpm.control`, `qfw.qpm.admission` and `qfw.qpm.execution` is its own
provider, 2, 3 and 4 by default, so a service answers `is_ready` while its
execution queue is full.

```c
defw2_qpm_run_req_t run = {
	.ctx = { .reservation_id = rid },
	.circuit = { DEFW2_QPM_FORMAT_OPENQASM2, qasm, strlen(qasm) },
	.num_qubits = 20,
	.num_shots = 1024,
	.return_statevector = true,
};
defw2_result_buffer_t sv = { .data = buf, .capacity = 16u << 20 };

defw2_qpm_async_run(execution, &run, &opts, &task, &status);
defw2_qpm_read_cq(execution, &(defw2_qpm_task_req_t){ .ctx = run.ctx,
		  .cid = task.cid }, &sv, &opts, &done, &status);
```

Every answer has two layers. The typed fields are what a C caller branches
on, and `extra` is a JSON object carrying the rest of what the service said,
which is how a QPM's provider-shaped answers travel without a schema per
provider. Outcomes are data and failures are status: a task whose
reservation does not match comes back with outcome `INVALID_RESERVATION`
and a status of OK, because the service answered.

A large result never travels inside a message. A caller that expects one
lends a buffer with `read_cq`, `peek_cq` or `sync_run`, and the provider
pushes the statevector into it. A buffer that is missing or too small gets
the size it needs back, and `read_cq` leaves the completion queued for the
retry. The buffer goes on the call that collects the result rather than on
`async_run`, because the result exists only once the task completes, and a
Python completion thread cannot drive a Margo transfer.

A C service supplies an operations table per API. Each operation answers
into a structure whose strings come from the call, through
`defw2_call_strdup` and its relatives, and the provider frees all of it once
the reply is on the wire.

A Python service answers the same calls through the call queue, as a typed
call: the consumer reads the call's request structure, fills its answer
structure, and responds with no reply bytes, so nothing is encoded between
C and Python in either direction. `defw2_qpm_smoke --serve` and
`tests/defw2_qpm_fake.py` are the same fake QPM in the two languages, and
the C checks and the Python checks pass against both.

`examples/defw2_spank_flow.c` is what the Slurm plugin would do with these
calls. QFw's plugin has a gateway reserve and release for it today, over
QSGP and in Python. The example finds the QPM in the directory, checks it
is ready, reserves, and later releases, each step a process of its own as
each is a callback of its own in the plugin:

```bash
export DEFW2_DIRSVC=<the directory's address>
rid=$(defw2-spank-flow reserve <service_id> <job_id> <user> 4 1024)
defw2-spank-flow release <service_id> "$rid"
```

It is 93 lines of code against the public headers alone, which is the C
caller experience the design asks for, under one hundred.
`defw2_spank_flow_check` runs it against the Python fake QPM and counts its
lines as `bench/defw_loc.py` counts DEFw's.

## Documents

A method with no typed form goes as a document, with `defw2_doc.h`: a JSON
object of named arguments in, and any JSON value out.

```c
char *answer = NULL;

defw2_doc_call(telemetry, DEFW2_API_QPM_TELEMETRY, "get_backend_info",
	       "{\"lib\": \"qdmi\"}", &opts, &answer, &status);
free(answer);
```

One RPC per API, `defw2.<api>.document`, is registered on the provider that
serves the API's typed methods, so a document shares its queue, header,
status and spans. C never parses a document. A method name must be an
identifier that does not start with an underscore, which both the client
and the provider check, and a document is at most 4 MiB.

The QPM's other three APIs, `qfw.qpm.admission-policy`, `qfw.qpm.scheduler`
and `qfw.qpm.telemetry`, are documents alone, on providers 6, 7 and 8. A C
service answers documents with one function per API, given to
`defw2_doc_bind`, and a queued service answers them from its queue, where
`defw2_call_document` tells a document from a typed call.

## Events

A service tells a caller that something happened through an event, so the
caller need not poll. The caller creates a sink, which is a provider in its
own process, so its runtime must be a server, and gives the service the
sink's address, its provider and a tag of its own. The service sends events
through a publisher.

A QPM takes that registration through `register_event_notification`, the
fifteenth typed QPM method:

```c
defw2_event_sink_opts_t sink_opts = { .callback = on_event, .arg = me };
defw2_qpm_notify_req_t notify = {
	.ctx = { .reservation_id = rid },
	.target = { NULL, DEFW2_PROVIDER_EVENT, "job-7" },
	.type = "circuit-result",
};

defw2_event_sink_create(rt, DEFW2_PROVIDER_EVENT, &sink_opts, &sink);
defw2_qpm_event_accept(sink);
notify.target.address = defw2_event_sink_address(sink);
defw2_qpm_register_event_notification(execution, &notify, &opts,
				      &decision, &status);
```

The tag and the type come back on every event, so one sink can tell its
registrations apart. A reservation limits a registration to that
reservation's tasks. The QPM keeps the registration, as v1's did, until a
delivery to the sink fails, so a caller ends it by destroying the sink. The
provider refuses a registration that names no sink, or names the
directory's provider, before the service sees it.

Python takes events with `next()`, or by iterating the sink, and holds no
lock while it waits, the interpreter's included:

```python
sink = defw2.EventSink(rt)
qpm.register_event_notification(sink, type='circuit-result', tag='job-7',
				reservation_id=rid)
for event in sink:
	collect(event.payload)		# a defw2.Task
```

The service sends each completion that matches a registration:

```c
defw2_event_publisher_create(rt, NULL, &pub);
rc = defw2_qpm_publish_completion(pub, &target, "completion", &task, tp);
if (defw2_event_target_gone(rc))
	drop_registration(&target);
```

A Python service does the same with `defw2.EventPublisher`, whose
`publish` returns False when a full queue dropped the event and raises
`defw2.TargetGone` when the registration should go.

Publishing copies the event and returns. The publisher delivers from an
execution stream of its own: to every target at once, each target's events
in order, each within a time limit, one second by default. A sink answers
as soon as it has queued an event, before its callback or its reader sees
it, so a slow consumer never holds up the service. A delivery that fails
drops what waits for that target, and the next publish to it says the
target is gone, so the service drops the registration. v1 delivered each
event as a blocking call, one client after another, and one client decoding
a large result held up the rest (openQSE/QFw#64).

Delivery is at most once. A full sink, a full queue or a target that cannot
be reached loses the event, and the service's own record, such as a
completion queue, is how a caller recovers. `seq` counts one sender's
events to one target from 1, so a gap shows a loss.

A QPM's completion event carries the task record `read_cq` answers with,
except the statevector, which it describes and never carries. A caller that
wants the statevector lends `read_cq` or `peek_cq` a buffer of the size the
description gives.

One RPC, `defw2.event.deliver`, carries every API's events. Its request is
an envelope naming the API and the event, then the payload as that API's
own wire structure, decoded by that API's own checked proc. A sink decodes
only the kinds its owner accepted, and refuses any other before allocating
for it.

The directory publishes the same way. A subscriber hears
`SERVICE_CONNECTED` when a record it matches becomes UP, by registering or by
its heartbeat resuming, and `SERVICE_DISCONNECTED` when one stops being UP,
by deregistering or timing out, each with the reason and the record, in the
order the directory recorded them:

```c
defw2_dir_subscribe_req_t req = {
	.target = { defw2_event_sink_address(sink), DEFW2_PROVIDER_EVENT,
		    "qpms" },
	.service_type = "qfw.qpm",
};

defw2_dir_event_accept(sink);
defw2_dir_subscribe(dir, &req, &opts, &id, &status);
```

A subscription lasts until `defw2_dir_unsubscribe`, or until a delivery to
its sink fails. A restarted directory has none, and each event's source is
the directory's runtime, so a subscriber that sees a new one subscribes
again. `tests/defw2_dir_event_smoke.c` checks every change, the order of a
hundred of them, the filters, and a dead subscriber that holds up no one.

`tests/defw2_event_smoke.c` stops a sink's process with SIGSTOP, which is
the client #64 describes, and checks that every other sink still gets its
events at once, that no publish waits on the network, and that the stalled
delivery fails at its time limit. Both fake QPMs keep registrations and
publish completions, so the C and the Python client each take both fakes'
events, and `tests/defw2_python_event_smoke.py` holds the Python classes to
what `defw2_event.h` promises.

## v1 code on v2

`defw2-python`, beside the built package, runs v1 Python on v2: a v1
client as `defw2-python script.py`, and a v1 QPM service module as
`defw2-python --serve svc_fake_iqm_qpm`. QFw's code runs under it
unchanged, events included. `bindings/python/README.md` says what it
provides, and `tests/defw2_compat_smoke.py` checks a v1 QPM and a v1 client
against v1's own answers.

## What the wire refuses

Mercury's own string decoder trusts the sender twice: it allocates whatever
length a message claims before checking the bytes are there, growing its
buffer and copying uninitialised memory when they are not, and it never
checks for the terminator. Its bulk-handle decoder has the first flaw too.
Every v2 string therefore goes through `hg_proc_defw2_str_t`, which refuses
a length longer than the field allows or than the message carries, and a
string whose last byte is not its only NUL. Counted bytes, tensor shapes and
lent buffers are bounded the same way, and a provider checks its own answer
before encoding it, so a field too long to send becomes a provider failure
rather than a reply that never arrives.

A decode that fails part way has allocated the fields before the one it
refused. `margo_free_input` would walk them, but it also drops a reference
on the handle that only a successful decode took, and Mercury then recycles
the handle under the handler. `defw2_free_partial` walks them with a proc of
its own instead. `tests/defw2_wire_smoke.c` sends hostile requests to a
live provider and measures the heap across a thousand of them, because the
leak sanitizer cannot see this leak: the decoded structure lives on a
handler stack that Argobots keeps pooled, so a stale pointer to the string
survives and counts as a reference.

## What a run records

`DEFW2_PROFILE=1` turns profiling on for a benchmark run. Each process then
writes node-local OTLP JSON under `DEFW2_TELEMETRY_DIR`, or `DEFW_LOG_DIR`
when that is unset, one export request per line, which is the benchmarking
design's file profile 1. Files are named after the agent, which defaults to
the host and the process identifier.

| File | Contents |
| --- | --- |
| `spans-<agent>.jsonl` | One `qfw.transport.rpc` span per call on each side. The client's carries the round trip, the service's carries `decode`, `handler` and `encode` as events |
| `metrics-<agent>.jsonl` | The `qfw.transport.rpc.duration` and `qfw.transport.rpc.bytes` histograms, and the process CPU and peak resident set |
| `margo-<agent>.*.stats.json` | Margo's own per-RPC counts, times and call paths, only with `DEFW2_MARGO_MONITOR=1` |

Every request carries the caller's W3C `traceparent`, so a service's span is
a child of the call that produced it and one trace crosses the processes.
`defw2_telemetry_run_begin` opens a run span, which is what a benchmark's
`qfw.bench.run` is, and every transport span recorded while it is open hangs
beneath it.

The process CPU and memory totals are written whether or not profiling is
on, since a run needs them to weigh cost against latency. With no directory
to write to there is nothing to record, and the runtime says so once.

Profiling is guarded by the flag, not sampled, because a sampled-out span is
not free. Measured in the image on `ofi+tcp`, a 64 byte round trip goes from
0.074 ms to 0.082 ms with it on, and throughput from 13,100 to 11,500
calls/s, so an always-on span would tax the quantity under measurement by
about a tenth. Spans cost roughly 640 bytes each on the client and 1 KiB on
the service, so a ten thousand call workload leaves about 16 MiB behind.

Margo 0.24 replaced the breadcrumb profiler that the design's telemetry
table names, so `enable_profiling` on its own produces nothing. What
produces Margo's own statistics is its monitor, which `defw2_init` installs
and points at the same directory.

**The monitor is opt-in, and it is not safe to leave on.** The default
monitor in Margo 0.24.2 reads freed memory in
`__margo_default_monitor_on_respond_cb` and takes a service down under
concurrent load. Eight clients against one service reproduce it in seconds,
and the address sanitizer names it. Filed upstream as
[mochi-hpc/mochi-margo#322](https://github.com/mochi-hpc/mochi-margo/issues/322), with a reproducer that needs nothing but
Margo. `DEFW2_MARGO_MONITOR=1` turns it on for a single-client run where its
call paths are worth having. Our own spans cover the same ground and are on
by default, so nothing else is lost.

## Serving from another language

A service that must not run on a Margo thread is served from the host's
call queue. The C handler decodes the request, puts it on the queue and
parks on an Argobots eventual; a thread the runtime knows nothing about
takes the call, answers it, and that wakes the handler to encode the reply.

```c
defw2_service_queue_open(svc, 0);
while (defw2_service_next_call(svc, 1000, &call) == DEFW2_OK) {
	request = defw2_call_request(call, &len);
	defw2_service_respond(call, reply, reply_len);
}
```

The hand-off costs about 35 microseconds per call against the C service on
`na+sm`, and the server's span reports it separately from the service's own
time. `bindings/python/` is the first consumer of this, and
`tests/defw2_queue_smoke.c` proves the mechanism without one.

## Environment

`defw2_config_from_env` reads the contract in the design's configuration
section. The names v2 adds:

| Variable | Default | Meaning |
| --- | --- | --- |
| `DEFW2_ADDRESS` | `ofi+tcp://` | Margo address. `ofi+cxi://` on Slingshot, `na+sm://` for one node |
| `DEFW2_DIRSVC` | composed from `DEFW_PARENT_*` | Where the directory service is |
| `DEFW2_MARGO_CONFIG` | built in | Path to a Margo JSON configuration |
| `DEFW2_PROFILE` | off | Turns on Margo profiling and diagnostics |
| `DEFW2_RPC_THREADS` | 2 for a server, 0 for a client | Handler execution streams |
| `DEFW2_PROGRESS_SPINDOWN_MS` | 0 for a server, Margo's own 10 for a client | How long Margo's progress loop spins after it has handled something before it waits again. See below |
| `DEFW2_BULK_POOL_MIB` | 1024 | The most memory, in MiB, a runtime keeps in registered bulk buffers to lend again. 0 keeps none. See below |
| `DEFW2_TELEMETRY_DIR` | `DEFW_LOG_DIR` | Where the OTLP files go |
| `DEFW2_MARGO_MONITOR` | off | Margo's own statistics. See the warning above |
| `DEFW2_PYTHON` | the active virtual environment's, else `python3` | The interpreter `defw2-python` runs |
| `DEFW2_COMPAT_SWEEP_MS` | 5000 | How often `defw2.compat` looks for a completion whose event was lost |
| `DEFW2_COMPAT_DIRSVC_CHECK_MS` | 2000 | How often `defw2.compat` asks the directory which runtime it is, while v1 code listens for its peer events |
| `DEFW2_COMPAT_DIRSVC_TIMEOUT_MS` | 2000 | How long the directory has to answer each of those questions. Over `ofi+tcp` a call to a directory that has stopped fails only when its time is up, so this decides how soon one is lost |

`defw2-python` finds the v1 tree it reuses at `DEFW_PATH`, as v1's launcher
did. It also reads the v1 names that still mean something: `DEFW_AGENT_NAME`,
`DEFW_AGENT_TYPE` (`service` and `dirsvc` are servers), `DEFW_LOG_DIR`,
`DEFW_LOG_LEVEL` (`error`, `warning`, `message`, `debug`, `all`),
`DEFW_LISTEN_PORT` for `ofi+tcp`, and `DEFW_DISABLE_DIRSVC`.

Two settings are rules rather than tuning, and `defw2_init` enforces both
whatever the configuration says.

**The progress loop always runs on its own execution stream.** That is what
keeps a foreign runtime, such as an embedded Python interpreter, away from
the network.

**A server always gets handler execution streams of its own.** Margo runs
handlers in the primary pool when it is asked for none, which would mean a
service only serves while its main thread is donated to Margo. v2 cannot
promise that thread, because a Python service holds it. A server asking for
no handler threads is raised to the default instead.

**A server does not spin.** After it has handled something, Margo's progress
loop spins for 10 ms before it waits again, which answers a call that comes
right after another sooner. A process that listens, a service or a client
that takes events, has handlers often enough to spin all the time, and that
holds a whole CPU. A W5 client taking events spent about 2 ms of CPU a job
spinning, and a quarter of a millisecond without. So a server spins for
0 ms, and a client keeps Margo's 10 ms. `DEFW2_PROGRESS_SPINDOWN_MS` sets both, and so does
`progress_spindown_ms` for a `defw2.Runtime`. A site's own Margo
configuration, `DEFW2_MARGO_CONFIG`, is used as written, so it sets
`progress_spindown_msec` itself.

**A runtime keeps its bulk buffers.** A handler that pulls a payload, as the
echo service does for W3, borrows a registered buffer from its runtime's
pool and returns it before it answers, rather than allocating one for the
call. A buffer is the size asked for rounded up to a power of two, from
64 KiB, and the pool holds at most `DEFW2_BULK_POOL_MIB`, 1024 MiB unless
set, lent or idle. It frees idle buffers to fit a new one, and a buffer it
still cannot fit is made for its one call. Kept buffers stay allocated
until the runtime finalizes, so a service that once moved 256 MiB keeps
256 MiB. `bulk_pool_mib` sets the same for a `defw2.Runtime`, and 0 keeps
no buffers at all.
