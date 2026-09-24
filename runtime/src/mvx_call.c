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

/* Runtime CALL resolution — the jBASE catalog model: subroutines
 * compile into shared libraries, and CALL binds at call time.
 *
 * Resolution order for mvx_sub_<NAME>:
 *   1. symbols already in the process (subroutines compiled into the
 *      program itself, or in libraries loaded earlier);
 *   2. cataloged subroutine libraries, loaded on first miss from the
 *      account's LIB/, each linked package's LIB/ (PACKAGES record),
 *      and the system account's LIB/.
 *
 * CALL @VAR routes here too, with the name taken from the variable —
 * which is what makes dispatch tables (and command frameworks)
 * possible.
 */
#include "mvx_runtime.h"
#include "mvx_ext.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*mvx_subfn)(mvx_ctx *, int32_t, mv_value **);

/* Cataloged subroutine (mvx_sub_<NAME>) or, failing that, an extension-package
   function.  Library loading (account LIB/, linked packages, system account) is
   shared with the extension registry — mvx_ext_load_libs() dlopen's every
   package lib RTLD_GLOBAL, so both mvx_sub_ symbols and mvx_ext tables land in
   one pass. */
/* TWO PACKAGES CLAIMING ONE SUBROUTINE (mvx#266).
 *
 * dlsym(RTLD_DEFAULT, ...) takes the first definition loaded and cannot see
 * that there were others, so an account holding two builds of the same
 * subroutine runs whichever the loader met first -- and `load_dir' walks LIB
 * with readdir, whose order is arbitrary.  Measured: mvpkg bundles its own
 * CMD.RUN while the cmd package ships another, and they differ (one calls
 * GETOPT.SENTENCE, one does not), so the same install worked or failed by
 * filesystem chance with nothing said.
 *
 * Said once per subroutine, not per CALL: the check walks the loaded
 * libraries, which is cheap once and wasteful on the hot path. */
static void report_duplicate(const char *name, const char *sym) {
    static struct seen { struct seen *next; char name[1]; } *g_seen;
    for (struct seen *s = g_seen; s; s = s->next)
        if (strcmp(s->name, name) == 0) return;
    size_t n = strlen(name);
    struct seen *s = malloc(sizeof *s + n);
    if (s) {
        memcpy(s->name, name, n + 1);
        s->next = g_seen;
        g_seen = s;
    }

    const char *winner = NULL, *other = NULL;
    if (mvx_ext_providers(sym, &winner, &other) < 2) return;
    fprintf(stderr,
            "mvx: %s is defined by more than one library, and which one runs "
            "depends on\n     the order LIB was read:\n"
            "       %s  (in use)\n"
            "       %s  (shadowed)\n"
            "     They are not required to agree.  One of them should not be "
            "installed here.\n",
            name, winner ? winner : "?", other ? other : "?");
}

static mvx_subfn find_sub(const char *name) {
    char sym[300];
    snprintf(sym, sizeof sym, "mvx_sub_%s", name);
    void *p = dlsym(RTLD_DEFAULT, sym);
    if (!p) {
        mvx_ext_load_libs();
        p = dlsym(RTLD_DEFAULT, sym);
    }
    if (p) report_duplicate(name, sym);
    return (mvx_subfn)p;
}

/* CATALOGED(name) — 1 if `CALL name` would resolve, 0 otherwise.  The MV
   catalog-lookup idiom (Pick has no standard subroutine-exists function), done
   here with the same resolution CALL uses, so a program can prefer an optional
   subroutine when it is installed and fall back when it is not — decided at
   run time, no recompile when the provider is added later. */
int64_t mv_cataloged_fn(const mv_value *namev) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(namev, nb, sizeof nb, &p);
    if (n <= 0 || n >= 256) return 0;
    char name[256];
    memcpy(name, p, (size_t)n);
    name[n] = '\0';
    return find_sub(name) != NULL ? 1 : 0;
}

void mvx_call(mvx_ctx *ctx, const char *name, int32_t argc,
              mv_value **argv) {
    mvx_subfn f = find_sub(name);
    if (f) { f(ctx, argc, argv); return; }
    /* Not a cataloged subroutine — try an extension-package function (a native
       function called as a statement; its result, if any, is discarded). */
    if (mvx_ext_has(name)) {
        mv_value scratch;
        mv_init(&scratch);
        mvx_ext_invoke(ctx, name, &scratch, argc, argv);
        mv_clear(&scratch);
        return;
    }
    mvx_fatal("CALL %s: subroutine is not cataloged (searched the "
              "program, LIB/, linked packages, and the system "
              "account)", name);
}

void mvx_call_var(mvx_ctx *ctx, const mv_value *namev, int32_t argc,
                  mv_value **argv) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(namev, nb, sizeof nb, &p);
    if (n <= 0 || n >= 256)
        mvx_fatal("CALL @: variable does not hold a subroutine name");
    char name[256];
    memcpy(name, p, (size_t)n);
    name[n] = '\0';
    mvx_call(ctx, name, argc, argv);
}
