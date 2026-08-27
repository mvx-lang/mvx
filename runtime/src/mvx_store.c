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

/* Storage runtime: file-spec resolution, the runtime lock table, and the
 * active select list.  Drivers plug in beneath via mvx_driver.h.
 *
 * Resolution (account root = $MVXACCOUNT or cwd):
 *   - spec names an existing directory        -> directory driver
 *   - otherwise                               -> named DB in the account's
 *                                                LMDB environment
 */
#include "mvx_driver.h"
#include "mvx_map.h"
#include "mv_bytes.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define DRV_SUFFIX ".dylib"
#else
#define DRV_SUFFIX ".so"
#endif

#ifndef MVX_DRIVER_DIR
#define MVX_DRIVER_DIR "."
#endif

/* ------------------------------------------------ driver loading (dlopen) */

typedef struct loaded_drv {
    char *name;
    const mvx_driver *drv;
    struct loaded_drv *next;
} loaded_drv;

static loaded_drv *g_drivers;

static const mvx_driver *driver_try(const char *dir, const char *name) {
    char path[4096];
    snprintf(path, sizeof path, "%s/libmvxdrv_%s" DRV_SUFFIX, dir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NULL;
    mvx_driver_entry_fn entry =
        (mvx_driver_entry_fn)dlsym(h, "mvx_driver_entry");
    const mvx_driver *d = entry ? entry(MVX_DRIVER_ABI) : NULL;
    if (!d) {
        dlclose(h);
        mvx_fatal("storage driver %s is not compatible (ABI %d)", path,
                  MVX_DRIVER_ABI);
    }
    return d;                           /* handle stays open for process life */
}

/* Find a driver by name — $MVXDRIVERS (colon-separated), then libmvxrt's own
   directory, then the compile-time one — and CACHE it.  NULL when there is no
   such driver here.

   Split out of driver_load so a caller can ASK about a driver instead of dying
   for want of it.  An account can perfectly well name a backend this host was
   not built with: migration is per FILE (ARCHITECTURE 4.4), so a repository's
   files need not all live on the same one, and a clone onto a machine without
   postgres is an ordinary thing to want to do rather than a misconfiguration.
   Deciding that is the caller's business, not the loader's. */
static const mvx_driver *driver_find(const char *name) {
    for (loaded_drv *l = g_drivers; l; l = l->next)
        if (strcmp(l->name, name) == 0) return l->drv;

    const mvx_driver *d = NULL;
    const char *sp = getenv("MVXDRIVERS");
    if (sp && sp[0]) {
        char *dup = strdup(sp);
        for (char *tok = strtok(dup, ":"); tok && !d;
             tok = strtok(NULL, ":"))
            d = driver_try(tok, name);
        free(dup);
    }
    /* Drivers ship in libmvxrt's own directory: relocatable, no baked
       path.  The compile-time dir is the last-resort fallback. */
    const char *rtd = mvx_runtime_dir();
    if (!d && rtd[0]) d = driver_try(rtd, name);
    if (!d) d = driver_try(MVX_DRIVER_DIR, name);
    if (!d) return NULL;

    loaded_drv *l = malloc(sizeof(loaded_drv));
    if (!l) mvx_fatal("out of memory loading driver");
    l->name = strdup(name);
    l->drv = d;
    l->next = g_drivers;
    g_drivers = l;
    return d;
}

/* Is this driver usable here?  The question a caller asks before offering the
   user a choice, and the reason driver_find exists. */
int mvx_driver_available(const char *name) {
    return name && name[0] && driver_find(name) != NULL;
}

/* The drivers this host actually has, as a comma-separated list — so a prompt
   can offer real options rather than ask the user to guess a name.  Read off
   the same directories the loader searches, by their file names, since that is
   the only enumeration there is: a driver is a libmvxdrv_<name> shared object.
   Names only; loading each one to confirm would be a lot of dlopen for a list
   that is about to be printed. */
static void driver_list_dir(const char *dir, char *out, size_t cap) {
    DIR *dh = opendir(dir);
    if (!dh) return;
    struct dirent *e;
    while ((e = readdir(dh))) {
        const char *n = e->d_name;
        if (strncmp(n, "libmvxdrv_", 10) != 0) continue;
        const char *suf = strstr(n + 10, DRV_SUFFIX);
        if (!suf || suf[strlen(DRV_SUFFIX)]) continue;
        char nm[64];
        size_t nl = (size_t)(suf - (n + 10));
        if (nl == 0 || nl >= sizeof nm) continue;
        snprintf(nm, sizeof nm, "%.*s", (int)nl, n + 10);
        /* one entry each, however many directories carry it */
        size_t have = strlen(out);
        char probe[70];
        snprintf(probe, sizeof probe, "%s,", nm);
        if (strstr(out, probe)) continue;
        snprintf(out + have, cap - have, "%s,", nm);
    }
    closedir(dh);
}

void mvx_driver_names(char *out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    const char *sp = getenv("MVXDRIVERS");
    if (sp && sp[0]) {
        char *dup = strdup(sp);
        for (char *tok = strtok(dup, ":"); tok; tok = strtok(NULL, ":"))
            driver_list_dir(tok, out, cap);
        free(dup);
    }
    const char *rtd = mvx_runtime_dir();
    if (rtd[0]) driver_list_dir(rtd, out, cap);
    driver_list_dir(MVX_DRIVER_DIR, out, cap);
    size_t n = strlen(out);
    if (n && out[n - 1] == ',') out[n - 1] = '\0';   /* drop the trailer */
}

/* Resolve a driver by name, or die.  For callers with nothing to fall back on;
   anything that can offer the user a choice asks mvx_driver_available first. */
static const mvx_driver *driver_load(const char *name) {
    const mvx_driver *d = driver_find(name);
    if (!d) {
        const char *rtd = mvx_runtime_dir();
        mvx_fatal("cannot load storage driver \"%s\" "
                  "(searched $MVXDRIVERS, %s and %s)",
                  name, rtd[0] ? rtd : "(runtime dir)", MVX_DRIVER_DIR);
    }
    return d;
}

/* ------------------------------------------------------- per-ctx state */

typedef struct lock_ent {
    char *key;                          /* spec \x01 id */
    mvx_file *f;                        /* for driver-held locks */
    struct lock_ent *next;
} lock_ent;

#define IX_MAX_ITEMS 16
#define IX_MAX_VALS 32
#define IX_KEY_MAX 511

typedef struct ixmeta {
    int loaded;
    int n;
    struct {
        char item[128];
        int64_t attr;
    } it[IX_MAX_ITEMS];
} ixmeta;

/* The relational mapping (mapmeta) and its pure projection/typing engine live in
   the shared mapper (mvx_map.h); this file keeps the persistence side. */

typedef struct open_file {
    mvx_file *f;
    ixmeta ix;
    mapmeta map;
    struct open_file *next;
} open_file;

/* mapping helpers, defined below but used by the WRITE mirror hook */
static void map_load(open_file *o);
static int map_project_one(mvx_ctx *ctx, mvx_file *f, mapmeta *m,
                           const char *id, int64_t idlen,
                           const mv_value *rec);
static int map_project(mvx_ctx *ctx, mvx_file *f, mapmeta *m, const char *id,
                       int64_t idlen, const mv_value *rec,
                       const mv_value *old);
static int map_validate_one(mvx_ctx *ctx, mapmeta *m, const mv_value *rec);
static int map_recompose(mvx_ctx *ctx, mvx_file *f, mapmeta *m,
                         const char *id, int64_t idlen, mv_value *rec);
static const char *map_identity_col(open_file *o, int64_t attr);

/* A file opened on demand by TRANS(), cached for the session so a query
   does not reopen (and reconnect) its lookup target once per record. */
typedef struct trans_ent {
    char name[64];
    int64_t nlen;
    mv_value fvar;                      /* MV_FILE handle, or unopened */
    int ok;
    mv_value dfvar;                     /* the file's DICT handle (lazy) */
    int dok;                            /* 0 untried, 1 open, -1 failed */
    struct trans_ent *next;
} trans_ent;

typedef struct store_state {
    open_file *files;
    lock_ent *locks;
    mv_value *sel_ids;                  /* materialised select list */
    int64_t sel_n, sel_pos;
    int sel_active;                     /* a list was formed this process */
    trans_ent *trans;                   /* TRANS() lookup-file cache */
} store_state;

static void sel_push(store_state *st, int64_t *cap, const char *p,
                     int64_t len) {
    if (st->sel_n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        mv_value *ns = realloc(st->sel_ids,
                               (size_t)*cap * sizeof(mv_value));
        if (!ns) mvx_fatal("out of memory in select list");
        st->sel_ids = ns;
    }
    mv_init(&st->sel_ids[st->sel_n]);
    mv_set_str(&st->sel_ids[st->sel_n], p, len);
    st->sel_n++;
}

/* --------------------------------------------------- session select list
   The session/select-list seam (ARCHITECTURE.md 7.3): a program ending
   with an unconsumed select list persists the remainder to the session
   file ($MVXSESSION, owned by the TCL session); the next program's
   first READNEXT picks it up.  That is how "SELECT ..." feeds the next
   command, classic style, across processes.  Ids are newline-separated
   (record ids containing newlines are not supported here).           */

static void session_load(store_state *st) {
    if (st->sel_active) return;
    const char *sf = getenv("MVXSESSION");
    if (!sf || !sf[0]) return;
    FILE *fp = fopen(sf, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return; }
    char *buf = malloc((size_t)sz);
    if (!buf) mvx_fatal("out of memory loading select list");
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(buf);
        return;
    }
    fclose(fp);
    fclose(fopen(sf, "wb"));            /* consumed exactly once */

    st->sel_active = 1;
    int64_t cap = 0;
    const char *p = buf, *end = buf + sz;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        int64_t len = (nl ? nl : end) - p;
        if (len > 0) sel_push(st, &cap, p, len);
        p = nl ? nl + 1 : end;
    }
    free(buf);
}

static void session_save(store_state *st) {
    if (!st->sel_active || st->sel_pos >= st->sel_n) return;
    const char *sf = getenv("MVXSESSION");
    if (!sf || !sf[0]) return;
    FILE *fp = fopen(sf, "wb");
    if (!fp) return;
    for (int64_t i = st->sel_pos; i < st->sel_n; i++) {
        char nb[40];
        const char *p;
        int64_t len = mv_val_chars(&st->sel_ids[i], nb, sizeof nb, &p);
        fwrite(p, 1, (size_t)len, fp);
        fputc('\n', fp);
    }
    fclose(fp);
}

static store_state *state(mvx_ctx *ctx) {
    store_state *st = mvx_ctx_store_get(ctx);
    if (!st) {
        st = calloc(1, sizeof(store_state));
        if (!st) mvx_fatal("out of memory creating store state");
        mvx_ctx_store_set(ctx, st);
    }
    return st;
}

static void clear_select(store_state *st) {
    for (int64_t i = 0; i < st->sel_n; i++) mv_clear(&st->sel_ids[i]);
    free(st->sel_ids);
    st->sel_ids = NULL;
    st->sel_n = st->sel_pos = 0;
}

void mvx_store_shutdown(mvx_ctx *ctx) {
    store_state *st = mvx_ctx_store_get(ctx);
    if (!st) return;
    session_save(st);                   /* hand leftover list to session */
    clear_select(st);
    for (lock_ent *l = st->locks; l;) {
        lock_ent *n = l->next;
        free(l->key);
        free(l);
        l = n;
    }
    for (open_file *o = st->files; o;) {
        open_file *n = o->next;
        mvx_file_base *b = (mvx_file_base *)o->f;
        b->driver->close(o->f);
        free(o);
        o = n;
    }
    free(st);
    mvx_ctx_store_set(ctx, NULL);
}

/* ------------------------------------------------------------- helpers */

static mvx_file *file_of(const mv_value *fvar, const char *what) {
    if (fvar->tag != MV_FILE || fvar->i == 0)
        mvx_fatal("%s: variable is not an open file variable", what);
    return (mvx_file *)(intptr_t)fvar->i;
}

static char *id_chars(const mv_value *id, char *buf, size_t cap,
                      int64_t *len) {
    const char *p;
    *len = mv_val_chars(id, buf, cap, &p);
    if (*len == 0) mvx_fatal("empty record id");
    return (char *)p;
}

static char *lock_key(mvx_file *f, const char *id, int64_t idlen) {
    mvx_file_base *b = (mvx_file_base *)f;
    size_t sl = strlen(b->spec);
    char *k = malloc(sl + 1 + (size_t)idlen + 1);
    if (!k) mvx_fatal("out of memory in lock table");
    memcpy(k, b->spec, sl);
    k[sl] = '\x01';
    memcpy(k + sl + 1, id, (size_t)idlen);
    k[sl + 1 + idlen] = '\0';
    return k;
}

/* Drop a lock entry; when the backend is the lock authority, tell it. */
static void lock_drop(store_state *st, const char *key) {
    for (lock_ent **pp = &st->locks; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->key, key) == 0) {
            lock_ent *dead = *pp;
            *pp = dead->next;
            if (dead->f) {
                mvx_file_base *b = (mvx_file_base *)dead->f;
                if (b->driver->unlock) {
                    const char *id = strchr(dead->key, '\x01') + 1;
                    b->driver->unlock(dead->f, id,
                                      (int64_t)strlen(id));
                }
            }
            free(dead->key);
            free(dead);
            return;
        }
    }
}

/* ----------------------------------------------------------------- API */

/* The account's default namespace on a daemon: the basename of the
   resolved MVXACCOUNT path (so ".", "./acct", and "/x/acct" all agree),
   or "default".  Shared by the store and the lmdbnet driver so a
   whole-account binding and its LISTF land in the same namespace. */
void mvx_account_namespace(char *out, size_t outlen) {
    const char *a = getenv("MVXACCOUNT");
    if (!a || !a[0]) a = ".";
    char rp[4096];
    const char *path = realpath(a, rp) ? rp : a;
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    size_t s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    size_t len = n - s;
    if (len == 0 || (len == 1 && path[s] == '.')) {
        snprintf(out, outlen, "default");
        return;
    }
    if (len >= outlen) len = outlen - 1;
    memcpy(out, path + s, len);
    out[len] = '\0';
}

/* Does this file have a backend binding?  Consult the account's
   BINDINGS record, whose lines are "SPEC driver {params...}" (an exact
   spec, or "*" for every LMDB file); driver names a storage driver
   (lmdbnet, and later postgres, mongo, ...) and params is its
   connection string, opaque to the runtime.  With no BINDINGS record,
   bare $MVXDAEMON binds the whole account to lmdbnet.  Returns 1 when
   bound, filling driver and params. */
static int binding_for(const char *cspec, char *driver, size_t dcap,
                       char *params, size_t pcap) {
    const char *envd = getenv("MVXDAEMON");
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    char path[4096];
    snprintf(path, sizeof path, "%s/BINDINGS", acct);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (envd && envd[0]) {
            char nsb[128];
            mvx_account_namespace(nsb, sizeof nsb);
            snprintf(driver, dcap, "lmdbnet");
            snprintf(params, pcap, "%s %s", envd, nsb);
            return 1;
        }
        return 0;
    }
    int star = 0, exact = 0;
    char stardrv[64] = "", starparm[512] = "";
    char exactdrv[64] = "", exactparm[512] = "";
    char ln[1152];
    while (fgets(ln, sizeof ln, fp)) {
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        char *sp = p;
        while (*sp && *sp != ' ' && *sp != '\t' && *sp != '\n' &&
               *sp != '\r')
            sp++;
        size_t nl = (size_t)(sp - p);
        if (nl == 0 || *p == '#') continue;
        char *dp = sp;                  /* driver field */
        while (*dp == ' ' || *dp == '\t') dp++;
        char *de = dp;
        while (*de && *de != ' ' && *de != '\t' && *de != '\n' &&
               *de != '\r')
            de++;
        char *pp = de;                  /* params: rest of the line */
        while (*pp == ' ' || *pp == '\t') pp++;
        char *pe = pp;
        while (*pe && *pe != '\n' && *pe != '\r') pe++;
        char *drvbuf, *parmbuf;
        size_t dl, pl;
        if (nl == 1 && p[0] == '*') {
            star = 1; drvbuf = stardrv; parmbuf = starparm;
            dl = sizeof stardrv; pl = sizeof starparm;
        } else if (strlen(cspec) == nl && memcmp(p, cspec, nl) == 0) {
            exact = 1; drvbuf = exactdrv; parmbuf = exactparm;
            dl = sizeof exactdrv; pl = sizeof exactparm;
        } else {
            continue;
        }
        snprintf(drvbuf, dl, "%.*s", (int)(de - dp), dp);
        snprintf(parmbuf, pl, "%.*s", (int)(pe - pp), pp);
    }
    fclose(fp);
    if (!exact && !star) return 0;
    const char *ud = exact ? exactdrv : stardrv;
    const char *up = exact ? exactparm : starparm;
    /* `@name` binds to a named connection profile: the driver comes from
       the profile, and the "@name" reference passes through so the
       driver resolves the address/namespace/token itself. */
    if (ud[0] == '@') {
        const char *cn = ud + 1;
        static char cdriver[64];
        if (!mvx_conn_lookup(cn, "driver", cdriver, sizeof cdriver))
            mvx_fatal("file %s: connection '%s' is not defined "
                      "(SET-CONNECTION %s driver=...)", cspec, cn, cn);
        snprintf(driver, dcap, "%s", cdriver);
        snprintf(params, pcap, "@%s", cn);
        return 1;
    }
    if (!ud[0]) ud = "lmdbnet";         /* default backend */
    if (!up[0] && strcmp(ud, "lmdbnet") == 0)
        up = envd && envd[0] ? envd : "";
    if (strcmp(ud, "lmdbnet") == 0 && !up[0])
        mvx_fatal("file %s is bound to lmdbnet but no daemon address "
                  "is configured (BINDINGS line or $MVXDAEMON)", cspec);
    snprintf(driver, dcap, "%s", ud);
    /* lmdbnet params are "addr [namespace]"; supply the account's
       default namespace when the binding names only an address. */
    static char nsparm[640];
    if (strcmp(ud, "lmdbnet") == 0) {
        const char *sp = up;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        while (*sp == ' ' || *sp == '\t') sp++;
        if (!*sp) {
            char nsb[128];
            mvx_account_namespace(nsb, sizeof nsb);
            snprintf(nsparm, sizeof nsparm, "%s %s", up, nsb);
            up = nsparm;
        }
    }
    snprintf(params, pcap, "%s", up);
    return 1;
}

