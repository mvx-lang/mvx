* The sort inside the SORT and LIST verbs.
*
* verbs/SORT.b builds its ordered result by inserting each key into a growing
* list at the position an ordered LOCATE returns:
*
*   LOCATE(K, KEYS; POS; BORD) THEN ... KEYS = INSERT(KEYS, POS, 0, 0, K)
*
* That is an insertion sort, and both of its halves are runtime operations --
* find the position, then open a gap for it.  This measures the loop itself, at
* the sizes a query actually returns, so the verb's cost is visible without a
* file, a dictionary and an account in the way.
      TPS = 1000
      AM = CHAR(254)
      N = 4000
      SEED = 20260827
      KEYS = "" ; IDS = ""
      START = SYSTEM(12)
      FOR I = 1 TO N
         SEED = MOD(SEED * 1103515245 + 12345, 2147483648)
         K = "k" : FMT(MOD(SEED, 1000000), "R%7")
         LOCATE(K, KEYS; POS; "AL") ELSE NULL
         KEYS = INSERT(KEYS, POS, 0, 0, K)
         IDS  = INSERT(IDS,  POS, 0, 0, "id":I)
      NEXT I
      ELAPSED = (SYSTEM(12) - START) / TPS
      C = DCOUNT(KEYS, AM)
      PRINT "sorted ":C:" keys in ":ELAPSED:"s  (":INT(C/ELAPSED):" keys/sec)"
* prove it really is in order
      BAD = 0
      FOR I = 2 TO C
         IF KEYS<I> < KEYS<I-1> THEN BAD = BAD + 1
      NEXT I
      IF BAD = 0 AND C = N THEN PRINT "VALID" ELSE PRINT "INVALID ":BAD
      PRINT "sortkeys;":INT(C/ELAPSED):";":ELAPSED:";1;keys=":N
      END
