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
*  * @file UNLINK-PKG
*  * @version 2.0
*  */
* UNLINK-PKG path — remove a linked package, unless another linked
* package declares it as a dependency.
S = TRIM(SENTENCE())
P = FIELD(S, " ", 2)
IF P = "" THEN
   PRINT "usage: UNLINK-PKG /path/to/package"
   STOP
END
OPEN "." TO ACC ELSE
   PRINT "cannot open the account directory"
   STOP
END
READ PKGS FROM ACC, "PACKAGES" ELSE PKGS = ""
LOCATE(P, PKGS; TPOS) ELSE
   PRINT P:" is not linked"
   STOP
END
CUR = P
GOSUB 9000
TARGET = PNAME
NP = DCOUNT(PKGS, @AM)
FOR LI = 1 TO NP
   IF LI # TPOS THEN
      CUR = PKGS<LI>
      GOSUB 9000
      LOCATE(TARGET, PDEPS; DP) THEN
         PRINT "cannot unlink ":TARGET:": ":PKGS<LI>:" depends on it"
         STOP
      END
   END
NEXT LI
PKGS = DELETE(PKGS, TPOS, 0, 0)
WRITE PKGS ON ACC, "PACKAGES"
PRINT "unlinked ":P
STOP

* ---- 9000: manifest of CUR -> PNAME, PDEPS -----------------------------
9000
PNAME = ""
PDEPS = ""
PMAN = ""
OPEN CUR TO MPD THEN
   READ MF FROM MPD, "PKG" THEN
      PMAN = "PKG"
      PNAME = MF<1>
      MN = DCOUNT(MF, @AM)
      FOR MI = 5 TO MN
         DNAME = MF<MI>
         * a '?' prefix marks an optional dependency (see LINK-PKG); strip it
         * so the reverse-dependency check still recognises the name — an
         * optional dependency is still a dependency for unlink safety.
         IF DNAME[1, 1] = "?" THEN DNAME = DNAME[2, LEN(DNAME)]
         IF DNAME # "" THEN
            PDEPS<-1> = DNAME
         END
      NEXT MI
   END
END
IF PMAN = "" THEN GOSUB 9500
IF PNAME = "" THEN
   LS = 0
   FOR MI = 1 TO LEN(CUR)
      IF CUR[MI, 1] = "/" THEN LS = MI
   NEXT MI
   PNAME = CUR[LS + 1, LEN(CUR)]
END
RETURN

