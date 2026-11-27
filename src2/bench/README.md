# DEFw benchmark harnesses

These harnesses measure DEFw on a set of workloads. They were written to
compare DEFw v1 with v2 on the same workloads, as described in "Profiling
and the v1 Comparison" in `docs/design_v2.md`, and both versions wrote the
same report, `defw-bench-summary/1`, so that a v1 run and a v2 run compared
field by field. That comparison is in `docs/comparison_report_v2.md`.

v1 is gone, and its harness and the campaign that ran both versions went
with it. What is here measures v2. Where this page still speaks of v1, it
says how that comparison was made.

## Layout

| Path | What it is |
| --- | --- |
| `defw_bench_common.py` | Workload table, payloads, statistics and the OTLP/JSON writer. It imports nothing from DEFw. |
| `v2/defw2_bench.py` | v2 launcher. It starts `defw2-echo`, or for W4 `defw2-dirsvc` and an echo registered there, runs the clients and writes the report. |
| `v2/defw2_bench.c` | The measured v2 client, built as `defw2-bench`. It knows nothing about workloads or reports. |
| `v2/defw2_bench_client.py` | The same measurement through the Python binding, with the same arguments and the same result file. |
| `v2/defw2_echo_service.py` | The echo service in Python, which is what the Python half of Phase 0 is measured against. |
| `qfw_qpm_client.py` | W5 and W6 through QFw's own client code, through `defw2.compat`. It runs under `qfw-srun`, and the launcher starts it. |
| `defw_loc.py` | Counts the lines of code in C and Python files. It counted both versions for the comparison's line-count criterion. |
| `defw_bench_compare.py` | Joins v1 and v2 reports on the workload and prints the ratios, for reports kept from the comparison. Each v2 run is set against v1 on the same provider, `ofi+tcp` against `ofi+tcp` and `na+sm` against `ofi+sm2`, and a row without that pair says it is unmatched. W5 and W6 compare what a job costs, event mode is a group of its own, and runs on CPUs of their own are kept apart from runs on shared ones. |

## Running the harness

The harness needs a build of DEFw and the Mochi stack on the library path.
Nothing else: no directory service, no Python runtime in the measured
path.

```bash
module load libfabric mochi
export LD_LIBRARY_PATH=<build>/src2:$LD_LIBRARY_PATH
export DEFW2_BIN_DIR=<build>/src2
python3 src2/bench/v2/defw2_bench.py W1 --transport ofi+tcp --clients 8
```

| Option | Default | Meaning |
| --- | --- | --- |
| `W1` to `W6` | | Workload from the design's Workloads table. W4, W5 and W6 are below |
| `--payload` | per workload | Payload size, such as `64`, `4KiB` or `16MiB` |
| `--calls` | per workload | Measured calls per client |
| `--warmup` | per workload | Unmeasured calls per client before measuring |
| `--clients` | `1` | Concurrent client processes |
| `--transport` | `ofi+tcp` | Margo provider, such as `ofi+tcp`, `na+sm` or `ofi+cxi` |
| `--rpc-threads` | runtime default | Handler execution streams in the service |
| `--client` | `c` | Measure the C client or the Python one |
| `--service` | `c` | Measure against the C service or the Python one |
| `--service-workers` | `2` | Queue workers in the Python service |
| `--bin-dir` | `$DEFW2_BIN_DIR` | Where `defw2-echo` and `defw2-bench` are |
| `--no-spans` | off | Leave profiling off and write only the summary |
| `--service-cpus`, `--client-cpus` | none | CPUs for the service side and for the clients, see Placement below |
| `--keep-thp` | off | Leave transparent huge pages to the kernel, see Placement below |
| `--out` | `/tmp/defw-bench` | Parent of the run directories |

W3, and any payload too large to ride inside a message, goes through the
bulk path with `defw2_echo_bulk`. Everything else uses `defw2_echo`. The
echo service pulls a W3 payload into a buffer from its runtime's bulk pool
and pushes it back from there. `DEFW2_BULK_POOL_MIB=0` makes it allocate a
buffer for each call instead, as it did in the Phase 3 campaign.

`--client` and `--service` pick which halves are Python, and the run's name
says which pair it was: no suffix for C to C, then `pycli`, `pysvc` or
`pypy`. The Python halves need the binding on `PYTHONPATH`. Measured on
`na+sm` with one client, a 64 byte round trip is 0.067 ms C to C, 0.065 ms
from the Python client, 0.102 ms to the Python service and 0.087 ms for
both, against v1's 4.591 ms.

