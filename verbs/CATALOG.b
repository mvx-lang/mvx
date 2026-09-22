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
* Build options, as BASIC takes them: NODEBUG for no debug information, STRIP
* for no symbols (mvx#223).  A cataloged program is the one that gets shipped,
* so this is where they matter most.
OPTS = ""
W = 4
LOOP
   KW = FIELD(S, " ", W)
WHILE KW # "" DO
   BEGIN CASE
      CASE OCONV(KW, "MCU") = "NODEBUG" ; OPTS = TRIM(OPTS:" NODEBUG")
      CASE OCONV(KW, "MCU") = "STRIP"   ; OPTS = TRIM(OPTS:" STRIP")
      CASE 1
         PRINT "CATALOG: unknown option ":KW:" (NODEBUG, STRIP)"
         STOP
   END CASE
   W = W + 1
REPEAT
IF FN = "" OR IT = "" THEN
   PRINT "usage: CATALOG filename itemname {NODEBUG} {STRIP}"
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
   RC = COMPILE("shared", FN:"/":IT, "LIB/":IT, OPTS)
   IF RC = 0 THEN
      KIND = "a subroutine"
      IF ISLIB = 2 THEN KIND = "a function"
      PRINT "[244] ":IT:" cataloged as ":KIND
   END ELSE
      PRINT "[247] compilation of ":IT:" failed"
   END
   STOP
END
* PUBLISH IT (mvx#248).  A cataloged main program has to be LOADABLE, so the
* runtime can run it inside an existing process instead of forking one -- an
* EXECUTE'd program then shares the caller's open files, locks, select list
* and transaction, rather than reopening everything and seeing a stale view.
*
* How many files that takes is the platform's business, not this verb's, so
* the driver decides: one on macOS, where an executable can also be loaded,
* and two where it cannot -- the program as CATALOG/<item>.so plus a small
* loader published as CATALOG/<item>.  Either way the program itself is
* compiled once and exists on disk once.
RC = COMPILE("catalog", FN:"/":IT, "CATALOG/":IT, OPTS)
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
