      AM = CHAR(254) ; VM = CHAR(253) ; SM = CHAR(252)
      A = "a" : VM : "b" : VM : "c"
      PRINT "1:" : COUNT(A, VM) : "," : DCOUNT(A, VM)
      PRINT "2:" : COUNT(A, AM) : "," : DCOUNT(A, AM)
      E = ""
      PRINT "3:" : COUNT(E, VM) : "," : DCOUNT(E, VM)
      S = "solo"
      PRINT "4:" : COUNT(S, VM) : "," : DCOUNT(S, VM)
* empty elements at both ends and in the middle
      G = VM : "x" : VM : VM : "y" : VM
      PRINT "5:" : COUNT(G, VM) : "," : DCOUNT(G, VM)
* mixed levels
      M = "p" : VM : "q" : AM : "r" : SM : "s"
      PRINT "6:" : COUNT(M, VM) : "," : COUNT(M, AM) : "," : COUNT(M, SM)
      PRINT "7:" : DCOUNT(M, VM) : "," : DCOUNT(M, AM) : "," : DCOUNT(M, SM)
* a non-mark delimiter, and a multi-character one
      T = "aXbXXc"
      PRINT "8:" : COUNT(T, "X") : "," : DCOUNT(T, "X") : "," : COUNT(T, "XX")
* count of a character that is also a mark, in a value with none
      PRINT "9:" : COUNT("plain", VM) : "," : DCOUNT("plain", VM)
* after an edit, the count must follow
      A<1,2> = "bbbb"
      PRINT "10:" : DCOUNT(A, VM) : "," : LEN(A)
      A<1,5> = "e"
      PRINT "11:" : DCOUNT(A, VM)
      END
