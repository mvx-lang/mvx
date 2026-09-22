* mvx#247: the language transaction surface.
*
* No file is opened here, so nothing enrols a connection: what this pins is
* the statement forms, the clause rules that UniData and UniVerse enforce,
* @TRANSACTION, and the refusals.  The atomicity itself is proven against a
* real backend in the sqlite block of scripts/test.sh -- it cannot be shown
* without a database.
PRINT "outside=":@TRANSACTION
TRANSACTION START THEN PRINT "started" ELSE PRINT "start refused"
PRINT "inside=":@TRANSACTION
* A second START is refused rather than nested: nesting would need savepoints
* and a partial rollback, and no MV offers one.
TRANSACTION START THEN PRINT "nested" ELSE PRINT "nesting refused"
TRANSACTION COMMIT THEN PRINT "committed" ELSE PRINT "commit refused"
PRINT "after=":@TRANSACTION
* COMMIT with none open is a failure the program can branch on.
TRANSACTION COMMIT THEN PRINT "committed again" ELSE PRINT "commit refused"
* ABORT takes no clause and yields no result -- an abort has no failure worth
* branching on, which is why UniData and UniVerse both reject THEN/ELSE here.
TRANSACTION START THEN PRINT "started again" ELSE PRINT "start refused"
TRANSACTION ABORT
PRINT "aborted=":@TRANSACTION
TRANSACTION ABORT
* @TRANSACTION is a depth, but MV code treats it as a boolean and that has to
* keep working: UniVerse holds 2 inside one and every site still writes IF.
TRANSACTION START THEN PRINT "started once more" ELSE PRINT "start refused"
IF @TRANSACTION THEN PRINT "the boolean sees it" ELSE PRINT "the boolean missed it"
TRANSACTION ABORT
* TRANSACTION is NOT reserved.  It is exactly the name a program would give a
* variable, so it opens a statement only in a shape that cannot be anything
* else -- START, COMMIT or ABORT following it -- which is the rule SEND and
* MESSAGE already follow.  Making it a keyword would break working code for
* the sake of one statement.
TRANSACTION = "still a variable"
PRINT TRANSACTION
PRINT "and @TRANSACTION is a different thing=":@TRANSACTION
END
