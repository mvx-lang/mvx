# MVX — Architecture

MVX is a native compiler and runtime for Pick/MultiValue BASIC, with a
pluggable storage layer, a classic TCL shell, and an extended shell.

This document records the design decisions made so far. It covers the
whole system for context, but **Slice 1** (the compiler and minimal
runtime) is the only part currently in scope for implementation.

---

## 1. Goals

- A **real compiler**: BASIC source straight to object code via LLVM. No
  transpilation to C (the jBASE approach), because that costs debuggability,
  build speed, and toolchain independence.
- **Executables and shared libraries**: main programs link to executables,
  subroutines compile into shared libraries, mirroring the jBASE
  catalog/deployment shape.
- **Source-level debugging**: DWARF debug info emitted against BASIC line
  numbers, so `gdb`/`lldb` step through BASIC, not generated C.
- **Pluggable storage**: MV files bound to backends via a shared-library
  driver interface. Mix and match per file within an account.
- **Multi-dialect**: eventually UniData-style flavour support, driven by
  declarative dialect profiles rather than forked frontends.
- **Container-native**: stateless app containers, scalable under Compose
  or Kubernetes.

---

## 2. System layers

```
  BASIC source
       |
  [ mvx-basic compiler ]  frontend -> AST -> LLVM IR -> object
       |
  [ runtime library ]  dynamic strings, arrays, I/O, EXECUTE, locks
       |
  [ storage driver interface ]
       |
  +----+----+----------+-------------+
  |         |          |             |
 SQLite    LMDB     Directory     Mongo / SQL
(default) (embed)  (source files)   (networked)
```

Above this sit the shells:

```
  classic TCL (C, frozen once complete)   <- default login shell
  extended TCL / XTCL (BASIC, cataloged)  <- superset, invoked as a verb
       |
  verbs: BASIC, CATALOG, RUN, LIST, SORT, SELECT ...  (all BASIC programs)
```

---

## 3. Slice 1 — compiler + minimal runtime

