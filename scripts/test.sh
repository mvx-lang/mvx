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
# MVX test harness.
#
#   scripts/test.sh            run everything
#   scripts/test.sh -q        quick: skip the 5-second sieve benchmark
#   scripts/test.sh --bless   (re)capture expected outputs — review the
#                             diff before committing!
#
# Phase 1 runs tests/*.b and diffs against tests/expected/<name>.out.
# Phase 2 runs scripted TCL sessions in a throwaway account and diffs
# against tests/expected/tcl-<name>.out (paths normalised).
# Phase 3 checks sieve correctness.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MVX="$ROOT/build/bin/mvx-basic"
TCL="$ROOT/build/bin/mvx"
EXP="$ROOT/tests/expected"

BLESS=0
QUICK=0
for a in "$@"; do
  case "$a" in
    --bless) BLESS=1 ;;
    -q) QUICK=1 ;;
    *) echo "usage: test.sh [-q] [--bless]" >&2; exit 2 ;;
  esac
done

[ -x "$MVX" ] || { echo "test.sh: build first (ninja -C build)" >&2; exit 2; }
mkdir -p "$EXP"

PASS=0
FAIL=0
TESTROOT="$(mktemp -d /tmp/mvxtest.XXXXXX)"
trap 'rm -rf "$TESTROOT"' EXIT

unset MVXACCOUNT MVXSESSION MVXPRIV MVXSYSTEM MVXPKGPATH MVXDRIVERS \
      MVX_SENTENCE 2>/dev/null || true

check() { # name actual-text
  name="$1"
  actual="$2"
  if [ "$BLESS" = 1 ]; then
    printf '%s\n' "$actual" > "$EXP/$name.out"
    echo "  blessed $name"
    return
  fi
  if [ ! -f "$EXP/$name.out" ]; then
    echo "FAIL $name: no expected output (run --bless)"
    FAIL=$((FAIL + 1))
    return
  fi
  if printf '%s\n' "$actual" | diff -u "$EXP/$name.out" - >"$TESTROOT/d" 2>&1
  then
    PASS=$((PASS + 1))
  else
    echo "FAIL $name:"
    sed 's/^/    /' "$TESTROOT/d" | head -20
    FAIL=$((FAIL + 1))
  fi
}

normalise() {
  # Substitute absolute roots, then collapse the column padding that
  # trails a substituted path.  A verb that FMT-pads a path column (e.g.
  # LIST-PKGS) sizes the padding from the *absolute* path length, which
  # differs by platform; @ROOT@ hides the path but not the trailing
  # spaces, so squeeze 2+ spaces after a normalised path token to one.
  sed -E -e "s#$TESTROOT#@TESTROOT@#g" -e "s#$ROOT#@ROOT@#g" \
         -e "s#(@(TEST)?ROOT@[^ ]*)  +#\1 #g" \
         -e 's/^([a-z][a-z0-9_-]*)@[0-9][^ ]* +/\1@VER /'
}

# ---------------------------------------------------------------- phase 1
echo "== language tests"

lang() { # name [stdin-text]
  name="$1"
  stdin="${2-}"
  out="$TESTROOT/$name"
  if ! "$MVX" "$ROOT/tests/$name.b" -o "$out" 2>"$TESTROOT/cerr"; then
    check "$name" "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
    return
  fi
  if [ -n "$stdin" ]; then
    actual="$(printf '%s\n' "$stdin" | "$out" 2>&1 | normalise)"
  else
    actual="$("$out" 2>&1 | normalise)"
  fi
  check "$name" "$actual"
}

lang smoke
lang goto
lang case
lang dynarr
lang dynalias
lang conv
lang strfns world
lang strmath
lang ifdef
lang equate
lang matparse
lang include
lang matches
lang loopctl
lang assign
lang vector
lang ongoto
lang uname
lang opendict

# STOP <code> sets the process exit status (CHECK-style CI gating): capture
# both stdout and the exit code so the whole contract is pinned.
out="$TESTROOT/stopcode"
if "$MVX" "$ROOT/tests/stopcode.b" -o "$out" 2>"$TESTROOT/cerr"; then
  scout="$("$out" 2>&1)" ; sc=$?
  check "stopcode" "out=[$scout] exit=$sc"
else
  check "stopcode" "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# KEYIN decoding from piped bytes (printable, specials, escape
# sequences, ESC pushback)
out="$TESTROOT/keyin"
if "$MVX" "$ROOT/tests/keyin.b" -o "$out" 2>"$TESTROOT/cerr"; then
  actual="$(printf 'a\r\t\177\033[A\033OB\033[3~\033OP\033[24~\001\033q' | \
            "$out" 2>&1)"
  check keyin "$actual"
else
  check keyin "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# mouse reporting (#57): SGR mouse reports decode to KEYIN "MOUSE", MOUSE()
# carries col/row/button/event; left-press, right-release, wheel-up, a
# button-held motion (DRAG), then a plain key still reads.
out="$TESTROOT/mouse"
if "$MVX" "$ROOT/tests/mouse.b" -o "$out" 2>"$TESTROOT/cerr"; then
  actual="$(printf '\033[<0;10;5M\033[<2;3;7m\033[<64;1;1M\033[<32;15;9Mxq' | \
            "$out" 2>&1)"
  check mouse "$actual"
else
  check mouse "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# CALL across separately compiled sources
out="$TESTROOT/callmain"
"$MVX" "$ROOT/tests/callmain.b" "$ROOT/tests/adder.b" -o "$out" \
  2>/dev/null && check callmain "$("$out" 2>&1)" \
  || check callmain "COMPILE FAILED"

# user-defined FUNCTIONs (DEFFUN + RETURN(value)), linked from many sources
out="$TESTROOT/funcs"
if "$MVX" "$ROOT/tests/funcs.b" "$ROOT/tests/func_square.b" \
     "$ROOT/tests/func_fact.b" "$ROOT/tests/func_greet.b" -o "$out" \
     2>"$TESTROOT/cerr"; then
  check funcs "$("$out" 2>&1)"
else
  check funcs "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# storage tests run inside their own account directory
STACCT="$TESTROOT/stacct"
mkdir -p "$STACCT/DIRDATA"
for t in store storedir dict matread readv locked onerror; do
  out="$TESTROOT/$t"
  if "$MVX" "$ROOT/tests/$t.b" -o "$out" 2>"$TESTROOT/cerr"; then
    actual="$(cd "$STACCT" && MVXACCOUNT=. "$out" 2>&1 | normalise)"
    check "$t" "$actual"
  else
    check "$t" "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
  fi
done

# OSEXEC — fine-grained command whitelist (#80): at the restricted tier a
# command runs only when a .mvx-private/permissions grant permits it for the
# caller's OS group; argv-style, so metacharacters are inert.
PACCT="$TESTROOT/pacct"
mkdir -p "$PACCT/.mvx-private"
{ echo "permit * = echo"; echo "permit $(id -gn) = true"; echo "deny * = echo -n -r --long"; } > "$PACCT/.mvx-private/permissions"
out="$TESTROOT/osexec"
if "$MVX" "$ROOT/tests/osexec.b" -o "$out" 2>"$TESTROOT/cerr"; then
  actual="$(cd "$PACCT" && MVXACCOUNT=. "$out" 2>/dev/null | normalise)"
  check osexec "$actual"
else
  check osexec "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# system-account layer (#80): a system-account 'deny' overrides an account
# 'permit' — the account grants echo, the system denies it, deny wins.
PSYS="$TESTROOT/pacct-sys"
mkdir -p "$PSYS/.mvx-private"
echo "permit * = echo" > "$PSYS/.mvx-private/permissions"
SYSACCT="$TESTROOT/sysacct"
mkdir -p "$SYSACCT/.mvx-private"
echo "deny * = echo" > "$SYSACCT/.mvx-private/permissions"
out="$TESTROOT/osexec_sys"
if "$MVX" "$ROOT/tests/osexec_sys.b" -o "$out" 2>"$TESTROOT/cerr"; then
  actual="$(cd "$PSYS" && MVXACCOUNT=. MVXSYSTEM="$SYSACCT" "$out" 2>/dev/null | normalise)"
  check osexec_sys "$actual"
else
  check osexec_sys "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# native FS primitives (#80): MKDIR / RMTREE / UNTAR, permit-gated by op name.
# Account grants mkdir + untar but not rmtree, so the first two run and rmtree
# is refused.
PNAT="$TESTROOT/pnat"
mkdir -p "$PNAT/.mvx-private"
echo "permit * = mkdir untar" > "$PNAT/.mvx-private/permissions"
( cd "$TESTROOT" && mkdir -p fx && printf 'native-ok' > fx/marker.txt \
  && tar czf "$PNAT/fixture.tgz" -C fx . )
( cd "$TESTROOT" && mkdir -p wfx/pkg && printf 'wrapped-ok' > wfx/pkg/inner.txt \
  && tar czf "$PNAT/wrapped.tgz" -C wfx pkg )
out="$TESTROOT/native"
if "$MVX" "$ROOT/tests/native.b" -o "$out" 2>"$TESTROOT/cerr"; then
  actual="$(cd "$PNAT" && MVXACCOUNT=. "$out" 2>/dev/null | normalise)"
  check native "$actual"
else
  check native "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# program-identity binding (#80, 8.4 constraint 3): a prog:NAME grant applies
# only when the running binary matches the system-blessed sha256 for NAME — a
# stale hash (re-cataloged / substituted binary) no longer inherits the grant.
PPROG="$TESTROOT/pacct-prog"
mkdir -p "$PPROG/.mvx-private"
echo "permit prog:TESTPROG = echo" > "$PPROG/.mvx-private/permissions"
SYSPROG="$TESTROOT/sysacct-prog"
mkdir -p "$SYSPROG/.mvx-private"
out="$TESTROOT/osexec_prog"
if "$MVX" "$ROOT/tests/osexec_prog.b" -o "$out" 2>"$TESTROOT/cerr"; then
  if command -v sha256sum >/dev/null 2>&1; then H="$(sha256sum "$out" | cut -d' ' -f1)"
  else H="$(shasum -a 256 "$out" | cut -d' ' -f1)"; fi
  echo "program TESTPROG = $H" > "$SYSPROG/.mvx-private/programs"
  a1="$(cd "$PPROG" && MVXACCOUNT=. MVXSYSTEM="$SYSPROG" "$out" 2>/dev/null)"
  echo "program TESTPROG = 0000000000000000000000000000000000000000000000000000000000000000" \
    > "$SYSPROG/.mvx-private/programs"
  a2="$(cd "$PPROG" && MVXACCOUNT=. MVXSYSTEM="$SYSPROG" "$out" 2>/dev/null)"
  check osexec_prog "$(printf 'blessed: %s\nstale: %s' "$a1" "$a2")"
else
  check osexec_prog "COMPILE FAILED: $(cat "$TESTROOT/cerr")"
fi

# ---------------------------------------------------------------- phase 2
echo "== system tests"

ACCT="$TESTROOT/acct"
"$ROOT/scripts/mkaccount.sh" "$ACCT" >/dev/null

tclrun() { # stdin piped in by caller
  "$TCL" -a "$ACCT" 2>&1 | normalise
}

# file lifecycle through the verbs
check tcl-files "$(printf '%s\n' \
  'CREATE-FILE STOCKF' \
  'COUNT STOCKF' \
  'LISTF' \
  'DELETE-FILE STOCKF' \
  'LISTF' | tclrun)"

# seed a queryable file with a dictionary
seed="$TESTROOT/seed.b"
cat > "$seed" <<'EOF'
X = CREATEFILE("PARTS")
OPEN "PARTS" TO F ELSE STOP
WRITE "Widget":@AM:"999":@AM:"blue" ON F, "W100"
WRITE "Gadget":@AM:"450":@AM:"red" ON F, "G200"
WRITE "Sprocket":@AM:"125":@AM:"blue" ON F, "S300"
OPEN "DICT", "PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"MD2$":@AM:"Price":@AM:"10R" ON D, "PRICE"
WRITE "D":@AM:"3":@AM:"":@AM:"Colour":@AM:"8L" ON D, "COLOR"
PRINT "seeded"
EOF
"$MVX" "$seed" -o "$TESTROOT/seedbin" 2>/dev/null
(cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/seedbin") >/dev/null

check tcl-query "$(printf '%s\n' \
  'LIST PARTS NAME PRICE COLOR BY PRICE' \
  'LIST PARTS NAME WITH COLOR = blue' \
  'SELECT PARTS WITH COLOR = blue' \
  'COUNT PARTS' \
  'COUNT PARTS' \
  'LIST DICT PARTS' \
  'CT DICT PARTS PRICE' | tclrun)"

# SORT (LIST sorted by id by default, or BY key) and SSELECT (a sorted
# select list feeding the next command)
check tcl-sort "$(printf '%s\n' \
  'SORT PARTS NAME PRICE COLOR' \
  'SORT PARTS NAME PRICE BY PRICE' \
  'SSELECT PARTS' \
  'LIST PARTS NAME' \
  'SSELECT PARTS WITH COLOR = blue' \
  'LIST PARTS NAME' | tclrun)"

# multivalue explosion + dictionary associations (D-item attr 6): an
# ORDERS file whose PRODUCT/QTY/PRICE are a parallel-multivalue group.
# O1 has two line items, O2 one; the associated columns explode onto
# aligned sub-rows while the single-valued Customer shows once.
oseed="$TESTROOT/oseed.b"
cat > "$oseed" <<'EOF'
X = CREATEFILE("ORDERS")
OPEN "ORDERS" TO F ELSE STOP
R = ""
R<1> = "Acme Corp"
R<5> = "Widget":@VM:"Gadget"
R<6> = "2":@VM:"1"
R<7> = "999":@VM:"450"
WRITE R ON F, "O1"
R = ""
R<1> = "Beta Ltd"
R<5> = "Sprocket"
R<6> = "5"
R<7> = "125"
WRITE R ON F, "O2"
OPEN "DICT", "ORDERS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"12L" ON D, "CUSTOMER"
WRITE "D":@AM:"5":@AM:"":@AM:"Product":@AM:"10L":@AM:"ORDERITEMS" ON D, "PRODUCT"
WRITE "D":@AM:"6":@AM:"":@AM:"Qty":@AM:"5R":@AM:"ORDERITEMS" ON D, "QTY"
WRITE "D":@AM:"7":@AM:"MD2$":@AM:"Price":@AM:"8R":@AM:"ORDERITEMS" ON D, "PRICE"
PRINT "seeded"
EOF
"$MVX" "$oseed" -o "$TESTROOT/oseedbin" 2>/dev/null
(cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/oseedbin") >/dev/null

check tcl-assoc "$(printf '%s\n' \
  'LIST ORDERS CUSTOMER PRODUCT QTY PRICE' \
  'SORT ORDERS CUSTOMER PRODUCT QTY PRICE' | tclrun)"

# TRANS() foreign-key lookup (#41): a CUSTMASTER file keyed by customer name,
# and a dedicated TORD orders file with a TRANS I-type that reads the city
# per record. Tests the TRANS()/XLATE() intrinsic, a TRANS I-type column,
# WITH on it, and the missing-record control codes. Own files — nothing
# shared is mutated.
tseed="$TESTROOT/tseed.b"
cat > "$tseed" <<'EOF'
X = CREATEFILE("CUSTMASTER")
Y = CREATEFILE("TORD")
OPEN "CUSTMASTER" TO C ELSE STOP
WRITE "Sydney":@AM:"NSW" ON C, "Acme Corp"
WRITE "Melbourne":@AM:"VIC" ON C, "Beta Ltd"
OPEN "TORD" TO F ELSE STOP
WRITE "Acme Corp":@AM:"Widget" ON F, "O1"
WRITE "Beta Ltd":@AM:"Gadget" ON F, "O2"
WRITE "Nobody Inc":@AM:"Orphan" ON F, "O9"
OPEN "DICT", "TORD" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"12L" ON D, "CUSTOMER"
WRITE "D":@AM:"2":@AM:"":@AM:"Product":@AM:"10L" ON D, "PRODUCT"
WRITE "I":@AM:"TRANS(CUSTMASTER,1,1,X)":@AM:"":@AM:"City":@AM:"12L" ON D, "CITY"
WRITE "I":@AM:"TRANS(CUSTMASTER,1,2,X)":@AM:"":@AM:"State":@AM:"6L" ON D, "CSTATE"
PRINT "trans-seeded"
PRINT "intrinsic: ":TRANS("CUSTMASTER", "Beta Ltd", 1, "X")
PRINT "orphan X: '":TRANS("CUSTMASTER", "Ghost", 1, "X"):"'"
PRINT "orphan C: '":TRANS("CUSTMASTER", "Ghost", 1, "C"):"'"
MVK = "Beta Ltd":@VM:"Ghost":@VM:"Acme Corp"
PRINT "mv X: ":CHANGE(TRANS("CUSTMASTER", MVK, 1, "X"), @VM, "/")
PRINT "mv C: ":CHANGE(TRANS("CUSTMASTER", MVK, 1, "C"), @VM, "/")
EOF
"$MVX" "$tseed" -o "$TESTROOT/tseedbin" 2>/dev/null
check tcl-trans "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/tseedbin"); \
  printf '%s\n' \
    'LIST TORD CUSTOMER CITY CSTATE BY @ID' \
    'LIST TORD CUSTOMER CITY WITH CSTATE = "VIC"' | tclrun)"

# SORT / SSELECT BY a TRANS I-type column (#53b): the sort key is the
# per-record foreign-key lookup, evaluated the same way LIST displays it
# (an orphan sorts as blank, ahead of the real cities). Own local file, so
# this is the client-side reference the co-located JOIN push-down must match.
check tcl-sorttrans "$(printf '%s\n' \
  'SORT TORD CUSTOMER CITY BY CITY' \
  'SORT TORD CUSTOMER CITY BY CITY FIRST 2' \
  'SSELECT TORD BY CITY' \
  'LIST TORD CITY' | tclrun)"

# Nested TRANS (#63): a TRANS whose target names a dict item recurses through
# the target file's dictionary. Chain NORD -> NCUST -> NREGION: NORD.CUSTREGION
# = TRANS(NCUST,1,REGIONNAME,X) and NCUST.REGIONNAME = TRANS(NREGION,2,1,X), so
# an order resolves to its customer's region. Covers the TRANS()/IEVAL runtime
# evaluator and a nested TRANS column through the verbs (a multivalued key maps
# element-wise, an orphan is blank). Own local files.
nnest="$TESTROOT/nnest.b"
cat > "$nnest" <<'EOF'
X = CREATEFILE("NREGION")
X = CREATEFILE("NCUST")
X = CREATEFILE("NORD")
OPEN "NREGION" TO RG ELSE STOP
WRITE "North" ON RG, "R1"
WRITE "South" ON RG, "R2"
OPEN "NCUST" TO CU ELSE STOP
WRITE "Acme":@AM:"R1" ON CU, "C1"
WRITE "Beta":@AM:"R2" ON CU, "C2"
OPEN "NORD" TO ND ELSE STOP
WRITE "C1":@AM:"Widget" ON ND, "O1"
WRITE "C2":@AM:"Gadget" ON ND, "O2"
WRITE "C9":@AM:"Orphan" ON ND, "O3"
OPEN "DICT", "NCUST" TO DCU ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"10L" ON DCU, "NAME"
WRITE "I":@AM:"TRANS(NREGION,2,1,X)":@AM:"":@AM:"Region":@AM:"8L" ON DCU, "REGIONNAME"
OPEN "DICT", "NORD" TO DND ELSE STOP
WRITE "D":@AM:"2":@AM:"":@AM:"Product":@AM:"10L" ON DND, "PRODUCT"
WRITE "I":@AM:"TRANS(NCUST,1,1,X)":@AM:"":@AM:"Customer":@AM:"10L" ON DND, "CUSTNAME"
WRITE "I":@AM:"TRANS(NCUST,1,REGIONNAME,X)":@AM:"":@AM:"Region":@AM:"8L" ON DND, "CUSTREGION"
PRINT "flat   : ":TRANS("NCUST", "C1", 1, "X")
PRINT "nested : ":TRANS("NCUST", "C1", "REGIONNAME", "X")
PRINT "nestmv : ":CHANGE(TRANS("NCUST", "C1":@VM:"C2", "REGIONNAME", "X"), @VM, "/")
PRINT "orphan : '":TRANS("NCUST", "C9", "REGIONNAME", "X"):"'"
READ ROW FROM ND, "O1" ELSE STOP
PRINT "ieval  : ":IEVAL(ROW, "TRANS(NCUST,1,REGIONNAME,X)")
EOF
"$MVX" "$nnest" -o "$TESTROOT/nnestbin" 2>/dev/null
check tcl-transnest "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/nnestbin"); \
  printf '%s\n' \
    'LIST NORD PRODUCT CUSTNAME CUSTREGION BY @ID' \
    'SORT NORD PRODUCT CUSTREGION BY CUSTREGION' | tclrun)"

