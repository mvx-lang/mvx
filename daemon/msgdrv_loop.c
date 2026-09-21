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

/* The `loop' transport (mvx#230): one host, no service, no network.
 *
 * It is the simplest thing that satisfies the contract -- a publish goes into
 * a queue and comes back out of pump() -- and it is not a toy:
 *
 *   - it is what a single-host site runs, which is most Pick sites;
 *   - it is the suite's transport, so messaging is tested with no broker,
 *     no network and no timing;
 *   - it is the PROOF THE ABSTRACTION IS REAL.  It advertises neither
 *     retained messages nor a will, so the daemon's compensation paths are
 *     exercised by every ordinary run rather than first meeting daylight when
 *     a second backend arrives.
 *
 * Deliberately queue-then-pump rather than calling back inside publish(): the
 * daemon publishes from inside its request handler, and a driver that
 * delivered synchronously would re-enter the daemon's own state half way
 * through updating it.
 */

#include "mvx_msgdrv.h"

#include <stdlib.h>
#include <string.h>

#define LOOP_QUEUE 1024
#define LOOP_SUBS 32

typedef struct {
    char *topic;
    char *payload;
    int64_t plen;
    int retained;
} loopmsg;

struct mvx_msgconn {
    loopmsg q[LOOP_QUEUE];
    int head, count;
    char *subs[LOOP_SUBS];
    int nsubs;
    unsigned caps;              /* what this instance admits to; see below */
};

/* An MQTT-style topic match, which is the least a pattern can mean: `+' is
   one level, `#' is the rest.  A driver with no wildcards would subscribe to
   every exact topic instead, which is what the capability bit is for. */
static int topic_match(const char *pat, const char *topic) {
    while (*pat && *topic) {
        if (*pat == '#') return 1;
        if (*pat == '+') {
            pat++;
            while (*topic && *topic != '/') topic++;
            if (*pat == '/' && *topic == '/') { pat++; topic++; }
            continue;
        }
        if (*pat != *topic) return 0;
        pat++;
        topic++;
    }
    if (*pat == '#') return 1;
    return *pat == '\0' && *topic == '\0';
}

static mvx_msgconn *loop_connect(const char *loc, const char *client_id,
                                 const char *will_topic,
                                 const char *will_payload, int64_t will_plen,
                                 char *err, size_t errlen) {
    (void)loc; (void)client_id; (void)err; (void)errlen;
    /* A will is meaningless here: there is no second party to publish it, and
       nothing to publish it to.  The daemon knows that from the capability
       bits and notices a dead session the direct way instead. */
    (void)will_topic; (void)will_payload; (void)will_plen;
    mvx_msgconn *c = calloc(1, sizeof *c);
    if (c) c->caps = MVX_MSGCAP_WILDCARD | MVX_MSGCAP_LOOPBACK;
    return c;
}

static void loop_disconnect(mvx_msgconn *c) {
    if (!c) return;
    for (int i = 0; i < c->count; i++) {
        loopmsg *m = &c->q[(c->head + i) % LOOP_QUEUE];
        free(m->topic);
        free(m->payload);
    }
    for (int i = 0; i < c->nsubs; i++) free(c->subs[i]);
    free(c);
}

static int loop_publish(mvx_msgconn *c, const char *topic, const char *payload,
                        int64_t plen, int durable, int retain, int budget_ms) {
    (void)durable; (void)budget_ms;
    if (!c) return -1;
    /* Retention is a REQUEST, and this backend cannot honour it.  Saying so
       by dropping the flag -- rather than quietly storing it -- is what keeps
       the daemon's compensation honest. */
    (void)retain;

    int subscribed = 0;
    for (int i = 0; i < c->nsubs; i++)
        if (topic_match(c->subs[i], topic)) subscribed = 1;
    if (!subscribed) return 1;          /* nobody here wants it; not an error */

    if (c->count >= LOOP_QUEUE) return 0;   /* degraded: the queue is full */
    loopmsg *m = &c->q[(c->head + c->count) % LOOP_QUEUE];
    m->topic = strdup(topic);
    m->payload = malloc((size_t)plen + 1);
    if (!m->topic || !m->payload) {
        free(m->topic);
        free(m->payload);
        return -1;
    }
    memcpy(m->payload, payload, (size_t)plen);
    m->payload[plen] = '\0';
    m->plen = plen;
    m->retained = 0;
    c->count++;
    return 1;
}

static int loop_subscribe(mvx_msgconn *c, const char *pattern) {
    if (!c || c->nsubs >= LOOP_SUBS) return 0;
    c->subs[c->nsubs] = strdup(pattern);
    if (!c->subs[c->nsubs]) return 0;
    c->nsubs++;
    return 1;
}

static int loop_unsubscribe(mvx_msgconn *c, const char *pattern) {
    if (!c) return 0;
    for (int i = 0; i < c->nsubs; i++)
        if (strcmp(c->subs[i], pattern) == 0) {
            free(c->subs[i]);
            c->subs[i] = c->subs[--c->nsubs];
            return 1;
        }
    return 0;
}

/* Nothing to watch: this transport has no descriptor.  The daemon asks for a
   short timer instead, which is how any driver without a socket joins the
   loop. */
static int loop_fds(mvx_msgconn *c, int *fds, short *events, int max) {
    (void)c; (void)fds; (void)events; (void)max;
    return 0;
}

static int loop_timeout(mvx_msgconn *c) { return c && c->count ? 0 : -1; }

static int loop_pump(mvx_msgconn *c, mvx_msgcb cb, void *user) {
    if (!c) return 0;
    while (c->count > 0) {
        loopmsg *m = &c->q[c->head];
        mvx_msgmsg mm = {m->topic, m->payload, m->plen, m->retained};
        /* Unlink BEFORE the callback: the daemon may publish again from
           inside it (a message that fans out), and the queue must not be
           half-consumed while that happens. */
        c->head = (c->head + 1) % LOOP_QUEUE;
        c->count--;
        if (cb) cb(user, &mm);
        free(m->topic);
        free(m->payload);
    }
    return 1;
}

static int loop_state(mvx_msgconn *c) {
    return c ? MVX_MSGDRV_UP : MVX_MSGDRV_DOWN;
}

static const mvx_msgdrv loop_drv = {
    "loop",
    MVX_MSGCAP_WILDCARD | MVX_MSGCAP_LOOPBACK,
    loop_connect,
    loop_disconnect,
    loop_publish,
    loop_subscribe,
    loop_unsubscribe,
    loop_fds,
    loop_timeout,
    loop_pump,
    loop_state,
};

const mvx_msgdrv *mvx_msgdrv_loop(void) { return &loop_drv; }
