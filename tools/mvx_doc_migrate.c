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

/* mvx-doc-migrate — move stored records from the pre-#157 blob to the
 * document form.
 *
 *     mvx-doc-migrate <driver> <location>
 *
 * The location is written exactly as BINDINGS writes it, which for the
 * connection-profile backends means @name — run it from the account so the
 * profile resolves:
 *
 *     mvx-doc-migrate sqlite   /srv/acct/acct.sqlite
 *     mvx-doc-migrate mysql    "host=db port=3306 user=mvx password=… dbname=acct"
 *     (cd /srv/acct && mvx-doc-migrate postgres @pgmain)
 *     (cd /srv/acct && mvx-doc-migrate mongo    @mongomain)
 *
 * A LOCATION, not a file.  A file in the old format cannot be opened — the
 * driver refuses it and LISTF does not list it — so the set of files to
 * convert only exists in the backend's own catalogue, and the driver is the
 * only thing that can enumerate it.  One run per database converts every file
 * in it.
 *
 * On the SQL backends the record moves into a NEW COLUMN (`doc`, of the
 * backend's JSON type) and the old `rec` column is dropped, so this is a
 * schema change as much as a data one — which is why it cannot be a query and
 * has to be the driver's own code.  Each file is converted in a transaction:
 * an interrupted run leaves a file wholly in the old format, never half in
 * each.  Re-running is safe; a file already converted is skipped.
 *
 * It drives the DRIVER rather than going through the runtime's store, because
 * everything above the driver already assumes the new format -- but it uses
 * the runtime's own driver SEARCH (mvx_driver_find), because a tool with its
 * own idea of where drivers live is a tool that works on the developer's
 * machine and not on anybody else's. */

#include "mvx_driver.h"
#include "mvx_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>



int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: mvx-doc-migrate <driver> <location>\n"
                "\n"
                "Brings every file in <location> up to the current storage\n"
                "format, in place.  Safe to re-run.  Two conversions:\n"
                "\n"
                "  records  the pre-#157 blob becomes a JSON document\n"
                "  ids      a binary record id becomes text, escaped only\n"
                "           where the database's character set cannot carry\n"
                "           the byte -- and re-spelled when that character\n"
                "           set has changed under the data (#236)\n"
                "\n"
                "The location is written as BINDINGS writes it; for a\n"
                "connection profile that is @name, so run it from the account.\n"
                "\n"
                "  mvx-doc-migrate sqlite /srv/acct/acct.sqlite\n"
                "  mvx-doc-migrate mysql 'host=h port=3306 user=u password=p "
                "dbname=d'\n"
                "  (cd /srv/acct && mvx-doc-migrate postgres @pgmain)\n"
                "  (cd /srv/acct && mvx-doc-migrate mongo @mongomain)\n");
        return 2;
    }
    const char *name = argv[1], *loc = argv[2];

    const mvx_driver *d = mvx_driver_find(name);
    if (!d) {
        fprintf(stderr, "mvx-doc-migrate: no driver '%s' (set MVXDRIVERS)\n",
                name);
        return 1;
    }
    if (!d->migrate_docs) {
        fprintf(stderr,
                "mvx-doc-migrate: the %s driver stores records as they are and "
                "has nothing to convert\n", name);
        return 0;
    }

    char err[1024] = "";
    int n = d->migrate_docs(loc, err, sizeof err);
    if (n < 0) {
        fprintf(stderr, "mvx-doc-migrate: %s\n", err[0] ? err : "failed");
        return 1;
    }
    printf("mvx-doc-migrate: %d file(s) converted in %s\n", n, loc);
    return 0;
}