/* Resolve a spec to its driver, and derive the dictionary spec when
   asked: DICT.<spec> as a sibling LMDB named DB, and <spec>.DICT as a
   sibling directory for directory files — so a directory file NAME and
   its dictionary NAME.DICT sit side by side (BP and BP.DICT), matching
   the git-legible form exactly. */
static const mvx_driver *resolve(const char *cspec, int want_dict,
                                 char *outspec, size_t cap) {
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";

    char path[4096];
    if (cspec[0] == '/')
        snprintf(path, sizeof path, "%s", cspec);
    else
        snprintf(path, sizeof path, "%s/%s", acct, cspec);

    struct stat sb;
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
        snprintf(outspec, cap, want_dict ? "%s.DICT" : "%s", cspec);
        return driver_load("dir");
    }
    /* Per-file backend binding (ARCHITECTURE.md 4.4: migration is per
       file).  A file may be bound to a networked or foreign backend by
       the account's BINDINGS record; unbound LMDB files are local.
       The connection params ride in the driver-level spec as
       "params\nspec" - opaque to the runtime, parsed by the driver. */
    char driver[64], params[512];
    if (binding_for(cspec, driver, sizeof driver, params, sizeof params)) {
        snprintf(outspec, cap, want_dict ? "%s\nDICT.%s" : "%s\n%s",
                 params, cspec);
        return driver_load(driver);
    }
    snprintf(outspec, cap, want_dict ? "DICT.%s" : "%s", cspec);
    return driver_load("lmdb");
}

int64_t mvx_open(mvx_ctx *ctx, const mv_value *dict, const mv_value *spec,
                 mv_value *fvar) {
    char db[40], sb[64];
    const char *dp = "";
    if (dict) mv_val_chars(dict, db, sizeof db, &dp);
    int want_dict = 0;
    if (dp[0] != '\0') {
        if (strcasecmp(dp, "DICT") == 0)
            want_dict = 1;
        else
            return 0;
    }

    const char *sp;
    int64_t slen = mv_val_chars(spec, sb, sizeof sb, &sp);
    if (slen == 0 || slen > 900 || memchr(sp, '\x01', (size_t)slen))
        return 0;

    char cspec[1024];
    memcpy(cspec, sp, (size_t)slen);
    cspec[slen] = '\0';

    char rspec[1152];
    const mvx_driver *drv = resolve(cspec, want_dict, rspec, sizeof rspec);

    char err[256] = "";
    mvx_file *f = drv->open(rspec, err, sizeof err);
    if (!f) {
        /* A plain missing file is the normal ELSE path and stays
           silent; an infrastructure failure must say why. */
        if (err[0])
            fprintf(stderr, "OPEN %s: %s\n", cspec, err);
        return 0;
    }

    store_state *st = state(ctx);
    /* calloc, not malloc: the index metadata (o->ix) must start zeroed,
       or ix_load reads a garbage "loaded" flag and skips initialising it,
       leaving ix_diff to walk a garbage index count. */
    open_file *o = calloc(1, sizeof(open_file));
    if (!o) mvx_fatal("out of memory in OPEN");
    o->f = f;
    o->next = st->files;
    st->files = o;

    mv_clear(fvar);
    fvar->tag = MV_FILE;
    fvar->i = (int64_t)(intptr_t)f;
    return 1;
}

/* ------------------------------------------------------------ indexing
   Metadata: the DICT record %INDEXES% lists indexed item names, one
   per attribute; each item's dictionary record supplies the attribute
   number.  Cached per open file; CREATE-INDEX / DELETE-INDEX
   invalidate through mvx_index_build / mvx_index_drop.  Maintenance
   is diff-based: only entries whose values actually changed are
   touched (ARCHITECTURE.md 5.3). */

static open_file *find_open(store_state *st, mvx_file *f) {
    for (open_file *o = st->files; o; o = o->next)
        if (o->f == f) return o;
    return NULL;
}

static void ix_load(open_file *o) {
    if (o->ix.loaded) return;
    o->ix.loaded = 1;
    o->ix.n = 0;
    mvx_file_base *b = (mvx_file_base *)o->f;
    /* Either MVX maintains the index (write_ix, LMDB) or the backend does
       (index_select over a mapped column, Postgres). */
    if (!b->driver->write_ix && !b->driver->index_select) return;

    char dspec[1720];
    const char *nl = strchr(b->spec, '\n');
    if (nl)
        snprintf(dspec, sizeof dspec, "%.*s\nDICT.%s",
                 (int)(nl - b->spec), b->spec, nl + 1);
    else
        snprintf(dspec, sizeof dspec, "DICT.%s", b->spec);
    char err[256] = "";
    mvx_file *d = b->driver->open(dspec, err, sizeof err);
    if (!d) return;

    mv_value xl, item, drec, ano;
    mv_init(&xl); mv_init(&item); mv_init(&drec); mv_init(&ano);
    if (b->driver->read(d, "%INDEXES%", 9, &xl)) {
        char nb[40];
        const char *p;
        int64_t len = mv_val_chars(&xl, nb, sizeof nb, &p);
        int64_t nattr = len ? 1 : 0;
        for (int64_t i = 0; i < len; i++)
            if (p[i] == '\xFE') nattr++;
        for (int64_t a = 1; a <= nattr && o->ix.n < IX_MAX_ITEMS; a++) {
            mv_extract_fn(&item, &xl, a, 0, 0);
            char ib[40];
            const char *ip;
            int64_t il = mv_val_chars(&item, ib, sizeof ib, &ip);
            if (il <= 0 || il >= 127) continue;
            char iname[128];
            memcpy(iname, ip, (size_t)il);
            iname[il] = '\0';
            if (!b->driver->read(d, iname, il, &drec)) continue;
            mv_extract_fn(&ano, &drec, 2, 0, 0);
            int64_t attr = mv_get_int(&ano);
            if (attr < 1) continue;
            memcpy(o->ix.it[o->ix.n].item, iname, (size_t)il + 1);
            o->ix.it[o->ix.n].attr = attr;
            o->ix.n++;
        }
    }
    mv_clear(&xl); mv_clear(&item); mv_clear(&drec); mv_clear(&ano);
    b->driver->close(d);
}

/* Values of one attribute, split on value marks; empty values and keys
   over the backend limit are not indexed. */
typedef struct ixvals {
    int n;
    char v[IX_MAX_VALS][IX_KEY_MAX + 1];
    int64_t len[IX_MAX_VALS];
} ixvals;

static void ix_values(const mv_value *rec, int64_t attr, ixvals *out) {
    out->n = 0;
    mv_value a;
    mv_init(&a);
    mv_extract_fn(&a, rec, attr, 0, 0);
    char nb[40];
    const char *p;
    int64_t len = mv_val_chars(&a, nb, sizeof nb, &p);
    const char *end = p + len;
    while (p < end && out->n < IX_MAX_VALS) {
        const char *vm = memchr(p, '\xFD', (size_t)(end - p));
        int64_t n = (vm ? vm : end) - p;
        if (n > 0 && n <= IX_KEY_MAX) {
            memcpy(out->v[out->n], p, (size_t)n);
            out->v[out->n][n] = '\0';
            out->len[out->n] = n;
            out->n++;
        }
        p = vm ? vm + 1 : end;
    }
    mv_clear(&a);
}

static int ixvals_has(const ixvals *vs, const char *p, int64_t n) {
    for (int i = 0; i < vs->n; i++)
        if (vs->len[i] == n && memcmp(vs->v[i], p, (size_t)n) == 0)
            return 1;
    return 0;
}

/* Build the delta list for one record transition old -> new. */
static int ix_diff(open_file *o, const mv_value *oldrec, int hadold,
                   const mv_value *newrec, mvx_ixop *ops, ixvals *pool) {
    int nops = 0;
    for (int k = 0; k < o->ix.n; k++) {
        ixvals *ov = &pool[k * 2], *nv = &pool[k * 2 + 1];
        ov->n = 0;
        if (hadold) ix_values(oldrec, o->ix.it[k].attr, ov);
        nv->n = 0;
        if (newrec) ix_values(newrec, o->ix.it[k].attr, nv);
        for (int i = 0; i < ov->n; i++)
            if (!ixvals_has(nv, ov->v[i], ov->len[i])) {
                ops[nops].item = o->ix.it[k].item;
                ops[nops].key = ov->v[i];
                ops[nops].klen = ov->len[i];
                ops[nops].add = 0;
                nops++;
            }
        for (int i = 0; i < nv->n; i++)
            if (!ixvals_has(ov, nv->v[i], nv->len[i])) {
                ops[nops].item = o->ix.it[k].item;
                ops[nops].key = nv->v[i];
                ops[nops].klen = nv->len[i];
                ops[nops].add = 1;
                nops++;
            }
    }
    return nops;
}

int64_t mvx_read(mvx_ctx *ctx, mv_value *rec, const mv_value *fvar,
                 const mv_value *id, int64_t lock) {
    mvx_file *f = file_of(fvar, "READ");
    mvx_file_base *b = (mvx_file_base *)f;
    char ib[40];
    int64_t idlen;
    const char *ip = id_chars(id, ib, sizeof ib, &idlen);

    if (lock) {
        store_state *st = state(ctx);
        char *key = lock_key(f, ip, idlen);
        lock_drop(st, key);             /* re-lock by same session is fine */
        /* A mirror-mapped file's SQL columns and association child tables are
           only a derived projection — the record blob stays authoritative — so
           it uses the process-local lock table, not the backend lock.  Native-
           mapped files (SQL is the source of truth) and unmapped files (the
           blob is, in the backend) are backend-authoritative and take the
           backend lock, so it arbitrates across processes (the advisory lock
           for Postgres). */
        open_file *lo = find_open(st, f);
        if (lo) map_load(lo);
        int mirror_mapped = lo && lo->map.nf > 0 && !lo->map.native;
        int backend_lock = b->driver->lock && !mirror_mapped;
        if (backend_lock) {
            if (lock == 2) {
                /* READU ... LOCKED: try once; on contention report -1
                   without reading or taking the lock. */
                if (!b->driver->lock(f, ip, idlen)) {
                    free(key);
                    return -1;
                }
            } else {
                /* Backend is the lock authority: classic READU blocks
                   until the holder releases (or its connection drops). */
                while (!b->driver->lock(f, ip, idlen)) usleep(50000);
            }
        }
        lock_ent *l = malloc(sizeof(lock_ent));
        if (!l) mvx_fatal("out of memory in lock table");
        l->key = key;
        l->f = backend_lock ? f : NULL;
        l->next = st->locks;
        st->locks = l;
    }
    int had = b->driver->read(f, ip, idlen, rec);
    /* Native mode: the relational form is the source of truth, so overlay
       the mapped attributes from the columns/child tables — surfacing writes
       an external tool made to the SQL directly, and even records it inserted
       that MVX never wrote (present in SQL, no rec blob). */
    open_file *o = find_open(state(ctx), f);
    if (o) map_load(o);
    if (o && o->map.nf > 0 && o->map.native &&
        ((mvx_file_base *)f)->driver->map_read) {
        if (!had) mv_set_str(rec, "", 0);   /* SQL-only insert: empty base */
        int pr = map_recompose(ctx, f, &o->map, ip, idlen, rec);
        if (pr > 0) return 1;
        if (pr == 0 && !had) return 0;
    }
    return had;
}

/* Open (once, then cached) the file TRANS() looks up.  The handle lives for
   the session so a query does not reopen its target per record. */
static trans_ent *trans_file(mvx_ctx *ctx, const char *nm, int64_t nl) {
    store_state *st = state(ctx);
    for (trans_ent *t = st->trans; t; t = t->next)
        if (t->nlen == nl && memcmp(t->name, nm, (size_t)nl) == 0) return t;
    trans_ent *t = calloc(1, sizeof *t);
    if (!t) mvx_fatal("out of memory in TRANS");
    if (nl >= (int64_t)sizeof t->name) nl = (int64_t)sizeof t->name - 1;
    memcpy(t->name, nm, (size_t)nl);
    t->name[nl] = '\0';
    t->nlen = nl;
    mv_value spec;
    mv_init(&spec);
    mv_set_str(&spec, nm, nl);
    mv_init(&t->fvar);
    t->ok = mvx_open(ctx, NULL, &spec, &t->fvar) ? 1 : 0;
    mv_clear(&spec);
    t->next = st->trans;
    st->trans = t;
    return t;
}

/* --- runtime I-type evaluator (#63) ----------------------------------------
   I-type descriptors (TRANS, DOCTAG) are evaluated here rather than only in the
   verbs, so a TRANS whose target names a dictionary item (a *nested* TRANS,
   #53a) resolves recursively.  ieval / dict_eval / trans_core are mutually
   recursive; a depth cap stops a self-referential dictionary looping. */
#define IEVAL_MAXDEPTH 16

static void ieval(mvx_ctx *ctx, mv_value *dst, const mv_value *rec,
                  const char *sp, int64_t sl, int depth);
static void trans_core(mvx_ctx *ctx, mv_value *dst, const char *np, int64_t nl,
                       const mv_value *key, const char *attr, int64_t attrl,
                       char ctl, int depth);

/* Open (lazily, cached) the DICT of a TRANS lookup file. */
static int trans_dict(mvx_ctx *ctx, trans_ent *t) {
    if (t->dok == 0) {
        mv_value dictv, fname;
        mv_init(&dictv);
        mv_set_str(&dictv, "DICT", 4);
        mv_init(&fname);
        mv_set_str(&fname, t->name, t->nlen);
        mv_init(&t->dfvar);
        t->dok = mvx_open(ctx, &dictv, &fname, &t->dfvar) ? 1 : -1;
        mv_clear(&dictv);
        mv_clear(&fname);
    }
    return t->dok > 0;
}

/* DOCTAG(tag): the value after "@tag " on the first comment line (* or !). */
static void ieval_doctag(mv_value *dst, const mv_value *rec, const char *tag,
                         int64_t tlen) {
    mv_set_str(dst, "", 0);
    if (tlen >= 2 && (tag[0] == '\'' || tag[0] == '"') && tag[tlen - 1] == tag[0]) {
        tag++;
        tlen -= 2;
    }
    if (tlen <= 0 || tlen > 200) return;
    char pat[204];
    pat[0] = '@';
    memcpy(pat + 1, tag, (size_t)tlen);
    pat[tlen + 1] = ' ';
    size_t patlen = (size_t)tlen + 2;
    char nb[40];
    const char *rp;
    int64_t rl = mv_val_chars(rec, nb, sizeof nb, &rp);
    const char *p = rp, *end = rp + rl;
    while (p <= end) {
        const char *am = memchr(p, '\xFE', (size_t)(end - p));
        const char *le = am ? am : end;
        const char *ls = p;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ls++;
        if (ls < le && (*ls == '*' || *ls == '!')) {
            const char *hit = memmem(ls, (size_t)(le - ls), pat, patlen);
            if (hit) {
                const char *vs = hit + patlen;
                while (vs < le && (*vs == ' ' || *vs == '\t')) vs++;
                const char *vend = le;
                while (vend > vs && (vend[-1] == ' ' || vend[-1] == '\t')) vend--;
                mv_set_str(dst, vs, vend - vs);
                return;
            }
        }
        if (!am) break;
        p = am + 1;
    }
}

/* Resolve dictionary item `item` of lookup file `t` against target record
   `rec` (id `id` for an @ID/attr-0 item): a D-type yields the attribute, an
   I-type is evaluated (recursively). */
