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
*  * @file WHO
*  * @version 2.0
*  */
* WHO — this session: its port, user and account (mvx#226).
*
* The port comes from the session registry.  WITHOUT A REGISTRY THIS STILL
* ANSWERS: a host with no mvx-msgd running prints what WHO has always printed,
* because "who am I" is a question the shell can answer by itself and a missing
* daemon must never take a working verb away.
ST = MSGSTATUS()
IF ST<1,1> = "up" THEN
   PRINT @USERNO:" ":ENV("USER"):" ":ENV("MVXACCTPATH")
END ELSE
   PRINT ENV("USER"):" ":ENV("MVXACCTPATH")
END
