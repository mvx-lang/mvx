* Prime sieve of Eratosthenes, per PlummersSoftwareLLC/Primes rules:
* sieve of 1,000,000; full sieves repeated for five seconds; the count is
* validated; passes completed is the score.
*
* NO DIMENSIONED ARRAY AT ALL.  sieve.b uses DIM, and sieve-banked.b uses DIM
* with a dynamic array inside each element because UniVerse caps a compile-time
* DIM at 64,000.  This one uses neither: the flags for the odd numbers below
* 1,000,000 are 500,000 VALUES OF A SINGLE DYNAMIC ARRAY.
*
* That is the shape a Pick programmer actually writes when the working set is
* one thing -- a record, a list, a set of flags -- and it is the hardest case
* for the runtime: half a million elements in one string, subscripted at random
* half a million times a pass.  It is here to be the benchmark that says
* whether the element index scales, or only looks good on sixteen elements.
*
* Index K represents the odd number 2*K+1, and lives at FLAGS<1,K>.
      TPS = 1000            ;* SYSTEM(12) ticks per second; UniVerse: 1
      SIEVESIZE = 1000000
      HALF = 500000
      Q = INT(SQRT(SIEVESIZE))
      VM = CHAR(253)
*
* One string of HALF set flags, built once, so a pass resets in one assignment
* rather than half a million.  Doubling is used rather than a HALF-iteration
* loop because building it one value at a time is the thing being measured
* everywhere else, and it is not what this benchmark is about.
      ONES = "1"
      N = 1
      LOOP
      WHILE N * 2 <= HALF DO
         ONES = ONES : VM : ONES
         N = N * 2
      REPEAT
      FOR I = N + 1 TO HALF
         ONES = ONES : VM : "1"
      NEXT I
*
      PASSES = 0
      START = SYSTEM(12)
      LIMIT = START + 5 * TPS
      LOOP
      WHILE SYSTEM(12) < LIMIT DO
         FLAGS = ONES
         FACTOR = 3
         LOOP
         WHILE FACTOR <= Q DO
            * find the next unmarked factor
            K = INT((FACTOR - 1) / 2)
            LOOP
            WHILE FLAGS<1,K> = 0 DO
               FACTOR = FACTOR + 2
               K = K + 1
            REPEAT
            * mark multiples, starting at factor squared
            MARK = INT((FACTOR * FACTOR - 1) / 2)
            LOOP
            WHILE MARK <= HALF DO
               FLAGS<1,MARK> = 0
               MARK = MARK + FACTOR
            REPEAT
            FACTOR = FACTOR + 2
         REPEAT
         PASSES = PASSES + 1
      REPEAT
      ELAPSED = (SYSTEM(12) - START) / TPS
*
* 2 is prime, then the odd primes at indices 1..HALF-1; index HALF is
* 1,000,001 and is not one of them
      NPRIME = 1 + COUNT(FLAGS, "1")
      IF FLAGS<1,HALF> = 1 THEN NPRIME = NPRIME - 1
      PRINT "Passes: ":PASSES:", Time: ":ELAPSED:", Avg: ":ELAPSED/PASSES:", Count: ":NPRIME
      IF NPRIME = 78498 THEN PRINT "VALID" ELSE PRINT "INVALID"
      PRINT "dynamic;":PASSES:";":ELAPSED:";1;algorithm=base,faithful=yes"
      END
