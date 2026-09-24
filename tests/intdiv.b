* INT(a / b) on integers (mvx#183).
*
* `/` is real division in MV, so the pair used to compile to a float round
* trip: integer to double, divide, saturating convert back.  Wrapped in INT()
* that is both slower and, above 2^53, WRONG -- a double cannot hold the
* operands, so the answer comes back off by one in either direction.
*
* INT() truncates toward zero and so does an integer divide, so the two agree
* on every pair an integer divide is defined for.  The cases it is not defined
* for are here too, because they must still answer what the float path did.
      PRINT "-- exact above 2^53 --"
      N = 9007199254740993
      PRINT INT(N / 1)
      M = 9007199254740995
      PRINT INT(M / 1)
      PRINT INT(M / 3)
*
      PRINT "-- truncation toward zero, both signs --"
      PRINT INT(7 / 2)
      PRINT INT(-7 / 2)
      PRINT INT(7 / -2)
      PRINT INT(-7 / -2)
*
      PRINT "-- a variable divisor, the shape MV code writes --"
      CHUNK = 16
      K = 100
      PRINT INT((K - 1) / CHUNK)
*
      PRINT "-- the undefined-for-sdiv cases still answer --"
      Z = 0
      PRINT INT(5 / Z)
      PRINT INT(-5 / Z)
      PRINT INT(0 / Z)
      BIG = -9223372036854775807 - 1
      NEG = -1
      PRINT INT(BIG / NEG)
*
      PRINT "-- still fractional without INT() --"
      PRINT 7 / 2
      END
