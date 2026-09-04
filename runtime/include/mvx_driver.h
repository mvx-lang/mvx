/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* MVX storage driver contract.
 *
 * THE MINIMAL CONTRACT IS THE WHOLE CONTRACT (ARCHITECTURE.md 4.1).
 * Application code must run correctly against exactly this interface;
 * nothing above the driver may depend on backend-specific behaviour.
 * Records cross this boundary as MV dynamic strings — marshalling to
 * and from backend formats happens inside the driver.
 *
 * Record locks (READU semantics) deliberately do NOT appear here: they
 * live in the runtime's lock table, keyed by file spec and record id,
 * because a lock can be held across user think-time and must never pin
 * a backend transaction.
 */
#ifndef MVX_DRIVER_H
#define MVX_DRIVER_H

#include "mvx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mvx_file mvx_file;       /* driver-owned handle */
typedef struct mvx_cursor mvx_cursor;   /* driver-owned select cursor */

/* One index maintenance operation: add or remove (key -> id) in the
   index named by item. */
typedef struct mvx_ixop {
    const char *item;
    const char *key;
    int64_t klen;
    int add;                            /* 1 = add entry, 0 = remove */
} mvx_ixop;

/* One column of a relational mapping (ARCHITECTURE / #18): the runtime
   computes the fields (backend-neutral) and the projected values; the
   driver renders them in its own form (SQL columns, document fields). */
typedef struct mvx_mapfield {
    const char *name;                   /* column / field name */
    const char *type;                   /* abstract type: TEXT/NUMERIC/... */
} mvx_mapfield;

/* One WITH condition for a multi-predicate push-down (select_multi). */
typedef struct mvx_pred {
    const char *col;                    /* mapped column, or NULL for the blob */
    int64_t attr;                       /* record attribute (when col NULL) */
    const char *op;                     /* "=", "#", ">", "<", ">=", "<=" */
    int numeric;                        /* compare numerically (range) */
    const char *val;
    int64_t vlen;
} mvx_pred;

