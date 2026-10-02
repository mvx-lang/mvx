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
*  * @file CONVERT-ACCOUNT
*  * @version 1.0
*  */
* CONVERT-ACCOUNT newtype {FROM oldtype} {CONNECTION conn} {LISTONLY}
*
* Move a whole account's files to another backend, one CONVERT-FILE at a
* time.  Written because deprecating a driver (mvx#327) otherwise reads
* "run CONVERT-FILE once for every file you have", which invites missing
* one.
*
* FROM is the deprecation case: convert what is on the backend being
* retired and leave the rest alone.  LISTONLY prints the plan and changes nothing -- a
* whole-account conversion is not something to learn the shape of by
* running it.
*
* A DIRECTORY FILE IS SKIPPED unless FROM dir names it.  The point of a
* dir file is that its records are OS files a person reads and git diffs;
* converting one to a hash backend destroys the property it was made for.
* So the obvious command does the useful thing, not the destructive one.
*
* VOC GOES LAST, as a precaution and not as a repair.  It is where
* resolution starts, so it is the one file whose move changes where
* everything else is looked up.  Measured: converting it FIRST does not
* currently break the files after it, because each CONVERT-FILE runs in its
* own process (EXECUTE) and re-resolves against the new VOC, which
* CONVERT-FILE has already recorded in .mvx's `voc =` (mvx#187).  The order
* costs one line and stops being free to get wrong the moment a conversion
* runs in-process, so it is pinned here rather than relied on by accident.
* The test asserts the PLAN order, which is the honest thing to assert --
* there is no observable breakage to assert instead.
*
* CATALOG IS NEVER CONVERTED, not even by FROM dir.  It holds the compiled
* executables the dispatcher reaches by PATH -- a VOC verb record says
* "CATALOG/CREATE-FILE" and the shell execs it -- so converting it to a
* hash backend does not lose a property the way converting BP does, it
* stops every cataloged verb in the account from running.  FILELIST()
* reports it as a dir file, so the refusal has to be here.
S = TRIM(SENTENCE())
USAGE = "usage: CONVERT-ACCOUNT newtype {FROM oldtype} {CONNECTION conn} {LISTONLY}"
NT = FIELD(S, " ", 2)
IF NT = "" THEN
   PRINT USAGE
   STOP
END
FROMT = ""
CONN = ""
LISTONLY = 0
NW = DCOUNT(S, " ")
I = 3
LOOP
WHILE I <= NW DO
   W = OCONV(FIELD(S, " ", I), "MCU")
   BEGIN CASE
   CASE W = "FROM"
      FROMT = FIELD(S, " ", I + 1)
      I = I + 2
   CASE W = "CONNECTION"
      CONN = FIELD(S, " ", I + 1, 99)
      I = NW + 1
   CASE W = "LISTONLY"
      LISTONLY = 1
      I = I + 1
   CASE 1
      PRINT "CONVERT-ACCOUNT: do not understand ":FIELD(S, " ", I)
      PRINT USAGE
      STOP
   END CASE
REPEAT
IF FROMT # "" AND FROMT = NT THEN
   PRINT "CONVERT-ACCOUNT: FROM and the new type are both ":NT:" -- nothing to do"
   STOP
END

* ---- work out the plan before touching anything ----------------------
L = FILELIST()
N = DCOUNT(L, @AM)
PLAN = ""
VOCTOO = 0
NSKIP = 0
NCAT = 0
FOR K = 1 TO N
   E = L<K>
   NM = E<1, 1>
   TY = E<1, 2>
   KEEP = 1
   IF NM = "CATALOG" THEN
      * Always skipped, and said out loud only when FROM asked for it (see
      * the header).  It is not one of "n directory files left alone" either,
      * because that line offers FROM dir as the way to include them and this
      * is the one FROM dir will still refuse.
      KEEP = 0
      IF FROMT # "" AND TY = FROMT THEN NCAT = 1
   END ELSE
      IF TY = NT THEN KEEP = 0                   ;* already where it is going
      IF FROMT # "" AND TY # FROMT THEN KEEP = 0 ;* not the type asked for
      IF FROMT = "" AND TY = "dir" THEN
         KEEP = 0                                ;* see the header
         NSKIP = NSKIP + 1
      END
   END
   IF KEEP THEN
      IF NM = "VOC" THEN
         VOCTOO = 1
      END ELSE
         PLAN<-1> = NM
      END
   END
NEXT K
NP = DCOUNT(PLAN, @AM)
IF PLAN = "" THEN NP = 0

IF NP = 0 AND VOCTOO = 0 THEN
   PRINT "CONVERT-ACCOUNT: nothing to convert to ":NT
   IF NSKIP > 0 THEN
      PRINT "  (":NSKIP:" directory file(s) left alone -- name FROM dir to include them)"
   END
   IF NCAT THEN PRINT "  (CATALOG is never converted -- it holds executables)"
   STOP
END

* ---- say what will happen, and stop there if asked -------------------
PRINT "CONVERT-ACCOUNT: ":NP + VOCTOO:" file(s) to ":NT
FOR K = 1 TO NP
   PRINT "  ":PLAN<K>
NEXT K
IF VOCTOO THEN PRINT "  VOC (last)"
IF NSKIP > 0 THEN
   PRINT "  ":NSKIP:" directory file(s) left alone -- name FROM dir to include them"
END
IF NCAT THEN PRINT "  CATALOG is never converted -- it holds executables"
IF LISTONLY THEN
   PRINT "listonly: nothing was changed"
   STOP
END

* ---- convert, VOC last ------------------------------------------------
TAIL = NT
IF CONN # "" THEN TAIL = NT:" ":CONN
NOK = 0
NBAD = 0
STOPPED = 0
* IF THE FIRST ONE FAILS, STOP THERE.  A wrong target -- a backend this host
* does not have, a connection that will not open -- fails identically for
* every file, and an account of 300 would say so 300 times.  Nothing has been
* converted at that point, so stopping is also the safe direction: the
* operator fixes the target and runs the same command again.
* A failure LATER is a different thing.  The files before it are converted and
* staying converted, so the run carries on and the report names what did not
* make it (mvx#335: no rollback across an account).
FOR K = 1 TO NP
   EXECUTE "CONVERT-FILE ":PLAN<K>:" ":TAIL CAPTURING OUT
   IF INDEX(OUT, " converted to ", 1) > 0 THEN
      NOK = NOK + 1
   END ELSE
      NBAD = NBAD + 1
      PRINT "  FAILED ":PLAN<K>:": ":TRIM(OUT)
      IF NOK = 0 THEN
         PRINT "CONVERT-ACCOUNT: stopping -- the first file failed, so the"
         PRINT "  target is wrong rather than the file.  Nothing was converted."
         STOPPED = 1
         K = NP
      END
   END
NEXT K
IF STOPPED THEN STOP
IF VOCTOO THEN
   EXECUTE "CONVERT-FILE VOC ":TAIL CAPTURING OUT
   IF INDEX(OUT, " converted to ", 1) > 0 THEN
      NOK = NOK + 1
   END ELSE
      NBAD = NBAD + 1
      PRINT "  FAILED VOC: ":TRIM(OUT)
   END
END
PRINT "converted ":NOK:" file(s) to ":NT
IF NBAD > 0 THEN
   PRINT NBAD:" file(s) could not be converted; the rest are done"
END