# JSON encode/decode (#24): the shared mapper + MAPFIELD build a mapping,
# JSONENCODE renders the record (single attrs -> scalar keys, an association ->
# array of objects; NUMERIC via MD2 unquoted, an absent value null), JSONDECODE
# maps a record back, and arbitrary inbound JSON is decoded on the fly.  No
# storage — a standalone program, so it runs anywhere.
jsonsrc="$TESTROOT/json.b"
cat > "$jsonsrc" <<'EOF'
SPEC = ""
SPEC<-1> = MAPFIELD("customer", 1)
SPEC<-1> = MAPFIELD("product", 5, "", "TEXT", "items")
SPEC<-1> = MAPFIELD("qty", 6, "", "NUMERIC", "items")
SPEC<-1> = MAPFIELD("price", 7, "MD2", "", "items")
R = ""
R<1> = "Acme Corp"
R<5> = "Widget":@VM:"Gadget"
R<6> = "2":@VM:"1"
R<7> = "999":@VM:"450"
PRINT JSONENCODE(R, SPEC)
R2 = JSONDECODE(JSONENCODE(R, SPEC), SPEC)
PRINT "roundtrip: ":R2<1>:" | ":CHANGE(R2<5>, @VM, "/"):" | ":CHANGE(R2<7>, @VM, "/")
IN = '{"customer":"Beta","items":[{"product":"Nut","qty":5,"price":null}]}'
R3 = JSONDECODE(IN, SPEC)
PRINT "inbound: ":R3<1>:" ":R3<5>:" price=[":CHANGE(R3<7>, @VM, "/"):"]"
EOF
"$MVX" "$jsonsrc" -o "$TESTROOT/jsonbin" 2>/dev/null
check tcl-json "$("$TESTROOT/jsonbin")"

# http extension package (#68): HTTPGET / HTTPGETFILE against a throwaway local
# server.  Needs python3 for the server; skipped where absent.
if command -v python3 >/dev/null 2>&1; then
  HSRV="$TESTROOT/httpsrv"; mkdir -p "$HSRV"
  printf 'hello from mvx http' > "$HSRV/hello.txt"
  # Let the OS pick a free port, so a leftover server from an aborted run can
  # never make us bind-fail (and then answer 404 from a since-deleted dir).
  HPORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
  python3 -m http.server "$HPORT" --directory "$HSRV" >/dev/null 2>&1 &
  HPID=$!
  sleep 1
  HDL="$TESTROOT/http.dl"
  httpsrc="$TESTROOT/http.b"
  cat > "$httpsrc" <<EOF
X = HTTPGET("http://127.0.0.1:$HPORT/hello.txt")
PRINT "get: [":X:"] len=":LEN(X)
S = HTTPGETFILE("http://127.0.0.1:$HPORT/hello.txt", "$HDL")
PRINT "getfile status=":S
PRINT "missing len=":LEN(HTTPGET("http://127.0.0.1:$HPORT/nope"))
EOF
  "$MVX" "$httpsrc" -o "$TESTROOT/httpbin" 2>/dev/null
  check tcl-http "$("$TESTROOT/httpbin"; echo "downloaded: [$(cat "$HDL" 2>/dev/null)]")"
  { kill "$HPID" && wait "$HPID"; } 2>/dev/null
else
  echo "  (http test skipped — python3 not found)"
fi

# TRIM(str, char[, option]) (#69): the character-trimming variants.
trimsrc="$TESTROOT/trim.b"
cat > "$trimsrc" <<'EOF'
PRINT "[":TRIM("  a  b  "):"]"                ;* classic squeeze
PRINT "[":TRIM("xxaxxbxx", "x"):"]"           ;* R (default)
PRINT "[":TRIM("xxaxxbxx", "x", "L"):"]"      ;* leading
PRINT "[":TRIM("xxaxxbxx", "x", "T"):"]"      ;* trailing
PRINT "[":TRIM("xxaxxbxx", "x", "B"):"]"      ;* both ends
PRINT "[":TRIM("xxaxxbxx", "x", "A"):"]"      ;* all
EOF
"$MVX" "$trimsrc" -o "$TESTROOT/trimbin" 2>/dev/null
check tcl-trim "$("$TESTROOT/trimbin")"

# JSON verb (#24): MAPSPEC derives the mapping from the file's dictionary, and
# the verb prepends the record id.  Local LMDB dicted file with an ITEMS assoc.
jvseed="$TESTROOT/jvseed.b"
cat > "$jvseed" <<'EOF'
X = CREATEFILE("JVF")
OPEN "JVF" TO F ELSE STOP
R = ""
R<1> = "Acme Corp"
R<5> = "Widget":@VM:"Gadget"
R<7> = "999":@VM:"450"
WRITE R ON F, "O1"
OPEN "DICT", "JVF" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"12L" ON D, "CUSTOMER"
WRITE "D":@AM:"5":@AM:"":@AM:"Product":@AM:"10L":@AM:"ITEMS" ON D, "PRODUCT"
WRITE "D":@AM:"7":@AM:"MD2":@AM:"Price":@AM:"8R":@AM:"ITEMS" ON D, "PRICE"
PRINT "jv-seeded"
EOF
"$MVX" "$jvseed" -o "$TESTROOT/jvseedbin" 2>/dev/null
check tcl-jsonverb "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/jvseedbin"); \
  printf 'JSON JVF O1\n' | tclrun)"

# COUNT with WITH (#45): on a local file the backend has no count push-down,
# so COUNT falls back to forming a filtered list and counting it. Also plain
# COUNT of every record.
cnseed="$TESTROOT/cnseed.b"
cat > "$cnseed" <<'EOF'
X = CREATEFILE("CNT")
OPEN "CNT" TO F ELSE STOP
WRITE "Widget":@AM:"NSW" ON F, "C1"
WRITE "Gadget":@AM:"VIC" ON F, "C2"
WRITE "Bolt":@AM:"NSW" ON F, "C3"
WRITE "Nut":@AM:"NSW" ON F, "C4"
OPEN "DICT", "CNT" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
PRINT "cnt-seeded"
EOF
"$MVX" "$cnseed" -o "$TESTROOT/cnseedbin" 2>/dev/null
check tcl-count "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/cnseedbin"); \
  printf '%s\n' \
    'COUNT CNT' \
    'COUNT CNT WITH STATE = "NSW"' \
    'COUNT CNT WITH STATE = "VIC"' | tclrun)"

# SUM (#46): total a numeric field. On a local file it scans, OCONV's the
# field, and adds; PRICE is MD2 so 999 totals as 9.99. Also a filtered sum.
smseed="$TESTROOT/smseed.b"
cat > "$smseed" <<'EOF'
X = CREATEFILE("SMF")
OPEN "SMF" TO F ELSE STOP
WRITE "NSW":@AM:"999" ON F, "O1"
WRITE "VIC":@AM:"450" ON F, "O2"
WRITE "NSW":@AM:"1200" ON F, "O3"
OPEN "DICT", "SMF" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
PRINT "sm-seeded"
EOF
"$MVX" "$smseed" -o "$TESTROOT/smseedbin" 2>/dev/null
check tcl-sum "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/smseedbin"); \
  printf '%s\n' \
    'SUM SMF PRICE' \
    'SUM SMF PRICE WITH STATE = "NSW"' | tclrun)"

# BY + FIRST n (#47): top-N by a field. On a local file the verb sorts and
# caps at n. Distinct keys (so the LIMIT boundary is unambiguous): PRICE is
# right-justified (numeric order), STATE left (byte order).
ftseed="$TESTROOT/ftseed.b"
cat > "$ftseed" <<'EOF'
X = CREATEFILE("FST")
OPEN "FST" TO F ELSE STOP
WRITE "NSW":@AM:"999" ON F, "P1"
WRITE "VIC":@AM:"450" ON F, "P2"
WRITE "QLD":@AM:"1200" ON F, "P3"
WRITE "ACT":@AM:"300" ON F, "P4"
OPEN "DICT", "FST" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
PRINT "ft-seeded"
EOF
"$MVX" "$ftseed" -o "$TESTROOT/ftseedbin" 2>/dev/null
check tcl-first "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/ftseedbin"); \
  printf '%s\n' \
    'SORT FST STATE PRICE BY PRICE FIRST 2' \
    'SORT FST STATE BY STATE FIRST 2' | tclrun)"

# range WITH (#48): >, <, >=, <= filter. On a local file the verb compares
# (numeric for the right-justified PRICE); FST reused (prices 999,450,1200,300
# internal). A text range on STATE stays a byte-order scan.
check tcl-range "$( \
  printf '%s\n' \
    'SORT FST STATE PRICE WITH PRICE > "500" BY @ID' \
    'SORT FST STATE PRICE WITH PRICE <= "450" BY @ID' \
    'SORT FST STATE WITH STATE > "N" BY STATE' | tclrun)"

# multi-condition WITH (#49): conditions AND together. On a local file LIST
# filters all of them; FST5 adds a second NSW below 500 so the AND discriminates.
mwseed="$TESTROOT/mwseed.b"
cat > "$mwseed" <<'EOF'
X = CREATEFILE("MWF")
OPEN "MWF" TO F ELSE STOP
WRITE "NSW":@AM:"999" ON F, "O1"
WRITE "VIC":@AM:"450" ON F, "O2"
WRITE "QLD":@AM:"1200" ON F, "O3"
WRITE "NSW":@AM:"100" ON F, "O5"
OPEN "DICT", "MWF" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
PRINT "mw-seeded"
EOF
"$MVX" "$mwseed" -o "$TESTROOT/mwseedbin" 2>/dev/null
check tcl-multiwith "$( \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/mwseedbin"); \
  printf '%s\n' \
    'LIST MWF STATE PRICE WITH STATE = "NSW" AND PRICE > "500" BY @ID' \
    'LIST MWF STATE PRICE WITH STATE = "NSW" WITH PRICE < "500" BY @ID' \
    'SORT MWF STATE PRICE WITH STATE = "NSW" AND PRICE > "500"' \
    'SELECT MWF WITH STATE = "NSW" AND PRICE < "500"' \
    'LIST MWF STATE PRICE' | tclrun)"

# DESCRIBE (#51) on a local (LMDB) file: no SQL backend, so the plan states the
# selection resolves in the driver and the verb applies the conditions. An SQL
# backend renders the actual query instead (tcl-pgdescribe). MWF reused.
check tcl-describe "$( \
  printf '%s\n' \
    'LIST DESCRIBE MWF STATE WITH STATE = "NSW"' \
    'SELECT MWF WITH PRICE > "500" DESCRIBE' | tclrun)"

# SQL mapping (#18 phase 1): the dictionary -> relational schema. Single
# attrs become parent columns; the ORDERITEMS association a child table.
# First a selective map (QTY omitted), then map-all with the data preview.
check tcl-map "$(printf '%s\n' \
  'MAP ORDERS CUSTOMER PRODUCT PRICE' \
  'MAP ORDERS DATA' | tclrun)"

# account credential store (.mvx-private): set/list with values masked,
# and upsert replacing an existing entry in place
check tcl-cred "$(printf '%s\n' \
  'SET-CREDENTIAL lmdbnet mvxdb-a:4300 SALES token=abc123' \
  'SET-CREDENTIAL postgres db:5432 mvx user=app password=s3cret' \
  'LIST-CREDENTIALS' \
  'SET-CREDENTIAL lmdbnet mvxdb-a:4300 SALES token=NEWTOK' \
  'LIST-CREDENTIALS' | tclrun)"

# record verbs + ED scripted session
check tcl-records "$(printf '%s\n' \
  'COPY PARTS W100 TO W900' \
  'CT PARTS W900' \
  'DELETE PARTS W900' \
  'ED PARTS NOTE1' \
  'I' \
  'alpha' \
  'beta' \
  '.' \
  'R/beta/BETA' \
  'FI' \
  'CT PARTS NOTE1' \
  'DELETE PARTS NOTE1' | tclrun)"

# privilege gate
check tcl-gate "$(printf '!echo leaked\n' | tclrun; \
  printf '!echo allowed\n' | MVXPRIV=unrestricted "$TCL" -a "$ACCT" 2>&1)"

# self-hosting: BASIC + CATALOG a verb, then run it.  BP is a directory
# file, so its dictionary carries a %FILE% marking it "dir".
mkdir -p "$ACCT/BP" "$ACCT/BP.DICT"
printf 'FILE\375dir\n' > "$ACCT/BP.DICT/%FILE%"
cat > "$ACCT/BP/HELLO" <<'EOF'
* /**
*  * @file HELLO
*  * @version 3.1
*  */
PRINT "hello from HELLO"
PRINT "sentence: ":SENTENCE()
EOF
check tcl-selfhost "$( \
  printf 'BASIC BP HELLO\n' | tclrun; \
  printf 'CATALOG BP HELLO\n' | MVXPRIV=developer "$TCL" -a "$ACCT" 2>&1; \
  printf 'HELLO A B\n' | tclrun)"

# docblocks through I-type dictionary items
printf 'I\nDOCTAG(file)\n\nFile\n10L\n' > "$ACCT/BP.DICT/FILE"
printf 'I\nDOCTAG(version)\n\nVersion\n8L\n' > "$ACCT/BP.DICT/VERSION"
check tcl-docblock "$(printf 'LIST BP FILE VERSION\n' | tclrun)"

# packages: build, link (dependency pulls cmd -> getopt), GIT help, unlink rules
"$ROOT/scripts/mkpkg.sh" "$ROOT/packages/getopt" >/dev/null
"$ROOT/scripts/mkpkg.sh" "$ROOT/packages/cmd" >/dev/null
"$ROOT/scripts/mkpkg.sh" "$ROOT/packages/git" >/dev/null
check tcl-packages "$(printf '%s\n' \
  "LINK-PKG $ROOT/packages/git" \
  'LIST-PKGS' \
  'GIT' \
  "UNLINK-PKG $ROOT/packages/cmd" \
  "UNLINK-PKG $ROOT/packages/git" \
  "UNLINK-PKG $ROOT/packages/cmd" | tclrun)"

# getopt: the declarative option parser exercised through the real LINK-PKG path.
# A consumer verb declares flags once; GETOPT.PARSE splits the sentence (quoted
# multi-word value, --flag, positionals) and the accessors read the result out
# of COMMON /GETOPT/ across the package boundary.
"$ROOT/scripts/mkpkg.sh" "$ROOT/packages/getopt" >/dev/null
GOP="$TESTROOT/gotest"
mkdir -p "$GOP/BP" "$GOP/VOC"
printf 'gotest\n1.0\ngetopt consumer\nmvx\n' > "$GOP/PKG"
cat > "$GOP/BP/GOTEST" <<'EOF'
SPEC = ""
CALL GETOPT.OPT(SPEC, "m", "message", 1, "", "commit message")
CALL GETOPT.OPT(SPEC, "o", "open", 0, "", "open format")
CALL GETOPT.SENTENCE(S)
CALL GETOPT.PARSE(SPEC, S, 1)
CALL GETOPT.VAL("m", MSG) ; PRINT "m=[" : MSG : "]"
CALL GETOPT.HAS("open", ISON) ; PRINT "open=" : ISON
CALL GETOPT.ARG(1, A1) ; PRINT "a1=[" : A1 : "]"
CALL GETOPT.NARGS(NA) ; PRINT "nargs=" : NA
EOF
printf 'V\nCATALOG/GOTEST' > "$GOP/VOC/GOTEST"
check tcl-getopt "$( \
  printf 'BUILD-PKG %s\n' "$GOP" | MVXPRIV=developer "$TCL" -a "$ACCT" 2>&1 | normalise; \
  printf '%s\n' "LINK-PKG $ROOT/packages/getopt" "LINK-PKG $GOP" \
    'GOTEST -m "hi there" one --open two' \
    "UNLINK-PKG $GOP" "UNLINK-PKG $ROOT/packages/getopt" | tclrun)"

# cmd declarative flags: a subcommand declares flags with CMD.FLAG; CMD.RUN parses
# the sentence against them via getopt and the handler reads GETOPT.VAL/HAS/ARG.
# Covers parse (quoted value + flag + positionals), generated per-sub --help, and
# an unknown-flag error.  Exercises the full cmd -> getopt dependency chain.
CFP="$TESTROOT/cftest"
mkdir -p "$CFP/BP" "$CFP/VOC"
printf 'cftest\n1.0\ncmd-flag test\nmvx\n' > "$CFP/PKG"
cat > "$CFP/BP/CFTEST" <<'EOF'
CALL CMD.INIT("CFTEST", "cmd flag demo")
CALL CMD.ADD("COMMIT", "record staged changes", "CFTEST.COMMIT")
CALL CMD.FLAG("COMMIT", "m", "message", 1, "", "commit message")
CALL CMD.FLAG("COMMIT", "a", "all", 0, "", "stage all")
CALL CMD.RUN
EOF
cat > "$CFP/BP/CFTEST.COMMIT" <<'EOF'
SUBROUTINE CFTEST.COMMIT
CALL GETOPT.VAL("m", MSG) ; PRINT "m=[" : MSG : "]"
CALL GETOPT.HAS("all", ALLF) ; PRINT "all=" : ALLF
CALL GETOPT.ARG(1, A1) ; PRINT "a1=[" : A1 : "]"
CALL GETOPT.NARGS(NA) ; PRINT "nargs=" : NA
RETURN
EOF
printf 'V\nCATALOG/CFTEST' > "$CFP/VOC/CFTEST"
check tcl-cmdflags "$( \
  printf 'BUILD-PKG %s\n' "$CFP" | MVXPRIV=developer "$TCL" -a "$ACCT" 2>&1 | normalise; \
  printf '%s\n' "LINK-PKG $ROOT/packages/getopt" "LINK-PKG $ROOT/packages/cmd" "LINK-PKG $CFP" \
    'CFTEST COMMIT -m "hello world" --all f1 f2' \
    'CFTEST COMMIT --help' \
    'CFTEST COMMIT -z' \
    "UNLINK-PKG $CFP" "UNLINK-PKG $ROOT/packages/cmd" "UNLINK-PKG $ROOT/packages/getopt" | tclrun)"

