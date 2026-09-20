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

/* mvx-msgd — the per-host session registry (mvx#226).
 *
 * MVX had no idea who was logged on.  WHO printed $USER and the account path,
 * there was no port number, and no session could name another.  This daemon
 * holds the roster: one small port number per session, allocated on HELLO and
 * freed when the connection drops.
 *
 * THE CONNECTION IS THE LEASE.  A session holds its socket open for as long as
 * it lives, so a clean exit, a killed process, a dropped SSH and a `docker kill`
 * all deregister it with no cleanup step and no stale entries.  That is why
 * this is a fork of daemon/mvxd.c's skeleton (poll loop, buffered framed I/O,
 * conn_reap, g_stop shutdown) rather than something new: mvxd already leases
 * record locks exactly this way, and the pattern is proven.
 *
 * A PORT IS SCOPED BY A PREFIX, which defaults to the account name, so a user
 * keeps the same address whichever host they log on to.  Allocation is the
 * lowest free number from portbase, which is deliberate: it makes the roster
 * reproducible, and therefore testable.
 *
 * Messaging proper (inboxes, MSG, the wall broadcast) and the pluggable
 * transport come next; this stage is identity alone, and it has to earn its
 * place on its own -- WHO and @USERNO are worth having whether or not a message
 * is ever sent.
 */

#include "mvxmsg_proto.h"
#include "mvx_msgdrv.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* One connection per session, plus one per attached child.  mvxd's 64 is a
   database daemon's number -- a handful of application processes; here a
   forty-user host with a verb running in each would exceed it, and the
   symptom would be a session that silently cannot register. */
#ifdef __APPLE__
#define MVX_DLSUFFIX ".dylib"
#else
#define MVX_DLSUFFIX ".so"
#endif

#define MAX_CONNS 512
#define MAX_FRAME (1u * 1024 * 1024)
#define MAX_TEXT 256
#define PORT_MAX 4096

static volatile sig_atomic_t g_stop;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static const char *g_prefix = "";       /* port scope; default: the account */
static int g_portbase = 1;
static const char *g_transport = "loop";
static const char *g_loc = "";          /* @profile or an inline address */
static char g_origin[128];              /* this daemon, in every record it sends */

/* ------------------------------------------------------------- roster */

/* THE INBOX IS BOUNDED, AND DROPS THE OLDEST.  An unbounded queue is a
   memory-exhaustion vector available to any logged-on user, and refusing new
   messages when full is the wrong end to drop: for a message the newest is
   the one that matters.  The count of what was lost is kept, so a program can
   say so honestly rather than quietly showing less than was sent. */
#define INBOX_MSGS 256
#define INBOX_BYTES (64 * 1024)

/* Per-port rate limit: a token bucket refilled at RATE per second, capped at
   BURST.  Without one, "message every port" in a loop is a denial of service
   any user can run. */
#define RATE_PER_SEC 10
#define RATE_BURST 30

/* Adjustable, so the suite can test what the inbox does when it OVERFLOWS --
   which needs three hundred messages in a moment, and is a different question
   from whether the limiter works.  A rule that cannot be turned off cannot be
   tested around. */
static double g_rate = RATE_PER_SEC;
static double g_burst = RATE_BURST;

typedef struct session {
    int fd;                             /* the lease: this connection */
    int port;
    char sid[64];                       /* host:pid:epoch-ms, never reused */
    char token[33];                     /* a child ATTACHes with this */
    char user[64], account[128], host[64], tty[64], prefix[128];
    long pid;
    time_t since;

    int mode;                           /* MVXMSG_MODE_* */
    char *inbox[INBOX_MSGS];            /* ring of whole message records */
    size_t inlen[INBOX_MSGS];
    int head, count;                    /* head = oldest */
    size_t bytes;
    uint32_t dropped;                   /* lost to overflow, since logon */

    double tokens;                      /* rate limit */
    time_t tokens_at;

    struct session *next;
} session;

static session *g_sessions;

/* A child that ATTACHed to its parent's port.  Its connection is NOT a lease:
   dropping it leaves the parent's session alone, which is the whole point --
   a verb finishing must not log its shell off. */
typedef struct attach {
    int fd;
    char sid[64];
    struct attach *next;
} attach;

static attach *g_attached;

