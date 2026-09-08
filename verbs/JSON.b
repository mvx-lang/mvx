* MVX — a native compiler and runtime for Pick/MultiValue BASIC.
* Copyright (C) 2026 Gordon Heydon.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License, version 2, as
* published by the Free Software Foundation.  There is NO WARRANTY, to
* the extent permitted by law; see the LICENSE file for details.
*
* SPDX-License-Identifier: GPL-2.0-only
* JSON {DICT} file id — show a record as a JSON object.  The mapping is
* derived from the file's dictionary (MAPSPEC): single-valued D-items become
* scalar keys, associated ones become an array of objects.  The record id is
* added as the leading "id" key.
S = TRIM(SENTENCE())
NAME = FIELD(S, " ", 2)
DICTF = 0
IDPOS = 3
IF NAME = "DICT" THEN
   DICTF = 1
   NAME = FIELD(S, " ", 3)
   IDPOS = 4
END
ID = FIELD(S, " ", IDPOS)
IF NAME = "" OR ID = "" THEN
   PRINT "usage: JSON {DICT} filename id"
   STOP
END
IF DICTF THEN
   OPEN "DICT", NAME TO F ELSE
      PRINT "cannot open DICT ":NAME
      STOP
   END
   SPEC = ""
END ELSE
   OPEN NAME TO F ELSE
      PRINT "cannot open ":NAME
      STOP
   END
   SPEC = MAPSPEC(NAME)
END
READ R FROM F, ID THEN
   J = JSONENCODE(R, SPEC)
   * splice the id in as the first key ("{" ... -> {"id":"..", ...})
   REST = J[2, LEN(J)]
   HEAD = '{"id":"' : ID : '"'
   IF REST = "}" THEN
      PRINT HEAD : "}"
   END ELSE
      PRINT HEAD : "," : REST
   END
END ELSE
   PRINT ID:" not on file"
END