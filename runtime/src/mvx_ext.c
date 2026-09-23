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
#include "mvx_driver.h"   /* MVX_DRIVER_ABI, checked against an artifact stamp */

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#define LIB_SUFFIX ".dylib"
#else
#define LIB_SUFFIX ".so"
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

/* WHAT HAS ALREADY BEEN LOADED (mvx#248).  The scan can now run more than
   once -- see mvx_ext_load_libs -- and a library loaded twice would register
   its extension functions twice.  dlopen itself is refcounted and would not
   mind, but the registry would grow a duplicate for every rescan. */
typedef struct loaded_lib {
    struct loaded_lib *next;
    char path[1];
} loaded_lib;
static loaded_lib *g_libs;

static int already_loaded(const char *path) {
    for (loaded_lib *l = g_libs; l; l = l->next)
        if (strcmp(l->path, path) == 0) return 1;
    return 0;
}

static void remember_loaded(const char *path) {
    size_t n = strlen(path);
    loaded_lib *l = malloc(sizeof *l + n);
    if (!l) return;                     /* forgetting costs a duplicate, not
                                           correctness */
    memcpy(l->path, path, n + 1);
    l->next = g_libs;
    g_libs = l;
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
        /* PROBE: the realpath keying is out while CI says whether it is what
           broke the installed MVPKG's first CALL. */
        const char *key = path;
        if (already_loaded(key)) continue;
        remember_loaded(key);
        void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);   /* GLOBAL: CALL sees mvx_sub_ */
        if (!h) {
            /* SAY WHY (#117).  This used to fail silently, so a library built
               against a newer runtime just never loaded and the user met it
               later as "subroutine is not cataloged" -- a long way from the
               cause.  The message names the file and what the loader said. */
            const char *why = dlerror();
            fprintf(stderr, "mvx: cannot load %s: %s\n", path,
                    why ? why : "unknown error");
            continue;
        }
        /* What was this compiled against?  mvx-basic stamps every artifact
           with the driver ABI it saw (#117).  An older stamp is fine -- the
           ABI only breaks forward -- but a NEWER one means this runtime does
           not have what the library was built to call, and loading it would
           trade a clear message here for an undefined symbol later. */
        const int32_t *built = (const int32_t *)dlsym(h, "mvx_built_abi");
        if (built && *built > MVX_DRIVER_ABI) {
            const char *bv = (const char *)dlsym(h, "mvx_built_version");
            fprintf(stderr,
                    "mvx: %s needs driver ABI %d but this runtime speaks %d"
                    "%s%s%s -- rebuild it, or upgrade mvx\n",
                    path, (int)*built, MVX_DRIVER_ABI,
                    bv ? " (built by mvx " : "", bv ? bv : "", bv ? ")" : "");
            dlclose(h);
            continue;
        }
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
    register_ext(mvx_msg_builtin());
    register_ext(mvx_logto_builtin());
}

/* A PACKAGE CAN BE LINKED WHILE THE SESSION IS RUNNING (mvx#248).
 *
 * This used to load once and never again, which was right while every verb
 * was a forked process that did its own loading in its own account.  With
 * verbs running in the session, a LINK-PKG during that session would never
 * take effect: the subroutines in the package just linked stay invisible, and
 * a CALL to one fails with "subroutine is not cataloged" -- a message that
 * names the subroutine and says nothing about the cause.
 *
 * Shown by restoring the one-shot and running LINK-PKG between two calls:
 * both fail, where with the rescan the second succeeds.
 *
 * So the PACKAGES file is stamped and the scan repeats when it changes.
 * Nothing is UNloaded: a library already open may have pointers into it, and
 * the cost of leaving it is an open handle rather than a wrong answer.  The
 * account's own LIB/ is rescanned too, because BUILD-PKG can add to it. */
static long long packages_stamp(void) {
    struct stat sb;
    if (stat("PACKAGES", &sb) != 0) return 0;
#ifdef __APPLE__
    return (long long)sb.st_mtimespec.tv_sec * 1000000000LL +
           sb.st_mtimespec.tv_nsec + sb.st_size;
#else
    return (long long)sb.st_mtim.tv_sec * 1000000000LL +
           sb.st_mtim.tv_nsec + sb.st_size;
#endif
}

static long long g_pkgstamp = -1;

void mvx_ext_load_libs(void) {
    long long stamp = packages_stamp();
    if (g_loaded && stamp == g_pkgstamp) return;
    g_pkgstamp = stamp;
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

    const char *sys = mvx_system_dir();   /* not the build tree (mvx#210) */
    char syslib[4096];
    snprintf(syslib, sizeof syslib, "%s/LIB", sys);
    load_dir(syslib);
}

/* THE CHAIN IS PER ACCOUNT, TOO (mvx#258).  The rescan above is triggered by
 * PACKAGES changing, which catches a LINK-PKG but not a LOGTO: the new account
 * may have no PACKAGES at all, and then the stamp is 0 in both -- unchanged --
 * so its own LIB/ would never be scanned and a CALL into it would fail with
 * "subroutine is not cataloged", naming the subroutine and not the cause.
 *
 * Nothing is unloaded.  A library already open may have pointers into it, and
 * "first registration wins" in register_ext means the account left keeps any
 * extension name it registered.  That is the same trade mvx_ext_load_libs
 * already makes for LINK-PKG: an open handle rather than a wrong answer. */
void mvx_ext_reset_libs(void) {
    g_loaded = 0;
    g_pkgstamp = -1;
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
