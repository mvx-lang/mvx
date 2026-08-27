* SUM / MAXIMUM / MINIMUM over a dynamic array.
*
* The reductions walk the value, parse every element as a number and format the
* result.  A totals line over a set of values is ordinary MV code -- SUM(AMT)
* across a multi-valued field -- so this measures the walk, the parse and the
* format together.
      TPS = 1000
      VM = CHAR(253)
      N = 5000
      SECS = 5
      NUMS = ""
      FOR I = 1 TO N
         IF I = 1 THEN NUMS = I ELSE NUMS = NUMS : VM : I
      NEXT I
      EXPECT = N * (N + 1) / 2
*
      OPS = 0 ; BAD = 0
      START = SYSTEM(12) ; LIMIT = START + SECS * TPS
      LOOP
      WHILE SYSTEM(12) < LIMIT DO
         FOR J = 1 TO 20
            T = SUM(NUMS)
            IF T # EXPECT THEN BAD = BAD + 1
            OPS = OPS + 1
         NEXT J
      REPEAT
      E = (SYSTEM(12) - START) / TPS
      PRINT "sums: ":OPS:", time: ":E:", sums/sec: ":INT(OPS/E)
      PRINT "elements/sec: ":INT(OPS*N/E):", wrong: ":BAD
      IF BAD = 0 THEN PRINT "VALID" ELSE PRINT "INVALID"
      PRINT "reduce;":INT(OPS*N/E):";":E:";1;elements=":N
      END
