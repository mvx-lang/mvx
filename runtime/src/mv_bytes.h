/* mv_bytes.h — the one door to a string's bytes.
 *
 * Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
 *
 * WHY A DOOR AT ALL.  Today an mv_string IS its bytes and these are both
 * `return st->data`.  The plan is that it stops being that: a dynamic array
 * becomes an element structure, and the flat bytes become a CACHE that is
 * regenerated when something actually asks for them (DESIGN-DYNAMIC-ARRAYS.md).
 *
 * On that day, every place that reads ->data behind the runtime's back returns
 * STALE BYTES -- silently, and only for values that happened to be edited by
 * subscript first.  There is no worse shape of bug: it is data-dependent, it
 * looks like corruption rather than a logic error, and the suite would find it
 * only by luck.
 *
 * So the door goes in FIRST, while both sides are still `st->data` and the
 * change is provably behaviour-neutral -- and scripts/test.sh fails the build
 * if ->data appears anywhere else.  A rule nothing checks is a rule that decays,
 * and this one has to hold across 41 sites in 6 files.
 *
 * INLINE, not a call.  The replace path is 72% of the banked sieve; making its
 * byte access a cross-translation-unit call would pay for the discipline in the
 * one place that cannot afford it.
 */
#ifndef MV_BYTES_H
#define MV_BYTES_H

#include "mvx_runtime.h"

/* The bytes, current.  Read-only: a caller that means to CHANGE them wants
   mv_str_wbytes, which is where the element view will be invalidated. */
static inline const char *mv_str_bytes(const mv_string *st) {
    return st->data;
}

/* The buffer, to write into.  Callers own the string (refs == 1) and are
   building or patching its bytes; this is where DYN_CURRENT gets cleared once
   there is an element view to clear. */
static inline char *mv_str_wbytes(mv_string *st) {
    return st->data;
}

#endif /* MV_BYTES_H */
