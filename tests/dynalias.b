* Dynamic-array aliasing and in-place mutation.  The runtime patches strings
* where they stand when a value owns its buffer outright, so every path that
* could observe a shared or self-referencing string belongs here.
*
* copy on write: mutating A must not reach B
      A = "red":@AM:"green":@AM:"blue"
      B = A
      A<2> = "GREEN"
      PRINT "cow A2=":A<2>:" B2=":B<2>
*
* the replacement value taken from the string being replaced
      S = "a":@VM:"b":@VM:"c"
      S<1,1> = S<1,3>
      PRINT "self=":S<1,1>:S<1,2>:S<1,3>
*
* an element that grows, then one that shrinks to nothing
      G = "1":@VM:"2":@VM:"3"
      G<1,2> = "22222"
      PRINT "grow=":G<1,1>:"/":G<1,2>:"/":G<1,3>
      G<1,2> = ""
      PRINT "shrink=":G<1,1>:"/":G<1,2>:"/":G<1,3>:" dc=":DCOUNT(G,@VM)
*
* appending in a loop, and appending to a shared string
      L = ""
      FOR I = 1 TO 5 ; L = L : I ; NEXT I
      PRINT "cat=":L
      P = "ab"
      Q = P
      P = P : "c"
      PRINT "catcow P=":P:" Q=":Q
*
* extracting a string into itself
      E = "x":@VM:"y":@VM:"z"
      E = E<1,2>
      PRINT "selfx=":E
*
* long enough to be worth indexing, then edited so the index must go
      LONG = "v1"
      FOR I = 2 TO 100 ; LONG = LONG : @VM : "v" : I ; NEXT I
      PRINT "long=":LONG<1,50>:"/":LONG<1,100>:" dc=":DCOUNT(LONG,@VM)
      LONG<1,50> = "Z"
      PRINT "repl=":LONG<1,49>:"/":LONG<1,50>:"/":LONG<1,51>
      LONG<1,2> = "wwwwwwwwww"
      PRINT "shift=":LONG<1,2>:"/":LONG<1,3>:"/":LONG<1,100>:" dc=":DCOUNT(LONG,@VM)
      END
