# DEFw benchmark harnesses

These harnesses measure DEFw v1 and DEFw v2 on the same workloads, as
described in "Profiling and the v1 Comparison" in `docs/design_v2.md`. Both
write the same report, `defw-bench-summary/1`, so a v1 run and a v2 run
compare field by field.

## Layout

| Path | What it is |
| --- | --- |
| `defw_bench_common.py` | Workload table, payloads, statistics and the OTLP/JSON writer. It imports nothing from DEFw, so both harnesses share it. |
| `v1/defw1_bench.py` | v1 launcher. It prepares a run and starts a v1 directory service with `defwp`. |
| `v1/defw1_bench_driver.py` | Runs inside that directory service. It spawns the echo service and the clients, then writes the report. |
| `v1/defw1_bench_client.py` | Runs inside each client process. It connects, warms up and measures echo calls, or for W4 resolves. |
| `v2/defw2_bench.py` | v2 launcher. It starts `defw2-echo`, or for W4 `defw2-dirsvc` and an echo registered there, runs the clients and writes the report. |
| `v2/defw2_bench.c` | The measured v2 client, built as `defw2-bench`. It knows nothing about workloads or reports. |
| `v2/defw2_bench_client.py` | The same measurement through the Python binding, with the same arguments and the same result file. |
| `v2/defw2_echo_service.py` | The echo service in Python, which is what the Python half of Phase 0 is measured against. |
| `qfw_qpm_client.py` | W5 and W6 through QFw's own client code, on v1 or on v2 through `defw2.compat`. It runs under `qfw-srun`, and the launcher starts it. |
| `defw_loc.py` | Counts the lines of code each version owns, in all and for equivalent function, for the line-count criterion. See Lines of code below. |
| `defw_bench_compare.py` | Joins v1 and v2 reports on the workload and prints the ratios. Each v2 run is set against v1 on the same provider, `ofi+tcp` against `ofi+tcp` and `na+sm` against `ofi+sm2`, and a row without that pair says it is unmatched. W5 and W6 compare what a job costs, event mode is a group of its own, and runs on CPUs of their own are kept apart from runs on shared ones. |
| `defw_campaign.py` | Runs the whole comparison on a QFw-SLURM-Cluster, from its host. See Running the campaign below. |

## Running the v1 harness

The harness needs a DEFw v1 installation and a Python environment with
PyYAML. In a QFw-SLURM-Cluster compute node:

```bash
export QFW_SHARED_ROOT=/workspace/qfw-container-base
source /opt/openqse/qfw/bin/qfw-activate --venv /opt/openqse/qfw-venv
python3 /workspace/qfw-container-base/QFw/DEFw/src2/bench/v1/defw1_bench.py W1 --defw-revision f1033c2
```

| Option | Default | Meaning |
| --- | --- | --- |
| `W1` to `W4` | | Workload from the design's Workloads table. W4 is below |
| `--payload` | per workload | Payload size, such as `64`, `4KiB` or `16MiB` |
| `--payload-kind` | `bytes` | Send the payload as `bytes` or as an ASCII `str` |
| `--calls` | per workload | Measured calls per client |
| `--warmup` | per workload | Unmeasured calls per client before measuring |
| `--clients` | `1` | Concurrent client processes |
| `--transport` | `tcp` | `tcp`, or `ofi+<provider>` such as `ofi+tcp` or `ofi+sm2` |
| `--defw-path` | `$DEFW_PATH` | The DEFw v1 installation to measure |
| `--defw-revision` | none | Its git revision, recorded in the report |
| `--out` | `/tmp/defw-bench` | Parent of the run directories. Keep it on node-local storage |
| `--service-cpus`, `--client-cpus` | none | CPUs for the service side and for the clients, see Placement below |
| `--keep-thp` | off | Leave transparent huge pages to the kernel, see Placement below |

`defw1_bench.py --help` lists the rest.

The workload defaults are:

| Workload | Payload | Calls per client | Warmup |
| --- | --- | --- | --- |
| W1 | 64 B | 10,000 | 100 |
| W2 | 4 KiB | 10,000 | 100 |
| W3 | 1 MiB | 1600 MiB divided by the payload size, between 5 and 100 | 2 |
| W4 | none | 1,000 | 100 |

## Running the v2 harness

The v2 harness needs a `DEFW_BUILD_V2=ON` build and the Mochi stack on the
library path. Nothing else: no directory service, no Python runtime in the
measured path.

```bash
module load libfabric mochi
export LD_LIBRARY_PATH=<build>/runtime/src:$LD_LIBRARY_PATH
export DEFW2_BIN_DIR=<build>/runtime/src
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
call anything. Each harness runs it against its own directory with one
echo service registered there, and every resolve asks for that service's
type, so every answer is one record:

```bash
python3 src2/bench/v2/defw2_bench.py W4 --transport ofi+tcp --client python
python3 src2/bench/v1/defw1_bench.py W4 --transport ofi+tcp
```

On v2 the launcher starts `defw2-dirsvc`, and a `defw2-echo` that
registers with it. A resolve is `defw2_dir_resolve`, or `Directory.resolve`
from Python. On v1 a resolve is `dirsvc.resolve_services`, which is how v1
finds any service, and the directory is the `defwp` the driver runs in. The
directory is the service measured, so its CPU and memory are what the
report gives as the service's. On v1 that includes the driver, which polls
every 50 ms. Each client's first resolve waits until the echo service has
registered, and the measured resolves are not checked, since an empty
answer is an answer too.

## Running W5 and W6

W5 and W6 time whole QPM jobs: `async_run`, then `read_cq` back to back
until the completion is ready. W5 runs a fixed 4-qubit circuit. W6 asks for
a 20-qubit statevector, 16 MiB, and lends a buffer for it. Their subject is
QFw's fake IQM QPM, which a QFw run serves, so the launcher starts no
service. It finds the QPM through the run's directory.

They need a QFw installation built with DEFw v2, as for any v2 run, and the
QPM's plane. One install serves both versions. Start the plane on one node,
with `QFW_DEFW_VERSION=2` for v2, or `DEFW_TRANSPORT=ofi` and
`DEFW_OFI_PROVIDER=tcp` for v1 on the provider v2 uses:

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
| `--qfw-run-dir` | The QFw run whose plane serves the QPM. Its directory, and for `--client qfw` its DEFw, come from the run's state |
| `--client` | `c` or `python` for the typed clients on v2, or `qfw` for QFw's own client code on the run's DEFw, v1 or v2 |
| `--directory` | A directory to use instead of the run's, for the typed clients |
| `--service-id` | The QPM's service_id. By default the directory's only QPM, as in a QFw run, which names its QPM for the run |
| `--qubits`, `--shots` | Per job, instead of the workload's |
| `--events` | Learn that a job is done from its completion event rather than by polling, then collect it with one `read_cq` |

On a v1 run, `--transport` sets the v1 clients' `DEFW_TRANSPORT`, so give
it the transport the plane runs on.

### Events

With `--events` a client registers for its reservation's completions, then
each job is `async_run`, a wait for that job's completion event, and one
`read_cq`. The `read_cq` takes the completion off the QPM's queue, as the
last poll would have, so the QPM's work per job differs only by the polls
and the event. The typed clients serve a sink and register it with
`register_event_notification`, so they run as servers. QFw's client
registers a `BaseEventAPI` of its own, as QFw's Qiskit backend does, on v1
and on v2 through `defw2.compat`. Each registers for QFw's evtype for a
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

## What a v2 run does

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

## What a v1 run does

1. The launcher writes `config.json` and starts `defwp` as a directory
   service, with the driver running inside it.
2. The driver spawns `svc_test_echo` with DEFw's own `defw_spawn_services`,
   then starts the clients as `defwp` agents.
3. Each client resolves `TestEcho` through the directory service, checks one
   echo, warms up, and signals that it is ready.
4. Once every client is ready, the driver releases them together. Each client
   times its calls, then writes its results.
5. The driver reads the service's CPU time and peak memory from `/proc`,
   stops the service, and writes the report.

## What is measured

- **Round trip.** `perf_counter_ns` around `echo.echo(payload)`, the public
  proxy call. That is the boundary an application sees, so the time covers
  everything v1 does for a call. The echoed value is checked after the clock
  stops.
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
  as the framework encoded it. For v1 that is a message's YAML text and the
  NUL that ends it, which the client counts around its measured loop. For
  v2 it is the message Mercury encoded, from the clients' spans: those of
  the run's trace, and the events their sinks took in while the run was
  measured. So it needs profiling on. Neither version's fixed transport
  header is counted, 28 bytes a message for v1 and Mercury's own for v2, so
  what is compared is the two encodings. A bulk transfer moves outside the
  messages and is not counted. v2's requests carry the trace context that
  profiling adds, 55 bytes, and v1's carry none.

## Placement

`--service-cpus` and `--client-cpus` put the two sides of a run on CPUs of
their own, written the way Linux lists them, such as `4-7` and `0-3`. Then
neither takes CPU time from the other, as when the service has a node of
its own. The service side is everything started beside the clients: the
echo service, W4's directory and the echo registered there, and for v1 the
directory and driver the echo service runs beside. A CPU must be one the
launcher may use. Without them the kernel places every process, and the
service and the clients compete.

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

## Running the campaign

`defw_campaign.py` runs every workload on both versions, from one
installation in one session, and gathers the reports, their comparison and
the line counts into one directory. It runs on the host of a
QFw-SLURM-Cluster, because placing containers on CPUs is the host's to do:

```bash
python3 src2/bench/defw_campaign.py --prefix <install> --plan
python3 src2/bench/defw_campaign.py --prefix <install> \
  --out shared-dir/defw2-baselines/<date>-campaign
