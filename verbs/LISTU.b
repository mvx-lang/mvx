* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* /**
*  * @file LISTU
*  * @version 1.0
*  */
* LISTU — who is logged on, by port (mvx#226).
*
* The roster comes from mvx-msgd, which holds it because the connection is the
* lease: a session that was killed is already gone from this list, with nothing
* to tidy up.  Ports are scoped by a prefix that defaults to the account name,
* so the same user has the same port wherever they logged on.
ST = MSGSTATUS()
IF ST<1,1> # "up" THEN
   PRINT "LISTU: no session registry is running (start mvx-msgd)"
   STOP
END
W = MSGWHO("SYSTEM")
N = DCOUNT(W, @AM)
IF W = "" THEN N = 0
PRINT "Port User            Account          Host            Logged on"
FOR I = 1 TO N
   PORT = W<I,1>
   USR  = W<I,2>
   ACCT = W<I,3>
   TTY  = W<I,5>
   SINCE = W<I,6>
*  The daemon reports seconds since the epoch; show the time of day, which is
*  what an operator reading a roster wants.
   HMS = OCONV(MOD(SINCE, 86400), "MTS")
   HOST = W<I,4>
   PRINT FMT(PORT, "R#4"):" ":FMT(USR, "L#15"):" ":FMT(ACCT, "L#16"):" ":
   PRINT FMT(HOST, "L#15"):" ":HMS
NEXT I
PRINT
PRINT N:" session(s) on prefix ":ST<1,4>
