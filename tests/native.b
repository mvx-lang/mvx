* Native filesystem primitives — MKDIR / RMTREE / UNTAR (issue #80).
* No shell, permit-gated by op name.  This account grants  permit * = mkdir
* untar  but NOT rmtree, so mkdir + untar run while rmtree is refused (-1).
   RC = MKDIR("nativetest/sub")
   PRINT "mkdir: ":RC
   RC = UNTAR("fixture.tgz", "nativetest")
   PRINT "untar: ":RC
   T = OSREAD("nativetest/marker.txt")
   PRINT "content: ":TRIM(T)
* UNTAR unwraps a single top-level directory so a package's files land at the
* destination root (wrapped.tgz holds pkg/inner.txt -> wtest/inner.txt).
   RC = UNTAR("wrapped.tgz", "wtest")
   PRINT "untar wrapped: ":RC
   PRINT "stripped: ":TRIM(OSREAD("wtest/inner.txt"))
* UNAME axes are read-only (ungated); their values vary by host, so assert shape.
   PRINT "endian ok: ":(UNAME('e') = "le" OR UNAME('e') = "be")
   PRINT "os set: ":(LEN(UNAME('s')) > 0)
   PRINT "arch set: ":(LEN(UNAME('m')) > 0)
   RC = RMTREE("nativetest")
   PRINT "rmtree (not permitted): ":RC
   END
