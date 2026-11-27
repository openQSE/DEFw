# Distributed Execution Framework

DEFw is the runtime that QFw's services and applications run on. It carries
calls between processes: a directory service where services register and
clients find them, typed remote calls and bulk data over Mercury and Margo,
and events from a service back to its callers.

This repository holds DEFw v2. DEFw v1, the runtime it replaced, embedded a
Python interpreter in a C program and was removed once QFw ran on v2 alone.
Its last commit is `8a3b94f`.

## Layout

| Path | What it is |
| --- | --- |
| `src2/` | The C runtime, the directory service `defw2-dirsvc`, the typed QPM APIs, the Python binding `defw2`, and `defw2-python`, which runs Python on DEFw. [src2/README.md](src2/README.md) describes them. |
| `python/` | The Python modules of v1 that v2 still runs QFw's code with: exceptions, logging, trace hooks, command helpers, the events API and the launcher service. `defw2.compat` loads them unchanged. |
| `docs/` | The [design](docs/design_v2.md), and the [comparison with v1](docs/comparison_report_v2.md) that led to the move. |

## Building

DEFw needs CMake 3.20 or later, a C compiler, libuuid, and Margo and
Argobots 1.2 or later, which it finds through pkg-config. Appendix B of the
[design](docs/design_v2.md) lists the versions it is developed against. The
Python binding is built when the Python that CMake finds can import cffi,
and skipped otherwise.

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build
ctest --test-dir build
cmake --install build
```

`build.sh` runs the first two steps. [src2/README.md](src2/README.md) has the
build in more detail.

## Running Python on DEFw

```bash
defw2-python script.py ARGS            # a client, such as an application
defw2-python -m module ARGS            # the same, by module name
defw2-python --serve service_module    # a QPM service module
```

`defw2-python` runs the active Python interpreter with the module names QFw's
code imports, `defw`, `defw_remote`, `defw_exception` and the rest, provided
on v2. It finds the directory from `DEFW2_DIRSVC`, or from the
`DEFW_PARENT_*` variables when that is unset. The environment it reads is in
[src2/README.md](src2/README.md#environment).
