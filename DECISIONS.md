# MVX — Settled decisions

## Slice 2 — storage

- **The driver contract lives in `mvx_driver.h`** and is exactly the
  minimal set from ARCHITECTURE.md 4.1: open/close, read/write/delete,
  select (snapshot cursor). Locks are NOT in the contract — they live in
  the runtime lock table (`mvx_store.c`), keyed by file spec + record
  id, because READU can span user think-time and must never pin a
  backend transaction.
- **Drivers are dlopen'd shared libraries** (`libmvxdrv_<name>.dylib`),
  loaded on first use via the single exported entry point
  `mvx_driver_entry(int abi)` with an ABI-version handshake
  (`MVX_DRIVER_ABI`). Backend dependencies link into the driver library
  — liblmdb is a dependency of `libmvxdrv_lmdb`, not of compiled
  programs, so it loads exactly when the driver does. Search path:
  `$MVXDRIVERS` (colon-separated), then the built-in driver directory
  baked in at build time. A missing or ABI-incompatible driver is a
  loud fatal error, not an OPEN ELSE — configuration breakage must not
  masquerade as a missing file.
- **File resolution**: account root is `$MVXACCOUNT` (default cwd). A
  spec naming an existing directory opens the directory driver
  (attributes ↔ lines, one record per file — the git-native shape);
  anything else is a named DB in the account's LMDB environment at
  `<account>/mvxdata.lmdb`. One env per account, one named DB per file,
  short transactions, copy-out reads, 511-byte key validation.
- **File variables** are a fifth value tag (`MV_FILE`), holding the
  driver handle pointer; handles are context-owned and closed at exit.
  WRITE releases the record lock, WRITEU keeps it — both after a
  successful driver write.
- **Dictionaries are sibling stores, resolved by naming convention.**
  `OPEN "DICT","X"` opens `DICT.X` (LMDB named DB) or `X.DICT`
  (a sibling directory beside the directory file `X`, so `BP` and
  `BP.DICT` sit side by side and match the git-legible form). Every
  statement then works on a dict handle
  unchanged, because a dictionary is just another record store.
  `CREATEFILE` creates DICT and DATA together, classic style;
  `DELETEFILE` removes both. Dictionary *semantics* (D-items driving
  LIST/SELECT and indexing) build on this in later slices.
- **File creation is explicit.** `OPEN` never creates; a nonexistent
  file takes ELSE, classic style. The driver contract carries handle-
  less `create`/`remove` operations, surfaced in BASIC as
  `CREATEFILE(spec {,"DIR"})` and `DELETEFILE(spec)` — these are the
  primitives the CREATE-FILE / DELETE-FILE verbs will wrap when TCL
  arrives, since verbs are BASIC programs, not C.
- **The account chooses its default hash backend.** `CREATE-FILE <name>`
  with no explicit type creates the account's default hash file; the
  built-in default is local lmdb, but the `.mvx` (or open-checkout
  `.mv-account`) descriptor may set `hash = <spec>` — a CREATE-FILE type
  such as `lmdb` or `USING postgres @pgmain` — to change it, so a whole
  account can standardise on one backend. An explicit type on the verb
  always overrides. On checkout of an open account (where a file's class is only
  `hash`) the tool prompts for the concrete type and can record the choice as
  this default — but that prompt is **per-platform**: MVX chooses a backend
  driver (lmdb / postgres / …), UniVerse chooses a hashed-file type (static
  2–18, dynamic ~30 — usually just the default, ~18/19), and UniData has no
  choice at all (hash and dir only, so `udt-git` never prompts). The `hash =`
  default holds whichever the platform uses.

## Indexing (ARCHITECTURE.md 5)

- **Indexes are an optional driver capability** (ABI 3): `write_ix` /
  `del_ix` apply a record write/delete and its index deltas in ONE
  transaction — the no-drift guarantee of 5.1 — plus `index_select`
  (NULL = no such index, distinct from empty) and `index_drop`. LMDB
  implements them as `<spec>.IDX.<item>` named DBs with MDB_DUPSORT;
  the directory driver declines the capability, so CREATE-INDEX
  refuses there.
- **Metadata is the DICT record `%INDEXES%`** (indexed item names, one
  per attribute), cached per open file and invalidated by the
  build/drop entry points. Extraction runs the dictionary attribute
  number; multivalues emit one entry per value; empty values and keys
  beyond the backend limit are not indexed.
- **Maintenance is diff-based** (5.3): the write path reads the old
  record, extracts old and new values per indexed item, and touches
  only entries that changed.