# native package build: BUILD-PKG compiles a package's BP -> CATALOG/LIB
# through the runtime (no shell, no mkpkg on PATH), needing only developer
# privilege.  A main verb GREET plus a SUBROUTINE it CALLs proves both the
# CATALOG (exe) and LIB (shared) outputs are produced and resolve.  The
# subroutine's name carries a dot (GREET.SUB) — the MV convention — so this also
# guards that a dotted name still gets the platform lib suffix and resolves (a
# name-dot must not read as an extension).
GPK="$TESTROOT/greet"
mkdir -p "$GPK/BP" "$GPK/VOC"
printf 'greet\n1.0\na built-natively test package\nmvx\n' > "$GPK/PKG"
cat > "$GPK/BP/GREET" <<'EOF'
M = ""
CALL GREET.SUB(M)
PRINT M
EOF
cat > "$GPK/BP/GREET.SUB" <<'EOF'
SUBROUTINE GREET.SUB(R)
R = "hi from the built subroutine"
RETURN
EOF
printf 'V\nCATALOG/GREET' > "$GPK/VOC/GREET"
check tcl-buildpkg "$( \
  printf 'BUILD-PKG %s\n' "$GPK" | MVXPRIV=developer "$TCL" -a "$ACCT" 2>&1 | normalise; \
  printf '%s\n' "LINK-PKG $GPK" 'GREET' | tclrun)"

# account adoption: mvx-git commits a live account; a *plain* git clone lands
# the tracked (directory) form; mvx-git-adopt rebuilds the live hash-file account
# from it (the CI path — no export direction exists).
CONV="$ROOT/build/bin/mvx-git-adopt"
if command -v git >/dev/null 2>&1 && [ -x "$ROOT/build/bin/mvx-git" ]; then
  RTS="$TESTROOT/rtsrc"
  "$ROOT/scripts/mkaccount.sh" "$RTS" >/dev/null
  rtseed="$TESTROOT/rtseed.b"
  cat > "$rtseed" <<'EOF'
X = CREATEFILE("PARTS")
OPEN "PARTS" TO F ELSE STOP
WRITE "Widget":@AM:"999" ON F, "W100"
WRITE "Gadget":@AM:"450" ON F, "G200"
OPEN "DICT", "PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
OPEN "VOC" TO V ELSE STOP
WRITE "V":@AM:"CATALOG/FOO" ON V, "FOO"
WRITE "PA":@AM:"HELLO" ON V, "MYPARA"
PRINT "seeded"
EOF
  "$MVX" "$rtseed" -o "$TESTROOT/rtseedbin" 2>/dev/null
  (cd "$RTS" && MVXACCOUNT=. "$TESTROOT/rtseedbin") >/dev/null
  ( cd "$RTS" && "$ROOT/build/bin/mvx-git" init >/dev/null 2>&1 && \
    git config user.email t@t && git config user.name t && \
    "$ROOT/build/bin/mvx-git" add -A >/dev/null 2>&1 && \
    "$ROOT/build/bin/mvx-git" commit -m stock >/dev/null 2>&1 )
  RTC="$TESTROOT/rtclone"
  git clone -q "$RTS" "$RTC" >/dev/null 2>&1
  MVX="$TCL" "$CONV" "$RTC" >/dev/null 2>&1
  # WHICH VOC RECORDS SURVIVE THE ROUND TRIP, asserted both ways (mvx#133).
  # MYPARA is the account's own and must come back.  FOO is a catalogued verb --
  # `CATALOG` writes it and BUILD writes it again when it provisions the clone,
  # so it is derived plumbing and a wholesale add leaves it out, exactly like a
  # file's own pointer.  Asserting only the survivor would not notice the rule
  # being applied to the wrong half.
  check tcl-account "$(printf '%s\n' \
    'COUNT PARTS' \
    'LIST PARTS NAME BY NAME' \
    'CT VOC MYPARA' \
    'CT VOC FOO' | "$TCL" -a "$RTC" 2>&1 | normalise)"
else
  echo "  (skipping tcl-account: git or mvx-git unavailable)"
fi

# #71: CREATE-FILE registers the file in the VOC as an "F" file pointer (attr 1
# F, attr 2 data, attr 3 dictionary), for both directory and lmdb files;
# DELETE-FILE removes it.
CFV="$TESTROOT/cfvoc"
"$ROOT/scripts/mkaccount.sh" "$CFV" >/dev/null
check tcl-createfile-voc "$(printf '%s\n' \
  'CREATE-FILE PARTS DIR' \
  'CREATE-FILE ORDERS' \
  'CT VOC PARTS' \
  'CT VOC ORDERS' \
  'DELETE-FILE ORDERS' \
  'CT VOC ORDERS' | "$TCL" -a "$CFV" 2>&1 | normalise)"

# .mvx default hash type: CREATE-FILE with no explicit type uses the account's
# `hash =` default (F1 has none -> lmdb; after `hash = DIR`, F2 -> a directory
# file); an explicit type still overrides the default (F3 DIR).
DHA="$TESTROOT/defhash"
"$ROOT/scripts/mkaccount.sh" "$DHA" >/dev/null
check tcl-default-hash "$( \
  "$TCL" -a "$DHA" -c 'CREATE-FILE F1' >/dev/null 2>&1; \
  "$TCL" -a "$DHA" -c 'LISTF' 2>&1 | normalise | grep -E '^F1 '; \
  printf 'hash = DIR\n' >> "$DHA/.mvx"; \
  "$TCL" -a "$DHA" -c 'CREATE-FILE F2'     >/dev/null 2>&1; \
  "$TCL" -a "$DHA" -c 'CREATE-FILE F3 DIR' >/dev/null 2>&1; \
  "$TCL" -a "$DHA" -c 'LISTF' 2>&1 | normalise | grep -E '^F2 |^F3 ')"

# %FILE%-driven type: adopting a plain-git checkout rebuilds each file as the
# backend its dictionary's %FILE% names - a hash file for ORDERS, a directory
# file for ARCHIVE - and its dictionary survives.
if command -v git >/dev/null 2>&1 && [ -x "$ROOT/build/bin/mvx-git" ]; then
  RDS="$TESTROOT/rdsrc"
  "$ROOT/scripts/mkaccount.sh" "$RDS" >/dev/null
  rdseed="$TESTROOT/rdseed.b"
  cat > "$rdseed" <<'EOF'
X = CREATEFILE("ORDERS")
OPEN "ORDERS" TO F ELSE STOP
WRITE "a":@AM:"1" ON F, "O1"
WRITE "b":@AM:"2" ON F, "O2"
OPEN "DICT", "ORDERS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Who":@AM:"10L" ON D, "WHO"
X = CREATEFILE("ARCHIVE", "DIR")
OPEN "ARCHIVE" TO A ELSE STOP
WRITE "old":@AM:"x" ON A, "R1"
PRINT "seeded"
EOF
  "$MVX" "$rdseed" -o "$TESTROOT/rdseedbin" 2>/dev/null
  (cd "$RDS" && MVXACCOUNT=. "$TESTROOT/rdseedbin") >/dev/null
  ( cd "$RDS" && "$ROOT/build/bin/mvx-git" init >/dev/null 2>&1 && \
    git config user.email t@t && git config user.name t && \
    "$ROOT/build/bin/mvx-git" add -A >/dev/null 2>&1 && \
    "$ROOT/build/bin/mvx-git" commit -m stock >/dev/null 2>&1 )
  RDC="$TESTROOT/rdclone"
  git clone -q "$RDS" "$RDC" >/dev/null 2>&1
  MVX="$TCL" "$CONV" "$RDC" >/dev/null 2>&1
  check tcl-account-dict "$( \
    "$TCL" -a "$RDC" -c 'COUNT ORDERS' 2>&1; \
    "$TCL" -a "$RDC" -c 'CT DICT ORDERS WHO' 2>&1 | head -1; \
    "$TCL" -a "$RDC" -c 'COUNT ARCHIVE' 2>&1; \
    { [ -d "$RDC/ORDERS" ] && echo 'ORDERS still directory' || echo 'ORDERS is a hash file'; }; \
    { [ -d "$RDC/ARCHIVE" ] && echo 'ARCHIVE stays a directory file' || echo 'ARCHIVE lost'; })"
else
  echo "  (skipping tcl-account-dict: git or mvx-git unavailable)"
fi

# CONVERT-FILE: change one file's backend, records + dictionary intact
CFA="$TESTROOT/cfacct"
"$ROOT/scripts/mkaccount.sh" "$CFA" >/dev/null
cfseed="$TESTROOT/cfseed.b"
cat > "$cfseed" <<'EOF'
X = CREATEFILE("PARTS")
OPEN "PARTS" TO F ELSE STOP
WRITE "Widget":@AM:"999" ON F, "W100"
WRITE "Gadget":@AM:"450" ON F, "G200"
OPEN "DICT", "PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
PRINT "seeded"
EOF
"$MVX" "$cfseed" -o "$TESTROOT/cfseedbin" 2>/dev/null
(cd "$CFA" && MVXACCOUNT=. "$TESTROOT/cfseedbin") >/dev/null
check tcl-convert-file "$( \
  "$TCL" -a "$CFA" -c 'CONVERT-FILE PARTS dir' 2>&1; \
  { [ -d "$CFA/PARTS" ] && echo 'PARTS is now a directory file' || echo 'not a dir'; }; \
  "$TCL" -a "$CFA" -c 'COUNT PARTS' 2>&1; \
  "$TCL" -a "$CFA" -c 'CONVERT-FILE PARTS lmdb' 2>&1; \
  { [ -d "$CFA/PARTS" ] && echo 'still a dir' || echo 'PARTS is a hash file again'; }; \
  "$TCL" -a "$CFA" -c 'LIST PARTS NAME BY NAME' 2>&1 | normalise)"

# mvx-git: the git-wrapper bin command (built by the git package) clones
# an account's legible form and rebuilds its hash files.  Needs the real
# git CLI and the built wrapper, so it is skipped where either is absent.
if command -v git >/dev/null 2>&1 && [ -x "$ROOT/build/bin/mvx-git" ]; then
  MGS="$TESTROOT/mgsrc"
  "$ROOT/scripts/mkaccount.sh" "$MGS" >/dev/null
  mgseed="$TESTROOT/mgseed.b"
  cat > "$mgseed" <<'EOF'
X = CREATEFILE("PARTS")
OPEN "PARTS" TO F ELSE STOP
WRITE "Widget":@AM:"999" ON F, "W100"
OPEN "DICT", "PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
PRINT "seeded"
EOF
  "$MVX" "$mgseed" -o "$TESTROOT/mgseedbin" 2>/dev/null
  (cd "$MGS" && MVXACCOUNT=. "$TESTROOT/mgseedbin") >/dev/null
  ( cd "$MGS" && printf 'mvxdata.lmdb/\n' > .gitignore && \
    "$ROOT/build/bin/mvx-git" init >/dev/null 2>&1 && \
    git config user.email t@t && git config user.name t && \
    "$ROOT/build/bin/mvx-git" add -A >/dev/null 2>&1 && \
    "$ROOT/build/bin/mvx-git" commit -m acct >/dev/null 2>&1 ) >/dev/null 2>&1
  MGC="$TESTROOT/mgclone"
  MVXCONVERT="$CONV" MVX="$TCL" "$ROOT/build/bin/mvx-git" clone "$MGS" "$MGC" >/dev/null 2>&1
  # a repo WITHOUT a .mvx descriptor: plain clone stays plain; only an
  # explicit opt-in ($MVXGIT_CREATE) turns it into a new account.
  MGP="$TESTROOT/mgplain"
  mkdir -p "$MGP/BP"; printf 'PRINT "hi"\n' > "$MGP/BP/HELLO"
  ( cd "$MGP" && git init -q -b main && git add -A && \
    git -c user.email=t@t -c user.name=t commit -qm src ) >/dev/null 2>&1
  MGPD="$TESTROOT/mgplain-default"
  MVXCONVERT="$CONV" MVX="$TCL" "$ROOT/build/bin/mvx-git" clone "$MGP" "$MGPD" </dev/null >/dev/null 2>&1
  MGPY="$TESTROOT/mgplain-optin"
  MVXGIT_CREATE=1 MVXCONVERT="$CONV" MVX="$TCL" "$ROOT/build/bin/mvx-git" clone "$MGP" "$MGPY" </dev/null >/dev/null 2>&1
  # a legible/directory account (no LMDB store) commits via mvx-git with no
  # convert step — its records are already the tracked files, so mvx-git is
  # plain git for it and never grows an LMDB store.
  MGD="$TESTROOT/mgdir"
  mkdir -p "$MGD/BP"
  printf '# MVX account descriptor\nname = mgdir\nversion = 1\n' > "$MGD/.mvx"
  printf 'PRINT "hi"\n' > "$MGD/BP/HELLO"
  ( cd "$MGD" && git init -q -b main && \
    MVXCONVERT="$CONV" MVX="$TCL" "$ROOT/build/bin/mvx-git" add -A && \
    git -c user.email=t@t -c user.name=t commit -qm acct ) >/dev/null 2>&1
  check tcl-mvxgit "$( \
    { [ -d "$MGC/mvxdata.lmdb" ] && echo 'account clone (.mvx) rebuilt' \
        || echo 'account clone NOT rebuilt'; }; \
    "$TCL" -a "$MGC" -c 'COUNT PARTS' 2>&1; \
    { [ -f "$MGPD/.mvx" ] && echo 'plain clone became account (WRONG)' \
        || echo 'plain clone stayed plain'; }; \
    { [ -f "$MGPY/.mvx" ] && echo 'opt-in clone created account' \
        || echo 'opt-in clone NOT created'; }; \
    { [ -d "$MGD/mvxdata.lmdb" ] && echo 'DIR account grew an LMDB store (WRONG)' \
        || echo 'DIR account stayed legible'; }; \
    ( cd "$MGD" && git ls-files BP/HELLO ))"

  # #66: an account can carry a git submodule.  mvx-git stages it as a gitlink
  # (mode 160000), not by flattening the submodule's files into records, and
  # status stays clean.
  MGSUB="$TESTROOT/mgsubrepo"
  mkdir -p "$MGSUB"
  ( cd "$MGSUB" && git init -q -b main && printf '# page\n' > Home.md && \
    git add -A && git -c user.email=t@t -c user.name=t commit -qm init ) >/dev/null 2>&1
  MGA="$TESTROOT/mgsubacct"
  mkdir -p "$MGA/BP"
  printf '# MVX account descriptor\nname = mgsubacct\nversion = 1\n' > "$MGA/.mvx"
  printf 'PRINT "hi"\n' > "$MGA/BP/HELLO"
  ( cd "$MGA" && git init -q -b main && \
    git config user.email t@t && git config user.name t && \
    git config protocol.file.allow always && \
    MVX="$TCL" "$ROOT/build/bin/mvx-git" add -A >/dev/null && \
    git commit -qm acct && \
    MVX="$TCL" "$ROOT/build/bin/mvx-git" -c protocol.file.allow=always \
        submodule add "$MGSUB" docs >/dev/null 2>&1 && \
    MVX="$TCL" "$ROOT/build/bin/mvx-git" add -A >/dev/null && \
    MVX="$TCL" "$ROOT/build/bin/mvx-git" commit "add submodule" >/dev/null ) >/dev/null 2>&1
  check tcl-mvxgit-submodule "$( cd "$MGA"; \
    { git ls-tree HEAD docs | grep -q '^160000 commit' \
        && echo 'docs is a gitlink' || echo 'docs is NOT a gitlink'; }; \
    { git ls-files .gitmodules | grep -q . \
        && echo '.gitmodules tracked' || echo '.gitmodules missing'; }; \
    { git ls-tree HEAD BP/HELLO | grep -q blob \
        && echo 'records intact' || echo 'records lost'; }; \
    MVX="$TCL" "$ROOT/build/bin/mvx-git" status 2>&1 )"

  # #58: inside an MVX account mvx-git drives the engine directly on the
  # account's own .git — init/add/commit/log/show read and write the live
  # hash-file records straight to/from git objects, no export copy.  The .git is
  # an ordinary repository, so plain git reads the history (records tracked at
  # <file>/<id>).  Same engine as the GIT verb (tcl-gitnative), via the binary.
  MGR="$TESTROOT/mgrecgit"
  "$ROOT/scripts/mkaccount.sh" "$MGR" >/dev/null
  cat > "$TESTROOT/mgrseed.b" <<'RGEOF'
OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"London" ON F, "C1"
WRITE "Bob":@AM:"Paris" ON F, "C2"
RGEOF
  "$TCL" -a "$MGR" -c 'CREATE-FILE CUST' >/dev/null 2>&1
  "$MVX" "$TESTROOT/mgrseed.b" -o "$TESTROOT/mgrseedbin" 2>/dev/null
  (cd "$MGR" && MVXACCOUNT=. "$TESTROOT/mgrseedbin") >/dev/null
  check tcl-mvxgit-recgit "$( \
    ( cd "$MGR"; "$ROOT/build/bin/mvx-git" init; \
      "$ROOT/build/bin/mvx-git" add CUST; \
      "$ROOT/build/bin/mvx-git" commit -m initial; \
      "$ROOT/build/bin/mvx-git" log ) 2>&1 \
        | sed -E -e 's/[0-9a-f]{7,40}/HASH/g' \
                 -e 's/^Author:.*/Author: A/' -e 's/^Date:.*/Date: D/'; \
    { [ -d "$MGR/.git" ] && echo 'account .git present'; }; \
    echo "plain-git tracked: $(cd "$MGR" && git ls-tree -r --name-only HEAD | tr '\n' ' ')"; \
    ( cd "$MGR"; "$ROOT/build/bin/mvx-git" show CUST C1 ))"

  # #58: an account that is a subdirectory of a larger repo (it has a .mvx but
  # no .git of its own) is tracked by that repo — mvx-git forwards to it and
  # never creates a nested .git; the account is rebuilt with mvx-convert-acct
  # (or an mvx-git clone) instead.
  MGSUB="$TESTROOT/mgsub"
  mkdir -p "$MGSUB/acct/BP"
  printf '# MVX account descriptor\nname = acct\nversion = 1\n' > "$MGSUB/acct/.mvx"
  printf 'PRINT "hi"\n' > "$MGSUB/acct/BP/HELLO"
  ( cd "$MGSUB" && git init -q -b main && git add -A && \
    git -c user.email=t@t -c user.name=t commit -qm main ) >/dev/null 2>&1
  ( cd "$MGSUB/acct" && "$ROOT/build/bin/mvx-git" status >/dev/null 2>&1 )
  check tcl-mvxgit-subdir "$( \
    { [ -d "$MGSUB/acct/.git" ] && echo 'nested .git created (WRONG)' \
        || echo 'no nested .git (forwarded to main repo)'; }; \
    ( cd "$MGSUB/acct" && "$ROOT/build/bin/mvx-git" log --oneline 2>&1 \
        | sed -E 's/[0-9a-f]{7,40}/HASH/g' ))"

  # open account format, commit side (mvx#73): with mvx.openaccount set, `add`
  # normalises the staged git objects to the open form - %FILE% becomes DIR/hash,
  # .mvx is stored at .mv-account, and the binary lmdb store is never tracked -
  # while the working tree on disk stays a native MVX account.
  MGF="$TESTROOT/mgopenform"
  "$ROOT/scripts/mkaccount.sh" "$MGF" >/dev/null
  "$TCL" -a "$MGF" -c 'CREATE-FILE PARTS DIR' >/dev/null 2>&1
  ( cd "$MGF" && "$ROOT/build/bin/mvx-git" init >/dev/null 2>&1 && \
    git config user.email t@t && git config user.name t && \
    git config mvx.openaccount true && \
    "$ROOT/build/bin/mvx-git" add -A >/dev/null 2>&1 && \
    "$ROOT/build/bin/mvx-git" commit -m init >/dev/null 2>&1 )
  check tcl-mvxgit-openform "$( cd "$MGF"; \
    echo "git %FILE% = $(git cat-file -p 'HEAD:PARTS.DICT/%FILE%')"; \
    { git cat-file -e 'HEAD:.mv-account' 2>/dev/null && echo 'git: .mv-account'; }; \
    { git cat-file -e 'HEAD:.mvx' 2>/dev/null && echo 'git: .mvx (WRONG)' \
        || echo 'git: no .mvx'; }; \
    { git cat-file -e 'HEAD:mvxdata.lmdb/data.mdb' 2>/dev/null \
        && echo 'git: lmdb store (WRONG)' || echo 'git: no lmdb store'; }; \
    { grep -q 'FILE' 'PARTS.DICT/%FILE%' && echo 'disk %FILE%: native'; }; \
    { [ -f .mvx ] && echo 'disk descriptor: .mvx'; })"

  # lmdb-file dictionaries: their records live in LMDB (no on-disk .DICT dir for
  # git's own add to catch), so `add -A` stages them explicitly at
  # <name>.DICT/<id> - the dictionary travels in git for a full round-trip.
  MGD2="$TESTROOT/mgdict"
  "$ROOT/scripts/mkaccount.sh" "$MGD2" >/dev/null
  cat > "$TESTROOT/mgd2seed.b" <<'EOF'
