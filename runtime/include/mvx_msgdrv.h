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

/* MVX message transport contract (mvx#230).
 *
 * THE MINIMAL CONTRACT IS THE WHOLE CONTRACT, as it is for storage drivers
 * (mvx_driver.h, ARCHITECTURE.md 4.1).  Nothing above mvx-msgd may depend on
 * backend-specific behaviour: a message crosses this boundary as a topic and
 * an MV record, and everything a particular service does differently happens
 * inside its driver.  The daemon is what a BASIC program talks to, and the
 * daemon must answer the same way whichever transport is underneath.
 *
 * WHAT IS GENERIC, AND WHAT IS ONE SERVICE'S IDEA:
 *
 *   generic    a topic (a '/'-separated name), publish, subscribe, receive,
 *              and a REQUEST for retention or durability
 *   MQTT's     quality of service, the retained flag on the wire, the last
 *              will, clean-session, shared subscriptions
 *
 * A driver says what it can do with capability bits and the daemon
 * compensates for the rest -- so a backend with no retained messages still
 * has a roster, and a backend with no will still notices a session that died.
 * That compensation lives in the daemon, once, rather than in each driver,
 * because two drivers implementing "presence" separately would eventually
 * implement it differently, and that is the migration break this rule exists
 * to prevent.
 *
 * PRESENCE IS NOT AN ENTRY POINT HERE.  It is a retained publish plus a will,
 * composed by the daemon out of the operations below.
 *
 * NO THREADS.  The daemon is one poll loop; a driver joins it by handing over
 * the descriptors it wants watched (fds) and being pumped when they are ready
 * or its timer expires.  A driver that starts a background thread would put
 * the roster and the inboxes in two hands at once.
 */
#ifndef MVX_MSGDRV_H
#define MVX_MSGDRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVX_MSGDRV_ABI 1

/* What a backend can do.  Everything here is OPTIONAL: the daemon works with
   none of them, and better with each one. */
#define MVX_MSGCAP_RETAIN   (1u << 0)  /* last value per topic, replayed on subscribe */
#define MVX_MSGCAP_WILL     (1u << 1)  /* the service announces our death for us */
#define MVX_MSGCAP_PERSIST  (1u << 2)  /* delivery survives a brief disconnect */
#define MVX_MSGCAP_WILDCARD (1u << 3)  /* pattern subscriptions */
#define MVX_MSGCAP_LOOPBACK (1u << 4)  /* our own publishes come back to us */

/* Transport state, as the daemon reports it through MSGSTATUS(). */
enum {
    MVX_MSGDRV_DOWN = 0,
    MVX_MSGDRV_CONNECTING = 1,
    MVX_MSGDRV_UP = 2,
};

typedef struct mvx_msgconn mvx_msgconn;   /* driver-owned */

typedef struct mvx_msgmsg {
    const char *topic;
    const char *payload;       /* MV record bytes; NOT NUL-terminated */
    int64_t plen;
    int retained;              /* 1 = a stored last value, not a live send */
} mvx_msgmsg;

typedef void (*mvx_msgcb)(void *user, const mvx_msgmsg *m);

typedef struct mvx_msgdrv {
    const char *name;
    unsigned caps;

    /* `loc` is a connection profile ("@name") or an inline address, resolved
       by the DAEMON -- so a credential never reaches a client process.  The
       will is ignored by a driver that does not advertise MVX_MSGCAP_WILL,
       and the daemon compensates rather than the driver pretending. */
    mvx_msgconn *(*connect)(const char *loc, const char *client_id,
                            const char *will_topic, const char *will_payload,
                            int64_t will_plen, char *err, size_t errlen);
    void (*disconnect)(mvx_msgconn *c);    /* graceful: clears the will */

    /* 1 published, 0 accepted but degraded (buffered, best effort), -1 hard
       failure.  MUST NOT block beyond budget_ms: a wedged service may not
       stall the daemon, because the daemon is also the local roster. */
    int (*publish)(mvx_msgconn *c, const char *topic, const char *payload,
                   int64_t plen, int durable, int retain, int budget_ms);

    int (*subscribe)(mvx_msgconn *c, const char *pattern);
    int (*unsubscribe)(mvx_msgconn *c, const char *pattern);

    /* Joining the daemon's poll loop.  fds() fills in what to watch and
       returns how many; timeout_ms() is -1 when the driver wants no timer;
       pump() delivers whatever has arrived through cb and returns 0 when the
       connection is gone. */
    int (*fds)(mvx_msgconn *c, int *fds, short *events, int max);
    int (*timeout_ms)(mvx_msgconn *c);
    int (*pump)(mvx_msgconn *c, mvx_msgcb cb, void *user);

    int (*state)(mvx_msgconn *c);          /* MVX_MSGDRV_* */
} mvx_msgdrv;

/* Every transport library exports exactly one entry point, as extensions and
   storage drivers do; it returns NULL for an ABI it does not implement. */
typedef const mvx_msgdrv *(*mvx_msgdrv_entry_fn)(int abi);
const mvx_msgdrv *mvx_msgdrv_entry(int abi);

/* The in-daemon transport: no service, no network, one host.  Built in rather
   than dlopen'd because a registry must work with nothing installed. */
const mvx_msgdrv *mvx_msgdrv_loop(void);

#ifdef __cplusplus
}
#endif
#endif /* MVX_MSGDRV_H */