static void dict_eval(mvx_ctx *ctx, mv_value *dst, trans_ent *t,
                      const char *item, int64_t ilen, const mv_value *rec,
                      const mv_value *id, int depth) {
    mv_set_str(dst, "", 0);
    if (depth >= IEVAL_MAXDEPTH || !t || !trans_dict(ctx, t)) return;
    mv_value key, di;
    mv_init(&key);
    mv_init(&di);
    mv_set_str(&key, item, ilen);
    if (mvx_read(ctx, &di, &t->dfvar, &key, 0) > 0) {
        mv_value ty;
        mv_init(&ty);
        mv_extract_fn(&ty, &di, 1, 0, 0);
        char tb[8];
        const char *tp;
        int64_t tl = mv_val_chars(&ty, tb, sizeof tb, &tp);
        if (tl > 0 && (tp[0] == 'I' || tp[0] == 'i')) {
            mv_value spec;
            mv_init(&spec);
            mv_extract_fn(&spec, &di, 2, 0, 0);
            char sbf[256];
            const char *sp;
            int64_t sl = mv_val_chars(&spec, sbf, sizeof sbf, &sp);
            ieval(ctx, dst, rec, sp, sl, depth + 1);
            mv_clear(&spec);
        } else {
            mv_value amc;
            mv_init(&amc);
            mv_extract_fn(&amc, &di, 2, 0, 0);
            int64_t ano = mv_get_int(&amc);
            if (ano >= 1) mv_extract_fn(dst, rec, ano, 0, 0);
            else if (id) mv_copy(dst, id);      /* @ID / attr 0 -> the key */
            mv_clear(&amc);
        }
        mv_clear(&ty);
    }
    mv_clear(&key);
    mv_clear(&di);
}

/* Evaluate an I-descriptor `sp[0..sl)` against record `rec`. */
static void ieval(mvx_ctx *ctx, mv_value *dst, const mv_value *rec,
                  const char *sp, int64_t sl, int depth) {
    mv_set_str(dst, "", 0);
    if (depth >= IEVAL_MAXDEPTH || sl < 8 || sp[sl - 1] != ')') return;
    if (sl >= 9 && strncmp(sp, "DOCTAG(", 7) == 0) {
        ieval_doctag(dst, rec, sp + 7, sl - 8);
        return;
    }
    if (strncmp(sp, "TRANS(", 6) == 0) {
        char inner[256];
        int64_t il = sl - 7;                    /* between "TRANS(" and ")" */
        if (il <= 0 || il >= (int64_t)sizeof inner) return;
        memcpy(inner, sp + 6, (size_t)il);
        inner[il] = '\0';
        char *parts[4] = {inner, NULL, NULL, NULL};
        int np = 0;
        for (char *q = inner; *q; q++)
            if (*q == ',') { *q = '\0'; if (++np < 4) parts[np] = q + 1; }
        if (np < 2) return;
        int64_t keyattr = parts[1] ? strtoll(parts[1], NULL, 10) : 0;
        const char *tat = parts[2] ? parts[2] : "";
        char ctl = (parts[3] && parts[3][0]) ? parts[3][0] : 'X';
        mv_value kv;
        mv_init(&kv);
        mv_extract_fn(&kv, rec, keyattr, 0, 0);       /* key = current<keyattr> */
        trans_core(ctx, dst, parts[0], (int64_t)strlen(parts[0]), &kv, tat,
                   (int64_t)strlen(tat), ctl, depth);
        mv_clear(&kv);
    }
}

/* Is [s,n) a decimal integer (an attribute number) rather than a dict-item
   name?  An empty target is attr 0. */
static int attr_is_numeric(const char *s, int64_t n) {
    if (n == 0) return 1;
    int64_t i = (s[0] == '-') ? 1 : 0;
    if (i == n) return 0;
    for (; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

/* The guts of TRANS: look up each @VM element of `key` in `file` and take
   target attribute `attr` — a number (classic, a raw attribute) or a dict-item
   name evaluated through the target's dictionary (nested, #53a).  Results are
   rejoined with @VM; a miss is "" ('X') or the key ('C'). */
static void trans_core(mvx_ctx *ctx, mv_value *dst, const char *np, int64_t nl,
                       const mv_value *key, const char *attr, int64_t attrl,
                       char ctl, int depth) {
    int numeric = attr_is_numeric(attr, attrl);
    int64_t attrno = numeric ? strtoll(attr, NULL, 10) : 0;
    char kb[64];
    const char *kp;
    int64_t kl = mv_val_chars(key, kb, sizeof kb, &kp);
    trans_ent *t = (nl > 0) ? trans_file(ctx, np, nl) : NULL;

    char *buf = NULL;
    size_t len = 0, cap = 0;
    const char *p = kp, *end = kp + kl;
    int first = 1;
    while (1) {
        const char *vm = memchr(p, '\xFD', (size_t)(end - p));
        const char *ve = vm ? vm : end;
        int64_t vlen = ve - p;

        mv_value one;
        mv_init(&one);
        int got = 0;
        if (t && t->ok && vlen > 0) {
            mv_value kv, rec;
            mv_init(&kv);
            mv_init(&rec);
            mv_set_str(&kv, p, vlen);
            if (mvx_read(ctx, &rec, &t->fvar, &kv, 0) > 0) {
                if (numeric) {
                    if (attrno <= 0) mv_set_str(&one, p, vlen);  /* attr 0 -> key */
                    else mv_extract_fn(&one, &rec, attrno, 0, 0);
                } else {
                    dict_eval(ctx, &one, t, attr, attrl, &rec, &kv, depth);
                }
                got = 1;
            }
            mv_clear(&rec);
            mv_clear(&kv);
        }
        if (!got) {
            if (ctl == 'C') mv_set_str(&one, p, vlen);   /* missing -> the key */
            else mv_set_str(&one, "", 0);                /* X (default) -> null */
        }

        char ob[64];
        const char *op;
        int64_t ol = mv_val_chars(&one, ob, sizeof ob, &op);
        size_t need = (size_t)ol + 1;
        if (len + need > cap) {
            cap = cap ? cap * 2 : 128;
            while (cap < len + need) cap *= 2;
            char *nb2 = realloc(buf, cap);
            if (!nb2) mvx_fatal("out of memory in TRANS");
            buf = nb2;
        }
        if (!first) buf[len++] = (char)0xFD;
        memcpy(buf + len, op, (size_t)ol);
        len += (size_t)ol;
        mv_clear(&one);
        first = 0;

        if (!vm) break;
        p = vm + 1;
    }
    mv_set_str(dst, buf ? buf : "", (int64_t)len);
    free(buf);
}

/* TRANS(file, key, attr, control): read record `key` from `file` and return
   its attribute `attr` (a number, or a dict-item name for a nested lookup;
   attr 0 = the key itself).  A missing record yields the empty string, or the
   key when control is "C".  The reference (per-record) semantics; #40/#53
   push it down to a JOIN when co-located and the target is a plain attribute. */
void mvx_trans(mvx_ctx *ctx, mv_value *dst, const mv_value *fname,
               const mv_value *key, const mv_value *attr,
               const mv_value *control) {
    char nb[72];
    const char *np;
    int64_t nl = mv_val_chars(fname, nb, sizeof nb, &np);
    char cb[8];
    const char *cp;
    int64_t cl = mv_val_chars(control, cb, sizeof cb, &cp);
    char ctl = cl > 0 ? cp[0] : 'X';
    char ab[64];
    const char *ap;
    int64_t al = mv_val_chars(attr, ab, sizeof ab, &ap);
    char abuf[64];
    if (al >= (int64_t)sizeof abuf) al = (int64_t)sizeof abuf - 1;
    memcpy(abuf, ap, (size_t)al);
    abuf[al] = '\0';
    trans_core(ctx, dst, np, nl, key, abuf, al, ctl, 0);
}

/* IEVAL(rec, ispec): evaluate an I-descriptor against a record — the runtime
   evaluator exposed to the verbs (and to programs).  "" for an unknown spec. */
void mvx_ieval(mvx_ctx *ctx, mv_value *dst, const mv_value *rec,
               const mv_value *spec) {
    char sb[256];
    const char *sp;
    int64_t sl = mv_val_chars(spec, sb, sizeof sb, &sp);
    ieval(ctx, dst, rec, sp, sl, 0);
}

/* Returns 0 on success, or -2 on a backend write failure when the caller
   has an ON ERROR handler (onerr != 0); without a handler a failure is
   fatal, as classic MV aborts to the debugger.  On error the record is
   not written and the lock is left as it was. */
int64_t mvx_write(mvx_ctx *ctx, const mv_value *rec, const mv_value *fvar,
                  const mv_value *id, int64_t keep_lock, int64_t onerr) {
    mvx_file *f = file_of(fvar, "WRITE");
    mvx_file_base *b = (mvx_file_base *)f;
    store_state *st = state(ctx);
    char ib[40];
    int64_t idlen;
    const char *ip = id_chars(id, ib, sizeof ib, &idlen);

    open_file *o = find_open(st, f);
    int ok;
    if (o) ix_load(o);
    /* Native mode: the projection is authoritative, so a value that does
       not fit its typed column rejects the write before it commits (the
       record is left untouched) — ON ERROR fires, else it is fatal.  Mirror
       mode never blocks a write; it stores NULL for the same value below. */
    if (o) {
        map_load(o);
        if (o->map.nf > 0 && o->map.native &&
            !map_validate_one(ctx, &o->map, rec)) {
            if (onerr) return -2;
            mvx_fatal("WRITE rejected by native map on %s id %.*s",
                      b->spec, (int)idlen, ip);
        }
    }
    /* Read the prior record once when either secondary indexes or a mapping
       need to diff against it, and share it between them. */
    mv_value old;
    int had_old = 0;
    int need_old = o && (o->ix.n > 0 || o->map.nf > 0);
    if (need_old) {
        mv_init(&old);
        had_old = b->driver->read(f, ip, idlen, &old);
    }
    if (o && o->ix.n > 0 && b->driver->write_ix) {
        mvx_ixop ops[IX_MAX_ITEMS * IX_MAX_VALS * 2];
        static ixvals pool[IX_MAX_ITEMS * 2];
        int nops = ix_diff(o, &old, had_old, rec, ops, pool);
        ok = b->driver->write_ix(f, ip, idlen, rec, ops, nops);
    } else {
        ok = b->driver->write(f, ip, idlen, rec);
    }
    if (!ok) {
        if (need_old) mv_clear(&old);
        if (onerr) return -2;
        mvx_fatal("WRITE failed on %s id %.*s", b->spec, (int)idlen, ip);
    }
    /* Keep a declared relational mapping (%MAP%) current.  On an update we
       diff against the prior record and write only the columns/child rows
       that changed; a new record projects in full.  Native mode already
       validated above; mirror mode is best-effort (mismatch -> NULL). */
    if (o && o->map.nf > 0)
        map_project(ctx, f, &o->map, ip, idlen, rec, had_old ? &old : NULL);
    if (need_old) mv_clear(&old);
    if (!keep_lock) {                   /* WRITE releases; WRITEU keeps */
        char *key = lock_key(f, ip, idlen);
        lock_drop(st, key);
        free(key);
    }
    return 0;
}

/* MATREAD arr FROM file, id: read the record and distribute its fields
   across the dimensioned array.  Returns found (drives THEN/ELSE); the
   array is left untouched when the record is absent. */
int64_t mvx_matread(mvx_ctx *ctx, mv_array *arr, const mv_value *fvar,
                    const mv_value *id, int64_t lock) {
    mv_value rec;
    mv_init(&rec);
    int64_t found = mvx_read(ctx, &rec, fvar, id, lock);
    if (found > 0) mv_matparse(arr, &rec);
    mv_clear(&rec);
    return found;
}

/* MATWRITE arr ON file, id: join the array into a record and write it. */
int64_t mvx_matwrite(mvx_ctx *ctx, const mv_array *arr, const mv_value *fvar,
                     const mv_value *id, int64_t keep_lock, int64_t onerr) {
    mv_value rec;
    mv_init(&rec);
    mv_matbuild(arr, &rec);
    int64_t st = mvx_write(ctx, &rec, fvar, id, keep_lock, onerr);
    mv_clear(&rec);
    return st;
}

/* MAPSPEC(file) — derive a mapping from a file's dictionary (#24): every D-item
   becomes one mapping field (its id is the name, DI<2> the attribute, DI<3> the
   conversion, DI<6> the association; the type is derived from the conversion via
   the shared mapper), assembled with the MAPFIELD builder and @AM-joined.  The
   result is the same %MAP%-format mapping JSONENCODE/JSONDECODE (and the SQL
   mapping) consume — one dict->spec derivation, reusable. */
void mvx_mapspec(mvx_ctx *ctx, mv_value *dst, const mv_value *fname) {
    mv_value dictv;
    mv_init(&dictv);
    mv_set_str(&dictv, "DICT", 4);
    mv_value fvar;
    mv_init(&fvar);
    int64_t ok = mvx_open(ctx, &dictv, fname, &fvar);
    mv_clear(&dictv);
    if (!ok) { mv_set_str(dst, "", 0); mv_clear(&fvar); return; }

    mvx_file *f = file_of(&fvar, "MAPSPEC");
    mvx_file_base *b = (mvx_file_base *)f;
    mvx_cursor *c = b->driver->select_begin(f);

    char *buf = NULL;
    size_t len = 0, cap = 0;
    mv_value id, rec, di[7], type, field;
    mv_init(&id); mv_init(&rec); mv_init(&type); mv_init(&field);
    for (int i = 0; i < 7; i++) mv_init(&di[i]);
    mv_set_str(&type, "", 0);
    while (c && b->driver->select_next(c, &id)) {
        char ib[128];
        const char *ip;
        int64_t il = mv_val_chars(&id, ib, sizeof ib, &ip);
        if (il == 0 || ip[0] == '%') continue;     /* skip control records */
        if (mvx_read(ctx, &rec, &fvar, &id, 0) <= 0) continue;
        mv_extract_fn(&di[1], &rec, 1, 0, 0);      /* DI<1> = item type */
        char t1[8];
        const char *t1p;
        int64_t t1l = mv_val_chars(&di[1], t1, sizeof t1, &t1p);
        if (t1l == 0 || t1p[0] != 'D') continue;   /* only D-items map */
        mv_extract_fn(&di[2], &rec, 2, 0, 0);      /* attr */
        char a2[24];
        const char *a2p;
        int64_t a2l = mv_val_chars(&di[2], a2, sizeof a2, &a2p);
        if (a2l == 0 || strtoll(a2p, NULL, 10) < 1) continue;  /* skip @ID/attr 0 */
        mv_extract_fn(&di[3], &rec, 3, 0, 0);      /* conversion */
        mv_extract_fn(&di[6], &rec, 6, 0, 0);      /* association */
        /* type left empty -> derived from the conversion by MAPFIELD */
        mvx_map_field(&field, &id, &di[2], &di[3], &type, &di[6]);
        char fb[512];
        const char *fp;
        int64_t fl = mv_val_chars(&field, fb, sizeof fb, &fp);
        size_t need = (size_t)fl + 1;
        if (len + need > cap) {
            cap = cap ? cap * 2 : 256;
            while (cap < len + need) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) mvx_fatal("out of memory in MAPSPEC");
            buf = nb;
        }
        if (len) buf[len++] = (char)0xFE;          /* @AM between fields */
        memcpy(buf + len, fp, (size_t)fl);
        len += (size_t)fl;
    }
    if (c) b->driver->select_end(c);
    mv_set_str(dst, buf ? buf : "", (int64_t)len);
    free(buf);
    mv_clear(&id); mv_clear(&rec); mv_clear(&type); mv_clear(&field);
    for (int i = 0; i < 7; i++) mv_clear(&di[i]);
    mv_clear(&fvar);
}

/* READV var FROM file, id, attr: read one attribute of a record.
   Returns found (drives THEN/ELSE); target untouched when absent. */
int64_t mvx_readv(mvx_ctx *ctx, mv_value *dst, const mv_value *fvar,
                  const mv_value *id, int64_t attr, int64_t lock) {
    mv_value rec;
    mv_init(&rec);
    int64_t found = mvx_read(ctx, &rec, fvar, id, lock);
    if (found > 0) mv_extract_fn(dst, &rec, attr, 0, 0);
    mv_clear(&rec);
    return found;
}

/* WRITEV expr ON file, id, attr: replace one attribute, preserving the
   rest of the record (creating it if absent). */
int64_t mvx_writev(mvx_ctx *ctx, const mv_value *val, const mv_value *fvar,
                   const mv_value *id, int64_t attr, int64_t keep_lock,
                   int64_t onerr) {
    mv_value rec;
    mv_init(&rec);
    if (!mvx_read(ctx, &rec, fvar, id, 0)) mv_set_str(&rec, "", 0);
    mv_replace_fn(&rec, &rec, attr, 0, 0, val);
    int64_t st = mvx_write(ctx, &rec, fvar, id, keep_lock, onerr);
    mv_clear(&rec);
    return st;
}

int64_t mvx_delete_rec(mvx_ctx *ctx, const mv_value *fvar,
                       const mv_value *id) {
    mvx_file *f = file_of(fvar, "DELETE");
    mvx_file_base *b = (mvx_file_base *)f;
    store_state *st = state(ctx);
    char ib[40];
    int64_t idlen;
    const char *ip = id_chars(id, ib, sizeof ib, &idlen);

    open_file *o = find_open(st, f);
    int64_t r;
    if (o) ix_load(o);
    if (o && o->ix.n > 0 && b->driver->del_ix) {
        mv_value old;
        mv_init(&old);
        int had = b->driver->read(f, ip, idlen, &old);
        if (had) {
            mvx_ixop ops[IX_MAX_ITEMS * IX_MAX_VALS * 2];
            static ixvals pool[IX_MAX_ITEMS * 2];
            int nops = ix_diff(o, &old, 1, NULL, ops, pool);
            r = b->driver->del_ix(f, ip, idlen, ops, nops);
        } else {
            r = 0;
        }
        mv_clear(&old);
    } else {
        r = b->driver->del(f, ip, idlen);
    }
    char *key = lock_key(f, ip, idlen);
    lock_drop(st, key);
    free(key);
    return r;
}

/* ------------------------------------------- index verbs' entry points */

int64_t mvx_index_build(mvx_ctx *ctx, const mv_value *fvar,
                        const mv_value *item) {
    mvx_file *f = file_of(fvar, "INDEXBUILD");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->write_ix && !b->driver->index_create) return -1;
    open_file *o = find_open(state(ctx), f);
    if (!o) return -1;

    o->ix.loaded = 0;                   /* pick up the new %INDEXES% */
    ix_load(o);

    char nb[40];
    const char *ip;
    int64_t il = mv_val_chars(item, nb, sizeof nb, &ip);
    char iname[128];
    if (il <= 0 || il >= 127) return -1;
    memcpy(iname, ip, (size_t)il);
    iname[il] = '\0';
    int slot = -1;
    for (int k = 0; k < o->ix.n; k++)
        if (strcmp(o->ix.it[k].item, iname) == 0) slot = k;
    if (slot < 0) return -1;

    /* Backend-maintained index (SQL CREATE INDEX): one shot, no per-record
       backfill.  Index a mapped identity column directly, else index the raw
       attribute via an expression on the blob — so any field is indexable and
       the mapped-column-preferred rule still holds. */
    if (b->driver->index_create)
        return b->driver->index_create(f, iname,
                                       map_identity_col(o, o->ix.it[slot].attr),
                                       o->ix.it[slot].attr);

    b->driver->index_drop(f, iname);    /* rebuild from empty */

    mvx_cursor *c = b->driver->select_begin(f);
    if (!c) return -1;
    int64_t count = 0;
    mv_value rid, rec;
    mv_init(&rid); mv_init(&rec);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        if (!b->driver->read(f, rp, rl, &rec)) continue;
        ixvals vs;
        ix_values(&rec, o->ix.it[slot].attr, &vs);
        mvx_ixop ops[IX_MAX_VALS];
        for (int i = 0; i < vs.n; i++) {
            ops[i].item = iname;
            ops[i].key = vs.v[i];
            ops[i].klen = vs.len[i];
            ops[i].add = 1;
        }
        if (!b->driver->write_ix(f, rp, rl, &rec, ops, vs.n)) {
            b->driver->select_end(c);
            mv_clear(&rid); mv_clear(&rec);
            return -1;
        }
        count++;
    }
    b->driver->select_end(c);
    mv_clear(&rid); mv_clear(&rec);
    return count;
}

