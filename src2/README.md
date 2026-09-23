# DEFw v2 prototype

v2 is built on Mercury and Margo and lives here, beside v1 in `src/`, which
it does not touch. `docs/design_v2.md` is the design. This directory is the
prototype that the go or no-go decision is made on.

What exists so far is the runtime core: configuration from the environment,
`defw2_init` and `defw2_finalize`, identity, the status model and the logging
sink. The directory client, the RPC tiers, bulk, events, the service host and
the benchmarks are still to come.

## Building

The build is off by default, so every existing DEFw build and CI job is
unchanged until it is asked for.

```bash
module load libfabric mochi          # in the QFw-SLURM-Cluster image
cmake -S . -B build -DDEFW_BUILD_V2=ON
cmake --build build
ctest --test-dir build -R defw2
```

Mercury, Margo and Argobots are found through pkg-config, so any of the
image's `/opt/qfw/mochi`, a Spack `mochi-margo`, or a local build works as
long as `PKG_CONFIG_PATH` points at it.

## Layout

| Path | Contents |
| --- | --- |
| `include/defw2/` | The public headers, written for bindings: opaque handles, fixed-width fields, explicit ownership |
| `core/` | Runtime, configuration, identity and logging |
| `tests/` | C tests. `defw2_runtime_smoke` brings the runtime up and down over `na+sm`, so it needs no network |

## Environment

`defw2_config_from_env` reads the contract in the design's configuration
section. The names v2 adds:

| Variable | Default | Meaning |
| --- | --- | --- |
| `DEFW2_ADDRESS` | `ofi+tcp://` | Margo address. `ofi+cxi://` on Slingshot, `na+sm://` for one node |
| `DEFW2_DIRSVC` | composed from `DEFW_PARENT_*` | Where the directory service is |
| `DEFW2_MARGO_CONFIG` | built in | Path to a Margo JSON configuration |
| `DEFW2_PROFILE` | off | Turns on Margo profiling and diagnostics |
| `DEFW2_RPC_THREADS` | 2 for a server, 0 for a client | Handler threads |

It also reads the v1 names that still mean something: `DEFW_AGENT_NAME`,
`DEFW_AGENT_TYPE` (`service` and `dirsvc` are servers), `DEFW_LOG_DIR`,
`DEFW_LOG_LEVEL`, `DEFW_LISTEN_PORT` for `ofi+tcp`, and
`DEFW_DISABLE_DIRSVC`.

The progress loop always runs on its own execution stream, whatever the
configuration says. That is the rule that keeps a foreign runtime, such as an
embedded Python interpreter, away from the network.
