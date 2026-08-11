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

/* OS text-file access — the bridge between hash-file records and the
 * outside world (editors, git, diff).  A record is a dynamic string; an
 * OS file is bytes.  These primitives move whole files; the caller
 * translates attribute marks to newlines with CHANGE() when it wants
 * the git-friendly attribute-per-line shape.
 *
 * Pure I/O, no exec — safe at any privilege tier.  The editor spawn
 * that VI needs is separate and gated (see mvx_exec.c).
 */
#include "mvx_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

void mvx_ctx_set_status(mvx_ctx *ctx, int64_t s);   /* in mvx_ctx.c */

/* OSREAD(path) -> file contents, or "" (STATUS() 1) when unreadable. */
void mv_osread(mvx_ctx *ctx, mv_value *dst, const mv_value *path) {
    char nb[40];
    const char *p;
    mv_val_chars(path, nb, sizeof nb, &p);
    FILE *fp = fopen(p, "rb");
    if (!fp) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, "", 0);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) mvx_fatal("out of memory in OSREAD");
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, "", 0);
        return;
    }
    fclose(fp);
    mvx_ctx_set_status(ctx, 0);
    mv_set_str(dst, buf, sz > 0 ? sz : 0);
    free(buf);
}

/* OSWRITE(data, path) -> 1 on success, 0 on failure. */
int64_t mv_oswrite(mvx_ctx *ctx, const mv_value *data,
                   const mv_value *path) {
    (void)ctx;
    char nb[40], db[40];
    const char *pp;
    mv_val_chars(path, nb, sizeof nb, &pp);
    const char *dp;
    int64_t dl = mv_val_chars(data, db, sizeof db, &dp);
    FILE *fp = fopen(pp, "wb");
    if (!fp) return 0;
    int ok = dl == 0 || fwrite(dp, 1, (size_t)dl, fp) == (size_t)dl;
    return (fclose(fp) == 0 && ok) ? 1 : 0;
}

/* OSDELETE(path) -> 1 if removed. */
int64_t mv_osdelete(mvx_ctx *ctx, const mv_value *path) {
    (void)ctx;
    char nb[40];
    const char *p;
    mv_val_chars(path, nb, sizeof nb, &p);
    return unlink(p) == 0;
}

/* UNAME(which) -> a uname(2) field; `which`'s first letter selects it:
   s=sysname (default) n=nodename r=release v=version m=machine.  Read-only
   platform info, so ungated at any tier (a package uses it to pick the right
   prebuilt artifact without shelling out to `uname`). */
void mvx_uname(mvx_ctx *ctx, mv_value *dst, const mv_value *which) {
    struct utsname u;
    if (uname(&u) != 0) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, "", 0);
        return;
    }
    char nb[40];
    const char *w;
    int64_t wl = mv_val_chars(which, nb, sizeof nb, &w);
    char c = wl > 0 ? w[0] : 's';
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c == 'e') {                 /* byte order — the axis for BASIC objects */
        const unsigned probe = 1;   /* + data files; not a utsname field */
        mvx_ctx_set_status(ctx, 0);
        mv_set_str(dst, (*(const char *)&probe) ? "le" : "be", 2);
        return;
    }
    const char *v;
    switch (c) {
        case 'n': v = u.nodename; break;
        case 'r': v = u.release; break;
        case 'v': v = u.version; break;
        case 'm': v = u.machine; break;
        default:  v = u.sysname; break;
    }
    mvx_ctx_set_status(ctx, 0);
    mv_set_str(dst, v, (int64_t)strlen(v));
}
