* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* BUILD-PKG <pkgdir> — build a package's BASIC source natively, the
* package-level peer of CATALOG.  It compiles <pkgdir>/BP/* through the
* runtime COMPILE primitive (which spawns the compiler argv-style, no shell):
* SUBROUTINE and FUNCTION sources become <pkgdir>/LIB/<name> shared libraries the CALL
* resolver loads, main programs become <pkgdir>/CATALOG/<name> verb
* executables.  The package ships its own VOC verb records, so VOC is left
* untouched.  This needs only developer privilege (compiling), not the
* unrestricted tier a shell-out to mkpkg would — so an installer can build a
* pure-BASIC package without granting OS execution, and without mkpkg on PATH.
*
* Out of scope (needs a C compiler / shell): a package with a native
* extension (a NATIVE manifest or build-native.sh) — BUILD-PKG reports
* BUILD-PKG-NATIVE so a caller can fall back to mkpkg.  *.BP fallback source
* directories (a bundled dependency copy) are likewise left to mkpkg.
   S = TRIM(SENTENCE())
   PDIR = FIELD(S, " ", 2)
   IF PDIR = "" THEN
      PRINT "usage: BUILD-PKG /path/to/package"
      STOP
   END

   * A native extension needs cc; detect it (its files show up in the package
   * directory listing) and defer to mkpkg rather than build a partial package.
   OPEN PDIR TO PF ELSE
      PRINT "cannot open package ":PDIR
      STOP
   END
   SELECT PF
   NATIVE = 0
   DONE = 0
   LOOP
      READNEXT ID ELSE DONE = 1
   UNTIL DONE DO
      IF ID = "NATIVE" OR ID = "build-native.sh" THEN NATIVE = 1
   REPEAT
   IF NATIVE THEN
      PRINT "BUILD-PKG-NATIVE: ":PDIR:" has a native extension; build with mkpkg"
      STOP
   END

   BPDIR = PDIR : "/BP"
   OPEN BPDIR TO BPF ELSE
      * no BP source — e.g. a binary-only release (CATALOG/LIB already shipped)
      PRINT "BUILD-PKG: no BP source in ":PDIR:" — nothing to build"
      STOP
   END
   SELECT BPF
   NBUILT = 0
   NERR = 0
   DONE = 0
   LOOP
      READNEXT IT ELSE DONE = 1
   UNTIL DONE DO
      IF IT[1, 1] # "_" AND IT[1, 1] # "." THEN
         READ SRC FROM BPF, IT THEN
            GOSUB FIRSTSTMT
            IF ISSUB THEN
               RC = COMPILE("shared", BPDIR:"/":IT, PDIR:"/LIB/":IT)
            END ELSE
               RC = COMPILE("exe", BPDIR:"/":IT, PDIR:"/CATALOG/":IT)
            END
            IF RC = 0 THEN
               NBUILT = NBUILT + 1
            END ELSE
               PRINT "BUILD-PKG: compilation of ":IT:" failed"
               NERR = NERR + 1
            END
         END
      END
   REPEAT
   PRINT "BUILD-PKG: ":PDIR:" — ":NBUILT:" item(s) built, ":NERR:" error(s)"
   STOP

* --- first real statement of SRC -> ISSUB (1 if a SUBROUTINE) -----------
* Skip comments in every style (* ! REM // and /* */ blocks) and $ preprocessor
* directives, matching the CATALOG verb, then classify: a SUBROUTINE source
* builds a LIB library.
FIRSTSTMT:
   ISSUB = 0
   FIRST = ""
   INBLK = 0
   NA = DCOUNT(SRC, @AM)
   FOR FI = 1 TO NA
      LN = TRIM(SRC<FI>)
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
            FI = NA
         END CASE
      END
   NEXT FI
   IF FIRST[1, 11] = "SUBROUTINE " OR FIRST = "SUBROUTINE" THEN ISSUB = 1
   * A FUNCTION builds into LIB/ exactly as a SUBROUTINE does -- it shares the
   * subroutine ABI, with argv[0] reserved for the result.  Without this it was
   * compiled as a program and the link failed on a missing _mvx_main, which is
   * why a package could not export a function-style accessor (#101).
   IF FIRST[1, 9] = "FUNCTION " OR FIRST = "FUNCTION" THEN ISSUB = 1
   RETURN
