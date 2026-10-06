# DEFw v2 Comparison Report

This report closes Phase 3 of the DEFw v2 prototype. It evaluates the
success criteria in [the design](design_v2.md#success-criteria) against a
campaign that ran every workload on both versions, from one build, on
5 October 2026. Phase 4 decides on it.

## Verdicts

| Criterion | Target | Result | Verdict |
| --- | --- | --- | --- |
| Small RPC round trip, C client, `ofi+tcp` | At most one tenth of v1's Python round trip | 0.108 ms against 4.706 ms, one 44th | Met |
| Small RPC round trip, Python client through the binding | At most one half of v1 | 0.118 ms, one 40th | Met |
| Small RPC round trip, Python service, C client | At most one half of v1 | 0.147 ms, one 32nd | Met |
| Throughput at eight concurrent clients | At least four times v1 | 58 times on `ofi+tcp`, 76 times on shared memory, 50 times with the Python service | Met |
| Bulk bandwidth, 16 MiB and above | At least 80% of Mercury's own bulk benchmark | 94% on `ofi+tcp` and 88% on shared memory at 16 MiB. 79.5% and 69% at 256 MiB | Not met at 256 MiB. Met with the buffer pool, re-measured on 6 October |
| Framework overhead per job in W5 | At most one half of v1 | 0.66 to 0.77 ms against 11.36 ms, one 15th to one 17th | Met |
| Unsafe deserialization on any path | None | None found, and one related finding | Met |
| Application-level regressions in W7 | None | None. Every run passed, in 4.4 to 5.1 s against v1's 21.6 to 22.9 s | Met |
| DEFw-owned lines of code for equivalent function | Fewer than v1's | 9,349 against 9,337, 12 more | Not met |
| C caller experience | The SPANK reserve and release flow in under 100 lines of C | 93 lines | Met |

Eight of the ten are met by the code the campaign measured, and bulk is
met with the buffer pool added since. The design's no-go rule does not
apply: it is for a Python service that fails its criterion, and a Python
service answers a C client in a 32nd of v1's time.

Two were not met in the campaign.

- **Bulk at 256 MiB.** At 16 MiB v2 moves 94% of what Mercury's own
  benchmark moves on `ofi+tcp`, and 88% on shared memory, against the
  benchmark's defaults. Against Mercury with one buffer in flight, as W3
  moves its payload, it is 76% and 68%. At 256 MiB v2 slows and Mercury
  does not, and the cause is the echo service behind W3, not DEFw's
  transport. It allocates and frees the whole buffer on every call. Kept
  between calls, the same buffer moves 256 MiB at 97% of Mercury on
  `ofi+tcp` and 87% on shared memory. A pool of registered buffers in
  libdefw2 would do that for any service, and `openQSE/DEFw` #45 added
  one. Re-measured with it on 6 October, in one session with Mercury's
  benchmark, v2 reaches 97% on `ofi+tcp` and 86% on shared memory at
  256 MiB, and 92% and 90% at 16 MiB. See
  [With the Pool](#with-the-pool-6-october-2026).
- **Lines of code, by 12.** That is 0.13% of either version, and the
  verdict turns on which areas the equivalent-function subset leaves out.
  They are named in [Lines of Code](#lines-of-code). In all, v2 owns 15,930
  lines and v1 10,504.

## How It Was Measured

**Build.** One QFw installation served both versions: QFw
`defw2-prototype` at `35354a4` with DEFw `defw2-prototype` at `127a32d`,
built with v2. `QFW_DEFW_VERSION` chose the version. The image is the
QFw-SLURM-Cluster `defw2-prototype` image of 30 September 2026, with
Mercury 2.4.1, Margo 0.24.2, Argobots 1.2 and libfabric 2.3.1, on a Docker
VM with 10 CPUs and 8 GB. Margo's monitor was off. Margo 0.24.3 changes
only the monitor.

**Placement.** `src2/bench/defw_campaign.py` ran the campaign and put the
containers on CPUs for each workload, and back afterwards.

- W1 to W4 ran in `c1`, given CPUs 0 to 7, with the service on 4 to 7 and
  the clients on 0 to 3.
- W5 and W6 put the clients in `c1` on CPUs 0 to 3, and the QPM's node on
  4 to 7.
- W7 put the compute nodes on CPUs 0 to 3 and the QPM nodes on 4 to 7.
- Every other container went to CPUs 8 and 9.
- One run of each workload kept the cluster's own layout, where the
  clients and the services share CPUs 0 to 3. The tables call those shared
  CPUs, and the rest own CPUs.

Transparent huge pages were off for every process measured, and each report
records what each process had while it ran.

**Transports.** v1 ran on tcp, `ofi+tcp` and `ofi+sm2`, and v2 on
`ofi+tcp` and `na+sm`. Each ratio pairs v2 with v1 on the same kind of
path. W5 to W7 ran on `ofi+tcp` only. Both versions logged as Phase 0 did,
at `error`, with Python's logging at `critical`.

**Runs.** 85 runs, and W7 three times for each version and QPM on their own
CPUs and once on shared ones. A failed run was tried once more, except v1's
W5 at eight clients, which can take ten minutes. 104 tries in all, and 5
runs failed, all W5 at eight clients. See [Failures](#failures).

## Results

### Small Calls

p50 per call with one client, and calls a second with eight, on own CPUs.

| Workload | Clients | v1 `ofi+tcp` | v2 `ofi+tcp` | v2 `na+sm` |
| --- | --- | --- | --- | --- |
| W1, 64 B | 1 | 4.706 ms | 0.108 ms | 0.086 ms |
| W1, 64 B | 8 | 416 | 24,120 | 32,675 |
| W2, 4 KiB | 1 | 10.485 ms | 0.230 ms | 0.103 ms |
| W2, 4 KiB | 8 | 181 | 11,053 | 17,449 |
| W4, resolve | 1 | 6.369 ms | 0.109 ms | 0.088 ms |
| W4, resolve | 8 | 316 | 20,889 | 20,339 |

- v1's transport hardly matters. Its W1 p50 is 4.668 ms on tcp and 4.463 ms
  on `ofi+sm2`, and its eight clients run 407 and 429 calls a second.
- Through Python, v2's W1 p50 on `ofi+tcp` is 0.118 ms from a Python client,
  0.147 ms against a Python service, and 0.156 ms with both. Eight clients
  get 20,938 calls a second from the Python service.
- On shared CPUs eight W1 clients get 22,882 calls a second on v2 and 380
  on v1, 60 times.
- A W1 call puts 310 bytes on the wire on v2 and 1,968 on v1. A W2 call
  puts 8,374 and 12,816, and a W4 resolve 509 and 3,094.
- The C echo service peaks at 34 MiB on `ofi+tcp` and 14 MiB on `na+sm`,
  and v1's at 54 and 42 MiB. The Python service peaks at 60 MiB.

### Bulk

W3 echoes its payload: the service pulls it from the client and pushes it
back, with the RPC inside the time. Its rate, one way, from one client:

| Size | v1 `ofi+tcp` | v2 `ofi+tcp` | v2 `na+sm` |
| --- | --- | --- | --- |
| 1 MiB | 45 MiB/s | 2,180 MiB/s | 2,913 MiB/s |
| 16 MiB | 47 MiB/s | 1,632 MiB/s | 1,844 MiB/s |
| 256 MiB | 47 MiB/s | 1,385 MiB/s | 1,458 MiB/s |

v1 moves bulk by RMA over `ofi+tcp`. Over `ofi+sm2` it has none, and moves
16 MiB at 0.64 MiB/s inline. At 256 MiB v2 peaks at 289 MiB and v1 at
1,776 MiB.

Mercury's `hg_bw_write`, a server pull, and `hg_bw_read`, a server push,
ran on W3's layout, its server on CPUs 4 to 7 and its client on 0 to 3,
with huge pages off. Each figure is the median of three. Since a W3 call
pulls and then pushes, it is set against the rate of a pull followed by a
push, and v2's rate is counted both ways.

| Size | Provider | Mercury pull | Mercury push | Pull then push | v2 W3, both ways | v2 against Mercury |
| --- | --- | --- | --- | --- | --- | --- |
| 16 MiB | `ofi+tcp` | 3,449 | 3,472 | 3,460 | 3,265 | 94% |
| 16 MiB | `na+sm` | 2,889 | 7,575 | 4,183 | 3,689 | 88% |
| 256 MiB | `ofi+tcp` | 3,482 | 3,486 | 3,484 | 2,769 | 79.5% |
| 256 MiB | `na+sm` | 2,900 | 7,646 | 4,205 | 2,917 | 69% |

- The reference is the benchmark as Mercury ships it, with 64 buffers in
  flight. At 256 MiB it ran with 4, to fit the node's memory. With one
  buffer, as W3 moves its payload, Mercury moved more at 16 MiB, 4,289
  MiB/s on `ofi+tcp` and 5,406 MiB/s on `na+sm` for a pull then a push.
  Against those, v2 reaches 76% and 68%.
- v2 slows from 16 to 256 MiB and Mercury does not, because the echo
  service allocates, registers and frees its buffer on every call. glibc
  hands back a freed block of up to 32 MiB from its heap with its pages
  still mapped, but maps anything larger afresh. So at 256 MiB every call
  faults in and zeroes new pages, and unmaps them after.

A build of the echo service that keeps one registered buffer between
calls, switched on and off in the same binary, ran W3 on W3's layout,
alternating, twice each. The means, one way:

| Size | Provider | A buffer each call | One buffer kept | Change |
| --- | --- | --- | --- | --- |
| 16 MiB | `ofi+tcp` | 1,531 MiB/s | 1,593 MiB/s | within run-to-run spread |
| 16 MiB | `na+sm` | 1,730 MiB/s | 1,762 MiB/s | within run-to-run spread |
| 256 MiB | `ofi+tcp` | 1,433 MiB/s, 178 ms a call | 1,688 MiB/s, 151 ms | +18% |
| 256 MiB | `na+sm` | 1,528 MiB/s, 168 ms a call | 1,830 MiB/s, 139 ms | +20% |

With the buffer kept, 256 MiB reaches 97% of Mercury on `ofi+tcp` and 87%
on shared memory, as 16 MiB does. The verdict above stands for the code
the campaign measured.

#### With the Pool, 6 October 2026

`openQSE/DEFw` #45 gave each runtime a pool of registered buffers, which
the echo service borrows from. W3 and Mercury's benchmark then ran in one
session, on W3's layout with huge pages off, in three rounds. Each round
ran Mercury's pull and push, then W3 with the pool and with it switched off
by `DEFW2_BULK_POOL_MIB=0`. The image had Margo 0.24.3 by then, and DEFw
was `defw2-prototype` at `2123137`. Each figure is the median of three, set
against Mercury as above.

| Size | Provider | Mercury pull then push | v2 with the pool, both ways | Against Mercury | Pool off |
| --- | --- | --- | --- | --- | --- |
| 16 MiB | `ofi+tcp` | 3,470 | 3,191 | 92% | 90% |
| 16 MiB | `na+sm` | 4,417 | 3,959 | 90% | 80% |
| 256 MiB | `ofi+tcp` | 3,462 | 3,365 | 97% | 75% |
| 256 MiB | `na+sm` | 4,410 | 3,801 | 86% | 64% |

- With the pool, v2 meets the criterion at both sizes on both providers.
- Switched off, the pool gives back the campaign's figures at 256 MiB, 75%
  and 64% against 79.5% and 69%. At 256 MiB the pool takes a call from 196
  to 152 ms on `ofi+tcp`, and from 181 to 135 ms on shared memory. Each W3
  run made 6 calls at 256 MiB and 100 at 16 MiB, after its warm-up.
- At 16 MiB what the pool changes is within the run-to-run spread.
- Against Mercury with one buffer in flight, as W3 moves its payload, v2
  reaches 98% and 99.7% at 256 MiB, and 74% and 80% at 16 MiB.

### Jobs

W5 runs QFw's fake IQM QPM, which sleeps about a millisecond a job, and
overhead is the job less that sleep. Polling, a job is `async_run` and then
`read_cq` until it is done. On events, it is `async_run`, the completion
event, and one `read_cq`. Own CPUs:

| Client | Clients | Mode | Overhead p50 | Overhead p99 | Jobs a second | Bytes a job |
| --- | --- | --- | --- | --- | --- | --- |
| QFw, v1 | 1 | polling | 11.358 ms | 12.462 ms | 79 | 5,990 |
| C, v2 | 1 | polling | 0.655 ms | 0.887 ms | 538 | 7,275 |
| Python, v2 | 1 | polling | 0.709 ms | 1.105 ms | 523 | 6,574 |
| QFw, v2 | 1 | polling | 0.768 ms | 1.006 ms | 509 | 6,287 |
| QFw, v1 | 1 | events | 16.136 ms | 18.256 ms | 57 | 9,303 |
| C, v2 | 1 | events | 0.879 ms | 1.397 ms | 482 | 3,817 |
| Python, v2 | 1 | events | 0.949 ms | 1.112 ms | 466 | 3,820 |
| QFw, v2 | 1 | events | 1.078 ms | 1.551 ms | 434 | 3,851 |
| C, v2 | 8 | polling | 18.663 ms | 26.438 ms | 402 | 6,796 |
| QFw, v2 | 8 | polling | 18.852 ms | 26.437 ms | 400 | 6,596 |
| C, v2 | 8 | events | 9.393 ms | 10.465 ms | 756 | 3,780 |
| Python, v2 | 8 | events | 9.630 ms | 11.498 ms | 736 | 3,783 |
| QFw, v2 | 8 | events | 10.733 ms | 12.999 ms | 663 | 3,814 |

- No v1 run at eight clients finished. See [Failures](#failures). Phase 2
  measured v1 there at 74 jobs a second, over ten jobs a client.
- At one client an event costs about 0.2 ms more than polling, a hop to the
  client and one back. At eight clients events nearly double what the QPM
  gets through and halve the overhead, since the QPM no longer answers
  polls.
- v1's events cost more than its polling. Each is a blocking RPC from the
  QPM carrying the whole record.
- On shared CPUs QFw's client on v2 got 377 jobs a second polling and 651 on
  events.

W6 returns a 16 MiB statevector, 20 jobs from one client:

| Client | Overhead p50 | Collect p50 | Client CPU a job |
| --- | --- | --- | --- |
| QFw, v1 | 19.442 s | 19.128 s | 5,330 ms |
| C, v2 | 14.240 ms | 13.418 ms | 5.8 ms |
| Python, v2 | 14.485 ms | 13.176 ms | 13.5 ms |
| QFw, v2 | 14.488 ms | 13.622 ms | 6.0 ms |

### W7

`qfw_qiskit_simple.sh` with 4 qubits under Slurm, its QPM a site service,
timed as a whole. Medians of three on own CPUs, and one run on shared CPUs.

| QPM | Placement | v1 | v2 |
| --- | --- | --- | --- |
| NWQ-Sim | own CPUs | 21.7 s | 4.8 s |
| NWQ-Sim | shared CPUs | 22.7 s | 4.7 s |
| Fake IQM | own CPUs | 22.6 s | 4.7 s |
| Fake IQM | shared CPUs | 22.9 s | 4.5 s |

Every run passed on both versions. Each NWQ-Sim QPM ran one job on the
cluster's own layout before the campaign moved it, because of
openQSE/QFw#96, under [Found by the Campaign](#found-by-the-campaign).

## Failures

Five runs failed both tries, all W5 at eight clients. Two others failed once
and passed on retry: v1's W4 on `ofi+sm2`, which lost a send, and QFw's
client on v2 polling, which hit openQSE/QFw#91 as below.

| Run | Cause |
| --- | --- |
| v1, QFw's client, polling, own CPUs | A client never became ready, and the run stopped at ten minutes |
| v1, QFw's client, polling, shared CPUs | 59 jobs never completed, with QFw#91's `KeyError` and "no longer active" in two others |
| v1, QFw's client, events, own CPUs | QFw#91's `KeyError` in the QPM's `set_task_state` |
| v1, QFw's client, events, shared CPUs | A client found no directory service when it started |
| v2, Python client, polling, own CPUs | QFw#91's `KeyError`, then on the retry the QPM stopped completing jobs |

- **openQSE/QFw#91**, the QPM's out-of-resources queue racing under
  concurrent callers, failed jobs on both versions. Rerun with the QPM
  logging errors, three of four v2 runs at eight clients hit it, and one
  showed a provider's completion thread dying in `process_oor_queue` before
  it handed over its own job's result. At `critical` that traceback never
  reaches the QPM's log, which is why the campaign's logs are silent.
- **The QPM stopping.** Twice, once on each version, the QPM went on
  answering every poll but completed no more jobs. On v2 all eight clients
  stopped at the same moment, 19 s into the run, after about 890 jobs
  each. From then on every job waited out the 60 s call timeout, until the
  run's 30-minute limit. Each time it began with a failure in the QPM. On
  v2 one `read_cq` failed with `provider-failure` at the moment the clients
  stopped, its message lost with the run, and the v1 run hit QFw#91's
  errors. The four reruns did not reproduce it. It is not explained, and
  the details are on QFw#91.
- **v1 at start.** Two v1 runs lost a client before it was ready, one to a
  directory it could not find.

In every v2 failure, DEFw answered every call it carried.

## Found by the Campaign

- **openQSE/QFw#96.** QFw's NWQ-Sim, TNQVM and QB QPMs pin their worker
  threads with a search that can loop forever, holding the worker pool's
  lock, when the QPM starts on a restricted set of CPUs. Every `async_run`
  then waits. W7's layout caused it.
- **openQSE/QFw-SLURM-Cluster#32.** The site-services manager on the
  cluster's `defw2-prototype` branch could not start the v1 plane: ssh
  dropped the empty DEFw version it forwarded, moving every argument after
  it.
- **Margo.** The monitor crash, mochi-hpc/mochi-margo#322, is fixed in
  Margo 0.24.3. The image still has 0.24.2. Margo also registers a
  provider's RPC id on the first forward to it without a lock. Two threads
  making that first call at once can both register it, and Mercury frees
  the first registration while a handle still uses it. libdefw2 leaves that
  registration to Margo. Its benchmark clients and event publisher make
  their first calls from one thread, so the campaign did not exercise it.
- **Peak memory**, Phase 0's open question, is the kernel's transparent
  huge pages. With them off, the C echo service peaks at 14 MiB on `na+sm`
  whichever language calls it.
- **Margo's spindown.** Margo spins its progress thread for 10 ms after any
  handler, and a process that listens has handlers often enough to spin
  all the time. `DEFW2_PROGRESS_SPINDOWN_MS` now sets it, 0 by default for
  a process that listens.

## Lines of Code

Lines of code, without comments and blank lines, counted by
`src2/bench/defw_loc.py` at `127a32d`.

| v1 area | Lines | Left out of the subset |
| --- | --- | --- |
| C transport | 805 | |
| C embedded Python | 541 | |
| C runtime | 3,558 | |
| telnet shell | 665 | v2 has no shell |
| Python infrastructure | 4,152 | |
| directory service | 251 | |
| events API | 30 | |
| launcher service | 208 | v2 starts no services |
| test services | 149 | test subjects, as v2's echo is |
| experiments | 145 | a test framework |
| all | 10,504 | |
| equivalent function | 9,337 | |

| v2 area | Lines | Left out of the subset |
| --- | --- | --- |
| echo service | 901 | a test subject, as v1's test services are |
| typed QPM API | 2,397 | v1 carries QFw's QPM API as Python QFw owns |
| compat | 2,229 | it runs v1's code on v2, which v1 needs no help to do |
| telemetry | 1,054 | v1 carries trace context and records nothing |
| directory | 4,366 | |
| events | 1,328 | |
| documents | 278 | |
| service hosting | 798 | |
| RPC and wire | 1,041 | |
| runtime | 727 | |
| Python binding | 811 | |
| all | 15,930 | |
| equivalent function | 9,349 | |

v1 counts `src` and `python` without `python/tests`. v2 counts `src2`
without its tests, benchmarks and examples.

Twelve lines is less than any area the subset leaves out, so the verdict
turns on how the areas are classified, and the tables give each reason.
v2's directory alone is 4,366 lines against v1's 251, since v2's tracks
liveness and generations, publishes changes, and caches bindings on the
client. The SPANK flow,
`src2/examples/defw2_spank_flow.c`, is 93 lines of code against the
public headers.

## Unsafe Deserialization

Every path by which v2 turns bytes from a peer into data was read: the
typed procs, documents, events, bulk tensors, the Python binding and
`defw2.compat`.

- **Typed calls** decode only the declared structures, through procs that
  refuse a length beyond the field's bound or beyond the bytes received.
  A tensor's rank, for example, is refused past `DEFW2_TENSOR_RANK_MAX`
  before any shape is read. The design's [Security](design_v2.md#security)
  section has the rest, and hostile requests against a live provider test
  it.
- **Documents** are JSON. C never parses one, and Python's `json` builds
  only dicts, lists, strings and numbers. A method name must be an
  identifier, and reaches only the handler's `document()`.
- **Python** has no `pickle`, `marshal`, YAML loader, `eval` or `exec`
  anywhere in v2. A statevector becomes a NumPy array of a dtype from a
  fixed table, over the bytes received. compat rebuilds a peer's exception
  only as an `Exception` subclass, from `defw_exception` or the built-ins,
  with the message as its text.
- **One finding.** compat's `connect_to_binding` imports the module a
  directory record names, and instantiates the class the record names,
  as v1's does. It is not deserialization, but a peer that can register in
  the directory chooses code a client loads. Accepting only the API modules
  QFw ships would close it.

v1, for comparison, decodes every message it receives with `yaml.load`
and `yaml.Loader`, in `defw_attachments.attach_load`, and that loader
constructs arbitrary Python objects.

## Data

The campaign's reports, their comparison and the line counts are outside
the repository, in the cluster's
`shared-dir/defw2-baselines/2026-10-05-campaign`, with every failed try
under `failed/`. `src2/bench/README.md` says how to run the campaign again.
