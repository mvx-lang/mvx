* INSERT and DELETE on a dynamic array.
*
* Both go through the general rebuild: the whole value is walked and copied
* into a fresh buffer, and the element index is thrown away.  A list that is
* being maintained -- the ordinary "keep it in order, add and remove" shape --
* pays that on every change.
*
* The list is kept at a stable length so the measurement is of the operations
* and not of a value that grows.
      TPS = 1000
      AM = CHAR(254)
      N = 2000
      SECS = 5
      LIST = ""
      FOR I = 1 TO N
         K = "row" : FMT(I, "R%6")
         IF I = 1 THEN LIST = K ELSE LIST = LIST : AM : K
      NEXT I
*
      OPS = 0
      SEED = 12345
      START = SYSTEM(12)
      LIMIT = START + SECS * TPS
      LOOP
      WHILE SYSTEM(12) < LIMIT DO
         FOR J = 1 TO 50
            SEED = MOD(SEED * 1103515245 + 12345, 2147483648)
            P = MOD(SEED, N) + 1
            LIST = INSERT(LIST, P, 0, 0, "inserted")
            LIST = DELETE(LIST, P, 0, 0)
            OPS = OPS + 2
         NEXT J
      REPEAT
      ELAPSED = (SYSTEM(12) - START) / TPS
      PRINT "ops: ":OPS:", time: ":ELAPSED:", ops/sec: ":INT(OPS/ELAPSED)
      C = DCOUNT(LIST, AM)
      PRINT "elements at end: ":C
      IF C = N THEN PRINT "VALID" ELSE PRINT "INVALID"
      PRINT "insdel;":INT(OPS/ELAPSED):";":ELAPSED:";1;elements=":N
      END