- **Only D-type attribute items are indexable** — the TRANS() rule of
  5.4 applied to what exists today: a computed item (I-type) may
  depend on content outside the stored attributes and would go
  silently stale, so CREATE-INDEX refuses it. Index maintenance stays
  local to a single record write in a single backend.
- **Queries use indexes transparently**: LIST/SELECT try
  INDEXSELECT(file, item, value) for an equality WITH on a D-item when
  no list is active, falling back to a scan; the result feeds the same
  select-list machinery either way.

## Networked daemon (ARCHITECTURE.md 4.3)

- **mvx-lmdbd owns its LMDB environment exclusively** and serialises client
  access over unix-socket or TCP; a file is either embedded-access or
  daemon-owned, never both. The daemon speaks raw record bytes — MV
  semantics stay in the client runtime — and links only liblmdb.
- **Deployment is the promised config swap, and migration is per
  file** (4.4): a file's backend is named at creation. `CREATE-FILE
  name USING <driver> {params}` records a binding in the account's
  `BINDINGS` record ("SPEC driver {params}" lines, `*` for all, exact
  wins) and creates the file through that driver; DELETE-FILE removes
  the binding with the file. The driver name — `lmdbnet` today,
  `postgres`/`mongo` later — is the type, deliberately not "remote":
  transport is a property of the driver, not a separate axis. Params
  are opaque to the runtime, carried in the driver-level spec as
  "params\nspec" and parsed by the driver. Bare `$MVXDAEMON` with no
  BINDINGS record binds the whole account to lmdbnet. LISTF shows each
  file's driver by name. The daemon address travels inside the
  driver-level spec ("addr\nspec"), so lock keys and index metadata
  stay distinct across daemons, and `lmdbnet` keeps one connection
  per daemon. Directory files always stay local. Binding is
  resolution only — existing data does not move.
- **The daemon is the single lock authority** for its files: the
  driver contract gains an optional lock capability (ABI 4); when
  present, READU acquires from the backend (blocking with retry,
  classic style) instead of the process-local table. Locks are leased
  to the connection — a client that dies without RELEASE loses its
  locks the moment the connection drops, so killed pods cannot orphan
  record locks. Proven in the harness.
- **SELECT snapshots inside the daemon, then sends**: the read
  transaction closes before a byte hits the wire, so a slow client
  never pins pages.
- Accepted knowingly, per the architecture: single point of failure,
  single-writer ceiling, and the HA story is ours to build. Protocol
  integers are host-order — same-architecture clients for now.

## Slice 3 — TCL

- **The C shell is dispatch only.** `mvx` implements the prompt,
  the builtin table (OFF/QUIT/BYE, `!`), VOC lookup, and fork/exec of
  cataloged executables — nothing else. Verbs are compiled BASIC
  programs in `CATALOG/`, named by VOC records (attr 1 `V`, attr 2
  executable path). Dispatch order: builtins, account VOC, system VOC,
  not-found.
- **CALL binds at runtime — the jBASE catalog model.** Compiled CALLs
  dispatch through `mvx_call`, which resolves `mvx_sub_<NAME>` from
  symbols already in the process (multi-source builds still work),
  then loads cataloged subroutine libraries from the account's `LIB/`,
  each linked package's `LIB/`, and the system `LIB/` (dlopen,
  RTLD_GLOBAL, on first miss). `CALL @VAR` takes the name from a
  variable — dispatch tables, and therefore frameworks, work. CATALOG
  detects a SUBROUTINE source and catalogs it into `LIB/` as a shared
  library instead of making a verb; mkpkg.sh does the same for
  packages. The subroutine ABI is unchanged — this is resolution
  policy, not calling convention.
- **The cmd package** (packages/cmd) is the Cobra-shaped command
  framework: CMD.INIT / CMD.ADD / CMD.RUN over a named COMMON, with
  generated help and CALL @ handler dispatch. packages/git is the
  reference consumer.
- **Packages are account-shaped directories** (`BP/` source, `VOC/`
  verb records, `CATALOG/` executables — built by `scripts/mkpkg.sh`)
  linked into an account by the LINK-PKG / UNLINK-PKG / LIST-PKGS
  verbs, which maintain the account's `PACKAGES` record (one path per
  attribute, edited through the directory driver — the account root is
  itself a directory file). TCL resolution: builtins, account VOC,
  linked packages in listed order, system VOC. The package list
  reloads when PACKAGES changes, so LINK-PKG takes effect in the same
  session. Package verbs execute from their own CATALOG but run in the
  linking account.
- **Standard verbs live once, in the system account** (SYSPROG-style).
  The build compiles `verbs/*.b` into `build/system/CATALOG` and the
  master VOC is a directory file kept in the repo (`system/VOC`, one
  text record per verb — configuration-as-code, diffable). Accounts
  hold only local VOC entries; local overrides system on lookup.
  System verbs execute by absolute path but run in the user's account
  (cwd), so they operate on account data. `$MVXSYSTEM` overrides the
  baked-in system location. Reading the master VOC needs no special
  machinery: it is an absolute-path directory file, which the existing
  directory driver already serves.
- **The sentence crosses via the environment**: TCL sets
  `$MVX_SENTENCE`; the `SENTENCE()` intrinsic reads it. Verbs parse
  their own arguments with FIELD().
- **The privilege gate lives in `mvx_exec.c`, in the runtime.** One
  gate covers every spawn path: TCL's `!`/SH builtins, EXECUTE, and the
  compiler. Tiers per 8.2 (restricted < developer < unrestricted,
  default deny) come from `$MVXPRIV` — the development stand-in for
  system config outside the account; the property that matters is that
  account data cannot write it. Spawning cataloged verbs is allowed at
  every tier; raw Unix needs unrestricted; compiling needs developer.
  All spawns are argv-style (`execv`), never through a shell, except
  the explicitly-unrestricted raw passthrough.
- **EXECUTE spawns `mvx -c`** so there is exactly one dispatcher in
  the system. CAPTURING collects stdout as a dynamic array (line ↔
  attribute); RETURNING receives the exit status (deviation from
  classic error-number lists, documented). Select-list passing across
  EXECUTE is deferred until session-state classification (6.6) exists.
- **Select lists cross processes through the session file.** `mvx`
  owns `$MVXSESSION` (created only when not inherited, so nested
  EXECUTE shares the outer session). A program exiting with an
  unconsumed select list persists the remainder there; the next
  program's first READNEXT consumes it, exactly once. `SYSTEM(11)`
  reports whether a list is active; query verbs use the active list
  instead of re-selecting, classic style. This is the session/
  select-list seam of ARCHITECTURE.md 7.3 — replacing the file with a
  session service is a config change, not surgery.
- **LIST and SELECT are BASIC verbs** driven by dictionary D-items
  (1=D, 2=attr#, 3=OCONV conversion, 4=heading, 5=format "12L"/"8R").
  WITH filters, BY sorts via ordered LOCATE insertion — using AR
  (numeric) ordering when the BY item's dict format is R-justified.
  SELECT installs its filtered ids with FORMLIST and exits, leaving
  the list for the next command.
- **`COMPILE(mode, src, out)`** is the narrow developer-tier primitive
  behind the BASIC and CATALOG verbs: structured arguments, argv built
  by the runtime, nothing to inject. BASIC compiles `FN ITEM` to
  `FN.O/ITEM.o`; CATALOG links to `CATALOG/ITEM` and writes the VOC
  entry — compile and publish stay separate verbs, classic style.
- **Account = parameter, not mode**: `-a` flag, then `$MVXACCOUNT`,
  then cwd; the shell chdirs to the account and children resolve
  relative to it. `-c` runs one sentence for ssh/cron use.

# Slice 1 decisions

Concrete resolutions of the two open decisions in `ARCHITECTURE.md` §3.3,
plus the smaller choices they force. These are load-bearing: the ABI ones
are permanent once separately compiled subroutines exist.

---

## Decision A — value representation

**Chosen: boxed value with numeric tags (option 1), plus compiler numeric
specialisation (option 3) — both implemented.**

The specialisation layer (in `compiler/src/codegen.cpp`, `NumericAnalysis`):
a scalar or DIM'd array is specialised when every value stored into it is
a provably numeric expression and it never escapes by reference (CALL
argument / subroutine parameter). The analysis is a fixed point over the
lattice Int < Dbl < NotNum:

- **Int tier** — provably integral: bare `i64` alloca, native integer
  ops. Division always yields Dbl (MV `/` is fractional); `INT()` is the
  idiom that brings a quotient back to the Int tier. Known deviation:
  `i64` arithmetic wraps on overflow where boxed arithmetic promotes to
  double — accepted for Slice 1.
- **Dbl tier** — provably numeric: bare `double` alloca, native FP ops.
  Boxed arithmetic already promotes through double and compares
  numerically via double, so this tier is exact to 2^53.
- **Arrays** get a storage class: `i8` buffer when every store is an
  integer literal in 0..255 (flag arrays), `i64` for integral stores,
  `f64` for numeric stores, boxed otherwise.

Everything else falls back to the boxed representation below; boxing at
the seam uses `mv_set_int` for Int-kind values so printed output is
indistinguishable from the boxed path.

Sieve result (1M sieve, 5 s, Apple M-series): boxed-only 404 passes;
double tier 5,767; int/byte tier **14,509 vs 14,652 for the equivalent C
byte-array sieve — 99% of C** — with the frontend untouched throughout.
The value-representation bet ARCHITECTURE.md 3.3 makes is confirmed.

```c
typedef struct mv_string {          /* immutable, refcounted */
    int64_t refs;
    int64_t len;
    char    data[];                 /* NUL-terminated for convenience */
} mv_string;