* ---- 9500: the same fields out of mvpkg.json (mvx#285) -----------------
* mv_package carries no PKG -- "PKG is gone; mvpkg.json is the manifest" is one
* of its own tests -- so a reader that knows only PKG sees nothing in it.  PKG
* is tried FIRST: mvx's own packages/http has one and no mvpkg.json.
*
* Scanned rather than JSONDECODE'd, because that is an extension the json
* PACKAGE provides and a standard verb cannot require a package to be installed.
*
* THE TWO MANIFESTS NAME THINGS DIFFERENTLY.  PKG holds short names (`cmd`
* depending on `getopt`); mvpkg.json holds owner-qualified ones
* (`mvx-lang/cmd` depending on `mvx-lang/getopt`).  Dependency names here are
* matched against other packages' names, so every name is reduced to its last
* path segment.
9500
MJ = OSREAD(CUR : "/mvpkg.json")
IF STATUS() # 0 THEN RETURN
PMAN = "mvpkg.json"
* Self-contained: 9500 only runs when there was no PKG, so there is nothing to
* clobber, and not every caller of 9000 initialises every field -- UNLINK-PKG
* wants only the name and the dependencies, and read PSYS unassigned when this
* block set it.
PVER = "" ; PSYS = ""
MJQ = CHAR(34)
MJKEY = "name" ; GOSUB 9510 ; MJB = MJVAL ; GOSUB 9530 ; PNAME = MJB
MJKEY = "version" ; GOSUB 9510 ; PVER = MJVAL
MJKEY = "systems" ; GOSUB 9520
MJN = DCOUNT(MJLIST, @AM)
FOR MI = 1 TO MJN
   IF PSYS = "" THEN PSYS = MJLIST<MI> ELSE PSYS := " " : MJLIST<MI>
NEXT MI
MJKEY = "dependencies" ; GOSUB 9520
MJN = DCOUNT(MJLIST, @AM)
FOR MI = 1 TO MJN
   MJD = MJLIST<MI>
   * '?' is a resolution marker, not part of the name (see LINK-PKG); '!' is a
   * runtime requirement rather than a dependency, and is not one to show.
   IF MJD[1, 1] = "?" THEN MJD = MJD[2, LEN(MJD)]
   IF MJD[1, 1] # "!" THEN
   * name[@sys,sys | @!sys,sys][:constraint].  PKG never carried either --
   * mvpkg.json does, and neither is part of the directory name a dependency
   * resolves to, so both come off before the name is used (mvx#285).
   MJC = INDEX(MJD, ":", 1)
   IF MJC > 0 THEN MJD = MJD[1, MJC - 1]
   MJF = ""
   MJA = INDEX(MJD, "@", 1)
   IF MJA > 0 THEN
      MJF = MJD[MJA + 1, LEN(MJD)]
      MJD = MJD[1, MJA - 1]
   END
   GOSUB 9540
      IF MJAPP THEN
         MJB = MJD ; GOSUB 9530
         IF MJB # "" THEN PDEPS<-1> = MJB
      END
   END
NEXT MI
RETURN

* 9510: MJVAL = the "MJKEY":"value" string in MJ, "" when there is none.
9510
MJVAL = ""
MJP = INDEX(MJ, MJQ : MJKEY : MJQ, 1)
IF MJP = 0 THEN RETURN
MJT = MJ[MJP + LEN(MJKEY) + 2, LEN(MJ)]
MJC = INDEX(MJT, ":", 1)
IF MJC = 0 THEN RETURN
MJT = MJT[MJC + 1, LEN(MJT)]
MJA = INDEX(MJT, MJQ, 1)
IF MJA = 0 THEN RETURN
MJT = MJT[MJA + 1, LEN(MJT)]
MJE = INDEX(MJT, MJQ, 1)
IF MJE = 0 THEN RETURN
MJVAL = MJT[1, MJE - 1]
RETURN

* 9520: MJLIST = the "MJKEY":[ ... ] array in MJ, one entry per attribute.
9520
MJLIST = ""
MJP = INDEX(MJ, MJQ : MJKEY : MJQ, 1)
IF MJP = 0 THEN RETURN
MJT = MJ[MJP, LEN(MJ)]
MJO = INDEX(MJT, "[", 1) ; MJE = INDEX(MJT, "]", 1)
IF MJO = 0 OR MJE <= MJO THEN RETURN
MJSEG = MJT[MJO, MJE - MJO + 1]
MJDONE = 0
LOOP UNTIL MJDONE DO
   MJA = INDEX(MJSEG, MJQ, 1)
   IF MJA = 0 THEN
      MJDONE = 1
   END ELSE
      MJSEG = MJSEG[MJA + 1, LEN(MJSEG)]
      MJE = INDEX(MJSEG, MJQ, 1)
      IF MJE = 0 THEN
         MJDONE = 1
      END ELSE
         MJV = TRIM(MJSEG[1, MJE - 1])
         MJSEG = MJSEG[MJE + 1, LEN(MJSEG)]
         IF MJV # "" THEN MJLIST<-1> = MJV
      END
   END
REPEAT
RETURN

* 9530: MJB = its own last path segment ("mvx-lang/cmd" -> "cmd").
9530
MJS = 0
FOR MJI = 1 TO LEN(MJB)
   IF MJB[MJI, 1] = "/" THEN MJS = MJI
NEXT MJI
IF MJS > 0 THEN MJB = MJB[MJS + 1, LEN(MJB)]
RETURN

* 9540: MJAPP = 1 when the platform filter MJF admits mvx.  Empty admits
* everything; "a,b" is a whitelist; "!a,b" a blacklist.  A dependency that does
* not apply here is not a dependency -- mv_package declares
* "mvx-lang/json@!mvx:^1.5", meaning json is needed everywhere EXCEPT mvx, and
* without this it would be demanded on the one system that excludes it.
9540
MJAPP = 1
IF MJF = "" THEN RETURN
MJNEG = 0
IF MJF[1, 1] = "!" THEN MJNEG = 1 ; MJF = MJF[2, LEN(MJF)]
MJHIT = 0
MJN2 = DCOUNT(MJF, ",")
FOR MJJ = 1 TO MJN2
   IF TRIM(FIELD(MJF, ",", MJJ)) = "mvx" THEN MJHIT = 1
NEXT MJJ
IF MJNEG THEN
   MJAPP = 0
   IF MJHIT = 0 THEN MJAPP = 1
END ELSE
   MJAPP = MJHIT
END
RETURN