## Running W4

W4 times directory resolves, the control plane's cost before a client can
call anything. The harness runs it against a directory of its own with one
echo service registered there, and every resolve asks for that service's
type, so every answer is one record:

```bash
python3 src2/bench/v2/defw2_bench.py W4 --transport ofi+tcp --client python
```

The launcher starts `defw2-dirsvc`, and a `defw2-echo` that registers with
it. A resolve is `defw2_dir_resolve`, or `Directory.resolve` from Python.
The directory is the service measured, so its CPU and memory are what the
report gives as the service's. Each client's first resolve waits until the
echo service has registered, and the measured resolves are not checked,
since an empty answer is an answer too.

## Running W5 and W6

W5 and W6 time whole QPM jobs: `async_run`, then `read_cq` back to back
until the completion is ready. W5 runs a fixed 4-qubit circuit. W6 asks for
a 20-qubit statevector, 16 MiB, and lends a buffer for it. Their subject is
QFw's fake IQM QPM, which a QFw run serves, so the launcher starts no
service. It finds the QPM through the run's directory.

They need a QFw installation and the QPM's plane. Start the plane on one
node:

```bash
source <prefix>/bin/qfw-activate --venv <venv>
qfw-setup --profile local --service-id fake-iqm
```

Then measure from another, with QFw active in the same way:

```bash
python3 src2/bench/v2/defw2_bench.py W5 --qfw-run-dir <run> \
  --client c --clients 8 --bin-dir <prefix>/bin
```

| Option | Meaning |
| --- | --- |
| `--qfw-run-dir` | The QFw run whose plane serves the QPM. Its directory comes from the run's state |
| `--client` | `c` or `python` for the typed clients, or `qfw` for QFw's own client code through `defw2.compat` |
| `--directory` | A directory to use instead of the run's, for the typed clients |
| `--service-id` | The QPM's service_id. By default the directory's only QPM, as in a QFw run, which names its QPM for the run |
| `--qubits`, `--shots` | Per job, instead of the workload's |
| `--events` | Learn that a job is done from its completion event rather than by polling, then collect it with one `read_cq` |

### Events

With `--events` a client registers for its reservation's completions, then
each job is `async_run`, a wait for that job's completion event, and one
`read_cq`. The `read_cq` takes the completion off the QPM's queue, as the
last poll would have, so the QPM's work per job differs only by the polls
and the event. The typed clients serve a sink and register it with
`register_event_notification`, so they run as servers. QFw's client
registers a `BaseEventAPI` of its own, as QFw's Qiskit backend does,
through `defw2.compat`. Each registers for QFw's evtype for a
circuit's result, `1`. QFw's client takes no `--events` for W6, since its
events carry the statevector and the `read_cq` would move it again.

A job gets the call timeout, `--timeout-ms` in the clients and 60 s by
default, to complete in, so a completion the QPM loses fails that job
rather than the run. The first job is checked, and tried up to three times
before the client gives up, because QFw's QPM fails a job now and then
under concurrent callers.

The report adds what a job is made of. `qpm.backend` is the QPM's own run
time, as the fake reports it. `qpm.overhead` is the job less that time,
which is the framework's cost per job and W5's headline. `qpm.collect` is
the `read_cq` that found the completion. For W6 it carries the statevector,
and for QFw's client it includes decoding it, since an application has not
got its result until then. `qpm.polls` counts the `read_cq` calls a job
took, one in event mode, and `qpm.events` the completion events, one in
event mode and none when it polls. Wire bytes per job include the event and its
acknowledgement in event mode. The QPM runs on another node, so its CPU
and memory are not reported.

## What a run does

1. The launcher writes `config.json` and starts `defw2-echo serve`, which
   prints its address and nothing else.
2. It starts one `defw2-bench` process per client, each with the run's
   traceparent.
3. Each client makes one checked call, warms up, and signals that it is
   ready. Neither the check nor the warmup carries the run's trace context,
   so one run is one trace holding exactly the measured calls.
4. Once every client is ready, the launcher releases them together.
5. Each client times its calls and writes its timings.
6. The launcher reads the service's CPU time and peak memory from `/proc`,
   stops it, and writes the report and the `qfw.bench.run` span.

