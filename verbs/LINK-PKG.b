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
OPEN CUR TO MPD THEN
   READ MF FROM MPD, "PKG" THEN
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
