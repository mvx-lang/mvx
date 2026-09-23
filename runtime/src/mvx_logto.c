/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Changing account from inside a session (mvx#258).
 *
 * LOGTO used to be a builtin of `mvx` and nothing else, so a site that
 * replaces TCL with its own login and menu -- which is the point of mvx#248
 * -- was bound to the account its shell started in.  An operator picking a
 * company or a division from a menu is exactly this, and CueBic did it.
 *
 * MEASURED ON THE REAL SYSTEMS, because the issue listed three questions
 * worth answering rather than guessing (UniData 8.3, UniVerse 14.2):
 *
 *   - a BASIC program SURVIVES its own LOGTO on both: a program that prints,
 *     does EXECUTE "LOGTO <acct>", then prints again prints both lines.  So
 *     this changes account and RETURNS; it does not end the program.
 *   - and the move STICKS.  On UniData an EXECUTE "WHERE" afterwards reported
 *     the new account, and TCL was still there when the program ended.
 *   - both also run the target's VOC LOGIN on the way in.  MVX has no LOGIN
 *     hook at all; that is mvx#264, and deliberately not decided here.
 *
 * The second of those is why `EXECUTE "LOGTO ..."` is a caller and not just
 * the intrinsic: it is the spelling that already works everywhere else, so
 * code ported from UniData uses it.  Here it used to find no VOC entry (LOGTO
 * being a shell builtin), fall back to spawning `mvx -c`, and move a CHILD
 * that immediately exited -- the caller stayed where it was, silently.
 *
 * Three callers, one implementation: the intrinsic, EXECUTE, and the shell.
 *
 * LOOK BEFORE LEAVING.  The account has to be let go before the move -- a
 * default-backend connection is named relative to the account, so releasing
 * it afterwards would name the wrong one -- and a session that left one
 * account only to find the next unreachable would be stranded between the
 * two.  So the destination is OPENED first and the move done with fchdir,
 * which cannot then fail for a reason a check would have caught. */

#include "mvx_runtime.h"
#include "mvx_ext.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* STATUS() after a LOGTO, so a BASIC shell can tell the two refusals apart:
   one is the operator's typo, the other is the program's own transaction. */
#define LOGTO_OK        0
#define LOGTO_NOACCT    1
#define LOGTO_TXN       2

int64_t mvx_logto(mvx_ctx *ctx, const char *acct) {
    if (!acct || !acct[0]) {
        mvx_ctx_set_status(ctx, LOGTO_NOACCT);
        fprintf(stderr, "LOGTO: no account named\n");
        return 0;
    }

    /* Hold the destination open: fchdir below then cannot fail for anything a
       stat would have caught, so there is no window in which the session has
       left one account and cannot enter the next. */
    int fd = open(acct, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        mvx_ctx_set_status(ctx, LOGTO_NOACCT);
        fprintf(stderr, "LOGTO: cannot enter account %s\n", acct);
        return 0;
    }

    /* Close its files, drop its locks and release the connections they were
       on (mvx#251).  Refused while a transaction is open: committing it after
       the account changed would commit into somewhere the program no longer
       is, and discarding it silently is worse.  mvx_store_leave says why. */
    if (!mvx_store_leave(ctx)) {
        close(fd);
        mvx_ctx_set_status(ctx, LOGTO_TXN);
        return 0;
    }

    if (fchdir(fd) != 0) {              /* the fd was a directory a moment ago */
        close(fd);
        mvx_ctx_set_status(ctx, LOGTO_NOACCT);
        fprintf(stderr, "LOGTO: cannot enter account %s\n", acct);
        return 0;
    }
    close(fd);

    /* What a spawned child would inherit.  A child resolves its account from
       the working directory, and MVXACCTPATH is what names it. */
    char here[4096];
    if (getcwd(here, sizeof here)) setenv("MVXACCTPATH", here, 1);
    setenv("MVXACCOUNT", ".", 1);

    /* The RESOLUTION CHAINS, both of which are per account: VOC, linked
       packages and the system account behind them (mvx#248), and the same
       three-tier walk for subroutine libraries.  A LOGTO that left either
       would keep answering for the account just left. */
    mvx_voc_reset();
    mvx_ext_reset_libs();

    /* Select lists do not cross a LOGTO.  mvx_store_leave has already dropped
       the in-process one; this is the file that hands a list to the next
       command, which belongs to the account that made it. */
    const char *sess = getenv("MVXSESSION");
    if (sess && sess[0]) {
        FILE *fp = fopen(sess, "wb");
        if (fp) fclose(fp);
    }

    mvx_ctx_set_status(ctx, LOGTO_OK);
    return 1;
}

/* LOGTO(acct) -- 1 when the session moved, 0 when it did not, with the reason
   in STATUS().  A value-returning intrinsic already takes THEN/ELSE, so the
   shape the issue asked for needs no statement:

       IF LOGTO("SALES") THEN GOSUB OPEN.THE.FILES ELSE ...

   Re-establishing what the shell had open is the shell's own business, which
   is the honest answer once the handles are gone: mvx_store_leave has closed
   them and mvx#251 makes a stale one say so rather than read freed memory. */
static void ext_logto(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                      mv_value **argv) {
    (void)argc;
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(argv[0], nb, sizeof nb, &p);
    char acct[4096];
    snprintf(acct, sizeof acct, "%.*s", (int)n, p);
    mv_set_int(ret, mvx_logto(ctx, acct));
}

static const mvx_extfn logto_fns[] = {
    {"LOGTO", 1, 1, ext_logto},
};
static const mvx_ext logto_ext = {"logto", 1, logto_fns};

const mvx_ext *mvx_logto_builtin(void) { return &logto_ext; }
