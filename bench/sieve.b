* Prime sieve of Eratosthenes, per PlummersSoftwareLLC/Primes rules:
* sieve of 1,000,000; repeat full sieves for 5 seconds; validate the
* count; report passes completed.
*
* BITS holds odd numbers only: index K represents 2*K+1.
SIEVESIZE = 1000000
HALF = 500000
Q = INT(SQRT(SIEVESIZE))
DIM BITS(500000)
PASSES = 0
START = SYSTEM(12)
LIMITMS = START + 5000
LOOP
WHILE SYSTEM(12) < LIMITMS DO
   * reset flags: 1 = prime candidate
   FOR I = 1 TO HALF
      BITS(I) = 1
   NEXT I
   FACTOR = 3
   LOOP
   WHILE FACTOR <= Q DO
      * find next unmarked factor
      K = INT((FACTOR - 1) / 2)
      LOOP
      WHILE BITS(K) = 0 DO
         FACTOR = FACTOR + 2
         K = K + 1
      REPEAT
      * mark multiples starting at factor^2
      MARK = INT((FACTOR * FACTOR - 1) / 2)
      LOOP
      WHILE MARK <= HALF DO
         BITS(MARK) = 0
         MARK = MARK + FACTOR
      REPEAT
      FACTOR = FACTOR + 2
   REPEAT
   PASSES = PASSES + 1
REPEAT
ELAPSED = (SYSTEM(12) - START) / 1000
* count: 2 plus odd primes 3..N (index 1..HALF-1; index HALF is 1000001)
NPRIME = 1
FOR I = 1 TO HALF - 1
   IF BITS(I) = 1 THEN NPRIME = NPRIME + 1
NEXT I
PRINT "Passes: ":PASSES:", Time: ":ELAPSED:", Avg: ":ELAPSED/PASSES:", Count: ":NPRIME
IF NPRIME = 78498 THEN PRINT "VALID" ELSE PRINT "INVALID"
PRINT "mvx;":PASSES:";":ELAPSED:";1;algorithm=base,faithful=yes"
END
