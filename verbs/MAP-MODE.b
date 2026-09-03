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
   * ONE MAPPING PER ATTRIBUTE -- native's rule, not mirror's.  Two dictionary
   * items may name the same attribute: PRICE with MD2 and PRICE.RAW with no
   * conversion are one stored value read two ways.  Mirror is where that
   * belongs -- every column is a projection of the record, so a secondary
   * version of a field costs a column and needs no cast to query it.
   *
   * Native has no record to project from; the columns ARE the record, so two
   * of them on one attribute leaves nothing to say which reconstructs it, and
   * the answer came down to the order %MAP% listed them in (mvx#158).
   *
   * Refusing is only half an answer, so say which one to keep.  The field that
   * rebuilds the attribute most exactly is the one native wants: an identity
   * field stores the value untouched, DATE and TIME carry it through a
   * conversion that reverses, and MD/MR/ML have dropped the raw digits.  This
   * is the same ranking the read-back uses, so the advice and the behaviour
   * cannot drift apart.
   NS = DCOUNT(SPEC, @AM)
   DUPN = 0
   FOR I = 1 TO NS
      AT = SPEC<I, 2>
      FIRST = 1
      FOR K = 1 TO I - 1
         IF SPEC<K, 2> = AT THEN FIRST = 0
      NEXT K
      IF FIRST THEN
         FLDS = ""
         BEST = ""
         BR = -1
         FOR J = 1 TO NS
            IF SPEC<J, 2> = AT THEN
               FLDS<-1> = SPEC<J, 1>
               R = 0
               IF SPEC<J, 4> = "DATE" THEN R = 1
               IF SPEC<J, 4> = "TIME" THEN R = 1
               IF SPEC<J, 3> = "" THEN R = 2
               IF R > BR THEN
                  BR = R
                  BEST = SPEC<J, 1>
               END
            END
         NEXT J
         IF DCOUNT(FLDS, @AM) > 1 THEN
            DUPN = DUPN + 1
            BEGIN CASE
            CASE BR = 2
               WHY = "stores the value unchanged"
            CASE BR = 1
               WHY = "its conversion reverses exactly"
            CASE 1
               WHY = "the closest, though its conversion drops the raw digits"
            END CASE
            PRINT "attribute ":AT:" is mapped by ":CHANGE(FLDS, @AM, ", ")
            PRINT "   for native keep ":BEST:" -- ":WHY
         END
      END
   NEXT I
   IF DUPN > 0 THEN
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
