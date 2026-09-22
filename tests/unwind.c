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

/* unwind — STOP comes back to the caller, ABORT and a fault do not (mvx#248).
 *
 * The split is UniData's, measured there rather than chosen: a STOP in a
 * program reached by EXECUTE returns to its caller and the caller keeps
 * running, while an ABORT or a runtime fault takes the caller with it.  So an
 * abort has to travel THROUGH a level that would have caught a stop, which is
 * why it is a flag on the unwind and not a separate mechanism.
 *
 *   unwind stop <prog>...   run each at a level, report what came back, and
 *                           say so at the end -- reaching the end IS the
 *                           assertion for STOP, and failing to reach it is
 *                           the assertion for ABORT
 *   unwind txn <prog>       start a transaction, run one program, and commit
 *                           after it has ended -- a STOP must not discard it
 *
 * That last one is the pattern the transaction work exists for (mvx#247): the
 * transaction is started in one place and committed in another, with programs
 * beginning and ending in between.  A STOP that unwinds never reaches exit(),
 * so the rollback registered there does not run and the transaction survives
 * -- which is the behaviour, not an accident of the implementation. */

#include "mvx_runtime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#define LIB_SUFFIX ".dylib"
#else
#define LIB_SUFFIX ".so"
#endif

/* The same resolution the runtime uses: suffix first, then plain. */
static mvx_program_fn load(const char *base) {
    char path[4096];
    snprintf(path, sizeof path, "%s%s", base, LIB_SUFFIX);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen(base, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("unwind: FAIL cannot load %s: %s\n", base, dlerror());
        return NULL;
    }
    return (mvx_program_fn)dlsym(h, "mvx_main");
}

static const char *leaf(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: unwind stop|txn <prog>...\n");
        return 2;
    }
    int txn = strcmp(argv[1], "txn") == 0;
    mvx_ctx *ctx = mvx_ctx_create();

    if (txn && !mvx_txn_start(ctx)) {
        printf("unwind: FAIL could not start a transaction\n");
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        mvx_program_fn f = load(argv[i]);
        if (!f) return 1;
        int64_t rc = mvx_level_run(ctx, f, "THE CHILD SENTENCE");
        printf("caller: %s came back, rc=%lld\n", leaf(argv[i]),
               (long long)rc);
    }

    if (txn) {
        printf("caller: @TRANSACTION is still %lld after it ended\n",
               (long long)mvx_txn_depth(ctx));
        printf("caller: commit -> %lld\n", (long long)mvx_txn_commit(ctx));
    }

    /* Reaching here at all is the assertion: an ABORT must not. */
    printf("unwind: the caller ran on\n");
    mvx_ctx_destroy(ctx);
    return 0;
}
