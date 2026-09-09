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
 * It dlopen's the driver itself rather than going through the runtime's store,
 * because everything above the driver already assumes the new format. */

#include "mvx_driver.h"

#ifndef MVX_DRIVER_DIR
#define MVX_DRIVER_DIR "."
#endif

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#define DRV_SUFFIX ".dylib"
#else
#define DRV_SUFFIX ".so"
#endif

static const mvx_driver *load_driver(const char *name, const char *dir) {
    char path[4096];
    snprintf(path, sizeof path, "%s/libmvxdrv_%s" DRV_SUFFIX, dir, name);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) return NULL;
    mvx_driver_entry_fn entry =
        (mvx_driver_entry_fn)dlsym(h, "mvx_driver_entry");
    return entry ? entry(MVX_DRIVER_ABI) : NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: mvx-doc-migrate <driver> <location>\n"
                "\n"
                "Converts every file in <location> from the pre-#157 record\n"
                "blob to the document form, in place.  Safe to re-run.\n"
                "\n"
                "The location is written as BINDINGS writes it; for a\n"
                "connection profile that is @name, so run it from the account.\n"
                "\n"
                "  mvx-doc-migrate sqlite /srv/acct/acct.sqlite\n"
                "  mvx-doc-migrate mysql 'host=h port=3306 user=u password=p "
                "dbname=d'\n"
                "  (cd /srv/acct && mvx-doc-migrate postgres @pgmain)\n");
        return 2;
    }
    const char *name = argv[1], *loc = argv[2];

    const mvx_driver *d = NULL;
    const char *dirs = getenv("MVXDRIVERS");
    if (dirs && *dirs) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s", dirs);
        for (char *tok = strtok(buf, ":"); tok && !d; tok = strtok(NULL, ":"))
            d = load_driver(name, tok);
    }
    if (!d) d = load_driver(name, MVX_DRIVER_DIR);
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
