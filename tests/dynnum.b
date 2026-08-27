      VM = CHAR(253)
      L = "7" : VM : "007" : VM : "-3" : VM : "0" : VM : "-0"
      PRINT "1:" : SUM(L) : "|" : MAXIMUM(L) : "|" : MINIMUM(L)
      M = "1.5" : VM : "2.25" : VM : "-0.75"
      PRINT "2:" : SUM(M) : "|" : MAXIMUM(M) : "|" : MINIMUM(M)
      X = "1e3" : VM : "+5" : VM : " 4" : VM : "4 " : VM : "-"
      PRINT "3:" : SUM(X) : "|" : MAXIMUM(X) : "|" : MINIMUM(X)
      Y = "abc" : VM : "12abc" : VM : "" : VM : "9"
      PRINT "4:" : SUM(Y) : "|" : MAXIMUM(Y) : "|" : MINIMUM(Y)
      Z = "999999999999999999" : VM : "1"
      PRINT "5:" : SUM(Z)
      W = "0000000000000000001" : VM : "2"
      PRINT "6:" : SUM(W)
      B2 = "1.0" : VM : "2.0"
      PRINT "7:" : SUM(B2)
      N2 = "-9223372036854775807" : VM : "0"
      PRINT "8:" : SUM(N2)
      PRINT "9:" : SUM("5")
      PRINT "10:" : SUM("")
      END
