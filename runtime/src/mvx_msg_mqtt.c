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

/* The MQTT transport (mvx#232) — the only file in the tree that knows what
 * MQTT is.  libmosquitto is linked here and nowhere else: not into libmvxrt,
 * not into mvx, not into a compiled BASIC program.  Everything above
 * mvx-msgd sees topics and MV records (mvx_msgdrv.h).
 *
 * NO BACKGROUND THREAD.  The daemon is one poll loop and the contract says a
 * driver joins it, so this drives libmosquitto the external way -- the socket
 * from mosquitto_socket(), then loop_read/loop_write/loop_misc as poll says
 * they are wanted.  mosquitto_loop_start() would put the roster and the
 * inboxes in two hands at once, which is the thing the no-threads rule is
 * there to prevent.
 *
 * WHAT MQTT CONTRIBUTES, and why it is worth a dependency at all:
 *   - one connection per host instead of a mesh: MSG * is a single publish;
 *   - retained topics and a last will, which presence is built from (#stage5);
 *   - authentication, TLS and clustering that are not ours to write.
 * Persistence is NOT among them -- a message to a logged-off port is dropped,
 * as classic Pick did (#228), and QoS 1 here is about not losing a message in
 * flight rather than storing one for later.
 *
 * DEGRADED IS NOT DEAD.  A broker that is unreachable must not take messaging
 * away: the daemon keeps delivering locally from its own roster, publishes
 * buffer here and flush on reconnect, and MSGSTATUS() says so.  Nothing in
 * this file may block the daemon: every call has a budget.
 */

#include "mvx_msgdrv.h"

/* THE CLIENT HEADER, NOT THE UMBRELLA.  mosquitto 2.1's <mosquitto.h> also
   pulls in the broker and its plugin API, which needs cJSON installed -- a
   header we have no business requiring to build a client.  The split header
   is client-only; older releases (Debian's 1.6, say) have just the umbrella,
   so fall back to it. */
#if defined(__has_include)
#  if __has_include(<mosquitto/libmosquitto.h>)
#    include <mosquitto/libmosquitto.h>
#  else
#    include <mosquitto.h>
#  endif
#else
#  include <mosquitto.h>
#endif
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MQ_QUEUE 1024           /* messages held between pumps */
#define MQ_OUTBUF 256           /* publishes buffered while disconnected */
#define MQ_SUBS 64
#define MQ_KEEPALIVE 30

typedef struct {
    char *topic;
    char *payload;
    int64_t plen;
    int retained;
} mqmsg;

typedef struct {
    char *topic;
    char *payload;
    int64_t plen;
    int qos, retain;
} mqpend;

struct mvx_msgconn {
    struct mosquitto *m;
    int connected;
    int connecting;
    time_t retry_at;
    unsigned backoff;           /* seconds, 1..30 */

    char host[256];
    int port;

    mqmsg in[MQ_QUEUE];         /* arrived, not yet pumped */
    int ihead, icount;

    mqpend out[MQ_OUTBUF];      /* published while the broker was away */
    int ohead, ocount;

    char *subs[MQ_SUBS];        /* re-subscribed on every reconnect */
    int nsubs;
};

/* ------------------------------------------------------------ callbacks */

static void on_connect(struct mosquitto *m, void *user, int rc) {
    (void)m;
    mvx_msgconn *c = user;
    if (rc != 0) return;
    c->connected = 1;
    c->connecting = 0;
    c->backoff = 1;
    /* A SUBSCRIPTION DOES NOT SURVIVE A RECONNECT, so re-send every one.
       Forgetting this is the classic way a reconnect looks successful and
       silently delivers nothing afterwards. */
    for (int i = 0; i < c->nsubs; i++)
        mosquitto_subscribe(c->m, NULL, c->subs[i], 1);
}

static void on_disconnect(struct mosquitto *m, void *user, int rc) {
    (void)m; (void)rc;
    mvx_msgconn *c = user;
    c->connected = 0;
    c->connecting = 0;
    c->retry_at = time(NULL) + 1;
    c->backoff = 1;
}

static void on_message(struct mosquitto *m, void *user,
                       const struct mosquitto_message *msg) {
    (void)m;
    mvx_msgconn *c = user;
    if (!msg || !msg->topic || c->icount >= MQ_QUEUE) return;
    mqmsg *q = &c->in[(c->ihead + c->icount) % MQ_QUEUE];
    q->topic = strdup(msg->topic);
    q->payload = malloc((size_t)msg->payloadlen + 1);
    if (!q->topic || !q->payload) {
        free(q->topic);
        free(q->payload);
        return;
    }
    memcpy(q->payload, msg->payload, (size_t)msg->payloadlen);
    q->payload[msg->payloadlen] = '\0';
    q->plen = msg->payloadlen;
    q->retained = msg->retain;
    c->icount++;
}

/* ------------------------------------------------------------- helpers */

/* tcp://host:1883, host:1883, or host.  A URL is what a person writes in a
   connection profile; the rest is what they write when they are in a hurry. */
