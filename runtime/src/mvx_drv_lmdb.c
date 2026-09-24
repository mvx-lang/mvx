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

/* Embedded LMDB driver: one environment per account at
 * <account>/mvxdata.lmdb, one named DB per MV file.  Key = record id,
 * value = MV record bytes — nearly a passthrough (ARCHITECTURE.md 4.2).
 *
 * Discipline enforced here:
 *   - short transactions only, one per operation;
 *   - reads copy out of the mmap before the txn ends;
 *   - single writer is LMDB's own constraint — nothing here assumes
 *     parallel writers;
 *   - key length validated against LMDB's 511-byte default.
 */
#include "mvx_driver.h"

#include <errno.h>
#include <lmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_KEY 511

typedef struct {
    mvx_file_base base;
    MDB_dbi dbi;
} lmdb_file;

struct mvx_cursor {
    mv_value *ids;
    int64_t n, pos;
};

/* ONE ENVIRONMENT AT A TIME, AND IT KNOWS WHICH ACCOUNT IT IS (mvx#278).
 *
 * This used to be a single global env with NO KEY -- `if (g_env) return
 * g_env;' -- so it belonged to whichever account opened a file first in the
 * process, and every account after it silently used that one.  A program that
 * opened a file in one account, LOGTO'd to another and wrote there WROTE INTO
 * THE FIRST ACCOUNT'S DATABASE, and the second never saw the record.  Reads
 * were wrong the same way.  That is the menu-switching-company case mvx#258
 * and mvx#264 exist to serve.
 *
 * `MVXACCOUNT' is "." under the shell and after mvx_logto, so the path did not
 * distinguish accounts either.  The env is keyed on the RESOLVED ABSOLUTE path
 * now, which names the database rather than the way it was spelled.
 *
 * Still one at a time: an lmdb file always lives in its own account's env
 * (lmdb_open takes no location), and the macOS named-semaphore cost below is
 * a reason not to hold several.  Asking for a different one closes the
 * current. */
static MDB_env *g_env;
static pid_t g_env_pid;
static char g_env_path[4096];

/* Close the environment on a clean exit so LMDB releases its lock.  On macOS
   that lock is a *named* POSIX semaphore (one per environment); leaving it
   open leaks the semaphore, and enough leaks — churning short-lived accounts,
   or a crash storm — exhaust the system-wide limit until every later open
   fails with ENOSPC (only a reboot clears them).  Guarded by the opener's pid
   so a forked child never closes the parent's env.  (Companion to the
   mdb_reader_check below, which reaps dead reader-table slots.) */
static void env_atexit(void) {
    if (g_env && getpid() == g_env_pid) {
        mdb_env_close(g_env);
        g_env = NULL;
    }
}

/* The database this account means, named absolutely.  realpath on the ACCOUNT
   (which exists) rather than on the store (which may not yet). */
static void env_path(char *out, size_t cap) {
    const char *acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    char rp[4096];
    const char *base = realpath(acct, rp) ? rp : acct;
    snprintf(out, cap, "%s/mvxdata.lmdb", base);
}

static void env_drop(void) {
    if (g_env && getpid() == g_env_pid) mdb_env_close(g_env);
    g_env = NULL;
    g_env_path[0] = '\0';
}

/* Let an account go (mvx#251's contract, mvx#278's fix).  Without this
   mvx_store_leave could close every file and still leave the env open, so the
   next account inherited it.  The location is always the account's own. */
static void lmdb_release_conn(const char *loc) {
    (void)loc;
    env_drop();
}

static MDB_env *env_get(char *err, size_t errlen) {
    char path[4096];
    env_path(path, sizeof path);
    if (g_env) {
        if (strcmp(g_env_path, path) == 0) return g_env;
        env_drop();                  /* a different account: not ours any more */
    }
    mkdir(path, 0775);

    MDB_env *env;
    int rc = mdb_env_create(&env);
    if (rc == 0) rc = mdb_env_set_maxdbs(env, 126);
    if (rc == 0) rc = mdb_env_set_mapsize(env, (size_t)1 << 30);
    if (rc == 0) rc = mdb_env_open(env, path, 0, 0664);
    if (rc != 0) {
#ifdef __APPLE__
        /* On macOS ENOSPC here is usually the named-semaphore lock table being
           full, not the disk — point at the real cause and the real fix. */
        if (rc == ENOSPC)
            snprintf(err, errlen,
                     "lmdb: %s: No space left on device — if the disk has free "
                     "space this is the macOS lock-semaphore limit (stale locks "
                     "from crashed processes); restart to clear it", path);
        else
#endif
            snprintf(err, errlen, "lmdb: %s: %s", path, mdb_strerror(rc));
        if (env) mdb_env_close(env);
        return NULL;
    }
    /* Reap reader-table slots left by killed processes; without this,
       enough unclean exits eventually exhaust the table and every
       open fails with MDB_READERS_FULL. */
    int dead = 0;
    mdb_reader_check(env, &dead);
    g_env = env;
    g_env_pid = getpid();
    snprintf(g_env_path, sizeof g_env_path, "%s", path);
    static int registered;
    if (!registered) { atexit(env_atexit); registered = 1; }
    return env;
}

