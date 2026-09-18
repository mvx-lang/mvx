# MVX — a native compiler and runtime for Pick/MultiValue BASIC.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only.
#
# pkgdeps.cmake — a published package's own dependencies, and whether its
# native libraries can load here (mvx#218, mvx#219).
#
# The install fetches mvpkg, and mvpkg cannot reach the registry without
# HTTPS -- which it gets from `curl', a package it DECLARES and the install did
# not read.  So the install now reads the manifest it just unpacked, resolves
# each required dependency through the registry, and installs it the same way.
# Nothing is asked: a dependency comes with the package that needs it, or the
# package does not come either.
#
# And a package whose native libraries cannot load is worse than a missing one
# (#219): the loader opens every library in the system account, so one
# unresolvable .so prints an error on every command a user runs afterwards.
# Checked before anything is copied, and skipped with a warning that names the
# library.
#
# Included by install-system.cmake, and by scripts/test.sh, which drives these
# functions offline.  Nothing here runs on include.
#
# Testability switches (environment):
#   MVPKG_REGISTRY=<url>       the registry to resolve names through.
#   MVX_INSTALL_PKGINFO=<dir>  read <dir>/<name>.json (name with '/' -> '_')
#                              instead of asking the registry.
#   MVX_INSTALL_NOLDD=1        skip the native-library check.

# Policies, for the same reason pkgver.cmake sets them: this file runs inside
# `cmake --install' and `cmake -P', where no project has set any.
cmake_policy(PUSH)
cmake_policy(VERSION 3.24)

# mvx_dep_parse(<spec> <system> <o-name> <o-constraint> <o-optional> <o-applies>)
#
# The dependency grammar, as MVPKG itself parses it (mv_package,
# MVPKG.INSTALL/DEPAPPLY) -- stated in one other place, so the two must agree:
#
#   [?]name[@sys,sys|@!sys,sys][:constraint]
#
#   ?           optional: install if available, do not fail if not.
#   @sys,sys    only those systems;  @!sys,sys   every system except those.
#   :^1.2       a version constraint (^ ~ >= <= > < =, or 1.2.*).
#
# "curl"                  -> curl, applies everywhere
# "mvx-lang/json@!mvx:^1.5" -> json, NOT on mvx (it is built into the runtime)
# "?mvx-lang/cmd"         -> optional, so the install leaves it alone
function(mvx_dep_parse spec system oname ocons oopt oappl)
  set(optional 0)
  set(d "${spec}")
  string(STRIP "${d}" d)
  if(d MATCHES "^\\?(.*)$")
    set(optional 1)
    set(d "${CMAKE_MATCH_1}")
  endif()
  set(cons "")
  string(FIND "${d}" ":" colon)
  if(colon GREATER -1)
    string(SUBSTRING "${d}" 0 ${colon} base)
    math(EXPR after "${colon} + 1")
    string(SUBSTRING "${d}" ${after} -1 cons)
    set(d "${base}")
  endif()
  set(applies 1)
  string(FIND "${d}" "@" at)
  if(at GREATER -1)
    string(SUBSTRING "${d}" 0 ${at} name)
    math(EXPR after "${at} + 1")
    string(SUBSTRING "${d}" ${after} -1 filt)
    set(negate 0)
    if(filt MATCHES "^!(.*)$")
      set(negate 1)
      set(filt "${CMAKE_MATCH_1}")
    endif()
    string(REPLACE "," ";" systems "${filt}")
    set(hit 0)
    foreach(s ${systems})
      if(s STREQUAL system)
        set(hit 1)
      endif()
    endforeach()
    if(negate)
      if(hit)
        set(applies 0)
      else()
        set(applies 1)
      endif()
    else()
      set(applies ${hit})
    endif()
    set(d "${name}")
  endif()
  set(${oname} "${d}" PARENT_SCOPE)
  set(${ocons} "${cons}" PARENT_SCOPE)
  set(${oopt} "${optional}" PARENT_SCOPE)
  set(${oappl} "${applies}" PARENT_SCOPE)
