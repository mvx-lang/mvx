      VM = CHAR(253) ; AM = CHAR(254) ; SM = CHAR(252)
* no attribute mark: the fast path's case
      A = "a" : VM : "bb" : VM : "ccc"
      A<1,2> = "ZZ"   ; PRINT "1:" : CHANGE(A, VM, "|")
      A<1,1> = ""     ; PRINT "2:" : CHANGE(A, VM, "|")
      A<1,3> = "long value here" ; PRINT "3:" : CHANGE(A, VM, "|")
      A<1,5> = "past the end"    ; PRINT "4:" : CHANGE(A, VM, "|")
* with attribute marks: the fast path must NOT fire
      B = "f1v1" : VM : "f1v2" : AM : "f2v1" : VM : "f2v2"
      B<1,2> = "X" ; PRINT "5:" : CHANGE(CHANGE(B, VM, "|"), AM, "^")
      B<2,1> = "Y" ; PRINT "6:" : CHANGE(CHANGE(B, VM, "|"), AM, "^")
      B<2,2> = ""  ; PRINT "7:" : CHANGE(CHANGE(B, VM, "|"), AM, "^")
* three levels
      C = "p" : SM : "q" : VM : "r"
      C<1,1,2> = "QQ" ; PRINT "8:" : CHANGE(CHANGE(C, VM, "|"), SM, "~")
* single element, and empty string
      D = "solo" ; D<1,1> = "new" ; PRINT "9:" : D
      E = ""     ; E<1,2> = "z"   ; PRINT "10:" : CHANGE(E, VM, "|")
      PRINT "11:" : LEN(A) : "," : DCOUNT(A, VM) : "," : COUNT(B, VM)
      END