/* --- relational mapping (see MAP / #18) -------------------------------
   Build a mapped file's projection.  spec is an @AM list, one field per
   element as "name <VM> attr# <VM> conv <VM> type <VM> assoc" (empty
   assoc = a single-valued parent column; otherwise a member of that
   association's child table).  The runtime decomposes each record; the
   driver materialises columns / child tables and persists.  Returns the
   record count, -1 on error, or -2 when the backend has no mapping. */

/* Validate a record against a mapping without touching the backend: 1 if
   every typed cell fits its column, 0 if any non-empty value mismatches.
   Native mode calls this to reject a bad WRITE before it commits. */
static int map_validate_one(mvx_ctx *ctx, mapmeta *m, const mv_value *rec) {
    mv_value av, ov, code;
    mv_init(&av); mv_init(&ov); mv_init(&code);
    char cell[256];
    int ok = 1;
    for (int i = 0; i < m->nf && ok; i++) {
        int nv = m->assocs[i][0] ? map_vcount(rec, m->anos[i], &av) : 0;
        for (int seq = 0; seq <= nv; seq++) {
            if (m->assocs[i][0] && seq == 0) continue;   /* MV: 1..nv only */
            if (map_cell(ctx, rec, m->anos[i], seq, m->convs[i], m->types[i],
                         &av, &ov, &code, cell, sizeof cell) < 0) {
                ok = 0;
                break;
            }
        }
    }
    mv_clear(&av); mv_clear(&ov); mv_clear(&code);
    return ok;
}

/* Group m's fields into parent columns + per-association members and
   materialise the schema via the driver.  Returns 1/0. */
static int map_ensure_schema(mvx_file *f, mapmeta *m, char *err,
                             size_t errlen) {
    mvx_file_base *b = (mvx_file_base *)f;
    mvx_mapfield pcol[MAP_MAXF];
    int npar = 0;
    for (int i = 0; i < m->nf; i++)
        if (m->assocs[i][0] == '\0') {
            pcol[npar].name = m->names[i];
            pcol[npar].type = m->types[i];
            npar++;
        }
    if (npar > 0 && !b->driver->map_ensure(f, pcol, npar, err, errlen))
        return 0;
    char *an[MAP_MAXA];
    int am[MAP_MAXA][MAP_MAXF], anm[MAP_MAXA], na = 0;
    for (int i = 0; i < m->nf; i++) {
        if (m->assocs[i][0] == '\0') continue;
        int slot = -1;
        for (int a = 0; a < na; a++)
            if (strcmp(an[a], m->assocs[i]) == 0) slot = a;
        if (slot < 0 && na < MAP_MAXA) {
            slot = na; an[na] = m->assocs[i]; anm[na++] = 0;
        }
        if (slot >= 0 && anm[slot] < MAP_MAXF) am[slot][anm[slot]++] = i;
    }
    if (na > 0 &&
        (!b->driver->map_child_ensure || !b->driver->map_child_apply)) {
        snprintf(err, errlen, "backend has no association child tables");
        return 0;
    }
    for (int a = 0; a < na; a++) {
        mvx_mapfield cc[MAP_MAXF];
        for (int k = 0; k < anm[a]; k++) {
            int i = am[a][k];
            cc[k].name = m->names[i];
            cc[k].type = m->types[i];
        }
        if (!b->driver->map_child_ensure(f, an[a], cc, anm[a], err, errlen))
            return 0;
    }
    return 1;
}

/* Project one association's rows (members am, count nm) for a record and
   replace its child-table rows via the driver.  Returns 1/0. */
static int map_child_project(mvx_ctx *ctx, mvx_file *f, mapmeta *m,
                             const char *id, int64_t idlen,
                             const mv_value *rec, const int *am, int nm,
                             const char *aname, mv_value *av, mv_value *ov,
                             mv_value *code) {
    mvx_file_base *b = (mvx_file_base *)f;
    mvx_mapfield cc[MAP_MAXF];
    for (int k = 0; k < nm; k++) {
        cc[k].name = m->names[am[k]];
        cc[k].type = m->types[am[k]];
    }
    int nv = 0;
    for (int k = 0; k < nm; k++) {
        int vc = map_vcount(rec, m->anos[am[k]], av);
        if (vc > nv) nv = vc;
    }
    if (nv == 0)
        return b->driver->map_child_apply(f, id, idlen, aname, cc, nm,
                                          NULL, NULL, 0);
    size_t ncell = (size_t)nv * (size_t)nm;
    const char **cv = malloc(ncell * sizeof *cv);
    int64_t *cl = malloc(ncell * sizeof *cl);
    char *arena = malloc(ncell * 256);
    if (!cv || !cl || !arena) { free(cv); free(cl); free(arena); return 0; }
    for (int seq = 1; seq <= nv; seq++)
        for (int k = 0; k < nm; k++) {
            size_t cell = (size_t)(seq - 1) * nm + k;
            char *dst = arena + cell * 256;
            cl[cell] = map_cell(ctx, rec, m->anos[am[k]], seq, m->convs[am[k]],
                                m->types[am[k]], av, ov, code, dst, 256);
            if (cl[cell] < 0) { cl[cell] = 0; dst[0] = '\0'; }
            cv[cell] = dst;
        }
    int ok = b->driver->map_child_apply(f, id, idlen, aname, cc, nm, cv, cl, nv);
    free(cv); free(cl); free(arena);
    return ok;
}

/* Project a record into the mapped columns / child tables.  When `old` is
   non-NULL (an update whose prior record is known) only the attributes that
   actually changed are written: a Pick WRITE hands us the whole record, but
   SQL is columns and rows, so re-emitting every column and re-DELETE/INSERTing
   every child row on each write is wasteful — we diff against `old` and touch
   only what moved.  With `old` NULL (a new record) the full projection runs. */
static int map_project(mvx_ctx *ctx, mvx_file *f, mapmeta *m, const char *id,
                       int64_t idlen, const mv_value *rec,
                       const mv_value *old) {
    mvx_file_base *b = (mvx_file_base *)f;
    mv_value av, ov, code, ta, tb;
    mv_init(&av); mv_init(&ov); mv_init(&code); mv_init(&ta); mv_init(&tb);
    int ok = 1;

    /* parent columns — only the changed ones (all, when there is no old) */
    static char ps[MAP_MAXF][256];
    mvx_mapfield pcol[MAP_MAXF];
    const char *vals[MAP_MAXF];
    int64_t vlens[MAP_MAXF];
    int nchg = 0;
    for (int i = 0; i < m->nf; i++) {
        if (m->assocs[i][0] != '\0') continue;
        if (old && map_attr_equal(old, rec, m->anos[i], &ta, &tb)) continue;
        int64_t vl = map_cell(ctx, rec, m->anos[i], 0, m->convs[i],
                              m->types[i], &av, &ov, &code, ps[nchg],
                              sizeof ps[0]);
        if (vl < 0) { vl = 0; ps[nchg][0] = '\0'; }
        pcol[nchg].name = m->names[i];
        pcol[nchg].type = m->types[i];
        vals[nchg] = ps[nchg];
        vlens[nchg] = vl;
        nchg++;
    }
    if (nchg > 0 && !b->driver->map_apply(f, id, idlen, pcol, vals, vlens, nchg))
        ok = 0;

    /* association child tables — skip any association left untouched */
    if (ok && b->driver->map_child_apply) {
        char *an[MAP_MAXA];
        int am[MAP_MAXA][MAP_MAXF], anm[MAP_MAXA];
        int na = map_group_assoc(m, an, am, anm);
        for (int a = 0; a < na && ok; a++) {
            if (old) {
                int changed = 0;
                for (int k = 0; k < anm[a] && !changed; k++)
                    if (!map_attr_equal(old, rec, m->anos[am[a][k]], &ta, &tb))
                        changed = 1;
                if (!changed) continue;
            }
            if (!map_child_project(ctx, f, m, id, idlen, rec, am[a], anm[a],
                                   an[a], &av, &ov, &code))
                ok = 0;
        }
    }
    mv_clear(&av); mv_clear(&ov); mv_clear(&code);
    mv_clear(&ta); mv_clear(&tb);
    return ok;
}

/* Full projection of a record (new record, or a rebuild). */
static int map_project_one(mvx_ctx *ctx, mvx_file *f, mapmeta *m,
                           const char *id, int64_t idlen,
                           const mv_value *rec) {
    return map_project(ctx, f, m, id, idlen, rec, NULL);
}

/* Native read: overlay the mapped attributes of `rec` with the current
   values held in the backend's relational form, so writes made to the SQL
   columns / child tables (by MVX or an external tool) are what the program
   reads.  The rec blob is the base — it carries the un-mapped attributes —
   and only the mapped attributes are replaced.  Returns 1 if the parent row
   exists (record present in SQL), 0 if absent, -1 on error; on 0/-1 rec is
   left as the caller's base. */
static int map_recompose(mvx_ctx *ctx, mvx_file *f, mapmeta *m,
                         const char *id, int64_t idlen, mv_value *rec) {
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->map_read) return 0;
    mv_value val, tmp, code;
    mv_init(&val); mv_init(&tmp); mv_init(&code);

    mvx_mapfield pcol[MAP_MAXF];
    int pidx[MAP_MAXF], npar = 0;
    for (int i = 0; i < m->nf; i++)
        if (m->assocs[i][0] == '\0') {
            pcol[npar].name = m->names[i];
            pcol[npar].type = m->types[i];
            pidx[npar++] = i;
        }
    char *vals[MAP_MAXF];
    int64_t lens[MAP_MAXF];
    for (int k = 0; k < npar; k++) { vals[k] = NULL; lens[k] = 0; }
    int present = b->driver->map_read(f, id, idlen, pcol, npar, vals, lens);
    if (present <= 0) {
        for (int k = 0; k < npar; k++) free(vals[k]);
        mv_clear(&val); mv_clear(&tmp); mv_clear(&code);
        return present;
    }
    for (int k = 0; k < npar; k++) {
        int i = pidx[k];
        map_uncell(ctx, m->types[i], m->convs[i], vals[k], lens[k], &val,
                   &tmp, &code);
        mv_replace_fn(rec, rec, m->anos[i], 0, 0, &val);
        free(vals[k]);
    }

    if (b->driver->map_child_read) {
        char *an[MAP_MAXA];
        int am[MAP_MAXA][MAP_MAXF], anm[MAP_MAXA], na = 0;
        for (int i = 0; i < m->nf; i++) {
            if (m->assocs[i][0] == '\0') continue;
            int slot = -1;
            for (int a = 0; a < na; a++)
                if (strcmp(an[a], m->assocs[i]) == 0) slot = a;
            if (slot < 0 && na < MAP_MAXA) {
                slot = na; an[na] = m->assocs[i]; anm[na++] = 0;
            }
            if (slot >= 0 && anm[slot] < MAP_MAXF) am[slot][anm[slot]++] = i;
        }
        for (int a = 0; a < na; a++) {
            int nm = anm[a];
            mvx_mapfield cc[MAP_MAXF];
            for (int k = 0; k < nm; k++) {
                int i = am[a][k];
                cc[k].name = m->names[i];
                cc[k].type = m->types[i];
            }
            char **cells = NULL;
            int64_t *clens = NULL;
            int nrows = 0;
            int rc = b->driver->map_child_read(f, id, idlen, an[a], cc, nm,
                                               &cells, &clens, &nrows);
            if (rc == 1) {           /* SQL is authoritative: rebuild attrs */
                mv_set_str(&val, "", 0);
                for (int k = 0; k < nm; k++)
                    mv_replace_fn(rec, rec, m->anos[am[a][k]], 0, 0, &val);
                for (int r = 0; r < nrows; r++)
                    for (int k = 0; k < nm; k++) {
                        int i = am[a][k];
                        size_t cell = (size_t)r * (size_t)nm + (size_t)k;
                        map_uncell(ctx, m->types[i], m->convs[i], cells[cell],
                                   clens[cell], &val, &tmp, &code);
                        mv_replace_fn(rec, rec, m->anos[i], r + 1, 0, &val);
                    }
            }
            if (cells)
                for (size_t x = 0; x < (size_t)nrows * (size_t)nm; x++)
                    free(cells[x]);
            free(cells);
            free(clens);
        }
    }
    mv_clear(&val); mv_clear(&tmp); mv_clear(&code);
    return 1;
}

/* Load a file's declared mapping (%MAP% in its dictionary) into o->map,
   once per open, and materialise its schema.  With no %MAP% the file is
   simply not mapped. */
static void map_load(open_file *o) {
    if (o->map.loaded) return;
    o->map.loaded = 1;
    o->map.nf = 0;
    mvx_file_base *b = (mvx_file_base *)o->f;
    if (!b->driver->map_ensure || !b->driver->map_apply) return;
    char dspec[1720];
    const char *nl = strchr(b->spec, '\n');
    if (nl)
        snprintf(dspec, sizeof dspec, "%.*s\nDICT.%s",
                 (int)(nl - b->spec), b->spec, nl + 1);
    else
        snprintf(dspec, sizeof dspec, "DICT.%s", b->spec);
    char err[256] = "";
    mvx_file *d = b->driver->open(dspec, err, sizeof err);
    if (!d) return;
    mv_value mp;
    mv_init(&mp);
    if (b->driver->read(d, "%MAP%", 5, &mp)) {
        char nb[40];
        const char *sp;
        int64_t sl = mv_val_chars(&mp, nb, sizeof nb, &sp);
        if (sl > 0) {
            map_parse(sp, sl, &o->map);
            if (o->map.nf > 0 &&
                map_ensure_schema(o->f, &o->map, err, sizeof err))
                o->map.ensured = 1;
        }
    }
    mv_clear(&mp);
    /* %MAPMODE% (absent = mirror) selects the write policy. */
    mv_value mm;
    mv_init(&mm);
    if (o->map.nf > 0 && b->driver->read(d, "%MAPMODE%", 9, &mm)) {
        char mb[16];
        const char *mpp;
        int64_t ml = mv_val_chars(&mm, mb, sizeof mb, &mpp);
        if (ml == 6 && strncmp(mpp, "native", 6) == 0) o->map.native = 1;
    }
    mv_clear(&mm);
    b->driver->close(d);
}

/* Backfill progress line (#28): a carriage-return-updated stderr indicator —
   records done, percent when the total is known, and the rate.  Throttled by the
   caller (every N records + once at the end); silent unless PROGRESS is asked. */
static void mapbuild_progress(int64_t count, int64_t total,
                              const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double el = (double)(now.tv_sec - t0->tv_sec) +
                (double)(now.tv_nsec - t0->tv_nsec) / 1e9;
    long rate = el > 0.0 ? (long)((double)count / el) : 0;
    if (total > 0)
        fprintf(stderr, "\rBUILD-MAP: %lld/%lld (%d%%) %ld rec/s   ",
                (long long)count, (long long)total,
                (int)((count * 100) / total), rate);
    else
        fprintf(stderr, "\rBUILD-MAP: %lld records %ld rec/s   ",
                (long long)count, rate);
    fflush(stderr);
}

