* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
/**
 * @file CONVERT-FILE
 * @version 1.0
 */
* CONVERT-FILE file newtype {connection} — change one file's storage
* backend, moving its records and dictionary into the new one.  Types
* are the driver names LISTF shows: "lmdb" (local hash file), "dir" (a
* legible directory file), or a driver such as "lmdbnet" with a
* connection.  The records are re-keyed into the new backend verbatim,
* so a hash file converts to and from a directory file (and back)
* without loss; the dictionary comes across too, but the %FILE% control
* record is left as the new backend stamped it, not the old one.  This
* is the per-file companion to the account converter (mvx-convert-acct).
S = TRIM(SENTENCE())
FN = FIELD(S, " ", 2)
NT = FIELD(S, " ", 3)
CONN = TRIM(FIELD(S, " ", 4, 99))
IF FN = "" OR NT = "" THEN
   PRINT "usage: CONVERT-FILE file newtype {connection}"
   PRINT "       newtype: lmdb | dir | <driver> (e.g. lmdbnet)"
   STOP
END
OPEN FN TO SRC ELSE
   PRINT "cannot open ":FN
   STOP
END

* ---- stage records and dictionary into a temp file of the new type ----
TMP = "%CVTF.":FN:"%"
JUNK = DELETEFILE(TMP)
TGT = TMP
GOSUB 2000
OPEN TMP TO TDST ELSE
   PRINT "cannot create the new ":NT:" file (bad type or connection?)"
   STOP
END
N = 0
SELECT SRC
DONE = 0
LOOP
   READNEXT ID ELSE DONE = 1
UNTIL DONE DO
   READ R FROM SRC, ID THEN
      WRITE R ON TDST, ID
      N = N + 1
   END
REPEAT
FROMD = FN
TOD = TMP
GOSUB 3000                          ;* copy dictionary FN -> TMP (skip %FILE%)

* ---- replace the file: drop the old backend, recreate as the new type -
SRC = ""
JUNK = DELETEFILE(FN)
TGT = FN
GOSUB 2000
OPEN FN TO FIN ELSE
   PRINT "cannot recreate ":FN
   STOP
END
OPEN TMP TO TSRC ELSE STOP
SELECT TSRC
DONE = 0
LOOP
   READNEXT ID ELSE DONE = 1
UNTIL DONE DO
   READ R FROM TSRC, ID THEN WRITE R ON FIN, ID
REPEAT
FROMD = TMP
TOD = FN
GOSUB 3000                          ;* copy dictionary TMP -> FN (skip %FILE%)
JUNK = DELETEFILE(TMP)
PRINT FN:" converted to ":NT:" (":N:" record(s))"
STOP

* ---- 2000: create the file named TGT with backend NT ------------------
2000
BEGIN CASE
CASE NT = "dir"
   XC = CREATEFILE(TGT, "DIR")
CASE NT = "lmdb"
*  USING lmdb, not a bare CREATEFILE.  A bare one takes the ACCOUNT'S
*  default, which is sqlite now (#187), so `CONVERT-FILE x lmdb` reported
*  success and left the file exactly where it was.
   XC = CREATEFILE(TGT, "USING lmdb")
CASE CONN # ""
   XC = CREATEFILE(TGT, "USING ":NT:" ":CONN)
CASE 1
   XC = CREATEFILE(TGT, "USING ":NT)
END CASE
RETURN

* ---- 3000: copy dictionary FROMD -> TOD, preserving the new %FILE% ----
3000
OPEN "DICT", FROMD TO DS ELSE RETURN
OPEN "DICT", TOD TO DD ELSE RETURN
SELECT DS
DDONE = 0
LOOP
   READNEXT DID ELSE DDONE = 1
UNTIL DDONE DO
   IF DID # "%FILE%" THEN
      READ DR FROM DS, DID THEN WRITE DR ON DD, DID
   END
REPEAT
RETURN
