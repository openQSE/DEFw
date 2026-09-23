# DEFw v2 prototype

v2 is built on Mercury and Margo and lives here, beside v1 in `src/`, which
it does not touch. `docs/design_v2.md` is the design. This directory is the
prototype that the go or no-go decision is made on.

What exists so far is the runtime core and the typed RPC tier: configuration
from the environment, `defw2_init` and `defw2_finalize`, identity, the status
model, the logging sink, bindings and typed stubs, the service host, and
`qfw.echo` as the reference service with its eager and bulk methods. The
directory client, the document tier, events, the Python binding and the
benchmarks are still to come.

## Building

The build is off by default, so every existing DEFw build and CI job is
unchanged until it is asked for.

```bash
module load libfabric mochi          # in the QFw-SLURM-Cluster image
cmake -S . -B build -DDEFW_BUILD_V2=ON
cmake --build build --target defw2 defw2-echo
ctest --test-dir build -R defw2
```

Mercury, Margo and Argobots are found through pkg-config, so any of the
image's `/opt/qfw/mochi`, a Spack `mochi-margo`, or a local build works as
long as `PKG_CONFIG_PATH` points at it.

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
comparison reads is `defw2-bench`, which is separate and still to come.

## Layout

| Path | Contents |
| --- | --- |
| `include/defw2/` | The public headers, written for bindings: opaque handles, fixed-width fields, explicit ownership, no Mercury |
| `core/` | Runtime, configuration, identity and logging |
| `rpc/` | The wire structures, the header and status helpers, bindings and the typed client stubs |
| `host/` | The service host: identity, provider registration and the run loop |
| `services/echo/` | `qfw.echo`, the reference service, and the `defw2-echo` tool |
| `tests/` | C tests. Both run over `na+sm`, so they need no network |
| `bench/` | The v1 side of the comparison, which runs against DEFw v1 |

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

It also reads the v1 names that still mean something: `DEFW_AGENT_NAME`,
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
