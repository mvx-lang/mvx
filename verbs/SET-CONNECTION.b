* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* SET-CONNECTION name field=value {field=value...}
* Define a named connection profile in .mvx-private/connections (local,
* git-ignored).  BINDINGS references it by name (@name), so the daemon
* host and credentials live in one place.  Example:
*   SET-CONNECTION salesdb driver=postgres address=mvxdb:5432 namespace=SALES user=app
S = TRIM(SENTENCE())
CN = FIELD(S, " ", 2)
NT = DCOUNT(S, " ")
FLDS = ""
FOR I = 3 TO NT
   IF FLDS # "" THEN FLDS := " "
   FLDS := FIELD(S, " ", I)
NEXT I
IF CN = "" OR FLDS = "" THEN
   PRINT "usage: SET-CONNECTION name field=value {field=value...}"
   STOP
END
OK = SETCONN(CN, FLDS)
IF OK THEN
   PRINT "connection stored: ":CN
END ELSE
   PRINT "could not store connection (check .mvx-private is writable)"
END