int64_t mvx_mapbuild(mvx_ctx *ctx, const mv_value *fvar,
                     const mv_value *spec, int64_t progress) {
    mvx_file *f = file_of(fvar, "MAPBUILD");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->map_ensure || !b->driver->map_apply) return -2;

    char nb[40];
    const char *sp;
    int64_t slen = mv_val_chars(spec, nb, sizeof nb, &sp);
    mapmeta m;
    memset(&m, 0, sizeof m);
    map_parse(sp, slen, &m);
    if (m.nf == 0) { free(m.buf); return 0; }

    char err[256] = "";
    if (!map_ensure_schema(f, &m, err, sizeof err)) {
        if (err[0]) fprintf(stderr, "MAPBUILD: %s\n", err);
        free(m.buf);
        return -1;
    }

    /* Push the whole backfill into the backend when the transform is
       expressible there (e.g. one UPDATE over all rows) — no records over the
       wire.  MVX_MAP_NOPUSH means "not expressible", so fall through to the
       per-record loop below. */
    if (b->driver->map_backfill) {
        mvx_mapfield cols[MAP_MAXF];
        const char *convs[MAP_MAXF], *assocs[MAP_MAXF];
        for (int i = 0; i < m.nf; i++) {
            cols[i].name = m.names[i];
            cols[i].type = m.types[i];
            convs[i] = m.convs[i];
            assocs[i] = m.assocs[i];
        }
        char berr[256] = "";
        int64_t bn = b->driver->map_backfill(f, cols, m.anos, convs, assocs,
                                             m.nf, berr, sizeof berr);
        if (bn != MVX_MAP_NOPUSH) {
            if (bn < 0 && berr[0]) fprintf(stderr, "MAPBUILD: %s\n", berr);
            free(m.buf);
            return bn;
        }
    }

    mvx_cursor *c = b->driver->select_begin(f);
    if (!c) { free(m.buf); return -1; }
    int64_t total = (progress && b->driver->select_count)
                        ? b->driver->select_count(c) : -1;
    struct timespec t0;
    if (progress) clock_gettime(CLOCK_MONOTONIC, &t0);
    /* Batch the per-record writes in one transaction, committing every 10k rows,
       so a large backfill pays a handful of commits instead of one per record. */
    int bulk = b->driver->bulk_begin && b->driver->bulk_commit;
    if (bulk) b->driver->bulk_begin(f);
    int64_t count = 0, rc = 0;
    mv_value rid, rec;
    mv_init(&rid); mv_init(&rec);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        if (!b->driver->read(f, rp, rl, &rec)) continue;
        if (!map_project_one(ctx, f, &m, rp, rl, &rec)) { rc = -1; break; }
        count++;
        if (bulk && count % 10000 == 0) {
            b->driver->bulk_commit(f);
            b->driver->bulk_begin(f);
        }
        if (progress && count % 1000 == 0) mapbuild_progress(count, total, &t0);
    }
    if (bulk) b->driver->bulk_commit(f);   /* commits, or rolls back a failed txn */
    b->driver->select_end(c);
    if (progress) { mapbuild_progress(count, total, &t0); fputc('\n', stderr); }
    mv_clear(&rid); mv_clear(&rec);
    free(m.buf);
    return rc < 0 ? rc : count;
}

/* Count records that would fail native (strict) validation against spec,
   without writing anything — the switch-to-native safety check.  Returns
   the violation count (0 = every record fits), or -2 if unsupported. */
int64_t mvx_mapcheck(mvx_ctx *ctx, const mv_value *fvar,
                     const mv_value *spec) {
    mvx_file *f = file_of(fvar, "MAPCHECK");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->select_begin) return -2;
    char nb[40];
    const char *sp;
    int64_t slen = mv_val_chars(spec, nb, sizeof nb, &sp);
    mapmeta m;
    memset(&m, 0, sizeof m);
    map_parse(sp, slen, &m);
    if (m.nf == 0) { free(m.buf); return 0; }

    mvx_cursor *c = b->driver->select_begin(f);
    if (!c) { free(m.buf); return -2; }
    int64_t bad = 0;
    mv_value rid, rec;
    mv_init(&rid); mv_init(&rec);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        if (!b->driver->read(f, rp, rl, &rec)) continue;
        if (!map_validate_one(ctx, &m, &rec)) bad++;
    }
    b->driver->select_end(c);
    mv_clear(&rid); mv_clear(&rec);
    free(m.buf);
    return bad;
}

/* Tear down a file's mapping (drop its columns + child tables).  spec is
   the same %MAP% string.  Returns 1, -1 on error, or -2 if unsupported. */
int64_t mvx_mapdrop(mvx_ctx *ctx, const mv_value *fvar,
                    const mv_value *spec) {
    (void)ctx;
    mvx_file *f = file_of(fvar, "MAPDROP");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->map_drop) return -2;
    char nb[40];
    const char *sp;
    int64_t slen = mv_val_chars(spec, nb, sizeof nb, &sp);
    mapmeta m;
    memset(&m, 0, sizeof m);
    map_parse(sp, slen, &m);
    if (m.nf == 0) { free(m.buf); return 0; }
    mvx_mapfield pcol[MAP_MAXF];
    int npar = 0;
    for (int i = 0; i < m.nf; i++)
        if (m.assocs[i][0] == '\0') {
            pcol[npar].name = m.names[i];
            pcol[npar].type = m.types[i];
            npar++;
        }
    char *an[MAP_MAXA];
    int na = 0;
    for (int i = 0; i < m.nf; i++) {
        if (m.assocs[i][0] == '\0') continue;
        int seen = 0;
        for (int a = 0; a < na; a++)
            if (strcmp(an[a], m.assocs[i]) == 0) seen = 1;
        if (!seen && na < MAP_MAXA) an[na++] = m.assocs[i];
    }
    char err[256] = "";
    int ok = b->driver->map_drop(f, pcol, npar, (const char **)an, na, err,
                                 sizeof err);
    if (!ok && err[0]) fprintf(stderr, "MAPDROP: %s\n", err);
    free(m.buf);
    return ok ? 1 : -1;
}

int64_t mvx_index_drop(mvx_ctx *ctx, const mv_value *fvar,
                       const mv_value *item) {
    mvx_file *f = file_of(fvar, "INDEXDROP");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->index_drop) return 0;
    char nb[40];
    const char *ip;
    int64_t il = mv_val_chars(item, nb, sizeof nb, &ip);
    char iname[128];
    if (il <= 0 || il >= 127) return 0;
    memcpy(iname, ip, (size_t)il);
    iname[il] = '\0';
    int64_t r = b->driver->index_drop(f, iname);
    open_file *o = find_open(state(ctx), f);
    if (o) o->ix.loaded = 0;            /* metadata changed */
    return r;
}

int64_t mvx_index_select(mvx_ctx *ctx, const mv_value *fvar,
                         const mv_value *item, const mv_value *key) {
    mvx_file *f = file_of(fvar, "INDEXSELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->index_select) return 0;
    open_file *o = find_open(state(ctx), f);
    if (!o) return 0;
    ix_load(o);

    char nb[40];
    const char *ip;
    int64_t il = mv_val_chars(item, nb, sizeof nb, &ip);
    char iname[128];
    if (il <= 0 || il >= 127) return 0;
    memcpy(iname, ip, (size_t)il);
    iname[il] = '\0';
    int found = 0;
    for (int k = 0; k < o->ix.n; k++)
        if (strcmp(o->ix.it[k].item, iname) == 0) found = 1;
    if (!found) return 0;               /* not a registered index */

    /* A backend-maintained index (no write_ix) is over the mapped *column*,
       which holds the projected value.  The WITH filter compares the raw
       attribute, so the index result equals the scan result only when the
       projection is the identity — a TEXT column with no conversion.  For
       any converted column, fall back to the scan (return 0) rather than
       risk a wrong (sub/superset) result. */
    if (!b->driver->write_ix) {
        map_load(o);
        int ok = 0;
        for (int i = 0; i < o->map.nf; i++)
            if (strcmp(o->map.names[i], iname) == 0) {
                ok = strcmp(o->map.types[i], "TEXT") == 0 &&
                     o->map.convs[i][0] == '\0';
                break;
            }
        if (!ok) return 0;
    }

    char kb[40];
    const char *kp;
    int64_t kl = mv_val_chars(key, kb, sizeof kb, &kp);
    mvx_cursor *c = b->driver->index_select(f, iname, kp, kl);
    if (!c) return 0;

    store_state *st = state(ctx);
    clear_select(st);
    st->sel_active = 1;
    int64_t cap = 0;
    mv_value rid;
    mv_init(&rid);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        sel_push(st, &cap, rp, rl);
    }
    mv_clear(&rid);
    b->driver->select_end(c);
    return 1;
}

/* Push a WITH filter into the backend: if `item` is a mapped identity column
   (its column value equals the raw attribute, so the pushed-down result
   equals the scan result) and `op`/`value` are pushable, form the select
   list from the backend-filtered ids and return 1.  Otherwise 0 — the verb
   scans as before.  This keeps a large SQL-backed query from streaming every
   record to the verb just to discard most of them. */
int64_t mvx_query_select(mvx_ctx *ctx, const mv_value *fvar,
                         const mv_value *item, const mv_value *op,
                         const mv_value *value, const mv_value *attr) {
    mvx_file *f = file_of(fvar, "QUERYSELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->select_where && !b->driver->select_attr) return 0;
    open_file *o = find_open(state(ctx), f);
    if (!o) return 0;

    char ob[8];
    const char *opp;
    int64_t ol = mv_val_chars(op, ob, sizeof ob, &opp);
    int iseq = ol == 1 && (opp[0] == '=' || opp[0] == '#');
    int isrange = (ol == 1 && (opp[0] == '>' || opp[0] == '<')) ||
                  (ol == 2 && (opp[0] == '>' || opp[0] == '<') && opp[1] == '=');
    if (!iseq && !isrange) return 0;
    char vb[40];
    const char *vp;
    int64_t vl = mv_val_chars(value, vb, sizeof vb, &vp);
    if (vl == 0) return 0;              /* empty value: NULL semantics differ */
    char opz[3];
    memcpy(opz, opp, (size_t)ol);
    opz[ol] = '\0';

    char nbuf[40];
    const char *ip;
    int64_t il = mv_val_chars(item, nbuf, sizeof nbuf, &ip);
    char iname[128];
    if (il > 0 && il < 127) { memcpy(iname, ip, (size_t)il); iname[il] = '\0'; }
    else iname[0] = '\0';

    /* Classify the field from the mapping: an identity projection (TEXT, no
       conversion) whose column equals the raw attribute, and/or numeric
       (NUMERIC/DATE/TIME) whose raw internal value compares numerically. */
    map_load(o);
    int identity = 0, numeric = 0;
    for (int i = 0; iname[0] && i < o->map.nf; i++)
        if (strcmp(o->map.names[i], iname) == 0) {
            identity = strcmp(o->map.types[i], "TEXT") == 0 &&
                       o->map.convs[i][0] == '\0';
            numeric = strcmp(o->map.types[i], "NUMERIC") == 0 ||
                      strcmp(o->map.types[i], "DATE") == 0 ||
                      strcmp(o->map.types[i], "TIME") == 0;
            break;
        }

    /* A range (>, <, ...) is only exact on a numeric field — MV compares its
       raw internal value numerically, which matches the backend numeric
       compare; text range is byte-vs-numeric ambiguous, so it scans. */
    mvx_cursor *c = NULL;
    if (isrange) {
        if (!numeric || !b->driver->select_attr) return 0;
        c = b->driver->select_attr(f, mv_get_int(attr), opz, vp, vl);
    } else if (identity && b->driver->select_where) {
        c = b->driver->select_where(f, iname, opz, vp, vl);
    } else if (b->driver->select_attr) {
        c = b->driver->select_attr(f, mv_get_int(attr), opz, vp, vl);
    }
    if (!c) return 0;

    store_state *st = state(ctx);
    clear_select(st);
    st->sel_active = 1;
    int64_t cap = 0;
    mv_value rid;
    mv_init(&rid);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        sel_push(st, &cap, rp, rl);
    }
    mv_clear(&rid);
    b->driver->select_end(c);
    return 1;
}

/* Push a multi-condition WITH (AND of predicates) into the backend as one
   WHERE.  `spec` is the conditions, @AM-separated, each "attr<VM>op<VM>value".
   Each is resolved with the same rules as the single-condition push-down
   (identity column / numeric-range-on-blob / raw blob); if any one is not
   pushable, returns 0 (all-or-nothing) so the verb filters them all. */
int64_t mvx_multiselect(mvx_ctx *ctx, const mv_value *fvar,
                        const mv_value *spec) {
    mvx_file *f = file_of(fvar, "MULTISELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->select_multi) return 0;
    open_file *o = find_open(state(ctx), f);
    if (!o) return 0;
    map_load(o);

    char sb[64];
    const char *sp;
    int64_t sl = mv_val_chars(spec, sb, sizeof sb, &sp);
    if (sl <= 0 || sl >= 2048) return 0;
    char buf[2048];
    memcpy(buf, sp, (size_t)sl);
    buf[sl] = '\0';

    mvx_pred preds[32];
    int np = 0;
    char *p = buf;
    while (p && *p && np < 32) {
        char *amend = p;
        while (*amend && *amend != (char)0xFE) amend++;
        char amsave = *amend;
        *amend = '\0';
        /* split condition on VM into attr / op / value */
        char *parts[3] = {p, NULL, NULL};
        int nv = 0;
        for (char *q = p; *q; q++)
            if (*q == (char)0xFD) { *q = '\0'; if (++nv < 3) parts[nv] = q + 1; }
        if (!parts[1] || !parts[2]) return 0;
        int64_t attrno = strtoll(parts[0], NULL, 10);
        const char *op = parts[1];
        const char *val = parts[2];
        if (attrno < 1 || val[0] == '\0') return 0;
        int range = (op[0] == '>' || op[0] == '<') &&
                    (op[1] == '\0' || (op[1] == '=' && op[2] == '\0'));
        int iseq = (op[0] == '=' || op[0] == '#') && op[1] == '\0';
        if (!range && !iseq) return 0;
        int identity = 0, numeric = 0;
        const char *col = NULL;
        for (int i = 0; i < o->map.nf; i++)
            if (o->map.anos[i] == attrno && o->map.assocs[i][0] == '\0') {
                identity = strcmp(o->map.types[i], "TEXT") == 0 &&
                           o->map.convs[i][0] == '\0';
                numeric = strcmp(o->map.types[i], "NUMERIC") == 0 ||
                          strcmp(o->map.types[i], "DATE") == 0 ||
                          strcmp(o->map.types[i], "TIME") == 0;
                if (identity) col = o->map.names[i];
                break;
            }
        if (range && !numeric) return 0;  /* text range: not pushable */
        preds[np].col = range ? NULL : col;   /* range uses the blob */
        preds[np].attr = attrno;
        preds[np].op = op;
        preds[np].numeric = range ? 1 : 0;
        preds[np].val = val;
        preds[np].vlen = (int64_t)strlen(val);
        np++;
        if (amsave == 0) break;
        p = amend + 1;
    }
    if (np == 0) return 0;

    mvx_cursor *c = b->driver->select_multi(f, preds, np);
    if (!c) return 0;
    store_state *st = state(ctx);
    clear_select(st);
    st->sel_active = 1;
    int64_t cap = 0;
    mv_value rid;
    mv_init(&rid);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        sel_push(st, &cap, rp, rl);
    }
    mv_clear(&rid);
    b->driver->select_end(c);
    return 1;
}

/* The name of the mapped identity column (TEXT, no conversion) for a file's
   attribute `attr`, or NULL if it isn't mapped that way — so a JOIN can use
   a real, indexable column instead of a split_part on the record blob. */
static const char *map_identity_col(open_file *o, int64_t attr) {
    if (!o) return NULL;
    map_load(o);
    for (int i = 0; i < o->map.nf; i++)
        if (o->map.anos[i] == attr && o->map.assocs[i][0] == '\0' &&
            strcmp(o->map.types[i], "TEXT") == 0 && o->map.convs[i][0] == '\0')
            return o->map.names[i];
    return NULL;
}

/* The mapped column to ORDER BY for attribute `attr`, when its type matches
   the sort order (`ordnum`: numeric/date/time for a right-justified BY, or
   identity text for a left-justified BY — so the SQL order equals MV's), or
   NULL.  *otext is set when the column needs COLLATE "C" (byte order). */
static const char *map_order_col(open_file *o, int64_t attr, int ordnum,
                                 int *otext) {
    if (!o) return NULL;
    map_load(o);
    for (int i = 0; i < o->map.nf; i++)
        if (o->map.anos[i] == attr && o->map.assocs[i][0] == '\0') {
            const char *t = o->map.types[i];
            int isnum = strcmp(t, "NUMERIC") == 0 || strcmp(t, "DATE") == 0 ||
                        strcmp(t, "TIME") == 0;
            int istext = strcmp(t, "TEXT") == 0 && o->map.convs[i][0] == '\0';
            if (ordnum && isnum) { *otext = 0; return o->map.names[i]; }
            if (!ordnum && istext) { *otext = 1; return o->map.names[i]; }
            return NULL;
        }
    return NULL;
}

