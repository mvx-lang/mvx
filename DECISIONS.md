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
- **The cmd package** (mvx-lang/mv_cmd) is the Cobra-shaped command
  framework: CMD.INIT / CMD.ADD / CMD.RUN over a named COMMON, with
  generated help and CALL @ handler dispatch. mvx-lang/mv_git is the
  reference consumer. Both are separate products, installed with MVPKG;
  mvx carried them as submodules until #169.
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
  every tier; compiling needs developer. Raw Unix (`!`, SH, OSEXEC)
  below unrestricted is not all-or-nothing: a plain single command runs
  iff the permit whitelist (`mvx_perm.c`) grants it, argv-style; a shell
  string (pipes, redirection, substitution, globs, chaining) still needs
  unrestricted. So a confined account (e.g. the mvpkg package account)
  runs exactly its declared command surface — the `.mvx` vendor permit —
  and nothing else. A denial returns a negative status the caller
  surfaces as an error: the TCL exits 126, so a BASIC `EXECUTE ...
  RETURNING code` sees it and can fail in-program. All spawns are
  argv-style (`execv`), never through a shell, except the
  unrestricted-only raw passthrough.
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

## Language transactions (mvx#247)

A program can bracket several writes so they commit or roll back as one unit,
extending the per-write guarantee of mvx#244 (one mapped write — record,
parent columns and child rows — is already atomic) across statements.

- **The spelling is `TRANSACTION START` / `TRANSACTION COMMIT` /
  `TRANSACTION ABORT`.** Verified by compiling and running probes on the real
  systems, not from documentation: UniData **and** UniVerse both accept this
  form, jBASE spells it `TRANSTART` / `TRANSEND` / `TRANSABORT`. Two of three
  agree, and the agreeing pair is the pair MVX code is most often ported from.
  Classic Pick, normally the tie-breaker, has no transactions to arbitrate
  with.
- **No `BEGIN TRANSACTION … END TRANSACTION` block.** UniVerse offers one and
  it looks like the tidier construct, but the shape real code uses is not
  lexical: on UniData at Gentrack, CueBic started the transaction in a
  pre-save, called subroutines that wrote records, and committed at the top of
  the next screen — all one process, never inside one block. A block form
  would not have expressed that program, so it would have been decoration.
- **`START` and `COMMIT` take `THEN`/`ELSE`; `ABORT` takes neither.** This is
  UniData's and UniVerse's rule exactly. An abort has no failure a program
  could branch on — discarding is best effort by definition — so a clause on
  it would be a clause that never fires.
- **A second `START` is refused, not nested** — which agrees with UniData and
  diverges from UniVerse. Measured: on UniData 8.3 the second `START` takes
  the `ELSE` and `@TRANSACTION` stays 1; on UniVerse 14.2.1 three successive
  `TRANSACTION START`s all succeeded and each `ABORT` unwound one level.
  Supporting that means savepoints in the backend and a partial rollback, and
  nothing has asked for it; refusing is honest, where pretending to nest and
  rolling the whole thing back on the inner `ABORT` would be silently wrong.
  Revisit if a real program needs it.
- **`@TRANSACTION` is a depth, and must keep working as a boolean.** It reads 0
  outside and 1 inside on MVX and UniData. On UniVerse the non-zero value is
  neither 1 nor stable: measured on 14.2.1, a single `TRANSACTION START`
  answered 3, then 4, then 7, 8, 9 on successive runs — it is a monotonically
  increasing transaction *number*, with nesting counted on top of it (three
  nested `START`s gave n, n+1, n+2, unwinding back to 0). So MV code written
  `IF @TRANSACTION` is not merely acceptable, it is the only portable reading;
  `IF @TRANSACTION = 1` works on UniData and silently never fires on
  UniVerse. MVX answers a depth rather than a flag only so it can grow if
  savepoints ever arrive. jBASE has no `@TRANSACTION` at all — the compiler
  says `Unknown @ system constant @TRANSACTION specified` — and exposes the
  same question as `TRANSQUERY()`.
- **One transaction, one connection.** A file spec is `"<location>\n<file>"`,
  so a connection is (driver, location); the first write inside a transaction
  enrols one and a write to any other is **refused**. Committing each backend
  separately would be atomic per backend and not overall — worse than
  refusing, because nothing afterwards could tell.
- **A refused write poisons the transaction**, and `COMMIT` then fails and
  rolls back what did enrol. Otherwise the guarantee would be worth nothing in
  the case it exists for: `START`, write A, write B refused, `COMMIT` — and A
  commits alone, which is the half a unit of work the feature exists to
  prevent. The refused write already reported its own failure, but a program
  is entitled to handle that by logging it and carrying on, and doing so must
  not be able to leave a partial commit behind. An explicit `ABORT` remains
  the way to give up deliberately.
- **A backend with no bracket refuses the write and says so.** `sqlite`,
  `postgres` and `mysql` implement `bulk_begin`/`bulk_commit`/`rollback`;
  `dir`, `lmdb`, `lmdbnet` and `mongo` do not. A write to one of those inside
  a transaction is diagnosed on stderr and fails — soft under `ON ERROR`,
  fatal without. A transaction must never be silently downgraded to a series
  of independent writes, because the program has already been told it has one.
  (For `mongo` this is the driver, not the server: a replica set can hold a
  transaction, and the ops could be added later without anything above the
  driver changing.)
