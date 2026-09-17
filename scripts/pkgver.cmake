# MVX — a native compiler and runtime for Pick/MultiValue BASIC.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only.
#
# pkgver.cmake — which version of a published package to install (mvx#212).
#
# The install used to take a PINNED version of each package it downloads, so
# every package release meant a commit to mvx before anyone could get it -- and
# the pin was wrong between the two.  Instead the install asks: it lists the
# package's GitHub releases, offers the latest stable one plus the newest
# release at each less-stable level, and takes the answer.  Nobody at the
# terminal (CI, the mvx release job, a scripted install) means the latest
# stable.  -DMVX_PACKAGE_VERSION_<pkg>=... still pins one explicitly, for a
# build that has to be reproducible.
#
# Included by install-system.cmake, and by scripts/test.sh, which drives
# mvx_release_choices and mvx_choose_version offline.  Nothing here runs on
# include.
#
# Testability switches (environment):
#   MVX_INSTALL_PROMPT=always|never   force the prompt on or off; otherwise it
#                                     is shown only when stdin is a terminal.
#   MVX_INSTALL_RELEASES=<dir>        read <dir>/<repo>.json instead of asking
#                                     GitHub.

# POLICIES ARE SET HERE, not inherited.  This file runs inside `cmake --install'
# and `cmake -P', where no project has set any, so an older CMake applies the
# OLD behaviour of every policy -- and under that, if(IN_LIST) is an unknown
# argument and a quoted "stable" is dereferenced as the variable `stable'.  A
# recent CMake has no OLD behaviour for those, which is how this passed on a
# Mac and failed on Ubuntu's 3.28.  Functions keep the policies in force when
# they are DEFINED, so setting them around the definitions is enough, and
# PUSH/POP leaves the including script as it was.
cmake_policy(PUSH)
cmake_policy(VERSION 3.24)

# The order less-stable levels are offered in, most stable first.  A level not
# named here is offered after these, alphabetically; `dev' is always last.
set(MVX_PKGVER_LEVELS rc beta alpha)

# _mvx_pkgver_parse(<tag> <prerelease> <out-level> <out-base> <out-num>)
#
#   2.0.4                  -> stable  2.0.4   0
#   2.1.0-rc3 / 2.1.0-rc.3 -> rc      2.1.0   3
#   dev                    -> dev
#   anything else          -> ""  (not offered)
#
# A plain version GitHub marks as a pre-release is level `pre': the release
# said it is not stable, and that wins over the shape of the tag.
function(_mvx_pkgver_parse tag pre olevel obase onum)
  set(level "")
  set(base "")
  set(num 0)
  if(tag STREQUAL "dev")
    set(level dev)
  elseif(tag MATCHES "^([0-9]+(\\.[0-9]+)*)$")
    set(base "${CMAKE_MATCH_1}")
    if(pre)
      set(level pre)
    else()
      set(level stable)
    endif()
  elseif(tag MATCHES "^([0-9]+(\\.[0-9]+)*)-([A-Za-z]+)\\.?([0-9]*)$")
    set(base "${CMAKE_MATCH_1}")
    string(TOLOWER "${CMAKE_MATCH_3}" level)
    if(NOT CMAKE_MATCH_4 STREQUAL "")
      set(num "${CMAKE_MATCH_4}")
    endif()
  endif()
  set(${olevel} "${level}" PARENT_SCOPE)
  set(${obase} "${base}" PARENT_SCOPE)
  set(${onum} "${num}" PARENT_SCOPE)
endfunction()