The per-call spans are libdefw2's own, written by the client and the service
processes themselves. The launcher writes only the run span they hang
beneath.

## What is measured

- **Round trip.** The clock around one call of the public client function,
  `defw2_echo` in C and `Echo.echo` in Python. That is the boundary an
  application sees, so the time covers everything DEFw does for a call.
  The echoed value is checked after the clock stops.
- **Throughput.** Calls per second, from the first client's first call to the
  last client's last call.
- **CPU per call.** For a client, `getrusage` around its measured loop. For the
  service, `/proc/<pid>/stat` between the go signal and the end of the run.
  Both include every thread of the process.
- **Peak memory.** `ru_maxrss` for clients and `VmHWM` for the service.
- **Bulk rate**, for W3 only. Payload MiB per second of round-trip time.
- **Wire bytes per call**, or per job for W5 and W6. Every message the
  measured calls exchanged, both ways: each request and its answer, and
  each event a service sent a client and its acknowledgement, each message
  as the framework encoded it. That is the message Mercury encoded, from
  the clients' spans: those of the run's trace, and the events their sinks
  took in while the run was measured. So it needs profiling on. Mercury's
  own transport header is not counted. A bulk transfer moves outside the
  messages and is not counted. Requests carry the trace context that
  profiling adds, 55 bytes.

## Placement

`--service-cpus` and `--client-cpus` put the two sides of a run on CPUs of
their own, written the way Linux lists them, such as `4-7` and `0-3`. Then
neither takes CPU time from the other, as when the service has a node of
its own. The service side is everything started beside the clients: the
echo service, and W4's directory and the echo registered there. A CPU must
be one the launcher may use. Without them the kernel places every process,
and the service and the clients compete.

A launcher turns transparent huge pages off for itself and every process it
starts, unless `--keep-thp` says to leave them to the kernel. A kernel whose
policy is `always` backs every 2 MiB range a process touches with a huge
page when one is free. A thread's stack, which uses a few KiB, then holds
2 MiB, and whether it does depends on how fragmented memory is at the time.
That made peak memory vary by a factor of six between runs of the same
service, from 14 MiB to 94 MiB for the C echo service on `na+sm`. With them
off, peak memory measures what the framework uses. The report's
`environment.placement` records the CPUs, the kernel's policy and whether
the run turned huge pages off, and under `seen` what each process had while
it ran, read from `/proc`.

## Output

Each run gets a directory named
`<UTC time>-v<major>-<workload>-<transport>-c<clients>-<trace id prefix>`.

| Path | Contents |
| --- | --- |
| `summary.json` | The report: workload, transport, environment, latency percentiles, throughput, CPU, memory, and per-client figures |
| `config.json` | The run's parameters |
| `results/client-N.json` | Raw per-call timings from client N |
| `logs/` | The output of the echo service and of each client |

`otlp/` holds OTLP/JSON, one export request per line. The processes record
themselves:

| Path | Contents |
| --- | --- |
| `otlp/spans-run.jsonl` | The `qfw.bench.run` span, written by the launcher |
| `otlp/spans-<agent>.jsonl` | One `qfw.transport.rpc` span per call, from each client and from the service |
| `otlp/metrics-<agent>.jsonl` | The duration and byte histograms, and each process's CPU and peak memory |
| `otlp/margo-<agent>.*.json` | Margo's own per-RPC counts, times and call paths, only under `DEFW2_MARGO_MONITOR=1`, which needs Margo 0.24.3 or later, see [mochi-hpc/mochi-margo#322](https://github.com/mochi-hpc/mochi-margo/issues/322) |

`src2/tests/defw2_otlp_check.py <run>/otlp` validates the lot, including
that every service span is a child of the call that produced it.

A run is one trace. Its root span is `qfw.bench.run`, and each measured call is
a `qfw.transport.rpc` span beneath it. The spans and files follow the file
profile of the QFw benchmarking design, `docs/design/benchmarking.md` in
openQSE/QFw.

Percentiles use the nearest-rank method. `p999_us` appears only when there
are at least 1000 samples.

## Safeguards

- The launcher refuses a payload and client count that look too large for the
  available memory. `--force` skips this check.
- Stopping the launcher with Ctrl-C or SIGTERM stops every process of the run.
  So does a run that times out or fails, including an echo service that never
  finished registering, which would otherwise keep its port and fail the next
  run.
