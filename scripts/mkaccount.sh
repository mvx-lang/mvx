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
# WHICH BACKEND THIS ACCOUNT USES, RECORDED RATHER THAN ASSUMED (#187).
#
#   driver  the default transport for a file nothing else placed
#   voc     VOC's own, declared separately because VOC is the bootstrap file:
#           it must be opened before anything that could describe it, and it
#           need not match the default -- an account keeps the VOC it was made
#           with while later files can go elsewhere.
#
# `driver`, not `hash`: `hash` already means the default CREATE-FILE type
# ("dir", a hash type), which is a different thing entirely.
#
# Both say what this account ACTUALLY used, so the answer survives the
# compiled-in default changing underneath it.
HASHDRV="$("$MVX" -a "$ACCT" -c 'LISTF' 2>/dev/null \
  | awk '$1 == "VOC" { print $2; exit }')"
[ -n "$HASHDRV" ] || HASHDRV=lmdb
printf '# MVX account descriptor\nname = %s\nversion = 1\ndriver = %s\nvoc = %s\n' \
  "$(basename "$ACCT")" "$HASHDRV" "$HASHDRV" > "$ACCT/.mvx"

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