static void split_addr(const char *loc, char *host, size_t hcap, int *port) {
    const char *p = loc && *loc ? loc : "localhost";
    if (strncmp(p, "tcp://", 6) == 0) p += 6;
    else if (strncmp(p, "mqtt://", 7) == 0) p += 7;
    const char *colon = strrchr(p, ':');
    if (colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1)) {
        size_t n = (size_t)(colon - p);
        if (n >= hcap) n = hcap - 1;
        memcpy(host, p, n);
        host[n] = '\0';
        *port = atoi(colon + 1);
    } else {
        snprintf(host, hcap, "%s", p);
        *port = 1883;
    }
}

static int try_connect(mvx_msgconn *c) {
    int rc = mosquitto_connect(c->m, c->host, c->port, MQ_KEEPALIVE);
    if (rc != MOSQ_ERR_SUCCESS) {
        c->connecting = 0;
        c->backoff = c->backoff < 30 ? c->backoff * 2 : 30;
        c->retry_at = time(NULL) + c->backoff;
        return 0;
    }
    c->connecting = 1;
    return 1;
}

/* -------------------------------------------------------------- driver */

static mvx_msgconn *mq_connect(const char *loc, const char *client_id,
                               const char *will_topic, const char *will_payload,
                               int64_t will_plen, char *err, size_t errlen) {
    static int lib_started;
    if (!lib_started) { mosquitto_lib_init(); lib_started = 1; }

    mvx_msgconn *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->backoff = 1;
    split_addr(loc, c->host, sizeof c->host, &c->port);

    c->m = mosquitto_new(client_id && *client_id ? client_id : NULL, true, c);
    if (!c->m) {
        snprintf(err, errlen, "mosquitto_new failed");
        free(c);
        return NULL;
    }

    /* Credentials come from the ENVIRONMENT of the daemon, never from a
       client process and never from account data a program could edit.  A
       container sets them; systemd sets them from a private file. */
    const char *user = getenv("MVXMSG_USER");
    const char *pass = getenv("MVXMSG_PASSWORD");
    if (user && *user) mosquitto_username_pw_set(c->m, user, pass);

    /* The will is the death notice presence is built on.  The daemon passes
       one only when it means it; a driver without MVX_MSGCAP_WILL would be
       ignoring this, which is why the capability bit exists. */
    if (will_topic && will_payload)
        mosquitto_will_set(c->m, will_topic, (int)will_plen, will_payload, 1, 1);

    mosquitto_connect_callback_set(c->m, on_connect);
    mosquitto_disconnect_callback_set(c->m, on_disconnect);
    mosquitto_message_callback_set(c->m, on_message);

    if (!try_connect(c)) {
        /* NOT A FAILURE.  A broker that is down must not stop the daemon
           starting: local messaging keeps working and this reconnects in the
           background.  Saying so here is the difference between a degraded
           system and one that will not boot. */
        snprintf(err, errlen, "cannot reach %s:%d yet; retrying",
                 c->host, c->port);
    }
    return c;
}

static void mq_disconnect(mvx_msgconn *c) {
    if (!c) return;
    if (c->connected) mosquitto_disconnect(c->m);   /* clears the will */
    for (int i = 0; i < c->icount; i++) {
        mqmsg *q = &c->in[(c->ihead + i) % MQ_QUEUE];
        free(q->topic);
        free(q->payload);
    }
    for (int i = 0; i < c->ocount; i++) {
        mqpend *o = &c->out[(c->ohead + i) % MQ_OUTBUF];
        free(o->topic);
        free(o->payload);
    }
    for (int i = 0; i < c->nsubs; i++) free(c->subs[i]);
    mosquitto_destroy(c->m);
    free(c);
}

static int mq_publish(mvx_msgconn *c, const char *topic, const char *payload,
                      int64_t plen, int durable, int retain, int budget_ms) {
    (void)budget_ms;
    if (!c) return -1;
    int qos = durable ? 1 : 0;
    if (c->connected) {
        int rc = mosquitto_publish(c->m, NULL, topic, (int)plen, payload, qos,
                                   retain ? true : false);
        if (rc == MOSQ_ERR_SUCCESS) return 1;
    }
    /* Buffered, oldest dropped when full: a message is worth holding across a
       blip, and is not worth exhausting memory for. */
    if (c->ocount >= MQ_OUTBUF) {
        mqpend *old = &c->out[c->ohead];
        free(old->topic);
        free(old->payload);
        c->ohead = (c->ohead + 1) % MQ_OUTBUF;
        c->ocount--;
    }
    mqpend *o = &c->out[(c->ohead + c->ocount) % MQ_OUTBUF];
    o->topic = strdup(topic);
    o->payload = malloc((size_t)plen + 1);
    if (!o->topic || !o->payload) {
        free(o->topic);
        free(o->payload);
        return -1;
    }
    memcpy(o->payload, payload, (size_t)plen);
    o->payload[plen] = '\0';
    o->plen = plen;
    o->qos = qos;
    o->retain = retain;
    c->ocount++;
    return 0;                   /* accepted, degraded */
}

