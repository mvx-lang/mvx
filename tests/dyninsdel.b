      AM = CHAR(254) ; VM = CHAR(253) ; SM = CHAR(252)
      SHOW = ""
      GOSUB RUNALL
      STOP
RUNALL:
* ---- INSERT at every interesting position
      A = "a" : AM : "b" : AM : "c"
      PRINT "01:" : CHANGE(CHANGE(CHANGE(INSERT(A, 1, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "02:" : CHANGE(CHANGE(CHANGE(INSERT(A, 2, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "03:" : CHANGE(CHANGE(CHANGE(INSERT(A, 3, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "04:" : CHANGE(CHANGE(CHANGE(INSERT(A, 4, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "05:" : CHANGE(CHANGE(CHANGE(INSERT(A, 9, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "06:" : CHANGE(CHANGE(CHANGE(INSERT(A, -1, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "07:" : CHANGE(CHANGE(CHANGE(INSERT(A, 0, 0, 0, "X"),AM,"^"),VM,"|"),SM,"~")
      PRINT "08:" : CHANGE(CHANGE(CHANGE(INSERT(A, 2, 0, 0, ""),AM,"^"),VM,"|"),SM,"~")
* ---- DELETE at every interesting position
      PRINT "09:" : CHANGE(CHANGE(CHANGE(DELETE(A, 1, 0, 0),AM,"^"),VM,"|"),SM,"~")
      PRINT "10:" : CHANGE(CHANGE(CHANGE(DELETE(A, 2, 0, 0),AM,"^"),VM,"|"),SM,"~")
      PRINT "11:" : CHANGE(CHANGE(CHANGE(DELETE(A, 3, 0, 0),AM,"^"),VM,"|"),SM,"~")
      PRINT "12:" : CHANGE(CHANGE(CHANGE(DELETE(A, 4, 0, 0),AM,"^"),VM,"|"),SM,"~")
      PRINT "13:" : CHANGE(CHANGE(CHANGE(DELETE(A, 0, 0, 0),AM,"^"),VM,"|"),SM,"~")
* ---- single element and empty
      S = "only"
      PRINT "14:" : CHANGE(CHANGE(CHANGE(DELETE(S, 1, 0, 0),AM,"^"),VM,"|"),SM,"~") : "|len=" : LEN(DELETE(S, 1, 0, 0))
      PRINT "15:" : CHANGE(CHANGE(CHANGE(INSERT(S, 1, 0, 0, "new"),AM,"^"),VM,"|"),SM,"~")
      E = ""
      PRINT "16:" : CHANGE(CHANGE(CHANGE(INSERT(E, 1, 0, 0, "z"),AM,"^"),VM,"|"),SM,"~") : "|" : CHANGE(CHANGE(CHANGE(DELETE(E, 1, 0, 0),AM,"^"),VM,"|"),SM,"~")
* ---- empty elements present
      G = AM : "x" : AM : AM : "y"
      PRINT "17:" : CHANGE(CHANGE(CHANGE(INSERT(G, 2, 0, 0, "Q"),AM,"^"),VM,"|"),SM,"~")
      PRINT "18:" : CHANGE(CHANGE(CHANGE(DELETE(G, 3, 0, 0),AM,"^"),VM,"|"),SM,"~")
      PRINT "19:" : CHANGE(CHANGE(CHANGE(DELETE(G, 1, 0, 0),AM,"^"),VM,"|"),SM,"~")
* ---- deeper levels must go the general way
      M = "p" : VM : "q" : AM : "r"
      PRINT "20:" : CHANGE(CHANGE(CHANGE(INSERT(M, 1, 2, 0, "Z"),AM,"^"),VM,"|"),SM,"~")
      PRINT "21:" : CHANGE(CHANGE(CHANGE(DELETE(M, 1, 1, 0),AM,"^"),VM,"|"),SM,"~")
      T = "u" : SM : "v"
      PRINT "22:" : CHANGE(CHANGE(CHANGE(INSERT(T, 1, 1, 2, "W"),AM,"^"),VM,"|"),SM,"~")
* ---- longer list, repeated maintenance, and the count after
      L = ""
      FOR I = 1 TO 40
         IF I = 1 THEN L = "e":I ELSE L = L : AM : "e":I
      NEXT I
      FOR I = 1 TO 20
         L = INSERT(L, I * 2, 0, 0, "ins":I)
         L = DELETE(L, I, 0, 0)
      NEXT I
      PRINT "23:count=" : DCOUNT(L, AM) : "|len=" : LEN(L)
      PRINT "24:" : L<1> : "," : L<20> : "," : L<40>
* ---- inserting a value taken from the same string
      B2 = "m" : AM : "n"
      PRINT "25:" : CHANGE(CHANGE(CHANGE(INSERT(B2, 1, 0, 0, B2<2>),AM,"^"),VM,"|"),SM,"~")
      RETURN
      END
