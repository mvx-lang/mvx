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

/* mvx-lmdbd — the networked LMDB daemon (ARCHITECTURE.md 4.3).
 *
 * Owns one LMDB environment EXCLUSIVELY and serialises client access:
 * a file is either embedded-access or daemon-owned, never both.  The
 * daemon is the single lock authority for its files; locks are leased
 * to the client connection and reaped when it drops, so a killed pod
 * cannot orphan a record lock.  SELECT snapshots the id list inside a
 * short transaction and only then sends it — a slow client never pins
 * a read transaction.
 *
 *   mvx-lmdbd -d <datadir> (-s <unix-socket> | -p <port>)
 *
 * The daemon speaks raw record bytes; MV semantics live in the client
 * runtime.  Requests are handled one at a time — writes serialise
 * naturally, matching LMDB's single-writer design.
 */
#include "../runtime/include/mvxd_proto.h"
#include "sha256.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <lmdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_CONNS 64
#define MAX_FRAME (16u * 1024 * 1024)
#define MAX_NS 16                       /* open environments kept (LRU) */

/* One LMDB environment per namespace (≈ Pick account), opened on demand
   under <datadir>/<namespace>/, so same-named files in different
   namespaces are isolated and each gets its own 126-sub-DB budget.
   Least-recently-used environments are closed when the cache is full. */
static const char *g_datadir;
typedef struct {
    char ns[128];
    MDB_env *env;
    unsigned used;
} nsent;
static nsent g_ns[MAX_NS];
static unsigned g_clock;

typedef struct lockent {
    char *key;                          /* ns \x01 spec \x01 id */
    int fd;
    struct lockent *next;
} lockent;

static lockent *g_locks;

/* Set by SIGTERM/SIGINT so the poll loop breaks and the cached environments
   are closed on the way out.  Without a clean close LMDB never releases each
   namespace's lock — on macOS that lock is a named POSIX semaphore, so a
   daemon killed mid-serve leaks one per open namespace, and a restart storm
   eventually exhausts the system limit (mirrors the runtime driver fix). */
