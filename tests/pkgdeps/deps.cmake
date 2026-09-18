# Test driver for scripts/pkgdeps.cmake (mvx#218, mvx#219).  One question per
# run, so a failure names itself:
#   cmake -DROOT=... -DWHAT=parse   -DARG=<spec> [-DSYS=mvx]          -P deps.cmake
#   cmake -DROOT=... -DWHAT=deps    -DARG=<manifest.json> [-DSYS=...] -P deps.cmake
#   cmake -DROOT=... -DWHAT=sat     -DARG=<version> -DARG2=<constraint> -P deps.cmake
#   cmake -DROOT=... -DWHAT=pick    -DARG=<versions> -DARG2=<constraint> -P deps.cmake
#   cmake -DROOT=... -DWHAT=native  -DARG=<dir> [-DARG2=<prefix>]      -P deps.cmake
include("${ROOT}/scripts/pkgdeps.cmake")
if(NOT DEFINED SYS)
  set(SYS "mvx")
endif()
if(NOT DEFINED ARG2)
  set(ARG2 "")
endif()

if(WHAT STREQUAL "parse")
  mvx_dep_parse("${ARG}" "${SYS}" n c o a)
  message("RESULT=[${n}|${c}|${o}|${a}]")
elseif(WHAT STREQUAL "deps")
  mvx_dep_list("${ARG}" "${SYS}" d)
  message("RESULT=[${d}]")
elseif(WHAT STREQUAL "sat")
  mvx_ver_satisfies("${ARG}" "${ARG2}" ok)
  message("RESULT=[${ok}]")
elseif(WHAT STREQUAL "pick")
  mvx_pick_version("${ARG}" "${ARG2}" v)
  message("RESULT=[${v}]")
elseif(WHAT STREQUAL "native")
  mvx_native_missing("${ARG}" "${ARG2}" missing)
  message("RESULT=[${missing}]")
else()
  message(FATAL_ERROR "deps.cmake: unknown WHAT=${WHAT}")
endif()
