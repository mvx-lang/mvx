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

/* Directory driver: an MV file backed by a Unix directory, one record
 * per file, attributes as lines (UniData DIR-file style).  This is the
 * driver that later carries BP source, so Git owns source natively
 * (ARCHITECTURE.md 9.1).
 *
 * Marshalling happens here, per the contract: attribute marks become
 * newlines on disk and back again on read.  Value/subvalue marks pass
 * through as raw bytes.
 */
#include "mvx_driver.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AM ((char)0xFE)

typedef struct {
    mvx_file_base base;
    char *path;
} dir_file;

struct mvx_cursor {
    char **ids;
    int64_t n, pos;
};

static int id_ok(const char *id, int64_t idlen) {
    if (idlen == 0 || (size_t)idlen > 255) return 0;
    if (id[0] == '.') return 0;
    for (int64_t i = 0; i < idlen; i++)
        if (id[i] == '/' || id[i] == '\0') return 0;
    return 1;
}

static char *rec_path(dir_file *f, const char *id, int64_t idlen) {
    size_t pl = strlen(f->path);
    char *p = malloc(pl + 1 + (size_t)idlen + 1);
    if (!p) mvx_fatal("out of memory in directory driver");
    memcpy(p, f->path, pl);
    p[pl] = '/';
    memcpy(p + pl + 1, id, (size_t)idlen);
    p[pl + 1 + idlen] = '\0';
    return p;
}

/* A BOUND FILE'S SPEC ARRIVES AS "params\nspec" (mvx#310).  resolve() says so
 * -- opaque to the runtime, parsed by the driver -- and every other driver
 * parses it: sqlite and postgres split it, lmdb skips it (mvx#309).  This one
 * did not, and used the whole thing as a path, so a file bound to dir was
 * created in a directory whose name began with a newline.  Reads and writes
 * still worked, being consistently wrong, but:
 *
 *   - the name is the interface for THIS driver, whose entire purpose is that a
 *     record is an OS file somebody can look at and edit.  A directory called
 *     "\nBP" is hostile to every tool that has to touch it;
 *   - "\nDICT.BP" cannot match the "DICT." prefix that fl_internal() tests, so
 *     a bound file exposed its dictionary in LISTF;
 *   - and the leading newline printed as a blank line, and never matched the
 *     name the bindings pass reported, so the file was listed twice.
 *
 * The params are SKIPPED rather than used.  Nothing produces any for dir today,
 * and a location for it would be a second root to resolve records against --
 * a real feature, not a spelling.  If one is ever wanted, this is the single
 * place that turns a spec into a path, so this is where it goes. */
static void spec_path(const char *spec, char *path, size_t cap) {
    const char *nl = strchr(spec, '\n');
    if (nl) spec = nl + 1;                 /* skip the params, keep the spec */

    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    if (spec[0] == '/')
        snprintf(path, cap, "%s", spec);
    else
        snprintf(path, cap, "%s/%s", acct, spec);
}

static const mvx_driver mvx_driver_dir;

static mvx_file *dir_open(const char *spec, char *err, size_t errlen) {
    char path[4096];
    spec_path(spec, path, sizeof path);    /* not its own copy of this */

    struct stat sb;
    if (stat(path, &sb) != 0)
        return NULL;                     /* missing = a silent not-found */
    if (!S_ISDIR(sb.st_mode)) {
        snprintf(err, errlen, "dir: %s is not a directory", path);
        return NULL;
    }
    dir_file *f = calloc(1, sizeof(dir_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_dir;
    /* the WHOLE spec, params included, the way sqlite and postgres keep it:
       ix_load() rebuilds a dictionary's spec from this and has to put the
       "DICT." after the params, not in front of them */
    f->base.spec = strdup(spec);
    f->path = strdup(path);
    return (mvx_file *)f;
}

static void dir_close(mvx_file *fh) {
    dir_file *f = (dir_file *)fh;
    free(f->base.spec);
    free(f->path);
    free(f);
}

static int dir_read(mvx_file *fh, const char *id, int64_t idlen,
                    mv_value *rec) {
    dir_file *f = (dir_file *)fh;
    if (!id_ok(id, idlen)) return 0;
    char *p = rec_path(f, id, idlen);
    FILE *fp = fopen(p, "rb");
    free(p);
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) mvx_fatal("out of memory reading record");
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        return 0;
    }
    fclose(fp);

    int64_t n = sz;
    while (n > 0 && buf[n - 1] == '\n') n--;    /* trailing newline */
    for (int64_t i = 0; i < n; i++)
        if (buf[i] == '\n') buf[i] = AM;
    mv_set_str(rec, buf, n);
    free(buf);
    return 1;
}

