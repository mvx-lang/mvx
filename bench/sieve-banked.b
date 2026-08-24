* Prime sieve of Eratosthenes, per PlummersSoftwareLLC/Primes rules:
* sieve of 1,000,000; full sieves repeated for five seconds; the count is
* validated; passes completed is the score.
*
* Banked variant of sieve.b.  The flags for the odd numbers below
* 1,000,000 need 500,000 elements, and UniVerse caps a compile-time DIM
* at 64,000 -- in practice at rather less -- so the flags are banked:
* CHUNK of them per DIM element, each element holding them as a dynamic
* array.  Every MV BASIC can express this, which is the point of having
* it.  It is the same algorithm on every platform, and what it costs to
* reach the flags this way is then visible in the score.
*
* Index K represents the odd number 2*K+1, and lives at BANK(B)<1,P>.
      TPS = 1000            ;* SYSTEM(12) ticks per second; UniVerse: 1
      SIEVESIZE = 1000000
      HALF = 500000
      CHUNK = 16
      NBANKS = 31250        ;* NBANKS * CHUNK = HALF, exactly
      Q = INT(SQRT(SIEVESIZE))
      VM = CHAR(253)
      DIM BANK(31250)
*
* one full bank of set flags, so a pass resets every bank in one statement
      ONES = "1"
      FOR I = 2 TO CHUNK
         ONES = ONES : VM : "1"
      NEXT I
*
      PASSES = 0
      START = SYSTEM(12)
      LIMIT = START + 5 * TPS
      LOOP
      WHILE SYSTEM(12) < LIMIT DO
         MAT BANK = ONES
         FACTOR = 3
         LOOP
         WHILE FACTOR <= Q DO
            * find the next unmarked factor
            K = INT((FACTOR - 1) / 2)
            LOOP
               B = INT((K - 1) / CHUNK) + 1
               P = K - (B - 1) * CHUNK
            WHILE BANK(B)<1,P> = 0 DO
               FACTOR = FACTOR + 2
               K = K + 1
            REPEAT
            * mark multiples, starting at factor squared
            MARK = INT((FACTOR * FACTOR - 1) / 2)
            LOOP
            WHILE MARK <= HALF DO
               B = INT((MARK - 1) / CHUNK) + 1
               P = MARK - (B - 1) * CHUNK
               BANK(B)<1,P> = 0
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
      NPRIME = 1
      FOR B = 1 TO NBANKS
         NPRIME = NPRIME + COUNT(BANK(B), "1")
      NEXT B
      B = INT((HALF - 1) / CHUNK) + 1
      P = HALF - (B - 1) * CHUNK
      IF BANK(B)<1,P> = 1 THEN NPRIME = NPRIME - 1
      PRINT "Passes: ":PASSES:", Time: ":ELAPSED:", Avg: ":ELAPSED/PASSES:", Count: ":NPRIME
      IF NPRIME = 78498 THEN PRINT "VALID" ELSE PRINT "INVALID"
      PRINT "banked;":PASSES:";":ELAPSED:";1;algorithm=base,faithful=yes"
      END
