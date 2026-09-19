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

/* The session's side of mvx-msgd (mvx#226): register at logon, and answer
 * @USERNO, MSGWHO and MSGSTATUS.
 *
 * NOTHING HERE MAY EVER BE FATAL.  mvx_drv_lmdbnet.c calls mvx_fatal when the
 * daemon connection drops, and for STORAGE that is right -- losing the database
 * mid-transaction is not something a program can carry on from.  This is the
 * opposite case: a missing or broken message daemon must leave every program
 * running exactly as it runs today, because messaging is not what the program
 * is for.  Every call here degrades to a value the caller can branch on.
 *
 * Nor may it block.  Every exchange with the daemon has a 250 ms budget and a
 * timeout is simply a soft failure, so a wedged daemon cannot stall a BASIC
 * program.  And there are no signals: delivery is pull, never push, which is
 * what lets a full-screen program stay uncorrupted (mv_input treats an
 * interrupted read as end of input, so an async writer here would be a way to
 * kill programs sitting at INPUT).
 */

#include "mvx_runtime.h"
#include "mvx_ext.h"
#include "mvxmsg_proto.h"

#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define MSG_BUDGET_MS 250
#define MSG_MAXFRAME (1u * 1024 * 1024)

static int g_fd = -1;                   /* daemon connection, or -1 */
static int g_tried;                     /* asked once; do not keep retrying */
static int g_port;                      /* 0 = not registered */
static char g_sid[64], g_token[64];

/* --------------------------------------------------------- the socket */

static const char *sock_path(void) {
    const char *s = getenv("MVXMSGD");
    if (s && *s) return s;
    return "/tmp/mvx-msgd.sock";
}

static int msg_connect(void) {
    if (g_fd >= 0) return g_fd;
    if (g_tried) return -1;             /* one attempt per process */
    g_tried = 1;
    const char *path = sock_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;                      /* no daemon: not an error */
    }
    g_fd = fd;
    return fd;
}

static void msg_drop(void) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_port = 0;
}

/* ------------------------------------------------------------ framing */

typedef struct { char *d; size_t len, cap; } obuf;

static void oput(obuf *o, const void *p, size_t n) {
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 128;
        while (cap < o->len + n) cap *= 2;
        char *d = realloc(o->d, cap);
        if (!d) return;
        o->d = d;
        o->cap = cap;
    }
    if (!o->d) return;
    memcpy(o->d + o->len, p, n);
    o->len += n;
}

static void o8(obuf *o, uint8_t v) { oput(o, &v, 1); }
static void o16(obuf *o, uint16_t v) { oput(o, &v, 2); }
static void o32(obuf *o, uint32_t v) { oput(o, &v, 4); }

static void ostr(obuf *o, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xffff) n = 0xffff;
    o16(o, (uint16_t)n);
    if (n) oput(o, s, n);
}

typedef struct { const char *p; size_t left; int bad; } ibuf;

static const char *itake(ibuf *i, size_t n) {
    if (i->left < n) { i->bad = 1; return NULL; }
    const char *p = i->p;
    i->p += n;
    i->left -= n;
    return p;
}