static volatile sig_atomic_t g_stop;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------ plumbing */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Per-connection buffered, non-blocking framed I/O (#10).  The daemon is a
   single poll loop, so a blocking read or write on one client stalls every
   other client (head-of-line blocking).  Each connection instead keeps a read
   buffer that accumulates bytes until a whole length-prefixed frame is present
   and a write buffer drained as the socket accepts data, so a peer that dribbles
   a request or reads its reply slowly never blocks the loop. */
typedef struct {
    int    fd;
    char  *rbuf;                 /* received bytes not yet framed */
    size_t rlen, rcap;
    char  *wbuf;                 /* queued reply bytes, wpos..wlen still pending */
    size_t wpos, wlen, wcap;
} conn;

static void conn_free(conn *c) {
    free(c->rbuf);
    free(c->wbuf);
    c->rbuf = c->wbuf = NULL;
    c->rlen = c->rcap = c->wpos = c->wlen = c->wcap = 0;
}

/* Append reply bytes to the connection's write buffer. */
static void wqueue(conn *c, const void *p, size_t n) {
    if (c->wlen + n > c->wcap) {
        size_t cap = c->wcap ? c->wcap : 256;
        while (cap < c->wlen + n) cap *= 2;
        c->wbuf = realloc(c->wbuf, cap);
        if (!c->wbuf) exit(70);
        c->wcap = cap;
    }
    memcpy(c->wbuf + c->wlen, p, n);
    c->wlen += n;
}

/* Push as much of the pending reply as the socket takes without blocking.
   Returns 0 if the peer is gone. */
static int conn_flush(conn *c) {
    while (c->wpos < c->wlen) {
        ssize_t r = send(c->fd, c->wbuf + c->wpos, c->wlen - c->wpos, 0);
        if (r > 0) {
            c->wpos += (size_t)r;
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        if (r < 0 && errno == EINTR) continue;
        return 0;                /* error or closed */
    }
    c->wpos = c->wlen = 0;       /* fully drained */
    return 1;
}

typedef struct {
    char *d;
    size_t len, cap;
} outbuf;

static void oput(outbuf *o, const void *p, size_t n) {
    if (o->len + n > o->cap) {
        o->cap = o->cap ? o->cap * 2 : 256;
        while (o->cap < o->len + n) o->cap *= 2;
        o->d = realloc(o->d, o->cap);
        if (!o->d) exit(70);
    }
    memcpy(o->d + o->len, p, n);
    o->len += n;
}

static void o16(outbuf *o, uint16_t v) { oput(o, &v, 2); }
static void o32(outbuf *o, uint32_t v) { oput(o, &v, 4); }

/* Cursor over the request payload. */
typedef struct {
    const char *p;
    size_t left;
    int bad;
} inbuf;

static const char *itake(inbuf *i, size_t n) {
    if (i->left < n) {
        i->bad = 1;
        return NULL;
    }
    const char *p = i->p;
    i->p += n;
    i->left -= n;
    return p;
}

static uint16_t i16(inbuf *i) {
    const char *p = itake(i, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

static uint32_t i32(inbuf *i) {
    const char *p = itake(i, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

static const char *istr16(inbuf *i, uint16_t *len) {
    *len = i16(i);
    return itake(i, *len);
}

/* -------------------------------------------------------------- locks */

static char *lock_key(const char *ns, const char *spec, uint16_t sl,
                      const char *id, uint16_t il) {
    size_t nl = strlen(ns);
    char *k = malloc(nl + 1 + (size_t)sl + 1 + il + 1);
    if (!k) exit(70);
    memcpy(k, ns, nl);
    k[nl] = '\x01';
    memcpy(k + nl + 1, spec, sl);
    k[nl + 1 + sl] = '\x01';
    memcpy(k + nl + 1 + sl + 1, id, il);
    k[nl + 1 + sl + 1 + il] = '\0';
    return k;
}

static int lock_try(const char *key, int fd) {
    for (lockent *l = g_locks; l; l = l->next)
        if (strcmp(l->key, key) == 0)
            return l->fd == fd;         /* re-lock by owner is fine */
    lockent *l = malloc(sizeof(lockent));
    if (!l) exit(70);
    l->key = strdup(key);
    l->fd = fd;
    l->next = g_locks;
    g_locks = l;
    return 1;
}

static void lock_release(const char *key, int fd) {
    for (lockent **pp = &g_locks; *pp; pp = &(*pp)->next)
        if (strcmp((*pp)->key, key) == 0 && (*pp)->fd == fd) {
            lockent *dead = *pp;
            *pp = dead->next;
            free(dead->key);
            free(dead);
            return;
        }
}

/* ------------------------------------------------------- authentication

   Access control is generic and Pick-agnostic: a namespace is an opaque
   partition, authorised per connection by a bearer token.  The token is
   provisioned out-of-band by mvx-lmdbd-admin into <datadir>/accounts
   (`name salt hash`, the token stored only as a salted SHA-256).  With
   no accounts file the daemon runs open (trusted-network mode). */

typedef struct authent {
    int fd;
    char ns[128];
    struct authent *next;
} authent;

static authent *g_auth;

static int creds_enabled(void) {
    char p[4096];
    snprintf(p, sizeof p, "%s/accounts", g_datadir);
    struct stat sb;
    return stat(p, &sb) == 0 && sb.st_size > 0;
}

/* Verify a presented token against the provisioned salt+hash. */
static int creds_check(const char *ns, const char *token) {
    char p[4096];
    snprintf(p, sizeof p, "%s/accounts", g_datadir);
    FILE *fp = fopen(p, "r");
    if (!fp) return 0;
    char ln[512], name[128], salt[64], hash[80];
    int ok = 0;
    while (fgets(ln, sizeof ln, fp)) {
        if (sscanf(ln, "%127s %63s %79s", name, salt, hash) != 3) continue;
        if (strcmp(name, ns) != 0) continue;
        char h[65];
        sha256_salted_hex(salt, token, h);
        ok = strcmp(h, hash) == 0;
        break;
    }
    fclose(fp);
    return ok;
}

static void auth_add(int fd, const char *ns) {
    for (authent *a = g_auth; a; a = a->next)
        if (a->fd == fd && strcmp(a->ns, ns) == 0) return;
    authent *a = malloc(sizeof(authent));
    if (!a) exit(70);
    a->fd = fd;
    snprintf(a->ns, sizeof a->ns, "%s", ns);
    a->next = g_auth;
    g_auth = a;
}

static int authed(int fd, const char *ns) {
    for (authent *a = g_auth; a; a = a->next)
        if (a->fd == fd && strcmp(a->ns, ns) == 0) return 1;
    return 0;
}

/* The lease: a dropped connection releases every lock and authorisation
   it held. */
static void conn_reap(int fd) {
    lockent **pp = &g_locks;
    while (*pp) {
        if ((*pp)->fd == fd) {
            lockent *dead = *pp;
            *pp = dead->next;
            free(dead->key);
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
    authent **ap = &g_auth;
    while (*ap) {
        if ((*ap)->fd == fd) {
            authent *dead = *ap;
            *ap = dead->next;
            free(dead);
        } else {
            ap = &(*ap)->next;
        }
    }
}

/* ---------------------------------------------------------- namespaces */

/* A namespace names a directory under the data dir, so reject anything
   that could escape it or is empty. */
static int ns_ok(const char *ns) {
    if (!ns[0] || strcmp(ns, ".") == 0 || strcmp(ns, "..") == 0) return 0;
    for (const char *c = ns; *c; c++)
        if (*c == '/' || *c == '\\') return 0;
    return 1;
}

/* The environment for a namespace, opened on demand at
   <datadir>/<ns>/mvxdata.lmdb.  Closes the least-recently-used
   environment when the cache is full.  NULL on a bad name or open
   failure. */
static MDB_env *env_for(const char *ns) {
    if (!ns_ok(ns)) return NULL;
    for (int i = 0; i < MAX_NS; i++)
        if (g_ns[i].env && strcmp(g_ns[i].ns, ns) == 0) {
            g_ns[i].used = ++g_clock;
            return g_ns[i].env;
        }
    int slot = -1;
    unsigned lru = ~0u;
    for (int i = 0; i < MAX_NS; i++) {
        if (!g_ns[i].env) { slot = i; break; }
        if (g_ns[i].used < lru) { lru = g_ns[i].used; slot = i; }
    }
    if (g_ns[slot].env) {
        mdb_env_close(g_ns[slot].env);
        g_ns[slot].env = NULL;
    }
    char dir[4096], envp[4096];
    snprintf(dir, sizeof dir, "%s/%s", g_datadir, ns);
    mkdir(g_datadir, 0775);
    mkdir(dir, 0775);
    snprintf(envp, sizeof envp, "%s/mvxdata.lmdb", dir);
    mkdir(envp, 0775);
    MDB_env *e = NULL;
    if (mdb_env_create(&e) || mdb_env_set_maxdbs(e, 126) ||
        mdb_env_set_mapsize(e, (size_t)1 << 30) ||
        mdb_env_open(e, envp, 0, 0664)) {
        if (e) mdb_env_close(e);
        return NULL;
    }
    int dead = 0;
    mdb_reader_check(e, &dead);
    snprintf(g_ns[slot].ns, sizeof g_ns[slot].ns, "%s", ns);
    g_ns[slot].env = e;
    g_ns[slot].used = ++g_clock;
    return e;
}

/* ---------------------------------------------------------------- lmdb */

static int dbi_of(MDB_txn *txn, const char *spec, uint16_t sl,
                  unsigned flags, MDB_dbi *dbi) {
    char name[1400];
    if (sl >= sizeof name) return MDB_NOTFOUND;
    memcpy(name, spec, sl);
    name[sl] = '\0';
    return mdb_dbi_open(txn, name, flags, dbi);
}

static int idx_dbi_of(MDB_txn *txn, const char *spec, uint16_t sl,
                      const char *item, uint16_t il, unsigned extra,
                      MDB_dbi *dbi) {
    char name[1400];
    if ((size_t)sl + il + 6 >= sizeof name) return MDB_NOTFOUND;
    memcpy(name, spec, sl);
    memcpy(name + sl, ".IDX.", 5);
    memcpy(name + sl + 5, item, il);
    name[sl + 5 + il] = '\0';
    return mdb_dbi_open(txn, name, MDB_DUPSORT | extra, dbi);
}

/* ------------------------------------------------------------ handlers */

static void handle(int fd, uint8_t op, inbuf *in, outbuf *out,
                   uint8_t *status) {
    *status = MVXD_ST_ERR;
    uint16_t nsl = 0, sl = 0, il = 0;
    const char *spec = NULL;

    /* every request names its namespace first */
    const char *nsp = istr16(in, &nsl);
    if (in->bad || nsl == 0 || nsl >= 128) return;
    char ns[128];
    memcpy(ns, nsp, nsl);
    ns[nsl] = '\0';

    /* AUTH authorises this connection for the namespace; in open mode
       (no accounts file) it is a no-op success. */
    if (op == MVXD_OP_AUTH) {
        uint16_t tl = 0;
        const char *tp = istr16(in, &tl);
        if (in->bad || tl >= 256) return;
        char token[256];
        memcpy(token, tp, tl);
        token[tl] = '\0';
        if (!creds_enabled())
            *status = MVXD_ST_OK;
        else if (creds_check(ns, token)) {
            auth_add(fd, ns);
            *status = MVXD_ST_OK;
        } else {
            *status = MVXD_ST_DENIED;
        }
        return;
    }

    /* Once accounts exist, every other op needs the connection to have
       authenticated this namespace. */
    if (creds_enabled() && !authed(fd, ns)) {
        *status = MVXD_ST_DENIED;
        return;
    }

    MDB_env *env = env_for(ns);
    if (!env) return;

    if (op != MVXD_OP_NAMES) {
        spec = istr16(in, &sl);
        if (in->bad) return;
    }

    MDB_txn *txn = NULL;
    MDB_dbi dbi;

    switch (op) {
    case MVXD_OP_OPEN: {
        if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) return;
        *status = dbi_of(txn, spec, sl, 0, &dbi) == 0 ? MVXD_ST_OK
                                                      : MVXD_ST_NO;
        mdb_txn_abort(txn);
        return;
    }
    case MVXD_OP_CREATE: {
        if (mdb_txn_begin(env, NULL, 0, &txn)) return;
        if (dbi_of(txn, spec, sl, 0, &dbi) == 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;       /* already exists */
            return;
        }
        mdb_txn_abort(txn);
        if (mdb_txn_begin(env, NULL, 0, &txn)) return;
        if (dbi_of(txn, spec, sl, MDB_CREATE, &dbi) == 0 &&
            mdb_txn_commit(txn) == 0)
            *status = MVXD_ST_OK;
        else
            mdb_txn_abort(txn);
        return;
    }
    case MVXD_OP_REMOVE: {
        if (mdb_txn_begin(env, NULL, 0, &txn)) return;
        if (dbi_of(txn, spec, sl, 0, &dbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;
            return;
        }
        if (mdb_drop(txn, dbi, 1) == 0 && mdb_txn_commit(txn) == 0)
            *status = MVXD_ST_OK;
        else
            mdb_txn_abort(txn);
        return;
    }
    case MVXD_OP_READ: {
        const char *id = istr16(in, &il);
        if (in->bad) return;
        if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) return;
        if (dbi_of(txn, spec, sl, 0, &dbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;
            return;
        }
        MDB_val k = {il, (void *)id}, v;
        if (mdb_get(txn, dbi, &k, &v) == 0) {
            o32(out, (uint32_t)v.mv_size);
            oput(out, v.mv_data, v.mv_size);   /* copy out, then abort */
            *status = MVXD_ST_OK;
        } else {
            *status = MVXD_ST_NO;
        }
        mdb_txn_abort(txn);
        return;
    }
    case MVXD_OP_WRITE:
    case MVXD_OP_WRITE_IX: {
        const char *id = istr16(in, &il);
        if (in->bad) return;
        uint32_t dl = i32(in);
        const char *data = itake(in, dl);
        if (in->bad) return;
        if (mdb_txn_begin(env, NULL, 0, &txn)) return;
        if (dbi_of(txn, spec, sl, 0, &dbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;
            return;
        }
        if (op == MVXD_OP_WRITE_IX) {
            uint16_t nops = i16(in);
            for (uint16_t k = 0; k < nops; k++) {
                uint16_t itl, kl;
                const char *item = istr16(in, &itl);
                const char *key = istr16(in, &kl);
                const char *addp = itake(in, 1);
                if (in->bad) {
                    mdb_txn_abort(txn);
                    return;
                }
                MDB_dbi xdbi;
                if (idx_dbi_of(txn, spec, sl, item, itl, MDB_CREATE,
                               &xdbi) != 0) {
                    mdb_txn_abort(txn);
                    return;
                }
                MDB_val xk = {kl, (void *)key}, xd = {il, (void *)id};
                if (*addp) {
                    int rc = mdb_put(txn, xdbi, &xk, &xd, 0);
                    if (rc != 0 && rc != MDB_KEYEXIST) {
                        mdb_txn_abort(txn);
                        return;
                    }
                } else {
                    int rc = mdb_del(txn, xdbi, &xk, &xd);
                    if (rc != 0 && rc != MDB_NOTFOUND) {
                        mdb_txn_abort(txn);
                        return;
                    }
                }
            }
        }
        MDB_val k = {il, (void *)id}, v = {dl, (void *)data};
        if (mdb_put(txn, dbi, &k, &v, 0) == 0 && mdb_txn_commit(txn) == 0)
            *status = MVXD_ST_OK;
        else
            mdb_txn_abort(txn);
        return;
    }
    case MVXD_OP_DEL:
    case MVXD_OP_DEL_IX: {
        const char *id = istr16(in, &il);
        if (in->bad) return;
        if (mdb_txn_begin(env, NULL, 0, &txn)) return;
        if (dbi_of(txn, spec, sl, 0, &dbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;
            return;
        }
        if (op == MVXD_OP_DEL_IX) {
            uint16_t nops = i16(in);
            for (uint16_t k = 0; k < nops; k++) {
                uint16_t itl, kl;
                const char *item = istr16(in, &itl);
                const char *key = istr16(in, &kl);
                const char *addp = itake(in, 1);
                (void)addp;
                if (in->bad) {
                    mdb_txn_abort(txn);
                    return;
                }
                MDB_dbi xdbi;
                if (idx_dbi_of(txn, spec, sl, item, itl, MDB_CREATE,
                               &xdbi) != 0)
                    continue;
                MDB_val xk = {kl, (void *)key}, xd = {il, (void *)id};
                int rc = mdb_del(txn, xdbi, &xk, &xd);
                if (rc != 0 && rc != MDB_NOTFOUND) {
                    mdb_txn_abort(txn);
                    return;
                }
            }
        }
        MDB_val k = {il, (void *)id};
        int rc = mdb_del(txn, dbi, &k, NULL);
        if (rc != 0 && rc != MDB_NOTFOUND) {
            mdb_txn_abort(txn);
            return;
        }
        if (mdb_txn_commit(txn) == 0)
            *status = rc == 0 ? MVXD_ST_OK : MVXD_ST_NO;
        return;
    }
    case MVXD_OP_SELECT: {
        if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) return;
        if (dbi_of(txn, spec, sl, 0, &dbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;
            return;
        }
        MDB_cursor *cur;
        uint32_t count = 0;
        size_t count_pos = out->len;
        o32(out, 0);
        if (mdb_cursor_open(txn, dbi, &cur) == 0) {
            MDB_val k, v;
            while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == 0) {
                o16(out, (uint16_t)k.mv_size);
                oput(out, k.mv_data, k.mv_size);
                count++;
            }
            mdb_cursor_close(cur);
        }
        mdb_txn_abort(txn);             /* snapshot complete before send */
        memcpy(out->d + count_pos, &count, 4);
        *status = MVXD_ST_OK;
        return;
    }
    case MVXD_OP_IDX_SELECT: {
        uint16_t itl, kl;
        const char *item = istr16(in, &itl);
        const char *key = istr16(in, &kl);
        if (in->bad) return;
        if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) return;
        MDB_dbi xdbi;
        if (idx_dbi_of(txn, spec, sl, item, itl, 0, &xdbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;       /* no such index */
            return;
        }
        MDB_cursor *cur;
        uint32_t count = 0;
        size_t count_pos = out->len;
        o32(out, 0);
        if (mdb_cursor_open(txn, xdbi, &cur) == 0) {
            MDB_val k = {kl, (void *)key}, d;
            if (mdb_cursor_get(cur, &k, &d, MDB_SET_KEY) == 0) {
                do {
                    o16(out, (uint16_t)d.mv_size);
                    oput(out, d.mv_data, d.mv_size);
                    count++;
                } while (mdb_cursor_get(cur, &k, &d, MDB_NEXT_DUP) == 0);
            }
            mdb_cursor_close(cur);
        }
        mdb_txn_abort(txn);
        memcpy(out->d + count_pos, &count, 4);
        *status = MVXD_ST_OK;
        return;
    }
    case MVXD_OP_IDX_DROP: {
        uint16_t itl;
        const char *item = istr16(in, &itl);
        if (in->bad) return;
        if (mdb_txn_begin(env, NULL, 0, &txn)) return;
        MDB_dbi xdbi;
        if (idx_dbi_of(txn, spec, sl, item, itl, 0, &xdbi) != 0) {
            mdb_txn_abort(txn);
            *status = MVXD_ST_NO;
            return;
        }
        if (mdb_drop(txn, xdbi, 1) == 0 && mdb_txn_commit(txn) == 0)
            *status = MVXD_ST_OK;
        else
            mdb_txn_abort(txn);
        return;
    }
    case MVXD_OP_NAMES: {
        if (mdb_txn_begin(env, NULL, MDB_RDONLY, &txn)) return;
        MDB_dbi main_dbi;
        if (mdb_dbi_open(txn, NULL, 0, &main_dbi) != 0) {
            mdb_txn_abort(txn);
            return;
        }
        MDB_cursor *cur;
        uint32_t count = 0;
        size_t count_pos = out->len;
        o32(out, 0);
        if (mdb_cursor_open(txn, main_dbi, &cur) == 0) {
            MDB_val k, v;
            while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == 0) {
                /* The name, not the key: LMDB 1.0 keeps the terminating NUL
                   in a sub-database's name key.  Sending it would put it on
                   the wire and into FILELIST(), where a name that compares
                   equal to nothing double-lists the file (mvx#307).  Same
                   reasoning as lmdb_names() in runtime/src/mvx_drv_lmdb.c. */
                size_t kn = k.mv_size;
                while (kn > 0 && ((const char *)k.mv_data)[kn - 1] == '\0')
                    kn--;
                if (kn == 0) continue;
                o16(out, (uint16_t)kn);
                oput(out, k.mv_data, kn);
                count++;
            }
            mdb_cursor_close(cur);
        }
        mdb_txn_abort(txn);
        memcpy(out->d + count_pos, &count, 4);
        *status = MVXD_ST_OK;
        return;
    }
    case MVXD_OP_LOCK:
    case MVXD_OP_UNLOCK: {
        const char *id = istr16(in, &il);
        if (in->bad) return;
        char *key = lock_key(ns, spec, sl, id, il);
        if (op == MVXD_OP_LOCK)
            *status = lock_try(key, fd) ? MVXD_ST_OK : MVXD_ST_BUSY;
        else {
            lock_release(key, fd);
            *status = MVXD_ST_OK;
        }
        free(key);
        return;
    }
    }
}

/* Turn every whole frame buffered on the connection into a queued reply.  A
   frame is a 4-byte little-endian length L (1..MAX_FRAME) followed by L bytes:
   a 1-byte op and an (L-1)-byte payload.  Returns 0 on a protocol error, which
   the caller treats as a reason to drop the connection. */
static int conn_dispatch(conn *c) {
    for (;;) {
        if (c->rlen < 4) return 1;                      /* need length prefix */
        uint32_t plen;
        memcpy(&plen, c->rbuf, 4);
        if (plen < 1 || plen > MAX_FRAME) return 0;
        if (c->rlen < 4 + (size_t)plen) return 1;       /* frame incomplete */
        uint8_t op = (uint8_t)c->rbuf[4];
        inbuf in = {c->rbuf + 5, plen - 1, 0};
        outbuf out = {0, 0, 0};
        uint8_t status;
        handle(c->fd, op, &in, &out, &status);
        uint32_t rlen = (uint32_t)(1 + out.len);
        wqueue(c, &rlen, 4);
        wqueue(c, &status, 1);
        if (out.len) wqueue(c, out.d, out.len);
        free(out.d);
        size_t used = 4 + (size_t)plen;
        memmove(c->rbuf, c->rbuf + used, c->rlen - used);
        c->rlen -= used;
    }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    const char *datadir = NULL, *sockpath = NULL;
    int port = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            datadir = argv[++i];
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            sockpath = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else {
            fprintf(stderr,
                    "usage: mvx-lmdbd -d datadir (-s unix-socket | -p port)\n");
            return 2;
        }
    }
    if (!datadir || (!sockpath && port == 0)) {
        fprintf(stderr,
                "usage: mvx-lmdbd -d datadir (-s unix-socket | -p port)\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_stop);           /* clean shutdown: close envs on the way out */
    signal(SIGINT, on_stop);

    /* Environments open lazily, one per namespace, under the data dir. */
    g_datadir = datadir;
    mkdir(datadir, 0775);

    int lfd;
    if (sockpath) {
        lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {0};
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
        unlink(sockpath);
        if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
            listen(lfd, 16) != 0) {
            fprintf(stderr, "mvx-lmdbd: cannot listen on %s\n", sockpath);
            return 1;
        }
    } else {
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((uint16_t)port);
        if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
            listen(lfd, 16) != 0) {
            fprintf(stderr, "mvx-lmdbd: cannot listen on port %d\n", port);
            return 1;
        }
    }
    fprintf(stderr, "mvx-lmdbd: serving %s on %s\n", datadir,
            sockpath ? sockpath : "tcp");

    set_nonblock(lfd);
    struct pollfd fds[MAX_CONNS + 1];
    conn cs[MAX_CONNS + 1] = {0};
    int nfds = 1;
    fds[0].fd = lfd;
    fds[0].events = POLLIN;

    for (;;) {
        if (g_stop) break;              /* SIGTERM/SIGINT: shut down cleanly */
        /* A connection asks for POLLOUT only while it still has reply bytes to
           flush; otherwise it just watches for more request data. */
        for (int i = 1; i < nfds; i++)
            fds[i].events =
                (short)(POLLIN | (cs[i].wpos < cs[i].wlen ? POLLOUT : 0));

        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR) { if (g_stop) break; continue; }
            break;
        }

        if (fds[0].revents & POLLIN) {
            int c = accept(lfd, NULL, NULL);
            if (c >= 0) {
                if (nfds <= MAX_CONNS) {
                    set_nonblock(c);
                    fds[nfds].fd = c;
                    fds[nfds].events = POLLIN;
                    fds[nfds].revents = 0;  /* not polled yet this pass */
                    cs[nfds] = (conn){.fd = c};
                    nfds++;
                } else {
                    close(c);
                }
            }
        }

        for (int i = 1; i < nfds; i++) {
            conn *cn = &cs[i];
            int fd = fds[i].fd;
            int dead = 0;

            if (fds[i].revents & (POLLERR | POLLNVAL)) dead = 1;

            /* Drain whatever is readable into the read buffer, then dispatch
               any whole frames.  One recv per readable event bounds the buffer;
               poll re-signals while data remains. */
            if (!dead && (fds[i].revents & POLLIN)) {
                if (cn->rcap - cn->rlen < 65536) {
                    size_t cap = cn->rcap ? cn->rcap * 2 : 65536;
                    while (cap - cn->rlen < 65536) cap *= 2;
                    cn->rbuf = realloc(cn->rbuf, cap);
                    if (!cn->rbuf) exit(70);
                    cn->rcap = cap;
                }
                ssize_t r = recv(fd, cn->rbuf + cn->rlen, cn->rcap - cn->rlen, 0);
                if (r > 0) {
                    cn->rlen += (size_t)r;
                    if (!conn_dispatch(cn)) dead = 1;
                } else if (r == 0) {
                    dead = 1;               /* peer closed */
                } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR) {
                    dead = 1;
                }
            }

            /* Push queued replies out; a slow reader just leaves bytes pending
               for a later POLLOUT instead of blocking the loop. */
            if (!dead && !conn_flush(cn)) dead = 1;

            /* Honour a hangup only once the reply is fully delivered. */
            if ((fds[i].revents & POLLHUP) && cn->wpos >= cn->wlen) dead = 1;

            if (dead) {
                conn_reap(fd);
                close(fd);
                conn_free(cn);
                fds[i] = fds[nfds - 1];
                cs[i] = cs[nfds - 1];
                nfds--;
                i--;
            }
        }
    }

    /* Close every cached environment so LMDB releases (and, on macOS,
       sem_unlinks) each namespace's lock instead of leaking it. */
    for (int i = 0; i < MAX_NS; i++)
        if (g_ns[i].env) { mdb_env_close(g_ns[i].env); g_ns[i].env = NULL; }
    return 0;
}
