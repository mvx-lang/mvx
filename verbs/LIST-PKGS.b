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
*  * @file LIST-PKGS
*  * @version 2.0
*  */
* LIST-PKGS — linked packages with manifest name@version and deps.
OPEN "." TO ACC ELSE
   PRINT "cannot open the account directory"
   STOP
END
READ PKGS FROM ACC, "PACKAGES" ELSE PKGS = ""
NP = DCOUNT(PKGS, @AM)
IF NP = 0 THEN
   PRINT "no packages linked"
   STOP
END
FOR LI = 1 TO NP
   CUR = PKGS<LI>
   GOSUB 9000
   STATE = "broken"
   OPEN CUR:"/VOC" TO PV THEN STATE = "ok"
   * Runtime dependencies read as "requires a, b"; a '+' build dependency is
   * not linked (it is needed to COMPILE the package, not to run it), so report
   * it separately rather than listing it among the runtime requirements.
   DEPTXT = "" ; BLDTXT = ""
   ND = DCOUNT(PDEPS, @AM)
   FOR DI = 1 TO ND
      D = PDEPS<DI>
      IF D[1, 1] = "+" THEN
         D = D[2, LEN(D)]
         IF BLDTXT = "" THEN BLDTXT = "builds with ":D ELSE BLDTXT = BLDTXT:", ":D
      END ELSE
         IF DEPTXT = "" THEN DEPTXT = "requires ":D ELSE DEPTXT = DEPTXT:", ":D
      END
   NEXT DI
   IF BLDTXT # "" THEN
      IF DEPTXT = "" THEN DEPTXT = BLDTXT ELSE DEPTXT = DEPTXT:" (":BLDTXT:")"
   END
   SYSTXT = ""
   IF PSYS # "" THEN SYSTXT = " [":PSYS:"]"
   * Pad the path column rather than FMT it: "L#46" cuts anything longer, and
   * a path is the one column where a silent truncation is useless -- a deep
   * checkout printed a prefix that named no directory you could cd to.  Pad
   * to the same width so short paths line up exactly as before, and let a
   * long one run past the column with its single separating space.
   PAD = 47 - LEN(CUR)
   IF PAD < 1 THEN PAD = 1
   PRINT FMT(PNAME:"@":PVER, "L#14"):" ":FMT(STATE, "L#6"):" ":CUR:SPACE(PAD):DEPTXT:SYSTXT
NEXT LI
PRINT NP:" package(s) linked"
STOP

* ---- 9000: manifest of CUR -> PNAME, PVER, PDEPS -----------------------
9000
PNAME = ""
PVER = ""
PSYS = ""
PDEPS = ""
OPEN CUR TO MPD THEN
   READ MF FROM MPD, "PKG" THEN
      PNAME = MF<1>
      PVER = MF<2>
      PSYS = MF<4>
      MN = DCOUNT(MF, @AM)
      FOR MI = 5 TO MN
         DNAME = MF<MI>
         * '?' marks an optional dependency (see LINK-PKG); it is a resolution
         * marker, not part of the name, so strip it for display.
         IF DNAME[1, 1] = "?" THEN DNAME = DNAME[2, LEN(DNAME)]
         IF DNAME # "" THEN
            PDEPS<-1> = DNAME
         END
      NEXT MI
   END
END
IF PNAME = "" THEN
   LS = 0
   FOR MI = 1 TO LEN(CUR)
      IF CUR[MI, 1] = "/" THEN LS = MI
   NEXT MI
   PNAME = CUR[LS + 1, LEN(CUR)]
END
IF PVER = "" THEN PVER = "?"
RETURN
