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

/* lmdbnet driver — the networked transport for LMDB-backed files.
 *
 * Presents the identical storage contract as the embedded driver, but
 * every operation travels to an mvx-lmdbd daemon.  A file's spec may carry
 * its daemon address ("addr\nspec" — placed by the store from the
 * account's REMOTE bindings); $MVXDAEMON is the default.  One
 * connection per daemon, shared by all its files.  A path means a
 * unix socket, host:port means TCP.  Locks are held by the daemon
 * against the connection — process exit releases them (the lease).
 */
#include "../include/mvx_driver.h"
#include "../include/mvxd_proto.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    mvx_file_base base;
    int fd;                             /* connection to this file's daemon */
    const char *rspec;                  /* spec without the address prefix */
    char ns[128];                       /* target namespace on the daemon */
} net_file;

struct mvx_cursor {
    mv_value *ids;
    int64_t n, pos;
};

/* One connection per daemon address, shared by every file bound to
   that daemon.  The driver-level spec may carry its daemon address as
   "addr\nspec"; without a prefix, $MVXDAEMON is the default. */
#define MAX_DAEMONS 8
#define MAX_AUTHED 16
static struct {
    char addr[512];
    int fd;
    char authed[MAX_AUTHED][128];       /* namespaces AUTH'd on this conn */
    int nauthed;
} g_conns[MAX_DAEMONS];
static int g_nconns;

/* Split an addr-prefixed spec; returns the bare spec, fills addr. */
static const char *split_addr(const char *spec, char *addr, size_t cap) {
    const char *nl = strchr(spec, '\n');
    if (nl) {
        size_t n = (size_t)(nl - spec);
        if (n >= cap) n = cap - 1;
        memcpy(addr, spec, n);
        addr[n] = '\0';
        return nl + 1;
    }
    const char *e = getenv("MVXDAEMON");
    snprintf(addr, cap, "%s", e ? e : "");
    return spec;
}

/* The pre-spec location is "addr [namespace]" (the store supplies the
   namespace).  Split it; default the namespace to the account's own. */
static void split_loc(const char *loc, char *addr, size_t acap, char *ns,
                      size_t ncap) {
    const char *sp = loc;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    size_t al = (size_t)(sp - loc);
    if (al >= acap) al = acap - 1;
    memcpy(addr, loc, al);
    addr[al] = '\0';
    while (*sp == ' ' || *sp == '\t') sp++;
    if (*sp)
        snprintf(ns, ncap, "%s", sp);
    else
        mvx_account_namespace(ns, ncap);
}

/* Resolve a driver-level location to address + namespace + connection
   name.  "@name" is a named connection profile (the store passes the
   reference through); anything else is the inline "addr [namespace]". */
static void resolve_loc(const char *loc, char *addr, size_t acap, char *ns,
                        size_t ncap, char *conn, size_t ccap) {
    if (loc[0] == '@') {
        snprintf(conn, ccap, "%s", loc + 1);
        if (!mvx_conn_lookup(conn, "address", addr, acap)) addr[0] = '\0';
        if (!mvx_conn_lookup(conn, "namespace", ns, ncap))
            mvx_account_namespace(ns, ncap);
    } else {
        conn[0] = '\0';
        split_loc(loc, addr, acap, ns, ncap);
    }
}

static int send_all(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, 0);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int recv_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return 0;
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int daemon_connect(const char *spec, char *err, size_t errlen) {
    for (int i = 0; i < g_nconns; i++)
        if (strcmp(g_conns[i].addr, spec) == 0) return g_conns[i].fd;
    if (!spec[0]) {
        snprintf(err, errlen,
                 "lmdbnet: no daemon address (REMOTE line or $MVXDAEMON)");
        return -1;
    }
    if (g_nconns >= MAX_DAEMONS) {
        snprintf(err, errlen, "lmdbnet: too many daemons");
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    int fd;
    if (strchr(spec, '/')) {            /* unix socket */
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {0};
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "%s", spec);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
            close(fd);
            snprintf(err, errlen, "lmdbnet: cannot reach daemon at %s",
                     spec);
            return -1;
        }
    } else {                            /* host:port */
        char host[256] = "127.0.0.1";
        int port = 0;
        const char *colon = strrchr(spec, ':');
        if (colon) {
            size_t hl = (size_t)(colon - spec);
            if (hl > 0 && hl < sizeof host) {
                memcpy(host, spec, hl);
                host[hl] = '\0';
            }
            port = atoi(colon + 1);
        }
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char ports[16];
        snprintf(ports, sizeof ports, "%d", port);
        if (getaddrinfo(host, ports, &hints, &res) != 0 || !res) {
            snprintf(err, errlen, "lmdbnet: cannot resolve %s", spec);
            return -1;
        }
        fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            close(fd);
            freeaddrinfo(res);
            snprintf(err, errlen, "lmdbnet: cannot reach daemon at %s",
                     spec);
            return -1;
        }
        freeaddrinfo(res);
    }
    snprintf(g_conns[g_nconns].addr, sizeof g_conns[0].addr, "%s", spec);
    g_conns[g_nconns].fd = fd;
    g_nconns++;
    return fd;
}