- **Rollback happens on the way out, via `atexit`.** A crash or a kill is
  already safe: the connection dies and the backend discards the uncommitted
  transaction (proven for sqlite in mvx#244). `STOP` is not, because
  `mvx_stop` is `exit(0)` and never reaches the store's shutdown. This matters
  because in the CueBic pattern an explicit `ABORT` is never written at all —
  the abort path *is* abnormal termination, and UniData is relied on to do it
  for you.

**Deliberate divergence: an `EXECUTE`'d child joins the transaction.** Neither
jBASE nor UniData does this. Measured on jBASE 6.2.1.1, a child reached by
`EXECUTE` reports `TRANSQUERY` = 0 while the parent still reads 1; measured on
UniData, a child `COUNT` of a file the parent wrote inside a transaction
answers "0 record(s) counted" — a stale view, silently wrong rather than an
error. MVX scopes the transaction to the connection on `store_state` rather
than to a program level, so once `EXECUTE` runs in-process (mvx#248) the child
sees the parent's writes and is covered by the same commit. This is the one
place MVX deliberately improves on both.

## Changing account from a program (mvx#258)

`LOGTO` used to be a builtin of `mvx` and nothing else, so a site that replaces
TCL with its own login and menu — the point of mvx#248 — was bound to the
account its shell started in. An operator picking a company or a division from
a menu is exactly this, and CueBic did it.

- **`LOGTO` changes account and RETURNS; it does not end the program.**
  Measured on UniData 8.3 and UniVerse 14.2.1: a program that prints, does
  `EXECUTE "LOGTO <acct>"`, then prints again prints both lines, and an
  `EXECUTE "WHERE"` afterwards reports the new account. So a shell can offer a
  menu of divisions and stay in its loop.
- **`EXECUTE "LOGTO ..."` is a caller too, not just the intrinsic.** It is the
  spelling that already works on both those systems, so ported code uses it.
  Here it used to find no VOC entry, fall back to spawning `mvx -c`, move a
  *child* that immediately exited, and leave the caller where it was without a
  word — the worst kind of failure, a silent no-op.
- **Deliberate divergence: the old account's files are CLOSED.** On UniData
  **and UniVerse** a handle opened before the `LOGTO` still reads afterwards —
  measured on both against a file that exists only in the account being left,
  so it is the handle surviving and not a file of the same name in the new
  account. There a handle is a path to a physical file, which exists whatever
  account you are in.

  MVX cannot follow, for a reason those systems do not have: **a handle here
  does not name a file.** `spec` is `"<location>\n<file>"`, and for a
  directory or an unbound LMDB file the location is empty and the driver
  re-reads `$MVXACCOUNT` on every call. A handle that survived would not
  dangle — it would *follow the session into the new account* and silently
  read that account's file of the same name, which is worse than either
  alternative. That is what the `g_file_gen` generation counter prevents.
  Keeping one also costs a database connection rather than a file descriptor,
  and mvx#251 measured where that ends: a session walking ten accounts could
  open nothing from the ninth onward.

  Matching them would mean the handle capturing an absolute,
  account-independent location at OPEN time — a change to what a spec is, not
  to `LOGTO`. mvx#267 holds the evidence and the two levers.
- **An open transaction refuses the move**, with `STATUS()` = 2. Committing it
  after the account changed would commit into somewhere the program no longer
  is, and discarding it silently is worse. Same answer mvx#247 gives when one
  transaction would reach two connections.
- **Three callers, one implementation** — the intrinsic, `EXECUTE`, and the
  `mvx` builtin all reach `mvx_logto`. The hard part is letting the old
  account go *before* entering the new one, and a second copy of that would
  drift.
- **A `LOGIN` hook, on entering an account** (mvx#264, settled after this).
  Both systems run the target account's VOC `LOGIN` on the way in — a fresh
  login, a `LOGTO` from TCL, and a `LOGTO` from inside a program — and *not*
  around each `BASIC` or `RUN`. MVX matches that: `mvx` startup and
  `mvx_logto`, so a cataloged program run straight from Unix still pays
  nothing and `docs/replacing-tcl.md` stays true.

## The account's own setup: LOGIN (mvx#264)

- **One spelling, `LOGIN`.** The two systems disagree and one prefers a name
  the other ignores. Measured with `PA` paragraphs printing distinct banners:
  UniData 8.3 keys on `LOGIN` and nothing else — neither the account name in
  either case, nor the Unix user name, runs anything. UniVerse 14.2.1 honours
  `LOGIN` *and* a record named after the account, and **the account-named one
  wins**: with both present only `bench` ran. `LOGIN` is the only spelling
  that works on both, and adopting UniVerse's precedence would mean an
  operator's new `LOGIN` losing silently to a record they cannot see.
- **An account-named login is not portable anyway.** It cannot survive a
  rename, which is exactly what the open account format does when the same
  tree is checked out under another name.
- **It is a `V` record naming a program**, because MVX has neither paragraphs
  nor PROCs. On UniData a cataloged program cannot be a `LOGIN` at all —
  `CATALOG` there writes to `CTLG`, not `VOC`, and creates no VOC record — so
  the portable idiom on those systems is a paragraph whose body runs a
  program. The hook itself does not care: it executes the *sentence* `LOGIN`,
  and both systems invoke paragraphs and PROCs the same way (measured:
  `EXECUTE "MYPARA"` and `EXECUTE "MYPROC"` both run from BASIC). So if MVX
  grows paragraphs or PROCs, `LOGIN` gets them with no change.
- **The account's own, never inherited.** Not from a linked package, not from
  the system account every account sits behind — one there would run in every
  account on the machine, silently, with nothing naming its origin. An account
  sets itself up; it does not set up its neighbours.
- **A failing `LOGIN` does not fail the move.** By the time it runs the
  session IS in the new account, so reporting failure would leave the program
  believing it is somewhere it is not. An abort is caught rather than taking
  the caller with it — a menu must not die because an account it moved to has
  a broken setup.
- **A `LOGIN` that `LOGTO`s does not start another one.** It is itself a
  program and may move; two pointing at each other would otherwise never stop.

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