static int mq_subscribe(mvx_msgconn *c, const char *pattern) {
    if (!c || c->nsubs >= MQ_SUBS) return 0;
    for (int i = 0; i < c->nsubs; i++)
        if (strcmp(c->subs[i], pattern) == 0) return 1;   /* already have it */
    c->subs[c->nsubs] = strdup(pattern);
    if (!c->subs[c->nsubs]) return 0;
    c->nsubs++;
    if (c->connected) mosquitto_subscribe(c->m, NULL, pattern, 1);
    return 1;
}

static int mq_unsubscribe(mvx_msgconn *c, const char *pattern) {
    if (!c) return 0;
    for (int i = 0; i < c->nsubs; i++)
        if (strcmp(c->subs[i], pattern) == 0) {
            free(c->subs[i]);
            c->subs[i] = c->subs[--c->nsubs];
            if (c->connected) mosquitto_unsubscribe(c->m, NULL, pattern);
            return 1;
        }
    return 0;
}

static int mq_fds(mvx_msgconn *c, int *fds, short *events, int max) {
    if (!c || max < 1) return 0;
    int s = mosquitto_socket(c->m);
    if (s < 0) return 0;
    fds[0] = s;
    events[0] = (short)(POLLIN | (mosquitto_want_write(c->m) ? POLLOUT : 0));
    return 1;
}

/* A timer is wanted whether or not the socket is busy: keepalives are due on
   their own schedule, and a disconnected driver has a reconnect to make. */
static int mq_timeout(mvx_msgconn *c) {
    if (!c) return -1;
    if (!c->connected) {
        time_t now = time(NULL);
        long wait = (long)(c->retry_at - now);
        return wait > 0 ? (int)(wait * 1000) : 0;
    }
    return 500;
}

static int mq_pump(mvx_msgconn *c, mvx_msgcb cb, void *user) {
    if (!c) return 0;

    if (!c->connected && !c->connecting && time(NULL) >= c->retry_at)
        try_connect(c);

    /* Read, write and housekeeping, driven from OUR loop rather than theirs.
       A lost connection is not fatal: the callbacks flag it and the reconnect
       above picks it up. */
    /* CHECK WHAT THE LOOP CALLS RETURN.  A broker that goes away while we are
       idle is noticed here and nowhere else: mosquitto only runs the
       disconnect callback for a loss it sees itself, so a socket that closed
       between pumps would otherwise leave this driver reporting `up' for
       ever, and a program asking MSGSTATUS would be told a comfortable lie. */
    int rc = mosquitto_loop_read(c->m, 1);
    if (rc == MOSQ_ERR_SUCCESS && mosquitto_want_write(c->m))
        rc = mosquitto_loop_write(c->m, 1);
    if (rc == MOSQ_ERR_SUCCESS) rc = mosquitto_loop_misc(c->m);
    if (rc == MOSQ_ERR_CONN_LOST || rc == MOSQ_ERR_NO_CONN ||
        rc == MOSQ_ERR_ERRNO) {
        if (c->connected || c->connecting) {
            c->connected = 0;
            c->connecting = 0;
            c->backoff = 1;
            c->retry_at = time(NULL) + 1;
        }
    }

    /* Anything buffered while the broker was away goes now, in order. */
    while (c->connected && c->ocount > 0) {
        mqpend *o = &c->out[c->ohead];
        int rc = mosquitto_publish(c->m, NULL, o->topic, (int)o->plen,
                                   o->payload, o->qos, o->retain ? true : false);
        if (rc != MOSQ_ERR_SUCCESS) break;
        free(o->topic);
        free(o->payload);
        c->ohead = (c->ohead + 1) % MQ_OUTBUF;
        c->ocount--;
    }

    while (c->icount > 0) {
        mqmsg *q = &c->in[c->ihead];
        mvx_msgmsg mm = {q->topic, q->payload, q->plen, q->retained};
        c->ihead = (c->ihead + 1) % MQ_QUEUE;
        c->icount--;
        if (cb) cb(user, &mm);
        free(q->topic);
        free(q->payload);
    }
    return 1;
}

static int mq_state(mvx_msgconn *c) {
    if (!c) return MVX_MSGDRV_DOWN;
    if (c->connected) return MVX_MSGDRV_UP;
    return c->connecting ? MVX_MSGDRV_CONNECTING : MVX_MSGDRV_DOWN;
}

static const mvx_msgdrv mqtt_drv = {
    "mqtt",
    MVX_MSGCAP_RETAIN | MVX_MSGCAP_WILL | MVX_MSGCAP_PERSIST |
        MVX_MSGCAP_WILDCARD | MVX_MSGCAP_LOOPBACK,
    mq_connect,
    mq_disconnect,
    mq_publish,
    mq_subscribe,
    mq_unsubscribe,
    mq_fds,
    mq_timeout,
    mq_pump,
    mq_state,
};

const mvx_msgdrv *mvx_msgdrv_entry(int abi) {
    return abi == MVX_MSGDRV_ABI ? &mqtt_drv : NULL;
}