X = CREATEFILE("ORDERS")
OPEN "ORDERS" TO F ELSE STOP
WRITE "Ada":@AM:"London" ON F, "O1"
OPEN "DICT", "ORDERS" TO D ELSE STOP
WRITE "D":@AM:"2":@AM:"":@AM:"Who":@AM:"10L" ON D, "WHO"
PRINT "seeded"
EOF
  "$MVX" "$TESTROOT/mgd2seed.b" -o "$TESTROOT/mgd2bin" 2>/dev/null
  (cd "$MGD2" && MVXACCOUNT=. "$TESTROOT/mgd2bin") >/dev/null
  ( cd "$MGD2" && "$ROOT/build/bin/mvx-git" init >/dev/null 2>&1 && \
    git config user.email t@t && git config user.name t && \
    "$ROOT/build/bin/mvx-git" add -A >/dev/null 2>&1 )
  check tcl-mvxgit-dict "$( cd "$MGD2"; \
    { git ls-files ORDERS/O1 | grep -q . && echo 'data: ORDERS/O1'; }; \
    { git ls-files 'ORDERS.DICT/WHO' | grep -q . && echo 'dict: ORDERS.DICT/WHO'; }; \
    { git ls-files 'ORDERS.DICT/%FILE%' | grep -q . && echo 'dict: ORDERS.DICT/%FILE%'; })"

  # direct clone materialisation (mv_git#4): `mvx-git clone` of an open-format
  # account builds a native MVX account straight from the git objects into the
  # backend - the open form never lands on disk (no `<file>/` record dirs, no
  # .mv-account, %FILE% is native), no adopt tool runs - with a DIR file, a hash
  # file, and their dictionaries all round-tripping.
  MGCO="$TESTROOT/mgcloneopen"
  "$ROOT/scripts/mkaccount.sh" "$MGCO" >/dev/null
  "$TCL" -a "$MGCO" -c 'CREATE-FILE PARTS DIR' >/dev/null 2>&1
  cat > "$TESTROOT/mgcoseed.b" <<'EOF'
OPEN "PARTS" TO F ELSE STOP
WRITE "Widget":@AM:"9" ON F, "W1"
OPEN "DICT", "PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
X = CREATEFILE("ORDERS")
OPEN "ORDERS" TO G ELSE STOP
WRITE "Ada" ON G, "O1"
PRINT "seeded"
EOF
  "$MVX" "$TESTROOT/mgcoseed.b" -o "$TESTROOT/mgcobin" 2>/dev/null
  (cd "$MGCO" && MVXACCOUNT=. "$TESTROOT/mgcobin") >/dev/null
  ( cd "$MGCO" && "$ROOT/build/bin/mvx-git" init >/dev/null 2>&1 && \
    git config user.email t@t && git config user.name t && \
    git config mvx.openaccount true && \
    "$ROOT/build/bin/mvx-git" add -A >/dev/null 2>&1 && \
    "$ROOT/build/bin/mvx-git" commit -m init >/dev/null 2>&1 )
  MGCOC="$TESTROOT/mgcloneopen-clone"
  MVX="$TCL" "$ROOT/build/bin/mvx-git" clone "$MGCO" "$MGCOC" </dev/null >/dev/null 2>&1
  check tcl-mvxgit-clone "$( cd "$MGCOC"; \
    { [ -f .mvx ] && echo 'descriptor: .mvx'; }; \
    { [ ! -e .mv-account ] && echo 'no .mv-account on disk'; }; \
    { [ ! -d ORDERS ] && [ -d mvxdata.lmdb ] && echo 'records in backend, not on disk'; }; \
    { grep -q 'FILE' 'PARTS.DICT/%FILE%' && echo 'disk %FILE%: native'; }; \
    "$TCL" -a . -c 'LISTF' 2>&1 | normalise | grep -E '^PARTS |^ORDERS '; \
    "$TCL" -a . -c 'LIST PARTS NAME' 2>&1 | normalise | grep -E 'W1|Widget' | head -1 )"

  # a materialised open account reads back clean: status translates the native
  # disk form up to the open form, so the committed %FILE% controls, the
  # .mv-account descriptor, and every record (incl. dir-backed dictionary items,
  # which must not accumulate a trailing terminator) round-trip with no reported
  # modification or deletion.  Provisioning artefacts BUILD creates (e.g. the
  # VOC/CATALOG pointer) may show untracked (??); only ` M `/` D ` lines fault.
  check tcl-mvxgit-clone-clean "$( cd "$MGCOC"; \
    st="$( MVX="$TCL" "$ROOT/build/bin/mvx-git" status 2>&1 )"; \
    echo "$st" | grep -E '^ (M|D) ' && echo 'DIRTY' || echo 'clean: no modified/deleted' )"

  # remotes (mv_git#14): push an open account to a bare remote, clone it, then a
  # fast-forward pull and a divergent-merge pull that RE-MATERIALISE records into
  # the clone's backend.  Plain `git pull` can't: the clone's native working tree
  # reads as dirty, so pull is engine-driven (git fetch + tree merge of FETCH_HEAD).
  RGIT="$ROOT/build/bin/mvx-git"
  RA="$TESTROOT/remA"; RREM="$TESTROOT/rem.git"; RB="$TESTROOT/remB"
  rgseed() { printf 'OPEN "CUST" TO F ELSE STOP\n%s\n' "$1" > "$TESTROOT/rg.b"; \
             "$MVX" "$TESTROOT/rg.b" -o "$TESTROOT/rg.bin" 2>/dev/null; \
             ( cd "$2" && MVXACCOUNT=. "$TESTROOT/rg.bin" ) >/dev/null; }
  "$ROOT/scripts/mkaccount.sh" "$RA" >/dev/null
  "$TCL" -a "$RA" -c 'CREATE-FILE CUST' >/dev/null 2>&1
  rgseed 'WRITE "Ada":@AM:"London" ON F, "C1"' "$RA"
  ( cd "$RA" && "$RGIT" init >/dev/null 2>&1 && git config user.name t && \
    git config user.email t@t && git config mvx.openaccount true && \
    "$RGIT" add -A >/dev/null 2>&1 && "$RGIT" commit -m base >/dev/null 2>&1 )
  git init --bare -q "$RREM"
  # The bare remote's HEAD must name the branch we push (main); otherwise it follows the
  # runner's init.defaultBranch (unset -> master) while the content is on main, so the clone
  # lands on an unborn branch and never materialises the account (#98).
  git -C "$RREM" symbolic-ref HEAD refs/heads/main
  ( cd "$RA" && git remote add origin "$RREM" && "$RGIT" push -u origin main >/dev/null 2>&1 )
  MVX="$TCL" "$RGIT" clone "$RREM" "$RB" </dev/null >/dev/null 2>&1
  ( cd "$RB" && git config user.name t; git config user.email t@t )
  rgseed 'WRITE "Bob":@AM:"Paris" ON F, "C2"' "$RA"
  ( cd "$RA" && "$RGIT" add -A >/dev/null 2>&1 && "$RGIT" commit -m c2 >/dev/null 2>&1 && \
    "$RGIT" push origin main >/dev/null 2>&1 )
  ff="$( cd "$RB" && "$RGIT" pull origin main 2>/dev/null | grep -o 'fast-forward' )"
  rgseed 'WRITE "Cy":@AM:"Rome" ON F, "C3"' "$RB"
  ( cd "$RB" && "$RGIT" add -A >/dev/null 2>&1 && "$RGIT" commit -m c3 >/dev/null 2>&1 )
  rgseed 'WRITE "Dee":@AM:"Oslo" ON F, "C4"' "$RA"
  ( cd "$RA" && "$RGIT" add -A >/dev/null 2>&1 && "$RGIT" commit -m c4 >/dev/null 2>&1 && \
    "$RGIT" push origin main >/dev/null 2>&1 )
  mg="$( cd "$RB" && "$RGIT" pull origin main 2>/dev/null | grep -o 'Merge' )"
  check tcl-mvxgit-remote "$( \
    echo "clone: $([ -f "$RB/.mvx" ] && echo materialised)"; \
    echo "pull-ff: $ff"; \
    echo "C2 after ff: $("$TCL" -a "$RB" -c 'CT CUST C2' 2>/dev/null | grep -c Paris)"; \
    echo "pull-merge: $mg"; \
    echo "C3 local kept: $("$TCL" -a "$RB" -c 'CT CUST C3' 2>/dev/null | grep -c Rome)"; \
    echo "C4 merged in: $("$TCL" -a "$RB" -c 'CT CUST C4' 2>/dev/null | grep -c Oslo)" )"

  # per-record .gitignore in open mode (mv_git#17): add AND status must honor
  # .gitignore at record granularity — a gitignored record (CUST/C9, an ordinary
  # lmdb record git's plain add never enumerates) is neither staged nor shown as
  # untracked, so derived artifacts (LIB/, VOC/LIB, …) stay out of the open form.
  IGA="$TESTROOT/ig17"
  "$ROOT/scripts/mkaccount.sh" "$IGA" >/dev/null
  "$TCL" -a "$IGA" -c 'CREATE-FILE CUST' >/dev/null 2>&1
  printf 'OPEN "CUST" TO F ELSE STOP\nWRITE "Ada":@AM:"London" ON F, "C1"\nWRITE "derived" ON F, "C9"\n' \
    > "$TESTROOT/ig17seed.b"
  "$MVX" "$TESTROOT/ig17seed.b" -o "$TESTROOT/ig17bin" 2>/dev/null
  (cd "$IGA" && MVXACCOUNT=. "$TESTROOT/ig17bin") >/dev/null
  printf 'CUST/C9\n' > "$IGA/.gitignore"
  ( cd "$IGA" && "$RGIT" init >/dev/null 2>&1 && git config user.name t && \
    git config user.email t@t && git config mvx.openaccount true )
  check tcl-mvxgit-ignore "$( cd "$IGA"; \
    echo "status hides C9: $("$RGIT" status 2>/dev/null | grep -c 'CUST/C9')"; \
    "$RGIT" add -A >/dev/null 2>&1; \
    echo "C1 staged: $(git ls-files --cached | grep -c 'CUST/C1')"; \
    echo "C9 staged: $(git ls-files --cached | grep -c 'CUST/C9')" )"

  # GIT verb owns the whole open-account workflow with NO CLI (mv_git#20 — the D3
  # constraint): INIT / CONFIG / ADD -A / COMMIT all via verbs.  GIT ADD -A stages
  # lmdb records (add_all lifted into the engine) and the verb honours
  # mvx.openaccount (openaccount_sync), so the commit is the portable OPEN form
  # (.mv-account, DIR/hash %FILE%) — not native .mvx.
  VBA="$TESTROOT/vbaddall"
  "$ROOT/scripts/mkaccount.sh" "$VBA" >/dev/null
  "$TCL" -a "$VBA" -c 'CREATE-FILE CUST' >/dev/null 2>&1
  printf 'OPEN "CUST" TO F ELSE STOP\nWRITE "Ada":@AM:"London" ON F, "C1"\n' > "$TESTROOT/vba.b"
  "$MVX" "$TESTROOT/vba.b" -o "$TESTROOT/vba.bin" 2>/dev/null
  (cd "$VBA" && MVXACCOUNT=. "$TESTROOT/vba.bin") >/dev/null
  "$TCL" -a "$VBA" -c "LINK-PKG $ROOT/packages/git" >/dev/null 2>&1
  "$TCL" -a "$VBA" -c 'GIT INIT' >/dev/null 2>&1
  "$TCL" -a "$VBA" -c 'GIT CONFIG user.name t' >/dev/null 2>&1
  "$TCL" -a "$VBA" -c 'GIT CONFIG user.email t@t' >/dev/null 2>&1
  "$TCL" -a "$VBA" -c 'GIT CONFIG mvx.openaccount true' >/dev/null 2>&1
  addout="$("$TCL" -a "$VBA" -c 'GIT ADD -A' 2>&1 | tail -1)"
  "$TCL" -a "$VBA" -c 'GIT COMMIT -m base' >/dev/null 2>&1
  check tcl-mvxgit-verb-addall "$( cd "$VBA"; \
    echo "add -A ran: $(echo "$addout" | grep -c staged)"; \
    echo "lmdb record committed: $(git ls-tree -r HEAD --name-only 2>/dev/null | grep -c '^CUST/C1$')"; \
    echo "open descriptor: $(git ls-tree -r HEAD --name-only 2>/dev/null | grep -c '^\.mv-account$')"; \
    echo "native .mvx absent: $(git ls-tree -r HEAD --name-only 2>/dev/null | grep -c '^\.mvx$')" )"

  # GIT TAG (mv_git#20): list / lightweight / annotated / delete — all engine, no
  # shell git (releases resolve via tags).  Reuses the VBA account (has a commit).
  "$TCL" -a "$VBA" -c 'GIT TAG v1.0' >/dev/null 2>&1
  "$TCL" -a "$VBA" -c 'GIT TAG -a v2.0 -m release-two' >/dev/null 2>&1
  check tcl-mvxgit-tag "$( cd "$VBA"; \
    echo "list: $("$TCL" -a . -c 'GIT TAG' 2>/dev/null | tr '\n' ',')"; \
    echo "annotated message: $(git tag -n1 v2.0 2>/dev/null | grep -c release-two)"; \
    "$TCL" -a . -c 'GIT TAG -d v1.0' >/dev/null 2>&1; \
    echo "after delete v1.0: $("$TCL" -a . -c 'GIT TAG' 2>/dev/null | tr '\n' ',')" )"
else
  echo "  (skipping tcl-mvxgit: git CLI or build/bin/mvx-git unavailable)"
fi

# native git (libgit2, no shell, restricted tier): init, export, add,
# commit, log - the injection-proof structured path
GACCT="$TESTROOT/gitacct"
mkdir -p "$GACCT/CATALOG"
"$TCL" -a "$GACCT" -c "CREATE-FILE CUST" >/dev/null 2>&1
gseed="$TESTROOT/gseed.b"
cat > "$gseed" <<'GSEOF'
OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"London" ON F, "C1"
WRITE "Bob":@AM:"Paris" ON F, "C2"
GSEOF
"$MVX" "$gseed" -o "$TESTROOT/gseedbin" 2>/dev/null
(cd "$GACCT" && MVXACCOUNT=. "$TESTROOT/gseedbin")
check tcl-gitnative "$( \
  printf "LINK-PKG $ROOT/packages/git\nGIT INIT\nGIT ADD CUST\nGIT STATUS\nGIT COMMIT -m initial\nGIT LOG\n" | \
    "$TCL" -a "$GACCT" 2>&1 \
      | sed -E -e 's/[0-9a-f]{7,40}/HASH/g' \
               -e 's/^Author:.*/Author: A/' -e 's/^Date:.*/Date: D/' | normalise; \
  printf 'DELETE CUST C1\nWRITE-C2\n' > /dev/null; \
  (cd "$GACCT" && MVXACCOUNT=. "$MVX" /dev/stdin -o "$TESTROOT/gmod" <<'GMEOF' >/dev/null 2>&1
OPEN "CUST" TO F ELSE STOP
WRITE "Bob":@AM:"Berlin" ON F, "C2"
GMEOF
   cd "$GACCT" && MVXACCOUNT=. "$TESTROOT/gmod"); \
  printf 'GIT STATUS\nGIT DIFF CUST\nGIT RESTORE CUST\nGIT STATUS\nCT CUST C2\n' | "$TCL" -a "$GACCT" 2>&1)"

# BUILD: provision an account from git-tracked config after a clone.
# A dictionary directory + BP source with no data file -> BUILD makes
# the file, imports the schema, catalogs the source.
VENDOR="$TESTROOT/vendor"
mkdir -p "$VENDOR/CATALOG" "$VENDOR/BP"
"$TCL" -a "$VENDOR" -c "CREATE-FILE PARTS" >/dev/null 2>&1
bseed="$TESTROOT/bseed.b"
cat > "$bseed" <<'BSEOF'
OPEN "DICT","PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"20L" ON D, "NAME"
BSEOF
"$MVX" "$bseed" -o "$TESTROOT/bseedbin" 2>/dev/null
(cd "$VENDOR" && MVXACCOUNT=. "$TESTROOT/bseedbin")
cat > "$VENDOR/BP/RPT" <<'RPTEOF'
/**
 * @file RPT
 */
PRINT "report verb works"
RPTEOF
MVXPRIV=developer "$TCL" -a "$VENDOR" -c "EXPORT DICT PARTS" >/dev/null 2>&1
printf 'PARTS lmdb\n' > "$VENDOR/FILES"
# "clone": copy only the tracked config, not the data
CLONE="$TESTROOT/clone"
mkdir -p "$CLONE"
cp -r "$VENDOR/PARTS.DICT" "$VENDOR/BP" "$VENDOR/FILES" "$CLONE/"
check tcl-build "$( \
  MVXPRIV=developer "$TCL" -a "$CLONE" -c "BUILD" 2>&1 | normalise; \
  printf 'LISTF\nLIST DICT PARTS\nRPT\n' | "$TCL" -a "$CLONE" 2>&1 | normalise)"

# delivery: stock -> branch(site) -> customise -> cherry-pick upstream
# -> merge stock down.  The Pick multi-site delivery workflow in git.
DACCT="$TESTROOT/delacct"
mkdir -p "$DACCT/CATALOG"
"$TCL" -a "$DACCT" -c "CREATE-FILE MENU" >/dev/null 2>&1
dseed2="$TESTROOT/dseed2.b"
cat > "$dseed2" <<'DEOF'
OPEN "MENU" TO F ELSE STOP
WRITE "Sales" ON F, "M1"
DEOF
"$MVX" "$dseed2" -o "$TESTROOT/dseed2bin" 2>/dev/null
(cd "$DACCT" && MVXACCOUNT=. "$TESTROOT/dseed2bin")
custom="$TESTROOT/dcustom.b"
cat > "$custom" <<'DCEOF'
OPEN "MENU" TO F ELSE STOP
WRITE "ACME Dashboard" ON F, "M9"
DCEOF
"$MVX" "$custom" -o "$TESTROOT/dcustombin" 2>/dev/null
check tcl-delivery "$( \
  export MVXACCOUNT="$DACCT"; \
  { printf "LINK-PKG $ROOT/packages/git\nGIT INIT\nGIT ADD MENU\nGIT COMMIT -m stock\nGIT BRANCH site\nGIT CHECKOUT site\n" | "$TCL" -a "$DACCT" 2>&1; \
    (cd "$DACCT" && "$TESTROOT/dcustombin"); \
    printf 'GIT ADD MENU\nGIT COMMIT -m acme-custom\nGIT CHECKOUT main\nGIT CHERRY-PICK site\nCT MENU M9\nGIT BRANCH\n' | "$TCL" -a "$DACCT" 2>&1; \
  } | sed -E 's/\[[0-9a-f]{7,40}\]/[HASH]/g; s/^[0-9a-f]{7,40} /HASH /g' | normalise; \
  unset MVXACCOUNT)"

