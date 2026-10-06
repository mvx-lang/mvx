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
*  * @file FIX-POINTERS
*  * @version 1.0
*  */
* FIX-POINTERS {LISTONLY} — bring an account's VOC file pointers up to date.
*
* Stage 2 of mvx#318 writes a pointer when a file is CREATED, so an account
* nobody has created a file in since then still carries the pre-stage-2
* form -- "F / name / name.DICT" -- for every file in it.  That form is
* IGNORED rather than trusted, so nothing is broken: resolution falls back
* to deriving the location, which is what it did before pointers existed.
*
* SO THIS IS CLEANUP, NOT A REPAIR.  What it buys is that resolution reads
* the account's own statement instead of deriving it, which among other
* things stops it asking the filesystem whether each file is a directory on
* every open.
*
* IT DOES NOT LET THE TOLERANCE BE DELETED, which is worth saying because
* the issue hoped it would.  Deleting it would make a legacy pointer
* BELIEVED, and a hash file's dictionary looked for at <name>.DICT rather
* than DICT.<name> -- so it can only go once every account anywhere has
* been swept, which is not a thing anyone can know.
*
* ONLY THE LEGACY FORM IS TOUCHED.  POINTERFIX reuses the same rule
* CREATE-FILE has always used: overwrite the pre-stage-2 form byte for
* byte, and leave anything else -- anybody's own edit, with their own
* attributes past the third -- exactly as it is.
S = TRIM(SENTENCE())
LISTONLY = 0
NW = DCOUNT(S, " ")
FOR I = 2 TO NW
   W = OCONV(FIELD(S, " ", I), "MCU")
   BEGIN CASE
   CASE W = "LISTONLY"
      LISTONLY = 1
   CASE 1
      PRINT "FIX-POINTERS: do not understand ":FIELD(S, " ", I)
      PRINT "usage: FIX-POINTERS {LISTONLY}"
      STOP
   END CASE
NEXT I

L = FILELIST()
N = DCOUNT(L, @AM)
NFIX = 0
NLEFT = 0
FIXED = ""
FOR K = 1 TO N
   E = L<K>
   NM = E<1, 1>
   IF LISTONLY THEN
      * Say what WOULD change without changing it.  POINTERFIX has no dry
      * run -- it is the thing that writes -- so the plan is read from the
      * pointer here instead.
      OPEN "VOC" TO V ELSE
         PRINT "FIX-POINTERS: cannot open VOC"
         STOP
      END
      READ R FROM V, NM THEN
         IF R = "F":@AM:NM:@AM:NM:".DICT" THEN
            FIXED<-1> = NM
            NFIX = NFIX + 1
         END ELSE
            NLEFT = NLEFT + 1
         END
      END ELSE
         NLEFT = NLEFT + 1
      END
   END ELSE
      IF POINTERFIX(NM) THEN
         FIXED<-1> = NM
         NFIX = NFIX + 1
      END ELSE
         NLEFT = NLEFT + 1
      END
   END
NEXT K

IF NFIX = 0 THEN
   PRINT "FIX-POINTERS: every pointer is already current (":NLEFT:" file(s))"
   STOP
END
IF LISTONLY THEN
   PRINT "FIX-POINTERS: ":NFIX:" pointer(s) would be rewritten"
END ELSE
   PRINT "FIX-POINTERS: ":NFIX:" pointer(s) rewritten"
END
FOR K = 1 TO NFIX
   PRINT "  ":FIXED<K>
NEXT K
PRINT "  ":NLEFT:" left alone (already current, or not ours to change)"
IF LISTONLY THEN PRINT "listonly: nothing was changed"