# mvx_release_choices(<releases-json-file> <asset-stem> <triple> <outvar>)
#
# From a GitHub releases listing, the versions worth offering, best first, as a
# list of "<tag>|<level>":
#   1. the latest stable release;
#   2. for each less-stable level, its newest release NEWER than that stable
#      one (a 2.0.0-rc6 is older than 2.0.4 and is not offered);
#   3. dev.
# Only releases carrying an asset for <triple> are considered, so everything
# offered can actually be installed.  Drafts are never offered.
function(mvx_release_choices json stem triple outvar)
  set(${outvar} "" PARENT_SCOPE)
  if(NOT EXISTS "${json}")
    return()
  endif()
  file(READ "${json}" doc)
  string(JSON n ERROR_VARIABLE jerr LENGTH "${doc}")
  if(jerr)
    return()
  endif()

  set(stable "")
  set(has_dev FALSE)
  set(levels "")
  math(EXPR last "${n} - 1")
  foreach(i RANGE 0 ${last})
    string(JSON tag  ERROR_VARIABLE e1 GET "${doc}" ${i} tag_name)
    string(JSON pre  ERROR_VARIABLE e2 GET "${doc}" ${i} prerelease)
    string(JSON drft ERROR_VARIABLE e3 GET "${doc}" ${i} draft)
    if(e1 OR drft)
      continue()
    endif()

    # an asset for this platform, or it is not a choice
    set(want "${stem}-${tag}-mvx-${triple}.tar.gz")
    set(found FALSE)
    string(JSON na ERROR_VARIABLE e4 LENGTH "${doc}" ${i} assets)
    if(NOT e4 AND na GREATER 0)
      math(EXPR alast "${na} - 1")
      foreach(j RANGE 0 ${alast})
        string(JSON an GET "${doc}" ${i} assets ${j} name)
        if(an STREQUAL want)
          set(found TRUE)
          break()
        endif()
      endforeach()
    endif()
    if(NOT found)
      continue()
    endif()

    _mvx_pkgver_parse("${tag}" "${pre}" level base num)
    if(level STREQUAL "")
      continue()
    elseif(level STREQUAL "dev")
      set(has_dev TRUE)
    elseif(level STREQUAL "stable")
      if(stable STREQUAL "" OR base VERSION_GREATER stable)
        set(stable "${base}")
      endif()
    else()
      # newest per level: higher base wins; same base, higher number wins
      if(NOT level IN_LIST levels)
        list(APPEND levels "${level}")
        set(best_${level} "${tag}")
        set(bbase_${level} "${base}")
        set(bnum_${level} "${num}")
      elseif(base VERSION_GREATER bbase_${level}
             OR (base VERSION_EQUAL bbase_${level} AND num GREATER bnum_${level}))
        set(best_${level} "${tag}")
        set(bbase_${level} "${base}")
        set(bnum_${level} "${num}")
      endif()
    endif()
  endforeach()

  set(out "")
  if(NOT stable STREQUAL "")
    list(APPEND out "${stable}|stable")
  endif()

  # known levels in their order, then the rest alphabetically
  set(order "")
  foreach(l ${MVX_PKGVER_LEVELS})
    if(l IN_LIST levels)
      list(APPEND order "${l}")
    endif()
  endforeach()
  set(rest "${levels}")
  if(order)
    list(REMOVE_ITEM rest ${order})
  endif()
  list(SORT rest)
  list(APPEND order ${rest})

  foreach(l ${order})
    # a pre-release is only worth offering if it is newer than the stable one;
    # 2.1.0-rc1 is newer than 2.0.4, 1.24.1-rc1 is not newer than 1.24.1
    if(stable STREQUAL "" OR bbase_${l} VERSION_GREATER stable)
      list(APPEND out "${best_${l}}|${l}")
    endif()
  endforeach()
  if(has_dev)
    list(APPEND out "dev|dev")
  endif()
  set(${outvar} "${out}" PARENT_SCOPE)
endfunction()

