* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* CREATE-MAP file field {field...} — declare the file's relational
* mapping and build it.  Writes the %MAP% control record into the
* dictionary, so from now on every WRITE mirrors the record into the
* projection (mirror mode).  BUILD-MAP re-runs the backfill on demand.
S = TRIM(SENTENCE())
FN = FIELD(S, " ", 2)
NT = DCOUNT(S, " ")
IF FN = "" OR NT < 3 THEN
   PRINT "usage: CREATE-MAP file field {field...} | ALL"
   STOP
END
OPEN "DICT", FN TO DD ELSE
   PRINT "cannot open DICT ":FN
   STOP
END
* Build the field list: the explicit names, or every mappable D-item for
* ALL / *.  ALL is deliberately not the default — mapping every field makes
* each WRITE project every column, so it must be asked for and confirmed.
FLDS = ""
A3 = FIELD(S, " ", 3)
IF (A3 = "ALL" OR A3 = "*") AND NT = 3 THEN
   SELECT DD
   DONE = 0
   LOOP
      READNEXT ID ELSE DONE = 1
   UNTIL DONE DO
      IF ID[1, 1] # "%" THEN
         READ DI FROM DD, ID THEN
            IF DI<1>[1, 1] = "D" THEN FLDS<-1> = ID
         END
      END
   REPEAT
   NF = DCOUNT(FLDS, @AM)
   IF NF = 0 THEN
      PRINT "no mappable fields"
      STOP
   END
   PRINT "map ALL ":NF:" field(s) of ":FN:"; every WRITE then projects all"
   PRINT "of them into SQL (mapping overhead).  Continue (y/n)? ":
   INPUT ANS
   IF ANS[1, 1] # "Y" AND ANS[1, 1] # "y" THEN
      PRINT "cancelled"
      STOP
   END
END ELSE
   FOR I = 3 TO NT
      FLDS<-1> = FIELD(S, " ", I)
   NEXT I
END
SPEC = ""
NS = 0
NF = DCOUNT(FLDS, @AM)
FOR I = 1 TO NF
   FLD = FLDS<I>
   READ DI FROM DD, FLD THEN
      IF DI<1>[1, 1] = "D" THEN
         CONV = DI<3>
         C2 = CONV[1, 2]
         T = "TEXT"
         IF C2 = "MD" OR C2 = "MR" OR C2 = "ML" THEN T = "NUMERIC"
         IF C2 = "MT" THEN T = "TIME"
         IF CONV[1, 1] = "D" THEN T = "DATE"
         * ONE COLUMN PER ATTRIBUTE.  Several dictionary items may name the
         * same attribute -- PRICE with MD2 and PRICE.RAW with none are the
         * same value read two ways -- and mapping both stored the value
         * twice with no rule about which was the truth.  The read-back loop
         * wrote every mapped field into its attribute in turn, so the LAST
         * one declared won and the other was silently discarded, order
         * deciding which (mvx#158).  Refusing the second is what removes the
         * ambiguity: the other item is still usable, derived from the one
         * column rather than stored beside it.
         DUP = 0
         FOR J = 1 TO NS
            IF SPEC<J, 2> = DI<2> THEN DUP = J
         NEXT J
         IF DUP THEN
            PRINT FLD:" and ":SPEC<DUP, 1>:" are both attribute ":DI<2>
            PRINT "an attribute can be mapped once; map one and derive the other"
            STOP
         END
         NS = NS + 1
         SPEC<NS> = FLD:@VM:DI<2>:@VM:CONV:@VM:T:@VM:DI<6>
      END ELSE
         PRINT FLD:" is not a mappable dictionary item (skipped)"
      END
   END ELSE
      PRINT FLD:" is not a dictionary item (skipped)"
   END
NEXT I
IF NS = 0 THEN
   PRINT "no mappable fields"
   STOP
END
WRITE SPEC ON DD, "%MAP%"
OPEN FN TO F ELSE
   PRINT "cannot open ":FN
   STOP
END
RC = MAPBUILD(F, SPEC)
BEGIN CASE
CASE RC = -2
   PRINT "backend does not support mapping"
CASE RC < 0
   PRINT "map build failed"
CASE 1
   PRINT "mapped ":RC:" record(s); writes are now mirrored"
   * A field indexed while un-mapped got an expression index on the record
   * blob.  Now that it is an identity column the query prefers the column,
   * so offer to rebuild the index on it (numeric/date still query the raw
   * blob, so only plain text columns benefit).
   READ XL FROM DD, "%INDEXES%" ELSE XL = ""
   IF XL # "" THEN
      NM = DCOUNT(SPEC, @AM)
      FOR J = 1 TO NM
         MF = FIELD(SPEC<J>, @VM, 1)
         MCONV = FIELD(SPEC<J>, @VM, 3)
         MTYPE = FIELD(SPEC<J>, @VM, 4)
         MASSOC = FIELD(SPEC<J>, @VM, 5)
         IF MTYPE = "TEXT" AND MCONV = "" AND MASSOC = "" THEN
            LOCATE(MF, XL; XP) THEN
               PRINT MF:" is indexed on the record; rebuild it on the ":
               PRINT "mapped column (y/n)? ":
               INPUT ANS
               IF ANS[1, 1] = "Y" OR ANS[1, 1] = "y" THEN
                  EXECUTE "DELETE-INDEX ":FN:" ":MF
                  EXECUTE "CREATE-INDEX ":FN:" ":MF
               END
            END
         END
      NEXT J
   END
END CASE
