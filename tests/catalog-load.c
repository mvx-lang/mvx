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

/* catalog-load — CATALOG publishes a form of a main program that can be
 * loaded and called in this process (mvx#248).
 *
 * Takes the BASE paths of cataloged programs -- what a VOC record names --
 * and resolves each the way the runtime will have to: the platform's library
 * suffix first, then the plain path.  That rule is the point of the test as
 * much as the loading is, because a cataloged program is not the same number
 * of files everywhere.  On macOS an executable can also be loaded, so there
 * is one file and the plain path answers; where it cannot (glibc refuses to
 * dlopen a position-independent executable) the program is <name>.so and the
 * plain path is a small loader with no program in it.
 *
 * With two or more programs it RUNS them all, which is the other half of the
 * point: every main program exports the same symbol, `mvx_main`, because that
 * is what the compiler emits for anything that is not a SUBROUTINE or
 * FUNCTION.  A one-program test would pass whatever the loader did with the
 * second.
 *
 * What keeps them apart is resolving through each library's OWN dlopen
 * handle.  That holds even under RTLD_GLOBAL -- measured on glibc 2.28, two
 * libraries both exporting mvx_main answer correctly through their handles,
 * while the same symbol looked up with RTLD_DEFAULT gives the FIRST one
 * loaded for both.  So the handle is the mechanism and RTLD_LOCAL is the
 * hygiene: it keeps a program's entry point out of the global namespace,
 * where that second lookup shows what going wrong looks like -- the wrong
 * program running, with nothing reported.
 *
 * --probe loads and checks the entry point without calling it, for a program
 * that should not actually be run (a standard verb wants a sentence and an
 * account).
 *
 * This is also the shape the in-process EXECUTE uses: make a context, call
 * mvx_main(ctx), destroy it -- exactly what main() in mvx_crt.c does for the
 * executable form, minus the process. */

#include "mvx_runtime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

typedef void (*entry_fn)(mvx_ctx *);

#ifdef __APPLE__
#define LIB_SUFFIX ".dylib"
#else
#define LIB_SUFFIX ".so"
#endif

/* Resolve a base path to whatever carries the program here, and load it.
   LOCAL, and resolved through this handle: see above. */
static void *load(const char *base, char *shown, size_t cap) {
    snprintf(shown, cap, "%s%s", base, LIB_SUFFIX);
    void *h = dlopen(shown, RTLD_NOW | RTLD_LOCAL);
    if (h) return h;
    snprintf(shown, cap, "%s", base);
    return dlopen(shown, RTLD_NOW | RTLD_LOCAL);
}

static entry_fn entry_of(const char *base, int *ok) {
    char shown[4096];
    void *h = load(base, shown, sizeof shown);
    if (!h) {
        printf("catalog-load: FAIL nothing loadable for %s: %s\n",
               base, dlerror());
        *ok = 0;
        return NULL;
    }
    entry_fn run = (entry_fn)dlsym(h, "mvx_main");
    if (!run) {
        printf("catalog-load: FAIL %s has no program in it: %s\n",
               shown, dlerror());
        *ok = 0;
        return NULL;
    }
    *ok = 1;
    return run;
}

int main(int argc, char **argv) {
    int probe = 0, i = 1;
    if (argc > 1 && strcmp(argv[1], "--probe") == 0) { probe = 1; i = 2; }
    if (i >= argc) {
        fprintf(stderr, "usage: catalog-load [--probe] <base-path>...\n");
        return 2;
    }
    int n = 0;
    for (; i < argc; i++) {
        int ok = 0;
        entry_fn run = entry_of(argv[i], &ok);
        if (!ok) return 1;
        n++;
        if (probe) continue;
        mvx_ctx *ctx = mvx_ctx_create();
        run(ctx);
        mvx_ctx_destroy(ctx);
    }
    if (probe)
        printf("catalog-load: %d cataloged program(s) are loadable here\n", n);
    else
        printf("catalog-load: both cataloged programs ran in this process\n");
    return 0;
}
