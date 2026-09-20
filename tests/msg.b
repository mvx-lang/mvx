* Messaging with NO session registry running (mvx#226).
*
* This is the guard on the invariant the whole feature rests on: a missing
* mvx-msgd must cost a program nothing.  Every phase-1 test runs without a
* daemon, so if anyone ever makes the client fatal -- the way the lmdbnet
* driver is fatal when its daemon goes, which is right for storage and
* catastrophic here -- this file fails first and loudest.
PRINT "port=":@USERNO
S = MSGSTATUS()
PRINT "state=":S<1,1>
PRINT "port-field=":S<1,3>
W = MSGWHO()
PRINT "who=[":W:"]"
N = 0
IF W # "" THEN N = DCOUNT(W, @AM)
PRINT "sessions=":N
* Sending, too, must be harmless with nothing to send through.
PRINT "send=":MSGSEND("!1", "nobody is listening")
PRINT "read=[":MSGREAD():"]"
PRINT "dropped=":MSGDROPPED()
PRINT "still running"
