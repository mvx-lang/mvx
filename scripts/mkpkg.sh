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
# Build an MVX package: compile BP/* into CATALOG/.  A package is an
# account-shaped directory (BP source, VOC verb records, CATALOG
# executables) that accounts link with LINK-PKG.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="${1:?usage: mkpkg.sh <package-directory>}"

# The compiler: a built tree's build/bin/mvx-basic by default, but overridable
# with $MVXBASIC (or an installed mvx-basic on PATH) so a package can be built
# against a setup-mvx toolchain in CI, with no mvx source checkout.
MVXBASIC="${MVXBASIC:-$ROOT/build/bin/mvx-basic}"
[ -x "$MVXBASIC" ] || command -v "$MVXBASIC" >/dev/null 2>&1 || MVXBASIC="$(command -v mvx-basic || true)"
[ -n "$MVXBASIC" ] || { echo "mkpkg: no mvx-basic (set \$MVXBASIC or put it on PATH)" >&2; exit 1; }

# A package has BASIC source (BP/) and/or a native language extension (NATIVE,
# e.g. the http package: functions only, no BP programs).
[ -d "$PKG/BP" ] || [ -f "$PKG/NATIVE" ] || {
  echo "mkpkg: $PKG has neither a BP source directory nor a NATIVE manifest" >&2
  exit 1; }
# A subroutine-only package has an empty VOC; git does not track empty
# directories, so recreate it here rather than failing a fresh checkout.
mkdir -p "$PKG/VOC"

case "$(uname)" in
  Darwin) EXT=dylib ; UNDEF="-undefined dynamic_lookup" ;;
  *)      EXT=so ; UNDEF="" ;;
esac

PKGNAME="$(head -1 "$PKG/PKG" 2>/dev/null)"
[ -n "$PKGNAME" ] || PKGNAME="$(basename "$PKG")"

# Dependency EXPORTS visibility.  A package's programs may call functions
# exported by a package it depends on (e.g. mvpkg calls HTTPGET/JSONDECODE
# from http/json).  The compiler reads a linked package's EXPORTS through
# MVXACCOUNT -> PACKAGES, so resolve this package's PKG dependencies (attr
# 5+) to their directories — a sibling of $PKG, or a $MVXPKGPATH segment,
# the same search LINK-PKG uses — and list them in a throwaway PACKAGES.
# The package's own EXPORTS still comes via MVXSYSTEM=$PKG below.
DEPACCT=""
if [ -f "$PKG/PKG" ]; then
  PKGPARENT="$(cd "$PKG/.." && pwd)"
  DEPS="$(awk 'NR>=5 && NF { print $1 }' "$PKG/PKG")"
  if [ -n "$DEPS" ]; then
    DEPACCT="$(mktemp -d "${TMPDIR:-/tmp}/mkpkg.deps.XXXXXX")"
    trap 'rm -rf "$DEPACCT"' EXIT
    : > "$DEPACCT/PACKAGES"
    for dep in $DEPS; do
      # a '?' prefix marks an optional dependency (LINK-PKG semantics): the
      # package bundles a fallback under the same names, so an unresolved one
      # is expected, not a warning.
      opt=""; case "$dep" in \?*) opt=1; dep="${dep#\?}" ;; esac
      depdir=""
      # a sibling of $PKG first, then each $MVXPKGPATH segment
      oldifs=$IFS; IFS=:
      for base in "$PKGPARENT" ${MVXPKGPATH:-}; do
        [ -n "$base" ] || continue
        cand="$base/$dep"
        if [ -f "$cand/EXPORTS" ] || [ -d "$cand/VOC" ] || [ -f "$cand/PKG" ]; then
          depdir="$cand"; break
        fi
      done
      IFS=$oldifs
      if [ -n "$depdir" ]; then
        echo "$depdir" >> "$DEPACCT/PACKAGES"
        echo "  dep $dep -> $depdir (EXPORTS visible)"
      elif [ -z "$opt" ]; then
        echo "  dep $dep: unresolved (searched $PKGPARENT/, \$MVXPKGPATH)" >&2
      fi
    done
  fi
fi

# SUBROUTINE sources bundle into ONE shared library per package (the
# jBASE deployment shape — one dlopen, one artifact); main programs
# become verb executables in CATALOG/.
mkdir -p "$PKG/CATALOG"
rm -rf "$PKG/LIB"

# native subroutine libraries (C using the subroutine ABI): a package
# with build-native.sh builds them into LIB/ before the BASIC subs.
if [ -x "$PKG/build-native.sh" ]; then
  "$PKG/build-native.sh"
fi

