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
*  * @file VERSION
*  * @version 1.0
*  */
* VERSION — what this toolchain is, from the TCL prompt (#117).
*
* Two numbers, and they answer different questions.  The VERSION is which
* release you are on, and it is what a package's `requires` range is matched
* against.  The DRIVER ABI is what decides whether a compiled artifact -- a
* storage driver, a package's LIB library -- can still be loaded at all: two
* releases sharing an ABI are interchangeable to one, and a bump is exactly
* what makes an older one refuse to load.
*
* A build from an untagged checkout reports 0.0.0-dev, which no `requires`
* range will satisfy.  That is deliberate: saying "unknown" is better than
* claiming a version the binary may not have.
*
* This is BASIC, not C, because it can be: MVXVERSION() and SYSTEM(1001)
* were added for the install-time check and the verb is just a reader.
      V = MVXVERSION()
      A = SYSTEM(1001)
      S = TRIM(SENTENCE())
      W = OCONV(FIELD(S, " ", 2), "MCU")
      BEGIN CASE
      CASE W = "ABI"
         PRINT A
      CASE W = "SHORT" OR W = ""
         IF W = "SHORT" THEN
            PRINT V
         END ELSE
            PRINT "mvx ":V
            PRINT "driver ABI ":A
         END
      CASE 1
         PRINT "usage: VERSION {SHORT | ABI}"
      END CASE
