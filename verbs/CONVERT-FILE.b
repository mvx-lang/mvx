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
* legible directory file), or a driver such as "postgres" with a
* connection.  The records are re-keyed into the new backend verbatim,
* so a hash file converts to and from a directory file (and back)
* without loss; the dictionary comes across too, but the %FILE% control
* record is left as the new backend stamped it, not the old one.  This
* is the per-file companion to CONVERT-ACCOUNT, which does the account.
S = TRIM(SENTENCE())
FN = FIELD(S, " ", 2)
NT = FIELD(S, " ", 3)
CONN = TRIM(FIELD(S, " ", 4, 99))
IF FN = "" OR NT = "" THEN
   PRINT "usage: CONVERT-FILE file newtype {connection}"
   PRINT "       newtype: lmdb | dir | <driver> (e.g. postgres)"
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

* ---- keep whatever the ACCOUNT put in the file's VOC pointer (#322) ---
*  Attributes 1 to 4 are MVX's: the type, the data location, the
*  dictionary location, and the options slot.  Attribute 5 onwards is the
*  account's -- a description, a site convention, anything.  A convert is a
*  DELETE and a CREATE, and the delete took the whole record with it, so
*  everything past MVX's own was quietly lost.  Nothing MVX writes put
*  anything there, which is why it went unnoticed; #318 gave attribute 4 a
*  meaning and made the loss matter.
KEPT = ""
OPEN "VOC" TO VOCF THEN
   READ VR FROM VOCF, FN THEN
      IF DCOUNT(VR, @AM) > 4 THEN KEPT = FIELD(VR, @AM, 5, 999)
   END
END

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

*  put the account's own attributes back on the freshly written pointer
IF KEPT # "" THEN
   OPEN "VOC" TO VOCF2 THEN
      READ VR2 FROM VOCF2, FN THEN
         LOOP
         WHILE DCOUNT(VR2, @AM) < 4 DO
            VR2 = VR2:@AM
         REPEAT
         WRITE VR2:@AM:KEPT ON VOCF2, FN
      END
   END
END
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