# GITIGNORE: bulk data excluded, dictionary tracked
IGACCT="$TESTROOT/igacct"
mkdir -p "$IGACCT/CATALOG"
"$TCL" -a "$IGACCT" -c "CREATE-FILE ORDERS" >/dev/null 2>&1
igseed="$TESTROOT/igseed.b"
cat > "$igseed" <<'IGEOF'
OPEN "ORDERS" TO F ELSE STOP
FOR I = 1 TO 5
   WRITE "order-":I ON F, "O":I
NEXT I
OPEN "DICT", "ORDERS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"20L" ON D, "CUST"
IGEOF
"$MVX" "$igseed" -o "$TESTROOT/igseedbin" 2>/dev/null
(cd "$IGACCT" && MVXACCOUNT=. "$TESTROOT/igseedbin")
check tcl-gitignore "$(printf '%s\n' \
  "LINK-PKG $ROOT/packages/git" \
  'GIT INIT' \
  'GIT IGNORE ORDERS' \
  'GIT ADD ORDERS' \
  'GIT ADD DICT ORDERS' \
  'GIT STATUS' | "$TCL" -a "$IGACCT" 2>&1 | normalise)"

# PORT-SOURCE: C-style comments to classic, output must compile
cat > "$ACCT/BP/CPORT" <<'EOF'
/**
 * @file CPORT
 */
// setup
A = 5 /* five */ + 1
B = "keep /* this */"   // trailing
/* block
   spans lines */
PRINT A:" ":B
EOF
check tcl-port "$(printf 'PORT-SOURCE BP CPORT\nCT BP CPORT.PORTED\n' | tclrun; \
  printf 'BASIC BP CPORT.PORTED\n' | MVXPRIV=developer "$TCL" -a "$ACCT" 2>&1)"

# secondary indexes: build, query through them, write-path maintenance
check tcl-index "$(printf '%s\n' \
  'CREATE-INDEX PARTS COLOR' \
  'LIST-INDEXES PARTS' \
  'LIST PARTS NAME WITH COLOR = blue' \
  'COPY PARTS W100 TO W950' \
  'LIST PARTS NAME WITH COLOR = blue' \
  'DELETE PARTS W950' \
  'LIST PARTS NAME WITH COLOR = blue' \
  'ED PARTS G200' \
  '3' \
  'R/red/blue' \
  'FI' \
  'LIST PARTS NAME WITH COLOR = blue' \
  'ED PARTS G200' \
  '3' \
  'R/blue/red' \
  'FI' \
  'LIST PARTS NAME WITH COLOR = red' \
  'CREATE-INDEX PARTS NAME' \
  'DELETE-INDEX PARTS COLOR' \
  'LIST-INDEXES PARTS' \
  'LIST PARTS NAME WITH COLOR = blue' \
  'DELETE-INDEX PARTS NAME' | tclrun)"

# EXPORT/IMPORT: a hash file round-trips through a git-native
# directory file; an external edit and a delete both mirror back
exacct_prog="$TESTROOT/exseed.b"
cat > "$exacct_prog" <<'EXEOF'
OPEN "CUST" TO F ELSE STOP
WRITE "Ada":@AM:"London" ON F, "C1"
WRITE "Bob":@AM:"Paris" ON F, "C2"
WRITE "Cy":@AM:"Berlin" ON F, "C3"
EXEOF
"$MVX" "$exacct_prog" -o "$TESTROOT/exseed" 2>/dev/null
check tcl-export "$( \
  printf 'CREATE-FILE CUST\n' | tclrun; \
  (cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/exseed"); \
  printf 'EXPORT CUST\n' | tclrun; \
  printf 'Ada\nLONDON\n' > "$ACCT/CUST.EXP/C1"; \
  rm -f "$ACCT/CUST.EXP/C2"; \
  printf 'IMPORT CUST\nLIST CUST\nCT CUST C1\n' | tclrun)"

# VI: export a record to a file, "edit" it with a scripted editor,
# import it back — the hash-file <-> text-file round trip
fakeed="$TESTROOT/fakeed.sh"
cat > "$fakeed" <<'FEEOF'
#!/bin/sh
awk 'NR==2{print toupper($0);next}{print}' "$1" > "$1.t" && mv "$1.t" "$1"
FEEOF
chmod +x "$fakeed"
check tcl-vi "$( \
  printf 'CREATE-FILE NOTES\n' | tclrun; \
  (cd "$ACCT" && MVXACCOUNT=. "$MVX" /dev/stdin -o "$TESTROOT/ns" <<'NSEOF' >/dev/null 2>&1
OPEN "NOTES" TO F ELSE STOP
WRITE "line one":@AM:"line two":@AM:"line three" ON F, "N1"
NSEOF
   cd "$ACCT" && MVXACCOUNT=. "$TESTROOT/ns"); \
  printf 'VI NOTES N1\n' | tclrun; \
  MVXPRIV=unrestricted MVXEDITOR="$fakeed" "$TCL" -a "$ACCT" -c "VI NOTES N1" 2>&1; \
  printf 'CT NOTES N1\n' | tclrun; \
  MVXPRIV=unrestricted MVXEDITOR=true "$TCL" -a "$ACCT" -c "VI NOTES N1" 2>&1)"

# select lists crossing EXECUTE into a program
prog="$TESTROOT/progsel.b"
cat > "$prog" <<'EOF'
EXECUTE "SELECT PARTS WITH COLOR = blue" CAPTURING X
DONE = 0
LOOP
   READNEXT ID ELSE DONE = 1
UNTIL DONE DO
   PRINT "got ":ID
REPEAT
EOF
"$MVX" "$prog" -o "$TESTROOT/progsel" 2>/dev/null
check tcl-session "$(cd "$ACCT" && \
  MVXACCOUNT=. MVXSESSION="$TESTROOT/sess" "$TESTROOT/progsel" 2>&1)"

# the networked daemon: same account flow through mvx-lmdbd, plus the lock
# lease (holder dies without releasing; next session proceeds)
DSOCK="/tmp/mvx-lmdbd-test-$$.sock"
DACCT="$TESTROOT/dacct"
mkdir -p "$DACCT"
"$ROOT/build/bin/mvx-lmdbd" -d "$TESTROOT/ddata" -s "$DSOCK" 2>/dev/null &
DPID=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  [ -S "$DSOCK" ] && break
  sleep 0.1
done
dlock="$TESTROOT/dlock.b"
cat > "$dlock" <<'EOF'
OPEN "PARTS" TO F ELSE STOP
READU R FROM F, "W100" THEN PRINT "locked, exiting without RELEASE"
EOF
"$MVX" "$dlock" -o "$TESTROOT/dlockbin" 2>/dev/null
check tcl-daemon "$( \
  export MVXDAEMON="$DSOCK"; \
  "$TCL" -a "$DACCT" -c "CREATE-FILE VOC" >/dev/null 2>&1; \
  printf 'CREATE-FILE PARTS\n' | "$TCL" -a "$DACCT" 2>&1; \
  (cd "$DACCT" && MVXACCOUNT=. MVXDAEMON="$DSOCK" "$TESTROOT/seedbin"); \
  printf 'LIST PARTS NAME COLOR\nCREATE-INDEX PARTS COLOR\nLIST PARTS NAME WITH COLOR = blue\nLISTF\n' | \
    "$TCL" -a "$DACCT" 2>&1; \
  (cd "$DACCT" && MVXACCOUNT=. MVXDAEMON="$DSOCK" "$TESTROOT/dlockbin"); \
  (cd "$DACCT" && MVXACCOUNT=. MVXDAEMON="$DSOCK" "$TESTROOT/dlockbin"); \
  unset MVXDAEMON)"
# mixed local/remote: per-file REMOTE binding with an explicit daemon
# address and NO $MVXDAEMON - locals stay local, the bound file goes
# through the daemon, one program reads both
MACCT="$TESTROOT/mixacct"
mkdir -p "$MACCT"
"$TCL" -a "$MACCT" -c "CREATE-FILE VOC" >/dev/null 2>&1
mixprog="$TESTROOT/mix.b"
cat > "$mixprog" <<'MIXEOF'
OPEN "LOCALF" TO L ELSE STOP
WRITE "local data" ON L, "L1"
OPEN "SHARED" TO R ELSE STOP
WRITE "remote data" ON R, "R1"
READ A FROM L, "L1" THEN PRINT "local read: ":A
READ B FROM R, "R1" THEN PRINT "remote read: ":B
MIXEOF
"$MVX" "$mixprog" -o "$TESTROOT/mixbin" 2>/dev/null
check tcl-mixed "$( \
  printf "CREATE-FILE LOCALF\nCREATE-FILE SHARED USING lmdbnet $DSOCK\nLISTF\n" | \
    "$TCL" -a "$MACCT" 2>&1 | sed "s#$DSOCK#@DSOCK@#g"; \
  (cd "$MACCT" && MVXACCOUNT=. "$TESTROOT/mixbin"); \
  printf 'LISTF\n' | "$TCL" -a "$MACCT" 2>&1)"

# namespace isolation: two accounts (nsa, nsb) with different names share
# ONE daemon; the same file name ORDERS holds different data in each and
# is mutually invisible.  A third account (nsc) names nsa's namespace
# explicitly in BINDINGS and reads nsa's data — the Q-pointer analog.
NSA="$TESTROOT/nsa"; NSB="$TESTROOT/nsb"; NSC="$TESTROOT/nsc"
mkdir -p "$NSA" "$NSB" "$NSC"
printf 'OPEN "ORDERS" TO F ELSE STOP\nWRITE "from A" ON F, "O1"\n' > "$TESTROOT/wa.b"
printf 'OPEN "ORDERS" TO F ELSE STOP\nWRITE "from B" ON F, "O1"\n' > "$TESTROOT/wb.b"
printf 'OPEN "ORDERS" TO F ELSE STOP\nREAD V FROM F, "O1" THEN PRINT V ELSE PRINT "(none)"\n' > "$TESTROOT/rd.b"
"$MVX" "$TESTROOT/wa.b" -o "$TESTROOT/wa" 2>/dev/null
"$MVX" "$TESTROOT/wb.b" -o "$TESTROOT/wb" 2>/dev/null
"$MVX" "$TESTROOT/rd.b" -o "$TESTROOT/rd" 2>/dev/null
check tcl-namespace "$( \
  export MVXDAEMON="$DSOCK"; \
  "$TCL" -a "$NSA" -c "CREATE-FILE VOC" >/dev/null 2>&1; \
  "$TCL" -a "$NSA" -c "CREATE-FILE ORDERS" >/dev/null 2>&1; \
  "$TCL" -a "$NSB" -c "CREATE-FILE VOC" >/dev/null 2>&1; \
  "$TCL" -a "$NSB" -c "CREATE-FILE ORDERS" >/dev/null 2>&1; \
  (cd "$NSA" && MVXACCOUNT=. "$TESTROOT/wa"); \
  (cd "$NSB" && MVXACCOUNT=. "$TESTROOT/wb"); \
  printf 'A reads: '; (cd "$NSA" && MVXACCOUNT=. "$TESTROOT/rd"); \
  printf 'B reads: '; (cd "$NSB" && MVXACCOUNT=. "$TESTROOT/rd"); \
  unset MVXDAEMON; \
  printf 'ORDERS lmdbnet %s nsa\n' "$DSOCK" > "$NSC/BINDINGS"; \
  printf 'C via nsa: '; (cd "$NSC" && MVXACCOUNT=. "$TESTROOT/rd") )"

kill $DPID 2>/dev/null
rm -f "$DSOCK"

# daemon authentication: mvx-lmdbd-admin provisions a namespace token
# (offline, into <datadir>/accounts); a client with the token in
# .mvx-private reads/writes, a client with the wrong token is denied.
ADATA="$TESTROOT/adata"; ASOCK="/tmp/mvx-auth-test-$$.sock"
AACCT="$TESTROOT/aacct"; BACCT="$TESTROOT/bacct"
mkdir -p "$AACCT/.mvx-private" "$BACCT/.mvx-private"
chmod 700 "$AACCT/.mvx-private" "$BACCT/.mvx-private"
ATOK="$("$ROOT/build/bin/mvx-lmdbd-admin" -d "$ADATA" create-account acct1 2>/dev/null)"
"$ROOT/build/bin/mvx-lmdbd" -d "$ADATA" -s "$ASOCK" 2>/dev/null &
APID=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  [ -S "$ASOCK" ] && break
  sleep 0.1
done
printf 'lmdbnet %s acct1 token=%s\n' "$ASOCK" "$ATOK" > "$AACCT/.mvx-private/credentials"
printf 'ORDERS lmdbnet %s acct1\n' "$ASOCK" > "$AACCT/BINDINGS"
printf 'lmdbnet %s acct1 token=deadbeefwrong\n' "$ASOCK" > "$BACCT/.mvx-private/credentials"
printf 'ORDERS lmdbnet %s acct1\n' "$ASOCK" > "$BACCT/BINDINGS"
chmod 600 "$AACCT/.mvx-private/credentials" "$BACCT/.mvx-private/credentials"
printf 'OPEN "ORDERS" TO F ELSE STOP\nWRITE "hi" ON F, "O1"\nREAD V FROM F, "O1" THEN PRINT "read: ":V\n' > "$TESTROOT/wauth.b"
printf 'OPEN "ORDERS" TO F ELSE PRINT "denied"\n' > "$TESTROOT/rauth.b"
"$MVX" "$TESTROOT/wauth.b" -o "$TESTROOT/wauth" 2>/dev/null
"$MVX" "$TESTROOT/rauth.b" -o "$TESTROOT/rauth" 2>/dev/null
check tcl-auth "$( \
  "$TCL" -a "$AACCT" -c "CREATE-FILE ORDERS" 2>&1 | sed "s#$ASOCK#@SOCK@#g"; \
  (cd "$AACCT" && MVXACCOUNT=. "$TESTROOT/wauth"); \
  printf 'wrong token: '; (cd "$BACCT" && MVXACCOUNT=. "$TESTROOT/rauth" 2>/dev/null); \
  "$ROOT/build/bin/mvx-lmdbd-admin" -d "$ADATA" list-accounts)"

# named connection profiles: BINDINGS references @conn1, and the host +
# namespace + token live in the local connection profile — so the
# committed binding never names the daemon.
CTOK="$("$ROOT/build/bin/mvx-lmdbd-admin" -d "$ADATA" create-account connns 2>/dev/null)"
CACCT="$TESTROOT/cacct"; mkdir -p "$CACCT"
check tcl-conn "$( \
  printf 'SET-CONNECTION conn1 driver=lmdbnet address=%s namespace=connns token=%s\nLIST-CONNECTIONS\nCREATE-FILE ORDERS USING @conn1\n' \
    "$ASOCK" "$CTOK" | "$TCL" -a "$CACCT" 2>&1 | sed "s#$ASOCK#@SOCK@#g"; \
  printf 'BINDINGS: '; cat "$CACCT/BINDINGS"; \
  (cd "$CACCT" && MVXACCOUNT=. "$TESTROOT/wauth"))"
kill $APID 2>/dev/null
rm -f "$ASOCK"

# A BACKEND THIS HOST DOES NOT HAVE is a question, not a fatal error (mvx#113).
# Migration is per file, so a repository's files need not all live on the same
# backend, and cloning onto a machine without postgres is an ordinary thing to
# want — it used to abort part-way through and leave a half-made account.
#
# The PROMPT cannot be driven from here, which is exactly why the override
# exists: a path only reachable by hand is a path that rots.  So the three
# non-interactive outcomes are what get asserted, and each is a different
# answer to "what happens when nobody can be asked".
DACCT="$TESTROOT/dacct"; mkdir -p "$DACCT"
check tcl-driver-missing "$( \
  printf 'no override: '; \
  "$TCL" -a "$DACCT" -c "CREATE-FILE ZA USING nosuchdrv" </dev/null 2>&1 \
    | sed -n 's/.*\(not a terminal\).*/\1/p'; \
  printf 'override:    '; \
  MVXDRIVER=lmdb "$TCL" -a "$DACCT" -c "CREATE-FILE ZB USING nosuchdrv" \
    </dev/null 2>&1 | sed -n 's/.*\(file ZB created\).*/\1/p'; \
  printf 'bad override: '; \
  MVXDRIVER=alsonot "$TCL" -a "$DACCT" -c "CREATE-FILE ZC USING nosuchdrv" \
    </dev/null 2>&1 | sed -n 's/.*\(is not a driver on this host\).*/\1/p')"

# postgres backend — only when MVX_PG names a reachable database, e.g.
#   MVX_PG='address=localhost:5432 dbname=mvx user=mvx password=mvx'
# Records round-trip byte-exact through a table (id/rec BYTEA), bound
# through a @connection; the schema isolates the namespace.
if [ -n "${MVX_PG:-}" ]; then
  # psql helper for the native-read test's "external writer" — parse the
  # connection out of MVX_PG (address=host:port dbname=.. user=.. password=..)
  PG_ADDR=$(printf '%s\n' "$MVX_PG" | sed -n 's/.*address=\([^ ]*\).*/\1/p')
  PG_DB=$(printf '%s\n'   "$MVX_PG" | sed -n 's/.*dbname=\([^ ]*\).*/\1/p')
  PG_USER=$(printf '%s\n' "$MVX_PG" | sed -n 's/.*user=\([^ ]*\).*/\1/p')
  PG_PASS=$(printf '%s\n' "$MVX_PG" | sed -n 's/.*password=\([^ ]*\).*/\1/p')
  PG_HOST=${PG_ADDR%%:*}; PG_PORT=${PG_ADDR##*:}
  psql_ext() { PGPASSWORD="$PG_PASS" psql -h "$PG_HOST" -p "$PG_PORT" \
                 -U "$PG_USER" -d "$PG_DB" -qtAc "$1"; }

  PGACCT="$TESTROOT/pgacct"; mkdir -p "$PGACCT"
  printf 'SET-CONNECTION pgtest driver=postgres %s namespace=mvxtest\n' \
    "$MVX_PG" | "$TCL" -a "$PGACCT" >/dev/null 2>&1
  # drop any leftover table from a prior run: bind manually so DELETE-FILE
  # resolves to postgres, then start clean (the check re-creates it)
  printf 'ORDERS @pgtest\n' > "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE ORDERS' >/dev/null 2>&1
  printf 'OPEN "ORDERS" TO F ELSE STOP\nWRITE "Widget":@VM:"Gadget" ON F, "O1"\nWRITE "Acme" ON F, "O2"\nREAD V FROM F, "O1" THEN PRINT "read: ":V<1,1>:"/":V<1,2>\n' > "$TESTROOT/pg.b"
  "$MVX" "$TESTROOT/pg.b" -o "$TESTROOT/pgbin" 2>/dev/null
  check tcl-pg "$( \
    "$TCL" -a "$PGACCT" -c 'CREATE-FILE ORDERS USING @pgtest' 2>&1; \
    (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgbin"); \
    printf 'COUNT ORDERS\nSELECT ORDERS\nLIST ORDERS\n' | \
      "$TCL" -a "$PGACCT" 2>&1)"

  # TRANS() JOIN push-down (#40 phase 2): a WITH filter on a TRANS I-type,
  # with source and target co-located on the same Postgres, compiles to one
  # JOIN — the filter runs server-side, only matching ids return. Result must
  # equal the per-record reference (verified by the same query on both).
  printf 'ORDJ @pgtest\nCUSJ @pgtest\nORDJMV @pgtest\n' >> "$PGACCT/BINDINGS"
  for jf in ORDJ CUSJ ORDJMV; do
    "$TCL" -a "$PGACCT" -c "DELETE-FILE $jf" >/dev/null 2>&1
    "$TCL" -a "$PGACCT" -c "CREATE-FILE $jf USING @pgtest" >/dev/null 2>&1
  done
  cat > "$TESTROOT/pgjn.b" <<'JEOF'
