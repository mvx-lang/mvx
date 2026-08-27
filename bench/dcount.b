* DCOUNT and the loop it lives in.
*
*   N = DCOUNT(LIST, VM) ; FOR I = 1 TO N ... LIST<1,I> ... NEXT I
*
* is the most common shape in MV code, and DCOUNT is a full scan of the value
* counting delimiters -- every time it is called.  This measures the walk as a
* whole: the count, and then reading every element.
      TPS = 1000
      VM = CHAR(253)
      N = 20000
      SECS = 5
      LIST = ""
      FOR I = 1 TO N
         K = "item" : FMT(I, "R%6")
         IF I = 1 THEN LIST = K ELSE LIST = LIST : VM : K
      NEXT I
*
      WALKS = 0 ; SEEN = 0 ; COUNTS = 0
      START = SYSTEM(12)
      LIMIT = START + SECS * TPS
      LOOP
      WHILE SYSTEM(12) < LIMIT DO
         * the count on its own, as a loop header would ask for it
         C = DCOUNT(LIST, VM)
         COUNTS = COUNTS + 1
         * and the walk it introduces
         FOR I = 1 TO C
            IF LIST<1,I> # "" THEN SEEN = SEEN + 1
         NEXT I
         WALKS = WALKS + 1
      REPEAT
      ELAPSED = (SYSTEM(12) - START) / TPS
      PRINT "Walks: ":WALKS:", elements seen: ":SEEN:", time: ":ELAPSED
      PRINT "walks/sec: ":INT(WALKS/ELAPSED):", elements/sec: ":INT(SEEN/ELAPSED)
      IF SEEN = WALKS * N THEN PRINT "VALID" ELSE PRINT "INVALID"
      PRINT "dcount;":INT(SEEN/ELAPSED):";":ELAPSED:";1;elements=":N
      END
