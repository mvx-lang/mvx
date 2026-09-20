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
*  * @file MSG
*  * @version 1.0
*  */
* MSG / MESSAGE — send a message to a port, a range, an account, a user, or
* every port at once (mvx#228).  The classic sentence forms:
*
*    MSG{n} * text                 every logged-on port (the wall broadcast)
*    MSG{n} !7 text                one port
*    MSG{n} !5-9 text              a range of ports
*    MSG{n} SALES,PAYROLL text     every port logged to those accounts
*    MSG{n} @fred text             every port that user is on
*
* A port that is not logged on gets nothing, as classic Pick did -- there is
* nowhere to put it -- and MSG says how many ports it actually reached.
*
* THE VARIANT DIGIT CHOOSES THE CLASS, not the position on the screen.  In
* R83 the sender picked the status line or the cursor and the receiving
* program had no say; here the class travels with the message and the
* RECEIVER decides, which is what keeps a full-screen program intact.
*
*    MSG   signed             MSG1  unsigned
*    MSG2  signed, cursor     MSG3  unsigned, cursor
*    MSG4  signed, bell       MSG5  signed, cursor, bell
*
* AND A MESSAGE CAN CARRY A RECORD (mvx#238):
*
*    MSG !7 WITH ORDERS O1234 have a look at this
*
* which sends the message AND an attachment saying where to find the
* record.  The receiving terminal OFFERS to open it -- it puts the command
* on the stack and says so -- so it opens because the person at it chose to,
* never because the sender said so.
S = SENTENCE()
VERB = FIELD(S, " ", 1)
TARGET = FIELD(S, " ", 2)
FILENAME = ""
RECID = ""
IF OCONV(FIELD(S, " ", 3), "MCU") = "WITH" THEN
   FILENAME = FIELD(S, " ", 4)
   RECID = FIELD(S, " ", 5)
   TEXT = FIELD(S, " ", 6, 999)
END ELSE
   TEXT = FIELD(S, " ", 3, 999)
END
IF TARGET = "" OR TEXT = "" THEN
   PRINT "usage: MSG{n} (* | !port{-port} | account{,account} | @user)"
   PRINT "           {WITH file id} text"
   STOP
END
* DON'T SEND A POINTER TO SOMETHING THAT IS NOT THERE.  The receiver would
* find out by trying to open it, long after the sender had gone.
IF FILENAME # "" THEN
   IF RECID = "" THEN
      PRINT "MSG: WITH needs a file and a record id"
      STOP
   END
   OPEN FILENAME TO ATTF ELSE
      PRINT "MSG: cannot open ":FILENAME
      STOP
   END
   READ ATTR FROM ATTF, RECID ELSE
      PRINT "MSG: ":RECID:" is not on file in ":FILENAME
      STOP
   END
END
* The class, from the digit on the verb.
EQU CSTATUS TO 0
EQU CCURSOR TO 1
EQU FBELL TO 16
EQU FSIGNED TO 32
DIGIT = VERB[4, 9]
IF VERB[1, 7] = "MESSAGE" THEN DIGIT = VERB[8, 9]
BEGIN CASE
   CASE DIGIT = "1" ; CLASS = CSTATUS
   CASE DIGIT = "2" ; CLASS = CCURSOR + FSIGNED
   CASE DIGIT = "3" ; CLASS = CCURSOR
   CASE DIGIT = "4" ; CLASS = CSTATUS + FSIGNED + FBELL
   CASE DIGIT = "5" ; CLASS = CCURSOR + FSIGNED + FBELL
   CASE 1           ; CLASS = CSTATUS + FSIGNED
END CASE
* The classic limits: a message meant for a status line has to fit on one,
* and one meant for the cursor may be longer.  Truncation is said out loud
* rather than done quietly.
LIMIT = 63
IF MOD(CLASS, 16) = CCURSOR THEN LIMIT = 250
IF LEN(TEXT) > LIMIT THEN
   PRINT "MSG: text truncated to ":LIMIT:" characters"
   TEXT = TEXT[1, LIMIT]
END
* The payload convention (mvx#238): file, id, program, mode, by value mark.
* Positions 5 and beyond are free for an application to use.  MODE is what
* the receiver should OFFER -- a verb sends "view", because someone handing
* you a record to look at is not asking you to change it.
PAYLOAD = ""
IF FILENAME # "" THEN
   PAYLOAD = FILENAME:@VM:RECID:@VM:"":@VM:"view"
END
N = MSGSEND(TARGET, TEXT, CLASS, PAYLOAD)
BEGIN CASE
   CASE N > 0
      PLURAL = "s"
      IF N = 1 THEN PLURAL = ""
      PRINT "sent to ":N:" port":PLURAL:
      IF FILENAME # "" THEN PRINT ", with ":FILENAME:" ":RECID:
      PRINT ""
   CASE N = 0
*     Nothing here matched.  If a message service is carrying this prefix the
*     message has gone to it, and a port of that number on another host will
*     have received it -- this daemon cannot say so until the roster spans
*     hosts (presence).  Saying "nobody" would be wrong, and saying "sent to
*     1" would be a guess; say what is actually known.
      ST = MSGSTATUS()
      IF ST<1,1> = "up" AND ST<1,2> # "loop" AND TARGET[1, 1] = "!" THEN
         PRINT "MSG: no port here is logged on as ":TARGET:
         PRINT "; forwarded to the ":ST<1,2>:" service"
      END ELSE
         PRINT "MSG: no logged-on port matches ":TARGET
      END
   CASE N = -1
      * the runtime has already said which permit is missing
      STOP
   CASE 1
      PRINT "MSG: no session registry is running (start mvx-msgd)"
END CASE
