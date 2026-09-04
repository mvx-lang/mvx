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
*  * @file LISTF
*  * @version 1.0
*  */
* LISTF — the MV files in this account.
L = FILELIST()
N = DCOUNT(L, @AM)
PRINT FMT("File", "L#24"):" Type"
FOR I = 1 TO N
   E = L<I>
   NM = E<1, 1>
   TY = E<1, 2>
   * The type IS the driver's name, sent by the runtime.  There was a CASE
   * ladder here mapping one-letter codes to names, which meant every new
   * backend needed an edit in this verb as well as in the runtime -- and if
   * you forgot, the letter leaked out to the user.
   PRINT FMT(NM, "L#24"):" ":TY
NEXT I
PRINT N:" file(s)"
