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

/* Resolving a verb to the program that is it (mvx#248).
 *
 * THIS USED TO LIVE IN THE SHELL, and had to move when EXECUTE stopped
 * spawning one.  A compiled program can EXECUTE without any shell above it --
 * run it straight from Unix and there is no dispatcher to call back into --
 * so the resolution has to be something the runtime itself can do, or
 * EXECUTE would work under `mvx` and not otherwise.
 *
 * It is the SAME THREE-TIER WALK the runtime already does for subroutines in
 * mvx_ext.c: the account first, then each linked package in the order
 * PACKAGES lists them, then the system account.  There it searches LIB/ for a
 * library; here it searches VOC for a verb.  A local VOC entry therefore
 * overrides a package's, and a package's overrides the system's, which is
 * what lets an account replace a standard verb with its own.
 *
 * Non-negotiable 7 is untouched: verbs are still BASIC programs, and what
 * moved is the loader, not the verbs.  The shell keeps the parts that are
 * genuinely a shell -- the prompt, the builtins, the command stack -- and
 * asks this for the one thing both it and EXECUTE need to know. */

#include "mvx_runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* A VOC verb record: attribute 1 is "V", attribute 2 the program's path
   relative to whichever account the record came from. */
static int voc_read(mvx_ctx *ctx, mv_value *voc, const char *verb,
                    char *path, size_t cap) {
    mv_value id, rec, a1, a2;
    mv_init(&id); mv_init(&rec); mv_init(&a1); mv_init(&a2);
    mv_set_str(&id, verb, (int64_t)strlen(verb));
    int found = 0;
    if (mvx_read(ctx, &rec, voc, &id, 0)) {
        mv_extract_fn(&a1, &rec, 1, 0, 0);
        mv_extract_fn(&a2, &rec, 2, 0, 0);
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(&a1, nb, sizeof nb, &p);
        if (n >= 1 && (p[0] == 'V' || p[0] == 'v')) {
            n = mv_val_chars(&a2, nb, sizeof nb, &p);
            if (n > 0 && (size_t)n < cap) {
                memcpy(path, p, (size_t)n);
                path[n] = '\0';
                found = 1;
            }
        }
    }
    mv_clear(&id); mv_clear(&rec); mv_clear(&a1); mv_clear(&a2);
    return found;
}

static int voc_open(mvx_ctx *ctx, mv_value *voc, const char *spec) {
    mv_value s;
    mv_init(&s);
    mv_set_str(&s, spec, (int64_t)strlen(spec));
    mv_init(voc);
    int ok = mvx_open(ctx, NULL, &s, voc) ? 1 : -1;
    mv_clear(&s);
    return ok;
}

static mv_value g_voc, g_sysvoc;
static int g_voc_state, g_sysvoc_state;

/* Linked packages: the account's PACKAGES record (one path per line,
   maintained by LINK-PKG / UNLINK-PKG) names package directories whose VOCs
   join the resolution chain.  Reloaded when the file changes, so a LINK-PKG
   takes effect in the same session. */
#define MAX_PKGS 16
static char g_pkgs[MAX_PKGS][1024];
static mv_value g_pkgvoc[MAX_PKGS];
static int g_pkgvoc_state[MAX_PKGS];
static int g_npkgs;
static long long g_pkg_stamp = -1;

static void pkgs_reload(void) {
    struct stat sb;
    long long mt = 0;
    if (stat("PACKAGES", &sb) == 0) {
        /* Nanosecond stamp + size: whole-second mtime misses a LINK-PKG
           landing in the same second as the previous reload. */
#ifdef __APPLE__
        mt = (long long)sb.st_mtimespec.tv_sec * 1000000000LL +
             sb.st_mtimespec.tv_nsec + sb.st_size;
#else
        mt = (long long)sb.st_mtim.tv_sec * 1000000000LL +
             sb.st_mtim.tv_nsec + sb.st_size;
#endif
    }
    if (mt == g_pkg_stamp) return;
    g_pkg_stamp = mt;
    for (int i = 0; i < g_npkgs; i++)
        if (g_pkgvoc_state[i] > 0) mv_clear(&g_pkgvoc[i]);
    g_npkgs = 0;
    FILE *fp = fopen("PACKAGES", "r");
    if (!fp) return;
    char ln[1024];
    while (fgets(ln, sizeof ln, fp) && g_npkgs < MAX_PKGS) {
        size_t n = strlen(ln);
        while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r' ||
                     ln[n - 1] == ' '))
            ln[--n] = '\0';
        if (n == 0) continue;
        snprintf(g_pkgs[g_npkgs], sizeof g_pkgs[0], "%s", ln);
        g_pkgvoc_state[g_npkgs] = 0;
        g_npkgs++;
    }
    fclose(fp);
}

/* Account VOC (local overrides), then linked packages in listed order, then
   the system account's master VOC.  A foreign verb resolves to a path in its
   OWN account's CATALOG but runs in the user's account, which is the working
   directory either way.

   1 found, 0 no such verb, -1 no VOC at all to ask. */
int mvx_voc_lookup(mvx_ctx *ctx, const char *verb, char *path, size_t cap) {
    if (g_voc_state == 0) g_voc_state = voc_open(ctx, &g_voc, "VOC");
    if (g_voc_state > 0 && voc_read(ctx, &g_voc, verb, path, cap))
        return 1;

    pkgs_reload();
    for (int i = 0; i < g_npkgs; i++) {
        if (g_pkgvoc_state[i] == 0) {
            char pv[1152];
            snprintf(pv, sizeof pv, "%s/VOC", g_pkgs[i]);
            g_pkgvoc_state[i] = voc_open(ctx, &g_pkgvoc[i], pv);
        }
        if (g_pkgvoc_state[i] > 0) {
            char rel[1024];
            if (voc_read(ctx, &g_pkgvoc[i], verb, rel, sizeof rel)) {
                snprintf(path, cap, "%s/%s", g_pkgs[i], rel);
                return 1;
            }
        }
    }

    if (g_sysvoc_state == 0) {
        char sysvoc[4096];
        snprintf(sysvoc, sizeof sysvoc, "%s/VOC", mvx_system_dir());
        g_sysvoc_state = voc_open(ctx, &g_sysvoc, sysvoc);
    }
    if (g_sysvoc_state > 0) {
        char rel[1024];
        if (voc_read(ctx, &g_sysvoc, verb, rel, sizeof rel)) {
            snprintf(path, cap, "%s/%s", mvx_system_dir(), rel);
            return 1;
        }
    }
    return (g_voc_state < 0 && g_sysvoc_state < 0) ? -1 : 0;
}

/* Forget everything cached (mvx#248).  The chain is per ACCOUNT -- its VOC,
   its PACKAGES, and the system account behind them -- so anything that
   changes account has to say so, or resolution keeps answering for the one
   just left.  LOGTO is the caller that matters. */
void mvx_voc_reset(void) {
    if (g_voc_state > 0) mv_clear(&g_voc);
    if (g_sysvoc_state > 0) mv_clear(&g_sysvoc);
    for (int i = 0; i < g_npkgs; i++)
        if (g_pkgvoc_state[i] > 0) mv_clear(&g_pkgvoc[i]);
    g_voc_state = 0;
    g_sysvoc_state = 0;
    g_npkgs = 0;
    g_pkg_stamp = -1;
}
