#!/bin/sh
# MVX — a native compiler and runtime for Pick/MultiValue BASIC.
# Copyright (C) 2026 Gordon Heydon.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2, as
# published by the Free Software Foundation.  There is NO WARRANTY, to
# the extent permitted by law; see the LICENSE file for details.
#
# SPDX-License-Identifier: GPL-2.0-only
# Create an MVX account.  Standard verbs come from the system account
# (built into build/system, overridable with $MVXSYSTEM); the account
# itself gets only an empty local VOC for its own cataloged programs.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACCT="${1:?usage: mkaccount.sh <account-directory>}"

mkdir -p "$ACCT"
# The mvx that creates the file: a built tree's build/bin/mvx by default, but
# overridable with $MVX (or an installed mvx on PATH) so an account can be made
# against a setup-mvx toolchain with no mvx source build -- the same resolution
# mkpkg.sh does for mvx-basic.  Without it this script only ever worked inside a
# built source tree: CI checks the repo out scripts-only, `build/bin/mvx` was
# not found, `set -e` aborted before .mvx was written, and every account the
# suite made had no VOC and no descriptor (mvx#137).
MVX="${MVX:-$ROOT/build/bin/mvx}"
[ -x "$MVX" ] || command -v "$MVX" >/dev/null 2>&1 || MVX="$(command -v mvx || true)"
[ -n "$MVX" ] || { echo "mkaccount: no mvx (set \$MVX or put it on PATH)" >&2; exit 1; }
"$MVX" -a "$ACCT" -c "CREATE-FILE VOC" >/dev/null
printf '# MVX account descriptor\nname = %s\nversion = 1\n' \
  "$(basename "$ACCT")" > "$ACCT/.mvx"

# Seed the account's default OS-command permissions from the system account's
# .mvx (its `permit`/`deny` lines), so a new account starts with the site
# baseline; per-account and system-layer policy then layer on top (see
# ARCHITECTURE.md 8.4).
SYS="${MVXSYSTEM:-$ROOT/build/system}"
# ...and the same for the system account itself: an installed toolchain keeps it
# under $MVXHOME/share/mvx/system, not beside this script (mvx#137).
[ -d "$SYS" ] || SYS="${MVXHOME:-}/share/mvx/system"
if [ -f "$SYS/.mvx" ]; then
  grep -E '^[[:space:]]*(permit|deny)[[:space:]]' "$SYS/.mvx" >> "$ACCT/.mvx" || true
fi

echo "account ready: $ACCT"