/* ------------------------------------------------------- request builder */

typedef struct {
    char *d;
    size_t len, cap;
} reqbuf;

static void rput(reqbuf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        while (b->cap < b->len + n) b->cap *= 2;
        b->d = realloc(b->d, b->cap);
        if (!b->d) mvx_fatal("out of memory in lmdbnet");
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
}

static void r16(reqbuf *b, uint16_t v) { rput(b, &v, 2); }
static void r32(reqbuf *b, uint32_t v) { rput(b, &v, 4); }

static void rstr16(reqbuf *b, const char *p, size_t n) {
    r16(b, (uint16_t)n);
    rput(b, p, n);
}

/* Round trip on a connection: returns status, fills *resp/*rlen. */
static int roundtrip(int fd, uint8_t op, reqbuf *b, char **resp,
                     uint32_t *rlen) {
    *resp = NULL;
    *rlen = 0;
    uint32_t plen = (uint32_t)(1 + b->len);
    if (!send_all(fd, &plen, 4) || !send_all(fd, &op, 1) ||
        (b->len && !send_all(fd, b->d, b->len)))
        mvx_fatal("lmdbnet: daemon connection lost");
    uint32_t alen;
    uint8_t status;
    if (!recv_all(fd, &alen, 4) || alen < 1 ||
        !recv_all(fd, &status, 1))
        mvx_fatal("lmdbnet: daemon connection lost");
    if (alen > 1) {
        *resp = malloc(alen - 1);
        if (!*resp) mvx_fatal("out of memory in lmdbnet");
        if (!recv_all(fd, *resp, alen - 1))
            mvx_fatal("lmdbnet: daemon connection lost");
        *rlen = alen - 1;
    }
    free(b->d);
    b->d = NULL;
    b->len = b->cap = 0;
    return status;
}

/* Authorise this connection for a namespace, once.  The bearer token
   comes from the account credential store (.mvx-private); with none we
   send an empty token, which an open-mode daemon accepts and an
   authenticated one rejects.  Returns 0 (with err) if the daemon denies. */
static int net_auth(int fd, const char *addr, const char *ns,
                    const char *conn, char *err, size_t errlen) {
    int ci = -1;
    for (int i = 0; i < g_nconns; i++)
        if (g_conns[i].fd == fd) { ci = i; break; }
    if (ci >= 0)
        for (int j = 0; j < g_conns[ci].nauthed; j++)
            if (strcmp(g_conns[ci].authed[j], ns) == 0) return 1;

    /* the token comes from the connection profile when bound by name,
       else from the credential store keyed by address + namespace */
    char tok[256] = "";
    if (conn && conn[0])
        mvx_conn_lookup(conn, "token", tok, sizeof tok);
    else
        mvx_cred_lookup("lmdbnet", addr, ns, "token", tok, sizeof tok);

    reqbuf b = {0, 0, 0};
    rstr16(&b, ns, strlen(ns));
    rstr16(&b, tok, strlen(tok));
    char *resp;
    uint32_t rlen;
    int st = roundtrip(fd, MVXD_OP_AUTH, &b, &resp, &rlen);
    free(resp);
    if (st != MVXD_ST_OK) {
        snprintf(err, errlen,
                 "lmdbnet: authentication failed for namespace '%s' "
                 "(set its token with SET-CREDENTIAL)",
                 ns);
        return 0;
    }
    if (ci >= 0 && g_conns[ci].nauthed < MAX_AUTHED)
        snprintf(g_conns[ci].authed[g_conns[ci].nauthed++],
                 sizeof g_conns[ci].authed[0], "%s", ns);
    return 1;
}

/* ------------------------------------------------------------ contract */

static const mvx_driver mvx_driver_lmdbnet;