static session *sess_by_fd(int fd) {
    for (session *s = g_sessions; s; s = s->next)
        if (s->fd == fd) return s;
    return NULL;
}

static session *sess_by_sid(const char *sid) {
    for (session *s = g_sessions; s; s = s->next)
        if (strcmp(s->sid, sid) == 0) return s;
    return NULL;
}

/* The lowest free port at or above portbase, within this prefix.  Lowest-free
   rather than next-highest so a fresh daemon always hands out 1, 2, 3 --
   golden-file tests depend on that, and so does anyone who reads a roster. */
static int port_alloc(const char *prefix) {
    for (int p = g_portbase; p < g_portbase + PORT_MAX; p++) {
        int taken = 0;
        for (session *s = g_sessions; s; s = s->next)
            if (s->port == p && strcmp(s->prefix, prefix) == 0) taken = 1;
        if (!taken) return p;
    }
    return -1;
}

/* The session this connection speaks for: its own, or the one it attached to.
   Everything that answers a question about "me" must use this, or a verb gets
   a different answer from the shell that ran it. */
static session *sess_for_fd(int fd) {
    session *s = sess_by_fd(fd);
    if (s) return s;
    for (attach *a = g_attached; a; a = a->next)
        if (a->fd == fd) return sess_by_sid(a->sid);
    return NULL;
}

static void attach_drop(int fd) {
    for (attach **pp = &g_attached; *pp;) {
        if ((*pp)->fd == fd) {
            attach *dead = *pp;
            *pp = dead->next;
            free(dead);
            continue;
        }
        pp = &(*pp)->next;
    }
}

static void sess_drop(int fd) {
    for (session **pp = &g_sessions; *pp;) {
        if ((*pp)->fd == fd) {
            session *dead = *pp;
            *pp = dead->next;
            for (int i = 0; i < dead->count; i++)
                free(dead->inbox[(dead->head + i) % INBOX_MSGS]);
            free(dead);
            continue;
        }
        pp = &(*pp)->next;
    }
}

/* Release everything a dropped connection held.  An ATTACHed child has no
   session of its own, so dropping it leaves its parent's port alone. */
static void conn_reap(int fd) {
    sess_drop(fd);
    attach_drop(fd);
}

static void rand_hex(char *out, size_t n) {
    static const char hex[] = "0123456789abcdef";
    unsigned char buf[64];
    size_t want = n / 2;
    if (want > sizeof buf) want = sizeof buf;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(buf, 1, want, f) != want)
            for (size_t i = 0; i < want; i++) buf[i] = (unsigned char)rand();
        fclose(f);
    } else {
        for (size_t i = 0; i < want; i++) buf[i] = (unsigned char)rand();
    }
    size_t i = 0;
    for (; i < want && i * 2 + 1 < n; i++) {
        out[i * 2] = hex[buf[i] >> 4];
        out[i * 2 + 1] = hex[buf[i] & 15];
    }
    out[i * 2] = '\0';
}

/* ------------------------------------------------------- transport */

static const mvx_msgdrv *g_drv;
static mvx_msgconn *g_conn;
static unsigned g_caps;                 /* what the driver admits to */
static unsigned g_nocaps;               /* forced off, for testing */

/* Topics, and only these four shapes.  Every future backend has to reproduce
   exactly this much, which is the point of keeping it small:
     mvx/<prefix>/port/<port>/msg     one port
     mvx/<prefix>/wall                every port
     mvx/<prefix>/presence/<port>     the roster (retained where possible)
     mvx/<prefix>/ctl/<daemon>        daemon to daemon
*/
static void topic_port(char *out, size_t cap, const char *prefix, int port) {
    snprintf(out, cap, "mvx/%s/port/%d/msg", prefix, port);
}

static void topic_wall(char *out, size_t cap, const char *prefix) {
    snprintf(out, cap, "mvx/%s/wall", prefix);
}

/* --------------------------------------------------------- delivery */

/* Put one record in a session's inbox.  Returns 1 if it was queued.
   MODE OFF discards -- except a wall or a system message, which a user may
   not switch off: "the system is going down in five minutes" is not theirs
   to suppress.  That is a deliberate departure from classic behaviour. */
