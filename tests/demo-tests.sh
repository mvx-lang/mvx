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
#
# demo-tests.sh — check the demo account (mvx-lang/demo) on every MV system.
#
# The demo account's claim is that an account crosses between MultiValue
# platforms through git with its dictionaries intact: conversions, formats,
# associations and I-types.  This suite is what checks the claim.  It CLONES
# the account with the platform's own driver and then reads it back through
# that platform's verbs, so the crossing is part of what is under test.
#
# It lives here rather than in the demo repository on purpose.  Plain files in
# an open account are checked out verbatim, so a test harness committed there
# would land in every account anyone clones — the demo account has to stay a
# demo account (mvx#122).
#
#   PLATFORM = mvx | udt | uv                (default mvx)
#   MVX      = the runtime / platform binary (mvx | udt | uv)
#   MVXGIT   = the git driver                (mvx-git | udt-git | uv-git)
#   DEMO     = repository to clone           (default the public demo)
#   WORK     = scratch directory             (default a mktemp -d)
#
#   PLATFORM=mvx MVX=build/bin/mvx MVXGIT=packages/git/bin/mvx-git \
#       sh tests/demo-tests.sh
set -u
PLATFORM="${PLATFORM:-mvx}"
: "${MVX:?set MVX to the platform binary (mvx, udt or uv)}"
: "${MVXGIT:?set MVXGIT to the git driver (mvx-git, udt-git or uv-git)}"
DEMO="${DEMO:-https://github.com/mvx-lang/demo.git}"
WORK="${WORK:-$(mktemp -d)}"
PASS=0; FAIL=0; SKIP=0

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s\n     expected: %s\n     actual:   %s\n' "$1" "$2" "$3"; }
skip() { SKIP=$((SKIP+1)); printf '  skip %s (%s)\n' "$1" "$2"; }
# t NAME EXPECTED ACTUAL — substring assertion
t()    { case "$3" in *"$2"*) ok "$1";; *) bad "$1" "$2" "$3";; esac; }
# nt NAME UNEXPECTED ACTUAL — the substring must NOT appear
nt()   { case "$3" in *"$2"*) bad "$1" "not: $2" "$3";; *) ok "$1";; esac; }

# --- runtime shim: one place that knows mvx from udt from uv -----------------
# V <sentence...> — run a TCL sentence in the demo account, echo its output
A="$WORK/demo"
if [ "$PLATFORM" = mvx ]; then
  V() { "$MVX" -a "$A" -c "$*" 2>&1; }
else
  # udt and uv take their sentences on stdin, one per line, in the account
  # directory.  QUIT leaves properly; EOF alone exits non-zero on some builds.
  V() { ( cd "$A" && printf '%s\nQUIT\n' "$*" | "$MVX" ) 2>&1; }
fi

say "== demo account suite — platform=$PLATFORM"
say "-- clone: the account arrives from git --"
rm -rf "$A"
clone_out=$( cd "$WORK" && "$MVXGIT" clone "$DEMO" demo 2>&1 )
if [ ! -d "$A" ]; then
  say "  FAIL clone — no account at $A"
  say "$clone_out" | sed 's/^/     /'
  exit 1
fi
ok "clone"
# The open-account opt-in must survive the clone: without it a commit from here
# would write the native form and the account would stop being portable.
t "clone kept the open form" "true" "$(git -C "$A" config --get mvx.openaccount 2>&1)"

say "-- the records arrived --"
t "clients"    "12 record"  "$(V COUNT CLIENTS)"
t "inventory"  "15 record"  "$(V COUNT INVENTORY)"
t "orders"     "20 record"  "$(V COUNT ORDERS)"
t "staff"      "8 record"   "$(V COUNT STAFF)"
t "states"     "8 record"   "$(V COUNT STATES)"

say "-- conversions: the stored form is not the shown form --"
# Money is the classic scaled integer — 24900 in the record, $249.00 on screen.
t "MD2\$ money"  '$249.00'   "$(V LIST INVENTORY PRICE WITH @ID = 'P100')"
t "D4/ date"     "2019"      "$(V LIST CLIENTS SINCE WITH @ID = 'C1001')"
t "MTH time"     "09:24"     "$(V LIST ORDERS ORD_TIME WITH @ID = '1001')"

say "-- associations: line items line up under their order --"
li="$(V LIST ORDERS PRODUCT_NO QTY PRICE WITH @ID = '1001')"
t "first line item"   "P100"     "$li"
t "second line item"  "P107"     "$li"
t "its own price"     '$89.90'   "$li"

# Every TRANS below passes on MVX and FAILS on UniData today, and that is not
# a harness problem: the open format carries the I-type expression byte for
# byte, and MVX's spelling does not mean the same thing to UniData's
# I-descriptor compiler — the key is an attribute number here and a field name
# there.  mv_git#90 is the converter work; these stay as failures until it
# lands, because that is what they are.
say "-- I-types --"
t "TRANS"              "Alice Nguyen"   "$(V LIST ORDERS CLIENT_NAME WITH @ID = '1001')"
mv="$(V LIST ORDERS PROD_NAME WITH @ID = '1001')"
t "TRANS, multivalued key (1)"  "Cordless Drill"   "$mv"
t "TRANS, multivalued key (2)"  "Extension Lead"   "$mv"
# order -> client -> state, resolved through the target's dictionary
t "TRANS, nested two hops"      "East"             "$(V LIST ORDERS REGION WITH @ID = '1001')"
t "TRANS, self-referential"     "Anita Sharma"     "$(V LIST STAFF MANAGER_NAME WITH @ID = 'S06')"
sel="$(V LIST ORDERS CLIENT_NAME WITH REGION = 'West')"
t "WITH on an I-type selects"   "Ella Brooks"      "$sel"
nt "WITH on an I-type excludes" "Alice Nguyen"     "$sel"

say "-- arithmetic I-types: where the platforms differ (mvx#121) --"
# QTY * PRICE and SUM(EPRICE) are ordinary on UniData and UniVerse; MVX's
# evaluator handles TRANS and DOCTAG only and returns empty.  Asserted per
# platform rather than skipped — the divergence IS the finding, and a suite
# that only checked what all three agree on would pass forever saying nothing.
ext="$(V LIST ORDERS EPRICE WITH @ID = '1001')"
if [ "$PLATFORM" = mvx ]; then
  nt "EPRICE is blank here (mvx#121)"  '$996.00'  "$ext"
else
  t  "EPRICE evaluates"                '$996.00'  "$ext"
fi

say "== $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