static mvx_file *net_open(const char *spec, char *err, size_t errlen) {
    char loc[512], addr[512], ns[128], conn[128];
    const char *rspec = split_addr(spec, loc, sizeof loc);
    resolve_loc(loc, addr, sizeof addr, ns, sizeof ns, conn, sizeof conn);
    int fd = daemon_connect(addr, err, errlen);
    if (fd < 0) return NULL;
    if (!net_auth(fd, addr, ns, conn, err, errlen)) return NULL;
    reqbuf b = {0, 0, 0};
    rstr16(&b, ns, strlen(ns));
    rstr16(&b, rspec, strlen(rspec));
    char *resp;
    uint32_t rlen;
    int st = roundtrip(fd, MVXD_OP_OPEN, &b, &resp, &rlen);
    free(resp);
    if (st != MVXD_ST_OK) return NULL;
    net_file *f = calloc(1, sizeof(net_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_lmdbnet;
    f->base.spec = strdup(spec);
    f->fd = fd;
    f->rspec = f->base.spec + (rspec - spec);
    snprintf(f->ns, sizeof f->ns, "%s", ns);
    return (mvx_file *)f;
}

static void net_close(mvx_file *fh) {
    net_file *f = (net_file *)fh;
    free(f->base.spec);
    free(f);
}

static reqbuf spec_req(mvx_file *fh) {
    reqbuf b = {0, 0, 0};
    net_file *f = (net_file *)fh;
    rstr16(&b, f->ns, strlen(f->ns));   /* namespace first, every op */
    rstr16(&b, f->rspec, strlen(f->rspec));
    return b;
}

#define NETFD(fh) (((net_file *)(fh))->fd)

static int net_read(mvx_file *fh, const char *id, int64_t idlen,
                    mv_value *rec) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_READ, &b, &resp, &rlen);
    if (st == MVXD_ST_OK && rlen >= 4) {
        uint32_t dl;
        memcpy(&dl, resp, 4);
        if (dl <= rlen - 4) mv_set_str(rec, resp + 4, dl);
    }
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_write(mvx_file *fh, const char *id, int64_t idlen,
                     const mv_value *rec) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    char nb[40];
    const char *rp;
    int64_t rl = mv_val_chars(rec, nb, sizeof nb, &rp);
    r32(&b, (uint32_t)rl);
    rput(&b, rp, (size_t)rl);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_WRITE, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_del(mvx_file *fh, const char *id, int64_t idlen) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_DEL, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

/* Parse a count+ids response into a cursor. */
static mvx_cursor *ids_cursor(const char *resp, uint32_t rlen) {
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in lmdbnet select");
    if (!resp || rlen < 4) return c;
    uint32_t count;
    memcpy(&count, resp, 4);
    const char *p = resp + 4, *end = resp + rlen;
    c->ids = calloc(count ? count : 1, sizeof(mv_value));
    if (!c->ids) mvx_fatal("out of memory in lmdbnet select");
    for (uint32_t i = 0; i < count && p + 2 <= end; i++) {
        uint16_t l;
        memcpy(&l, p, 2);
        p += 2;
        if (p + l > end) break;
        mv_init(&c->ids[c->n]);
        mv_set_str(&c->ids[c->n], p, l);
        c->n++;
        p += l;
    }
    return c;
}

static mvx_cursor *net_select_begin(mvx_file *fh) {
    reqbuf b = spec_req(fh);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_SELECT, &b, &resp, &rlen);
    mvx_cursor *c = st == MVXD_ST_OK ? ids_cursor(resp, rlen)
                                     : calloc(1, sizeof(mvx_cursor));
    free(resp);
    return c;
}

static int net_select_next(mvx_cursor *c, mv_value *id) {
    if (!c || c->pos >= c->n) return 0;
    mv_copy(id, &c->ids[c->pos++]);
    return 1;
}