typedef struct mvx_driver {
    const char *name;

    /* NULL on failure; err receives a message. */
    mvx_file *(*open)(const char *spec, char *err, size_t errlen);
    void (*close)(mvx_file *f);

    /* 1 = found (rec set), 0 = not found. */
    int (*read)(mvx_file *f, const char *id, int64_t idlen, mv_value *rec);

    /* 1 = ok, 0 = failure (bad id, backend error). */
    int (*write)(mvx_file *f, const char *id, int64_t idlen,
                 const mv_value *rec);

    /* 1 = deleted, 0 = did not exist. */
    int (*del)(mvx_file *f, const char *id, int64_t idlen);

    /* Snapshot the id list up front (short transaction), then stream. */
    mvx_cursor *(*select_begin)(mvx_file *f);
    int (*select_next)(mvx_cursor *c, mv_value *id);   /* 1 = got, 0 = end */
    void (*select_end)(mvx_cursor *c);

    /* File lifecycle: handle-less operations on the spec.  open() must
       NOT create — creation is explicit (the CREATE-FILE verb's
       primitive).  create returns 0 if the file already exists;
       remove returns 0 if it does not. */
    int (*create)(const char *spec, char *err, size_t errlen);
    int (*remove)(const char *spec, char *err, size_t errlen);

    /* Optional (may be NULL): enumerate the file specs this driver
       holds for the account, as an @AM-separated list.  Backing for
       the LISTF verb.  `loc` is the connection/location the runtime
       resolved for the account (e.g. "@conn" or a conninfo) so a
       backend that serves many locations knows which to list; a driver
       with a single implicit store (embedded LMDB, the daemon) ignores
       it.  DICT./index tables are included; the runtime filters them. */
    int (*names)(const char *loc, mv_value *out, char *err, size_t errlen);

    /* Optional indexing capability (ARCHITECTURE.md 5) — all four may
       be NULL if the backend has no native secondary indexes.  The
       record write/delete and its index deltas MUST commit atomically
       (one transaction): that is the no-index-drift guarantee.  Keys
       arrive already extracted; the driver stores (key -> record id)
       with duplicate keys allowed. */
    int (*write_ix)(mvx_file *f, const char *id, int64_t idlen,
                    const mv_value *rec, const mvx_ixop *ops, int nops);
    int (*del_ix)(mvx_file *f, const char *id, int64_t idlen,
                  const mvx_ixop *ops, int nops);
    /* NULL result = no such index (distinct from an empty cursor). */
    mvx_cursor *(*index_select)(mvx_file *f, const char *item,
                                const char *key, int64_t klen);
    int (*index_drop)(mvx_file *f, const char *item);

    /* Optional lock authority (may be NULL): a backend that arbitrates
       between sessions (the mvx-lmdbd daemon) grants and releases record
       locks itself; lock returns 0 while another session holds it.
       When NULL, the runtime's process-local lock table applies. */
    int (*lock)(mvx_file *f, const char *id, int64_t idlen);
    int (*unlock)(mvx_file *f, const char *id, int64_t idlen);

    /* Optional relational-mapping capability (may be NULL): project a
       mapped file's records into the backend's native relational form.
       The runtime supplies the columns and the per-record projected
       values; the driver materialises and persists them. */
    int (*map_ensure)(mvx_file *f, const mvx_mapfield *cols, int ncols,
                      char *err, size_t errlen);
    int (*map_apply)(mvx_file *f, const char *id, int64_t idlen,
                     const mvx_mapfield *cols, const char **vals,
                     const int64_t *vlens, int ncols);
    /* Association child tables (may be NULL): one table per association,
       rows keyed (id, seq).  map_child_apply replaces a record's rows;
       vals/vlens are row-major (nrows x ncols): cell (r,c) at r*ncols+c. */
    int (*map_child_ensure)(mvx_file *f, const char *assoc,
                            const mvx_mapfield *cols, int ncols,
                            char *err, size_t errlen);
    int (*map_child_apply)(mvx_file *f, const char *id, int64_t idlen,
                           const char *assoc, const mvx_mapfield *cols,
                           int ncols, const char **vals,
                           const int64_t *vlens, int nrows);
    /* Tear down a mapping: drop the parent columns and the association
       child tables (may be NULL). */
    int (*map_drop)(mvx_file *f, const mvx_mapfield *cols, int ncols,
                    const char **assocs, int nassocs, char *err,
                    size_t errlen);
    /* Native-read capability (may be NULL): read a mapped record back from
       the relational form, so external writes to the columns/child tables
       are visible.  map_read fills vals[i]/lens[i] for each parent column
       (vals[i] = NULL for SQL NULL); returns 1 if the parent row exists, 0
       if absent, -1 on error.  map_child_read allocates a row-major array
       of (*nrows * ncols) cells (and lens) for one association's rows,
       ordered by seq; returns 1 (including zero rows) or -1.  Every
       non-NULL string the driver returns is malloc'd; the runtime frees
       each cell, then *cells and *lens. */
    int (*map_read)(mvx_file *f, const char *id, int64_t idlen,
                    const mvx_mapfield *cols, int ncols,
                    char **vals, int64_t *lens);
    int (*map_child_read)(mvx_file *f, const char *id, int64_t idlen,
                          const char *assoc, const mvx_mapfield *cols,
                          int ncols, char ***cells, int64_t **lens,
                          int *nrows);
    /* Optional (may be NULL): build a native index for a mapped column in
       one shot — the backend already stores and maintains the column (via
       the mapping) and its own index, so there is no per-record backfill and
       no write_ix/del_ix maintenance.  A driver providing this advertises
       index_select/index_drop but leaves write_ix/del_ix NULL.  Returns the
       indexed row count, or -1 on error (e.g. the column is not mapped).
       `col` names a mapped identity column to index (NULL to index the raw
       record attribute `attr` via an expression index on the blob), so any
       dictionary field is indexable, not only mapped ones. */
    int (*index_create)(mvx_file *f, const char *item, const char *col,
                        int64_t attr);
    /* Optional server-side WITH push-down (may be NULL): the ids whose
       mapped column `col` satisfies `op` `val`, filtered in the backend so a
       query need not stream every record to the verb.  `op` is "=" or "#"
       (not-equal).  Uses the column's index if one exists, else a
       backend-side scan.  NULL result = cannot push down (caller scans). */
    mvx_cursor *(*select_where)(mvx_file *f, const char *col, const char *op,
                                const char *val, int64_t vlen);
    /* Optional server-side WITH push-down on a raw record attribute (may be
       NULL): the ids where attribute `attr` (1-based) of the stored record
       satisfies `op` `val`.  Unlike select_where this reads the attribute
       out of the record blob itself, so it needs no mapped column and is
       exact for any field type — at the cost of not using a column index.
       op is "=" or "#".  NULL result = cannot push down (caller scans). */
    mvx_cursor *(*select_attr)(mvx_file *f, int64_t attr, const char *op,
                               const char *val, int64_t vlen);
    /* Optional co-located TRANS() JOIN push-down (may be NULL): the source
       ids whose foreign key (source attribute `src_keyattr`) points at a
       target record whose attribute `tgt_attr` satisfies `op` `val`.  `tgt`
       is the opened target file; the driver runs the join only when it lives
       on the same backend/database as `src` (else NULL -> per-record TRANS).
       op is "=".  Turns an N+1 per-record lookup into one JOIN.
       src_keycol/tgt_col name a mapped identity column for the source key
       and the target attribute (NULL/"" to read it from the record blob);
       a real column can use an index and, in native mode, is authoritative. */
    mvx_cursor *(*select_join)(mvx_file *src, int64_t src_keyattr,
                               const char *src_keycol, mvx_file *tgt,
                               int64_t tgt_attr, const char *tgt_col,
                               const char *op, const char *val, int64_t vlen);
    /* Optional server-side COUNT (may be NULL): the number of matching
       records, computed in the backend so a filtered count returns one row
       instead of a stream of ids.  `op` NULL/"" counts all; otherwise it is
       "=" or "#" against a mapped identity column `col` (when set) or the raw
       record attribute `attr` (blob).  Returns the count, or -1 when it
       cannot push down (the caller counts by scanning). */
    int64_t (*count_where)(mvx_file *f, const char *col, int64_t attr,
                           const char *op, const char *val, int64_t vlen);
    /* Optional server-side SUM (may be NULL): the total of numeric column
       `sumcol`, written to `out` as text.  `fop` NULL/"" totals the whole
       file, else "="/"#" filters on a mapped identity column `fcol` (when
       set) or the raw record attribute `fattr`.  Returns 1 if pushed, 0 if
       not (the caller sums by scanning). */
    int (*sum_where)(mvx_file *f, const char *sumcol, const char *fcol,
                     int64_t fattr, const char *fop, const char *fval,
                     int64_t fvlen, char *out, size_t cap);
    /* Optional ORDER BY / LIMIT push-down (may be NULL): ids ordered by
       mapped column `ocol` (text ordered COLLATE "C" to match MV's byte sort
       when `otext`, else natural order), limited to `limit` rows (0 = all),
       optionally filtered like the other push-downs.  So a "top-N by field"
       fetches N ids server-side.  NULL result = cannot push (caller sorts). */
    mvx_cursor *(*select_order)(mvx_file *f, const char *fcol, int64_t fattr,
                                const char *fop, const char *fval,
                                int64_t fvlen, const char *ocol, int otext,
                                int64_t limit);
    /* Optional multi-condition WITH push-down (may be NULL): the ids matching
       every predicate (AND).  Each `mvx_pred` names a mapped column `col` or
       the raw record attribute `attr`, an op ("=","#",">","<",">=","<="), and
       whether to compare numerically.  NULL result = cannot push. */
    mvx_cursor *(*select_multi)(mvx_file *f, const mvx_pred *preds, int npred);
    /* Optional query-plan description (may be NULL): render — WITHOUT executing
       anything — how this backend would run a filtered/ordered id query.  For
       an SQL backend that is the SQL text; a document store would render its
       native query.  Inputs match select_multi (the AND'd predicates) plus an
       optional ORDER BY column (`otext` -> byte-order collation to match MV's
       sort) and LIMIT (0 = none).  Writes a NUL-terminated plan into `out`
       (truncated to `cap`); returns 1 if it rendered a server-side plan, 0 if
       it cannot (the caller then words the client-side fallback itself).  This
       is what backs the verbs' DESCRIBE modifier. */
    int (*explain)(mvx_file *f, const mvx_pred *preds, int npred,
                   const char *ocol, int otext, int64_t limit,
                   char *out, size_t cap);
    /* Optional (may be NULL): the number of ids a cursor snapshotted, so a
       long backfill can show a percentage.  select_begin captures the id list
       up front, so this is known at no extra cost.  NULL = total unknown. */
    int64_t (*select_count)(mvx_cursor *c);
    /* Optional bulk-write bracketing (may be NULL): begin/commit a transaction
       around a large sequence of writes on the file's connection, so a backfill
       pays one commit per batch instead of one per record.  The runtime calls
       bulk_begin, streams writes, and bulk_commit periodically and at the end
       (a commit on an already-failed transaction simply rolls it back).  NULL =
       the backend has no transactional batching (writes autocommit).

       bulk_begin RETURNS WHETHER IT STARTED ONE: 0 means a transaction was
       already open and this bracket is nested inside it, so the outer one
       carries the atomicity and only its owner may end it.  A driver that
       ignores this and issues an unconditional BEGIN corrupts the outer
       batch -- MySQL's START TRANSACTION silently COMMITS the open one. */
    int (*bulk_begin)(mvx_file *f);
    int (*bulk_commit)(mvx_file *f);
    /* Optional whole-mapping backfill (may be NULL): materialise a mapping in
       the backend in one statement (e.g. UPDATE base SET col = f(rec) over all
       rows) instead of the runtime reading and projecting every record over the
       wire.  cols (name+type), anos and convs describe each mapped field;
       assocs[i] is the association the field belongs to ("" = a parent column).
       Returns the number of records materialised, -1 on error (err filled), or
       MVX_MAP_NOPUSH when the transformation cannot be expressed server-side —
       the caller then falls back to per-record projection. */
    int64_t (*map_backfill)(mvx_file *f, const mvx_mapfield *cols,
                            const int64_t *anos, const char **convs,
                            const char **assocs, int nf, char *err,
                            size_t errlen);
    /* Optional co-located TRANS() ORDER BY push-down (may be NULL): the source
       ids ordered by the value TRANS() looks up — target attribute `tgt_attr`
       of the record its foreign key (source attribute `src_keyattr`) points
       at — limited to `limit` rows (0 = all).  Reproduces the per-record
       TRANS() reference exactly: the (possibly multivalued) key is unnested on
       @VM, each element translated, the results @VM-joined in key order (a
       miss -> "" for control 'X', the key itself for 'C'); the ids are ordered
       as text (COLLATE "C" when `otext`) to match MV's byte sort.  `ctl` is
       'X' or 'C'.  Runs only when `tgt` is co-located with `src` (else NULL ->
       the verb sorts the reference client-side).  src_keycol/tgt_col name a
       mapped identity column for the source key / target attribute (NULL/""
       to read from the record blob).  This is `select_join` (the WITH
       push-down) with an ORDER BY / LIMIT instead of a filter. */
    mvx_cursor *(*select_join_order)(mvx_file *src, int64_t src_keyattr,
                                     const char *src_keycol, mvx_file *tgt,
                                     int64_t tgt_attr, const char *tgt_col,
                                     char ctl, int otext, int64_t limit);
    /* Optional transaction abort (may be NULL): discard everything written
       since bulk_begin.  Without it a bracket can only ever COMMIT, so a
       write that fails half way -- the record stored but its mapping not, or
       an association holding some of its rows and not the rest -- would be
       committed in that state.  That is mapping drift, the same failure the
       index guarantee above forbids, and there is no way to detect it after
       the fact: the record and its projection simply disagree.

       A driver offering bulk_begin/bulk_commit SHOULD offer this too.  The
       runtime brackets one logical WRITE (record + parent columns + a child
       table per association) and calls rollback when any part of it fails.

       NESTING: bulk_begin returns whether it actually STARTED a transaction
       (0 = one was already open, e.g. a backfill batch surrounding this
       write).  Only the caller that started one may commit or roll it back;
       an inner bracket that returned 0 must do neither. */
    int (*rollback)(mvx_file *f);
} mvx_driver;

/* map_backfill sentinel: the transform is not expressible in this backend, so
   the runtime should fall back to the per-record projection loop. */
#define MVX_MAP_NOPUSH ((int64_t)(-0x7fffffffffffffffLL - 1))

/* Common header every driver embeds first in its mvx_file. */
typedef struct mvx_file_base {
    const mvx_driver *driver;
    char *spec;                         /* canonical spec, lock-table key */
} mvx_file_base;

/* Drivers are shared libraries (libmvxdrv_<name>.dylib / .so), loaded
   with dlopen on first use.  Backend dependencies (liblmdb, a database
   client, ...) are linked into the driver library, so they load with
   the driver and never burden compiled programs that don't use them.

   Every driver library exports exactly one entry point:

       const mvx_driver *mvx_driver_entry(int abi);

   It must return NULL if `abi` is not an ABI version it supports,
   otherwise its driver vtable.  The search path is $MVXDRIVERS
   (colon-separated), then the runtime's built-in driver directory. */
#define MVX_DRIVER_ABI 11

typedef const mvx_driver *(*mvx_driver_entry_fn)(int abi);

#ifdef __cplusplus
}
#endif
#endif /* MVX_DRIVER_H */
