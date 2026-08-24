/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MV_INTERN_H
#define MV_INTERN_H

#include "mvx_runtime.h"

/* Runtime-private string services.  Generated code never sees any of this:
   it holds mv_value slots and calls the runtime, so mv_string is free to
   change shape (see codegen.cpp, "mutation goes through runtime calls"). */

/* Offset index over one mark level of one byte range of a string, built on
   demand by the dynamic-array code and dropped whenever the bytes move.
   Defined in mv_dyn.c, which is its only user. */
struct mv_ix;

/* Free and forget s->ix.  Called from every path that rewrites the bytes. */
void mvx_ix_drop(mv_string *s);

/* Allocate with capacity beyond the initial length, so that appending or
   growing in place can avoid the next allocation. */
mv_string *mvx_str_alloc_cap(int64_t len, int64_t cap);

#endif
