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
      * WHICH manifest is 9000's business, not this gate's.  This used to READ
      * "PKG" itself and stop when it was missing, which is how a package that
      * carries only mvpkg.json was told it is not a package (mvx#285).  9000
      * reports what it found in PMAN; empty means neither manifest is there.
      GOSUB 9000
      IF PMAN = "" THEN
         PRINT CUR:" is not a package (no PKG or mvpkg.json manifest)"
         STOP
      END
      * the manifest's systems field (attr 4) lists the MV platforms the
      * package targets; refuse one that declares systems but not this one.
      * The declared runtime requirement, checked BEFORE anything is linked.
      * Refusing is the whole point: without this the install succeeds and the
      * failure arrives later as an undefined symbol at dlopen, a long way
      * from what caused it (#117).
      NRQ = DCOUNT(PREQ, @AM)
      FOR RI = 1 TO NRQ
         RQ = PREQ<RI>
         GOSUB 9300
         IF REQOK = 0 THEN
            PRINT CUR:" needs ":RQNAME:" ":RQOP:RQWANT:
            PRINT ", but this is ":RQHAVE
            STOP
         END
      NEXT RI
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
* 9300: is requirement RQ satisfied?  RQ is "mvx>=0.1.2" or "mvx-abi>=14".
* Sets REQOK, and RQNAME/RQOP/RQWANT/RQHAVE for the message.
*
* Only >= is honoured, and a bare version means >=.  A package saying it needs
* at least some version is the case that matters; the rest of a range grammar
* would be more parser than the problem has earned, and guessing at "<" or "~"
* semantics is how a check starts refusing things it should not.
9300
   REQOK = 1
   RQOP = ">="
   RQNAME = RQ
   RQWANT = ""
   P = INDEX(RQ, ">=", 1)
   IF P > 0 THEN
      RQNAME = RQ[1, P - 1]
      RQWANT = RQ[P + 2, LEN(RQ)]
   END ELSE
      P = INDEX(RQ, "=", 1)
      IF P > 0 THEN
         RQNAME = RQ[1, P - 1]
         RQWANT = RQ[P + 1, LEN(RQ)]
      END
   END
   IF RQWANT = "" THEN RETURN        ;* nothing asked for
   BEGIN CASE
   CASE RQNAME = "mvx-abi"
      RQHAVE = SYSTEM(1001)
      IF RQHAVE < RQWANT THEN REQOK = 0
   CASE RQNAME = "mvx"
      RQHAVE = MVXVERSION()
      * AN UNKNOWN VERSION WARNS, IT DOES NOT REFUSE.  A build from an
      * untagged checkout -- every CI run and most developer trees -- reports
      * 0.0.0-dev, and refusing everything then would block all package use on
      * exactly the builds people work in.  The issue asks for a refusal only
      * where "the version data is reliable"; here it says out loud that it is
      * not.  The ABI below is always reliable, being a compile-time constant,
      * so that one still refuses.
      IF RQHAVE[1, 5] = "0.0.0" THEN
         PRINT "warning: ":CUR:" wants mvx ":RQOP:RQWANT:
         PRINT ", and this build does not know its version (":RQHAVE:")"
         RETURN
      END
      GOSUB 9400
      IF VCMP < 0 THEN REQOK = 0
   CASE 1
      RETURN                         ;* a requirement we do not know: ignore
   END CASE
   RETURN

* 9400: compare RQHAVE against RQWANT as dotted numbers -> VCMP (-1/0/1).
* Only the numeric x.y.z prefix is compared: a develop build describes itself
* as 0.1.4-27-gabc1234, and what that means for a ">=0.1.4" test is that it is
* newer than 0.1.4, so the suffix is dropped rather than parsed.
9400
   VCMP = 0
   HV = FIELD(RQHAVE, "-", 1)
   WV = RQWANT
   FOR VI = 1 TO 3
      HP = FIELD(HV, ".", VI)
      WP = FIELD(WV, ".", VI)
      IF HP = "" THEN HP = 0
      IF WP = "" THEN WP = 0
      IF VCMP = 0 THEN
         IF HP + 0 < WP + 0 THEN VCMP = -1
         IF HP + 0 > WP + 0 THEN VCMP = 1
      END
   NEXT VI
   RETURN

9000
PNAME = ""
PVER = ""
PSYS = ""
PDEPS = ""
PREQ = ""
PMAN = ""
OPEN CUR TO MPD THEN
   READ MF FROM MPD, "PKG" THEN
      PMAN = "PKG"
      PNAME = MF<1>
      PVER = MF<2>
      PSYS = MF<4>
      MN = DCOUNT(MF, @AM)
      FOR MI = 5 TO MN
         IF MF<MI> # "" THEN
            * '!' marks a RUNTIME requirement rather than a package
            * dependency: !mvx>=0.1.2 says which mvx this package needs.
            * A prefix, because dependencies run from attribute 5 to the end
            * of the record and a fixed attribute number would collide with
            * them -- the manifest already uses '?' for optional and '+' for
            * build-only, so this follows the grammar that is there (#117).
            IF MF<MI>[1, 1] = "!" THEN
               PREQ<-1> = MF<MI>[2, LEN(MF<MI>)]
            END ELSE
               PDEPS<-1> = MF<MI>
            END
         END
      NEXT MI
   END
END
* NO PKG?  THE OTHER MANIFEST (mvx#285).  mv_package dropped PKG -- "PKG is
* gone; mvpkg.json is the manifest" is one of its own tests -- and every reader
* here looked only for PKG, so linking it answered "is not a package".  PKG is
* still tried FIRST: mvx's own packages/http carries one and no mvpkg.json.
IF PMAN = "" THEN GOSUB 9500
IF PNAME = "" THEN
   LS = 0
   FOR MI = 1 TO LEN(CUR)
      IF CUR[MI, 1] = "/" THEN LS = MI
   NEXT MI
   PNAME = CUR[LS + 1, LEN(CUR)]
END
RETURN

* ---- 9500: the same five fields out of mvpkg.json ----------------------
* Scanned, not JSONDECODE'd.  JSONDECODE is an extension the json PACKAGE
* provides, and a standard verb cannot depend on a package being installed --
* the same bootstrap rule mvpkg follows for its own transport.  Five fields are
* wanted, two flat strings and three string arrays, which is less code than the
* dependency would be.
*
* THE TWO MANIFESTS NAME THINGS DIFFERENTLY, and this is the part to get right.
* PKG holds short names (`cmd`, depending on `getopt`); mvpkg.json holds
* owner-qualified ones (`mvx-lang/cmd`, depending on `mvx-lang/getopt`).  9200
* resolves a dependency by looking for a SIBLING DIRECTORY of that name, so an
* owner prefix would send it looking for `<parent>/mvx-lang/getopt`.  Every name
* taken from here is reduced to its last path segment.
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
   * the prefixes are the same grammar PKG uses, and they sit OUTSIDE the name,
   * so strip them, reduce the name, then put them back.
   MJPFX = ""
   IF MJD[1, 1] = "?" THEN MJPFX = "?" ; MJD = MJD[2, LEN(MJD)]
   IF MJD[1, 1] = "!" THEN
      PREQ<-1> = MJD[2, LEN(MJD)]
   END ELSE
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
         IF MJB # "" THEN PDEPS<-1> = MJPFX : MJB
      END
   END
NEXT MI
* devDependencies are build-only, which is exactly what '+' means in PKG.
MJKEY = "devDependencies" ; GOSUB 9520
MJN = DCOUNT(MJLIST, @AM)
FOR MI = 1 TO MJN
   MJB = MJLIST<MI> ; GOSUB 9530
   IF MJB # "" THEN PDEPS<-1> = "+" : MJB
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
