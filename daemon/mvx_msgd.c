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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* One connection per session, plus one per attached child.  mvxd's 64 is a
   database daemon's number -- a handful of application processes; here a
   forty-user host with a verb running in each would exceed it, and the
   symptom would be a session that silently cannot register. */
#define MAX_CONNS 512
#define MAX_FRAME (1u * 1024 * 1024)
#define MAX_TEXT 256
#define PORT_MAX 4096

static volatile sig_atomic_t g_stop;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static const char *g_prefix = "";       /* port scope; default: the account */
static int g_portbase = 1;
static const char *g_transport = "loop";

/* ------------------------------------------------------------- roster */

typedef struct session {
    int fd;                             /* the lease: this connection */
    int port;
    char sid[64];                       /* host:pid:epoch-ms, never reused */
    char token[33];                     /* a child ATTACHes with this */
    char user[64], account[128], host[64], tty[64], prefix[128];
    long pid;
    time_t since;
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

    case MVXMSG_OP_STAT: {
        session *s = sess_for_fd(fd);
        ostr(out, g_transport);
        o16(out, MVXMSG_STATE_UP);
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
            "                [-x prefix] [-b portbase]\n");
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
        else { usage(); return 2; }
    }
    if (!sockpath && port == 0) { usage(); return 2; }
    if (g_portbase < 0) g_portbase = 1;
    if (strcmp(g_transport, "loop") != 0) {
        fprintf(stderr, "mvx-msgd: transport '%s' is not built in yet "
                        "(only 'loop')\n", g_transport);
        return 2;
    }

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
    fprintf(stderr, "mvx-msgd: %s transport, ports from %d, on %s\n",
            g_transport, g_portbase, sockpath ? sockpath : "tcp");

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

    if (sockpath) unlink(sockpath);
    return 0;
}