# mvx_choose_version(<display> <repo> <asset-stem> <triple> <override> <workdir> <outvar>)
#
# The version to install, or "" to install nothing.  An override (the -D
# variable) is used as given.  Otherwise the choices are listed; with a
# terminal the user picks one (Enter takes the latest stable), and without one
# the latest stable is taken -- or nothing, if there is no stable release,
# because an unattended install must not quietly pick a pre-release.
function(mvx_choose_version name repo stem triple override workdir outvar)
  set(${outvar} "" PARENT_SCOPE)
  if(NOT "${override}" STREQUAL "")
    message(STATUS "  ${name}: ${override} (set at configure time)")
    set(${outvar} "${override}" PARENT_SCOPE)
    return()
  endif()

  file(MAKE_DIRECTORY "${workdir}")
  if(DEFINED ENV{MVX_INSTALL_RELEASES})
    set(json "$ENV{MVX_INSTALL_RELEASES}/${repo}.json")
  else()
    set(json "${workdir}/${repo}.releases.json")
    set(api "https://api.github.com/repos/mvx-lang/${repo}/releases?per_page=100")
    set(hdr HTTPHEADER "Accept: application/vnd.github+json")
    # Unauthenticated API calls are rate-limited per IP, and CI runners share
    # IPs.  A token, when there is one, keeps the listing from failing there.
    if(DEFINED ENV{GITHUB_TOKEN} AND NOT "$ENV{GITHUB_TOKEN}" STREQUAL "")
      list(APPEND hdr HTTPHEADER "Authorization: Bearer $ENV{GITHUB_TOKEN}")
    elseif(DEFINED ENV{GH_TOKEN} AND NOT "$ENV{GH_TOKEN}" STREQUAL "")
      list(APPEND hdr HTTPHEADER "Authorization: Bearer $ENV{GH_TOKEN}")
    endif()
    file(DOWNLOAD "${api}" "${json}" STATUS st TIMEOUT 60 ${hdr})
    list(GET st 0 rc)
    if(NOT rc EQUAL 0)
      list(GET st 1 why)
      message(WARNING
        "mvx: cannot list ${name} releases -- ${why}\n"
        "     ${name} is not installed.  Set the version at configure time to skip\n"
        "     the lookup, or install it later with mvpkg.")
      return()
    endif()
  endif()

  mvx_release_choices("${json}" "${stem}" "${triple}" choices)
  list(LENGTH choices nchoices)
  if(nchoices EQUAL 0)
    message(WARNING "mvx: no ${name} release is published for ${triple}; "
                    "${name} is not installed.")
    return()
  endif()

  list(GET choices 0 first)
  string(REPLACE "|" ";" first "${first}")
  list(GET first 1 firstlevel)

  # prompt only for a person: forced either way, or stdin is a terminal
  set(ask FALSE)
  if("$ENV{MVX_INSTALL_PROMPT}" STREQUAL "always")
    set(ask TRUE)
  elseif(NOT "$ENV{MVX_INSTALL_PROMPT}" STREQUAL "never")
    execute_process(COMMAND sh -c "test -t 0" RESULT_VARIABLE tty)
    if(tty EQUAL 0)
      set(ask TRUE)
    endif()
  endif()

  if(NOT ask)
    if(NOT firstlevel STREQUAL "stable")
      message(WARNING "mvx: ${name} has no stable release for ${triple}; not installing a "
                      "pre-release unattended.  Set the version at configure time to choose one.")
      return()
    endif()
    list(GET first 0 v)
    message(STATUS "  ${name}: ${v} (latest stable)")
    set(${outvar} "${v}" PARENT_SCOPE)
    return()
  endif()

  message(NOTICE "\n${name} -- which version?")
  set(k 0)
  foreach(c ${choices})
    math(EXPR k "${k} + 1")
    string(REPLACE "|" ";" c "${c}")
    list(GET c 0 tag)
    list(GET c 1 level)
    if(level STREQUAL "stable")
      set(note "latest stable")
    elseif(level STREQUAL "dev")
      set(note "nightly, rebuilt in place")
    else()
      set(note "")
    endif()
    if(k EQUAL 1)
      set(note "${note}  [default]")
    endif()
    string(LENGTH "${tag}" tl)
    math(EXPR pad "14 - ${tl}")
    if(pad LESS 1)
      set(pad 1)
    endif()
    string(REPEAT " " ${pad} sp)
    string(LENGTH "${level}" ll)
    math(EXPR pad2 "9 - ${ll}")
    if(pad2 LESS 1)
      set(pad2 1)
    endif()
    string(REPEAT " " ${pad2} sp2)
    message(NOTICE "  ${k}) ${tag}${sp}${level}${sp2}${note}")
  endforeach()
  message(NOTICE "  s) skip -- do not install ${name}")

  foreach(attempt 1 2 3)
    execute_process(
      COMMAND sh -c "printf '%s' \"$1\" >&2; read -r a || a=; printf '%s' \"$a\"" _ "Choice [1]: "
      OUTPUT_VARIABLE ans)
    string(STRIP "${ans}" ans)
    if(ans STREQUAL "")
      set(ans 1)
    endif()
    if(ans STREQUAL "s" OR ans STREQUAL "S")
      message(STATUS "  ${name}: skipped")
      return()
    endif()
    if(ans MATCHES "^[0-9]+$" AND ans GREATER 0 AND NOT ans GREATER nchoices)
      math(EXPR idx "${ans} - 1")
      list(GET choices ${idx} pick)
      string(REPLACE "|" ";" pick "${pick}")
      list(GET pick 0 v)
      message(STATUS "  ${name}: ${v}")
      set(${outvar} "${v}" PARENT_SCOPE)
      return()
    endif()
    message(NOTICE "  '${ans}' is not one of the choices.")
  endforeach()
  message(WARNING "mvx: no valid choice for ${name}; not installing it.")
endfunction()

cmake_policy(POP)