static int dir_write(mvx_file *fh, const char *id, int64_t idlen,
                     const mv_value *rec) {
    dir_file *f = (dir_file *)fh;
    if (!id_ok(id, idlen)) return 0;
    char nb[40];
    const char *rp;
    int64_t rlen = mv_val_chars(rec, nb, sizeof nb, &rp);

    char *p = rec_path(f, id, idlen);
    FILE *fp = fopen(p, "wb");
    free(p);
    if (!fp) return 0;
    for (int64_t i = 0; i < rlen; i++) {
        char c = rp[i] == AM ? '\n' : rp[i];
        if (fputc(c, fp) == EOF) { fclose(fp); return 0; }
    }
    if (rlen > 0 && fputc('\n', fp) == EOF) { fclose(fp); return 0; }
    return fclose(fp) == 0;
}

static int dir_del(mvx_file *fh, const char *id, int64_t idlen) {
    dir_file *f = (dir_file *)fh;
    if (!id_ok(id, idlen)) return 0;
    char *p = rec_path(f, id, idlen);
    int rc = unlink(p);
    free(p);
    return rc == 0;
}

static int cmp_ids(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static mvx_cursor *dir_select_begin(mvx_file *fh) {
    dir_file *f = (dir_file *)fh;
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in SELECT");
    DIR *d = opendir(f->path);
    if (!d) return c;
    int64_t cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;      /* ignore-pattern: dotfiles */
        char *p = rec_path(f, e->d_name, (int64_t)strlen(e->d_name));
        struct stat sb;
        int regular = stat(p, &sb) == 0 && S_ISREG(sb.st_mode);
        free(p);
        if (!regular) continue;
        if (c->n == cap) {
            cap = cap ? cap * 2 : 64;
            char **ns = realloc(c->ids, (size_t)cap * sizeof(char *));
            if (!ns) mvx_fatal("out of memory in SELECT");
            c->ids = ns;
        }
        c->ids[c->n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(c->ids, (size_t)c->n, sizeof(char *), cmp_ids);  /* deterministic */
    return c;
}

static int dir_select_next(mvx_cursor *c, mv_value *id) {
    if (c->pos >= c->n) return 0;
    char *s = c->ids[c->pos++];
    mv_set_str(id, s, (int64_t)strlen(s));
    return 1;
}

static void dir_select_end(mvx_cursor *c) {
    for (int64_t i = 0; i < c->n; i++) free(c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t dir_select_count(mvx_cursor *c) { return c ? c->n : 0; }

static int dir_create(const char *spec, char *err, size_t errlen) {
    char path[4096];
    spec_path(spec, path, sizeof path);
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        snprintf(err, errlen, "dir: cannot create %s", path);
        return 0;
    }
    return 1;                    /* idempotent: an existing directory is fine */
}

static int dir_remove(const char *spec, char *err, size_t errlen) {
    char path[4096];
    spec_path(spec, path, sizeof path);
    DIR *d = opendir(path);
    if (!d) {
        snprintf(err, errlen, "dir: %s does not exist", path);
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char p[4352];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        unlink(p);                      /* leaves subdirectories behind */
    }
    closedir(d);
    if (rmdir(path) != 0) {
        snprintf(err, errlen, "dir: %s not empty", path);
        return 0;
    }
    return 1;
}

static const mvx_driver mvx_driver_dir = {
    "dir",
    dir_open, dir_close,
    dir_read, dir_write, dir_del,
    dir_select_begin, dir_select_next, dir_select_end,
    dir_create, dir_remove,
    NULL,                               /* names: the store scans dirs */
    NULL, NULL, NULL, NULL,             /* no native index capability */
    NULL, NULL,                         /* locks: runtime local table */
    .select_count = dir_select_count,
};

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_dir : NULL;
}
