* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* CATALOG file item — link a program into CATALOG/ and publish it in VOC.
S = SENTENCE()
FN = FIELD(S, " ", 2)
IT = FIELD(S, " ", 3)
IF FN = "" OR IT = "" THEN
   PRINT "usage: CATALOG filename itemname"
   STOP
END
OPEN FN TO F ELSE
   PRINT "cannot open ":FN
   STOP
END
READ SRC FROM F, IT ELSE
   PRINT IT:" not on file ":FN
   STOP
END
* A SUBROUTINE or FUNCTION source catalogs into LIB/ as a shared library the
* runtime CALL resolver loads; only main programs become verbs.  Skip
* comments in every style (* ! REM // and /* */ blocks) and $ preprocessor
* directives when finding the first real statement.
FIRST = ""
INBLK = 0
NA = DCOUNT(SRC, @AM)
FOR I = 1 TO NA
   LN = TRIM(SRC<I>)
   IF INBLK THEN
      P = INDEX(LN, "*/", 1)
      IF P > 0 THEN
         INBLK = 0
         LN = TRIM(LN[P + 2, LEN(LN)])
      END ELSE
         LN = ""
      END
   END
   IF LN # "" THEN
      C2 = LN[1, 2]
      BEGIN CASE
      CASE C2 = "/*"
         IF INDEX(LN, "*/", 1) = 0 THEN INBLK = 1
      CASE LN[1, 1] = "*" OR LN[1, 1] = "!" OR C2 = "//"
         X = 0
      CASE OCONV(FIELD(LN, " ", 1), "MCU") = "REM"
         X = 0
      CASE LN[1, 1] = "$"
         X = 0
      CASE 1
         FIRST = LN
         I = NA
      END CASE
   END
NEXT I
* A FUNCTION catalogs the same way a SUBROUTINE does.  It shares the
* subroutine ABI -- the compiler marks it isSubroutine and reserves argv[0]
* for the result -- so the only thing that stopped a cataloged FUNCTION from
* resolving was this classification: it fell through to the exe path and the
* link failed on a missing _mvx_main (#101).
ISLIB = 0
IF FIRST[1, 11] = "SUBROUTINE " OR FIRST = "SUBROUTINE" THEN ISLIB = 1
IF FIRST[1, 9] = "FUNCTION " OR FIRST = "FUNCTION" THEN ISLIB = 2
IF ISLIB THEN
   X = CREATEFILE("LIB", "DIR")
   RC = COMPILE("shared", FN:"/":IT, "LIB/":IT)
   IF RC = 0 THEN
      KIND = "a subroutine"
      IF ISLIB = 2 THEN KIND = "a function"
      PRINT "[244] ":IT:" cataloged as ":KIND
   END ELSE
      PRINT "[247] compilation of ":IT:" failed"
   END
   STOP
END
RC = COMPILE("exe", FN:"/":IT, "CATALOG/":IT)
IF RC = 0 ELSE
   PRINT "[247] compilation of ":IT:" failed"
   STOP
END
OPEN "VOC" TO V ELSE
   PRINT "cannot open VOC"
   STOP
END
WRITE "V":@AM:"CATALOG/":IT ON V, IT
PRINT "[244] ":IT:" cataloged"