/* Push a WITH filter on a TRANS() I-type down to a co-located JOIN: parse the
   TRANS(file,keyattr,attr,control) descriptor, and if the target file is on
   the same backend as the source, form the select list from the joined ids
   server-side (only "=", non-empty value, control X — the INNER JOIN case
   that exactly matches the per-record reference).  Returns 1 if pushed, else
   0 so the verb evaluates TRANS per record. */
int64_t mvx_transselect(mvx_ctx *ctx, const mv_value *fvar,
                        const mv_value *spec, const mv_value *op,
                        const mv_value *value) {
    mvx_file *f = file_of(fvar, "TRANSSELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->select_join) return 0;

    char ob[8];
    const char *opp;
    int64_t ol = mv_val_chars(op, ob, sizeof ob, &opp);
    if (!(ol == 1 && opp[0] == '=')) return 0;
    char vb[40];
    const char *vp;
    int64_t vl = mv_val_chars(value, vb, sizeof vb, &vp);
    if (vl == 0) return 0;

    char sb[256];
    const char *sp;
    int64_t sl = mv_val_chars(spec, sb, sizeof sb, &sp);
    if (sl < 8 || strncmp(sp, "TRANS(", 6) != 0 || sp[sl - 1] != ')') return 0;
    char inner[256];
    int64_t ilen = sl - 7;              /* between "TRANS(" and ")" */
    if (ilen <= 0 || ilen >= (int64_t)sizeof inner) return 0;
    memcpy(inner, sp + 6, (size_t)ilen);
    inner[ilen] = '\0';
    char *parts[4] = {inner, NULL, NULL, NULL};
    int np = 0;
    for (char *q = inner; *q; q++)
        if (*q == ',') { *q = '\0'; if (++np < 4) parts[np] = q + 1; }
    if (np < 2) return 0;
    int64_t keyattr = parts[1] ? strtoll(parts[1], NULL, 10) : 0;
    int64_t tattr = parts[2] ? strtoll(parts[2], NULL, 10) : 0;
    char ctl = (parts[3] && parts[3][0]) ? parts[3][0] : 'X';
    if (ctl != 'X' || keyattr < 1 || tattr < 1) return 0;

    trans_ent *t = trans_file(ctx, parts[0], (int64_t)strlen(parts[0]));
    if (!t->ok || t->fvar.tag != MV_FILE) return 0;
    mvx_file *tf = (mvx_file *)(intptr_t)t->fvar.i;
    if (((mvx_file_base *)tf)->driver != b->driver) return 0;  /* same driver */

    store_state *st = state(ctx);
    /* Prefer a mapped identity column for either side of the join. */
    const char *src_keycol = map_identity_col(find_open(st, f), keyattr);
    const char *tgt_col = map_identity_col(find_open(st, tf), tattr);

    char opz[2] = {'=', '\0'};
    mvx_cursor *c = b->driver->select_join(f, keyattr, src_keycol, tf, tattr,
                                           tgt_col, opz, vp, vl);
    if (!c) return 0;

    clear_select(st);
    st->sel_active = 1;
    int64_t cap = 0;
    mv_value rid;
    mv_init(&rid);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        sel_push(st, &cap, rp, rl);
    }
    mv_clear(&rid);
    b->driver->select_end(c);
    return 1;
}

/* Push a BY on a TRANS() I-type down to a co-located JOIN + ORDER BY / LIMIT:
   parse the TRANS(file,keyattr,attr,control) descriptor and, if the target is
   on the same backend, form the *ordered* select list server-side (the same
   join the WITH push-down uses, ordered by the looked-up attribute instead of
   filtered).  Text order only — byte order matches MV's sort; a numeric BY
   falls back to the client, which sorts the per-record reference (X or C).
   Returns 1 if pushed, else 0 so the verb sorts per record. */
int64_t mvx_transorderselect(mvx_ctx *ctx, const mv_value *fvar,
                             const mv_value *spec, const mv_value *onum,
                             const mv_value *limit) {
    mvx_file *f = file_of(fvar, "TRANSORDERSELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->select_join_order) return 0;
    if (mv_get_int(onum) != 0) return 0;       /* numeric order not pushable */
    int64_t lim = mv_get_int(limit);

    char sb[256];
    const char *sp;
    int64_t sl = mv_val_chars(spec, sb, sizeof sb, &sp);
    if (sl < 8 || strncmp(sp, "TRANS(", 6) != 0 || sp[sl - 1] != ')') return 0;
    char inner[256];
    int64_t ilen = sl - 7;              /* between "TRANS(" and ")" */
    if (ilen <= 0 || ilen >= (int64_t)sizeof inner) return 0;
    memcpy(inner, sp + 6, (size_t)ilen);
    inner[ilen] = '\0';
    char *parts[4] = {inner, NULL, NULL, NULL};
    int np = 0;
    for (char *q = inner; *q; q++)
        if (*q == ',') { *q = '\0'; if (++np < 4) parts[np] = q + 1; }
    if (np < 2) return 0;
    int64_t keyattr = parts[1] ? strtoll(parts[1], NULL, 10) : 0;
    int64_t tattr = parts[2] ? strtoll(parts[2], NULL, 10) : 0;
    char ctl = (parts[3] && parts[3][0]) ? parts[3][0] : 'X';
    if ((ctl != 'X' && ctl != 'C') || keyattr < 1 || tattr < 1) return 0;

    trans_ent *t = trans_file(ctx, parts[0], (int64_t)strlen(parts[0]));
    if (!t->ok || t->fvar.tag != MV_FILE) return 0;
    mvx_file *tf = (mvx_file *)(intptr_t)t->fvar.i;
    if (((mvx_file_base *)tf)->driver != b->driver) return 0;  /* same driver */

    store_state *st = state(ctx);
    /* Prefer a mapped identity column for either side of the join. */
    const char *src_keycol = map_identity_col(find_open(st, f), keyattr);
    const char *tgt_col = map_identity_col(find_open(st, tf), tattr);

    mvx_cursor *c = b->driver->select_join_order(f, keyattr, src_keycol, tf,
                                                 tattr, tgt_col, ctl, 1, lim);
    if (!c) return 0;

    clear_select(st);
    st->sel_active = 1;
    int64_t cap = 0;
    mv_value rid;
    mv_init(&rid);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        sel_push(st, &cap, rp, rl);
    }
    mv_clear(&rid);
    b->driver->select_end(c);
    return 1;
}

/* Count records in the backend: with no item, count(*); with a pushable
   filter (=/# on a mapped identity column or the raw record attribute), a
   filtered count.  Returns the count, or -1 when it cannot push down so the
   verb counts by scanning. */
int64_t mvx_querycount(mvx_ctx *ctx, const mv_value *fvar,
                       const mv_value *item, const mv_value *op,
                       const mv_value *value, const mv_value *attr) {
    mvx_file *f = file_of(fvar, "QUERYCOUNT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->count_where) return -1;
    open_file *o = find_open(state(ctx), f);

    char ib[40];
    const char *ip;
    int64_t il = mv_val_chars(item, ib, sizeof ib, &ip);
    if (il == 0)                          /* no WITH: count every record */
        return b->driver->count_where(f, NULL, 0, "", NULL, 0);

    char ob[8];
    const char *opp;
    int64_t ol = mv_val_chars(op, ob, sizeof ob, &opp);
    if (!(ol == 1 && (opp[0] == '=' || opp[0] == '#'))) return -1;
    char vb[40];
    const char *vp;
    int64_t vl = mv_val_chars(value, vb, sizeof vb, &vp);
    if (vl == 0) return -1;

    int64_t attrno = mv_get_int(attr);
    char opz[2] = {opp[0], '\0'};
    const char *col = o ? map_identity_col(o, attrno) : NULL;
    if (col) return b->driver->count_where(f, col, 0, opz, vp, vl);
    if (attrno < 1) return -1;            /* @ID / I-type: no blob count */
    return b->driver->count_where(f, NULL, attrno, opz, vp, vl);
}

/* Sum a numeric field in the backend: `sumfield` must be mapped to a NUMERIC
   column (its display value is what a report totals), optionally filtered by
   the same push-down as COUNT.  Writes the total to dst as text, or "" when
   it cannot push down so the verb sums by scanning. */
void mvx_querysum(mvx_ctx *ctx, mv_value *dst, const mv_value *fvar,
                  const mv_value *sumfield, const mv_value *item,
                  const mv_value *op, const mv_value *value,
                  const mv_value *attr) {
    mv_set_str(dst, "", 0);
    mvx_file *f = file_of(fvar, "QUERYSUM");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->sum_where) return;
    open_file *o = find_open(state(ctx), f);
    if (!o) return;
    map_load(o);

    char sb[64];
    const char *sp;
    int64_t sl = mv_val_chars(sumfield, sb, sizeof sb, &sp);
    if (sl <= 0 || sl >= 64) return;
    char sname[64];
    memcpy(sname, sp, (size_t)sl);
    sname[sl] = '\0';
    const char *sumcol = NULL;
    for (int i = 0; i < o->map.nf; i++)
        if (o->map.assocs[i][0] == '\0' &&
            strcmp(o->map.types[i], "NUMERIC") == 0 &&
            strcmp(o->map.names[i], sname) == 0) {
            sumcol = o->map.names[i];
            break;
        }
    if (!sumcol) return;                  /* not a numeric column: scan */

    const char *fcol = NULL, *fop = "", *fval = NULL;
    int64_t fattr = 0, fvl = 0;
    char opz[2] = {0, 0}, vb[40], ib[40];
    const char *ip, *vp;
    int64_t il = mv_val_chars(item, ib, sizeof ib, &ip);
    if (il > 0) {
        char ob[8];
        const char *opp;
        int64_t ol = mv_val_chars(op, ob, sizeof ob, &opp);
        if (!(ol == 1 && (opp[0] == '=' || opp[0] == '#'))) return;
        fvl = mv_val_chars(value, vb, sizeof vb, &vp);
        if (fvl == 0) return;
        opz[0] = opp[0];
        fop = opz;
        fval = vp;
        int64_t attrno = mv_get_int(attr);
        fcol = map_identity_col(o, attrno);
        if (!fcol) {
            if (attrno < 1) return;
            fattr = attrno;
        }
    }
    char out[64];
    if (b->driver->sum_where(f, sumcol, fcol, fattr, fop, fval, fvl, out,
                             sizeof out))
        mv_set_str(dst, out, (int64_t)strlen(out));
}

/* Push BY (+ optional WITH) + FIRST n into the backend as ORDER BY / LIMIT:
   form the ordered, limited (and filtered) select list server-side when the
   order field is a mapped column of a matching type.  Returns 1 if pushed, 0
   to sort in the verb. */
