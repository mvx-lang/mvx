* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* BASIC file item — compile a source record to an object.
* BASIC compiles; CATALOG links and publishes (ARCHITECTURE.md 6.4).
S = SENTENCE()
FN = FIELD(S, " ", 2)
IT = FIELD(S, " ", 3)
* Build options, after the item name: NODEBUG leaves out the debug information,
* STRIP leaves out the symbols.  Neither changes what the program does; both are
* for shipping one (mvx#223).  Passed to COMPILE as words -- a verb has no
* business knowing how the compiler spells its flags.
OPTS = ""
W = 4
LOOP
   KW = FIELD(S, " ", W)
WHILE KW # "" DO
   BEGIN CASE
      CASE OCONV(KW, "MCU") = "NODEBUG" ; OPTS = TRIM(OPTS:" NODEBUG")
      CASE OCONV(KW, "MCU") = "STRIP"   ; OPTS = TRIM(OPTS:" STRIP")
      CASE 1
         PRINT "BASIC: unknown option ":KW:" (NODEBUG, STRIP)"
         STOP
   END CASE
   W = W + 1
REPEAT
IF FN = "" OR IT = "" THEN
   PRINT "usage: BASIC filename itemname {NODEBUG} {STRIP}"
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
X = CREATEFILE(FN:".O", "DIR")
RC = COMPILE("c", FN:"/":IT, FN:".O/":IT:".o", OPTS)
IF RC = 0 THEN
   PRINT "[241] ":IT:" compiled"
END ELSE
   PRINT "[247] compilation of ":IT:" failed"
END
