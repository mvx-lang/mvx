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

/* mvx-launch — the executable half of a cataloged program (mvx#248).
 *
 * A cataloged main program has to exist in two forms: a shared library, so
 * the runtime can load it and run it inside an existing process, and
 * something the operating system will execute, because that is what a VOC
 * record names and what the shell runs.
 *
 * THE PROGRAM ITSELF IS ONLY COMPILED ONCE.  It lives in CATALOG/<name>.so;
 * this binary holds none of it.  A copy of this one file is published as
 * CATALOG/<name>, and at run time it finds the library sitting beside it,
 * loads it, and calls the entry point the compiler emits for every main
 * program.  Building the program twice instead -- once as an executable and
 * once as a library -- would put two copies of the same code on disk, each
 * with its own debug information, and each would have to be kept in step.
 *
 * Loading the executable itself, and so needing only one file, is not
 * available: glibc refuses to dlopen a position-independent executable
 * ("cannot dynamically load position-independent executable"), measured on
 * 2.28, the floor the Linux binaries are built against (mvx#203).
 *
 * IT FINDS THE LIBRARY FROM ITS OWN PATH, not from argv[0].  The shell hands
 * a verb the words the operator typed, so argv[0] is a verb name and not a
 * path at all; and a path derived from the current directory would break the
 * moment an account were run from somewhere else.  Asking the system where
 * this process was loaded from is the only answer that survives an account
 * being moved, copied or mounted somewhere new.
 *
 * The sentence reaches the program the same way it always has, through the
 * environment, so there is nothing here to pass on. */

#include "mvx_runtime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdint.h>
#define MVX_LIB_SUFFIX ".dylib"
#else
#include <unistd.h>
#define MVX_LIB_SUFFIX ".so"
#endif

/* Where this executable was loaded from.  Both answers are the kernel's own,
   and neither depends on argv or the working directory. */
static int self_path(char *out, size_t cap) {
#ifdef __APPLE__
    uint32_t n = (uint32_t)cap;
    return _NSGetExecutablePath(out, &n) == 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#endif
}

int main(void) {
    char path[4096];
    if (!self_path(path, sizeof path)) {
        fprintf(stderr, "mvx-launch: cannot determine my own path\n");
        return 70;                       /* EX_SOFTWARE, as mvx_fatal uses */
    }
    size_t n = strlen(path);
    if (n + sizeof MVX_LIB_SUFFIX > sizeof path) {
        fprintf(stderr, "mvx-launch: path too long: %s\n", path);
        return 70;
    }
    memcpy(path + n, MVX_LIB_SUFFIX, sizeof MVX_LIB_SUFFIX);

    /* LOCAL, and resolved through this handle: every main program exports the
       same entry symbol, so a program's entry point has no business in the
       global namespace where another lookup could reach it. */
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "mvx-launch: cannot load %s: %s\n", path, dlerror());
        return 70;
    }
    void (*run)(mvx_ctx *) = (void (*)(mvx_ctx *))dlsym(h, "mvx_main");
    if (!run) {
        fprintf(stderr, "mvx-launch: %s has no program in it\n", path);
        return 70;
    }

    /* The same three lines the compiled executable used to carry, from
       mvx_crt.c: a context, the program, and the teardown.  STOP and a fatal
       error still leave through exit(), so neither reaches the end here. */
    mvx_ctx *ctx = mvx_ctx_create();
    run(ctx);
    mvx_ctx_destroy(ctx);
    return 0;
}
