* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* MAP-MODE file {native|mirror} — show or change a mapping's write policy.
* mirror (default): the record blob is the source of truth; the projection
* is best-effort and a bad value stores NULL.  native: the typed columns
* are authoritative, so a WRITE whose value does not fit its column is
* rejected (ON ERROR).  Switching to native first checks that every
* existing record already fits.
S = TRIM(SENTENCE())
FN = FIELD(S, " ", 2)
MODE = FIELD(S, " ", 3)
IF FN = "" THEN
   PRINT "usage: MAP-MODE file {native|mirror}"
   STOP
END
OPEN "DICT", FN TO DD ELSE
   PRINT "cannot open DICT ":FN
   STOP
END
READ SPEC FROM DD, "%MAP%" ELSE
   PRINT FN:" has no mapping"
   STOP
END
READ CUR FROM DD, "%MAPMODE%" ELSE CUR = "mirror"
IF MODE = "" THEN
   PRINT FN:" mapping mode: ":CUR
   STOP
END
MODE = OCONV(MODE, "MCL")
BEGIN CASE
CASE MODE = "mirror"
   WRITE "mirror" ON DD, "%MAPMODE%"
   PRINT FN:" mapping mode: mirror"
CASE MODE = "native"
   * ONE COLUMN PER ATTRIBUTE.  Two dictionary items may name the same
   * attribute: PRICE with MD2 and PRICE.RAW with no conversion are one stored
   * value read two ways.  Mirroring both is the point of mirror mode, where
   * every column is a projection of the record, so a secondary version of a
   * field costs an extra column and needs no cast to query it.
   *
   * Native has no record to project from -- the columns ARE the record -- so
   * two of them on one attribute leaves nothing to say which one reconstructs
   * it, and the answer came down to the order %MAP% happened to list them in
   * (mvx#158).  So native takes one mapping per attribute, on every backend.
   DUP = ""
   NS = DCOUNT(SPEC, @AM)
   FOR I = 1 TO NS
      FOR J = I + 1 TO NS
         IF SPEC<J, 2> = SPEC<I, 2> THEN
            IF DUP = "" THEN
               DUP = SPEC<I, 1>:" and ":SPEC<J, 1>
               DUP = DUP:" both map attribute ":SPEC<I, 2>
            END
         END
      NEXT J
   NEXT I
   IF DUP # "" THEN
      PRINT DUP
      PRINT "native takes one mapping per attribute; still mirror"
      STOP
   END
   OPEN FN TO F ELSE
      PRINT "cannot open ":FN
      STOP
   END
   BAD = MAPCHECK(F, SPEC)
   BEGIN CASE
   CASE BAD = -2
      PRINT "backend does not support mapping"
   CASE BAD > 0
      PRINT BAD:" record(s) do not fit the mapping; still mirror"
   CASE 1
      WRITE "native" ON DD, "%MAPMODE%"
      PRINT FN:" mapping mode: native"
   END CASE
CASE 1
   PRINT "mode must be native or mirror"
END CASE