typedef struct mv_value {
    int64_t    tag;                 /* MV_UNASSIGNED / MV_INT / MV_DBL / MV_STR */
    int64_t    i;                   /* valid when tag == MV_INT */
    double     d;                   /* valid when tag == MV_DBL */
    mv_string *s;                   /* owned ref when tag == MV_STR, else NULL */
} mv_value;                         /* 32 bytes, fixed layout — part of the ABI */
```

Key properties:

- **Numbers stay numeric.** `I = 5` sets `MV_INT`; arithmetic on two
  numeric tags never touches a string. Stringification happens lazily
  (PRINT, concat). This alone avoids the 100x string-round-trip cliff
  while remaining fully boxed and correct.
- **Strings are immutable and refcounted**, so copy/assign is a retain,
  not a heap copy.
- **Field layout is frozen and known to the compiler.** Codegen may load
  `tag`/`i`/`d` directly (fast paths) but all mutation goes through
  runtime calls. This is the seam where option 3 (type specialisation)
  plugs in later: the IR emitter works through a `ValueRef` abstraction so
  a provably-numeric variable can become a bare `i64` alloca without
  touching the parser or AST.
- Numeric string comparison follows MV rules: if both operands look
  numeric, compare numerically; otherwise byte-wise string compare.
- `MV_UNASSIGNED` coerces to 0 / "" with a runtime warning to stderr
  ("zero used", classic Pick style), not a hard error.

## Decision B — subroutine ABI (permanent)

```c
void mvx_sub_<NAME>(mvx_ctx *ctx, int32_t argc, mv_value **argv);
```

- **Hidden context parameter first, always** — present from day one even
  though Slice 1 only uses it for output state. Session state, locks, and
  the privilege gate ride on it later without an ABI break.
- **Every argument is `mv_value*`** pointing at the caller's slot —
  `CALL SUB(A, B)` is by-reference, callee mutation is visible to the
  caller. A non-lvalue argument (expression, literal) is materialised
  into a caller temp and passed by pointer; mutation of it is legal and
  discarded, matching MV behaviour.
- **Arity is checked at runtime, at call entry** (`argc` vs declared
  count); mismatch is a fatal runtime error naming the subroutine.
  Traditional MV defers arity failure to runtime; we keep that but fail
  fast and loud. Link-time checking can be layered on later without an
  ABI change since `argc` stays in the signature.
- **Name mangling: `mvx_sub_` + subroutine name as written** (MV names
  are conventionally uppercase; the name is taken verbatim from the
  `SUBROUTINE` statement). Flat C namespace, `dlsym`-friendly.
- Main programs compile to `void mvx_main(mvx_ctx *ctx)`; a tiny runtime
  crt provides the real `main()`, creates the context, calls `mvx_main`.

## Smaller settled choices

- **Reference dialect: traditional Pick BASIC** (classic Pick / R83
  style). Wherever MV platforms diverge, classic Pick behaviour is the
  tie-breaker: `IF ... THEN ... END ELSE ... END` block form, `=`/`#`
  comparators, `LOOP`/`UNTIL`/`WHILE`/`DO`/`REPEAT`, 1-based `DIM`,
  warn-and-zero on unassigned variables, PRECISION 4 output. Later MV
  extensions are admitted only where classic Pick has no equivalent
  (e.g. `SYSTEM(12)` millisecond clock for benchmarking, since classic
  `TIME()` is whole seconds). **C-style comments** (`/* */` and `//`)
  are an admitted extension: both are impossible token sequences in
  valid classic code (after `/` the grammar requires an operand), so
  there is no ambiguity, and docblocks need no `*` prefix per line.
  Guards: newlines inside a block comment still terminate statements
  (stripping them would quietly invent line continuation), and the
  known cost is one-way portability — MVX source using them will not
  compile on legacy MV platforms; legacy source never contains them,
  so imports are unaffected. Numeric statement labels, `GOTO`/`GO TO`,
  and `GOSUB`/`RETURN` are implemented: labels compile to basic blocks,
  GOSUB keeps a 1024-deep return stack dispatched on RETURN, and RETURN
  with an empty stack ends the program (or returns to the caller in a
  subroutine). `STOP` terminates the whole program even from inside a
  subroutine. FOR-loop state lives in stack slots rather than SSA values
  so jumps into loop bodies stay well-formed; mem2reg promotes them back
  in label-free code, so the sieve pass rate is unchanged.
