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

/* mvxc.h — the MVX client library (#289).
 *
 * One API for an external program to use an MVX account: records, the values
 * in them, subroutines and sentences.  This is the surface mv-connect and any
 * language binding is built on, and the ONLY supported one -- mvx_runtime.h is
 * internal and happens to be installed.
 *
 * TWO RULES SHAPE EVERY DECLARATION HERE.
 *
 * 1. EVERY CALL IS A REQUEST AND A RESPONSE, AND EVERY RESULT IS OWNED BY THE
 *    HANDLE THAT RETURNED IT.  Nothing hands back a pointer into runtime
 *    memory, takes a callback, exposes a descriptor, or assumes the caller and
 *    the account share an address space.  This implementation is in-process;
 *    the rule is what lets a later one put a socket underneath without the API
 *    changing.  It costs nothing now and cannot be retrofitted.
 *
 * 2. NOTHING HERE ABORTS THE HOST.  The runtime calls mvx_fatal on a bad
 *    write; a library that kills a web server because a file is missing is
 *    unusable.  Every entry point returns a status and leaves the process
 *    standing.
 *
 * MV VALUES CROSS THE BOUNDARY AS AN OPAQUE mvxc_val, never as mv_value.  The
 * internal representation is four fields today and mvx#183's remaining work
 * changes it; freezing it here would block that or break every binding.  (It
 * stays public to EXTENSION authors, who recompile against each mvx release --
 * a binding does not.)
 */
#ifndef MVXC_H
#define MVXC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mvxc_status {
    MVXC_OK = 0,
    MVXC_NOTFOUND,   /* no such record, file or subroutine */
    MVXC_LOCKED,     /* held by another session */
    MVXC_DENIED,     /* the runtime privilege gate refused */
    MVXC_ERROR       /* anything else; mvxc_error() says what */
} mvxc_status;

typedef struct mvxc_session mvxc_session;
typedef struct mvxc_file    mvxc_file;
typedef struct mvxc_val     mvxc_val;

/* --- session ------------------------------------------------------------
 * `account` is the account directory.  NULL takes the process default, the
 * same one a verb would get ($MVXACCOUNT, else the working directory).
 * One session is one MV session: its select list, its locks, its account. */
mvxc_session *mvxc_connect(const char *account, mvxc_status *st);
void          mvxc_disconnect(mvxc_session *s);

/* The last failure on this session, "" when there has not been one.  Owned by
 * the session and replaced by the next failure. */
const char   *mvxc_error(mvxc_session *s);

/* --- values -------------------------------------------------------------
 * STRING LIFETIME, the one rule to know: every `const char *` an accessor
 * returns is owned by the value and stays valid until that value is MODIFIED
 * or FREED.  Two reads are therefore safe together --
 *
 *     printf("%s %s\n", mvxc_attr(r, 1), mvxc_attr(r, 2));
 *
 * -- which is the point; an accessor that invalidated the previous one would
 * be unusable for exactly the thing callers do most.  A set_* or ins_* call
 * releases them all. */
mvxc_val *mvxc_new(void);
mvxc_val *mvxc_new_str(const char *s);
mvxc_val *mvxc_from_bytes(const char *p, size_t n);
void      mvxc_free(mvxc_val *v);

const char *mvxc_str   (mvxc_val *v);                  /* the whole value */
const char *mvxc_bytes (mvxc_val *v, size_t *n);       /* @AM/@VM/@SM intact */
const char *mvxc_attr  (mvxc_val *v, int a);
const char *mvxc_val_at(mvxc_val *v, int a, int m);
const char *mvxc_sub   (mvxc_val *v, int a, int m, int s);

/* Values in attribute `a`; a <= 0 counts attributes instead.  An empty
 * attribute is ONE empty value, not none -- MV's own reading, and the thing
 * a caller written against DCOUNT expects. */
int mvxc_dcount(mvxc_val *v, int a);

void mvxc_set     (mvxc_val *v, const char *s);
void mvxc_set_attr(mvxc_val *v, int a, const char *s);
void mvxc_set_val (mvxc_val *v, int a, int m, const char *s);
void mvxc_ins_val (mvxc_val *v, int a, int m, const char *s);
void mvxc_del_val (mvxc_val *v, int a, int m);

/* --- files and records ---------------------------------------------------
 * `dict` is NULL for the data file, "DICT" for its dictionary. */
mvxc_file *mvxc_open (mvxc_session *s, const char *name, const char *dict,
                      mvxc_status *st);
void       mvxc_close(mvxc_file *f);

/* The caller owns what mvxc_read returns and frees it with mvxc_free.  NULL
 * with *st = MVXC_NOTFOUND is a miss, which is not an error. */
mvxc_val   *mvxc_read (mvxc_file *f, const char *id, mvxc_status *st);
mvxc_val   *mvxc_readu(mvxc_file *f, const char *id, int wait, mvxc_status *st);
mvxc_status mvxc_write (mvxc_file *f, const char *id, mvxc_val *rec);
mvxc_status mvxc_delete(mvxc_file *f, const char *id);
void        mvxc_release(mvxc_file *f, const char *id);

/* --- the select list -----------------------------------------------------
 * mvxc_next returns NULL at the end.  The id is owned by the SESSION and is
 * replaced by the next call, because that is what a remote cursor can
 * promise. */
mvxc_status mvxc_select(mvxc_file *f);
const char *mvxc_next  (mvxc_session *s);

/* --- running things ------------------------------------------------------
 * mvxc_call passes argv in and out, as BASIC does: a subroutine's changes are
 * visible AFTER the call returns, never during it.  Saying so now is what
 * lets a remote implementation copy them across a wire.
 *
 * mvxc_execute runs a TCL sentence; `capture` receives its output when it is
 * not NULL, one attribute per line. */
mvxc_status mvxc_call   (mvxc_session *s, const char *name,
                         int argc, mvxc_val **argv);
mvxc_status mvxc_execute(mvxc_session *s, const char *sentence,
                         mvxc_val *capture);

/* --- what this is --------------------------------------------------------
 * The client library's own version, which is the toolchain's.  A binding
 * should report it, because the answer to "which mvx" is usually the first
 * question about a bug. */
const char *mvxc_version(void);

#ifdef __cplusplus
}
#endif
#endif /* MVXC_H */