static const mvx_driver mvx_driver_lmdb;

/* Open the named DB; creating is the caller's choice via flags. */
static int dbi_open(unsigned flags, const char *spec, MDB_dbi *dbi,
                    char *err, size_t errlen) {
    MDB_env *env = env_get(err, errlen);
    if (!env) return 0;
    MDB_txn *txn;
    int rc = mdb_txn_begin(env, NULL, 0, &txn);
    if (rc != 0) {
        snprintf(err, errlen, "lmdb: %s", mdb_strerror(rc));
        return 0;
    }
    rc = mdb_dbi_open(txn, spec, flags, dbi);
    if (rc != 0) {
        mdb_txn_abort(txn);
        if (rc != MDB_NOTFOUND)
            snprintf(err, errlen, "lmdb: %s: %s", spec, mdb_strerror(rc));
        return 0;
    }
    rc = mdb_txn_commit(txn);
    if (rc != 0) {
        snprintf(err, errlen, "lmdb: %s", mdb_strerror(rc));
        return 0;
    }
    return 1;
}

static mvx_file *lmdb_open(const char *spec, char *err, size_t errlen) {
    MDB_dbi dbi;
    if (!dbi_open(0, spec, &dbi, err, errlen))   /* no MDB_CREATE: explicit */
        return NULL;

    lmdb_file *f = calloc(1, sizeof(lmdb_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_lmdb;
    f->base.spec = strdup(spec);
    f->dbi = dbi;
    return (mvx_file *)f;
}

static void lmdb_close(mvx_file *fh) {
    lmdb_file *f = (lmdb_file *)fh;
    free(f->base.spec);
    free(f);
}

static int lmdb_read(mvx_file *fh, const char *id, int64_t idlen,
                     mv_value *rec) {
    lmdb_file *f = (lmdb_file *)fh;
    if (idlen > MAX_KEY) return 0;
    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, MDB_RDONLY, &txn) != 0) return 0;
    MDB_val k = {(size_t)idlen, (void *)id}, v;
    int rc = mdb_get(txn, f->dbi, &k, &v);
    if (rc == 0)
        mv_set_str(rec, v.mv_data, (int64_t)v.mv_size);  /* copy out */
    mdb_txn_abort(txn);
    return rc == 0;
}

static int lmdb_write(mvx_file *fh, const char *id, int64_t idlen,
                      const mv_value *rec) {
    lmdb_file *f = (lmdb_file *)fh;
    if (idlen > MAX_KEY) return 0;
    char nb[40];
    const char *rp;
    int64_t rlen = mv_val_chars(rec, nb, sizeof nb, &rp);

    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, 0, &txn) != 0) return 0;
    MDB_val k = {(size_t)idlen, (void *)id};
    MDB_val v = {(size_t)rlen, (void *)rp};
    int rc = mdb_put(txn, f->dbi, &k, &v, 0);
    if (rc != 0) { mdb_txn_abort(txn); return 0; }
    return mdb_txn_commit(txn) == 0;
}

static int lmdb_del(mvx_file *fh, const char *id, int64_t idlen) {
    lmdb_file *f = (lmdb_file *)fh;
    if (idlen > MAX_KEY) return 0;
    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, 0, &txn) != 0) return 0;
    MDB_val k = {(size_t)idlen, (void *)id};
    int rc = mdb_del(txn, f->dbi, &k, NULL);
    if (rc != 0) { mdb_txn_abort(txn); return 0; }
    return mdb_txn_commit(txn) == 0;
}

static mvx_cursor *lmdb_select_begin(mvx_file *fh) {
    lmdb_file *f = (lmdb_file *)fh;
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in SELECT");

    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, MDB_RDONLY, &txn) != 0) return c;
    MDB_cursor *cur;
    if (mdb_cursor_open(txn, f->dbi, &cur) != 0) {
        mdb_txn_abort(txn);
        return c;
    }
    MDB_val k, v;
    int64_t cap = 0;
    while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == 0) {
        if (c->n == cap) {
            cap = cap ? cap * 2 : 64;
            mv_value *ns = realloc(c->ids, (size_t)cap * sizeof(mv_value));
            if (!ns) mvx_fatal("out of memory in SELECT");
            c->ids = ns;
        }
        mv_init(&c->ids[c->n]);
        mv_set_str(&c->ids[c->n], k.mv_data, (int64_t)k.mv_size);
        c->n++;
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);                 /* snapshot done, txn closed */
    return c;
}