endfunction()

# mvx_dep_list(<manifest.json> <system> <out>)
#
# The required dependencies of a package manifest that apply to <system>, in
# declared order.  Optional ones and ones filtered out by @system are dropped
# here rather than by the caller, so "which dependencies does this package
# need on mvx" has one answer.
function(mvx_dep_list manifest system out)
  set(deps "")
  if(NOT EXISTS "${manifest}")
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  file(READ "${manifest}" js)
  string(JSON n ERROR_VARIABLE e LENGTH "${js}" dependencies)
  if(e OR n LESS 1)
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  math(EXPR last "${n} - 1")
  foreach(i RANGE ${last})
    string(JSON spec ERROR_VARIABLE e2 GET "${js}" dependencies ${i})
    if(NOT e2 AND NOT spec STREQUAL "")
      mvx_dep_parse("${spec}" "${system}" dname dcons dopt dappl)
      if(dappl AND NOT dopt)
        list(APPEND deps "${dname}|${dcons}")
      endif()
    endif()
  endforeach()
  set(${out} "${deps}" PARENT_SCOPE)
endfunction()

# _mvx_ver_nums(<version> <out>)  "1.4.0" -> 1;4;0, pre-release -> ""
function(_mvx_ver_nums v out)
  if(v MATCHES "^v?([0-9]+(\\.[0-9]+)*)$")
    string(REPLACE "." ";" nums "${CMAKE_MATCH_1}")
    set(${out} "${nums}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

# _mvx_ver_cmp(<a> <b> <out>)  -1 / 0 / 1, comparing element by element
function(_mvx_ver_cmp a b out)
  _mvx_ver_nums("${a}" an)
  _mvx_ver_nums("${b}" bn)
  set(r 0)
  foreach(i RANGE 3)
    set(x 0)
    set(y 0)
    list(LENGTH an alen)
    list(LENGTH bn blen)
    if(i LESS alen)
      list(GET an ${i} x)
    endif()
    if(i LESS blen)
      list(GET bn ${i} y)
    endif()
    if(r EQUAL 0)
      if(x LESS y)
        set(r -1)
      elseif(x GREATER y)
        set(r 1)
      endif()
    endif()
  endforeach()
  set(${out} "${r}" PARENT_SCOPE)
endfunction()

# mvx_ver_satisfies(<version> <constraint> <out>)
#
# The subset of the constraint syntax MV manifests actually use: ^ (same
# leading non-zero), ~ (same major.minor), >= > <= < =, a bare version, an
# x.y.* wildcard, and a comma-separated list all of which must hold.  A
# constraint this does not understand is not silently treated as satisfied --
# it returns 0, and the caller says so.
function(mvx_ver_satisfies version constraint out)
  if(constraint STREQUAL "")
    set(${out} 1 PARENT_SCOPE)
    return()
  endif()
  _mvx_ver_nums("${version}" vn)
  if(vn STREQUAL "")
    set(${out} 0 PARENT_SCOPE)   # a pre-release never satisfies a constraint here
    return()
  endif()
  string(REPLACE "," ";" parts "${constraint}")
  set(ok 1)
  foreach(p ${parts})
    string(STRIP "${p}" p)
    set(hit 0)
    if(p MATCHES "^\\^(.+)$")
      set(base "${CMAKE_MATCH_1}")
      _mvx_ver_cmp("${version}" "${base}" c)
      _mvx_ver_nums("${base}" bn)
      list(GET bn 0 bmaj)
      list(GET vn 0 vmaj)
      if(NOT c LESS 0 AND vmaj EQUAL bmaj)
        set(hit 1)
      endif()
    elseif(p MATCHES "^~(.+)$")
      set(base "${CMAKE_MATCH_1}")
      _mvx_ver_cmp("${version}" "${base}" c)
      _mvx_ver_nums("${base}" bn)
      list(LENGTH bn blen)
      list(GET bn 0 bmaj)
      list(GET vn 0 vmaj)
      set(minor_ok 1)
      if(blen GREATER 1)
        list(GET bn 1 bmin)
        list(LENGTH vn vlen)
        set(vmin 0)
        if(vlen GREATER 1)
          list(GET vn 1 vmin)
        endif()
        if(NOT vmin EQUAL bmin)
          set(minor_ok 0)
        endif()
      endif()
      if(NOT c LESS 0 AND vmaj EQUAL bmaj AND minor_ok)
        set(hit 1)
      endif()
    elseif(p MATCHES "^>=(.+)$")
      _mvx_ver_cmp("${version}" "${CMAKE_MATCH_1}" c)
      if(NOT c LESS 0)
        set(hit 1)
      endif()
    elseif(p MATCHES "^<=(.+)$")
      _mvx_ver_cmp("${version}" "${CMAKE_MATCH_1}" c)
      if(NOT c GREATER 0)
        set(hit 1)
      endif()
    elseif(p MATCHES "^>(.+)$")
      _mvx_ver_cmp("${version}" "${CMAKE_MATCH_1}" c)
      if(c GREATER 0)
        set(hit 1)
      endif()
    elseif(p MATCHES "^<(.+)$")
      _mvx_ver_cmp("${version}" "${CMAKE_MATCH_1}" c)
      if(c LESS 0)
        set(hit 1)
      endif()
    elseif(p MATCHES "^=?(.*)\\*$")
      set(pfx "${CMAKE_MATCH_1}")
      if(version MATCHES "^${pfx}")
        set(hit 1)
      endif()
    elseif(p MATCHES "^=?([0-9].*)$")
      _mvx_ver_cmp("${version}" "${CMAKE_MATCH_1}" c)
      if(c EQUAL 0)
        set(hit 1)
      endif()
    endif()
    if(NOT hit)
      set(ok 0)
    endif()
  endforeach()
  set(${out} "${ok}" PARENT_SCOPE)
endfunction()

# mvx_pick_version(<versions> <constraint> <out>)
#
# The newest STABLE version in the registry's space-separated list that
# satisfies the constraint.  Newest first is the registry's order, but this
# does not rely on it -- it compares.  Empty when nothing satisfies.
function(mvx_pick_version versions constraint out)
  string(REPLACE " " ";" vl "${versions}")
  set(best "")
  foreach(v ${vl})
    string(STRIP "${v}" v)
    _mvx_ver_nums("${v}" n)
    if(NOT n STREQUAL "")            # stable only: no -rc, no dev
      mvx_ver_satisfies("${v}" "${constraint}" ok)
      if(ok)
        if(best STREQUAL "")
          set(best "${v}")
        else()
          _mvx_ver_cmp("${v}" "${best}" c)
          if(c GREATER 0)
            set(best "${v}")
          endif()
        endif()
      endif()
    endif()
  endforeach()
  set(${out} "${best}" PARENT_SCOPE)
endfunction()

# mvx_registry_url(<out>)
function(mvx_registry_url out)
  set(u "$ENV{MVPKG_REGISTRY}")
  if(u STREQUAL "")
    set(u "https://packages.mvx-lang.org")
  endif()
  string(REGEX REPLACE "/+$" "" u "${u}")
  set(${out} "${u}" PARENT_SCOPE)
endfunction()

# mvx_registry_resolve(<name> <triple> <system> <workdir> <o-json>)
#
# Ask the registry what <name> is on this platform.  The registry resolves a
# PROVIDED name (`curl' -> mvx-lang/curl, or curl-cmd where that is what fits)
# and answers with the concrete package, its version, its published tarball and
# its own dependencies -- which is why resolution belongs there and not here.
# The answer is returned verbatim as JSON; empty when the registry could not be
# reached or does not know the name.
# An exact version may be named as a sixth argument, for a constraint the
# registry's own pick does not satisfy.
function(mvx_registry_resolve name triple system workdir ojson)
  set(${ojson} "" PARENT_SCOPE)
  set(want "")
  if(ARGC GREATER 5)
    list(GET ARGV 5 want)
  endif()
  if(NOT triple MATCHES "^([^-]+)-(.+)-([^-]+)$")
    return()
  endif()
  set(os "${CMAKE_MATCH_1}")
  set(arch "${CMAKE_MATCH_2}")
  set(endian "${CMAKE_MATCH_3}")

  set(offline "$ENV{MVX_INSTALL_PKGINFO}")
  if(NOT offline STREQUAL "")
    string(REPLACE "/" "_" stem "${name}")
    if(EXISTS "${offline}/${stem}.json")
      file(READ "${offline}/${stem}.json" js)
      set(${ojson} "${js}" PARENT_SCOPE)
    endif()
    return()
  endif()

  mvx_registry_url(base)
  file(MAKE_DIRECTORY "${workdir}")
  string(REPLACE "/" "_" stem "${name}")
  set(dst "${workdir}/reg-${stem}.json")
  set(q "${base}/package/${name}?system=${system}&os=${os}&arch=${arch}&endian=${endian}")
  if(NOT want STREQUAL "")
    set(q "${q}&version=${want}")
  endif()
  file(DOWNLOAD "${q}" "${dst}" STATUS st TIMEOUT 60)
  list(GET st 0 rc)
  if(NOT rc EQUAL 0)
    return()
  endif()
  file(READ "${dst}" js)
  string(JSON v ERROR_VARIABLE e GET "${js}" version)
  if(e)
    return()                       # an error body is not a package
  endif()
  set(${ojson} "${js}" PARENT_SCOPE)
endfunction()

# mvx_json_field(<json> <key> <out>)  — "" when absent, never an error
function(mvx_json_field js key out)
  string(JSON v ERROR_VARIABLE e GET "${js}" "${key}")
  if(e)
    set(v "")
  endif()
  set(${out} "${v}" PARENT_SCOPE)
endfunction()

# mvx_native_missing(<dir> <prefix> <out>)
#
# The shared libraries under <dir> that this machine cannot resolve, as
# "lib.so: missing.so.1" lines (mvx#219).  Linux only -- `ldd' is what reports
# it, and the published packages that carry native code are Linux ones.
#
# LD_LIBRARY_PATH covers the two places a dependency legitimately lives: the
# install's own lib (libmvxrt) and the package's own LIB.  Anything still
# unresolved is genuinely absent from the machine, which is the question.
function(mvx_native_missing dir prefix out)
  set(${out} "" PARENT_SCOPE)
  if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    return()
  endif()
  if(NOT "$ENV{MVX_INSTALL_NOLDD}" STREQUAL "")
    return()
  endif()
  find_program(MVX_LDD ldd)
  if(NOT MVX_LDD)
    return()
  endif()
  file(GLOB_RECURSE sos "${dir}/*.so")
  set(missing "")
  foreach(so ${sos})
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env
              "LD_LIBRARY_PATH=${prefix}/lib:${dir}/LIB:$ENV{LD_LIBRARY_PATH}"
              "${MVX_LDD}" "${so}"
      OUTPUT_VARIABLE lddout ERROR_VARIABLE lddout RESULT_VARIABLE lddrc)
    # `not a dynamic executable' and a non-zero rc are not a missing library;
    # only a named "=> not found" is.
    string(REGEX MATCHALL "[^ \t\n]+ => not found" gone "${lddout}")
    foreach(g ${gone})
      string(REGEX REPLACE " => not found" "" g "${g}")
      get_filename_component(sn "${so}" NAME)
      list(APPEND missing "${sn}: ${g}")
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES missing)
  set(${out} "${missing}" PARENT_SCOPE)
endfunction()

cmake_policy(POP)