OPEN "CUSJ" TO C ELSE STOP
WRITE "AcmeCorp":@AM:"Sydney" ON C, "C1"
WRITE "BetaLtd":@AM:"Melbourne" ON C, "C2"
WRITE "Gamma":@AM:"Sydney" ON C, "C3"
OPEN "ORDJ" TO O ELSE STOP
WRITE "C1":@AM:"Widget" ON O, "O1"
WRITE "C2":@AM:"Gadget" ON O, "O2"
WRITE "C3":@AM:"Bolt" ON O, "O3"
WRITE "C9":@AM:"Nut" ON O, "O4"
OPEN "DICT", "ORDJ" TO D ELSE STOP
WRITE "D":@AM:"2":@AM:"":@AM:"Product":@AM:"10L" ON D, "PRODUCT"
WRITE "I":@AM:"TRANS(CUSJ,1,2,X)":@AM:"":@AM:"City":@AM:"10L" ON D, "CITY"
JEOF
  "$MVX" "$TESTROOT/pgjn.b" -o "$TESTROOT/pgjnbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgjnbin")
  # Sydney -> O1,O3 (C1,C3); Melbourne -> O2; orphan C9 (O4) matches neither.
  # First over the raw blobs (split_part); then with the target CITY mapped to
  # an identity column (#42) the join uses t."CITY" instead — same result.
  cat > "$TESTROOT/pgjndict.b" <<'JDEOF'
OPEN "DICT", "CUSJ" TO DC ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"10L" ON DC, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"City":@AM:"10L" ON DC, "CITY"
JDEOF
  "$MVX" "$TESTROOT/pgjndict.b" -o "$TESTROOT/pgjndictbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgjndictbin")
  check tcl-transjoin "$( \
    "$TCL" -a "$PGACCT" -c 'LIST ORDJ PRODUCT CITY WITH CITY = "Sydney" BY @ID' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'LIST ORDJ PRODUCT CITY WITH CITY = "Melbourne" BY @ID' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'CREATE-MAP CUSJ NAME CITY' >/dev/null 2>&1; \
    "$TCL" -a "$PGACCT" -c 'LIST ORDJ PRODUCT CITY WITH CITY = "Sydney" BY @ID' 2>&1)"

  # TRANS() JOIN with a multivalued key (#53): classic TRANS maps element-wise
  # over a multivalued foreign key, so the pushed-down JOIN must match a record
  # when ANY of its key values points at a target row satisfying the filter
  # (unnest the key on @VM).  M2's key is C1@VM C2 -> Sydney AND Melbourne; a
  # scalar JOIN would match neither.  CUSJ is mapped (above) so the join uses
  # the CITY column.
  cat > "$TESTROOT/pgjnmv.b" <<'MVEOF'
OPEN "ORDJMV" TO O ELSE STOP
WRITE "C1":@AM:"Single" ON O, "M1"
WRITE "C1":@VM:"C2":@AM:"Multi" ON O, "M2"
OPEN "DICT", "ORDJMV" TO D ELSE STOP
WRITE "D":@AM:"2":@AM:"":@AM:"Product":@AM:"10L" ON D, "PRODUCT"
WRITE "I":@AM:"TRANS(CUSJ,1,2,X)":@AM:"":@AM:"City":@AM:"10L" ON D, "CITY"
MVEOF
  "$MVX" "$TESTROOT/pgjnmv.b" -o "$TESTROOT/pgjnmvbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgjnmvbin")
  # Sydney -> M1 (C1) + M2 (C1) = 2; Melbourne -> M2 (C2) = 1.
  check tcl-transjoinmv "$( \
    "$TCL" -a "$PGACCT" -c 'SELECT ORDJMV WITH CITY = "Sydney"' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SELECT ORDJMV WITH CITY = "Melbourne"' 2>&1)"

  # ORDER BY a TRANS() column push-down (#53b): SORT BY a co-located TRANS
  # I-type pushes the ORDER BY into the JOIN (a string_agg over the unnested
  # key reproduces mvx_trans, incl. a multivalued key). Distinct looked-up
  # cities keep it deterministic; an orphan sorts blank-first. The final block
  # forces the client path (a pre-activated select list makes SYSTEM(11)=1,
  # skipping the push-down) — it must match, proving the JOIN order equals the
  # per-record reference.
  printf 'SCUS @pgtest\nSORD @pgtest\n' >> "$PGACCT/BINDINGS"
  for jf in SCUS SORD; do
    "$TCL" -a "$PGACCT" -c "DELETE-FILE $jf" >/dev/null 2>&1
    "$TCL" -a "$PGACCT" -c "CREATE-FILE $jf USING @pgtest" >/dev/null 2>&1
  done
  cat > "$TESTROOT/pgto.b" <<'TOEOF'
OPEN "SCUS" TO C ELSE STOP
WRITE "Adelaide" ON C, "CA"
WRITE "Brisbane" ON C, "CB"
WRITE "Canberra" ON C, "CC"
WRITE "Darwin" ON C, "CD"
OPEN "SORD" TO F ELSE STOP
WRITE "CB":@AM:"Widget" ON F, "S1"
WRITE "CD":@AM:"Gadget" ON F, "S2"
WRITE "CA":@AM:"Bolt" ON F, "S3"
WRITE "CC":@AM:"Nut" ON F, "S4"
WRITE "CX":@AM:"Orphan" ON F, "S5"
WRITE "CA":@VM:"CD":@AM:"Multi" ON F, "S6"
OPEN "DICT", "SORD" TO D ELSE STOP
WRITE "D":@AM:"2":@AM:"":@AM:"Product":@AM:"10L" ON D, "PRODUCT"
WRITE "I":@AM:"TRANS(SCUS,1,1,X)":@AM:"":@AM:"City":@AM:"12L" ON D, "CITY"
OPEN "DICT", "SCUS" TO DC ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"City":@AM:"12L" ON DC, "CITY"
TOEOF
  "$MVX" "$TESTROOT/pgto.b" -o "$TESTROOT/pgtobin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgtobin")
  check tcl-transorder "$( \
    "$TCL" -a "$PGACCT" -c 'SORT SORD PRODUCT CITY BY CITY' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SORT SORD PRODUCT CITY BY CITY FIRST 3' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'CREATE-MAP SCUS CITY' >/dev/null 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SORT SORD PRODUCT CITY BY CITY' 2>&1; \
    printf 'SELECT SORD\nSORT SORD PRODUCT CITY BY CITY\n' | \
      "$TCL" -a "$PGACCT" 2>&1)"

  # COUNT push-down (#45): count(*) server-side, filtered by a mapped column
  # or the raw blob attribute — one number back, no id stream. ORDJ has
  # CUSTID (attr1) and PRODUCT; CUSJ.CITY is mapped after the join test.
  printf 'CNP @pgtest\n' >> "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE CNP' >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-FILE CNP USING @pgtest' >/dev/null 2>&1
  cat > "$TESTROOT/pgcnt.b" <<'CNEOF'
OPEN "CNP" TO F ELSE STOP
WRITE "Widget":@AM:"NSW" ON F, "C1"
WRITE "Gadget":@AM:"VIC" ON F, "C2"
WRITE "Bolt":@AM:"NSW" ON F, "C3"
WRITE "Nut":@AM:"NSW" ON F, "C4"
OPEN "DICT", "CNP" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
CNEOF
  "$MVX" "$TESTROOT/pgcnt.b" -o "$TESTROOT/pgcntbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgcntbin")
  "$TCL" -a "$PGACCT" -c 'CREATE-MAP CNP NAME STATE' >/dev/null 2>&1
  check tcl-pgcount "$( \
    "$TCL" -a "$PGACCT" -c 'COUNT CNP' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'COUNT CNP WITH STATE = "NSW"' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'COUNT CNP WITH NAME = "Bolt"' 2>&1)"

  # SUM push-down (#46): PRICE is mapped NUMERIC, so SUM totals sum("PRICE")
  # server-side (the column holds the MD2 display value).
  printf 'SMP @pgtest\n' >> "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE SMP' >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-FILE SMP USING @pgtest' >/dev/null 2>&1
  cat > "$TESTROOT/pgsum.b" <<'SMEOF'
OPEN "SMP" TO F ELSE STOP
WRITE "NSW":@AM:"999" ON F, "O1"
WRITE "VIC":@AM:"450" ON F, "O2"
WRITE "NSW":@AM:"1200" ON F, "O3"
OPEN "DICT", "SMP" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
SMEOF
  "$MVX" "$TESTROOT/pgsum.b" -o "$TESTROOT/pgsumbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgsumbin")
  "$TCL" -a "$PGACCT" -c 'CREATE-MAP SMP STATE PRICE' >/dev/null 2>&1
  check tcl-pgsum "$( \
    "$TCL" -a "$PGACCT" -c 'SUM SMP PRICE' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SUM SMP PRICE WITH STATE = "NSW"' 2>&1)"

  # BY + FIRST push-down (#47): top-N pushes ORDER BY / LIMIT. Distinct keys,
  # mapped STATE (text -> COLLATE "C") and PRICE (NUMERIC); result must equal
  # the local sort (tcl-first).
  printf 'FSTP @pgtest\n' >> "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE FSTP' >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-FILE FSTP USING @pgtest' >/dev/null 2>&1
  cat > "$TESTROOT/pgft.b" <<'FTEOF'
OPEN "FSTP" TO F ELSE STOP
WRITE "NSW":@AM:"999" ON F, "P1"
WRITE "VIC":@AM:"450" ON F, "P2"
WRITE "QLD":@AM:"1200" ON F, "P3"
WRITE "ACT":@AM:"300" ON F, "P4"
OPEN "DICT", "FSTP" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
FTEOF
  "$MVX" "$TESTROOT/pgft.b" -o "$TESTROOT/pgftbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgftbin")
  "$TCL" -a "$PGACCT" -c 'CREATE-MAP FSTP STATE PRICE' >/dev/null 2>&1
  check tcl-pgfirst "$( \
    "$TCL" -a "$PGACCT" -c 'SORT FSTP STATE PRICE BY PRICE FIRST 2' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SORT FSTP STATE BY STATE FIRST 2' 2>&1)"

  # range push-down (#48): numeric range pushes NULLIF(mvx_attr,'')::numeric;
  # text range falls back to the scan. Result must equal the local tcl-range.
  check tcl-pgrange "$( \
    "$TCL" -a "$PGACCT" -c 'SORT FSTP STATE PRICE WITH PRICE > "500" BY @ID' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SORT FSTP STATE PRICE WITH PRICE <= "450" BY @ID' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SORT FSTP STATE WITH STATE > "N" BY STATE' 2>&1)"

  # multi-condition WITH push-down (#49): conditions AND into one WHERE.
  # Result must equal the local tcl-multiwith.
  printf 'MWP @pgtest\n' >> "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE MWP' >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-FILE MWP USING @pgtest' >/dev/null 2>&1
  cat > "$TESTROOT/pgmw.b" <<'MWEOF'
OPEN "MWP" TO F ELSE STOP
WRITE "NSW":@AM:"999" ON F, "O1"
WRITE "VIC":@AM:"450" ON F, "O2"
WRITE "QLD":@AM:"1200" ON F, "O3"
WRITE "NSW":@AM:"100" ON F, "O5"
OPEN "DICT", "MWP" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
MWEOF
  "$MVX" "$TESTROOT/pgmw.b" -o "$TESTROOT/pgmwbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgmwbin")
  "$TCL" -a "$PGACCT" -c 'CREATE-MAP MWP STATE PRICE' >/dev/null 2>&1
  check tcl-pgmultiwith "$( \
    "$TCL" -a "$PGACCT" -c 'LIST MWP STATE PRICE WITH STATE = "NSW" AND PRICE > "500" BY @ID' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'LIST MWP STATE PRICE WITH STATE = "NSW" WITH PRICE < "500" BY @ID' 2>&1)"

  # DESCRIBE (#51): the verb renders the backend query it would run instead of
  # running it — an identity-column equality, a numeric range on the blob, an
  # ORDER BY / LIMIT push, and a non-pushable @ID condition that scans and
  # filters in the verb.  Reuses MWP (mapped STATE, PRICE above).
  # DESCRIBE / EXPLAIN work both right after the verb and trailing the
  # sentence — same plan either way — so the cases mix the two positions.
  check tcl-pgdescribe "$( \
    "$TCL" -a "$PGACCT" -c 'LIST DESCRIBE MWP STATE WITH STATE = "NSW"' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'LIST MWP WITH STATE = "NSW" AND PRICE > "500" DESCRIBE' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'SORT EXPLAIN MWP BY PRICE FIRST 3' 2>&1; \
    "$TCL" -a "$PGACCT" -c 'LIST MWP WITH @ID = "O1" DESCRIBE' 2>&1)"

  # cross-process record locks (#16), including the mapped association subtables:
  # a READU on a PG-backed file takes a Postgres advisory lock keyed by the
  # record id, so it governs the parent columns AND the association child rows
  # alike (they share the id) — a second *process* sees the whole record LOCKED.
  # LK is mapped with a LINES association and switched to native mode, so the
  # holder and probes read the child subtable back under the lock.  A background
  # holder grabs K1 and signals via an OS file; while it holds, a second process
  # finds it LOCKED; after the holder RELEASEs and exits, a third acquires it and
  # reads its 2 association rows.  Deterministic — poll the held-flag before
  # probing, and wait for the holder's (synchronous) RELEASE before re-probing.
  printf 'LK @pgtest\n' >> "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE LK' >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-FILE LK USING @pgtest' >/dev/null 2>&1
  cat > "$TESTROOT/lkdict.b" <<'EOF'
OPEN "DICT", "LK" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"10L" ON D, "NAME"
WRITE "D":@AM:"5":@AM:"":@AM:"Item":@AM:"6L":@AM:"LINES" ON D, "ITEM"
WRITE "D":@AM:"6":@AM:"":@AM:"Qty":@AM:"4R":@AM:"LINES" ON D, "QTY"
EOF
  "$MVX" "$TESTROOT/lkdict.b" -o "$TESTROOT/lkdictbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/lkdictbin") >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-MAP LK NAME ITEM QTY' >/dev/null 2>&1
  cat > "$TESTROOT/lkseed.b" <<'EOF'
OPEN "LK" TO F ELSE STOP
R = ""
R<1> = "widget"
R<5> = "a":@VM:"b"
R<6> = "1":@VM:"2"
WRITE R ON F, "K1"
EOF
  "$MVX" "$TESTROOT/lkseed.b" -o "$TESTROOT/lkseedbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/lkseedbin") >/dev/null 2>&1
  cat > "$TESTROOT/lkhold.b" <<'EOF'
OPEN "LK" TO F ELSE STOP
READU R FROM F, "K1" ELSE R = ""
J = OSWRITE("1", "HELDFLAG")
LOOP
   G = OSREAD("GOFLAG")
UNTIL STATUS() = 0 DO
REPEAT
RELEASE F, "K1"
EOF
  "$MVX" "$TESTROOT/lkhold.b" -o "$TESTROOT/lkholdbin" 2>/dev/null
  cat > "$TESTROOT/lktry.b" <<'EOF'
OPEN "LK" TO F ELSE STOP
READU R FROM F, "K1" LOCKED
   PRINT "locked by another session"
END THEN
   PRINT "acquired ":R<1>:" lines=":DCOUNT(R<5>, @VM)
   RELEASE F, "K1"
END ELSE
   PRINT "gone"