# native extension library (#54): a package with a NATIVE manifest (source
# files, plus optional "pkgconfig: <libs>" lines) builds one shared library
# LIB/libmvxext_<name> exposing mvx_ext functions.  Runtime symbols (mv_*,
# mvx_*, the core mapper) resolve from the host at load, exactly like drivers.
# A prebuilt LIB/libmvxext_<name> shipped without a NATIVE manifest is
# binary-only (mkpkg leaves it alone).
if [ -f "$PKG/NATIVE" ]; then
  NSRCS=""; NPKGCONF=""
  while IFS= read -r nline || [ -n "$nline" ]; do
    case "$nline" in
      pkgconfig:*) NPKGCONF="$NPKGCONF ${nline#pkgconfig:}" ;;
      ""|\#*) ;;
      *) NSRCS="$NSRCS $PKG/$nline" ;;
    esac
  done < "$PKG/NATIVE"
  if [ -n "$NSRCS" ]; then
    NCFLAGS=""; NLDFLAGS=""
    if [ -n "$NPKGCONF" ]; then
      # shellcheck disable=SC2086
      NCFLAGS="$(pkg-config --cflags $NPKGCONF 2>/dev/null || true)"
      # shellcheck disable=SC2086
      NLDFLAGS="$(pkg-config --libs $NPKGCONF 2>/dev/null || true)"
    fi
    mkdir -p "$PKG/LIB"
    # shellcheck disable=SC2086
    cc -O2 -fPIC -shared $UNDEF -I"$ROOT/runtime/include" $NCFLAGS $NSRCS $NLDFLAGS \
       -o "$PKG/LIB/libmvxext_$PKGNAME.$EXT"
    echo "  built LIB/libmvxext_$PKGNAME.$EXT (native extension)"
  fi
fi
# PLATFORM.H — per-platform compile-time defines, kept in an include directory-file
# (a package's own BP.INC, or the mvpkg-shared MVPKG.INC), not in BP, and reached
# as `$INCLUDE <name> PLATFORM.H`.  A build artifact (gitignored): on the MVX target
# it is comment-only, since MVX is a builtin compiler define.  Vendor builds
# (build-udt.sh …) and mvpkg's MVPKG INIT write the $DEFINE MV + $DEFINE <vendor>
# lines instead.  MVPKG.INC is provisioned here for the dev/CI build; a real
# mvpkg-managed account gets the shared one from MVPKG INIT.
for INC in BP.INC MVPKG.INC; do
  if grep -rlF -- "\$INCLUDE $INC PLATFORM.H" "$PKG"/BP >/dev/null 2>&1; then
    mkdir -p "$PKG/$INC"
    cat > "$PKG/$INC/PLATFORM.H" <<'PLATH'
* PLATFORM.H — generated by the build for the MVX target.
* MVX is a builtin compiler define, so there is nothing to declare here.
* UniData / UniVerse / D3 builds write their vendor $DEFINE lines in this file.
PLATH
  fi
done
SUBS=""
# Compile BP/ plus any additional <name>.BP source files (e.g. a package's
# bundled fallback subroutines in CMD.BP).  An unmatched *.BP glob stays
# literal and is skipped by the -f guard below.
for src in "$PKG"/BP/* "$PKG"/*.BP/*; do
  [ -f "$src" ] || continue
  name="$(basename "$src")"
  case "$name" in *.H) continue ;; esac   # headers are $INCLUDE'd, not compiled
  # first real statement token, skipping *,!,REM,// line comments,
  # /* */ block comments (so docblocks in either style are ignored) and
  # $ preprocessor directives ($IFDEF/$DEFINE/...) that may precede SUBROUTINE
  first="$(awk '
    { line = $0 }
    inblk { if (line ~ /\*\//) { sub(/.*\*\//, "", line); inblk = 0 } else next }
    { sub(/^[ \t]+/, "", line) }
    line ~ /^\/\*/ { if (line !~ /\*\//) { inblk = 1 }; next }
    line ~ /^(\*|!|\/\/)/ { next }
    line ~ /^[Rr][Ee][Mm]([ \t]|$)/ { next }
    line ~ /^\$/ { next }
    line ~ /^[ \t]*$/ { next }
    { n = split(line, a, /[ \t]/); print a[1]; exit }
  ' "$src")"
  if [ "$first" = "SUBROUTINE" ]; then
    SUBS="$SUBS $src"
    echo "  bundling $name"
  else
    # MVXSYSTEM=$PKG so the compiler sees this package's own EXPORTS — a verb
    # may call the package's extension functions as expressions; MVXACCOUNT
    # (when set) adds the dependencies' EXPORTS resolved above.
    MVXSYSTEM="$PKG" MVXACCOUNT="$DEPACCT" \
      "$MVXBASIC" "$src" -o "$PKG/CATALOG/$name"
    echo "  cataloged $name"
  fi
done
if [ -n "$SUBS" ]; then
  mkdir -p "$PKG/LIB"
  # shellcheck disable=SC2086
  MVXSYSTEM="$PKG" MVXACCOUNT="$DEPACCT" \
    "$MVXBASIC" -shared $SUBS -o "$PKG/LIB/lib$PKGNAME.$EXT"
  echo "  built LIB/lib$PKGNAME.$EXT"
fi

# bin/: standalone OS commands a package ships (built by build-native.sh
# or checked in).  Symlink them beside mvx so they land on the dev PATH;
# an install would copy them into a real bin directory instead.
if [ -d "$PKG/bin" ]; then
  mkdir -p "$ROOT/build/bin"
  for b in "$PKG"/bin/*; do
    [ -f "$b" ] && [ -x "$b" ] || continue
    babs="$(cd "$(dirname "$b")" && pwd)/$(basename "$b")"
    ln -sf "$babs" "$ROOT/build/bin/$(basename "$b")"
    echo "  installed bin/$(basename "$b") -> build/bin/$(basename "$b")"
  done
fi

echo "package built: $PKG"
