* Messaging through the registry (mvx#228), from one session to itself.
*
* Deterministic by construction: ONE session, which sends and then drains.
* A receiver does not have to be running when a message is sent -- the inbox
* lives in the daemon -- and that is what lets the suite avoid two live
* sessions racing each other.
P = @USERNO
PRINT "port=":P
PRINT "to self=":MSGSEND("!":P, "one")
PRINT "to nobody=":MSGSEND("!4000", "into the void")
* The port prefix is the account name, which is also how an account is
* addressed -- so ask the registry rather than hard-coding it.
ACCT = MSGSTATUS()<1,4>
PRINT "to my account=":MSGSEND(ACCT, "by account")
PRINT "to me by name=":MSGSEND("@":ENV("USER"), "by user")
PRINT "wall=":MSGSEND("*", "to everyone")
PRINT "pending=":MSGPENDING()
LOOP WHILE MSGPENDING() > 0 DO
   M = MSGREAD()
   IF M = "" THEN EXIT
   CLASS = M<2>
   KIND = "direct"
   IF MOD(CLASS, 16) = 2 THEN KIND = "wall"
*  The port, not the user: a golden file must not depend on who ran it.
   PRINT "  ":KIND:" from port ":M<3>:": ":M<8>
REPEAT
PRINT "drained=":MSGPENDING()
*
* MODE OFF discards an ordinary message on arrival -- but not a wall, which
* is not the user's to suppress.
PRINT "mode was ":MSGMODE("OFF")
PRINT "direct while off=":MSGSEND("!":P, "muted"):" pending=":MSGPENDING()
PRINT "wall while off=":MSGSEND("*", "not muted"):" pending=":MSGPENDING()
M = MSGREAD()
PRINT "the one that got through: ":M<8>
PRINT "mode was ":MSGMODE("ON")
*
* The inbox is bounded and drops the OLDEST, counting what it lost.
FOR I = 1 TO 300
   X = MSGSEND("!":P, "flood ":I)
NEXT I
PRINT "queued=":MSGPENDING()
PRINT "dropped=":MSGDROPPED()
FIRST = MSGREAD()
PRINT "oldest kept: ":FIRST<8>