END
EOF
  "$MVX" "$TESTROOT/lktry.b" -o "$TESTROOT/lktrybin" 2>/dev/null
  # a helper that runs the holder in the background, waits until it holds, runs
  # the probe (its output is the result), then releases the holder and reaps it.
  pglock_probe() {
    rm -f "$PGACCT/HELDFLAG" "$PGACCT/GOFLAG"
    ( cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/lkholdbin" >/dev/null 2>&1 ) &
    _hp=$!
    for _i in $(seq 1 200); do [ -f "$PGACCT/HELDFLAG" ] && break; sleep 0.05; done
    (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/lktrybin" 2>&1)
    : > "$PGACCT/GOFLAG"
    wait "$_hp" 2>/dev/null
  }
  # phase 1 — mirror mode: the SQL columns/child tables are only a derived
  # projection, so READU uses the process-local table, NOT the backend lock;
  # a second *process* is therefore NOT blocked.
  LKM="$(pglock_probe)"
  # phase 2 — native mode: the SQL (parent + association subtables) is the
  # source of truth, so READU takes the cross-process advisory lock; the probe
  # finds it LOCKED, then (after the holder RELEASEs) acquires it and reads the
  # 2 association subtable rows back under the lock.
  "$TCL" -a "$PGACCT" -c 'MAP-MODE LK native' >/dev/null 2>&1
  LKB="$(pglock_probe)"
  LKC="$(cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/lktrybin" 2>&1)"
  check tcl-pglock "$(printf '%s\n%s\n%s\n' "$LKM" "$LKB" "$LKC")"

  # whole-account LISTF on Postgres (#17): an account bound entirely to a PG
  # connection (`* @conn`) enumerates the schema's record tables via the
  # driver's names(); DICT.<file> and association child tables are excluded.
  # A dedicated schema (lftest) keeps the listing deterministic.  Spaces are
  # collapsed so the assertion is on the files/types, not column padding.
  LFACCT="$TESTROOT/lfacct"; mkdir -p "$LFACCT"
  printf 'SET-CONNECTION lfpg driver=postgres %s namespace=lftest\n' "$MVX_PG" \
    | "$TCL" -a "$LFACCT" >/dev/null 2>&1
  printf '* @lfpg\n' > "$LFACCT/BINDINGS"
  "$TCL" -a "$LFACCT" -c 'DELETE-FILE FOO' >/dev/null 2>&1
  "$TCL" -a "$LFACCT" -c 'DELETE-FILE BAR' >/dev/null 2>&1
  "$TCL" -a "$LFACCT" -c 'CREATE-FILE FOO' >/dev/null 2>&1
  "$TCL" -a "$LFACCT" -c 'CREATE-FILE BAR' >/dev/null 2>&1
  check tcl-pglistf "$("$TCL" -a "$LFACCT" -c 'LISTF' 2>&1 | sed 's/  */ /g')"

  # mapping phase 2 (#23/#26): BUILD-MAP projects single-valued attrs into
  # columns on the record's table and each association into a child table.
  printf 'MORD @pgtest\n' >> "$PGACCT/BINDINGS"
  "$TCL" -a "$PGACCT" -c 'DELETE-FILE MORD' >/dev/null 2>&1
  "$TCL" -a "$PGACCT" -c 'CREATE-FILE MORD USING @pgtest' >/dev/null 2>&1
  cat > "$TESTROOT/pgmap.b" <<'MEOF'
OPEN "MORD" TO F ELSE STOP
R = ""
R<1> = "Acme Corp"
R<5> = "Widget":@VM:"Gadget"
R<6> = "2":@VM:"1"
R<7> = "999":@VM:"450"
WRITE R ON F, "O1"
R = ""
R<1> = "Beta Ltd"
R<5> = "Sprocket"
R<6> = "5"
R<7> = "125"
WRITE R ON F, "O2"
OPEN "DICT", "MORD" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"20L" ON D, "CUSTOMER"
WRITE "D":@AM:"5":@AM:"":@AM:"Product":@AM:"10L":@AM:"ORDERITEMS" ON D, "PRODUCT"
WRITE "D":@AM:"6":@AM:"":@AM:"Qty":@AM:"5R":@AM:"ORDERITEMS" ON D, "QTY"
WRITE "D":@AM:"7":@AM:"MD2$":@AM:"Price":@AM:"8R":@AM:"ORDERITEMS" ON D, "PRICE"
MEOF
  "$MVX" "$TESTROOT/pgmap.b" -o "$TESTROOT/pgmapbin" 2>/dev/null
  (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgmapbin")
  check tcl-mapbuild "$(printf 'BUILD-MAP MORD CUSTOMER PRODUCT QTY PRICE\n' | \
    "$TCL" -a "$PGACCT" 2>&1)"

  # BUILD-MAP PROGRESS (#28): opt-in live indicator (records/percent/rate) on
  # stderr — suppressed here; the count summary still prints on stdout and the
  # result is unchanged.  Quiet by default (tcl-mapbuild above has no PROGRESS).
  check tcl-mapprogress "$(printf 'BUILD-MAP MORD CUSTOMER PRICE PROGRESS\n' | \
    "$TCL" -a "$PGACCT" 2>/dev/null)"

  # mirror-on-write (#18): CREATE-MAP declares %MAP%; a later WRITE from a
  # program then auto-projects into the mapping (via the runtime hook).
  cat > "$TESTROOT/pgw3.b" <<'W3EOF'
OPEN "MORD" TO F ELSE STOP
R = ""
R<1> = "Gamma Inc"
R<5> = "Nut":@VM:"Washer"
R<6> = "40":@VM:"40"
R<7> = "0.05":@VM:"0.02"
WRITE R ON F, "O3"
PRINT "wrote O3"
W3EOF
  "$MVX" "$TESTROOT/pgw3.b" -o "$TESTROOT/pgw3bin" 2>/dev/null
  check tcl-mapmirror "$( \
    printf 'CREATE-MAP MORD CUSTOMER PRODUCT QTY PRICE\n' | \
      "$TCL" -a "$PGACCT" 2>&1; \
    (cd "$PGACCT" && MVXACCOUNT=. "$TESTROOT/pgw3bin"))"

  # LIST-MAPS / DELETE-MAP (#31), in a fresh account so LIST-MAPS is clean
  VMACCT="$TESTROOT/vmacct"; mkdir -p "$VMACCT"
  printf 'SET-CONNECTION vpg driver=postgres %s namespace=vmtest\n' \
    "$MVX_PG" | "$TCL" -a "$VMACCT" >/dev/null 2>&1
  printf 'PARTS @vpg\n' > "$VMACCT/BINDINGS"
  "$TCL" -a "$VMACCT" -c 'DELETE-FILE PARTS' >/dev/null 2>&1
  "$TCL" -a "$VMACCT" -c 'CREATE-FILE PARTS USING @vpg' >/dev/null 2>&1
  cat > "$TESTROOT/vm.b" <<'VMEOF'
OPEN "PARTS" TO F ELSE STOP
WRITE "Widget":@AM:"999" ON F, "P1"
OPEN "DICT", "PARTS" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
VMEOF
  "$MVX" "$TESTROOT/vm.b" -o "$TESTROOT/vmbin" 2>/dev/null
  (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmbin")
  check tcl-mapverbs "$(printf '%s\n' \
    'CREATE-MAP PARTS NAME PRICE' \
    'LIST-MAPS' \
    'DELETE-MAP PARTS' \
    'LIST-MAPS' | "$TCL" -a "$VMACCT" 2>&1)"

  # typed DATE/TIME columns (#32): a D/MT dict item projects to a real
  # SQL date/time column carrying the internal value as ISO-8601, not the
  # locale-shaped display conversion.  Empty cells store NULL.
  printf 'EVT @vpg\n' >> "$VMACCT/BINDINGS"
  "$TCL" -a "$VMACCT" -c 'DELETE-FILE EVT' >/dev/null 2>&1
  "$TCL" -a "$VMACCT" -c 'CREATE-FILE EVT USING @vpg' >/dev/null 2>&1
  cat > "$TESTROOT/vmdt.b" <<'DTEOF'
OPEN "EVT" TO F ELSE STOP
R = ""
R<1> = "Launch"
R<2> = ICONV("25 JUL 2026", "D")
R<3> = ICONV("14:30:00", "MTS")
WRITE R ON F, "E1"
R = ""
R<1> = "Empty"
WRITE R ON F, "E2"
OPEN "DICT", "EVT" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"D4/":@AM:"When":@AM:"12R" ON D, "WHEN"
WRITE "D":@AM:"3":@AM:"MTS":@AM:"At":@AM:"10R" ON D, "AT"
DTEOF
  "$MVX" "$TESTROOT/vmdt.b" -o "$TESTROOT/vmdtbin" 2>/dev/null
  (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmdtbin")
  # LIST reads the record through the D/MT conversions; the SQL projection
  # rendered the same instants as ISO date/time columns (verified via psql).
  check tcl-mapdt "$(printf '%s\n' \
    'CREATE-MAP EVT NAME WHEN AT' \
    'SORT EVT BY NAME WHEN AT NAME' | "$TCL" -a "$VMACCT" 2>&1)"

  # backfill push-down (#56): EVT's parent NAME(text)/WHEN(date)/AT(time)
  # columns are all SQL-expressible, so CREATE-MAP's backfill runs as one
  # server-side UPDATE over all rows (no records over the wire) instead of the
  # per-record loop.  Switch EVT native so reads come from those columns, then
  # the identical SORT must render byte-for-byte the same as the blob-based read
  # above — proving the single-UPDATE date/time column values are correct.
  "$TCL" -a "$VMACCT" -c 'MAP-MODE EVT native' >/dev/null 2>&1
  check tcl-mapdtnative "$(printf 'SORT EVT BY NAME WHEN AT NAME\n' | \
    "$TCL" -a "$VMACCT" 2>&1)"

  # native mode (#33): the typed columns are authoritative, so a WRITE whose
  # value does not fit its column is rejected (ON ERROR) and the record is
  # not written — versus mirror mode, which stores NULL and proceeds.
  # MAP-MODE views/sets the policy, refusing a switch that existing data
  # would violate.
  printf 'ITM @vpg\n' >> "$VMACCT/BINDINGS"
  "$TCL" -a "$VMACCT" -c 'DELETE-FILE ITM' >/dev/null 2>&1
  "$TCL" -a "$VMACCT" -c 'CREATE-FILE ITM USING @vpg' >/dev/null 2>&1
  cat > "$TESTROOT/vmi.b" <<'IEOF'
OPEN "ITM" TO F ELSE STOP
WRITE "Widget":@AM:"1000" ON F, "I1"
OPEN "DICT", "ITM" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
IEOF
  "$MVX" "$TESTROOT/vmi.b" -o "$TESTROOT/vmibin" 2>/dev/null
  (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmibin")
  # native write: good row commits, bad (non-numeric price) row rejected
  cat > "$TESTROOT/vmnat.b" <<'NEOF'
OPEN "ITM" TO F ELSE STOP
WRITE "Gadget":@AM:"2550" ON F, "I2" ON ERROR PRINT "I2 rejected"
PRINT "I2 ok"
WRITE "Broken":@AM:"abc" ON F, "I3" ON ERROR PRINT "I3 rejected"
PRINT "done"
NEOF
  "$MVX" "$TESTROOT/vmnat.b" -o "$TESTROOT/vmnatbin" 2>/dev/null
  # mirror write: the same bad row is tolerated (projects NULL)
  cat > "$TESTROOT/vmmir.b" <<'MEOF'
OPEN "ITM" TO F ELSE STOP
WRITE "Junk":@AM:"notanum" ON F, "I9" ON ERROR PRINT "I9 rejected"
PRINT "I9 written"
MEOF
  "$MVX" "$TESTROOT/vmmir.b" -o "$TESTROOT/vmmirbin" 2>/dev/null
  check tcl-mapnative "$( \
    printf '%s\n' 'CREATE-MAP ITM NAME PRICE' 'MAP-MODE ITM native' \
      | "$TCL" -a "$VMACCT" 2>&1; \
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmnatbin"); \
    printf 'COUNT ITM\n' | "$TCL" -a "$VMACCT" 2>&1; \
    "$TCL" -a "$VMACCT" -c 'MAP-MODE ITM mirror' 2>&1; \
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmmirbin"); \
    "$TCL" -a "$VMACCT" -c 'MAP-MODE ITM native' 2>&1)"

  # native read (#34): in native mode READ recomposes the record from the
  # SQL columns/child rows, so an edit made straight to the tables (here via
  # psql, an "external" writer) is what the program reads.  Needs psql.
  if command -v psql >/dev/null 2>&1; then
    printf 'ORD @vpg\n' >> "$VMACCT/BINDINGS"
    "$TCL" -a "$VMACCT" -c 'DELETE-FILE ORD' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-FILE ORD USING @vpg' >/dev/null 2>&1
    cat > "$TESTROOT/vmord.b" <<'OEOF'
OPEN "ORD" TO F ELSE STOP
R = ""
R<1> = "Acme Corp"
R<2> = ICONV("25 JUL 2026", "D")
R<3> = "keep me"
R<5> = "Widget":@VM:"Gadget"
R<6> = "2":@VM:"1"
WRITE R ON F, "O1"
OPEN "DICT", "ORD" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"12L" ON D, "CUSTOMER"
WRITE "D":@AM:"2":@AM:"D4/":@AM:"When":@AM:"12R" ON D, "WHEN"
WRITE "D":@AM:"5":@AM:"":@AM:"Product":@AM:"10L":@AM:"ORDITEMS" ON D, "PRODUCT"
WRITE "D":@AM:"6":@AM:"MR0":@AM:"Qty":@AM:"5R":@AM:"ORDITEMS" ON D, "QTY"
OEOF
    "$MVX" "$TESTROOT/vmord.b" -o "$TESTROOT/vmordbin" 2>/dev/null
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmordbin")
    "$TCL" -a "$VMACCT" -c 'CREATE-MAP ORD CUSTOMER WHEN PRODUCT QTY' \
      >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'MAP-MODE ORD native' >/dev/null 2>&1
    # external edits straight to the tables (id 'O1' = \x4f31)
    psql_ext "UPDATE vmtest.\"ORD\" SET \"CUSTOMER\"='Beta Ltd', \"WHEN\"=DATE '2027-01-15' WHERE id='\\x4f31'" >/dev/null
    psql_ext "UPDATE vmtest.\"ORD_ORDITEMS\" SET \"QTY\"=99 WHERE id='\\x4f31' AND seq=1" >/dev/null
    psql_ext "INSERT INTO vmtest.\"ORD_ORDITEMS\"(id,seq,\"PRODUCT\",\"QTY\") VALUES('\\x4f31',3,'Sprocket',5)" >/dev/null
    psql_ext "INSERT INTO vmtest.\"ORD\"(id,\"CUSTOMER\",\"WHEN\") VALUES('\\x4f39','SQL Only',DATE '2026-12-31')" >/dev/null
    cat > "$TESTROOT/vmordr.b" <<'REOF'
OPEN "ORD" TO F ELSE STOP
READ R FROM F, "O1" THEN
   PRINT "O1 ":R<1>:" | ":OCONV(R<2>,"D4/"):" | note=":R<3>
   PRINT "   items ":R<5>:" qty ":R<6>
END
READ R FROM F, "O9" THEN
   PRINT "O9 ":R<1>:" | ":OCONV(R<2>,"D4/")
END ELSE PRINT "O9 missing"
REOF
    "$MVX" "$TESTROOT/vmordr.b" -o "$TESTROOT/vmordrbin" 2>/dev/null
    check tcl-mapnread "$( \
      (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmordrbin"); \
      printf 'COUNT ORD\n' | "$TCL" -a "$VMACCT" 2>&1)"

    # write diff (#35): an update projects only what changed.  Proven with
    # Postgres row xmin — a parent-only write leaves the child rows physically
    # untouched; a line-item change advances their xmin; an identical rewrite
    # touches nothing.  Fresh mirror-mode file, seeded via a program.
    printf 'OPT @vpg\n' >> "$VMACCT/BINDINGS"
    "$TCL" -a "$VMACCT" -c 'DELETE-FILE OPT' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-FILE OPT USING @vpg' >/dev/null 2>&1
    cat > "$TESTROOT/vmopt.b" <<'OPTEOF'
OPEN "OPT" TO F ELSE STOP
R = ""
R<1> = "Acme"
R<5> = "Widget":@VM:"Gadget"
R<6> = "2":@VM:"1"
WRITE R ON F, "P1"
OPEN "DICT", "OPT" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Customer":@AM:"12L" ON D, "CUSTOMER"
WRITE "D":@AM:"5":@AM:"":@AM:"Product":@AM:"10L":@AM:"OITEMS" ON D, "PRODUCT"
WRITE "D":@AM:"6":@AM:"MR0":@AM:"Qty":@AM:"5R":@AM:"OITEMS" ON D, "QTY"
OPTEOF
    "$MVX" "$TESTROOT/vmopt.b" -o "$TESTROOT/vmoptbin" 2>/dev/null
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmoptbin")
    "$TCL" -a "$VMACCT" -c 'CREATE-MAP OPT CUSTOMER PRODUCT QTY' >/dev/null 2>&1
    xchild() { psql_ext "SELECT string_agg(xmin::text,',' ORDER BY seq) \
                 FROM vmtest.\"OPT_OITEMS\" WHERE id='\\x5031'"; }
    cat > "$TESTROOT/vmd1.b" <<'D1'
OPEN "OPT" TO F ELSE STOP
READ R FROM F, "P1" THEN R<1> = "Renamed Co"
WRITE R ON F, "P1"
D1
    cat > "$TESTROOT/vmd2.b" <<'D2'
OPEN "OPT" TO F ELSE STOP
READ R FROM F, "P1" THEN R<6,1> = "42"
WRITE R ON F, "P1"
D2
    cat > "$TESTROOT/vmd3.b" <<'D3'
OPEN "OPT" TO F ELSE STOP
READ R FROM F, "P1" THEN WRITE R ON F, "P1"
D3
    "$MVX" "$TESTROOT/vmd1.b" -o "$TESTROOT/vmd1bin" 2>/dev/null
    "$MVX" "$TESTROOT/vmd2.b" -o "$TESTROOT/vmd2bin" 2>/dev/null
    "$MVX" "$TESTROOT/vmd3.b" -o "$TESTROOT/vmd3bin" 2>/dev/null
    X0=$(xchild)
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmd1bin")   # parent-only
    X1=$(xchild)
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmd2bin")   # line-item
    X2=$(xchild)
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmd3bin")   # identical
    X3=$(xchild)
    check tcl-mapdiff "$(printf '%s\n' \
      "parent-only leaves children: $([ "$X0" = "$X1" ] && echo yes || echo no)" \
      "line-item rewrites children: $([ "$X1" != "$X2" ] && echo yes || echo no)" \
      "identical write no-ops: $([ "$X2" = "$X3" ] && echo yes || echo no)" \
      "customer=$(psql_ext "SELECT \"CUSTOMER\" FROM vmtest.\"OPT\" WHERE id='\\x5031'")" \
      "qty1=$(psql_ext "SELECT \"QTY\" FROM vmtest.\"OPT_OITEMS\" WHERE id='\\x5031' AND seq=1")")"

    # native Postgres indexes on mapped columns (#37): CREATE-INDEX emits a
    # real SQL index and equality WITH pushes down to it — but only on an
    # identity-projected (TEXT, no-conv) column, else it falls back to the
    # scan so the result never differs.
    printf 'CIX @vpg\n' >> "$VMACCT/BINDINGS"
    "$TCL" -a "$VMACCT" -c 'DELETE-FILE CIX' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-FILE CIX USING @vpg' >/dev/null 2>&1
    cat > "$TESTROOT/vmcix.b" <<'CIXEOF'
OPEN "CIX" TO F ELSE STOP
WRITE "Widget":@AM:"NSW":@AM:"1500" ON F, "C1"
WRITE "Gadget":@AM:"VIC":@AM:"900" ON F, "C2"
WRITE "Sprocket":@AM:"NSW":@AM:"250" ON F, "C3"
OPEN "DICT", "CIX" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"3":@AM:"MD2":@AM:"Credit":@AM:"10R" ON D, "CREDIT"
CIXEOF
    "$MVX" "$TESTROOT/vmcix.b" -o "$TESTROOT/vmcixbin" 2>/dev/null
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmcixbin")
    "$TCL" -a "$VMACCT" -c 'CREATE-MAP CIX NAME STATE CREDIT' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-INDEX CIX STATE' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-INDEX CIX CREDIT' >/dev/null 2>&1
    IXEXISTS=$(psql_ext "SELECT count(*) FROM pg_indexes WHERE schemaname='vmtest' AND indexname='CIX_STATE_idx'")
    # push-down proof: divert C2's STATE column to NSW in SQL only (rec still
    # VIC); if C2 appears, the SQL index ran, not a rec scan.
    psql_ext "UPDATE vmtest.\"CIX\" SET \"STATE\"='NSW' WHERE id='\\x4332'" >/dev/null
    check tcl-pgindex "$( \
      echo "STATE index exists: $IXEXISTS"; \
      echo "-- WITH STATE = NSW (index push-down, C2 diverted in SQL) --"; \
      "$TCL" -a "$VMACCT" -c 'LIST CIX NAME STATE WITH STATE = "NSW" BY NAME' 2>&1; \
      echo "-- WITH CREDIT = 1500 (converted column -> blob push-down) --"; \
      "$TCL" -a "$VMACCT" -c 'LIST CIX NAME CREDIT WITH CREDIT = "1500"' 2>&1; \
      "$TCL" -a "$VMACCT" -c 'DELETE-INDEX CIX STATE' 2>&1; \
      echo "STATE index after drop: $(psql_ext "SELECT count(*) FROM pg_indexes WHERE schemaname='vmtest' AND indexname='CIX_STATE_idx'")")"

    # WITH push-down (#38/#39): a filter on a mapped identity column runs in
    # the backend even without an index (proven by diverting the column in SQL
    # only — if C2 appears, the WHERE read the column, not the rec blob), and
    # an un-mapped or converted field pushes down straight onto the record
    # blob (split_part on rec), so it too filters server-side.
    printf 'PDN @vpg\n' >> "$VMACCT/BINDINGS"
    "$TCL" -a "$VMACCT" -c 'DELETE-FILE PDN' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-FILE PDN USING @vpg' >/dev/null 2>&1
    cat > "$TESTROOT/vmpdn.b" <<'PDNEOF'
OPEN "PDN" TO F ELSE STOP
WRITE "Widget":@AM:"NSW":@AM:"":@AM:"gold":@AM:"1500" ON F, "C1"
WRITE "Gadget":@AM:"VIC":@AM:"":@AM:"silver":@AM:"900" ON F, "C2"
WRITE "Bolt":@AM:"NSW":@AM:"":@AM:"gold":@AM:"250" ON F, "C3"
OPEN "DICT", "PDN" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"4":@AM:"":@AM:"Tier":@AM:"8L" ON D, "TIER"
WRITE "D":@AM:"5":@AM:"MD2":@AM:"Credit":@AM:"10R" ON D, "CREDIT"
PDNEOF
    "$MVX" "$TESTROOT/vmpdn.b" -o "$TESTROOT/vmpdnbin" 2>/dev/null
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmpdnbin")
    # STATE + CREDIT mapped; TIER left unmapped; no index created
    "$TCL" -a "$VMACCT" -c 'CREATE-MAP PDN NAME STATE CREDIT' >/dev/null 2>&1
    # divert C2's STATE column to ZZZ in SQL only (rec blob still VIC)
    psql_ext "UPDATE vmtest.\"PDN\" SET \"STATE\"='ZZZ' WHERE id='\\x4332'" >/dev/null
    check tcl-pgpushdown "$( \
      echo "-- WITH STATE = ZZZ (identity column; C2 diverted in SQL) --"; \
      "$TCL" -a "$VMACCT" -c 'LIST PDN NAME STATE WITH STATE = "ZZZ"' 2>&1; \
      echo "-- WITH STATE # NSW (not-equal push-down) --"; \
      "$TCL" -a "$VMACCT" -c 'LIST PDN NAME STATE WITH STATE # "NSW" BY NAME' 2>&1; \
      echo "-- WITH TIER = gold (unmapped field -> blob push-down) --"; \
      "$TCL" -a "$VMACCT" -c 'LIST PDN NAME TIER WITH TIER = "gold" BY NAME' 2>&1; \
      echo "-- WITH CREDIT = 1500 (converted field, raw -> blob push-down) --"; \
      "$TCL" -a "$VMACCT" -c 'LIST PDN NAME CREDIT WITH CREDIT = "1500"' 2>&1)"

    # expression indexes (#43): CREATE-INDEX on an un-mapped field builds a
    # Postgres expression index on the blob (via the IMMUTABLE mvx_attr
    # helper), so the blob push-down becomes an index scan. A mapped identity
    # field still gets a column index. PDN has STATE mapped, TIER unmapped.
    "$TCL" -a "$VMACCT" -c 'CREATE-INDEX PDN STATE' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-INDEX PDN TIER' >/dev/null 2>&1
    IDXDEF=$(psql_ext "SELECT CASE WHEN indexdef LIKE '%mvx_attr(rec, 4)%' \
      THEN 'expression' ELSE 'other' END FROM pg_indexes \
      WHERE schemaname='vmtest' AND indexname='PDN_TIER_idx'")
    STDEF=$(psql_ext "SELECT CASE WHEN indexdef LIKE '%(\"STATE\")%' \
      THEN 'column' ELSE 'other' END FROM pg_indexes \
      WHERE schemaname='vmtest' AND indexname='PDN_STATE_idx'")
    EXPLN=$(psql_ext "SET enable_seqscan=off; EXPLAIN SELECT id FROM \
      vmtest.\"PDN\" WHERE vmtest.mvx_attr(rec,4)='gold'")
    USES=$(printf '%s' "$EXPLN" | grep -q 'PDN_TIER_idx' && echo yes || echo no)
    check tcl-exprindex "$(printf '%s\n' \
      "TIER (unmapped) index kind: $IDXDEF" \
      "STATE (mapped) index kind: $STDEF" \
      "blob filter uses the expression index: $USES")"

    # CREATE-MAP reindex prompt (#44): a field indexed while un-mapped has an
    # expression index; mapping it offers to rebuild the index on the column.
    printf 'RIX @vpg\n' >> "$VMACCT/BINDINGS"
    "$TCL" -a "$VMACCT" -c 'DELETE-FILE RIX' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-FILE RIX USING @vpg' >/dev/null 2>&1
    cat > "$TESTROOT/vmrix.b" <<'RIXEOF'
OPEN "RIX" TO F ELSE STOP
WRITE "Widget":@AM:"NSW" ON F, "C1"
OPEN "DICT", "RIX" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
RIXEOF
    "$MVX" "$TESTROOT/vmrix.b" -o "$TESTROOT/vmrixbin" 2>/dev/null
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmrixbin")
    "$TCL" -a "$VMACCT" -c 'CREATE-INDEX RIX STATE' >/dev/null 2>&1
    rixkind() { psql_ext "SELECT CASE WHEN indexdef LIKE '%mvx_attr%' THEN \
      'expression' WHEN indexdef LIKE '%(\"STATE\")%' THEN 'column' ELSE '?' \
      END FROM pg_indexes WHERE schemaname='vmtest' AND indexname='RIX_STATE_idx'"; }
    RBEFORE=$(rixkind)
    printf 'y\n' | "$TCL" -a "$VMACCT" -c 'CREATE-MAP RIX NAME STATE' >/dev/null 2>&1
    RAFTER=$(rixkind)
    check tcl-mapreindex "$(printf '%s\n' \
      "STATE index before map: $RBEFORE" \
      "STATE index after CREATE-MAP + y: $RAFTER")"

    # CREATE-MAP ALL (#50): mapping every field is opt-in and confirmed, since
    # each WRITE then projects every column. 'n' cancels, 'y' proceeds.
    printf 'MAF @vpg\n' >> "$VMACCT/BINDINGS"
    "$TCL" -a "$VMACCT" -c 'DELETE-FILE MAF' >/dev/null 2>&1
    "$TCL" -a "$VMACCT" -c 'CREATE-FILE MAF USING @vpg' >/dev/null 2>&1
    cat > "$TESTROOT/vmmaf.b" <<'MAFEOF'
OPEN "MAF" TO F ELSE STOP
WRITE "Widget":@AM:"NSW" ON F, "M1"
OPEN "DICT", "MAF" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
MAFEOF
    "$MVX" "$TESTROOT/vmmaf.b" -o "$TESTROOT/vmmafbin" 2>/dev/null
    (cd "$VMACCT" && MVXACCOUNT=. "$TESTROOT/vmmafbin")
    check tcl-mapall "$( \
      printf 'n\n' | "$TCL" -a "$VMACCT" -c 'CREATE-MAP MAF ALL' 2>&1; \
      printf 'y\n' | "$TCL" -a "$VMACCT" -c 'CREATE-MAP MAF ALL' 2>&1)"
  else
    echo "  (postgres psql-dependent map/index tests skipped — psql not found)"
  fi
else
  echo "  (postgres test skipped — set MVX_PG to run)"
fi

# sqlite backend.  UNCONDITIONAL, unlike postgres/mongo: an embedded backend
# needs no server, which is the whole reason it exists — so if it is built, it
# is tested on every run rather than behind an env var nobody sets.
if ls "$ROOT"/build/lib/libmvxdrv_sqlite.* >/dev/null 2>&1; then
  echo "== sqlite backend"
  SQA="$TESTROOT/sqacct"; mkdir -p "$SQA"
  printf '# MVX account descriptor\nname=sqacct\nversion=1\n' > "$SQA/.mvx"
  printf '* sqlite %s/acct.sqlite\n' "$SQA" > "$SQA/BINDINGS"
  "$TCL" -a "$SQA" -c 'CREATE-FILE CUST' >/dev/null 2>&1
  cat > "$TESTROOT/sqseed.b" <<'SQEOF'
OPEN "CUST" TO F ELSE PRINT "no CUST" ; STOP
WRITE "Ada":@AM:"London":@AM:"42" ON F, "C1"
WRITE "Grace":@AM:"York":@AM:"7" ON F, "C2"
WRITE "Alan":@AM:"Cambridge":@AM:"115" ON F, "C3"
READ R FROM F, "C2" THEN
   PRINT "rt=":R<1>:"/":R<3>
END ELSE PRINT "rt=LOST"
DELETE F, "C3"
N = 0
SELECT F
D = 0
LOOP UNTIL D DO
   READNEXT ID ELSE D = 1
   IF NOT(D) THEN N += 1
REPEAT
PRINT "n=":N
SQEOF
  "$MVX" "$TESTROOT/sqseed.b" -o "$TESTROOT/sqseed" >/dev/null 2>&1
  sqout="$(cd "$SQA" && MVXACCOUNT=. "$TESTROOT/sqseed" 2>&1)"
  # a record survives the round trip with its marks, and DELETE really deletes
  case "$sqout" in
    *"rt=Grace/7"*) PASS=$((PASS+1)); echo "  record round-trips byte-exact" ;;
    *) FAIL=$((FAIL+1)); echo "FAIL sqlite round-trip: $sqout" ;;
  esac
  case "$sqout" in
    *"n=2"*) PASS=$((PASS+1)); echo "  select sees the delete" ;;
    *) FAIL=$((FAIL+1)); echo "FAIL sqlite select/delete: $sqout" ;;
  esac
  # LISTF names the file AND its driver -- the cross-file listing this
  # backend exists for, and the path that used to work only for postgres
  lf="$("$TCL" -a "$SQA" -c 'LISTF' 2>&1)"
  case "$lf" in
    *"CUST"*"sqlite"*) PASS=$((PASS+1)); echo "  LISTF enumerates via the driver" ;;
    *) FAIL=$((FAIL+1)); echo "FAIL sqlite LISTF: $lf" ;;
  esac
  # COUNT and a WITH filter run IN the backend, not by streaming ids to the
  # verb: DESCRIBE shows the statement, so this asserts the push-down happened
  # rather than merely that the answer was right.
  "$TCL" -a "$SQA" -c 'CREATE-FILE DICT CUST' >/dev/null 2>&1
  cat > "$TESTROOT/sqdict.b" <<'SQDEOF'
