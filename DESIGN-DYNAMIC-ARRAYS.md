# Dynamic arrays: elements as the truth, bytes as a cache

A design for mv_git#130 — *"a Pick program that keeps its working set in a
dynamic array should not fall off a cliff."*

Status: **proposal**. Nothing here is built. The measurements are real.

---

## 1. Where we are

The banked sieve, five seconds, count validated at 78,498 on every platform:

| platform | flat | banked | cost of banking |
| --- | --- | --- | --- |
| MVX, before | 13,660 | 75 | 182x |
| **MVX, after the snprintf fix** | **13,405** | **144** | **93x** |
| jBASE 6.2.1 `-O4` | 216 | 7 | 31x |
| UniData 8.3 | 66 | 35 | 1.9x |

Halving the cost of banking took removing one thing that was never needed:
every dynamic-array edit converted its value to characters with
`snprintf("%lld")`, and the sieve did that **58,486,425 times in five seconds
to produce the single character `0`**.

UniData pays 1.9x because its interpreter overhead already dominates. MVX pays
93x because the numeric path is genuinely fast and the dynamic-array path is
genuinely not. **That ratio is the target, not the absolute number** — we are
already 4x UniData's absolute score on this benchmark.

### What the profile says now

Exclusive samples, banked sieve, after the fix:

```
mv_replace_fn   1792   (72%)     ← inlines locate() and inplace_repl()
ix_for           106
memmove           88
modify            86
val_span          61
mv_arr_elem       56
```

### Two things that are *not* the problem

Both were measured, because both are the obvious guess:

- **The fast path already works.** 58.5M in-place same-length hits against
  2,343,750 rebuilds — and those rebuilds are exactly `31250 banks × 75
  passes`, the first write to each bank after `MAT BANK = ONES`. That is
  copy-on-write doing its job.
- **The element index never engages.** A bank is ~31 bytes; `IX_MIN_BYTES` is
  128. #130 suspected index-dropping was the cost. On this benchmark there is
  no index to drop.

So the remaining 72% is not scanning and not rebuilding. It is **per-call
overhead on an operation that should not need a call at all**: convert the
value to bytes, walk to the element, patch a byte, in a shared library, 58
million times.

---

## 2. The proposal

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

| # | Stage | Expected | Risk |
| --- | --- | --- | --- |
| 0 | *done* — remove the `snprintf` round-trip | 75 → 144 | none, shipped |
| 1 | *done* — `mv_str_bytes()` / `mv_str_wbytes()` + the grep that enforces them | 145 (unchanged, as intended) | none; the guard was proved by breaking it |
| 2 | `dyn` built on first subscripted write; bytes materialise on demand | the 72% | stale-bytes bugs if a site is missed |
| 3 | Elements hold native values | the value round-trip | MV type-equality rules |
| 4 | Revisit `IX_STRIDE` / `IX_MIN_BYTES` with numbers | small | none |

Stage 1 is worth doing **even if we stop there**: it makes the byte
representation a thing with one door, which is what any future change to it
needs.

---

## 5. Alternatives considered

**Strengthen the existing index** (#130's own suggestion 1–3: adjust offsets
on edit instead of dropping, extend on append, add a sequential cursor).
Cheaper and lower risk. Rejected as the primary plan because **it does not
touch this benchmark at all** — the banks are below the index threshold — and
because it leaves the value round-trip in place. Worth doing for long fields
independently.

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

- Stage 2 does not beat 144 by a clear margin on the banked sieve.
- The flat sieve regresses at all — 13,405 is the number that says the numeric
  fast path is intact.
- Memory on a realistic record set grows enough to matter on a machine with
  gigabytes of it — which is a far higher bar than the 1975 one, and is about
  blast radius rather than economy (3.4).
- Any stale-bytes bug that the suite does not catch, found by hand. That would
  say the accessor discipline is not enforceable, and the design rests on it.
