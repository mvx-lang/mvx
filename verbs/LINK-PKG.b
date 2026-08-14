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
*  * @file LINK-PKG
*  * @version 2.0
*  */
* LINK-PKG path — link a package and its dependency closure.
* The PKG manifest (attr 1 name, 2 version, 3 description, 4 target
* systems, 5.. dependency names) drives resolution: a dependency is
* satisfied by an already-linked package of that name, or found beside
* the requiring package, or on $MVXPKGPATH — and is linked automatically.
* A dependency name prefixed '?' is optional: it is linked if it can be
* resolved (overriding any bundled copy), but its absence is not an error
* — the requiring package ships a fallback under the same names (e.g.
* git bundles cmd in CMD.BP), so it runs standalone with no package
* manager present.
* A name prefixed '+' is a BUILD dependency: needed to compile the package
* (mvpkg, say, provides the shared PLATFORM.H every managed package
* includes), never to run it.  Linking skips them entirely — mkpkg resolves
* them at build time, and `MVPKG install --source` installs them before it
* builds.
S = TRIM(SENTENCE())
P = FIELD(S, " ", 2)
IF P = "" THEN
   PRINT "usage: LINK-PKG /path/to/package"
   STOP
END
OPEN "." TO ACC ELSE
   PRINT "cannot open the account directory"
   STOP
END
READ PKGS FROM ACC, "PACKAGES" ELSE PKGS = ""

* names of already-linked packages
LNAMES = ""
NP = DCOUNT(PKGS, @AM)
FOR LI = 1 TO NP
   CUR = PKGS<LI>
   GOSUB 9000
   LNAMES<LI> = PNAME
NEXT LI

QUEUE = ""
QUEUE<1> = P
ADDED = ""
LOOP
UNTIL QUEUE = "" DO
   CUR = QUEUE<1>
   QUEUE = DELETE(QUEUE, 1, 0, 0)
   SKIP = 0
   LOCATE(CUR, PKGS; POS) THEN SKIP = 1
   IF SKIP = 0 THEN
      * a package is identified by its PKG manifest, not its VOC: a
      * package may expose only subroutines (a LIB library) and carry
      * no verb records at all.
      OPEN CUR TO PV ELSE
         PRINT CUR:" is not a package (cannot open ":CUR:")"
         STOP
      END
      READ PKGREC FROM PV, "PKG" ELSE
         PRINT CUR:" is not a package (no PKG manifest)"
         STOP
      END
      GOSUB 9000
      * the manifest's systems field (attr 4) lists the MV platforms the
      * package targets; refuse one that declares systems but not this one.
      IF PSYS # "" THEN
         OKSYS = 0
         NSY = DCOUNT(PSYS, " ")
         FOR SI = 1 TO NSY
            IF FIELD(PSYS, " ", SI) = "mvx" THEN OKSYS = 1
         NEXT SI
         IF OKSYS = 0 THEN
            PRINT CUR:" does not support mvx (systems: ":PSYS:")"
            STOP
         END
      END
      LOCATE(PNAME, LNAMES; POS) THEN SKIP = 1
   END
   IF SKIP = 0 THEN
      PKGS<-1> = CUR
      LNAMES<-1> = PNAME
      ADDED<-1> = CUR
      DEPLIST = PDEPS
      ND = DCOUNT(DEPLIST, @AM)
      FOR DI = 1 TO ND
         D = DEPLIST<DI>
         * a '+' prefix marks a BUILD dependency — needed to compile the
         * package, never to run it (mvpkg supplies the shared PLATFORM.H).
         * Linking is a runtime concern, so skip it entirely.
         IF D[1, 1] = "+" THEN GOTO 9100
         * a '?' prefix marks an optional dependency: linked if resolvable
         * (the full package overrides), else assumed satisfied by a copy the
         * requiring package bundles (e.g. git's CMD.BP) — so do not fail.
         OPTDEP = 0
         IF D[1, 1] = "?" THEN OPTDEP = 1 ; D = D[2, LEN(D)]
         LOCATE(D, LNAMES; POS) THEN
            X = 0
         END ELSE
            GOSUB 9200
            IF RPATH # "" THEN
               QUEUE<-1> = RPATH
            END ELSE
               IF OPTDEP = 0 THEN
                  PRINT "cannot resolve dependency '":D:"' of ":CUR
                  PRINT "searched: linked packages, ":CUR:"/../, $MVXPKGPATH"
                  STOP
               END
            END
         END
* next dependency — a build dep jumps here, skipping the link
9100
      NEXT DI
   END
REPEAT
IF ADDED = "" THEN
   PRINT P:" is already linked"
   STOP
END
WRITE PKGS ON ACC, "PACKAGES"
NA = DCOUNT(ADDED, @AM)
FOR LI = 1 TO NA
   PRINT "linked ":ADDED<LI>
NEXT LI
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
         IF MF<MI> # "" THEN
            PDEPS<-1> = MF<MI>
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
RETURN

* ---- 9200: resolve dependency name D near CUR -> RPATH -----------------
9200
RPATH = ""
LS = 0
FOR MI = 1 TO LEN(CUR)
   IF CUR[MI, 1] = "/" THEN LS = MI
NEXT MI
CAND = CUR[1, LS]:D
OPEN CAND:"/VOC" TO TV THEN
   RPATH = CAND
   RETURN
END
PP = ENV("MVXPKGPATH")
NSEG = DCOUNT(PP, ":")
FOR MI = 1 TO NSEG
   SEG = FIELD(PP, ":", MI)
   IF SEG # "" THEN
      CAND = SEG:"/":D
      OPEN CAND:"/VOC" TO TV THEN
         RPATH = CAND
         RETURN
      END
   END
NEXT MI
RETURN
