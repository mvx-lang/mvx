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

/* levels — a program run at a new level shares the session and not the rest
 * (mvx#248).
 *
 * Until a program reached by EXECUTE runs in this process, the operating
 * system does the separating and none of this is written down anywhere.  In
 * one process every piece of state is either deliberately shared or
 * deliberately fresh, and the failure when one is wrong is SILENT -- the
 * caller reads a value it did not write, or fails to read one it did, with
 * nothing reported.  So each side of the line is asserted here rather than
 * left to be discovered.
 *
 * The expected answers are not invented: they are what jBASE 6.2.1.1 does,
 * measured by running the same shape there.  A program reached by EXECUTE
 * read the caller's NAMED common and found its UNNAMED common uninitialised.
 *
 * Takes the child program's base path and drives it directly, which is the
 * shape an in-process EXECUTE will use: push a level, call mvx_main, pop. */

#include "mvx_runtime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#define LIB_SUFFIX ".dylib"
#else
#define LIB_SUFFIX ".so"
#endif

typedef void (*entry_fn)(mvx_ctx *);

static int fails = 0;

static void is(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) == 0) {
        printf("levels:   %s\n", what);
    } else {
        printf("levels: FAIL %s: got [%s], want [%s]\n", what, got, want);
        fails++;
    }
}

/* The same resolution the runtime will use: suffix first, then plain. */
static entry_fn load(const char *base) {
    char path[4096];
    snprintf(path, sizeof path, "%s%s", base, LIB_SUFFIX);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen(base, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("levels: FAIL cannot load %s: %s\n", base, dlerror());
        return NULL;
    }
    return (entry_fn)dlsym(h, "mvx_main");
}

static const char *str_of(mv_value *v) {
    static char buf[256];
    const char *p;
    int64_t n = mv_val_chars(v, buf, sizeof buf, &p);
    static char out[256];
    if (n > (int64_t)sizeof out - 1) n = sizeof out - 1;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
    return out;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: levels <child-base-path>\n");
        return 2;
    }
    entry_fn child = load(argv[1]);
    if (!child) return 1;

    mvx_ctx *ctx = mvx_ctx_create();

    /* The caller's two blocks.  Index 0 of each, which is what the child's
       first COMMON declaration of each block names. */
    mv_set_str(mvx_common_scalar(ctx, "NM", 0), "parent-named", 12);
    mv_set_str(mvx_common_scalar(ctx, "",   0), "parent-unnamed", 14);

    /* AND THE TRANSACTION SPANS THE LEVEL, which is the whole reason for
       sharing the session (mvx#247/#248).  Neither jBASE nor UniData does
       this: measured, a program reached by EXECUTE on jBASE reports
       TRANSQUERY 0 while its caller still reads 1, and on UniData a child
       counting a file the parent wrote inside a transaction answers "0
       record(s) counted" -- a stale view, silently wrong rather than an
       error.  Here the child is INSIDE it, so its write is part of what the
       caller commits. */
    if (!mvx_txn_start(ctx)) {
        printf("levels: FAIL could not start a transaction\n");
        return 1;
    }

    mvx_ctx *lv = mvx_level_push(ctx, "CHILD ITS OWN SENTENCE");
    child(lv);
    mvx_level_pop(lv);

    if (!mvx_txn_commit(ctx)) {
        printf("levels: FAIL the commit failed\n");
        fails++;
    }

    /* NAMED common crosses the level, so the child's assignment is visible
       here.  UNNAMED does not, so the caller's is exactly as it was left --
       this is the assertion that would fail if the two lists were one. */
    is("a named COMMON block crosses into the level and back",
       str_of(mvx_common_scalar(ctx, "NM", 0)), "child-named");
    is("the unnamed COMMON block is the caller's own",
       str_of(mvx_common_scalar(ctx, "", 0)), "parent-unnamed");

    mvx_ctx_destroy(ctx);
    if (fails) return 1;
    printf("levels: a level shares the session and keeps the rest\n");
    return 0;
}