OPEN "DICT", "CUST" TO D ELSE PRINT "no dict" ; STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L":@AM:"S" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"City":@AM:"12L":@AM:"S" ON D, "CITY"
SQDEOF
  "$MVX" "$TESTROOT/sqdict.b" -o "$TESTROOT/sqdict" >/dev/null 2>&1
  (cd "$SQA" && MVXACCOUNT=. "$TESTROOT/sqdict" >/dev/null 2>&1)
  cnt="$("$TCL" -a "$SQA" -c 'COUNT CUST WITH CITY = "London"' 2>&1)"
  case "$cnt" in
    *"1 record"*) PASS=$((PASS+1)); echo "  filtered COUNT is correct" ;;
    *) FAIL=$((FAIL+1)); echo "FAIL sqlite COUNT: $cnt" ;;
  esac
  desc="$("$TCL" -a "$SQA" -c 'LIST CUST WITH CITY = "London" DESCRIBE' 2>&1)"
  case "$desc" in
    *"SELECT id FROM"*"mvx_attr"*) PASS=$((PASS+1))
      echo "  WITH is pushed into SQL, not scanned in the verb" ;;
    *) FAIL=$((FAIL+1)); echo "FAIL sqlite push-down plan: $desc" ;;
  esac
else
  echo "  (sqlite test skipped — driver not built)"
fi

# mongo backend — only when MVX_MONGO names a reachable MongoDB, e.g.
#   MVX_MONGO='address=localhost:27017 namespace=mvxtest'
if [ -n "${MVX_MONGO:-}" ]; then
  MGACCT="$TESTROOT/mgacct"; mkdir -p "$MGACCT"
  printf '# MVX account descriptor\nname=mgacct\nversion=1\n' > "$MGACCT/.mvx"
  printf 'SET-CONNECTION mongotest driver=mongo %s\n' "$MVX_MONGO" | \
    "$TCL" -a "$MGACCT" >/dev/null 2>&1
  printf 'ORDERS @mongotest\n' > "$MGACCT/BINDINGS"
  "$TCL" -a "$MGACCT" -c 'DELETE-FILE ORDERS' >/dev/null 2>&1
  # records round-trip byte-exact (marks and all) as { _id, rec } documents;
  # write two, read one (multivalue preserved), delete the other.
  printf 'OPEN "ORDERS" TO F ELSE STOP\nWRITE "Widget":@VM:"Gadget" ON F, "O1"\nWRITE "Acme" ON F, "O2"\nREAD V FROM F, "O1" THEN PRINT "read: ":V<1,1>:"/":V<1,2>\nDELETE F, "O2"\n' > "$TESTROOT/mg.b"
  "$MVX" "$TESTROOT/mg.b" -o "$TESTROOT/mgbin" 2>/dev/null
  check tcl-mongo "$( \
    "$TCL" -a "$MGACCT" -c 'CREATE-FILE ORDERS USING @mongotest' 2>&1; \
    (cd "$MGACCT" && MVXACCOUNT=. "$TESTROOT/mgbin"); \
    printf 'COUNT ORDERS\nSELECT ORDERS\nLIST ORDERS\n' | \
      "$TCL" -a "$MGACCT" 2>&1)"

  # relational mapping + native index + WITH/COUNT push-down (#62). CREATE-MAP
  # projects each mapped dict column onto its { _id, rec } document as a native
  # BSON field (NAME/STATE text, PRICE numeric) and each association as an
  # embedded array (LINES = [{PRODUCT,QTY},…]), so equality filters and counts
  # run server-side and CREATE-INDEX builds a real Mongo index on NAME. The
  # query results must equal the client-side scan reference.
  printf 'MORD @mongotest\n' >> "$MGACCT/BINDINGS"
  "$TCL" -a "$MGACCT" -c 'DELETE-FILE MORD' >/dev/null 2>&1
  "$TCL" -a "$MGACCT" -c 'CREATE-FILE MORD USING @mongotest' >/dev/null 2>&1
  cat > "$TESTROOT/mgmap.b" <<'MMEOF'
OPEN "MORD" TO F ELSE STOP
WRITE "Widget":@AM:"NSW":@AM:"999":@AM:"A":@VM:"B":@AM:"2":@VM:"3" ON F, "O1"
WRITE "Gadget":@AM:"VIC":@AM:"450":@AM:"C":@AM:"1" ON F, "O2"
WRITE "Bolt":@AM:"NSW":@AM:"1200":@AM:"D":@AM:"5" ON F, "O3"
WRITE "Nut":@AM:"NSW":@AM:"300" ON F, "O4"
OPEN "DICT", "MORD" TO D ELSE STOP
WRITE "D":@AM:"1":@AM:"":@AM:"Name":@AM:"12L" ON D, "NAME"
WRITE "D":@AM:"2":@AM:"":@AM:"State":@AM:"6L" ON D, "STATE"
WRITE "D":@AM:"3":@AM:"MD2":@AM:"Price":@AM:"8R" ON D, "PRICE"
WRITE "D":@AM:"4":@AM:"":@AM:"Product":@AM:"10L":@AM:"LINES" ON D, "PRODUCT"
WRITE "D":@AM:"5":@AM:"MD0":@AM:"Qty":@AM:"5R":@AM:"LINES" ON D, "QTY"
MMEOF
  "$MVX" "$TESTROOT/mgmap.b" -o "$TESTROOT/mgmapbin" 2>/dev/null
  (cd "$MGACCT" && MVXACCOUNT=. "$TESTROOT/mgmapbin")
  check tcl-mongomap "$( \
    "$TCL" -a "$MGACCT" -c 'CREATE-MAP MORD NAME STATE PRICE PRODUCT QTY' 2>&1; \
    "$TCL" -a "$MGACCT" -c 'COUNT MORD' 2>&1; \
    "$TCL" -a "$MGACCT" -c 'COUNT MORD WITH STATE = "NSW"' 2>&1; \
    "$TCL" -a "$MGACCT" -c 'COUNT MORD WITH NAME = "Bolt"' 2>&1; \
    "$TCL" -a "$MGACCT" -c 'LIST MORD NAME WITH STATE = "NSW" BY @ID' 2>&1; \
    "$TCL" -a "$MGACCT" -c 'CREATE-INDEX MORD NAME' 2>&1; \
    "$TCL" -a "$MGACCT" -c 'LIST MORD NAME WITH NAME = "Bolt"' 2>&1)"
else
  echo "  (mongo test skipped — set MVX_MONGO to run)"
fi

# ---------------------------------------------------------------- phase 3
# The sieve result is deterministic (Count 78498); only its *execution*
# on a loaded CI runner is occasionally flaky (a transient OOM during the
# LLVM compile leaves no binary).  Retry a few times and, on failure,
# surface the actual compile/run error instead of an empty "FAIL sieve:".
if [ "$QUICK" = 0 ]; then
  echo "== sieve"
  sieve_ok=0
  sieve_diag=""
  for attempt in 1 2 3; do
    if ! "$MVX" "$ROOT/bench/sieve.b" -o "$TESTROOT/sieve" \
         2>"$TESTROOT/sieve.err"; then
      sieve_diag="compile failed (attempt $attempt): $(cat "$TESTROOT/sieve.err")"
      continue
    fi
    sieve_out="$("$TESTROOT/sieve" 2>"$TESTROOT/sieve.err")"
    if printf '%s' "$sieve_out" | grep -q "Count: 78498" &&
       printf '%s' "$sieve_out" | grep -q "VALID"; then
      sieve_ok=1
      echo "  sieve valid ($(printf '%s' "$sieve_out" | head -1))"
      break
    fi
    sieve_diag="run output (attempt $attempt): ${sieve_out:-<empty>} $(cat "$TESTROOT/sieve.err")"
  done
  if [ "$sieve_ok" = 1 ]; then
    PASS=$((PASS + 1))
  elif [ -n "${MVX_SIEVE_OPTIONAL:-}" ]; then
    # CI sets this: the sieve is a perf benchmark, not a correctness gate,
    # and is sensitive to runner contention — report but do not fail.
    echo "WARN sieve (non-fatal): $sieve_diag"
  else
    echo "FAIL sieve: $sieve_diag"
    FAIL=$((FAIL + 1))
  fi
fi

# -------------------------------------------------------- phase 4: install
# Install to a throwaway prefix and drive it with every MVX_* override
# unset, proving the binaries locate the runtime, drivers, and system
# account relative to themselves.
if [ "$QUICK" = 0 ]; then
  echo "== install"
  IPFX="$TESTROOT/prefix"
  rm -rf "$IPFX"
  if cmake --install "$ROOT/build" --prefix "$IPFX" >/dev/null 2>&1; then
    IACCT="$TESTROOT/iacct"
    rm -rf "$IACCT"; mkdir -p "$IACCT"
    prog="$IACCT/hi.b"
    printf 'OPEN "PARTS" TO F ELSE STOP\nWRITE "Widget":@AM:"9.99" ON F,"W1"\nREAD R FROM F,"W1" THEN PRINT "installed:":R<1>\n' > "$prog"
    out="$(
      env -u MVXSYSTEM -u MVXDRIVERS -u MVXBIN -u MVXSESSION -u MVXACCOUNT \
          -u DYLD_LIBRARY_PATH -u LD_LIBRARY_PATH sh -c '
        P="$1"; A="$2"; PR="$3"
        "$P/bin/mvx" -a "$A" -c "CREATE-FILE VOC"   >/dev/null 2>&1
        "$P/bin/mvx" -a "$A" -c "CREATE-FILE PARTS" >/dev/null 2>&1
        "$P/bin/mvx-basic" "$PR" -o "$A/hi" 2>&1 || { echo COMPILE-FAIL; exit 0; }
        (cd "$A" && MVXACCOUNT=. ./hi)
      ' _ "$IPFX" "$IACCT" "$prog" 2>&1)"
    if [ "$out" = "installed:Widget" ]; then
      PASS=$((PASS + 1)); echo "  install ok (self-locating, no MVX_* vars)"
    else
      FAIL=$((FAIL + 1)); echo "FAIL install: $out"
    fi
  else
    FAIL=$((FAIL + 1)); echo "FAIL install: cmake --install failed"
  fi
fi

# ---- the byte representation has one door -----------------------------------
#
# An mv_string is its bytes today, and is meant to stop being that: a dynamic
# array becomes an element structure with the flat bytes as a cache
# (DESIGN-DYNAMIC-ARRAYS.md).  On that day every ->data read behind the
# runtime's back returns STALE BYTES -- silently, and only for values that were
# edited by subscript first.
#
# The accessors went in while both sides were still `st->data`, which is the
# only moment that change is provably behaviour-neutral.  This is what keeps
# them in: a rule nothing checks is a rule that decays, and this one has to hold
# across six files that have no other reason to agree with each other.
echo "== byte accessor discipline"
stray=$(grep -rn -- '->data' "$ROOT"/runtime/src/*.c 2>/dev/null \
        | grep -v '^.*mv_str\.c:' || true)
if [ -n "$stray" ]; then
  FAIL=$((FAIL + 1))
  echo "FAIL ->data outside mv_str.c -- use mv_str_bytes()/mv_str_wbytes():"
  echo "$stray" | sed 's|^|    |'
else
  PASS=$((PASS + 1)); echo "  no raw ->data outside mv_str.c"
fi

echo "== $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
