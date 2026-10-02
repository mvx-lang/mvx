* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* SET-CREDENTIAL driver target key field=value {field=value...}
* Store a backend secret in the account credential store
* (.mvx-private/credentials, git-ignored, mode 0600).  BINDINGS names
* only the non-secret reference (driver, target, key); the secret lives
* here.  Example:
*   SET-CREDENTIAL mongo mvxdb-a:27017 SALES token=abc123
S = TRIM(SENTENCE())
DRV = FIELD(S, " ", 2)
TGT = FIELD(S, " ", 3)
KEY = FIELD(S, " ", 4)
NT = DCOUNT(S, " ")
FLDS = ""
FOR I = 5 TO NT
   IF FLDS # "" THEN FLDS := " "
   FLDS := FIELD(S, " ", I)
NEXT I
IF DRV = "" OR TGT = "" OR KEY = "" OR FLDS = "" THEN
   PRINT "usage: SET-CREDENTIAL driver target key field=value {field=value...}"
   STOP
END
OK = SETCRED(DRV, TGT, KEY, FLDS)
IF OK THEN
   PRINT "credential stored: ":DRV:" ":TGT:" ":KEY
END ELSE
   PRINT "could not store credential (check .mvx-private is writable)"
END