int64_t mvx_orderselect(mvx_ctx *ctx, const mv_value *fvar,
                        const mv_value *fitem, const mv_value *fop_v,
                        const mv_value *fval_v, const mv_value *fattr_v,
                        const mv_value *oattr_v, const mv_value *onum_v,
                        const mv_value *limit_v) {
    mvx_file *f = file_of(fvar, "ORDERSELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    if (!b->driver->select_order) return 0;
    open_file *o = find_open(state(ctx), f);
    if (!o) return 0;

    int otext = 0;
    const char *ocol = map_order_col(o, mv_get_int(oattr_v),
                                     (int)mv_get_int(onum_v), &otext);
    if (!ocol) return 0;                  /* order field not a matching column */

    /* optional filter, pushable like the others */
    const char *fcol = NULL, *fop = "", *fval = NULL;
    int64_t fattr = 0, fvl = 0;
    char opz[2] = {0, 0}, vb[40], ib[40];
    const char *ip, *vp;
    int64_t il = mv_val_chars(fitem, ib, sizeof ib, &ip);
    if (il > 0) {
        char ob[8];
        const char *opp;
        int64_t ol = mv_val_chars(fop_v, ob, sizeof ob, &opp);
        if (!(ol == 1 && (opp[0] == '=' || opp[0] == '#'))) return 0;
        fvl = mv_val_chars(fval_v, vb, sizeof vb, &vp);
        if (fvl == 0) return 0;
        opz[0] = opp[0];
        fop = opz;
        fval = vp;
        int64_t attrno = mv_get_int(fattr_v);
        fcol = map_identity_col(o, attrno);
        if (!fcol) {
            if (attrno < 1) return 0;
            fattr = attrno;
        }
    }

    mvx_cursor *c = b->driver->select_order(f, fcol, fattr, fop, fval, fvl,
                                            ocol, otext, mv_get_int(limit_v));
    if (!c) return 0;
    store_state *st = state(ctx);
    clear_select(st);
    st->sel_active = 1;
    int64_t cap = 0;
    mv_value rid;
    mv_init(&rid);
    while (b->driver->select_next(c, &rid)) {
        char rb[40];
        const char *rp;
        int64_t rl = mv_val_chars(&rid, rb, sizeof rb, &rp);
        sel_push(st, &cap, rp, rl);
    }
    mv_clear(&rid);
    b->driver->select_end(c);
    return 1;
}

/* DESCRIBE: render how the backend would run the WITH / BY / FIRST query the
   verb parsed — without executing anything — so the user sees the plan (the SQL
   for an SQL backend, a note for a backend with no server-side query planner).
   `pspec` is the MULTISELECT condition list (attr<VM>op<VM>value, @AM-separated;
   empty for none); `ospec` is "battr<VM>bnum<VM>limit" (battr 0 = no pushable
   BY, bnum 1 = right-justified/numeric sort, limit 0 = no FIRST).  The push-down
   priority mirrors the verbs so the description matches what would actually run:
   ORDER+LIMIT first (SORT ... BY ... FIRST), else a multi-condition WHERE, else
   a plain scan with the conditions applied in the verb. */
void mvx_describe(mvx_ctx *ctx, mv_value *dst, const mv_value *fvar,
                  const mv_value *pspec, const mv_value *ospec) {
    mvx_file *f = file_of(fvar, "DESCRIBE");
    mvx_file_base *b = (mvx_file_base *)f;
    open_file *o = find_open(state(ctx), f);
    if (o) map_load(o);
    const char *drv = b->driver->name;

    /* --- parse the WITH conditions into predicates (as mvx_multiselect) --- */
    mvx_pred preds[32];
    char ops[32][4];
    int np = 0, allpush = 1;
    char sb[64];
    const char *sp;
    int64_t sl = mv_val_chars(pspec, sb, sizeof sb, &sp);
    char buf[2048];
    if (sl > 0 && sl < (int64_t)sizeof buf) {
        memcpy(buf, sp, (size_t)sl);
        buf[sl] = '\0';
        char *p = buf;
        while (p && *p && np < 32) {
            char *amend = p;
            while (*amend && *amend != (char)0xFE) amend++;
            char amsave = *amend;
            *amend = '\0';
            char *parts[3] = {p, NULL, NULL};
            int nv = 0;
            for (char *q = p; *q; q++)
                if (*q == (char)0xFD) { *q = '\0'; if (++nv < 3) parts[nv] = q + 1; }
            if (parts[1] && parts[2] && parts[2][0]) {
                int64_t attrno = strtoll(parts[0], NULL, 10);
                const char *op = parts[1], *val = parts[2];
                int range = (op[0] == '>' || op[0] == '<') &&
                            (op[1] == '\0' || (op[1] == '=' && op[2] == '\0'));
                int iseq = (op[0] == '=' || op[0] == '#') && op[1] == '\0';
                int numeric = 0, identity = 0;
                const char *col = NULL;
                if (o)
                    for (int i = 0; i < o->map.nf; i++)
                        if (o->map.anos[i] == attrno && o->map.assocs[i][0] == '\0') {
                            identity = strcmp(o->map.types[i], "TEXT") == 0 &&
                                       o->map.convs[i][0] == '\0';
                            numeric = strcmp(o->map.types[i], "NUMERIC") == 0 ||
                                      strcmp(o->map.types[i], "DATE") == 0 ||
                                      strcmp(o->map.types[i], "TIME") == 0;
                            if (identity) col = o->map.names[i];
                            break;
                        }
                int pushable = attrno >= 1 && (iseq || (range && numeric));
                if (!pushable) allpush = 0;
                strncpy(ops[np], op, 3);
                ops[np][3] = '\0';
                preds[np].col = range ? NULL : col;
                preds[np].attr = attrno;
                preds[np].op = ops[np];
                preds[np].numeric = range ? 1 : 0;
                preds[np].val = val;
                preds[np].vlen = (int64_t)strlen(val);
                np++;
            } else {
                allpush = 0;
            }
            if (amsave == 0) break;
            p = amend + 1;
        }
    }

    /* --- parse the BY / FIRST spec --- */
    int64_t battr = 0, bnum = 0, limit = 0;
    char ob[64];
    const char *op2;
    int64_t ol = mv_val_chars(ospec, ob, sizeof ob, &op2);
    if (ol > 0 && ol < (int64_t)sizeof ob) {
        char obuf[64];
        memcpy(obuf, op2, (size_t)ol);
        obuf[ol] = '\0';
        char *parts[3] = {obuf, NULL, NULL};
        int nv = 0;
        for (char *q = obuf; *q; q++)
            if (*q == (char)0xFD) { *q = '\0'; if (++nv < 3) parts[nv] = q + 1; }
        battr = strtoll(parts[0], NULL, 10);
        if (parts[1]) bnum = strtoll(parts[1], NULL, 10);
        if (parts[2]) limit = strtoll(parts[2], NULL, 10);
    }

    char plan[4096], sql[4000];
    int done = 0;

    /* Priority 1: BY + FIRST with at most one filter -> ORDER BY / LIMIT push. */
    if (!done && b->driver->explain && limit > 0 && np <= 1 && battr > 0) {
        int otext = 0;
        const char *ocol = o ? map_order_col(o, battr, (int)bnum, &otext) : NULL;
        int filterok = np == 0 ||
                       (ops[0][1] == '\0' && (ops[0][0] == '=' || ops[0][0] == '#'));
        if (ocol && filterok) {
            mvx_pred fp;
            if (np == 1) { fp = preds[0]; fp.numeric = 0; }
            if (b->driver->explain(f, np == 1 ? &fp : NULL, np, ocol, otext,
                                   limit, sql, sizeof sql)) {
                snprintf(plan, sizeof plan, "%s: %s", drv, sql);
                done = 1;
            }
        }
    }

    /* Priority 2: every condition pushes -> one server-side WHERE (no order). */
    if (!done && b->driver->explain && np >= 1 && allpush) {
        if (b->driver->explain(f, preds, np, NULL, 0, 0, sql, sizeof sql)) {
            size_t pp = (size_t)snprintf(plan, sizeof plan, "%s: %s", drv, sql);
            if (battr > 0 || limit > 0)
                snprintf(plan + pp, sizeof plan - pp, "; then %s in the verb",
                         limit > 0 ? "sorted and limited" : "sorted");
            done = 1;
        }
    }

    /* Priority 3: no server-side query for this shape. */
    if (!done) {
        if (b->driver->explain &&
            b->driver->explain(f, NULL, 0, NULL, 0, 0, sql, sizeof sql)) {
            size_t pp = (size_t)snprintf(plan, sizeof plan, "%s: %s", drv, sql);
            if (np > 0)
                pp += (size_t)snprintf(plan + pp, sizeof plan - pp,
                                       "; %d condition(s) applied in the verb", np);
            if (battr > 0 || limit > 0)
                snprintf(plan + pp, sizeof plan - pp, "; sorted in the verb");
        } else {
            snprintf(plan, sizeof plan,
                     "%s: no server-side query planner; the driver returns the "
                     "id list (index lookup when available, else a sequential "
                     "scan) and the verb applies any conditions and ordering",
                     drv);
        }
    }
    mv_set_str(dst, plan, (int64_t)strlen(plan));
}

void mvx_release(mvx_ctx *ctx, const mv_value *fvar, const mv_value *id) {
    store_state *st = state(ctx);
    if (!fvar) {                        /* bare RELEASE: drop everything */
        for (lock_ent *l = st->locks; l;) {
            lock_ent *n = l->next;
            if (l->f) {
                mvx_file_base *lb = (mvx_file_base *)l->f;
                if (lb->driver->unlock) {
                    const char *id = strchr(l->key, '\x01') + 1;
                    lb->driver->unlock(l->f, id, (int64_t)strlen(id));
                }
            }
            free(l->key);
            free(l);
            l = n;
        }
        st->locks = NULL;
        return;
    }
    mvx_file *f = file_of(fvar, "RELEASE");
    char ib[40];
    int64_t idlen;
    const char *ip = id_chars(id, ib, sizeof ib, &idlen);
    char *key = lock_key(f, ip, idlen);
    lock_drop(st, key);
    free(key);
}

void mvx_select(mvx_ctx *ctx, const mv_value *fvar) {
    mvx_file *f = file_of(fvar, "SELECT");
    mvx_file_base *b = (mvx_file_base *)f;
    store_state *st = state(ctx);
    clear_select(st);
    st->sel_active = 1;

    /* Materialise inside one short driver transaction — matches MV
       select-list semantics and keeps read txns short (4.2). */
    mvx_cursor *c = b->driver->select_begin(f);
    if (!c) return;
    int64_t cap = 0;
    mv_value id;
    mv_init(&id);
    while (b->driver->select_next(c, &id)) {
        if (st->sel_n == cap) {
            cap = cap ? cap * 2 : 64;
            mv_value *ns = realloc(st->sel_ids,
                                   (size_t)cap * sizeof(mv_value));
            if (!ns) mvx_fatal("out of memory in SELECT");
            st->sel_ids = ns;
        }
        mv_init(&st->sel_ids[st->sel_n]);
        mv_copy(&st->sel_ids[st->sel_n], &id);
        st->sel_n++;
    }
    mv_clear(&id);
    b->driver->select_end(c);
}

void mvx_formlist(mvx_ctx *ctx, const mv_value *ids) {
    store_state *st = state(ctx);
    clear_select(st);
    st->sel_active = 1;
    char nb[40];
    const char *p;
    int64_t len = mv_val_chars(ids, nb, sizeof nb, &p);
    int64_t cap = 0;
    const char *end = p + len;
    while (p < end) {
        const char *am = memchr(p, '\xFE', (size_t)(end - p));
        int64_t n = (am ? am : end) - p;
        if (n > 0) sel_push(st, &cap, p, n);
        p = am ? am + 1 : end;
    }
}

/* SYSTEM(11): is a select list active (in-process or session)? */
int64_t mvx_list_active(mvx_ctx *ctx) {
    store_state *st = state(ctx);
    if (st->sel_active) return st->sel_pos < st->sel_n;
    const char *sf = getenv("MVXSESSION");
    if (!sf || !sf[0]) return 0;
    FILE *fp = fopen(sf, "rb");
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fclose(fp);
    return sz > 0;
}

int64_t mvx_readnext(mvx_ctx *ctx, mv_value *id) {
    store_state *st = state(ctx);
    session_load(st);
    if (st->sel_pos >= st->sel_n) return 0;
    mv_copy(id, &st->sel_ids[st->sel_pos++]);
    return 1;
}

/* FILELIST(): every MV file in the account — subdirectories (directory
   driver) plus LMDB named DBs, as "name @VM type" attributes.  DICT
   stores and infrastructure directories are filtered out. */
void mvx_filelist(mvx_ctx *ctx, mv_value *dst) {
    (void)ctx;
    char *buf = NULL;
    size_t len = 0, cap = 0;

#define FL_PUT(p, n, ty)                                                  \
    do {                                                                  \
        size_t need = (n) + 3;                                            \
        if (len + need > cap) {                                           \
            cap = cap ? cap * 2 : 256;                                    \
            while (cap < len + need) cap *= 2;                            \
            char *nb = realloc(buf, cap);                                 \
            if (!nb) mvx_fatal("out of memory in FILELIST");              \
            buf = nb;                                                     \
        }                                                                 \
        if (len) buf[len++] = (char)0xFE;                                 \
        memcpy(buf + len, (p), (n));                                      \
        len += (n);                                                       \
        buf[len++] = (char)0xFD;                                          \
        buf[len++] = (ty);                                                \
    } while (0)

    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    /* scandir + alphasort, not readdir: directory order is filesystem-
       dependent (APFS returns sorted, ext4 hash order), so LISTF would
       otherwise vary by platform.  A file listing should be stable. */
    struct dirent **ents = NULL;
    int ne = scandir(acct, &ents, NULL, alphasort);
    for (int i = 0; i < ne; i++) {
        const char *nm = ents[i]->d_name;
        if (nm[0] != '.' && strcmp(nm, "mvxdata.lmdb") != 0) {
            char p[4096];
            snprintf(p, sizeof p, "%s/%s", acct, nm);
            struct stat sb;
            if (stat(p, &sb) == 0 && S_ISDIR(sb.st_mode))
                FL_PUT(nm, strlen(nm), 'D');
        }
        free(ents[i]);
    }
    free(ents);

    const mvx_driver *lmdb = driver_load("lmdb");
    if (lmdb->names) {
        mv_value names;
        mv_init(&names);
        char err[256] = "";
        if (lmdb->names(NULL, &names, err, sizeof err) &&
            names.tag == MV_STR && names.s->len > 0) {
            const char *p = mv_str_bytes(names.s), *end = p + names.s->len;
            while (p < end) {
                const char *am = memchr(p, '\xFE', (size_t)(end - p));
                size_t n = (am ? am : end) - p;
                int internal = (n > 5 && memcmp(p, "DICT.", 5) == 0) ||
                               memmem(p, n, ".IDX.", 5) != NULL;
                if (n > 0 && !internal) FL_PUT(p, n, 'L');
                p = am ? am + 1 : end;
            }
        }
        mv_clear(&names);
    }
    /* Bound files: the BINDINGS record names them (exactly).  Their
       type is the driver name, so LISTF distinguishes lmdbnet from
       postgres and the like.  A "*" entry is a policy, not a file. */
    int listed_bound = 0;
    {
        const char *acct2 = getenv("MVXACCOUNT");
        if (!acct2 || !acct2[0]) acct2 = ".";
        char rpath[4096];
        snprintf(rpath, sizeof rpath, "%s/BINDINGS", acct2);
        FILE *rf = fopen(rpath, "r");
        if (rf) {
            listed_bound = 1;
            char ln[1152];
            while (fgets(ln, sizeof ln, rf)) {
                char *p = ln;
                while (*p == ' ' || *p == '\t') p++;
                char *sp2 = p;
                while (*sp2 && *sp2 != ' ' && *sp2 != '\t' &&
                       *sp2 != '\n' && *sp2 != '\r')
                    sp2++;
                size_t n = (size_t)(sp2 - p);
                if (n == 0 || *p == '#' || (n == 1 && p[0] == '*'))
                    continue;
                char *dp = sp2;
                while (*dp == ' ' || *dp == '\t') dp++;
                char *de = dp;
                while (*de && *de != ' ' && *de != '\t' &&
                       *de != '\n' && *de != '\r')
                    de++;
                /* emit "name @VM driver" with an explicit driver type */
                if (len) buf[len++] = (char)0xFE;
                if (len + n > cap) { cap = cap ? cap*2 : 256;
                    while (cap < len + n) cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) mvx_fatal("out of memory in FILELIST");
                    buf = nb; }
                memcpy(buf + len, p, n); len += n;
                if (len + 1 > cap) { cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) mvx_fatal("out of memory in FILELIST");
                    buf = nb; }
                buf[len++] = (char)0xFD;
                size_t dl = (size_t)(de - dp);
                if (len + dl > cap) { cap = cap ? cap*2 : 256;
                    while (cap < len + dl) cap *= 2;
                    char *nb = realloc(buf, cap);
                    if (!nb) mvx_fatal("out of memory in FILELIST");
                    buf = nb; }
                memcpy(buf + len, dp, dl); len += dl;
            }
            fclose(rf);
        }
    }
    /* Whole-account Postgres binding (`* @conn` resolving to driver=postgres):
       per-file BINDINGS lines list nothing here, so enumerate the schema's
       record tables via the driver.  The empty spec matches only the `*`
       policy (never an exact file), giving the star driver and its @conn. */
    {
        char bdrv[64] = "", bparm[512] = "";
        if (binding_for("", bdrv, sizeof bdrv, bparm, sizeof bparm) &&
            strcmp(bdrv, "postgres") == 0) {
            const mvx_driver *pg = driver_load("postgres");
            if (pg->names) {
                mv_value names;
                mv_init(&names);
                char err[256] = "";
                if (pg->names(bparm, &names, err, sizeof err) &&
                    names.tag == MV_STR && names.s->len > 0) {
                    const char *p = mv_str_bytes(names.s), *end = p + names.s->len;
                    while (p < end) {
                        const char *am = memchr(p, '\xFE', (size_t)(end - p));
                        size_t n = (am ? am : end) - p;
                        int internal = n > 5 && memcmp(p, "DICT.", 5) == 0;
                        if (n > 0 && !internal) FL_PUT(p, n, 'P');
                        p = am ? am + 1 : end;
                    }
                }
                mv_clear(&names);
            }
        }
    }
    const char *dmn = getenv("MVXDAEMON");
    if (!listed_bound && dmn && dmn[0]) {
        const mvx_driver *net = driver_load("lmdbnet");
        if (net->names) {
            mv_value names;
            mv_init(&names);
            char err[256] = "";
            if (net->names(NULL, &names, err, sizeof err) &&
                names.tag == MV_STR && names.s->len > 0) {
                const char *p = mv_str_bytes(names.s);
                const char *end = p + names.s->len;
                while (p < end) {
                    const char *am = memchr(p, '\xFE',
                                            (size_t)(end - p));
                    size_t n = (am ? am : end) - p;
                    int internal =
                        (n > 5 && memcmp(p, "DICT.", 5) == 0) ||
                        memmem(p, n, ".IDX.", 5) != NULL;
                    if (n > 0 && !internal) FL_PUT(p, n, 'N');
                    p = am ? am + 1 : end;
                }
            }
            mv_clear(&names);
        }
    }
#undef FL_PUT

    mv_set_str(dst, buf ? buf : "", (int64_t)len);
    free(buf);
}

/* -------------------------------------------------------- file lifecycle
   The primitives behind the future CREATE-FILE / DELETE-FILE verbs
   (which will be BASIC programs, per the architecture). */

static int spec_cstr(const mv_value *spec, char *out, size_t cap) {
    char nb[40];
    const char *sp;
    int64_t slen = mv_val_chars(spec, nb, sizeof nb, &sp);
    if (slen == 0 || (size_t)slen >= cap) return 0;
    memcpy(out, sp, (size_t)slen);
    out[slen] = '\0';
    return 1;
}

/* Maintain the BINDINGS record (CREATE-FILE writes it, DELETE-FILE
   cleans it): one "SPEC driver {params}" line per bound file. */
static void binding_add(const char *cspec, const char *driver,
                        const char *params) {
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    char path[4096];
    snprintf(path, sizeof path, "%s/BINDINGS", acct);
    FILE *rp = fopen(path, "r");    /* skip if an exact entry exists */
    if (rp) {
        char ln[1152];
        size_t cl = strlen(cspec);
        while (fgets(ln, sizeof ln, rp)) {
            char *p = ln;
            while (*p == ' ' || *p == '\t') p++;
            char *sp2 = p;
            while (*sp2 && *sp2 != ' ' && *sp2 != '\t' &&
                   *sp2 != '\n' && *sp2 != '\r')
                sp2++;
            if ((size_t)(sp2 - p) == cl && memcmp(p, cspec, cl) == 0) {
                fclose(rp);
                return;
            }
        }
        fclose(rp);
    }
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    if (params && params[0])
        fprintf(fp, "%s %s %s\n", cspec, driver, params);
    else
        fprintf(fp, "%s %s\n", cspec, driver);
    fclose(fp);
}

static void binding_remove(const char *cspec) {
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    char path[4096];
    snprintf(path, sizeof path, "%s/BINDINGS", acct);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char out[16384];
    size_t olen = 0;
    char ln[1152];
    size_t cl = strlen(cspec);
    int changed = 0;
    while (fgets(ln, sizeof ln, fp)) {
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        char *sp2 = p;
        while (*sp2 && *sp2 != ' ' && *sp2 != '\t' && *sp2 != '\n' &&
               *sp2 != '\r')
            sp2++;
        if ((size_t)(sp2 - p) == cl && memcmp(p, cspec, cl) == 0) {
            changed = 1;
            continue;
        }
        size_t n = strlen(ln);
        if (olen + n < sizeof out) {
            memcpy(out + olen, ln, n);
            olen += n;
        }
    }
    fclose(fp);
    if (!changed) return;
    if (olen == 0) {
        unlink(path);
        return;
    }
    fp = fopen(path, "w");
    if (!fp) return;
    fwrite(out, 1, olen, fp);
    fclose(fp);
}

/* Whether the open account format is enabled ($MVX_OPENACCOUNT, set by mvx-git
   from the account's git config `mvx.openaccount`).  The open form is a
   git-boundary translation only: on disk an MVX account is always native, so
   this gates how the record-git engine reads/writes git objects and how
   status/diff compare — never what is written to disk.  See DECISIONS.md. */
int mvx_openaccount(void) {
    const char *e = getenv("MVX_OPENACCOUNT");
    return e && (*e == '1' || *e == 'y' || *e == 'Y' || *e == 't' || *e == 'T');
}

/* Classify a master-VOC record by its MVX type code for the record-git filter
   (each MV platform defines its own set): 0 = keep (the user's own procs),
   1 = always drop (a system verb/keyword the destination supplies), 2 = drop in
   the open interchange only (a file/dir pointer — the portable form travels as
   <file>.DICT/%FILE%).

   On MVX the standard verbs come from the *system* account, so the account's
   own VOC holds only the user's local verbs (cataloged from BP, which travels)
   — those are kept.  Only file/dir pointers are platform-specific, dropped in
   the open interchange since %FILE% carries the portable file type.  (Contrast
   UniData, whose account VOC is populated with the system verbs, so its
   classifier drops V/K.) */
int mvx_voc_class(const char *type, int64_t len) {
    static const struct { const char *t; int c; } tbl[] = {
        /* A CATALOGUED VERB IS REBUILT, NOT CARRIED.  `CATALOG BP MYPROG`
           writes `V` / CATALOG/MYPROG, and CATALOG/ is furniture that never
           travels (mv_git#130) -- so the committed record named a directory the
           clone does not have.  BP travels, and BUILD re-catalogues from it when
           it provisions an account, which is the same assumption the CATALOG
           provisioning-pointer skip already rests on (mvx#77).  So it is
           derived, like a file's own pointer, and a wholesale add leaves it out
           (mvx#133); naming it explicitly still stages it.

           This REPLACES the earlier reading, which was that an MVX account's
           VOC holds only the user's own verbs and those are kept.  True as far
           as it went -- they are the user's, not the system's -- but being the
           user's and being DERIVED are different questions, and the second one
           decides this. */
        {"V", 1},                           /* catalogued verb */
        {"F", 2}, {"DIR", 2}, {"Q", 2},     /* file / directory / q-pointer */
        {NULL, 0}
    };
    for (int i = 0; tbl[i].t; i++) {
        size_t sl = strlen(tbl[i].t);
        if ((size_t)len == sl && strncasecmp(type, tbl[i].t, sl) == 0)
            return tbl[i].c;
    }
    return 0;
}