static uint16_t i16(ibuf *i) {
    const char *p = itake(i, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

static uint32_t i32(ibuf *i) {
    const char *p = itake(i, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

static void istr(ibuf *i, char *out, size_t cap) {
    uint16_t n = i16(i);
    const char *p = itake(i, n);
    if (!p) { if (cap) out[0] = '\0'; return; }
    if (n >= cap) n = (uint16_t)(cap - 1);
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Read exactly n bytes, or give up when the budget runs out.  A short read is
   a soft failure like any other: the caller carries on without messaging. */
static int read_full(int fd, char *p, size_t n, int budget_ms) {
    size_t got = 0;
    while (got < n) {
        struct pollfd pf = {fd, POLLIN, 0};
        int r = poll(&pf, 1, budget_ms);
        if (r <= 0) return 0;           /* timeout, or interrupted: give up */
        ssize_t k = recv(fd, p + got, n - got, 0);
        if (k > 0) { got += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        return 0;
    }
    return 1;
}

/* One request, one reply.  Returns the status, or -1 if the daemon could not
   be reached -- which drops the connection so later calls degrade quietly. */
static int roundtrip(uint8_t op, obuf *req, char *resp, size_t *rlen) {
    int fd = msg_connect();
    if (fd < 0) return -1;

    uint32_t plen = (uint32_t)(1 + req->len);
    char hdr[5];
    memcpy(hdr, &plen, 4);
    hdr[4] = (char)op;
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = 5;
    iov[1].iov_base = req->d;
    iov[1].iov_len = req->len;
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = iov;
    mh.msg_iovlen = req->len ? 2 : 1;
    if (sendmsg(fd, &mh, 0) < 0) { msg_drop(); return -1; }

    char lenbuf[4];
    if (!read_full(fd, lenbuf, 4, MSG_BUDGET_MS)) { msg_drop(); return -1; }
    uint32_t rl;
    memcpy(&rl, lenbuf, 4);
    if (rl < 1 || rl > MSG_MAXFRAME) { msg_drop(); return -1; }
    char st;
    if (!read_full(fd, &st, 1, MSG_BUDGET_MS)) { msg_drop(); return -1; }
    size_t body = rl - 1;
    if (body > *rlen) { msg_drop(); return -1; }
    if (body && !read_full(fd, resp, body, MSG_BUDGET_MS)) {
        msg_drop();
        return -1;
    }
    *rlen = body;
    return (unsigned char)st;
}

/* ------------------------------------------------- who this session is */

static const char *env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return (v && *v) ? v : dflt;
}

static void this_user(char *out, size_t cap) {
    const char *u = getenv("USER");
    if (!u || !*u) u = getenv("LOGNAME");
    if (!u || !*u) {
        struct passwd *pw = getpwuid(getuid());
        u = (pw && pw->pw_name) ? pw->pw_name : "?";
    }
    snprintf(out, cap, "%s", u);
}

static void this_account(char *out, size_t cap) {
    /* The account's own name, as the prompt and the daemon namespace use it. */
    mvx_account_namespace(out, cap);
    if (!out[0]) snprintf(out, cap, "%s", env_or("MVXACCOUNT", "?"));
}

/* The port scope.  A connection profile may name one; otherwise it is the
   account, so a user's address does not change with the host they log on to. */
static void this_prefix(char *out, size_t cap) {
    const char *conn = getenv("MVXMSGCONN");
    if (conn && *conn && mvx_conn_lookup(conn, "prefix", out, cap) && out[0])
        return;
    const char *p = getenv("MVXMSGPREFIX");
    if (p && *p) { snprintf(out, cap, "%s", p); return; }
    this_account(out, cap);
}

/* ------------------------------------------------------ registration */

/* Join the port this session already holds (a verb under a TCL that
   registered), or take a new one.  Called lazily: a program that never touches
   messaging never talks to the daemon at all. */
static int msg_ensure(void) {
    if (g_port) return g_port;
    if (msg_connect() < 0) return 0;

    const char *sid = getenv("MVXMSGSESSION");
    const char *tok = getenv("MVXMSGTOKEN");
    if (sid && *sid && tok && *tok) {
        obuf req = {0, 0, 0};
        ostr(&req, sid);
        ostr(&req, tok);
        char resp[256];
        size_t rlen = sizeof resp;
        int st = roundtrip(MVXMSG_OP_ATTACH, &req, resp, &rlen);
        free(req.d);
        if (st == MVXMSG_ST_OK) {
            ibuf in = {resp, rlen, 0};
            g_port = i16(&in);
            snprintf(g_sid, sizeof g_sid, "%s", sid);
            snprintf(g_token, sizeof g_token, "%s", tok);
            return g_port;
        }
        /* The parent's lease is gone; fall through and register afresh. */
    }
    return mvx_msg_register();
}

int mvx_msg_register(void) {
    if (g_port) return g_port;
    if (msg_connect() < 0) return 0;

    char prefix[128], account[128], user[64], host[64];
    this_prefix(prefix, sizeof prefix);
    this_account(account, sizeof account);
    this_user(user, sizeof user);
    if (gethostname(host, sizeof host) != 0) snprintf(host, sizeof host, "?");
    host[sizeof host - 1] = '\0';
    const char *tty = ttyname(0);

    obuf req = {0, 0, 0};
    o16(&req, MVXMSG_PROTO_VER);
    ostr(&req, prefix);
    ostr(&req, account);
    ostr(&req, user);
    ostr(&req, host);
    ostr(&req, tty ? tty : "");
    o32(&req, (uint32_t)getpid());
    char resp[512];
    size_t rlen = sizeof resp;
    int st = roundtrip(MVXMSG_OP_HELLO, &req, resp, &rlen);
    free(req.d);
    if (st != MVXMSG_ST_OK) return 0;

    ibuf in = {resp, rlen, 0};
    g_port = i16(&in);
    istr(&in, g_sid, sizeof g_sid);
    istr(&in, g_token, sizeof g_token);
    return g_port;
}

const char *mvx_msg_session_id(void) { return g_sid; }
const char *mvx_msg_session_token(void) { return g_token; }

void mvx_msg_bye(void) {
    if (g_fd < 0 || !g_port) return;
    obuf req = {0, 0, 0};
    char resp[64];
    size_t rlen = sizeof resp;
    roundtrip(MVXMSG_OP_BYE, &req, resp, &rlen);
    free(req.d);
    msg_drop();
}

/* ------------------------------------------------- BASIC-facing answers */

/* @USERNO — this session's port, or 0 when there is no registry. */
int64_t mvx_msg_port(void) { return msg_ensure(); }

/* MSGWHO(scope): the roster, one session per attribute, fields by value mark:
   port, user, account, host, tty, logged-on time.  Empty when unavailable, so
   a caller can tell "nobody" from "cannot ask" by checking MSGSTATUS. */
void mvx_msg_who(mv_value *out, int64_t scope) {
    mv_set_str(out, "", 0);
    if (!msg_ensure()) return;

    obuf req = {0, 0, 0};
    o16(&req, (uint16_t)(scope ? MVXMSG_SCOPE_SYSTEM : MVXMSG_SCOPE_LOCAL));
    size_t cap = 64 * 1024;
    char *resp = malloc(cap);
    if (!resp) { free(req.d); return; }
    size_t rlen = cap;
    int st = roundtrip(MVXMSG_OP_WHO, &req, resp, &rlen);
    free(req.d);
    if (st != MVXMSG_ST_OK) { free(resp); return; }

    ibuf in = {resp, rlen, 0};
    uint32_t n = i32(&in);
    char *acc = NULL;
    size_t alen = 0, acap = 0;
    for (uint32_t i = 0; i < n && !in.bad; i++) {
        uint16_t port = i16(&in);
        char user[64], account[128], host[64], tty[64];
        istr(&in, user, sizeof user);
        istr(&in, account, sizeof account);
        istr(&in, host, sizeof host);
        istr(&in, tty, sizeof tty);
        uint32_t since = i32(&in);
        char line[512];
        int len = snprintf(line, sizeof line, "%s%u\xfd%s\xfd%s\xfd%s\xfd%s\xfd%u",
                           i ? "\xfe" : "", port, user, account, host,
                           tty[0] ? tty : "-", since);
        if (len < 0) break;
        if (alen + (size_t)len + 1 > acap) {
            size_t want = acap ? acap * 2 : 1024;
            while (want < alen + (size_t)len + 1) want *= 2;
            char *g = realloc(acc, want);
            if (!g) break;
            acc = g;
            acap = want;
        }
        memcpy(acc + alen, line, (size_t)len);
        alen += (size_t)len;
    }
    if (acc) mv_set_str(out, acc, (int64_t)alen);
    free(acc);
    free(resp);
}

/* MSGSTATUS(): state, transport, port, prefix — by value mark.  The first
   field is what a program branches on. */
void mvx_msg_status(mv_value *out) {
    /* Split around the value marks deliberately: "\xfd0" would lex as one
       out-of-range hex escape, not a mark followed by a zero. */
    static const char unavail[] = "unavailable\xfd" "\xfd" "0" "\xfd";
    if (!msg_ensure()) {
        mv_set_str(out, unavail, (int64_t)(sizeof unavail - 1));
        return;
    }
    obuf req = {0, 0, 0};
    char resp[512];
    size_t rlen = sizeof resp;
    int st = roundtrip(MVXMSG_OP_STAT, &req, resp, &rlen);
    free(req.d);
    if (st != MVXMSG_ST_OK) {
        mv_set_str(out, unavail, (int64_t)(sizeof unavail - 1));
        return;
    }
    ibuf in = {resp, rlen, 0};
    char transport[64], prefix[128];
    istr(&in, transport, sizeof transport);
    uint16_t state = i16(&in);
    uint16_t port = i16(&in);
    istr(&in, prefix, sizeof prefix);
    const char *sname = state == MVXMSG_STATE_UP ? "up"
                      : state == MVXMSG_STATE_CONNECTING ? "connecting"
                      : "degraded";
    char line[512];
    int len = snprintf(line, sizeof line, "%s\xfd%s\xfd%u\xfd%s",
                       sname, transport, port, prefix);
    mv_set_str(out, line, len < 0 ? 0 : len);
}

/* ------------------------------------------------- extension functions */

static void ext_msgwho(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                       mv_value **argv) {
    (void)ctx;
    int64_t scope = 0;
    if (argc >= 1) {
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(argv[0], nb, sizeof nb, &p);
        if (n >= 6 && (p[0] == 'S' || p[0] == 's')) scope = 1;
    }
    mvx_msg_who(ret, scope);
}

static void ext_msgstatus(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                          mv_value **argv) {
    (void)ctx; (void)argc; (void)argv;
    mvx_msg_status(ret);
}

static const mvx_extfn msg_fns[] = {
    {"MSGWHO", 0, 1, ext_msgwho},
    {"MSGSTATUS", 0, 0, ext_msgstatus},
};

static const mvx_ext msg_ext = {"msg", 2, msg_fns};

const mvx_ext *mvx_msg_builtin(void) { return &msg_ext; }
