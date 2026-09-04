      AM = CHAR(254) ; VM = CHAR(253)
      GOSUB SHOW
      STOP
SHOW:
* sorted, exact hits and misses at both ends and the middle
      L = "b" : AM : "d" : AM : "f"
      LOCATE("d", L; P; "AL") THEN PRINT "1 hit ":P ELSE PRINT "1 ins ":P
      LOCATE("a", L; P; "AL") THEN PRINT "2 hit ":P ELSE PRINT "2 ins ":P
      LOCATE("e", L; P; "AL") THEN PRINT "3 hit ":P ELSE PRINT "3 ins ":P
      LOCATE("z", L; P; "AL") THEN PRINT "4 hit ":P ELSE PRINT "4 ins ":P
* duplicates: the first byte-match must win
      D = "a" : AM : "b" : AM : "b" : AM : "c"
      LOCATE("b", D; P; "AL") THEN PRINT "5 hit ":P ELSE PRINT "5 ins ":P
* ordering-equal but not byte-equal, right justified
      R = "1" : AM : "01" : AM : "2"
      LOCATE("01", R; P; "AR") THEN PRINT "6 hit ":P ELSE PRINT "6 ins ":P
      LOCATE("1", R; P; "AR")  THEN PRINT "7 hit ":P ELSE PRINT "7 ins ":P
* descending
      DD = "f" : AM : "d" : AM : "b"
      LOCATE("d", DD; P; "DL") THEN PRINT "8 hit ":P ELSE PRINT "8 ins ":P
      LOCATE("c", DD; P; "DL") THEN PRINT "9 hit ":P ELSE PRINT "9 ins ":P
* UNSORTED input with a sort order given -- undefined-ish, but must not differ
      U = "m" : AM : "a" : AM : "z" : AM : "c"
      LOCATE("z", U; P; "AL") THEN PRINT "10 hit ":P ELSE PRINT "10 ins ":P
      LOCATE("a", U; P; "AL") THEN PRINT "11 hit ":P ELSE PRINT "11 ins ":P
      LOCATE("q", U; P; "AL") THEN PRINT "12 hit ":P ELSE PRINT "12 ins ":P
* no order given: plain linear
      LOCATE("z", U; P) THEN PRINT "13 hit ":P ELSE PRINT "13 ins ":P
* empty and single
      E = ""
      LOCATE("x", E; P; "AL") THEN PRINT "14 hit ":P ELSE PRINT "14 ins ":P
      S = "only"
      LOCATE("only", S; P; "AL") THEN PRINT "15 hit ":P ELSE PRINT "15 ins ":P
* values level
      V = "p" : VM : "q" : VM : "r"
      LOCATE("q", V, 1; P; "AL") THEN PRINT "16 hit ":P ELSE PRINT "16 ins ":P
      RETURN
      END
