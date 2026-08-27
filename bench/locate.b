* LOCATE over a sorted dynamic array — the other half of what a dynamic array
* is for.
*
* sieve-dynamic.b measures subscripted WRITES on single-character elements.
* This measures the idiom MV code actually spends its time in: keeping a list
* in order and looking things up in it.
*
*   LOCATE(X, LIST; POS; "AL") THEN ... ELSE INSERT
*
* An ordered LOCATE is told the array is sorted.  A linear scan does not use
* that; a binary search does, and needs O(1) access to element k to do it --
* which is what an element index is.  This benchmark exists to say what that
* is worth.
      TPS = 1000
      AM = CHAR(254)
      N = 20000                  ;* elements in the list
      SECS = 5
*
* A sorted list of fixed-width keys, ATTRIBUTE separated -- LOCATE with no
* level searches attributes, which is the plain "a list of things" case.
      LIST = ""
      FOR I = 1 TO N
         K = "K" : FMT(I, "R%7")
         IF I = 1 THEN LIST = K ELSE LIST = LIST : AM : K
      NEXT I
*
      HITS = 0 ; MISSES = 0 ; OPS = 0
      START = SYSTEM(12)
      LIMIT = START + SECS * TPS
      SEED = 1
      LOOP
      WHILE SYSTEM(12) < LIMIT DO
         * a spread of lookups across the list: present keys and absent ones
         FOR J = 1 TO 200
            SEED = MOD(SEED * 1103515245 + 12345, 2147483648)
            IX = MOD(SEED, N) + 1
            WANT = "K" : FMT(IX, "R%7")
            LOCATE(WANT, LIST; POS; "AL") THEN HITS = HITS + 1 ELSE MISSES = MISSES + 1
            OPS = OPS + 1
            * one that is not there: between two keys
            WANT = "K" : FMT(IX, "R%7") : "x"
            LOCATE(WANT, LIST; POS; "AL") THEN HITS = HITS + 1 ELSE MISSES = MISSES + 1
            OPS = OPS + 1
         NEXT J
      REPEAT
      ELAPSED = (SYSTEM(12) - START) / TPS
      PRINT "Lookups: ":OPS:", Time: ":ELAPSED:", per sec: ":INT(OPS/ELAPSED)
      PRINT "hits: ":HITS:", misses: ":MISSES
      IF HITS = MISSES THEN PRINT "VALID" ELSE PRINT "INVALID"
      PRINT "locate;":INT(OPS/ELAPSED):";":ELAPSED:";1;elements=":N
      END