```

`--prefix` is a QFw installation built with DEFw v2 from the revision
under test, as the containers see it. `--plan` lists the runs. `--quick`
makes every run with fewer calls, which tests the campaign rather than
measuring anything, and `--only W1,W5` runs some workloads.

| Runs | Providers | Clients |
| --- | --- | --- |
| W1, W2 and W4 | v1 on tcp, `ofi+tcp` and `ofi+sm2`, v2 on `ofi+tcp` and `na+sm` | 1 and 8 |
| W1 with Python on either side, and W4 from Python | v2 on both | 1, and 8 against the Python service |
| W3, 1, 16 and 256 MiB | v1 on `ofi+tcp` and `ofi+sm2`, v2 on both | 1 |
| W5, polling and on events | `ofi+tcp`, QFw's client on v1, and the C, Python and QFw clients on v2 | 1 and 8 |
| W6 | `ofi+tcp`, the same clients | 1 |
| W7 | the site planes, NWQ-Sim and the fake IQM | 3 runs a version |

v1 makes fewer calls where it would otherwise take hours: five of 16 MiB
on `ofi+sm2` and none of 256 MiB, which it moves inline at well under a
MiB a second, and ten W5 jobs a client at eight clients.

The service and the clients get CPUs of their own. The Docker VM needs
ten. For W1 to W4 the harness's node gets CPUs 0 to 7, and the launcher
puts the service on 4 to 7 and the clients on 0 to 3. For W5 and W6 the
clients' node gets 0 to 3 and the QPM's node 4 to 7, and for W7 the
compute nodes get 0 to 3 and the QPMs' nodes 4 to 7. Every other container
waits on 8 and 9. Each workload's headline pair, eight clients on
`ofi+tcp`, runs once more with every container as it was, as Phase 0 and
Phase 2 ran. The campaign puts every container back when it ends, however
it ends.

W5 and W6 start a QFw plane of their own for every run, with the fake IQM
QPM given eight slots as in Phase 2. W7 runs `qfw_qiskit_simple.sh` under
`salloc` against the site planes, which have to be running: v1 from
`site.yaml` and v2 from `site-defw2.yaml`.

A run that fails is tried once more, and both tries are kept, since a run
that fails now and then is a result too. A failed try's run directory goes
under `failed/`, out of the comparison's way. Before the first run the
campaign removes the shared-memory regions `na+sm` left in `/dev/shm` for
processes that no longer exist. A process that is killed cannot remove its
own, and once a container's 64 MiB fills, libfabric's sm2 refuses to start
and v1 falls back to tcp.

| Path | Contents |
| --- | --- |
| `campaign.json` | The arguments, every container's CPUs before the campaign, and each run's outcome, time and log |
| `runs/` | Each run's directory, as its launcher wrote it |
| `failed/` | The directories of tries that failed |
| `logs/` | Each run's command and output |
| `plane/` | The runtime configuration and services manifest the W5 and W6 planes start from |
| `compare.txt`, `compare.json` | `defw_bench_compare.py` over `runs/` |
| `loc.txt`, `loc.json` | `defw_loc.py` |

## Lines of code

The design's criterion is DEFw-owned lines of code for equivalent function,
v2 fewer than v1's C and Python combined. `defw_loc.py` counts what both
halves of that need:

```bash
python3 src2/bench/defw_loc.py --json loc.json
```

A line of code holds something other than whitespace or a comment, and a
docstring counts as a comment. It counts the C and Python files git tracks
under `src` and `python` for v1 and `src2` for v2, without tests,
benchmarks, examples or generated files, and groups them into areas. The
equivalent-function subset leaves out the areas one version has and the
other has no counterpart for, and the table says why for each. For v1
those are its telnet shell, its launcher service, its test services and
its experiments. For v2 they are the echo service, the typed QPM API,
compat and telemetry. `--files` lists every file with its count. The
`defw2_loc_check` test checks the counter on the cases that trip counters,
and fails when a source file belongs to no area.

## Output

Each run gets a directory named
`<UTC time>-v<major>-<workload>-<transport>-c<clients>-<trace id prefix>`.

| Path | Contents |
| --- | --- |
| `summary.json` | The report: workload, transport, environment, latency percentiles, throughput, CPU, memory, and per-client figures |
| `otlp/spans.jsonl` | OTLP/JSON trace data, one export request per line |
| `config.json` | The run's parameters |
| `results/client-N.json` | Raw per-call timings from client N |
| `driver.log` | The driver's output |
| `dirsvc/`, `client-N/` | v1 only: DEFw logs of the directory service, of the echo service under `dirsvc/`, and of each client |
| `logs/` | v2 only: the output of the echo service and of each client |

A v2 run's `otlp/` holds more, because the processes record themselves:

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

## How payloads travel in v1

The payload size decides v1's path, which matters when reading results.

- `bytes` under 4096 stay inside the YAML message.
- `bytes` of 4096 or more become attachments, carried as base64 text. W2's
  4 KiB payload sits exactly on this threshold.
- Attachments of 64 KiB or more go by RMA instead, when the transport is `ofi`
  and the peer supports it.
- A `str` payload stays inside the YAML message at any size.

## Safeguards

- `ofi+*` is refused when the installation's `libdefw.so` does not link
  libfabric, because DEFw would quietly use tcp instead. The driver also fails
  the run when any process logs that it fell back to tcp, and `--force` does
  not skip that check.
- Other DEFw installations are removed from `PYTHONPATH`. `defwp` appends its
  own paths after `PYTHONPATH`, so an environment activated for a different
  installation would otherwise be the one loaded. The report records which
  `defw` module every process used.
- The launcher refuses a payload and client count that look too large for the
  available memory. `--force` skips this check and the libfabric check.
- Stopping the launcher with Ctrl-C or SIGTERM stops every process of the run.
  So does a run that times out or fails, including an echo service that never
  finished registering, which would otherwise keep its port and fail the next
  run.