static int inbox_put(session *s, int class, const char *rec, size_t len) {
    if (s->mode == MVXMSG_MODE_OFF &&
        class != MVXMSG_CLASS_WALL && class != MVXMSG_CLASS_SYSTEM)
        return 0;

    char *copy = malloc(len + 1);
    if (!copy) return 0;
    memcpy(copy, rec, len);
    copy[len] = '\0';

    while (s->count >= INBOX_MSGS ||
           (s->bytes + len > INBOX_BYTES && s->count > 0)) {
        char *old = s->inbox[s->head];
        s->bytes -= s->inlen[s->head];
        free(old);
        s->head = (s->head + 1) % INBOX_MSGS;
        s->count--;
        s->dropped++;
    }
    int slot = (s->head + s->count) % INBOX_MSGS;
    s->inbox[slot] = copy;
    s->inlen[slot] = len;
    s->bytes += len;
    s->count++;
    return 1;
}

/* One send's worth of tokens, or 0 when the port has spent its budget. */
static int rate_ok(session *s) {
    time_t now = time(NULL);
    if (s->tokens_at == 0) { s->tokens = g_burst; s->tokens_at = now; }
    double elapsed = (double)(now - s->tokens_at);
    if (elapsed > 0) {
        s->tokens += elapsed * g_rate;
        if (s->tokens > g_burst) s->tokens = g_burst;
        s->tokens_at = now;
    }
    if (s->tokens < 1.0) return 0;
    s->tokens -= 1.0;
    return 1;
}

/* Does this session match the classic target form?
     *            every logged-on port
     !7, !5-9     a port, or a range
     @fred        a user, wherever they are logged on
     SALES,PAY    those accounts
   Ports absent from the roster are simply skipped, which is what classic Pick
   did with a logged-off line: there is nowhere to put the message. */
