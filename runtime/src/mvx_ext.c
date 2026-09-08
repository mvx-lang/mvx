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

/* Language-extension registry + loader (#54).  See mvx_ext.h.
 *
 * The library scan is also the CALL subroutine loader: it dlopen's every package
 * LIB/ lib RTLD_GLOBAL (so mvx_sub_<NAME> stays resolvable to mvx_call), and for
 * any lib exporting mvx_ext_entry it registers that lib's function table. */

#include "mvx_ext.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#define LIB_SUFFIX ".dylib"
#else
#define LIB_SUFFIX ".so"
#endif

#ifndef MVX_SYSTEM_DIR
#define MVX_SYSTEM_DIR "."
#endif

typedef struct reg_ent {
    const char *name;                   /* points into the loaded lib (never freed) */
    int32_t minargs, maxargs;
    mvx_extfn_t fn;
    struct reg_ent *next;
} reg_ent;

static reg_ent *g_reg;
static int g_loaded;

static reg_ent *find_ext(const char *name) {
    for (reg_ent *r = g_reg; r; r = r->next)
        if (strcmp(r->name, name) == 0) return r;
    return NULL;
}

static void register_ext(const mvx_ext *e) {
    for (int i = 0; i < e->nfns; i++) {
        const mvx_extfn *f = &e->fns[i];
        if (!f->name || find_ext(f->name)) continue;   /* first registration wins */
        reg_ent *r = malloc(sizeof *r);
        if (!r) mvx_fatal("out of memory registering extension %s", f->name);
        r->name = f->name;
        r->minargs = f->minargs;
        r->maxargs = f->maxargs;
        r->fn = f->fn;
        r->next = g_reg;
        g_reg = r;
    }
}

static void load_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    size_t sl = strlen(LIB_SUFFIX);
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n <= sl || strcmp(e->d_name + n - sl, LIB_SUFFIX) != 0)
            continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);   /* GLOBAL: CALL sees mvx_sub_ */
        if (!h) continue;                                 /* failure: symbols stay absent */
        mvx_ext_entry_fn entry = (mvx_ext_entry_fn)dlsym(h, "mvx_ext_entry");
        if (entry) {
            const mvx_ext *ext = entry(MVX_EXT_ABI);
            if (ext) register_ext(ext);   /* NULL = not an ext lib for this ABI */
        }
    }
    closedir(d);
}

/* The tables compiled into libmvxrt.  Registered before any dlopen, so a
   package shipping the same name cannot displace a built-in ("first
   registration wins" in register_ext) -- which is the point of a built-in:
   JSONENCODE means the same thing in every account, whatever is installed. */
static void register_builtins(void) {
    static int done;
    if (done) return;
    done = 1;
    register_ext(mvx_json_builtin());
}

void mvx_ext_load_libs(void) {
    if (g_loaded) return;
    g_loaded = 1;

    register_builtins();                        /* before any dlopen */
    load_dir("LIB");                            /* account catalog */

    FILE *fp = fopen("PACKAGES", "r");          /* linked packages */
    if (fp) {
        char ln[1024];
        while (fgets(ln, sizeof ln, fp)) {
            size_t n = strlen(ln);
            while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r' ||
                         ln[n - 1] == ' '))
                ln[--n] = '\0';
            if (!n) continue;
            char lib[1152];
            snprintf(lib, sizeof lib, "%s/LIB", ln);
            load_dir(lib);
        }
        fclose(fp);
    }

    const char *sys = getenv("MVXSYSTEM");
    if (!sys || !sys[0]) sys = MVX_SYSTEM_DIR;
    char syslib[4096];
    snprintf(syslib, sizeof syslib, "%s/LIB", sys);
    load_dir(syslib);
}

int mvx_ext_has(const char *name) {
    register_builtins();          /* available before any library is searched */
    if (find_ext(name)) return 1;
    if (!g_loaded) { mvx_ext_load_libs(); return find_ext(name) != NULL; }
    return 0;
}

void mvx_ext_invoke(mvx_ctx *ctx, const char *name, mv_value *ret,
                    int32_t argc, mv_value **argv) {
    register_builtins();
    reg_ent *r = find_ext(name);
    if (!r && !g_loaded) { mvx_ext_load_libs(); r = find_ext(name); }
    if (!r)
        mvx_fatal("%s: extension function is not available (searched LIB/, "
                  "linked packages, and the system account)", name);
    if (argc < r->minargs || argc > r->maxargs)
        mvx_fatal("%s: wrong number of arguments (got %d, expected %d..%d)",
                  name, (int)argc, (int)r->minargs, (int)r->maxargs);
    r->fn(ctx, ret, argc, argv);
}
