# Dynamic arrays: elements as the truth, bytes as a cache

A design for mv_git#130 — *"a Pick program that keeps its working set in a
dynamic array should not fall off a cliff."*

Status: **the fast path is built; the representation change is not.** The
work took a different route from the one proposed here. Section 1 says where
things stand and how they got there; sections 2 and 3 are the original
proposal, kept because its stages 2 and 3 are still open.

---

## 1. Where we are

The banked sieve, five seconds, count validated at 78,498 on every platform:

| platform | flat | banked | cost of banking |
| --- | --- | --- | --- |
| **MVX now** | **14,011** | **321** | **44x** |
| MVX after the snprintf fix (Aug) | 13,405 | 144 | 93x |
| MVX before (Aug) | 13,660 | 75 | 182x |
| UniData 8.3 | 68 | 35 | 1.9x |
| UniVerse 14.2 | cannot run | 35 | — |
| OpenQM (ScarletDME, `-O2`) | 46 | 19 | 2.4x |
| jBASE 6.2.1 `-O4` (Aug) | 216 | 7 | 31x |

Measured September 2026 unless marked. MVX is a Release build on an Apple M1
Max, best of interleaved runs; a C version of the flat sieve scores **14,387**
on the same machine, so MVX is at **97% of C**. ScarletDME ran in a Linux VM on
the same machine, UniData and UniVerse on x86 VMs; the same C sieve agrees to
within 1.6% across all of them. UniVerse cannot run the flat sieve because it
caps a compile-time `DIM` at 64,000 elements, which is why the banked version
exists.

`bench/sieve-dynamic.b` keeps all 500,000 flags in **one** dynamic array, the
shape a Pick programmer writes when the working set is one thing. It scores
**818** (79 in August), more than twice the banked version.

UniData pays 1.9x for banking because its interpreter overhead already
dominates. MVX still pays 44x because its numeric path is close to C, so
anything else shows. **That ratio is still the measure**, and it has halved
again since the snprintf fix.

### How it got here

Each step was measured on the day it landed. The figures come from the commit
messages, and a baseline can drift by a few per cent between commits.

| step | banked | one array | where |
| --- | ---: | ---: | --- |
| before | 75 | 79 | |
| integers formatted without `snprintf` | 144 | | #144 |
| every byte access through `mv_str_bytes()` / `mv_str_wbytes()` | 145 | | #144 |
| the index engages on 8 bytes, not 128 | 165 | | #144 |
| a level's index found by arithmetic, not a list walk | 175 | | #144 |
| an offset per element boundary, not one per 16 | 186 | 412 | #144 |
| copy-on-write keeps the index | 203 | 506 | #144 |
| a one-byte replace stored without `memmove` | | 549 | #144 |
| `X<1,v>` on a value with no attribute mark skips the AM lookup | 291 | 791 | #144 |
| **now** | **321** | **818** | |