static int lmdb_select_next(mvx_cursor *c, mv_value *id) {
    if (c->pos >= c->n) return 0;
    mv_copy(id, &c->ids[c->pos++]);
    return 1;
}

static void lmdb_select_end(mvx_cursor *c) {
    for (int64_t i = 0; i < c->n; i++) mv_clear(&c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t lmdb_select_count(mvx_cursor *c) { return c ? c->n : 0; }

static int lmdb_create(const char *spec, char *err, size_t errlen);
static int lmdb_remove(const char *spec, char *err, size_t errlen);
static int lmdb_names(const char *loc, mv_value *out, char *err, size_t errlen);
static int lmdb_write_ix(mvx_file *f, const char *id, int64_t idlen,
                         const mv_value *rec, const mvx_ixop *ops,
                         int nops);
static int lmdb_del_ix(mvx_file *f, const char *id, int64_t idlen,
                       const mvx_ixop *ops, int nops);
static mvx_cursor *lmdb_index_select(mvx_file *f, const char *item,
                                     const char *key, int64_t klen);
static int lmdb_index_drop(mvx_file *f, const char *item);

static const mvx_driver mvx_driver_lmdb = {
    "lmdb",
    lmdb_open, lmdb_close,
    lmdb_read, lmdb_write, lmdb_del,
    lmdb_select_begin, lmdb_select_next, lmdb_select_end,
    lmdb_create, lmdb_remove,
    lmdb_names,
    lmdb_write_ix, lmdb_del_ix, lmdb_index_select, lmdb_index_drop,
    NULL, NULL,                         /* locks: runtime local table */
    .select_count = lmdb_select_count,
    .release_conn = lmdb_release_conn,  /* let a left account go (mvx#278) */
};

static int lmdb_create(const char *spec, char *err, size_t errlen) {
    MDB_dbi dbi;
    if (dbi_open(0, spec, &dbi, err, errlen)) return 0;  /* already exists */
    return dbi_open(MDB_CREATE, spec, &dbi, err, errlen);
}

static int lmdb_remove(const char *spec, char *err, size_t errlen) {
    MDB_dbi dbi;
    if (!dbi_open(0, spec, &dbi, err, errlen)) return 0;
    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, 0, &txn) != 0) return 0;
    if (mdb_drop(txn, dbi, 1) != 0) { mdb_txn_abort(txn); return 0; }
    return mdb_txn_commit(txn) == 0;
}

/* Named-DB names are the keys of the environment's unnamed main DB. */
static int lmdb_names(const char *loc, mv_value *out, char *err, size_t errlen) {
    MDB_env *env = env_get(err, errlen);
    if (!env) return 0;
    MDB_txn *txn;
    if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) != 0) return 0;
    MDB_dbi main_dbi;
    if (mdb_dbi_open(txn, NULL, 0, &main_dbi) != 0) {
        mdb_txn_abort(txn);
        return 0;
    }
    MDB_cursor *cur;
    if (mdb_cursor_open(txn, main_dbi, &cur) != 0) {
        mdb_txn_abort(txn);
        return 0;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    MDB_val k, v;
    while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == 0) {
        if (len + k.mv_size + 1 > cap) {
            cap = cap ? cap * 2 : 256;
            while (cap < len + k.mv_size + 1) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) mvx_fatal("out of memory in LISTF");
            buf = nb;
        }
        if (len) buf[len++] = (char)0xFE;
        memcpy(buf + len, k.mv_data, k.mv_size);
        len += k.mv_size;
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    mv_set_str(out, buf ? buf : "", (int64_t)len);
    free(buf);
    return 1;
}

/* ------------------------------------------------- indexing capability
   Index = companion named DB "<spec>.IDX.<item>" with MDB_DUPSORT:
   key = extracted value, data = record id.  Record and index deltas
   share one transaction — no index drift (ARCHITECTURE.md 5.1). */

static int idx_dbi(MDB_txn *txn, lmdb_file *f, const char *item,
                   unsigned extra, MDB_dbi *dbi) {
    char name[1400];
    snprintf(name, sizeof name, "%s.IDX.%s", f->base.spec, item);
    return mdb_dbi_open(txn, name, MDB_DUPSORT | extra, dbi);
}