**Target milestone**: compile and run a Pick BASIC implementation of the
prime sieve from the [Primes benchmark](https://github.com/PlummersSoftwareLLC/Primes),
producing a correct prime count and a competitive pass rate.

No storage layer, no VOC, no TCL, no catalog. The compiler is driven
directly from the Unix shell.

### 3.1 Why the sieve

The sieve is small enough to hand-verify but exercises the entire
pipeline end to end:

| Sieve requirement | What it forces |
|---|---|
| `DIM`'d bit/byte array | Dimensioned array representation and indexing, distinct from dynamic strings |
| Outer loop to sqrt(N) | Numeric conversion in a string-typed language |
| Inner striding loop | Loop codegen, integer arithmetic |
| 5-second timed run | Clock primitive in the runtime |
| Result line | `PRINT` and output formatting |
| Benchmark comparison | **Reveals codegen quality immediately** |

That last row is the point. A naive implementation where every array
element is a dynamic string and every arithmetic operation round-trips
string -> number -> string will be roughly two orders of magnitude off a C
implementation. The sieve surfaces that on day one rather than after the
language is built.

### 3.2 Language subset for Slice 1

Enough to express the sieve and exercise the ABI:

- Variable assignment
- Integer and floating arithmetic, comparison operators
- `IF` / `THEN` / `ELSE`
- `FOR` / `NEXT` and `LOOP` / `REPEAT`
- `DIM` and dimensioned array element access
- `PRINT` / `CRT`
- `CALL` to a separately compiled subroutine
- `SUBROUTINE` declaration with argument passing
- A timing intrinsic (`TIME()` or equivalent)

Everything else — dynamic array functions, file I/O, `EXECUTE`, `OCONV`/
`ICONV`, select lists — is out of scope for Slice 1.

### 3.3 Open decisions to settle first

These two shape every line of generated code and are effectively
permanent once subroutines start being compiled separately.

#### Decision A — value representation

MV BASIC is string-typed at heart: a variable holds a dynamic string, and
numbers are strings that happen to parse as numeric. The naive
implementation is correct but far too slow for the sieve.

Options:

1. **Uniform boxed value** — every variable is a runtime value struct
   (tagged union of string / int / double). Simple, correct, slow.
2. **Boxed value plus numeric fast path** — the value struct caches a
   parsed numeric form so repeated arithmetic avoids re-parsing. Moderate
   complexity, moderate gain.
3. **Type specialisation in the compiler** — analyse the function; where a
   variable is provably only used numerically, emit native `i64`/`double`
   operations with no runtime calls at all. Falls back to boxed values
   where the analysis fails.

**Recommendation**: build option 1 first for correctness, but design the
value struct and the IR-emission layer so option 3 can be added without
reworking the frontend. The sieve is the test case that justifies option 3.
Do not ship Slice 1 as "done" until the sieve runs at a respectable rate —
that is the whole point of choosing this milestone.

#### Decision B — subroutine ABI

Separately compiled subroutines in shared libraries must agree on a
calling convention forever. MV `CALL SUB(A, B)` passes by reference —
the callee can mutate the caller's variables.

To settle:

- How the value struct is passed (pointer to value, always).
- Argument count and arity checking — MV traditionally allows arity
  mismatch to fail at runtime; decide whether to check at link time.
- Whether there is a hidden context/environment parameter (needed later
  for session state, locks, and the privilege gate — **add it now** even
  if unused in Slice 1, because adding a parameter later breaks every
  compiled subroutine).
- Name mangling for cataloged subroutine symbols.

### 3.4 Minimal runtime for Slice 1

A small C or C++ library the compiler emits calls into:

- The dynamic value type: allocation, copy, release, string/number
  conversion.
- Dimensioned array allocation and element access.
- `PRINT` and output formatting.
- Timing intrinsic.
- `CALL` dispatch for direct (link-time resolved) calls.

No storage, no locks, no `EXECUTE`.

### 3.5 Implementation notes

- **Language**: C++17 or later. The LLVM C++ API is first-class; the C API
  lags, particularly for `DIBuilder` debug info.
- **Reference dialect**: traditional Pick BASIC (classic Pick / R83
  style), implemented concretely. Do **not** build the dialect-profile
  abstraction until a second dialect is actually needed — the abstraction
  designed before the second implementation will be the wrong abstraction.
- **Debug info**: emit DWARF via `DIBuilder` from the start, mapping to
  BASIC source lines. Retrofitting debug info is far harder than emitting
  it as you go.
- **Driver CLI**: `mvx-basic -c prog.b -o prog.o`, plus linking to executable or
  `-shared` for subroutine libraries. Errors on stderr in a parseable form
  (item, line, message) — the `BASIC` verb will depend on this contract
  later.

---

## 4. Storage layer (Slice 2 — not yet in scope)

Recorded here so Slice 1 does not paint it into a corner.

### 4.1 Driver interface

MV record access is primitive — a keyed blob store — so the portable core
is small:

```
open(connection-params)      -> handle
read(handle, id)             -> record | not-found
write(handle, id, record)
delete(handle, id)
select(handle)               -> cursor over ids
lock(handle, id) / unlock    -- MV READU semantics
```

**The minimal contract is the whole contract.** Backends may advertise
optional capabilities (query pushdown, native indexes, sorting) via
capability negotiation, but application code must run correctly against
the minimal set. If BASIC code or a verb ever depends on backend-specific
behaviour, the migration path between backends breaks.

Records are marshalled to and from the MV dynamic-string format **in the
driver**. MV record format is the lingua franca; dictionaries then work
unchanged regardless of backend.

### 4.2 LMDB as first backend

- One LMDB environment per account; one named DB per MV file.
- Key = record id, value = MV record bytes. Nearly a passthrough.
- Read gives a pointer into the mmap valid only for the transaction —
  copy out immediately.
- **Single writer**: concurrent reads scale beautifully, writes serialise.
  Suits MV's read-heavy profile; do not architect expecting parallel writers.
- **Short transactions only.** A long-lived read txn pins pages and bloats
  the file. `SELECT` snapshots the id list inside a short txn, then closes
  it — which matches MV select-list materialisation semantics anyway.
- **Record locks live in the runtime, not in LMDB txns.** `READU` can hold
  a lock across user think-time; an LMDB write txn must never be held that
  long.
- Max key size defaults to 511 bytes — validate record ids on write.

### 4.3 Multi-host storage

LMDB is embedded and single-host; a network filesystem will corrupt it. An
account that must be reached from more than one host puts its files on a
backend built for that — **postgres**, or mongo or mysql — with the address
in a named connection profile so the committed `BINDINGS` record names only
the profile.

This was once our own daemon, `mvx-lmdbd`, reached through an `lmdbnet`
driver. Both were removed (mvx#327). The argument against them was
pushdown: `lmdbnet` supplied none of `select_where`, `select_order`,
`count_where`, `sum_where`, `select_join` or `explain`, so every `WITH`
streamed the whole id list to the verb and filtered it there — paying
network latency for the privilege. postgres answers those in SQL. The
decision and what was kept from it are recorded in `DECISIONS.md`.

Cross-session record locking survived the change: the driver contract's
optional `lock`/`unlock` slots are answered by **postgres**, with
session-level advisory locks (mvx#16), so a `READU` is honoured across
processes and released when the holding session ends. What did not survive
is the *lease* — the daemon dropped a dead client's locks the moment its
connection closed, which is what made a killed pod safe. A postgres session
ends with its connection too, so the property largely carries over, but it
is the server's behaviour now rather than ours to guarantee. Backends with
no lock authority (embedded LMDB, directory files) use the process-local
lock table, which coordinates within one process only.

### 4.4 The migration curve

```
SQLite  ->  Postgres / MySQL / Mongo
```

Each step is a config swap because the driver interface is the boundary.
The migration trigger is knowable in advance: **more than one host, or a
need for native HA**. Until then SQLite carries the workload with almost no
operational surface — one local file, no server, and it answers `WITH` and
`BY` in SQL. Migration can be done **per file**, moving one hot file to a
heavier backend while the rest stay local (`CONVERT-FILE`), or a whole
account at once (`CONVERT-ACCOUNT`).

---

## 5. Indexing (Slice 2+)

LMDB provides no secondary indexing — a sorted primary key B+tree only.
Indexes are built and maintained in the write path.

### 5.1 Structure

An index is a companion named DB using `MDB_DUPSORT`:

```
main DB:   CUSTOMERS              key = record-id  value = MV record
index DB:  CUSTOMERS.IDX.STATE    key = "VIC"      value = record-id  (DUPSORT)
```

All named DBs share one transaction, so record and index updates commit
atomically. No index drift.

### 5.2 Write path

Inside one transaction: read old record, extract old indexed values,
extract new values, delete stale index entries, add new ones, write the
record, commit.

Extraction runs the **dictionary definition** (attribute number,
conversions, correlatives) — the same logic `LIST` and `SELECT` use. A
multivalued attribute emits one index entry per value, so `WITH` matches
if any multivalue matches.

### 5.3 Optimisations

1. **Diff, do not replace.** Set-difference old and new extracted values;
   touch only indexes whose values actually changed. Most writes change
   few attributes.
2. **Skip the read-old when possible** — on insert there is nothing to
   diff; on the common `READU` then `WRITE` pattern, pass the already-read
   record through.
3. **Extract only indexed attributes**, driven by a per-file list.
4. **Cache index metadata at file open.** Never re-read dictionary items
   per write. The dictionary is the source of truth; the cache is the hot
   path. Invalidate on index create/drop.
5. **Batch within a transaction** for bulk loads only — larger
   transactions hold the single writer lock longer.

### 5.4 The `TRANS()` problem

A dictionary item containing `TRANS()` derives its value from a record in
**another file**. Indexing such an item creates a dependency the local
write path cannot see: when the remote record changes, index entries in
the local file silently go stale, and no write to the local file occurred.

This is worse here than on legacy MV, because the remote record may live
in a different backend behind a different daemon — so maintenance would
cross store boundaries and lose atomicity.

**Decision**: at `CREATE-INDEX` time, inspect the dictionary item's
correlative. If it contains `TRANS()` or another non-local correlative,
**refuse to build the index** and explain why. The blessed fix is
**denormalisation** — store the translated value in the local record so
the index depends only on data being written.

Optionally allow an explicit *advisory* mode: the index narrows the
candidate set, the query re-evaluates `TRANS()` live to confirm, and a
manual `REBUILD-INDEX` verb cleans drift after reference-data changes.
Documented as approximate.

**Do not build reverse-dependency cascade maintenance.** It reintroduces
cross-store coordination and lost atomicity, with severe write
amplification — one small reference-data edit re-indexing thousands of
records.

The governing principle: **index maintenance stays local to a single
record write in a single backend.**

---

## 6. Shells (Slice 3+)

### 6.1 Two shells, sh/bash style

- **Classic TCL** — implemented in C, a faithful Pick TCL clone. It is the
  default login shell and the always-present baseline that works even if
  the compiler or catalog is broken. **Once feature-complete, it is
  frozen.** All new capability goes in the extended shell.
- **Extended TCL (XTCL)** — a cataloged BASIC program, a **strict
  superset**. Adds quality-of-life features: history, completion, better
  scripting, improved errors. Invoked as a verb from classic TCL (like
  typing `zsh` inside `sh`), or set as a login shell via a small launcher
  binary.

The discipline that makes this work: classic stays frozen, extended is a
superset, **both dispatch through the same VOC against the same verb
set**. If verb resolution ever diverges, the superset property breaks.

A shell being just a cataloged program means users can write their own —
restricted menu shells, application shells, batch drivers — for free.

### 6.2 Verbs are BASIC, the shell is C

Classic TCL in C implements only the shell: dispatch, VOC lookup,
command-line parsing, select-list plumbing. Verbs (`LIST`, `SORT`,
`BASIC`, `CATALOG`, `SELECT`) are compiled BASIC programs. This bounds
the frozen C surface to a dispatch engine rather than a hundred commands,
and both shells inherit the same verb set automatically.

### 6.3 Builtins vs VOC verbs

Builtins are shell-internal — they manipulate session state or need
unparsed input, so they cannot be expressed as verbs taking parsed
arguments:

- `!` / `SH` — raw passthrough to Unix
- `LOGTO` — account switching
- `OFF` / `QUIT` — session termination

Dispatch order: builtin table, then VOC lookup, then not-found.

Because builtins are compiled in, they cannot be added or removed by
editing a VOC — which closes the self-granting account problem for these
commands.

### 6.4 The `BASIC` verb

A short BASIC program: resolve `filename itemname` to a source record,
spawn the `mvx-basic` compiler, parse the result, report errors in classic
format. It reimplements nothing.

Preserve the classic split: **`BASIC` compiles to an object; `CATALOG`
links and publishes.** Keeping linking in `CATALOG` keeps each verb
simple and matches expected behaviour.

### 6.5 Bootstrapping order

```
compiler -> runtime + storage -> classic TCL (C) -> leaf verbs in BASIC
  -> EXECUTE hardening -> select lists -> VOC -> XTCL in BASIC
```

Every stage is runnable. `EXECUTE` is the pivot — a BASIC shell cannot
exist until nesting, output capture, select-list passing, and error
propagation are solid, and classic TCL's real job is to stress `EXECUTE`
into shape.

### 6.6 Session state classification

Needed for both nested shells and `EXECUTE` generally. Classify each piece
of state as:

- **Session-scoped** (inherited by nested shells and `EXECUTE`d programs):
  account, user, `@USERNO`, active select list.
- **Program-scoped** (fresh per invocation): `@`-variables, `COMMON`, open
  file handles.

---

## 7. Unix integration

### 7.1 TCL as a login shell

Set the user's shell to the TCL binary; they land at a TCL prompt and log
on to an account, Pick-style. Requirements commonly missed:

- Must handle `-c command` — `ssh user@host command`, cron, and `scp` all
  invoke the login shell non-interactively.
- Must handle login-shell invocation (`argv[0]` beginning with `-`).
- Should be listed in `/etc/shells` — `chsh` and some daemons require it.
- Terminal and signal handling: job control, SIGHUP on disconnect, `stty`
  state and cleanup for full-screen verbs.
- Clean exit must release held record locks — a dropped SSH session must
  not orphan locks.

### 7.2 Admin invocation

`cd` to an account directory and run `mvx`. Account context comes from
the working directory rather than a logon prompt.

Resolution order: explicit `-a account` flag, then `MVXACCOUNT` env var,
then cwd if it looks like an account, then prompt. Account context is a
**parameter, not a mode** — both entry paths converge on the same
downstream behaviour, so keep the resolution in one place.

### 7.3 Container deployment

- **App containers are stateless**; a session runs entirely within one
  container. Scale by running many independent sessions, not by spreading
  one program across containers.
- Config via env vars (driver, backend URL, lock manager) so one image
  runs under Compose on LMDB and under Kubernetes on a networked backend.
- Health and readiness endpoints for probes.
- **Graceful shutdown must release distributed locks on SIGTERM.**
- Two seams to abstract early even while running in-process: the **lock
  manager** and **session/select-list storage**. With those, single-host to
  multi-host is a configuration change rather than surgery.

---

## 8. Security model

### 8.1 The gate lives in the runtime

Anyone who can compile a BASIC program can call whatever the runtime
exposes. Restricting `!` in the shell alone is decorative — a user writes
three lines of BASIC and gets a shell. **The privilege check sits inside
the runtime's exec primitive**, evaluated at call time against the current
session, not at compile time (a program compiled by a developer may be run
by a restricted user).

One gate covers every path: the C shell's `!` builtin, XTCL's equivalent,
a user's own program, and the `BASIC` verb's compiler spawn.

It returns an error the BASIC program can handle (`STATUS()`, `ELSE`
branch) rather than killing the process.

### 8.2 Privilege tiers

| Tier | Unix exec | Compile / catalog |
|---|---|---|
| restricted | no | no |
| developer | no | yes |
| unrestricted | yes | yes |

Default is **deny**, so a misconfigured account is restrictive rather than
a hole.

The developer tier needs compile and link as **separate narrow
primitives** — not general exec with careful quoting. A narrow
`compile(item)` primitive takes structured arguments and builds the
command line itself, so there is nothing to inject.

Be honest in documentation: developer tier is not a hard boundary if the
runtime ever grants general exec.

### 8.3 Configuration placement

The privilege flag lives in **system-level config outside the account**.
If it lives in a record the account can write, it is self-granting.

### 8.4 Privileged verbs

An application may legitimately need to run a specific external command
while its users have no general Unix access. The admin whitelists the
program.

Three constraints make this safe, and they are painful to retrofit:

1. **Whitelist the command, not just the program.** The grant is "program
   X may exec *these specific commands*", validated by the runtime at
   spawn. A program granted `backup.sh` cannot spawn `sh`.
2. **Spawn argv-style, never through a shell.** With `execve`-style
   argument vectors, shell metacharacters in user input are inert — they
   become literal argument text. This eliminates the injection class
   structurally rather than by sanitising.
3. **Bind the grant to program identity, not to a name.** Otherwise a user
   catalogs their own program under the blessed name and inherits the
   grant. Re-cataloging a whitelisted program should invalidate the grant
   or require re-approval.

**Mechanism (mvx_perm.c + `OSEXEC`).** A BASIC program runs one external
command with `OSEXEC(argv[, capture])`, where `argv` is an FM-delimited
dynamic array — field 1 the command, the rest its arguments. It spawns
argv-style (constraint 2), so metacharacters in argument fields are inert.
At any tier below `unrestricted` the command is checked against grant lines,
scoped by an identity that matches the caller's OS **groups** or **username**,
plus `*` for anyone:

```
permit <who> = mkdir tar rm mkpkg      # these commands, any arguments
permit *     = uname                    # * = any user/group
deny   <who> = rm -r -R --recursive     # ...but never rm WITH those switches
deny   *     = shutdown                  # ...and never this command at all
```

A `permit` grants a command; a `deny` is a hard override that wins over any
permit — with no switches it blocks the command outright, with switches it
blocks it only when one is present. Switch matching handles **bundled short
options** (a deny of `-r` also blocks `-fr`) and **long options**
(`--recursive`, `--recursive=…`); list whichever forms a command accepts.
Commands match by basename (`tar` covers `/bin/tar`).

Grants are unioned from three files, in increasing authority:

1. `<account>/.mvx` — the packager's declaration. A new account is **seeded**
   from the system account's `.mvx` (`mkaccount.sh` copies its `permit`/`deny`
   lines), so it starts with the site baseline.
2. `<account>/.mvx-private/permissions` — the account's site policy
   (git-ignored, file-permission protected).
3. `<system>/.mvx-private/permissions` — the **system-account layer**
   (`$MVXSYSTEM`, else `MVX_SYSTEM_DIR`): the admin's per-user/group override,
   outside every account (8.3). Because a `deny` wins globally, this layer can
   lock a command or switch down for a user/group regardless of what an account
   grants itself — the account files can only narrow, never escalate past a
   system deny.

**Program-identity binding (constraint 3).** A grant may name a *program*
instead of a user/group — `permit prog:MVPKG = mkdir tar mkpkg` — which applies
for any user, but only when the running verb's binary matches the one **blessed**
under that name. Blessings live only in the system layer, so a user cannot
self-bless:

```
# <system>/.mvx-private/programs
program MVPKG = <sha256-of-approved-binary>
```

The runtime sha256s the running binary and compares. Re-cataloging the program
changes its binary and thus its hash, so the grant stops matching until the
admin re-blesses the new hash — a user who catalogs their own program under the
name `MVPKG` gets a different hash and inherits nothing.

This gives least privilege for OS-touching primitives — a program that only
needs `mkdir`/`tar` is granted just those, not full `!`/SH.

**Native primitives.** The common install-time ops are also built in, so a
package does them with no external command (and no shell): `MKDIR(path)`,
`RMTREE(path)`, `UNTAR(tarball, dest)`. They are still mutating, so below the
unrestricted tier each needs a `permit` for its op name (`mkdir`, `rmtree`,
`untar`) — which a site grants to the admin/dev groups that install packages —
while `UNAME(which)` (read-only platform info, for picking a prebuilt artifact)
stays ungated, as `OSREAD` is. Paths are used as-is, never through a shell.

### 8.5 Container mode

In a Kubernetes deployment the container's Unix environment is minimal and
disposable, so escaping to a shell inside it is far less serious than on a
shared host. Containers provide a second boundary beneath the TCL one —
a good reason not to over-engineer the first.

---

## 9. Source control integration

Git has superseded everything else, and classic Pick has no equivalent.
The answer differs by artifact type, and conflating them is the mistake.

### 9.1 Source code (BP) — directory is truth

Source lives in a directory. Git owns it natively: checkout, branch, and
merge just work with **zero sync logic**. The `BASIC` verb reads source
from the directory, exactly as UniData does with DIR-type files.

MV verb compatibility (`ED`, `COPY`, `LIST` against BP) comes from a
**directory-backed storage driver** — `read` is a file read, `write` a
file write, `select` a readdir. That is roughly 200 lines and it is
already part of the pluggable design.

Ignore-patterns in the directory driver handle foreign files that other
tools drop into the directory (dotfiles, `.git/`, known non-source
extensions).

**Rejected**: hash-file-as-truth with a filesystem watcher mirroring to a
directory. Git rewrites the directory wholesale with no notification, so
every checkout, merge, and branch switch creates a divergence to
reconcile backwards. Bidirectional sync has an inherent conflict problem
with no correct answer — the class of bug that produces "conflicted copy"
files. It requires inotify/FSEvents handling, missed-event recovery,
write-rename editor semantics, recursive event storms during checkout, and
a conflict policy — a substantial subsystem existing only to work around
the wrong choice of source of truth.

### 9.2 VOC and dictionaries — hash file is truth, Git tracks an export

VOC and dictionary items are **live operational data**, not just source.
Every verb dispatch reads VOC; every query reads the dictionary. They need
hash-file performance, MV locking, and transactional consistency, so they
must not become directory-backed.

They are also configuration-as-code that belongs in version control.

**Resolution**: they are export/import artifacts, not synced files.
`EXPORT` writes each record as a text file in a Git-tracked directory;
`IMPORT` reads them back. Movement between store and repository is
explicit — the same shape as `pg_dump`/`psql` or a database migration
tool.

Explicit beats a watcher here specifically because a `git checkout` that
silently rewrote VOC would change verb dispatch underneath a running
session, possibly mid-command. Operational data wants a human-triggered,
transactional, validated apply.

Requirements:

1. **A diff-friendly serialisation format** — attribute per line,
   deterministic ordering, one file per record named by record id. If the
   format does not diff and merge cleanly, Git gives you nothing. This is
   the real design work.
2. **Transactional, validated import** — parse and validate everything
   (does the cataloged program exist? is the dictionary item well-formed?)
   then apply atomically. A failed import leaves VOC untouched.
3. **A status/diff mode** showing which records differ between directory
   and hash file, so a checkout can be reviewed before it is applied.

**Rejected**: layered or overlay filesystems. Overlays create ambiguity
about which copy is live and reintroduce divergence with extra
indirection. The hash file is always live; the directory is always a
snapshot.

### 9.3 Summary

| Artifact | Source of truth | Git |
|---|---|---|
| BP / source | directory | native, no sync |
| VOC / dictionaries | hash file | exported text, explicit `EXPORT`/`IMPORT` |
| Data files | backend store | not tracked |

---

## 10. Multi-dialect support (deferred)

Eventually, UniData-style flavour support. The architecture: one AST, one
runtime, dialect as a **declarative profile** consulted by the lexer,
parser, and lowering — not a forked frontend.

Dialects differ lexically (comment markers, separators), syntactically
(`LOCATE` argument order, `READU` variants, `MATREAD` forms), semantically
(0- vs 1-based dynamic array addressing, coercion rules, `@AM`/`@VM`
naming), and in intrinsics (`OCONV`/`ICONV` code coverage, `SYSTEM()`
returns).

Normalise dialect syntax into common AST nodes at parse time so the
backend never sees dialect. Differences that cannot be normalised become
compile-time flags or runtime parameters.

**Sequencing**: implement one dialect concretely and completely first. The
profile abstraction designed before a second implementation exists will be
the wrong abstraction.

Full cross-flavour source compatibility is genuinely hard at the edges —
`OCONV`/`ICONV` coverage, exact `@`-variable semantics, and `STATUS()`
codes are where real UniData and UniVerse spend enormous effort. Aim for a
common core; treat exotic conversions as a long tail.

---

## 11. Naming

- `mvx-basic` — compiler and CLI
- `mvx` — classic TCL shell (the interactive environment; MVX TCL)
- MVX BASIC — the language implementation
- XTCL — extended shell

Note: "MVX" is used elsewhere (MvvmCross shorthand in the .NET community,
an industrial operations platform, an AI orchestration product). None are
in the MultiValue, database, or compiler space, so there is no confusion
risk with the target audience — but searchability is mediocre and
`github.com/mvx` is likely taken. `mvx-lang` or `OpenMVX` are more
findable alternatives for the org and repository name.