Alongside those: ordered `LOCATE` became a binary search once the order is
verified (7,874 to 1,867,440 lookups a second on a sorted 20,000-element list,
#144); `COUNT` and `DCOUNT` of a single mark read the index (#145); `INSERT` and
`DELETE` at the top level move the tail once and shift the index rather than
rebuilding it (119,520 to 1,958,040 operations a second, #146); and the `SUM`
family stopped copying and `strtod`-parsing plain integers (#147).

**This is the "strengthen the existing index" alternative that section 5
rejected.** It was rejected on the grounds that the index never engaged at bank
size. That was a true observation and the wrong conclusion: the fix was to make
it engage. Dropping `IX_MIN_BYTES` from 128 to 8 alone took 145 to 165.

Element reads and writes now cost **5 to 8 nanoseconds at any length and in any
access order** (a one-character-per-element list from 16 to 65,536 elements).

### What the profile says now

macOS `sample`, banked sieve, September 2026:

```
75%   the replace path: mv_replace_fn with inplace_repl, locate and
      ix_for inlined, plus val_span
15%   malloc/free, almost all from cow_keep_index
 9%   mv_arr_elem
```

About 7% of samples sit in PLT stubs, because the runtime is a shared library.

The 15% is the first write to each bank after `MAT BANK = ONES`: 31,250 banks
a pass, each copying its string and then allocating an index set plus one block
per level (`cow_keep_index`, `runtime/src/mv_dyn.c`).

### What is still open

- **The call itself.** With the subscripted write reduced to a call that does
  nothing, the single-array sieve scored 1,063 against 532 with the work in
  (#144). Past that needs the call to go, which is compiler specialisation
  (ARCHITECTURE.md 3.3 option 3), not a runtime change.
- **Copy-on-write allocations.** One allocation for the whole index set would
  replace up to four per shared string.
- **`LOCATE` reaches its field and value with `nth_span`**, not the index
  (`mv_locate_fn`), so `LOCATE(X, REC<5>; …)` on a long record rescans fields 1
  to 4 on every call.
- **The read side.** `IF BANK(B)<1,P> = 0` extracts into a reused temp and then
  parses the one-byte string to compare it. Native element values (3.5) are
  aimed at exactly this.
- **Elements as the truth** (section 2) was never built. Whether it still pays
  is an open question now that the byte path costs single-digit nanoseconds;
  the call and the parse above are the larger costs.

### How the others compare

From the same September measurements (`bench/*.b`, ported only where a timer
needed it):

| | MVX | ScarletDME | UniData | UniVerse |
| --- | ---: | ---: | ---: | ---: |
| `INSERT` + `DELETE`, 2,000 items, ops/s | 2.0M | 95k | 650k | 1.25M |
| ordered `LOCATE`, 20,000 items, lookups/s | 2.0M | 7.8k | 20k | 12k |
| `X<1,K>` read at 65,536 elements, in order | 8 ns | 58 µs | 62 ns | 30 µs |
| `X<1,K>` read at 65,536 elements, random order | 5 ns | 74 µs | 17 µs | 36 µs |

- **ScarletDME** (the GPL descendant of OpenQM 2.6) keeps one field-level hint,
  rescans a field from its first byte to reach a value, rebuilds the whole
  string on every replace except an owned append, and formats every stored
  integer with `sprintf("%d")` — 15% of its banked profile.
- **UniData** is constant time for in-order access and linear for random
  access, which looks like a cached cursor rather than an index. For the usual
  `FOR I = 1 TO DCOUNT(...)` loop, MVX's lead over it is mostly compiled against
  interpreted.
- **UniVerse** has a field-level position cache like ScarletDME's, and its
  `INSERT`/`DELETE` is within 1.6x of MVX.
- The ordered `LOCATE` figures are mostly binary search against linear scan. An
  unordered `LOCATE` in MVX is still a linear scan.

---

## 2. The proposal (not built)

This was written before any of section 1 was built, and describes a change that
has not been made. Its measurements are the August ones: at 144 passes, the
replace path (`mv_replace_fn`, with `locate` and `inplace_repl` inlined) was
**72%** of the banked sieve's samples, and that is the 72% referred to below.

> Store a dynamic array as an indexed object, and materialise the flat string
> only when something actually needs the bytes.

`R<1,5> = X` should be an array store. Today it is a string edit that happens
to be one byte long.

### The shape

`mv_value` does not change — the ABI passes `mv_value *` and that is settled
(ARCHITECTURE.md 3.3 Decision B). `mv_string` already carries an optional
`mv_ix *ix`; this replaces that hook with a fuller one:

```c
typedef struct mv_string {
    int64_t refs;
    int64_t len;        /* meaningful only when bytes are current */
    int64_t cap;
    mv_dyn *dyn;        /* elements, when there are any */
    unsigned flags;     /* BYTES_CURRENT | DYN_CURRENT */
    char    data[];
} mv_string;
```

Either side may be stale, never both:

- `BYTES_CURRENT` — `data[0..len)` is the value.
- `DYN_CURRENT` — the element structure is the value.

A read of the bytes materialises from `dyn`; a subscripted write updates `dyn`
and clears `BYTES_CURRENT`. A value that is only ever written and read by
subscript never serialises at all.

### Why this is feasible here, specifically

The byte representation is far less exposed than it looks:

```
direct ->data uses in the runtime:  41, across 6 files
references to mv_string in codegen:  0
```

Generated code never touches an `mv_string`; it calls entry points. So
"materialise before anyone sees bytes" is enforceable at **41 call sites**,
not scattered through the compiler. That is the single fact that makes this a
weekend-shaped change rather than a rewrite.

---

## 3. The hard parts

None of these is a reason not to do it. All of them are reasons to do it in
stages with a benchmark between each.

### 3.1 Materialisation points must be exhaustive

A missed one returns stale bytes — the worst class of bug, because it is
silent and data-dependent. Every one of the 41 sites becomes
`mv_str_bytes(st)` rather than `st->data`, and `->data` becomes off-limits
outside the accessor. **Enforce it mechanically**: a grep in `scripts/test.sh`
that fails the build if `->data` appears outside `mv_str.c`. The suite has
learned this lesson repeatedly — a rule nothing checks is a rule that decays.

Known materialisation points: file writes, `OCONV`/`ICONV`, `PRINT`, passing
to a cataloged subroutine, `LEN`, substring `X[s,l]`, comparison, and anything
crossing the driver contract.

### 3.2 Substrings and LOCATE need bytes to point at

`X[s,l]` counts characters across the whole value, marks included. `LOCATE`
compares element text. Both are cheap on a flat string and awkward on a
structure. Simplest honest answer: **these materialise**. They are not the
inner loop of a write-heavy program, and a design that keeps every operation
fast usually keeps none of them correct.

### 3.3 Copy-on-write must cover both sides

`refs > 1` currently means "copy the bytes before editing". With two
representations it means copy whichever is current, and the `MAT BANK = ONES`
case shows this is a real path taken 2.3M times per run — 31250 banks sharing
one string until each is first written.

### 3.4 Memory

An element vector costs ~16 bytes per element against ~2 bytes of flat text for
the sieve's banks — **8x**.

**This matters less than it first looks, and the reason is historical.** Pick's
representation was designed when memory was counted in kilobytes, and packing a
record into the fewest possible bytes was the whole game. A machine now has
gigabytes. An 8x multiplier on a working set that is measured in megabytes is
not the squeeze it would have been in 1975, and designing around it as though it
still is means keeping a 1975 trade-off long after the thing it traded against
stopped being scarce.

So: **take the memory.** Build `dyn` on first subscripted write and keep it.

The index that was built instead makes the same trade: an offset per element
boundary costs eight bytes an element, and it bought 175 to 186 on the banked
sieve over one offset every sixteen elements. It is still built only when
something subscripts the value.

Two bounds stay, and neither is about saving bytes for their own sake:

- A value that is never subscript-written never builds one, so reading a large
  record costs exactly what it costs today.
- A cap on element count, high enough never to be met by working data, so a
  pathological value (a 10M-element array from a bad parse) degrades to flat
  rather than exhausting the machine. That is a blast radius, not an economy.

### 3.5 Native element values

The natural extension, and the second half of the user's proposal: an element
holds `{tag, i, d, bytes}` rather than always text. `R<1,5> = 0` then stores
an integer and never formats anything; `IF R<1,5> = 0` compares integers.
This is where the *rest* of the 72% goes.

It also inherits MV's type rules exactly: `"0012"` and `12` are different
strings and equal numbers, so an element must remember which it was given.
This is the same problem `mv_value` already solves — reuse it rather than
invent a second answer.

---

## 4. Staged plan

Each stage ships independently and is judged on the banked sieve plus the
suite. **Stop at any stage that does not pay.**

| # | Stage | Expected | Outcome |
| --- | --- | --- | --- |
| 0 | remove the `snprintf` round-trip | 75 → 144 | **done**, 144 |
| 1 | `mv_str_bytes()` / `mv_str_wbytes()` + the grep that enforces them | unchanged, as intended | **done**, 145; the guard was proved by breaking it |
| — | *not in the original plan:* engage, reach and keep the index (section 1) | — | **done**, 145 → 291; 321 now |
| 2 | `dyn` built on first subscripted write; bytes materialise on demand | the 72% | not started; the 72% is now mostly the call |
| 3 | Elements hold native values | the value round-trip | not started; still aimed at the read-side parse |
| 4 | Revisit `IX_STRIDE` / `IX_MIN_BYTES` with numbers | small | **done**: stride 1, minimum 8 bytes |

Stage 1 is worth doing **even if we stop there**: it makes the byte
representation a thing with one door, which is what any future change to it
needs.

---

## 4a. Prior art: uArray (gheydon/uarray, 2013)

This design is not speculative. It was built in PHP, for the same problem, by
the same person asking for it here — and the shape it settled on is the shape
above, with three refinements worth stealing outright.

```php
private $data = array();        // the elements — truth once split
private $output = NULL;         // the flat string — NULL when stale
private $needs_exploding = FALSE;
private $is_tainted = FALSE;
```

**Lazy in BOTH directions.** `$needs_exploding` means the string is not split
until something subscripts it; `$output === NULL` means the string is not
rebuilt until something stringifies it. A value read from a file, passed
along and written back whole is never split at all. That is the JIT bound in
3.4, and it was load-bearing there too.

**Taint propagates to the parent.** A nested `uArray` holds a reference to the
container that owns it, and marking a value dirty marks its field and the whole
record dirty. MVX's equivalent is `<a,v,s>`: an edit at the SM level invalidates
the VM split above it and the AM split above that. The C version needs this and
it is the part most likely to be got wrong, because a stale parent index is
exactly the silent-wrong-data failure of 3.1.

**A scalar is not an array.** `__toString()` returns `$data[0]` directly when
that is all there is, with the comment *"Don't cache the first 2 as it is not
worth it"* — most MV values are one value, and the machinery should not charge
them for the possibility of being more.

The PHP version could be relaxed about copying where the C one cannot; what
carries over is the state machine, not the implementation.

## 5. Alternatives considered

**Strengthen the existing index** (#130's own suggestion 1–3: adjust offsets
on edit instead of dropping, extend on append, add a sequential cursor).
Cheaper and lower risk. Rejected as the primary plan because **it does not
touch this benchmark at all** — the banks are below the index threshold — and
because it leaves the value round-trip in place. Worth doing for long fields
independently.

**This is what was built, and it paid** (section 1). The threshold was the
problem, not the approach: lowered to 8 bytes, the index engaged on every bank.
Offsets are shifted on `INSERT`/`DELETE` rather than dropped. The sequential
cursor was not needed: an offset per boundary makes every access direct.

**`memchr` for the element walk.** Tried, **measured, and reverted: 144 → 100
passes.** Elements are one character and a mark, so every scan is two or three
bytes and the vector setup costs more than the byte loop. Recorded in the
source so it is not "fixed" again.

**Compiler type specialisation** (ARCHITECTURE.md 3.3 option 3) for the
subscript path — emit the store inline where the array is provably local.
Larger, and it should come after the runtime representation is right; the two
compose.

---

## 6. What would make us stop

These applied to stage 2 of the original plan, and were written against the
August figures. Restated against today's:

- Stage 2 (or 3) does not beat **321** by a clear margin on the banked sieve,
  or **818** on the single-array sieve.
- The flat sieve regresses at all — **14,011** against C's 14,387 is the number
  that says the numeric fast path is intact.
- Memory on a realistic record set grows enough to matter on a machine with
  gigabytes of it — which is a far higher bar than the 1975 one, and is about
  blast radius rather than economy (3.4).
- Any stale-bytes bug that the suite does not catch, found by hand. That would
  say the accessor discipline is not enforceable, and the design rests on it.
