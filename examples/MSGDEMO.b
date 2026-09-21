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
*  * @file MSGDEMO
*  * @version 1.0
*  */
* MSGDEMO — a full-screen program that takes messages without losing its form.
*
* THE GUARANTEE IS STRUCTURAL.  Nothing in the messaging path writes to this
* program's screen: a message sits in the session's inbox until the program
* asks for it, so the only thing that can paint over the form is the program
* itself.  Here it paints on the bottom line and puts the cursor back.
*
* The poll is the KEYIN tick, which a full-screen program has anyway.  A
* program that never calls MSGPENDING simply never sees a message -- which is
* the right default for a report that runs for an hour.
*
* Try it: run this, then from another session   MSG !<port> hello
   WIDE = SYSTEM(2)
   DEEP = SYSTEM(3)
   MSGLINE = DEEP - 1
*  clear the screen and hide the cursor
   PRINT @(-1):@(-7):
   GOSUB PAINT
   COUNT = 0
   PENDFILE = ""
   PENDID = ""
   LOOP
*     the tick, and the message poll in one
      K = KEYIN(250)
      IF MSGPENDING() > 0 THEN GOSUB SHOWMSG
      IF K = "" THEN CONTINUE
      IF K = CHAR(27) OR K = "q" THEN EXIT
      IF K = "v" THEN
         GOSUB SHOWREC
         CONTINUE
      END
      COUNT = COUNT + 1
      GOSUB PAINT
   REPEAT
*  cursor back, screen clear
   PRINT @(-8):@(-1):
   PRINT "left after ":COUNT:" keystroke(s)"
   STOP
*
PAINT:
   PRINT @(0, 0):@(-4):"MSGDEMO — port ":@USERNO:", keys pressed: ":COUNT
   PRINT @(0, 2):"This is the form.  A message must not disturb it."
   PRINT @(0, 3):"Send one from another session:  MSG !":@USERNO:" hello"
   PRINT @(0, 4):"Or with a record:  MSG !":@USERNO:" WITH ORDERS O1234 look"
   PRINT @(0, 5):"Press v to open an attached record, q or ESC to leave."
   RETURN
*
* One message at a time, on the bottom line, cursor restored.  The class says
* how it was sent: a wall message is marked, and a bell is rung only if the
* sender asked for one.
SHOWMSG:
   LOOP WHILE MSGPENDING() > 0 DO
      M = MSGREAD()
      IF M = "" THEN EXIT
      CLASS = M<2>
      SENDER = M<3>:"/":M<4>
      TEXT = M<8>
      MARK = "***"
*     a wall broadcast is marked as one
      IF MOD(CLASS, 16) = 2 THEN MARK = "ALL"
      LINE = MARK:" ":SENDER:": ":TEXT
*     AN ATTACHED RECORD IS AN OFFER (mvx#238).  Attribute 9 says where a
*     record is -- file, id, program, mode -- and this program decides what
*     to do about it.  Here it says so on the status line and waits to be
*     asked; a real screen would offer to open it, in the mode the sender
*     suggested, and offer VIEW instead when the record is locked elsewhere.
      PAY = M<9>
      IF PAY # "" THEN
         LINE = LINE:"  [":PAY<1,1>:" ":PAY<1,2>:" - press v to ":PAY<1,4>:"]"
         PENDFILE = PAY<1,1>
         PENDID = PAY<1,2>
      END
      PRINT @(0, MSGLINE):@(-4):LINE[1, WIDE - 1]:
*     ring the bell only if the sender asked for one
      IF MOD(INT(CLASS / 16), 2) = 1 THEN PRINT CHAR(7):
   REPEAT
   RETURN
*
* The offer, taken.  Nothing here happened because a message arrived: it
* happened because somebody pressed a key.
SHOWREC:
   IF PENDFILE = "" THEN RETURN
   OPEN PENDFILE TO PF THEN
      READ PR FROM PF, PENDID THEN
         PRINT @(0, 7):@(-4):"--- ":PENDFILE:" ":PENDID:" ---"
         FOR A = 1 TO DCOUNT(PR, @AM)
            PRINT @(0, 7 + A):@(-4):A:"  ":PR<A>[1, WIDE - 6]
         NEXT A
      END ELSE
         PRINT @(0, 7):@(-4):PENDID:" is no longer on file"
      END
   END ELSE
      PRINT @(0, 7):@(-4):"cannot open ":PENDFILE
   END
   RETURN