static int apply_ops(MDB_txn *txn, lmdb_file *f, const char *id,
                     int64_t idlen, const mvx_ixop *ops, int nops) {
    for (int i = 0; i < nops; i++) {
        MDB_dbi dbi;
        if (idx_dbi(txn, f, ops[i].item, MDB_CREATE, &dbi) != 0) return 0;
        MDB_val k = {(size_t)ops[i].klen, (void *)ops[i].key};
        MDB_val d = {(size_t)idlen, (void *)id};
        if (ops[i].add) {
            int rc = mdb_put(txn, dbi, &k, &d, 0);
            if (rc != 0 && rc != MDB_KEYEXIST) return 0;
        } else {
            int rc = mdb_del(txn, dbi, &k, &d);
            if (rc != 0 && rc != MDB_NOTFOUND) return 0;
        }
    }
    return 1;
}

static int lmdb_write_ix(mvx_file *fh, const char *id, int64_t idlen,
                         const mv_value *rec, const mvx_ixop *ops,
                         int nops) {
    lmdb_file *f = (lmdb_file *)fh;
    if (idlen > MAX_KEY) return 0;
    char nb[40];
    const char *rp;
    int64_t rlen = mv_val_chars(rec, nb, sizeof nb, &rp);

    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, 0, &txn) != 0) return 0;
    if (!apply_ops(txn, f, id, idlen, ops, nops)) {
        mdb_txn_abort(txn);
        return 0;
    }
    MDB_val k = {(size_t)idlen, (void *)id};
    MDB_val v = {(size_t)rlen, (void *)rp};
    if (mdb_put(txn, f->dbi, &k, &v, 0) != 0) {
        mdb_txn_abort(txn);
        return 0;
    }
    return mdb_txn_commit(txn) == 0;
}

static int lmdb_del_ix(mvx_file *fh, const char *id, int64_t idlen,
                       const mvx_ixop *ops, int nops) {
    lmdb_file *f = (lmdb_file *)fh;
    if (idlen > MAX_KEY) return 0;
    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, 0, &txn) != 0) return 0;
    if (!apply_ops(txn, f, id, idlen, ops, nops)) {
        mdb_txn_abort(txn);
        return 0;
    }
    MDB_val k = {(size_t)idlen, (void *)id};
    int rc = mdb_del(txn, f->dbi, &k, NULL);
    if (rc != 0 && rc != MDB_NOTFOUND) {
        mdb_txn_abort(txn);
        return 0;
    }
    return mdb_txn_commit(txn) == 0;
}

static mvx_cursor *lmdb_index_select(mvx_file *fh, const char *item,
                                     const char *key, int64_t klen) {
    lmdb_file *f = (lmdb_file *)fh;
    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, MDB_RDONLY, &txn) != 0) return NULL;
    MDB_dbi dbi;
    if (idx_dbi(txn, f, item, 0, &dbi) != 0) {   /* no such index */
        mdb_txn_abort(txn);
        return NULL;
    }
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in index select");
    MDB_cursor *cur;
    if (mdb_cursor_open(txn, dbi, &cur) == 0) {
        MDB_val k = {(size_t)klen, (void *)key}, d;
        int64_t cap = 0;
        if (mdb_cursor_get(cur, &k, &d, MDB_SET_KEY) == 0) {
            do {
                if (c->n == cap) {
                    cap = cap ? cap * 2 : 16;
                    mv_value *ns = realloc(c->ids,
                                           (size_t)cap * sizeof(mv_value));
                    if (!ns) mvx_fatal("out of memory in index select");
                    c->ids = ns;
                }
                mv_init(&c->ids[c->n]);
                mv_set_str(&c->ids[c->n], d.mv_data, (int64_t)d.mv_size);
                c->n++;
            } while (mdb_cursor_get(cur, &k, &d, MDB_NEXT_DUP) == 0);
        }
        mdb_cursor_close(cur);
    }
    mdb_txn_abort(txn);                 /* snapshot done */
    return c;
}

static int lmdb_index_drop(mvx_file *fh, const char *item) {
    lmdb_file *f = (lmdb_file *)fh;
    MDB_txn *txn;
    if (mdb_txn_begin(g_env, NULL, 0, &txn) != 0) return 0;
    MDB_dbi dbi;
    if (idx_dbi(txn, f, item, 0, &dbi) != 0) {
        mdb_txn_abort(txn);
        return 0;                       /* no such index */
    }
    if (mdb_drop(txn, dbi, 1) != 0) {
        mdb_txn_abort(txn);
        return 0;
    }
    return mdb_txn_commit(txn) == 0;
}

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_lmdb : NULL;
}
