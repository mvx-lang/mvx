* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* CREATE-FILE {DICT | DATA} name {DIR | DIRECTORY | USING <driver> {connection}}
*
* DICT and DATA make one half of the file, as they do on U2.  A dictionary on
* its own is how a SHARED dictionary is made -- several files can name it, and
* then one set of D-items serves all of them -- and data on its own is how a
* file borrows someone else's.  With neither word, both halves are made.
* The file's backend is decided at creation: a directory file, a
* local LMDB file (the default), or a file on another driver
* (lmdbnet, and later postgres/mongo) bound in the account's BINDINGS
* record. For lmdbnet the connection defaults to $MVXDAEMON.
S = TRIM(SENTENCE())
USAGE = "usage: CREATE-FILE {DICT|DATA} name {DIR | DIRECTORY | USING driver {connection}}"
HALF = ""
N = 2
W = OCONV(FIELD(S, " ", 2), "MCU")
IF W = "DICT" OR W = "DATA" THEN
   HALF = W
   N = 3
END
NAME = FIELD(S, " ", N)
TYPE = OCONV(FIELD(S, " ", N + 1), "MCU")
IF NAME = "" THEN
   PRINT USAGE
   STOP
END
BEGIN CASE
CASE TYPE = "DIR" OR TYPE = "DIRECTORY"
   OK = CREATEFILE(NAME, TRIM(HALF:" DIR"))
CASE TYPE = "USING"
   DRV = FIELD(S, " ", N + 2)
   CONN = FIELD(S, " ", N + 3, 99)
   IF DRV = "" THEN
      PRINT USAGE
      STOP
   END
   TV = TRIM(HALF:" USING "):" ":DRV
   IF CONN # "" THEN TV = TV:" ":CONN
   OK = CREATEFILE(NAME, TV)
CASE TYPE = ""
   IF HALF = "" THEN OK = CREATEFILE(NAME) ELSE OK = CREATEFILE(NAME, HALF)
CASE 1
   PRINT USAGE
   STOP
END CASE
IF OK THEN
   PRINT "[417] file ":NAME:" created"
END ELSE
   PRINT "unable to create ":NAME:" (does it already exist?)"
END