- **Dynamic arrays** live in the boxed string representation (marks
  0xFE/0xFD/0xFC). `A<a,v,s>` parses by attempting the extraction and
  backtracking to less-than when it does not close with `>`; subscripts
  parse at additive precedence, so comparisons inside subscripts need
  parentheses — the same resolution classic MV compilers use.
  Assigning through `A<...>` demotes the base from the numeric tiers,
  since the value then carries marks. `BEGIN CASE` desugars to a nested
  IF chain in the parser; there is no CASE node in codegen.
- **COMMON is context-owned, positional, always boxed.** Blocks (unnamed
  and `/NAME/`) live in `mvx_ctx`, so all programs in a process share
  them through the hidden context parameter — no process-global state.
  Slot storage is chunked and never realloc'd: compiled code binds slot
  addresses once at function entry, so addresses must stay stable as
  later programs extend a block. COMMON variables never specialise.
- **Runtime is C11** (clean frozen ABI, no C++ mangling in the contract);
  the compiler is C++17 against the LLVM C++ API.
- **Arrays**: `DIM A(n[,m])`, 1-based, bounds-checked, elements are
  `mv_value` slots. A distinct `mv_array` heap object, not a dynamic
  string.
- **Timing intrinsics**: `TIME()` → integer seconds since midnight;
  `SYSTEM(12)` → milliseconds since midnight (jBASE/UniVerse-compatible),
  which is what the sieve's 5-second loop uses.