static int target_matches(const session *s, const char *target) {
    if (!target || !*target) return 0;
    if (strcmp(target, "*") == 0) return 1;

    if (target[0] == '!') {
        long lo = 0, hi = 0;
        const char *dash = strchr(target + 1, '-');
        lo = strtol(target + 1, NULL, 10);
        hi = dash ? strtol(dash + 1, NULL, 10) : lo;
        if (hi < lo) { long t = lo; lo = hi; hi = t; }
        return s->port >= lo && s->port <= hi;
    }

    if (target[0] == '@') return strcmp(s->user, target + 1) == 0;

    /* one or more account names, comma separated */
    const char *p = target;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n == strlen(s->account) && strncmp(p, s->account, n) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* A message arriving FROM the transport, whichever transport it is.  This is
   the only path into a local inbox: delivery does not short-circuit for a
   message that happens to have come from this host, because then the local
   and remote cases would be different code and only one of them would be
   exercised by the tests. */
static void on_message(void *user, const mvx_msgmsg *m) {
    (void)user;
    if (!m || !m->topic) return;

    /* mvx/<prefix>/port/<port>/msg  or  mvx/<prefix>/wall */
    char prefix[128];
    int port = 0, wall = 0;
    const char *p = m->topic;
    if (strncmp(p, "mvx/", 4) != 0) return;
    p += 4;
    const char *slash = strchr(p, '/');
    if (!slash) return;
    size_t plen = (size_t)(slash - p);
    if (plen >= sizeof prefix) return;
    memcpy(prefix, p, plen);
    prefix[plen] = '\0';
    p = slash + 1;
    if (strcmp(p, "wall") == 0) wall = 1;
    else if (strncmp(p, "port/", 5) == 0) port = atoi(p + 5);
    else return;

    /* OUR OWN PUBLISH COMING BACK.  A service that echoes to its own
       subscribers (MQTT does) would deliver a local message twice, since the
       daemon already put it in the inbox before publishing.  The origin field
       is the record's last attribute. */
    if (g_origin[0] && m->plen > 0) {
        size_t olen = strlen(g_origin);
        if ((size_t)m->plen > olen &&
            memcmp(m->payload + m->plen - olen, g_origin, olen) == 0 &&
            (unsigned char)m->payload[m->plen - olen - 1] == 0xfe)
            return;
    }

    int class = MVXMSG_CLASS_STATUS;
    /* attribute 2 of the record is the class */
    const char *am = memchr(m->payload, '\xfe', (size_t)m->plen);
    if (am) class = atoi(am + 1) & 0x0f;

    for (session *s = g_sessions; s; s = s->next) {
        if (strcmp(s->prefix, prefix) != 0) continue;
        if (!wall && s->port != port) continue;
        inbox_put(s, class, m->payload, (size_t)m->plen);
    }
}

/* ------------------------------------------------------------ plumbing
   Buffered, non-blocking framed I/O, as daemon/mvxd.c does it: one poll loop,
   so a blocking read or write on one client would stall every other. */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

typedef struct {
    int fd;
    char *rbuf;
    size_t rlen, rcap;
    char *wbuf;
    size_t wpos, wlen, wcap;
} conn;

static void conn_free(conn *c) {
    free(c->rbuf);
    free(c->wbuf);
    c->rbuf = c->wbuf = NULL;
    c->rlen = c->rcap = c->wpos = c->wlen = c->wcap = 0;
}

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

static int conn_flush(conn *c) {
    while (c->wpos < c->wlen) {
        ssize_t r = send(c->fd, c->wbuf + c->wpos, c->wlen - c->wpos, 0);
        if (r > 0) { c->wpos += (size_t)r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        if (r < 0 && errno == EINTR) continue;
        return 0;
    }
    c->wpos = c->wlen = 0;
    return 1;
}

typedef struct { char *d; size_t len, cap; } outbuf;

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

static void ostr(outbuf *o, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 0xffff) n = 0xffff;
    o16(o, (uint16_t)n);
    if (n) oput(o, s, n);
}

typedef struct { const char *p; size_t left; int bad; } inbuf;

static const char *itake(inbuf *i, size_t n) {
    if (i->left < n) { i->bad = 1; return NULL; }
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

/* A request string, copied into a bounded buffer: the payload is not
   NUL-terminated and must never be treated as though it were. */
static void istr(inbuf *i, char *out, size_t cap) {
    uint16_t n = i16(i);
    const char *p = itake(i, n);
    if (!p) { out[0] = '\0'; return; }
    if (n >= cap) n = (uint16_t)(cap - 1);
    memcpy(out, p, n);
    out[n] = '\0';
}

/* --------------------------------------------------------------- ops */

static void handle(int fd, uint8_t op, inbuf *in, outbuf *out, uint8_t *status) {
    *status = MVXMSG_ST_ERR;

    switch (op) {
    case MVXMSG_OP_HELLO: {
        uint16_t ver = i16(in);
        char prefix[128], account[128], user[64], host[64], tty[64];
        istr(in, prefix, sizeof prefix);
        istr(in, account, sizeof account);
        istr(in, user, sizeof user);
        istr(in, host, sizeof host);
        istr(in, tty, sizeof tty);
        uint32_t pid = i32(in);
        if (in->bad || ver != MVXMSG_PROTO_VER) return;

        /* The prefix is the port's scope and defaults to the account, so the
           same user has the same address wherever they log on.  A daemon-wide
           prefix overrides it, for a site that wants one namespace. */
        const char *scope = g_prefix[0] ? g_prefix
                          : (prefix[0] ? prefix : account);

        session *s = sess_by_fd(fd);
        if (s) { *status = MVXMSG_ST_OK; }   /* HELLO twice: keep the port */
        else {
            int port = port_alloc(scope);
            if (port < 0) return;
            s = calloc(1, sizeof *s);
            if (!s) exit(70);
            s->fd = fd;
            s->port = port;
            s->pid = (long)pid;
            s->since = time(NULL);
            snprintf(s->prefix, sizeof s->prefix, "%s", scope);
            snprintf(s->account, sizeof s->account, "%s", account);
            snprintf(s->user, sizeof s->user, "%s", user);
            snprintf(s->host, sizeof s->host, "%s", host);
            snprintf(s->tty, sizeof s->tty, "%s", tty);
            snprintf(s->sid, sizeof s->sid, "%s:%lu:%lld", host[0] ? host : "?",
                     (unsigned long)pid, (long long)s->since);
            rand_hex(s->token, sizeof s->token);
            s->next = g_sessions;
            g_sessions = s;

            /* Subscribe for this port and for the prefix's wall.  A driver
               with wildcards needs only the patterns; one without needs the
               exact topics, which is why this asks rather than assumes. */
            char topic[256];
            if (g_drv && g_conn) {
                topic_port(topic, sizeof topic, s->prefix, s->port);
                g_drv->subscribe(g_conn, topic);
                topic_wall(topic, sizeof topic, s->prefix);
                g_drv->subscribe(g_conn, topic);
            }
            *status = MVXMSG_ST_OK;
        }
        o16(out, (uint16_t)s->port);
        ostr(out, s->sid);
        ostr(out, s->token);
        return;
    }

    case MVXMSG_OP_ATTACH: {
        char sid[64], token[64];
        istr(in, sid, sizeof sid);
        istr(in, token, sizeof token);
        if (in->bad) return;
        session *s = sess_by_sid(sid);
        /* A child attaches to the port its PARENT holds, and proves it with a
           token it was handed in its environment.  It never attaches to
           somebody else's port: that is the part of D3's dev-att this does not
           reproduce. */
        if (!s || strcmp(s->token, token) != 0) {
            *status = MVXMSG_ST_DENIED;
            return;
        }
        attach_drop(fd);
        attach *a = calloc(1, sizeof *a);
        if (!a) exit(70);
        a->fd = fd;
        snprintf(a->sid, sizeof a->sid, "%s", sid);
        a->next = g_attached;
        g_attached = a;
        o16(out, (uint16_t)s->port);
        *status = MVXMSG_ST_OK;
        return;
    }

    case MVXMSG_OP_BYE:
        sess_drop(fd);
        *status = MVXMSG_ST_OK;
        return;

    case MVXMSG_OP_WHO: {
        uint16_t scope = i16(in);
        if (in->bad) return;
        session *me = sess_for_fd(fd);
        const char *want = me ? me->prefix : (g_prefix[0] ? g_prefix : NULL);
        uint32_t count = 0;
        size_t count_pos = out->len;
        o32(out, 0);
        /* BY PORT, ascending.  The list is newest-first, and a roster that
           reorders itself every time somebody logs on is unreadable for a
           person and untestable for the suite. */
        for (int p = g_portbase; p < g_portbase + PORT_MAX; p++) {
          for (session *s = g_sessions; s; s = s->next) {
            if (s->port != p) continue;
            if (want && strcmp(s->prefix, want) != 0) continue;
            o16(out, (uint16_t)s->port);
            ostr(out, s->user);
            ostr(out, s->account);
            ostr(out, s->host);
            ostr(out, s->tty);
            o32(out, (uint32_t)s->since);
            count++;
          }
        }
        memcpy(out->d + count_pos, &count, 4);
        /* Every roster this daemon holds is a local one until a transport
           carries presence; say so rather than implying otherwise. */
        o16(out, MVXMSG_SCOPE_LOCAL);
        (void)scope;
        *status = MVXMSG_ST_OK;
        return;
    }

    case MVXMSG_OP_SEND: {
        char target[256], text[1024], payload[2048];
        istr(in, target, sizeof target);
        uint16_t class = i16(in);
        istr(in, text, sizeof text);
        istr(in, payload, sizeof payload);
        if (in->bad) return;

        session *me = sess_for_fd(fd);
        if (!me) return;                /* not registered: nothing to send as */
        if (!rate_ok(me)) { *status = MVXMSG_ST_BUSY; return; }

        /* THE RECORD IS BUILT HERE, from the roster, not from what the sender
           claims to be.  A sender that could write its own from-port and
           from-user would make every message unattributable. */
        /* A message sent to every port IS a wall message, whatever class the
           sender asked for -- otherwise the receiver cannot tell a broadcast
           from a note, and MODE OFF would be deciding one thing while the
           record said another.  The flags (bell, signed) are the sender's and
           are kept. */
        int wall = (class == MVXMSG_CLASS_WALL) || strcmp(target, "*") == 0;
        unsigned eff = wall ? ((class & ~0x0fu) | MVXMSG_CLASS_WALL)
                            : (unsigned)class;

        time_t now = time(NULL);
        char rec[4096];
        int n = snprintf(rec, sizeof rec,
                         "1\xfe%u\xfe%d\xfe%s\xfe%s\xfe%s\xfe%lld\xfe%s\xfe%s\xfe%s",
                         eff, me->port, me->user, me->account,
                         me->host, (long long)now, text, payload, g_origin);
        if (n < 0) return;
        if ((size_t)n >= sizeof rec) n = (int)sizeof rec - 1;

        /* RESOLVED HERE, AGAINST THE ROSTER, THEN PUBLISHED PER PORT.  The
           alternative -- let the service fan out by wildcard -- would make
           MQTT's subscription model load-bearing, and would leave MSG unable
           to say how many ports it reached.  Ports that are not logged on are
           simply absent from the roster, which is how a message to a
           logged-off line comes to be dropped.

           The count is PORTS ADDRESSED, not inboxes that accepted.  Across a
           service a sender cannot learn that a receiver had messages switched
           off, so counting acceptances would give an answer that changed with
           the transport -- exactly what this contract exists to prevent. */
        /* LOCAL FIRST, THEN PUBLISHED.  Two users on one host must be able to
           message each other with the broker switched off -- "degraded, not
           dead" is the whole promise -- so a local session is delivered to
           directly, and the publish is for the other hosts.  Our own echo
           comes back and is ignored by its origin field (on_message), which
           is what stops a local message arriving twice. */
        uint32_t delivered = 0;
        char topic[256];
        for (session *t = g_sessions; t; t = t->next) {
            if (strcmp(t->prefix, me->prefix) != 0) continue;
            if (!wall && !target_matches(t, target)) continue;
            inbox_put(t, (int)(eff & 0x0fu), rec, (size_t)n);
            delivered++;
        }
        if (wall) {
            topic_wall(topic, sizeof topic, me->prefix);
            g_drv->publish(g_conn, topic, rec, n, 0, 0, 250);
        } else if (target[0] == '!') {
            /* An explicit port may be on another host.  The count stays what
               was resolved HERE: this daemon cannot know what is logged on
               elsewhere until presence arrives, and guessing would have MSG
               report a delivery that never happened. */
            long lo = strtol(target + 1, NULL, 10);
            const char *dash = strchr(target + 1, '-');
            long hi = dash ? strtol(dash + 1, NULL, 10) : lo;
            if (hi < lo) { long t2 = lo; lo = hi; hi = t2; }
            for (long p2 = lo; p2 <= hi && p2 - lo < 64; p2++) {
                topic_port(topic, sizeof topic, me->prefix, (int)p2);
                g_drv->publish(g_conn, topic, rec, n, 0, 0, 250);
            }
        }
        o32(out, delivered);
        *status = MVXMSG_ST_OK;
        return;
    }

    case MVXMSG_OP_PEEK: {
        session *s = sess_for_fd(fd);
        if (!s) return;
        o32(out, (uint32_t)s->count);
        o32(out, s->dropped);
        *status = MVXMSG_ST_OK;
        return;
    }

    case MVXMSG_OP_RECV: {
        uint16_t max = i16(in);
        if (in->bad) return;
        session *s = sess_for_fd(fd);
        if (!s) return;
        if (max == 0) max = 1;
        uint32_t count = 0;
        size_t count_pos = out->len;
        o32(out, 0);
        while (s->count > 0 && count < max) {
            char *m = s->inbox[s->head];
            size_t len = s->inlen[s->head];
            o16(out, (uint16_t)(len > 0xffff ? 0xffff : len));
            oput(out, m, len > 0xffff ? 0xffff : len);
            free(m);
            s->inbox[s->head] = NULL;
            s->bytes -= len;
            s->head = (s->head + 1) % INBOX_MSGS;
            s->count--;
            count++;
        }
        memcpy(out->d + count_pos, &count, 4);
        *status = MVXMSG_ST_OK;
        return;
    }

    case MVXMSG_OP_MODE: {
        uint16_t mode = i16(in);
        if (in->bad) return;
        session *s = sess_for_fd(fd);
        if (!s) return;
        o16(out, (uint16_t)s->mode);
        if (mode <= MVXMSG_MODE_DEFER) s->mode = mode;
        *status = MVXMSG_ST_OK;
        return;
    }

    case MVXMSG_OP_STAT: {
        session *s = sess_for_fd(fd);
        ostr(out, g_drv ? g_drv->name : g_transport);
        /* ASK THE DRIVER, do not assume.  A broker that has gone away leaves
           local messaging working, and a program that wants to say so needs
           to be able to tell the difference. */
        int dstate = g_drv && g_drv->state ? g_drv->state(g_conn)
                                           : MVX_MSGDRV_UP;
        o16(out, (uint16_t)(dstate == MVX_MSGDRV_UP ? MVXMSG_STATE_UP
                          : dstate == MVX_MSGDRV_CONNECTING
                              ? MVXMSG_STATE_CONNECTING
                              : MVXMSG_STATE_DOWN));
        o16(out, (uint16_t)(s ? s->port : 0));
        ostr(out, s ? s->prefix : g_prefix);
        *status = MVXMSG_ST_OK;
        return;
    }
    }
}

static int conn_dispatch(conn *c) {
    for (;;) {
        if (c->rlen < 4) return 1;
        uint32_t plen;
        memcpy(&plen, c->rbuf, 4);
        if (plen < 1 || plen > MAX_FRAME) return 0;
        if (c->rlen < 4 + (size_t)plen) return 1;
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

static void usage(void) {
    fprintf(stderr,
            "usage: mvx-msgd (-s unix-socket | -p port) [-t transport]\n"
            "                [-x prefix] [-b portbase] [-r per-sec] [-k burst]\n"
            "                [-c @profile] [-X nocaps=retain,will,...]\n");
}

int main(int argc, char **argv) {
    const char *sockpath = NULL;
    int port = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) sockpath = argv[++i];
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) g_transport = argv[++i];
        else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) g_prefix = argv[++i];
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) g_portbase = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) g_rate = atof(argv[++i]);
        else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) g_burst = atof(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) g_loc = argv[++i];
        else if (strcmp(argv[i], "-X") == 0 && i + 1 < argc) {
            /* -X nocaps=retain,will -- pretend the transport cannot do these,
               so the daemon's compensation for a backend that lacks them is
               exercised on purpose rather than first meeting daylight when a
               second backend arrives. */
            const char *v = argv[++i];
            if (strncmp(v, "nocaps=", 7) == 0) {
                v += 7;
                while (*v) {
                    if (strncmp(v, "retain", 6) == 0) g_nocaps |= MVX_MSGCAP_RETAIN;
                    else if (strncmp(v, "will", 4) == 0) g_nocaps |= MVX_MSGCAP_WILL;
                    else if (strncmp(v, "persist", 7) == 0) g_nocaps |= MVX_MSGCAP_PERSIST;
                    else if (strncmp(v, "wildcard", 8) == 0) g_nocaps |= MVX_MSGCAP_WILDCARD;
                    else if (strncmp(v, "loopback", 8) == 0) g_nocaps |= MVX_MSGCAP_LOOPBACK;
                    const char *comma = strchr(v, ',');
                    if (!comma) break;
                    v = comma + 1;
                }
            }
        }
        else { usage(); return 2; }
    }
    if (!sockpath && port == 0) { usage(); return 2; }
    if (g_portbase < 0) g_portbase = 1;
    if (strcmp(g_transport, "loop") == 0) {
        g_drv = mvx_msgdrv_loop();      /* built in: always available */
    } else {
        /* A transport is a shared library beside the storage drivers, found
           the same way they are, and loaded only when a site asks for it by
           name.  So a build without libmosquitto is not a build without
           messaging -- it is a build without MQTT. */
        char path[1024];
        const char *dir = getenv("MVXMSGDRIVERS");
        if (!dir || !*dir) dir = getenv("MVXDRIVERS");
        if (dir && *dir)
            snprintf(path, sizeof path, "%s/libmvxmsg_%s%s", dir, g_transport,
                     MVX_DLSUFFIX);
        else
            snprintf(path, sizeof path, "libmvxmsg_%s%s", g_transport,
                     MVX_DLSUFFIX);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            fprintf(stderr, "mvx-msgd: no transport '%s': %s\n",
                    g_transport, dlerror());
            return 2;
        }
        mvx_msgdrv_entry_fn entry =
            (mvx_msgdrv_entry_fn)dlsym(h, "mvx_msgdrv_entry");
        g_drv = entry ? entry(MVX_MSGDRV_ABI) : NULL;
        if (!g_drv) {
            fprintf(stderr, "mvx-msgd: %s is not a transport for ABI %d\n",
                    path, MVX_MSGDRV_ABI);
            return 2;
        }
    }

    /* A CLIENT ID MUST BE UNIQUE ACROSS THE SERVICE.  MQTT takes a repeated
       one as the same client reconnecting and closes the older session, so
       two daemons sharing a name kick each other off in a loop -- and the
       symptom is not an error but silence: each one looks connected and
       neither receives anything.  Host and pid make it unique without
       needing configuration. */
    char clientid[128], selfhost[64];
    if (gethostname(selfhost, sizeof selfhost) != 0)
        snprintf(selfhost, sizeof selfhost, "host");
    selfhost[sizeof selfhost - 1] = '\0';
    snprintf(clientid, sizeof clientid, "mvx-msgd-%s-%ld", selfhost,
             (long)getpid());
    snprintf(g_origin, sizeof g_origin, "%s", clientid);

    char derr[256] = {0};
    g_conn = g_drv->connect(g_loc, clientid, NULL, NULL, 0, derr, sizeof derr);
    if (!g_conn) {
        fprintf(stderr, "mvx-msgd: transport %s: %s\n", g_drv->name,
                derr[0] ? derr : "could not connect");
        return 1;
    }
    /* WHAT THE DRIVER SAYS IT CAN DO, minus anything forced off for a test.
       The daemon reads these and compensates; it never asks which backend it
       is talking to. */
    g_caps = g_drv->caps & ~g_nocaps;

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);

    int lfd;
    if (sockpath) {
        lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a = {0};
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof a.sun_path, "%s", sockpath);
        unlink(sockpath);
        if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
            listen(lfd, 16) != 0) {
            fprintf(stderr, "mvx-msgd: cannot listen on %s\n", sockpath);
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
            fprintf(stderr, "mvx-msgd: cannot listen on port %d\n", port);
            return 1;
        }
    }
    fprintf(stderr, "mvx-msgd: %s transport (caps %#x), ports from %d, on %s\n",
            g_drv->name, g_caps, g_portbase, sockpath ? sockpath : "tcp");

    set_nonblock(lfd);
    struct pollfd fds[MAX_CONNS + 1];
    conn cs[MAX_CONNS + 1];
    memset(cs, 0, sizeof cs);
    int nfds = 1;
    fds[0].fd = lfd;
    fds[0].events = POLLIN;

    for (;;) {
        if (g_stop) break;
        for (int i = 1; i < nfds; i++)
            fds[i].events =
                (short)(POLLIN | (cs[i].wpos < cs[i].wlen ? POLLOUT : 0));

        /* The transport joins this loop rather than running a thread of its
           own: the roster and the inboxes are in one hand that way.  A driver
           with no descriptor (loop) simply asks for a timer. */
        int dn = 0;
        if (g_drv->fds) {
            int dfds[8];
            short devs[8];
            dn = g_drv->fds(g_conn, dfds, devs, 8);
            for (int i = 0; i < dn && nfds + i <= MAX_CONNS; i++) {
                fds[nfds + i].fd = dfds[i];
                fds[nfds + i].events = devs[i];
                fds[nfds + i].revents = 0;
            }
        }
        int wait_ms = g_drv->timeout_ms ? g_drv->timeout_ms(g_conn) : -1;

        if (poll(fds, (nfds_t)(nfds + dn), wait_ms) < 0) {
            if (errno == EINTR) { if (g_stop) break; continue; }
            break;
        }

        /* Whatever arrived on the transport goes into the local inboxes. */
        if (g_drv->pump && !g_drv->pump(g_conn, on_message, NULL)) {
            fprintf(stderr, "mvx-msgd: transport %s disconnected\n",
                    g_drv->name);
        }

        if (fds[0].revents & POLLIN) {
            int c = accept(lfd, NULL, NULL);
            if (c >= 0) {
                if (nfds <= MAX_CONNS) {
                    set_nonblock(c);
                    fds[nfds].fd = c;
                    fds[nfds].events = POLLIN;
                    fds[nfds].revents = 0;
                    memset(&cs[nfds], 0, sizeof cs[nfds]);
                    cs[nfds].fd = c;
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
                    dead = 1;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK &&
                           errno != EINTR) {
                    dead = 1;
                }
            }

            if (!dead && !conn_flush(cn)) dead = 1;
            if ((fds[i].revents & POLLHUP) && cn->wpos >= cn->wlen) dead = 1;

            if (dead) {
                conn_reap(fd);          /* the lease ends with the connection */
                close(fd);
                conn_free(cn);
                fds[i] = fds[nfds - 1];
                cs[i] = cs[nfds - 1];
                nfds--;
                i--;
            }
        }
    }

    if (g_drv && g_conn) g_drv->disconnect(g_conn);
    if (sockpath) unlink(sockpath);
    return 0;
}
