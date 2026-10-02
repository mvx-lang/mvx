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
*  * @file CREATE-INDEX
*  * @version 1.0
*  */
* CREATE-INDEX file item — index a dictionary attribute item.
* Only D-type attribute items are indexable: a computed item whose
* value depends on anything outside the record would go silently
* stale (the TRANS() rule, ARCHITECTURE.md 5.4).
S = TRIM(SENTENCE())
FN = FIELD(S, " ", 2)
IT = FIELD(S, " ", 3)
IF FN = "" OR IT = "" THEN
   PRINT "usage: CREATE-INDEX file item"
   STOP
END
OPEN FN TO F ELSE
   PRINT "cannot open ":FN
   STOP
END
OPEN "DICT", FN TO DC ELSE
   PRINT FN:" has no dictionary - nothing to index by"
   STOP
END
READ DI FROM DC, IT ELSE
   PRINT IT:" is not a dictionary item in ":FN
   STOP
END
IF DI<1>[1, 1] # "D" THEN
   PRINT IT:" is not a D-type attribute item - only local attribute"
   PRINT "extractions are indexable (the TRANS() rule)"
   STOP
END
IF NUM(DI<2>) = 0 OR DI<2> < 1 THEN
   PRINT IT:" has no usable attribute number"
   STOP
END
READ XL FROM DC, "%INDEXES%" ELSE XL = ""
ADDED = 0
LOCATE(IT, XL; POS) ELSE
   XL<-1> = IT
   WRITE XL ON DC, "%INDEXES%"
   ADDED = 1
END
N = INDEXBUILD(F, IT)
* A FAILED BUILD MUST NOT LEAVE THE ITEM LISTED (mvx#347).  %INDEXES% is
* written before the build, because the runtime reads it to learn which
* item it is building -- and nothing took it back out again, so a build
* that failed still left LIST-INDEXES reporting an index that exists in no
* backend.  Measured on MariaDB: `CITY / 1 index(es)` against a table whose
* only key was PRIMARY.  Only what this run added is removed; an index that
* was already there is not disturbed by a failure to rebuild it.
IF N < 0 AND ADDED THEN
   READ XL2 FROM DC, "%INDEXES%" THEN
      LOCATE(IT, XL2; P2) THEN
         XL2 = DELETE(XL2, P2, 0, 0)
         WRITE XL2 ON DC, "%INDEXES%"
      END
   END
END
BEGIN CASE
CASE N >= 0
   PRINT "index ":FN:".":IT:" built, ":N:" record(s)"
CASE N = -2
   * The backend says it cannot index THIS KIND of field.  Not a fault, and
   * not something to retry: say what still works so the reader does not go
   * looking for a broken driver.
   PRINT FN:".":IT:" cannot be indexed by this backend"
   PRINT "the query still works -- the filter runs in the backend or the"
   PRINT "verb, unindexed, so only speed is lost"
CASE 1
   PRINT "index build failed (backend without index capability?)"
END CASE