- **DWARF**: emitted always (no `-g` flag needed to opt in),
  `DW_LANG_BASIC`, one `DISubprogram` per program/subroutine, line table
  against the `.b` source.
- **Driver**: `mvx-basic -c prog.b -o prog.o` (object), `mvx-basic prog.b -o prog`
  (compile+link executable), `mvx-basic -shared sub.b -o libsub.dylib`.
  Errors to stderr as `item:line: message`.
- **I-type evaluation is a runtime primitive** (#63): `TRANS`/`DOCTAG`
  descriptors are evaluated in the runtime (`mvx_ieval` / `mvx_dict_eval`,
  exposed to BASIC as `IEVAL(rec, ispec)`), and the query verbs
  (LIST/SELECT/SORT/SSELECT) call it instead of each carrying a duplicate
  evaluator — one evaluator, no drift.
- **Nested TRANS is an MVX extension beyond classic R83.** Classic `TRANS`
  takes a numeric attribute and returns a *raw* attribute; chaining was done
  with further correlatives. MVX additionally lets the target argument name a
  **dictionary item** in the target file, which is evaluated through that
  file's dictionary — so if it is itself an I-type it recurses
  (`TRANS(CUST,1,REGIONNAME,X)` where `REGIONNAME` is another `TRANS`). A
  numeric target keeps the exact classic behaviour, so this is purely
  additive; a depth cap stops a self-referential dictionary looping. This is
  the one place the "classic is the tie-breaker" rule is deliberately
  extended, because the classic form (a bare attribute number) cannot express
  a chained lookup at all.
- **Open account format** (record-git cross-platform interchange). A MultiValue
  account is stored in git in a portable, backend-neutral form so that a
  clone/checkout by one build (`mvx-git`) rebuilds into a live account on another
  (`udt-git` on UniData). It is **opt-in** via a git config flag, the
  `core.autocrlf` analogue: `mvx.openaccount = true` in the account's
  `.git/config` (surfaced to the runtime as `$MVX_OPENACCOUNT`).
  - **The open form lives only in git objects; on disk the account is always
    native.** On MVX the working tree is a real MVX account (`.mvx`, native
    `%FILE%` = `FILE <VM> type <VM> conn`, lmdb/directory files); on UniData a
    native UniData account. The open form is never written to disk — the
    record-git **engine translates at the git boundary**: on commit it writes
    open blobs *from* the native account; on checkout it builds the native
    account *directly* from the open blobs (no intermediate open-form files, no
    checkout-then-convert); `status`/`diff` translate the on-disk native records
    *up* to the open form before comparing against the open blobs. This covers
    the records too: a record's git blob is a legible **hybrid** form (marks ↔
    newlines) so it browses and diffs on GitHub, but that form never lands on
    disk — checkout materialises it straight into the backing store (the hash
    file, directory file, …).
  - **`.DICT/%FILE%` is `DIR` or `hash` only** in git (the portable file class,
    connection dropped): `dir → DIR`, `lmdb`/any hash backend → `hash`; the
    reverse on checkout maps `hash` to the account's default hash backend and
    `DIR` to the directory driver. A per-file remote binding is a *local*
    concern (BINDINGS), never in the portable form.
  - **`VOC`/`MD`** carry the classic portable `F` file pointers (attr 1
    `DIR`/`hash`, attr 2 data, attr 3 dictionary) — see CREATE-FILE (mvx#71).
  - **Dictionaries** travel as legible records so another MV system rebuilds
    them — this is what makes the account exportable off MVX. The canonical
    **open-dict** schema (and the `.mv-account` open-account descriptor) are
    specified in the [Open Dict & Account Interchange](https://github.com/mvx-lang/mvx/wiki/open-dict)
    wiki page, with a round-trip prototype in `tests/opendict.b`. This includes
    the dict controls: `%FILE%`
    (normalised to DIR/hash, above) and **`%INDEXES%`**,
    the portable list of indexed item names. The index *structures* are derived
    (never committed) and rebuilt on checkout (BUILD/CREATE-INDEX), so an index
    moves with the account. A platform without an on-disk `%INDEXES%` record
    (UniData, like `%FILE%`) generates it **virtually** in git on commit and
    consumes it on checkout — so indexes move both ways.
  - **The git descriptor is `.mv-account`** (the on-disk native `.mvx` maps to
    it, and back on checkout). It identifies the directory as an account and
    holds what a native build needs, plus `openaccount = <version>`. UniData has
    no on-disk descriptor, so `udt-git` reads `.mv-account` transiently to build
    the account and synthesises it on commit. An account may sit at the **repo
    root** (e.g. mv_git, mv_eb) or in a **subdirectory** of a larger repo (some
    mvx accounts); the descriptor marks it wherever it is, and a repo may carry
    more than one.
  Without the flag the engine stores the platform's own legible form (native
  `%FILE%`), i.e. current behaviour. The flag lives in git config so it travels
  with the clone, set once per account, like `autocrlf`. *(Landed (mvx#25/#73):
  the flag and `mvx.openaccount → $MVX_OPENACCOUNT` plumbing; the engine boundary
  translation — commit native→open, checkout open→native directly, status/diff in
  open-space — for records, `%FILE%` (DIR/hash + `hash =` default), legible
  dictionaries (open-dict), `%INDEXES%`, and the `.mvx` ⇄ `.mv-account` descriptor
  (a real conversion, not a rename: the portable form drops MVX-local `permit`/
  `deny` policy, which is re-established locally and never shipped in git); and
  the udt-git UniData ⇄ open converter. One descriptor schema, shared by both
  builds via `mv_git_desc_open`.)*

## Portable MV BASIC (udt / D3 / UniVerse ports)

- **`LOCATE` — Format 1 (parenthesized) only.** In every MultiValue BASIC we
  ship (mvpkg, json, git, …), use only the parenthesized form:
  - `LOCATE(x, arr; pos)` — attribute-level
  - `LOCATE(x, arr, amc; pos)` — value-level within attribute `amc`
  - `LOCATE(x, arr, amc, vmc; pos)` — subvalue-level
  - optional trailing `; "AL"|"AR"|"DL"|"DR"` for sorted-insert position.

  Do **not** use Format 2, the `LOCATE x IN arr<amc> SETTING pos` statement form.
  It is flavour-dependent: verified on Rocket UniData 8.x, `LOCATE x IN R<f>
  SETTING p` searches the **attribute** level of the whole array (it does *not*
  drill into field `f`'s values the way UniVerse's identical syntax does), and a
  bare `IN arr` with no subscript is a compile error. Format 1 is level-explicit
  and behaves identically across UniData/D3/jBASE/UniVerse. Revisit the
  `IN…SETTING` form later, per-flavour, once flavour support is in scope.
