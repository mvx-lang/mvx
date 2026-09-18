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
# Build the configured packages (MVX_PACKAGES) and install them into the system
# account: their verbs (VOC + CATALOG), extension libraries (LIB), and the
# aggregated EXPORTS the compiler reads.  So a configured package is always
# available — no LINK-PKG, no per-compile flag.
#   install-pkgs.sh <system-dir> <root> [pkg ...]
set -e
SYS="${1:?usage: install-pkgs.sh <system-dir> <root> [pkg...]}"; shift
ROOT="${1:?}"; shift

mkdir -p "$SYS/CATALOG" "$SYS/VOC" "$SYS/LIB" "$SYS/EXPORTS.d"
: > "$SYS/EXPORTS"                       # regenerated from scratch each build
rm -f "$SYS/EXPORTS.d"/*                 # and so is its provenance

for pkg in "$@"; do
  pdir="$ROOT/packages/$pkg"
  [ -d "$pdir" ] || { echo "install-pkgs: no package $pkg" >&2; exit 1; }
  "$ROOT/scripts/mkpkg.sh" "$pdir" >/dev/null
  [ -d "$pdir/CATALOG" ] && cp -R "$pdir/CATALOG/." "$SYS/CATALOG/" 2>/dev/null || true
  [ -d "$pdir/VOC" ]     && cp -R "$pdir/VOC/."     "$SYS/VOC/"     2>/dev/null || true
  [ -d "$pdir/LIB" ]     && cp -R "$pdir/LIB/."     "$SYS/LIB/"     2>/dev/null || true
  # THE AGGREGATE, AND WHO PUT WHAT IN IT.  EXPORTS is what the compiler reads;
  # EXPORTS.d/<pkg> records which package each name came from, which is what
  # lets the install decide that an added curl supersedes the built-in http
  # rather than both answering to HTTPGET (mvx#218).
  if [ -f "$pdir/EXPORTS" ]; then
    cat "$pdir/EXPORTS" >> "$SYS/EXPORTS"
    cp "$pdir/EXPORTS" "$SYS/EXPORTS.d/$pkg"
  fi
  echo "  installed package $pkg into the system account"
done
