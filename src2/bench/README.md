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
| `defw_bench_compare.py` | Joins v1 and v2 reports on the workload and prints the ratios. Each v2 run is set against v1 on the same provider, `ofi+tcp` against `ofi+tcp` and `na+sm` against `ofi+sm2`, and a row without that pair says it is unmatched. |

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
| `--out` | `/tmp/defw-bench` | Parent of the run directories |

W3, and any payload too large to ride inside a message, goes through the
bulk path with `defw2_echo_bulk`. Everything else uses `defw2_echo`.

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

On a v1 run, `--transport` sets the v1 clients' `DEFW_TRANSPORT`, so give
it the transport the plane runs on.

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
took. The QPM runs on another node, so its CPU and memory are not reported.

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
| `otlp/margo-<agent>.*.json` | Margo's own per-RPC counts, times and call paths, only under `DEFW2_MARGO_MONITOR=1`, which is unsafe above one client, see [mochi-hpc/mochi-margo#322](https://github.com/mochi-hpc/mochi-margo/issues/322) |

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
