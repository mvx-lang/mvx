# MVX

[![ci](https://github.com/mvx-lang/mvx/actions/workflows/ci.yml/badge.svg)](https://github.com/mvx-lang/mvx/actions/workflows/ci.yml)
[![license: GPL-2.0](https://img.shields.io/badge/license-GPL--2.0-blue.svg)](LICENSE)

A native compiler and runtime environment for Pick/MultiValue BASIC.
BASIC source compiles straight to object code via LLVM — no C
transpilation — with DWARF debug info against BASIC source lines, and
the compiler is surrounded by the pieces that make a MultiValue system
a system: pluggable storage (embedded LMDB, a networked daemon, plain
directories), a classic TCL shell, dictionaries and dictionary-driven
`LIST`/`SELECT` queries, secondary indexes, a package ecosystem, and
first-class git version control of hash-file records.

See `ARCHITECTURE.md` for the full design, `DECISIONS.md` for settled
implementation decisions, and the [wiki](https://github.com/mvx-lang/mvx/wiki)
for the documentation (checked out under `docs/` as a submodule — clone
with `--recurse-submodules`, or run `git submodule update --init`).

## Try it

Run the demo account — Snake and a full-screen file demo, cataloged as
verbs — straight from a published image (no build needed):

```sh
docker run --rm -it ghcr.io/mvx-lang/mvx-demo
demo> SNAKE          # arrows steer; q or ESC quits
demo> FSDEMO
```

Or grab a relocatable Linux build (amd64 / arm64) from the
[latest release](https://github.com/mvx-lang/mvx/releases/latest),
unpack it, and put `bin/` on your `PATH`:

```sh
tar xzf mvx-lang-*-linux-amd64.tar.gz     # or -arm64
./mvx-lang/bin/mvx -a myaccount
```

To build from source instead, see **Build** below.

## Why I built this

This is a personal project — an itch I have wanted to scratch for a
long time. Ever since Apple shipped clang and LLVM, I saw LLVM as a
great way to build a real, native implementation of Pick BASIC:
compile straight to machine code instead of transpiling to C, with
proper source-level debugging along the way.

The other half of the idea is the data. There are now plenty of
excellent databases that can hold MultiValue data just as well as Pick
ever did, so the storage layer no longer has to be a bespoke Pick
engine — it can sit behind a driver contract and let a modern database
do the heavy lifting. Between a modern compiler toolchain and modern
storage, it felt like the right time to see whether I could actually
build it. So I did.

## Seeking Pick/MultiValue products to port

**I am looking for real Pick-based products to port to MVX** — the goal
is to make MVX more feature-complete and robust by exercising it
against actual application code rather than synthetic tests. Real
programs surface the dialect corners, intrinsic coverage, and
operational edges that matter.

If you have a Pick/MultiValue application (UniVerse, UniData, jBASE,
D3, Reality, or similar) that you would be willing to let me test
against, please get in touch. **I am happy to sign an NDA** to get
access to code for testing.

**Infrastructure and development tools are the priority to start
with** — application frameworks and toolsets such as **CueBic**
(Apscore International) and **SB+ / SystemBuilder** (now SB/XA, Rocket
Software) — because porting the platforms that products
are *built on* creates the foundation to build and run those products
on MVX. Runtime/application frameworks, screen and report generators,
and similar toolkits are exactly what I would like to test against
first.

To reach me, either:

- **Open a GitHub issue** at
  [github.com/mvx-lang/mvx/issues](https://github.com/mvx-lang/mvx/issues),
  or
- **Email Gordon Heydon — gordon@heydon.com.au**.

Bug reports, feature requests, and dialect-compatibility notes are also
welcome the same way.

## Status

The compiler and the environment are both well advanced. Highlights:

- **Native compiler** — programs and subroutines to executables and
  shared libraries, DWARF debug info (`lldb` steps BASIC source). The
  1M prime-sieve benchmark runs at ~99% of an equivalent C program via
  two tiers of numeric specialisation over the boxed value
  representation.
- **Storage** — a pluggable driver contract with embedded LMDB, a
  networked `mvx-lmdbd` daemon, and directory backends; dictionaries,
  secondary indexes, record locks, and select lists; mixed
  local/remote per file (`CREATE-FILE … USING <driver>`).
- **Environment** — classic TCL with VOC dispatch, the standard verb
  set (all written in MVX BASIC), `EXECUTE` and a runtime privilege
  gate, packages with dependency manifests, full-screen terminal
  support, and git version control of records (branch/merge/cherry-pick
  for multi-site delivery). A whole account round-trips through git: mvx-git (or mvx-convert-acct)
  exports it to a legible directory form and rebuilds a clone into hash
  files.

## Build

Requires CMake, Ninja, and LLVM (`brew install llvm cmake ninja`).

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm
ninja -C build
```

Produces `build/bin/mvx-basic`, `build/bin/mvx`, the runtime in
`build/lib/`, and the system account (standard verbs) in
`build/system/`.

### Choosing components

Core (compiler, runtime, shell, and the directory + LMDB drivers) always
builds. Optional components are selected with CMake flags — a summary of
what's enabled prints at the end of configuration:

| flag | default | effect |
|---|---|---|
| `MVX_WITH_POSTGRES` | `AUTO` | Postgres driver: `AUTO` builds it when libpq is found, `ON` requires it (errors if missing), `OFF` skips it |
| `MVX_WITH_DAEMON` | `ON` | the `mvx-lmdbd` daemon and `mvx-lmdbd-admin` |
| `MVX_MINIMAL` | `OFF` | core only — sets the defaults above to `OFF` |

```sh
cmake -S . -B build -G Ninja -DMVX_WITH_POSTGRES=OFF   # no Postgres
cmake -S . -B build -G Ninja -DMVX_MINIMAL=ON          # core only
```

Because drivers are dlopen'd, a backend left out simply isn't available
at run time — nothing above the driver depends on it.

Install a relocatable prefix (binaries in `bin/`, runtime and drivers in
`lib/`, standard verbs in `share/mvx/system/`) with:

```sh
cmake --install build --prefix ~/.local
```

Installed binaries locate the runtime, drivers, and system account
relative to their own path, so `mvx` runs from `PATH` with no environment
variables and the prefix can be moved as a unit.

## Tests

```sh
ninja -C build check          # or: scripts/test.sh   (-q skips the sieve)
```

Language tests diff `tests/*.b` output against `tests/expected/`;
system tests run scripted TCL sessions in a throwaway account. After an
intentional behaviour change, re-capture with `scripts/test.sh --bless`
and review the diff.

## Use

```sh
build/bin/mvx-basic prog.b -o prog          # compile + link executable
build/bin/mvx-basic -c prog.b -o prog.o     # compile to object only
build/bin/mvx-basic -shared subs.b -o libsubs.dylib   # subroutine library
build/bin/mvx-basic prog.b subs.b -o prog   # main + subroutines together
```

Compile errors go to stderr as `item:line: message`.

Source-level debugging works out of the box:

```sh
lldb prog
(lldb) b prog.b:12
(lldb) run
```

## Trying it

```sh
build/bin/mvx-basic tests/smoke.b -o /tmp/smoke && /tmp/smoke
build/bin/mvx-basic bench/sieve.b -o /tmp/sieve && /tmp/sieve
```

## Examples

`examples/` holds fuller programs that exercise the full-screen
terminal support (KEYIN, cursor/screen control, colour):

- **`FSDEMO.b`** — a full-screen demo in Midnight Commander dress: a
  cyan menu bar and function-key bar, a movable marker (arrows, HOME,
  END, PGUP/PGDN), and chrome that re-flows live when the terminal is
  resized.
- **`SNAKE.b`** — Snake, in MVX BASIC: arrows steer, eating grows and
  speeds the snake, walls and your own tail end it. The game tick is a
  `KEYIN` with a timeout; the body is a dynamic array driven by
  `LOCATE` / `INSERT` / `DELETE`.

```sh
build/bin/mvx-basic examples/SNAKE.b -o /tmp/snake && /tmp/snake
build/bin/mvx-basic examples/FSDEMO.b -o /tmp/fsdemo && /tmp/fsdemo
```

## The environment

Create an account and log on to the classic shell:

```sh
scripts/mkaccount.sh myaccount
build/bin/mvx -a myaccount
```

```
myaccount> CREATE-FILE CUSTOMERS
myaccount> LIST CUSTOMERS
myaccount> OFF
```

Requires LMDB (`brew install lmdb`) for storage and, for the git
package, libgit2 (`brew install libgit2`). See the
[wiki](https://github.com/mvx-lang/mvx/wiki) for the full documentation.

## Layout

- `compiler/` — lexer, parser, LLVM codegen, `mvx-basic` driver (C++17)
- `runtime/` — value type, arrays, storage, intrinsics (C11);
  `mvx_runtime.h` is the permanent ABI surface
- `daemon/` — `mvx-lmdbd`, the networked LMDB storage daemon
- `tcl/` — `mvx`, the classic shell
- `verbs/` — the standard verb set (MVX BASIC)
- `packages/` — shipped packages (`cmd`, `git`, `sample`)
- `docs/` — the documentation, a submodule of the GitHub wiki
- `examples/` — larger sample programs (full-screen demo, Snake)
- `demo/` — the demo account (a submodule of `mvx-lang/demo`): clients,
  inventory, orders with line items, staff. Committed in the open account
  format, so the same repository builds an account on UniData and UniVerse
  as well as here — dictionaries, conversions, associations and I-types
  included
- `demo-mvx/` — an MVX-only demo account, for what does not travel: the
  mouse demo needs terminal support UniVerse has not got
- `bench/sieve.b` — the prime-sieve benchmark
- `tests/`, `scripts/test.sh` — the test harness

## License

Copyright (C) 2026 Gordon Heydon.

MVX is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License, version 2** as published
by the Free Software Foundation. See [`LICENSE`](LICENSE) for the full
text.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
