# Jello Virtual Machine

Jello is a statically typed programming language with its own [compiler](https://github.com/Lazarus404/jello-lang) and virtual machine. A
program compiles to a single `.jlo` bytecode module, which the VM runs with a JIT compiler on
arm64 and x86_64.

## The repository

| Directory         | What it holds                                       |
|-------------------|-----------------------------------------------------|
| [`tests/`](tests) | The compiler, written in Jello itself               |
| [`vm/`](vm) | The virtual machine, its native libraries and tools, in C |

### Jello, the virtual machine

The virtual machine has been ported from it's [old repo](https://github.com/Lazarus404/jellovm/) and will be maintained here from now on.

`vm/` builds these:

- `jello`, which runs a `.jlo` module, with a JIT compiler on arm64 and x86_64 as well as an
  interpreter. It runs a program's lightweight processes, each with its own heap, on one
  scheduler thread per CPU, and `jello --boot` turns a module into a standalone executable.
- `jlo_tool`, which makes projects (`jlo_tool init`), fetches their dependencies, and
  disassembles and inspects modules.
- `std.jdll`, the standard library's native code, and `tls.jdll`, TLS on Mbed TLS, which only a
  program that uses TLS loads.
- `libjello_vm.a` with `jello.h`, for a C program that embeds the VM, and `jello/jdll.h`, for
  writing native libraries of your own.

### Building from source

You need a C compiler and CMake 3.5 or later. Build the VM:

```sh
./build.sh
```

That configures `vm/` into `build/` and builds `jello`, `jlo_tool`, `libjello_vm.a` and the
native libraries there. Arguments go to CMake, so a release build is:

```sh
./build.sh -DCMAKE_BUILD_TYPE=Release
```

Or with CMake directly:

```sh
cmake -S vm -B build
cmake --build build
```

`-DJELLO_ENABLE_JIT=OFF` builds an interpreter-only VM. `jelloc`, the compiler as a native
program, is built when `-DJELLOC_IMAGE` names a compiler module from
[jello-lang](https://github.com/Lazarus404/jello-lang), whose own scripts build the VM from a
checkout of this repository beside it.

Run the VM's tests, which build it first:

```sh
./test.sh
```

## Licensing

Jello, meaning its compiler, its virtual machine, its tools and its standard library, is
copyright Jahred Love and is released under the BSD 3-Clause licence, in [`LICENSE`](LICENSE),
which each source file names in its `SPDX-License-Identifier` header.

The VM vendors two libraries, under their own licences:

| Library | Where it goes | Licence |
|---------|---------------|---------|
| [PCRE2](https://www.pcre.org/) 10.30 | `std.jdll`, for regular expressions | BSD 3-Clause, in [`vm/third_party/pcre2-10.30/LICENCE`](vm/third_party/pcre2-10.30/LICENCE) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) 3.5.1 | `tls.jdll`, for TLS | Apache 2.0 or GPL 2.0 or later, in [`vm/third_party/mbedtls-3.5.1/LICENSE`](vm/third_party/mbedtls-3.5.1/LICENSE) |

The compiler's character-width tables are generated from the Unicode Character Database, under
the [Unicode License v3](jelloc/third_party/ucd/LICENSE.txt). A release carries Jello's licence
and the vendored libraries' in its `licenses/` directory.
