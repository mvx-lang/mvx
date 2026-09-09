* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* SELECT file {WITH item op value {AND ...}} — form the active select list
* for the next command (the session carries it across processes).  Multiple
* WITH/AND conditions are ANDed and pushed to one SQL WHERE when possible.
S = TRIM(SENTENCE())
* DESCRIBE / EXPLAIN may sit right after the verb (SELECT DESCRIBE file …) or
* trail the sentence; either way it prints the query plan instead of running.
DESC = 0
DW = FIELD(S, " ", 2)
IF DW = "DESCRIBE" OR DW = "EXPLAIN" THEN
   DESC = 1
   S = FIELD(S, " ", 1):" ":FIELD(S, " ", 3, 9999)
END
DICTF = 0
FN = FIELD(S, " ", 2)
TBASE = 3
IF FN = "DICT" THEN
   DICTF = 1
   FN = FIELD(S, " ", 3)
   TBASE = 4
END
IF FN = "" THEN
   PRINT "usage: SELECT {DICT} file {WITH item op value}"
   STOP
END
IF DICTF THEN
   OPEN "DICT", FN TO F ELSE
      PRINT "cannot open DICT ":FN
      STOP
   END
END ELSE
   OPEN FN TO F ELSE
      PRINT "cannot open ":FN
      STOP
   END
END
DOPEN = 0
IF DICTF = 0 THEN
   DOPEN = 1
   OPEN "DICT", FN TO DC ELSE DOPEN = 0
END

* ---- parse WITH conditions (ANDed) ----
NW = 0
WIS = ""
WOPS = ""
WVS = ""
NT = DCOUNT(S, " ")
I = TBASE
LOOP
WHILE I <= NT DO
   T = FIELD(S, " ", I)
   IF T = "WITH" OR T = "AND" THEN
      NW = NW + 1
      WIS<NW> = FIELD(S, " ", I + 1)
      WOPS<NW> = FIELD(S, " ", I + 2)
      WVS<NW> = FIELD(S, " ", I + 3)
      I = I + 3
   END
   IF T = "DESCRIBE" OR T = "EXPLAIN" THEN DESC = 1
   I = I + 1
REPEAT
FOR K = 1 TO NW
   V = WVS<K>
   IF LEN(V) >= 2 THEN
      Q = V[1, 1]
      IF Q = "'" OR Q = '"' THEN WVS<K> = V[2, LEN(V) - 2]
   END
NEXT K

* ---- resolve each condition's attribute / I-descriptor ----
WANOS = ""
WSPECS = ""
FOR K = 1 TO NW
   WI = WIS<K>
   WANO = ""
   WSPEC = ""
   IF WI = "@ID" THEN
      WANO = 0
   END ELSE
      GOT = 0
      IF DOPEN THEN
         READ DI FROM DC, WI THEN
            IF DI<1>[1, 1] = "I" THEN
               WANO = -1
               WSPEC = DI<2>
            END ELSE
               WANO = DI<2>
            END
            GOT = 1
         END
      END
      IF GOT = 0 THEN
         PRINT WI:" is not a dictionary item in ":FN
         STOP
      END
   END
   WANOS<K> = WANO
   WSPECS<K> = WSPEC
NEXT K

* ---- DESCRIBE: show the query plan, don't run it -----------------------
IF DESC THEN
   PSPEC = ""
   FOR K = 1 TO NW
      PSPEC<K> = WANOS<K>:@VM:WOPS<K>:@VM:WVS<K>
   NEXT K
   PLAN = DESCRIBE(F, PSPEC, "0":@VM:"0":@VM:"0")
   PRINT PLAN
   STOP
END

IF SYSTEM(11) = 0 THEN
   IXUSED = 0
   IF NW >= 1 THEN
      PSPEC = ""
      FOR K = 1 TO NW
         PSPEC<K> = WANOS<K>:@VM:WOPS<K>:@VM:WVS<K>
      NEXT K
      IXUSED = MULTISELECT(F, PSPEC)
   END
   IF IXUSED = 0 AND NW = 1 THEN
      IF WANOS<1> = -1 AND WSPECS<1>[1, 6] = "TRANS(" THEN
         IXUSED = TRANSSELECT(F, WSPECS<1>, WOPS<1>, WVS<1>)
      END
      * NOT for an EMPTY search value.  An index holds one entry per VALUE and
      * an empty attribute has none, so WITH X = "" finds nothing through the
      * index while the scan matches every record whose X is empty — the index
      * would change the answer, which is the whole of mvx#173.  Indexing
      * empties instead would put an entry on every record with a sparse
      * attribute, so the scan answers this one.
      IF IXUSED = 0 AND WOPS<1> = "=" AND WANOS<1> > 0 AND WVS<1> # "" THEN
         IXUSED = INDEXSELECT(F, WIS<1>, WVS<1>)
      END
   END
   IF IXUSED THEN
      NW = 0
   END ELSE
      SELECT F
   END
END
IDS = ""
DONE = 0
LOOP
   READNEXT ID ELSE DONE = 1
UNTIL DONE DO
   OK = 1
   IF NW >= 1 THEN
      READ R FROM F, ID ELSE R = ""
      FOR K = 1 TO NW
         IF OK THEN
            BEGIN CASE
            CASE WANOS<K> = 0
               RV = ID
            CASE WANOS<K> = -1
               IF DOPEN THEN
                  RV = IEVAL(R, WSPECS<K>, DC)
               END ELSE
                  RV = IEVAL(R, WSPECS<K>)
               END
            CASE 1
               RV = R<WANOS<K>>
            END CASE
            WOP = WOPS<K>
            WV = WVS<K>
            CK = 0
            * ANY VALUE MATCHES.  A multivalued attribute is compared VALUE BY
            * VALUE, not as one string, so WITH CITY = "London" matches a record
            * whose CITY is London]York.  The INDEX path has always done this
            * (ARCHITECTURE.md 5.2, and classic Pick), so before this the same
            * query answered differently depending on whether an index happened
            * to exist -- CREATE-INDEX changed results (mvx#173).
            * DCOUNT of an empty attribute is 0, but MV reads it as ONE empty
            * value: without the clamp a record with an empty CITY would stop
            * matching WITH CITY # "London", which it must.
            WNV = DCOUNT(RV, @VM)
            IF WNV = 0 THEN WNV = 1
            FOR WI = 1 TO WNV
               WVAL = RV<1, WI>
               BEGIN CASE
               CASE WOP = "="
                  IF WVAL = WV THEN CK = 1
               CASE WOP = "#"
                  IF WVAL # WV THEN CK = 1
               CASE WOP = ">"
                  IF WVAL > WV THEN CK = 1
               CASE WOP = "<"
                  IF WVAL < WV THEN CK = 1
               CASE WOP = ">="
                  IF WVAL >= WV THEN CK = 1
               CASE WOP = "<="
                  IF WVAL <= WV THEN CK = 1
               END CASE
            NEXT WI
            IF CK = 0 THEN OK = 0
         END
      NEXT K
   END
   IF OK THEN
      IDS<-1> = ID
   END
REPEAT
FORMLIST IDS
PRINT DCOUNT(IDS, @AM):" record(s) selected"
STOP

