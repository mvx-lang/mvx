# Test driver for scripts/pkgver.cmake (mvx#212): prints the chosen version.
#   cmake -DROOT=... -DREPO=... -DSTEM=... -DOVERRIDE=... -DWORK=... -P choose.cmake
include("${ROOT}/scripts/pkgver.cmake")
if(NOT DEFINED OVERRIDE)
  set(OVERRIDE "")
endif()
mvx_choose_version("${REPO}" "${REPO}" "${STEM}" "linux-x86_64-le" "${OVERRIDE}" "${WORK}" v)
message("RESULT=[${v}]")