/* Stamp a %FILE% control record into a just-created dictionary: a
   metadata hint that travels with the schema in git (BUILD reads it to
   know a file's backend after a clone).  On disk it is always the native
   record = "FILE" VM type VM conn; the engine translates it to the portable
   DIR/hash class when it writes the open form to git. */
static void write_file_meta(const mvx_driver *drv, const char *dictspec,
                            const char *type, const char *conn) {
    char err[256] = "";
    mvx_file *d = drv->open(dictspec, err, sizeof err);
    if (!d) return;
    char rec[700];
    size_t n = 0;
    memcpy(rec + n, "FILE", 4); n += 4;
    rec[n++] = (char)0xFD;                  /* value mark */
    size_t tl = strlen(type);
    memcpy(rec + n, type, tl); n += tl;
    if (conn && conn[0]) {
        rec[n++] = (char)0xFD;
        size_t cl = strlen(conn);
        memcpy(rec + n, conn, cl); n += cl;
    }
    mv_value rv;
    mv_init(&rv);
    mv_set_str(&rv, rec, (int64_t)n);
    drv->write(d, "%FILE%", 6, &rv);
    mv_clear(&rv);
    drv->close(d);
}

/* Register a newly created file in the account's VOC as a file pointer, so
   every MV file is discoverable there (#71) alongside its dictionary.  The
   record is the classic "F" file-defining item — attr 1 "F", attr 2 the data
   location, attr 3 the dictionary location (by name; MVX resolves files by
   name through the driver).  Skips the master dictionary itself and bare
   dictionaries (a file's own VOC item already covers its dictionary), never
   clobbers an existing VOC record, and is a silent no-op before the VOC exists
   (the bootstrap CREATE-FILE VOC). */
static void voc_register(const char *name) {
    size_t nl = strlen(name);
    if (strcmp(name, "VOC") == 0 || strcmp(name, "MD") == 0) return;
    if (nl > 5 && strcmp(name + nl - 5, ".DICT") == 0) return;

    char vspec[1152];
    const mvx_driver *drv = resolve("VOC", 0, vspec, sizeof vspec);
    char err[256] = "";
    mvx_file *v = drv->open(vspec, err, sizeof err);
    if (!v) return;

    mv_value existing;
    mv_init(&existing);
    if (drv->read(v, name, (int64_t)nl, &existing)) {   /* already present */
        mv_clear(&existing);
        drv->close(v);
        return;
    }
    mv_clear(&existing);

    char rec[600];
    size_t n = 0;
    rec[n++] = 'F';
    rec[n++] = (char)0xFE;                 /* attribute mark */
    memcpy(rec + n, name, nl); n += nl;
    rec[n++] = (char)0xFE;
    memcpy(rec + n, name, nl); n += nl;
    memcpy(rec + n, ".DICT", 5); n += 5;

    mv_value rv;
    mv_init(&rv);
    mv_set_str(&rv, rec, (int64_t)n);
    drv->write(v, name, (int64_t)nl, &rv);
    mv_clear(&rv);
    drv->close(v);
}

/* Remove a file's VOC pointer on DELETE-FILE — but only if the record is in
   fact a file pointer (attr 1 == "F"), never a verb that happens to share the
   name. */
static void voc_unregister(const char *name) {
    size_t nl = strlen(name);
    char vspec[1152];
    const mvx_driver *drv = resolve("VOC", 0, vspec, sizeof vspec);
    char err[256] = "";
    mvx_file *v = drv->open(vspec, err, sizeof err);
    if (!v) return;
    mv_value rec;
    mv_init(&rec);
    if (drv->read(v, name, (int64_t)nl, &rec)) {
        char nb[40];
        const char *p;
        int64_t len = mv_val_chars(&rec, nb, sizeof nb, &p);
        if (len >= 1 && p[0] == 'F' && (len == 1 || p[1] == (char)0xFE))
            drv->del(v, name, (int64_t)nl);
    }
    mv_clear(&rec);
    drv->close(v);
}

/* The account's default file type for CREATE-FILE with no explicit type, read
   from the `.mvx` (or open-checkout `.mv-account`) descriptor line
   `hash = <spec>`, where <spec> is a CREATE-FILE type such as `lmdb` or
   `USING postgres @pgmain`.  Empty ⇒ the built-in default (local lmdb).  Lets an
   account pick a default hash backend for new files while still allowing an
   explicit type per CREATE-FILE. */
void mvx_account_hash(char *buf, size_t cap) {
    if (cap) buf[0] = '\0';
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    const char *names[2] = {".mvx", ".mv-account"};
    for (int i = 0; i < 2 && !buf[0]; i++) {
        char p[4200];
        snprintf(p, sizeof p, "%s/%s", acct, names[i]);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        char line[600];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            char *k = line;
            while (*k == ' ' || *k == '\t') k++;
            char *ke = eq;
            while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
            if ((size_t)(ke - k) != 4 || strncasecmp(k, "hash", 4) != 0)
                continue;
            char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = strlen(v);
            while (vl && (v[vl - 1] == '\n' || v[vl - 1] == '\r' ||
                          v[vl - 1] == ' ' || v[vl - 1] == '\t'))
                vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(buf, v, vl);
            buf[vl] = '\0';
            break;
        }
        fclose(f);
    }
}

/* --- when the backend a file names is not on this host (mvx#113) -----------
 *
 * Migration is per FILE, so a repository's files need not all live on the same
 * backend — and a clone onto a machine without postgres is an ordinary thing to
 * want.  Until now that was fatal, part-way through, leaving a half-made
 * account behind.  Ask instead.
 *
 * THE QUESTION IS ASKED WHERE THE FILE IS CREATED, and nowhere else.  Rebinding
 * a file on an ordinary OPEN would point a live file at empty local storage
 * while its data sat in a backend that was merely unreachable — a far worse
 * answer than refusing.  At create time there is no data to lose yet: a clone's
 * records arrive from git afterwards.
 *
 * Per file, with an "always", because both halves matter: an account that
 * deliberately spread files across backends should not have that flattened by
 * one answer, and forty files must not mean forty questions.
 */
static char g_sub_all[64];        /* the "always" answer, for the rest of the run */
static int  g_sub_skip_all;       /* likewise, for "no" */

/* 1 -> use `chosen`; 0 -> skip this file; -1 -> abort the operation. */
static int driver_substitute(const char *file, const char *want,
                             char *chosen, size_t cap) {
    /* SUPPLIED UP FRONT BEATS ASKING.  It is how a script says what it wants,
       and it is the only reason this path is testable — a prompt no automated
       test can reach is a prompt that rots. */
    const char *env = getenv("MVXDRIVER");
    if (env && env[0]) {
        if (!mvx_driver_available(env)) {
            fprintf(stderr, "MVXDRIVER=\"%s\" is not a driver on this host\n",
                    env);
            return -1;
        }
        snprintf(chosen, cap, "%s", env);
        return 1;
    }
    if (g_sub_skip_all) return 0;
    if (g_sub_all[0]) { snprintf(chosen, cap, "%s", g_sub_all); return 1; }

    char avail[512];
    mvx_driver_names(avail, sizeof avail);
    for (char *c = avail; *c; c++) if (*c == ',') *c = ' ';

    char dflt[600];
    mvx_account_hash(dflt, sizeof dflt);
    if (!dflt[0] || !mvx_driver_available(dflt))
        snprintf(dflt, sizeof dflt, "lmdb");

    fprintf(stderr, "\n%s: backend \"%s\" is not available on this host\n",
            file, want);
    fprintf(stderr, "  available: %s\n", avail[0] ? avail : "(none)");

    /* NO TERMINAL MEANS NO GUESSING.  Not a prompt into a closed pipe, and not
       a silent default that quietly puts the file somewhere nobody chose. */
    if (!isatty(0)) {
        fprintf(stderr, "  not a terminal - set MVXDRIVER=<name> to choose "
                        "one, or run this where it can ask\n");
        return -1;
    }

    for (;;) {
        fprintf(stderr, "  create it on \"%s\" instead?"
                        "  [y]es  [n]o, skip  [a]lways  [q]uit"
                        "  (or type a driver name): ", dflt);
        fflush(stderr);
        char line[128];
        if (!fgets(line, sizeof line, stdin)) {
            fprintf(stderr, "\n");
            return -1;                       /* input ended: abort, not guess */
        }
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (!*p || strcasecmp(p, "y") == 0 || strcasecmp(p, "yes") == 0) {
            snprintf(chosen, cap, "%s", dflt);
            return 1;
        }
        if (strcasecmp(p, "n") == 0 || strcasecmp(p, "no") == 0) return 0;
        if (strcasecmp(p, "q") == 0 || strcasecmp(p, "quit") == 0) return -1;
        if (strcasecmp(p, "a") == 0 || strcasecmp(p, "all") == 0 ||
            strcasecmp(p, "always") == 0) {
            snprintf(g_sub_all, sizeof g_sub_all, "%s", dflt);
            snprintf(chosen, cap, "%s", dflt);
            return 1;
        }
        /* A name, so the user is not stuck with the one on offer. */
        if (mvx_driver_available(p)) {
            snprintf(chosen, cap, "%s", p);
            return 1;
        }
        fprintf(stderr, "  \"%s\" is not a driver here\n", p);
    }
}

/* Bind `file` to the backend `want`, asking if this host does not have it.
 *
 * What a CLONE needs is the BINDING, not a create: on MVX a hash file comes
 * into existence on first write, so materialising never calls CREATE-FILE for
 * one — and routing this through CREATE-FILE instead fails outright, because by
 * then the file's VOC pointer has been restored and creating over it is refused.
 * Meanwhile the binding is the whole point: without it the file is silently made
 * on the local default, which is exactly the substitution the user wanted to be
 * asked about (mvx#113).
 *
 * Returns 1 when the file is bound (to `want`, or to whatever was chosen in its
 * place), 0 when the user declined or there was nobody to ask. */
int mvx_bind_driver(const char *file, const char *want) {
    if (!file || !file[0] || !want || !want[0]) return 0;
    /* A connection profile resolves its own driver later; nothing to check. */
    if (want[0] == '@') { binding_add(file, want, ""); return 1; }

    /* Already what this account would use: no binding, nothing to say.  An
       entry here would only record what was true anyway, on every file. */
    char dflt[600];
    mvx_account_hash(dflt, sizeof dflt);
    const char *eff = dflt[0] ? dflt : "lmdb";
    if (strcasecmp(eff, want) == 0) return 1;

    if (mvx_driver_available(want)) { binding_add(file, want, ""); return 1; }

    char sub[64];
    if (driver_substitute(file, want, sub, sizeof sub) != 1) return 0;
    /* The substitute may BE the default, in which case say nothing rather than
       record a binding that means "as usual". */
    if (strcasecmp(eff, sub) != 0) binding_add(file, sub, "");
    return 1;
}

int64_t mvx_createfile(mvx_ctx *ctx, const mv_value *spec,
                       const mv_value *type) {
    (void)ctx;
    char cspec[1024];
    if (!spec_cstr(spec, cspec, sizeof cspec)) return 0;

    char tb[600];
    const char *tp = "";
    if (type) mv_val_chars(type, tb, sizeof tb, &tp);
    /* No explicit type: fall back to the account's default hash type. */
    char defbuf[600];
    if (!tp[0]) {
        mvx_account_hash(defbuf, sizeof defbuf);
        if (defbuf[0]) tp = defbuf;
    }
    char err[256] = "";

    /* CREATE-FILE name USING <driver> {params}: bind at creation and
       create through that driver.  The binding is recorded in BINDINGS
       so every later OPEN resolves there. */
    if (strncasecmp(tp, "USING ", 6) == 0 ||
        strcasecmp(tp, "USING") == 0) {
        const char *dp2 = tp + 5;
        while (*dp2 == ' ' || *dp2 == '\t') dp2++;
        char drvname[64];
        const char *de = dp2;
        while (*de && *de != ' ' && *de != '\t') de++;
        snprintf(drvname, sizeof drvname, "%.*s", (int)(de - dp2), dp2);
        const char *ap = de;
        while (*ap == ' ' || *ap == '\t') ap++;
        if (!drvname[0]) {
            fprintf(stderr, "CREATE-FILE USING: name a driver "
                            "(e.g. lmdbnet)\n");
            return 0;
        }
        const char *envd = getenv("MVXDAEMON");
        if (strcmp(drvname, "lmdbnet") == 0 && !ap[0] &&
            (!envd || !envd[0])) {
            fprintf(stderr, "CREATE-FILE USING lmdbnet: no daemon "
                            "address (give one, or set $MVXDAEMON)\n");
            return 0;
        }
        /* The named backend may not be on this host — a clone of an account
           whose files were migrated elsewhere is the ordinary way to get here.
           Ask rather than abort, and bind to whatever is chosen so every later
           OPEN resolves there (mvx#113).  The connection params belong to the
           backend that is gone, so they do not travel with the substitution. */
        /* NOT for `@name`: that is a connection PROFILE, not a driver — the
           driver comes from the profile when the binding is resolved, so there
           is nothing here to check and "@conn1" is not a file on disk. */
        if (drvname[0] != '@' && !mvx_driver_available(drvname)) {
            char sub[64];
            int r = driver_substitute(cspec, drvname, sub, sizeof sub);
            if (r <= 0) return 0;
            snprintf(drvname, sizeof drvname, "%s", sub);
            ap = "";
        }
        binding_add(cspec, drvname, ap);
        char dataspec[1720], dictspec[1720];
        const mvx_driver *drv =
            resolve(cspec, 0, dataspec, sizeof dataspec);
        if (!drv->create(dataspec, err, sizeof err)) {
            if (err[0]) fprintf(stderr, "CREATE-FILE: %s\n", err);
            binding_remove(cspec);
            return 0;
        }
        resolve(cspec, 1, dictspec, sizeof dictspec);
        if (!drv->create(dictspec, err, sizeof err)) {
            drv->remove(dataspec, err, sizeof err);
            binding_remove(cspec);
            return 0;
        }
        write_file_meta(drv, dictspec, drvname, ap);
        voc_register(cspec);
        return 1;
    }

    if (tp[0] == 'D' || tp[0] == 'd') {
        const mvx_driver *drv = driver_load("dir");
        if (!drv->create(cspec, err, sizeof err)) return 0;
        /* A dictionary directory (NAME.DICT) is a file but needs no
           dictionary of its own — never create NAME.DICT.DICT. */
        size_t cl = strlen(cspec);
        if (cl > 5 && strcmp(cspec + cl - 5, ".DICT") == 0) return 1;
        char dspec[1152];
        snprintf(dspec, sizeof dspec, "%s.DICT", cspec);
        if (!drv->create(dspec, err, sizeof err)) {
            drv->remove(cspec, err, sizeof err);
            return 0;
        }
        write_file_meta(drv, dspec, "dir", "");
        voc_register(cspec);
        return 1;
    }

    /* LMDB-backed: honour the file's local/remote binding.  DICT and
       DATA are created together, classic style. */
    /* A binding inherited from somewhere else may name a backend this host does
       not have — the same question as the USING path above, and asked here
       because resolve() goes straight to the loader, which has nothing to fall
       back on (mvx#113). */
    {
        char bdrv[64], bpar[512];
        if (binding_for(cspec, bdrv, sizeof bdrv, bpar, sizeof bpar) &&
            !mvx_driver_available(bdrv)) {
            char sub[64];
            int r = driver_substitute(cspec, bdrv, sub, sizeof sub);
            if (r <= 0) return 0;
            binding_add(cspec, sub, "");
        }
    }
    char dataspec[1720], dictspec[1720];
    const mvx_driver *drv = resolve(cspec, 0, dataspec, sizeof dataspec);
    if (!drv->create(dataspec, err, sizeof err)) return 0;
    resolve(cspec, 1, dictspec, sizeof dictspec);
    if (!drv->create(dictspec, err, sizeof err)) {
        drv->remove(dataspec, err, sizeof err);
        return 0;
    }
    write_file_meta(drv, dictspec, "lmdb", "");
    voc_register(cspec);
    return 1;
}

int64_t mvx_deletefile(mvx_ctx *ctx, const mv_value *spec) {
    (void)ctx;
    char cspec[1024];
    if (!spec_cstr(spec, cspec, sizeof cspec)) return 0;

    char dspec[1720], rspec[1720];
    const mvx_driver *drv = resolve(cspec, 1, dspec, sizeof dspec);
    char err[256] = "";
    drv->remove(dspec, err, sizeof err);        /* dict first, may be absent */
    resolve(cspec, 0, rspec, sizeof rspec);
    int64_t r = drv->remove(rspec, err, sizeof err);
    if (r) {
        binding_remove(cspec);              /* binding dies with it */
        voc_unregister(cspec);              /* and its VOC file pointer (#71) */
    }
    return r;
}
