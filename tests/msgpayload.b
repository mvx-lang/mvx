* Payloads and the statement spellings (mvx#238), from one session to itself.
*
* Deterministic by construction, like msgsend.b: ONE session, which sends and
* then drains.  The inbox lives in the daemon, so a receiver does not have to
* be running when a message is sent.
P = @USERNO
PRINT "port=":P
*
* THE STATEMENTS ARE SPELLINGS, NOT A SECOND IMPLEMENTATION.  SEND goes
* through MSGSEND and takes THEN when it reached somebody -- which is `> 0',
* not the bare count, because -1 (denied) and -2 (no transport) are both TRUE
* to an MV IF and would otherwise take THEN on a message that went nowhere.
SEND "by statement" TO P THEN
   PRINT "send-then=yes"
END ELSE
   PRINT "send-then=no"
END
SEND "into the void" TO 4000 THEN
   PRINT "send-void=yes"
END ELSE
   PRINT "send-void=no"
END
SEND "no clause at all" TO P
PRINT "pending after three sends=":MSGPENDING()
*
* MESSAGE is the statement form of MSGMODE, and answers the mode it replaced.
MESSAGE OFF
PRINT "muted send=":MSGSEND("!":P, "while off"):" pending=":MSGPENDING()
MESSAGE ON
PRINT "mode restored=":MSGMODE("ON")
*
* NEITHER NAME IS RESERVED.  `MESSAGE' is exactly what a program would call a
* variable; taking it away to add a statement would break working code.
SEND = "still a variable"
MESSAGE = "so is this"
PRINT SEND:" / ":MESSAGE
*
* THE PAYLOAD: attribute 9, value-marked -- file, id, program, mode.  The
* receiver decides what to do with it, which is what makes this an offer and
* not remote control.
PAY = "ORDERS":@VM:"O1234":@VM:"":@VM:"view"
PRINT "with payload=":MSGSEND("!":P, "have a look", 0, PAY)
GOT = 0
LOOP WHILE MSGPENDING() > 0 DO
   M = MSGREAD()
   IF M = "" THEN EXIT
   IF M<9> # "" THEN
      GOT = GOT + 1
      PRINT "attached: file=":M<9,1>:" id=":M<9,2>:" mode=":M<9,4>
      PRINT "  free positions are empty: [":M<9,5>:"]"
   END
REPEAT
PRINT "payloads seen=":GOT
PRINT "drained=":MSGPENDING()
*
* A message with no attachment carries an empty attribute 9, not a missing
* one -- so a reader can always ask without checking first.
X = MSGSEND("!":P, "plain")
M = MSGREAD()
PRINT "plain payload=[":M<9>:"] text=":M<8>