static void net_select_end(mvx_cursor *c) {
    if (!c) return;
    for (int64_t i = 0; i < c->n; i++) mv_clear(&c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t net_select_count(mvx_cursor *c) { return c ? c->n : 0; }

static int net_create(const char *spec, char *err, size_t errlen) {
    char loc[512], addr[512], ns[128], conn[128];
    const char *rspec = split_addr(spec, loc, sizeof loc);
    resolve_loc(loc, addr, sizeof addr, ns, sizeof ns, conn, sizeof conn);
    int fd = daemon_connect(addr, err, errlen);
    if (fd < 0) return 0;
    if (!net_auth(fd, addr, ns, conn, err, errlen)) return 0;
    reqbuf b = {0, 0, 0};
    rstr16(&b, ns, strlen(ns));
    rstr16(&b, rspec, strlen(rspec));
    char *resp;
    uint32_t rlen;
    int st = roundtrip(fd, MVXD_OP_CREATE, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_remove(const char *spec, char *err, size_t errlen) {
    char loc[512], addr[512], ns[128], conn[128];
    const char *rspec = split_addr(spec, loc, sizeof loc);
    resolve_loc(loc, addr, sizeof addr, ns, sizeof ns, conn, sizeof conn);
    int fd = daemon_connect(addr, err, errlen);
    if (fd < 0) return 0;
    if (!net_auth(fd, addr, ns, conn, err, errlen)) return 0;
    reqbuf b = {0, 0, 0};
    rstr16(&b, ns, strlen(ns));
    rstr16(&b, rspec, strlen(rspec));
    char *resp;
    uint32_t rlen;
    int st = roundtrip(fd, MVXD_OP_REMOVE, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_names(const char *loc, mv_value *out, char *err, size_t errlen) {
    char addr[512], ns[128];
    split_addr("", addr, sizeof addr);  /* default daemon */
    mvx_account_namespace(ns, sizeof ns);
    int fd = daemon_connect(addr, err, errlen);
    if (fd < 0) return 0;
    if (!net_auth(fd, addr, ns, "", err, errlen)) return 0;
    reqbuf b = {0, 0, 0};
    rstr16(&b, ns, strlen(ns));
    char *resp;
    uint32_t rlen;
    int st = roundtrip(fd, MVXD_OP_NAMES, &b, &resp, &rlen);
    if (st != MVXD_ST_OK || !resp || rlen < 4) {
        free(resp);
        return 0;
    }
    uint32_t count;
    memcpy(&count, resp, 4);
    char *buf = NULL;
    size_t blen = 0, bcap = 0;
    const char *p = resp + 4, *end = resp + rlen;
    for (uint32_t i = 0; i < count && p + 2 <= end; i++) {
        uint16_t l;
        memcpy(&l, p, 2);
        p += 2;
        if (p + l > end) break;
        if (blen + l + 1 > bcap) {
            bcap = bcap ? bcap * 2 : 256;
            while (bcap < blen + l + 1) bcap *= 2;
            buf = realloc(buf, bcap);
            if (!buf) mvx_fatal("out of memory in lmdbnet names");
        }
        if (blen) buf[blen++] = (char)0xFE;
        memcpy(buf + blen, p, l);
        blen += l;
        p += l;
    }
    mv_set_str(out, buf ? buf : "", (int64_t)blen);
    free(buf);
    free(resp);
    return 1;
}

static int net_write_ix(mvx_file *fh, const char *id, int64_t idlen,
                        const mv_value *rec, const mvx_ixop *ops,
                        int nops) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    char nb[40];
    const char *rp;
    int64_t rl = mv_val_chars(rec, nb, sizeof nb, &rp);
    r32(&b, (uint32_t)rl);
    rput(&b, rp, (size_t)rl);
    r16(&b, (uint16_t)nops);
    for (int i = 0; i < nops; i++) {
        rstr16(&b, ops[i].item, strlen(ops[i].item));
        rstr16(&b, ops[i].key, (size_t)ops[i].klen);
        uint8_t add = ops[i].add ? 1 : 0;
        rput(&b, &add, 1);
    }
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_WRITE_IX, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_del_ix(mvx_file *fh, const char *id, int64_t idlen,
                      const mvx_ixop *ops, int nops) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    r16(&b, (uint16_t)nops);
    for (int i = 0; i < nops; i++) {
        rstr16(&b, ops[i].item, strlen(ops[i].item));
        rstr16(&b, ops[i].key, (size_t)ops[i].klen);
        uint8_t add = 0;
        rput(&b, &add, 1);
    }
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_DEL_IX, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static mvx_cursor *net_index_select(mvx_file *fh, const char *item,
                                    const char *key, int64_t klen) {
    reqbuf b = spec_req(fh);
    rstr16(&b, item, strlen(item));
    rstr16(&b, key, (size_t)klen);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_IDX_SELECT, &b, &resp, &rlen);
    if (st != MVXD_ST_OK) {             /* no such index */
        free(resp);
        return NULL;
    }
    mvx_cursor *c = ids_cursor(resp, rlen);
    free(resp);
    return c;
}

static int net_index_drop(mvx_file *fh, const char *item) {
    reqbuf b = spec_req(fh);
    rstr16(&b, item, strlen(item));
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_IDX_DROP, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_lock(mvx_file *fh, const char *id, int64_t idlen) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_LOCK, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static int net_unlock(mvx_file *fh, const char *id, int64_t idlen) {
    reqbuf b = spec_req(fh);
    rstr16(&b, id, (size_t)idlen);
    char *resp;
    uint32_t rlen;
    int st = roundtrip(NETFD(fh), MVXD_OP_UNLOCK, &b, &resp, &rlen);
    free(resp);
    return st == MVXD_ST_OK;
}

static const mvx_driver mvx_driver_lmdbnet = {
    "lmdbnet",
    net_open, net_close,
    net_read, net_write, net_del,
    net_select_begin, net_select_next, net_select_end,
    net_create, net_remove,
    net_names,
    net_write_ix, net_del_ix, net_index_select, net_index_drop,
    net_lock, net_unlock,
    .select_count = net_select_count,
    .takes_connection = 1,   /* reads .mvx-private/connections (mvx#319) */
};

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_lmdbnet : NULL;
}
