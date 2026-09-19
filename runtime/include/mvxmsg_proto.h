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

/* mvx-msgd wire protocol — shared by the daemon and the runtime's client
 * (mvx#226).
 *
 * Framing is mvxd's, deliberately: u32 payload-length | u8 op | fields for a
 * request, u32 payload-length | u8 status | fields for a reply, strings as u16
 * length + bytes, integers little-endian host order.  One framing in the tree
 * is one framing to get right.
 *
 * WHAT IS NOT HERE IS THE POINT.  No topic, no broker address, no quality of
 * service, no retained flag: this is the boundary a BASIC program sees, and it
 * must stay transport-blind so a backend can be swapped underneath without
 * anything above the daemon noticing.  The transport contract is a separate
 * header, seen only by the daemon.
 *
 * The connection IS the session lease.  A session registers with HELLO and is
 * deregistered when its connection drops — by BYE, by exit, or by being killed.
 * Nothing has to be cleaned up afterwards, which is the same reason mvxd leases
 * locks to the connection rather than to a process id.
 */
#ifndef MVXMSG_PROTO_H
#define MVXMSG_PROTO_H

#define MVXMSG_PROTO_VER 1

enum {
    /* proto-ver, prefix, account, user, host, tty, pid
       -> OK: port, session-id, attach-token */
    MVXMSG_OP_HELLO = 1,

    /* session-id, attach-token -> OK: port
       A child process joining the port its parent already holds. */
    MVXMSG_OP_ATTACH,

    /* (no fields) -> OK.  Graceful deregister; dropping the connection does
       the same thing, so this is politeness rather than bookkeeping. */
    MVXMSG_OP_BYE,

    /* scope -> OK: count, then per session:
       port, user, account, host, tty, since (u32 epoch seconds) */
    MVXMSG_OP_WHO,

    /* (no fields) -> OK: transport, state, port, prefix */
    MVXMSG_OP_STAT,
};

/* WHO scopes: this host, or every host sharing the prefix.  Only LOCAL can be
   answered until a transport carries presence, and the daemon says which it
   gave you rather than pretending. */
enum {
    MVXMSG_SCOPE_LOCAL = 0,
    MVXMSG_SCOPE_SYSTEM = 1,
};

/* Transport health, as STAT reports it. */
enum {
    MVXMSG_STATE_DOWN = 0,
    MVXMSG_STATE_CONNECTING = 1,
    MVXMSG_STATE_UP = 2,
};

enum {
    MVXMSG_ST_OK = 0,
    MVXMSG_ST_NO = 1,           /* nothing to report / no such session */
    MVXMSG_ST_BUSY = 2,         /* rate limited (later stages) */
    MVXMSG_ST_ERR = 3,
    MVXMSG_ST_DENIED = 4,       /* not permitted */
};

#endif /* MVXMSG_PROTO_H */
