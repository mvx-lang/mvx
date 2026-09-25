cmake_minimum_required(VERSION 3.24)
project(mvx LANGUAGES C CXX)

# The toolchain's own version, so a package can say which mvx it needs and
# something can check it (#117).  Taken from the git tag, because the tag is
# what a release IS -- a VERSION file would be a second place to forget.  A
# checkout with no tags, or a source tarball, reports 0.0.0-dev, which no
# `requires` range will satisfy: better to say "unknown" out loud than to
# claim a version the binary may not have.
# WHAT VERSION IS THIS BUILD?  What the builder says, then what git says, then
# "we do not know".
#
# A RELEASE MUST NOT HAVE TO ASK GIT (mvx#299).  v0.4.0 published binaries that
# reported 0.0.0-dev, because mvx#203 moved the release build into a container
# that runs as root over a workspace owned by the runner's user -- so git
# refused the repository outright ("detected dubious ownership") and the
# ERROR_QUIET below threw the message away.  The release knows the tag it is
# building; it passes -DMVX_VERSION=x.y.z and git is not consulted at all.  The
# container images never had a chance either way: .dockerignore excludes .git.
set(MVX_VERSION "" CACHE STRING "version to stamp in; empty means ask git")
if(MVX_VERSION)
  message(STATUS "MVX version ${MVX_VERSION} (given)")
else()
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    # --match v*: the release workflow keeps a rolling `dev` tag on develop's
    # HEAD, and without this that tag is what describe finds -- so every develop
    # build would report "unknown" instead of "newer than the last release".
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v*" --dirty
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                    OUTPUT_VARIABLE MVX_GIT_DESC
                    ERROR_VARIABLE MVX_GIT_ERR
                    RESULT_VARIABLE MVX_GIT_RC
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_STRIP_TRAILING_WHITESPACE)
    if(MVX_GIT_DESC MATCHES "^v[0-9]")
      string(REGEX REPLACE "^v" "" MVX_VERSION "${MVX_GIT_DESC}")
    elseif(NOT MVX_GIT_RC EQUAL 0)
      # SAY IT.  This was ERROR_QUIET, so a build that could not work out its
      # own version said nothing and stamped 0.0.0-dev -- which is how a
      # release shipped misreporting itself with every check green.
      message(WARNING
        "git describe could not name a version (rc=${MVX_GIT_RC}): ${MVX_GIT_ERR} -- "
        "pass -DMVX_VERSION=x.y.z to stamp one")
    endif()
  endif()
  if(NOT MVX_VERSION)
    set(MVX_VERSION "0.0.0-dev")
  endif()
  message(STATUS "MVX version ${MVX_VERSION}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The runtime uses glibc extensions (memmem, dladdr).  Without _GNU_SOURCE
# glibc leaves them undeclared, so C89 implicit-declaration rules make them
# return int — truncating the returned 64-bit pointer to a crash.  Set it
# tree-wide; it is a no-op on macOS, where these live in the base headers.
add_compile_definitions(_GNU_SOURCE)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

# --- build options -----------------------------------------------------
# Select which optional components to build.  Core (compiler, runtime,
# shell, dir + lmdb drivers) is always built; because drivers are
# dlopen'd, anything left out simply isn't available at run time — nothing
# above the driver couples to it.  Each new backend adds one flag and one
# guarded block.  MVX_WITH_POSTGRES is tri-state: AUTO builds it when
# libpq is present, ON requires it (hard error if missing), OFF skips it.
option(MVX_MINIMAL "Core only: skip optional backends and the daemon" OFF)
if(MVX_MINIMAL)
  set(_mvx_pg_default OFF)
  set(_mvx_daemon_default OFF)
else()
  set(_mvx_pg_default AUTO)
  set(_mvx_daemon_default ON)
endif()
set(MVX_WITH_POSTGRES "${_mvx_pg_default}" CACHE STRING
  "Postgres driver: AUTO (build if libpq present), ON (require it), OFF")
set_property(CACHE MVX_WITH_POSTGRES PROPERTY STRINGS AUTO ON OFF)
set(MVX_WITH_MONGO "${_mvx_pg_default}" CACHE STRING
  "MongoDB driver: AUTO (build if libmongoc present), ON (require it), OFF")
set_property(CACHE MVX_WITH_MONGO PROPERTY STRINGS AUTO ON OFF)
set(MVX_WITH_SQLITE "${_mvx_pg_default}" CACHE STRING
  "SQLite driver: AUTO (build if sqlite3 present), ON (require it), OFF")
set_property(CACHE MVX_WITH_SQLITE PROPERTY STRINGS AUTO ON OFF)
set(MVX_WITH_MYSQL "${_mvx_pg_default}" CACHE STRING
  "MySQL/MariaDB driver: AUTO (build if a client library is present), ON, OFF")
set_property(CACHE MVX_WITH_MYSQL PROPERTY STRINGS AUTO ON OFF)
option(MVX_WITH_DAEMON "Build the mvx-lmdbd daemon and admin tool"
  ${_mvx_daemon_default})
# Packages built into the system account at build time (#54): always available
# to every compile and process (their EXPORTS + verbs + extension libraries),
# with no LINK-PKG.  Set to "" to exclude them, or add more (semicolon-
# separated) to bundle them into a distribution.
#
# json used to be listed here, which meant JSONENCODE/JSONDECODE existed only
# because the packages/json submodule happened to be checked out.  The codec is
# built into libmvxrt now (#169), so it needs no package and no configuration.
set(MVX_PACKAGES "http" CACHE STRING "Packages built into the system account")

# Packages BUNDLED WITH AN INSTALL, downloaded prebuilt rather than built here
# (mvx#198).  Since #169 the package submodules are gone, which is right -- they
# were pinned and drifting -- but it left an installed mvx with no GIT: the
# toolchain shipped, the package did not, and `scripts/test.sh' fetching them
# for tests helped nobody who had installed mvx.
#
# PREBUILT, NOT BUILT FROM SOURCE.  Each of these publishes an mvx asset already
# compiled against a published toolchain; building them here instead would put
# libgit2 and a compiler on the critical path of every install.
#
# MVPKG IS NOT IN THE LIST AND IS NOT OPTIONAL: it is the package manager, the
# thing that installs everything else afterwards, so it comes with the toolchain
# the way the toolchain comes with its own verbs.  The list is what it would
# otherwise have to fetch first.
set(MVX_BUNDLED_PACKAGES "git" CACHE STRING
    "Published packages downloaded into the system account at install time")
# NOT PINNED (mvx#212).  Empty means the install asks: it lists each package's
# releases and offers the latest stable one plus the newest at each less-stable
# level -- or, with nobody at the terminal, takes the latest stable.  Set one to
# install exactly that version, for a build that has to be reproducible.
# (scripts/test.sh still pins the packages it TESTS against, which is a
# different thing: a test run must not depend on what was released today.)
set(MVX_PACKAGE_VERSION_mvpkg "" CACHE STRING
    "mvpkg release to install; empty = ask at install (latest stable if unattended)")
set(MVX_PACKAGE_VERSION_git "" CACHE STRING
    "mvx-lang/git release to install; empty = ask at install (latest stable if unattended)")
# THE OLD PINS ARE RENAMED, NOT REUSED.  A build directory configured before
# this still holds them in its cache, and there they are indistinguishable from
# a pin someone chose on purpose -- so under the old names every existing build
# would go on installing its stale version and never ask.  Drop them, and say
# what replaced them.
foreach(old MVX_MVPKG_VERSION MVX_PKG_VERSION_git)
  if(DEFINED CACHE{${old}})
    message(WARNING
      "${old} is no longer used (mvx#212): the install now asks which version to "
      "install.  To pin one, set MVX_PACKAGE_VERSION_mvpkg or "
      "MVX_PACKAGE_VERSION_git instead.  Dropped ${old}=${${old}}.")
    unset(${old} CACHE)
  endif()
endforeach()

# The asset triple is decided HERE, at configure time, not in the install script:
# a script run by `cmake --install' has no CMAKE_SYSTEM_* of its own, and the
# answer belongs to the machine being configured either way.  Assets are named
# <pkg>-<ver>-mvx-<os>-<arch>-<endian>.tar.gz.
string(TOLOWER "${CMAKE_SYSTEM_NAME}" MVX_PKG_OS)
set(MVX_PKG_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
  set(MVX_PKG_ENDIAN "be")
else()
  set(MVX_PKG_ENDIAN "le")
endif()

# --- runtime -----------------------------------------------------------
# A SHARED library so its code is mapped once and shared across every mvx
# process (each cataloged verb is its own executable), and so dlopen'd
# subroutine libraries and storage drivers resolve mv_*/mvx_* from the
# single loaded copy — no more force-loading a static archive into each
# executable.
add_library(mvxrt SHARED
  runtime/src/mv_value.c
  runtime/src/mv_dyn.c
  runtime/src/mv_str.c
  runtime/src/mv_conv.c
  runtime/src/mvx_ctx.c
  runtime/src/mvx_map.c
  runtime/src/mvx_json.c
  runtime/src/mvx_doc.c
  runtime/src/mvx_ext.c
  runtime/src/mvx_voc.c
  runtime/src/mvx_logto.c
  runtime/src/mvx_para.c
  runtime/src/mvx_proc.c
  runtime/src/mvx_store.c
  runtime/src/mvx_exec.c
  runtime/src/mvx_perm.c
  runtime/src/mvx_cred.c
  runtime/src/mvx_conn.c
  runtime/src/mvx_msg.c
  runtime/src/mvx_id.c
  runtime/src/mvx_call.c
  runtime/src/mvx_term.c
  runtime/src/mvx_os.c
  runtime/src/mvx_acct.c
  daemon/sha256.c)
target_include_directories(mvxrt PUBLIC runtime/include)

# --- the client library (#289) ------------------------------------------
# One API for a program outside MVX -- and, through mv-connect, a language
# binding -- to use an account: records, the values in them, subroutines and
# sentences.  Separate from mvxrt on purpose: this is the surface with an ABI
# PROMISE, while mvx_runtime.h is internal that happens to be installed, and
# keeping them apart is what lets mv_value keep changing (mvx#183) without
# breaking anything built against mvxc.h.
add_library(mvxc SHARED client/mvxc.c)
target_include_directories(mvxc PUBLIC client/include)
target_link_libraries(mvxc PRIVATE mvxrt)
target_include_directories(mvxrt PRIVATE daemon)   # sha256.h for program identity
# The runtime calls libm (sin/pow/sqrt/... for the numeric intrinsics);
# glibc keeps libm separate, so link it explicitly (a no-op on macOS,
# where it lives in libSystem).
find_library(M_LIBRARY m)
if(M_LIBRARY)
  target_link_libraries(mvxrt PRIVATE ${M_LIBRARY})
endif()
# DECLARE THE dlopen DEPENDENCY (mvx#203).  The runtime dlopen's storage
# drivers, extension libraries and cataloged subroutines.  glibc 2.34 merged
# libdl into libc, so on anything newer this links by accident and the
# omission is invisible; on the glibc 2.28 the release now builds against,
# `dlopen' is in libdl and has to be asked for.  CMAKE_DL_LIBS is empty where
# it is not needed, so this is correct everywhere rather than a special case.
target_link_libraries(mvxrt PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(mvxrt PRIVATE
  MVX_VERSION="${MVX_VERSION}"
  MVX_DRIVER_DIR="${CMAKE_BINARY_DIR}/lib"
  MVX_BIN_DIR="${CMAKE_BINARY_DIR}/bin"
  MVX_SYSTEM_DIR="${CMAKE_BINARY_DIR}/system")
set_target_properties(mvxrt PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --- storage drivers: dlopen'd shared libraries ------------------------
# Each driver links its own backend dependency; compiled programs link
# only libmvxrt.  Runtime symbols (mv_*, mvx_*) resolve from the host
# process at load time.
function(mvx_driver name)
  add_library(mvxdrv_${name} SHARED ${ARGN})
  target_include_directories(mvxdrv_${name} PRIVATE runtime/include)
  set_target_properties(mvxdrv_${name} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    PREFIX "lib")
  if(APPLE)
    target_link_options(mvxdrv_${name} PRIVATE -undefined dynamic_lookup)
  endif()
endfunction()

mvx_driver(dir runtime/src/mvx_drv_dir.c)

find_path(LMDB_INCLUDE_DIR lmdb.h REQUIRED)
find_library(LMDB_LIBRARY lmdb REQUIRED)
mvx_driver(lmdb runtime/src/mvx_drv_lmdb.c)
target_include_directories(mvxdrv_lmdb PRIVATE ${LMDB_INCLUDE_DIR})
target_link_libraries(mvxdrv_lmdb PRIVATE ${LMDB_LIBRARY})

mvx_driver(lmdbnet runtime/src/mvx_drv_lmdbnet.c)

# postgres driver — gated by MVX_WITH_POSTGRES.  libpq is keg-only on
# Homebrew, so also probe its opt prefix.
set(MVX_POSTGRES_ENABLED OFF)
if(NOT MVX_WITH_POSTGRES STREQUAL "OFF")
  find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
    HINTS /opt/homebrew/opt/libpq/include /usr/local/opt/libpq/include
          /usr/include/postgresql)
  find_library(LIBPQ_LIBRARY pq
    HINTS /opt/homebrew/opt/libpq/lib /usr/local/opt/libpq/lib)
  if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
    set(MVX_POSTGRES_ENABLED ON)
  elseif(MVX_WITH_POSTGRES STREQUAL "ON")
    message(FATAL_ERROR
      "MVX_WITH_POSTGRES=ON but libpq (libpq-fe.h / libpq) was not found")
  endif()
endif()
if(MVX_POSTGRES_ENABLED)
  message(STATUS "Building postgres driver against ${LIBPQ_LIBRARY}")
  mvx_driver(postgres runtime/src/mvx_drv_postgres.c)
  target_include_directories(mvxdrv_postgres PRIVATE ${LIBPQ_INCLUDE_DIR})
  target_link_libraries(mvxdrv_postgres PRIVATE ${LIBPQ_LIBRARY})
endif()

# sqlite driver — an embedded SQL backend with the full push-down set.
# Unlike postgres it needs no server, and because an account's files are all
# tables in ONE database file, cross-file JOIN/ORDER BY are always available.
set(MVX_SQLITE_ENABLED OFF)
if(NOT MVX_WITH_SQLITE STREQUAL "OFF")
  find_path(SQLITE3_INCLUDE_DIR sqlite3.h
            HINTS /opt/homebrew/opt/sqlite/include /usr/local/opt/sqlite/include)
  find_library(SQLITE3_LIBRARY NAMES sqlite3
               HINTS /opt/homebrew/opt/sqlite/lib /usr/local/opt/sqlite/lib)
  if(SQLITE3_INCLUDE_DIR AND SQLITE3_LIBRARY)
    set(MVX_SQLITE_ENABLED ON)
    message(STATUS "Building sqlite driver against ${SQLITE3_LIBRARY}")
    mvx_driver(sqlite runtime/src/mvx_drv_sqlite.c)
    target_include_directories(mvxdrv_sqlite PRIVATE ${SQLITE3_INCLUDE_DIR})
    target_link_libraries(mvxdrv_sqlite PRIVATE ${SQLITE3_LIBRARY})
  else()
    message(STATUS "sqlite driver: sqlite3 not found, skipping")
  endif()
endif()

# mysql/mariadb driver — built against the MariaDB Connector/C, which speaks the
# MySQL wire protocol, so it serves BOTH servers at run time.  It is found
# specifically, never libmysqlclient: MySQL 8 dropped my_bool (still used by
# MYSQL_BIND on MariaDB), so accepting either client library means a conditional
# typedef and two build paths that are never tested against each other.  On a
# GitHub runner MySQL's headers are present by default, so a loose search finds
# THOSE -- which is how that break was discovered rather than designed around.
#
# Debian/Ubuntu: libmariadb-dev.  Homebrew: mariadb-connector-c.
set(MVX_MYSQL_ENABLED OFF)
if(NOT MVX_WITH_MYSQL STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MARIADB QUIET IMPORTED_TARGET libmariadb)
  endif()
  if(NOT MARIADB_FOUND)
    find_path(MARIADB_INCLUDE_DIR mysql.h
              HINTS /opt/homebrew/opt/mariadb-connector-c/include/mariadb
                    /usr/local/opt/mariadb-connector-c/include/mariadb
                    /usr/include/mariadb
              NO_DEFAULT_PATH)
    find_library(MARIADB_LIBRARY NAMES mariadb
                 HINTS /opt/homebrew/opt/mariadb-connector-c/lib
                       /usr/local/opt/mariadb-connector-c/lib)
  endif()
  if(MARIADB_FOUND)
    set(MVX_MYSQL_ENABLED ON)
    message(STATUS "Building mysql driver against MariaDB Connector/C (pkg-config)")
    mvx_driver(mysql runtime/src/mvx_drv_mysql.c)
    target_link_libraries(mvxdrv_mysql PRIVATE PkgConfig::MARIADB)
  elseif(MARIADB_INCLUDE_DIR AND MARIADB_LIBRARY)
    set(MVX_MYSQL_ENABLED ON)
    message(STATUS "Building mysql driver against ${MARIADB_LIBRARY}")
    mvx_driver(mysql runtime/src/mvx_drv_mysql.c)
    target_include_directories(mvxdrv_mysql PRIVATE ${MARIADB_INCLUDE_DIR})
    target_link_libraries(mvxdrv_mysql PRIVATE ${MARIADB_LIBRARY})
  else()
    message(STATUS "mysql driver: MariaDB Connector/C not found, skipping")
  endif()
endif()

# mongo driver — gated by MVX_WITH_MONGO.  The mongo-c-driver ships pkg-config
# modules mongoc2 / bson2 (v2 API).
set(MVX_MONGO_ENABLED OFF)
if(NOT MVX_WITH_MONGO STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    # v2 (Homebrew) exposes mongoc2/bson2; v1 (apt libmongoc-dev) libmongoc-1.0.
    pkg_check_modules(MONGOC QUIET IMPORTED_TARGET mongoc2 bson2)
    if(NOT MONGOC_FOUND)
      pkg_check_modules(MONGOC QUIET IMPORTED_TARGET libmongoc-1.0 libbson-1.0)
    endif()
  endif()
  if(MONGOC_FOUND)
    set(MVX_MONGO_ENABLED ON)
  elseif(MVX_WITH_MONGO STREQUAL "ON")
    message(FATAL_ERROR
      "MVX_WITH_MONGO=ON but libmongoc (pkg-config mongoc2/bson2) was not found")
  endif()
endif()
if(MVX_MONGO_ENABLED)
  message(STATUS "Building mongo driver against ${MONGOC_LIBRARIES}")
  mvx_driver(mongo runtime/src/mvx_drv_mongo.c)
  target_link_libraries(mvxdrv_mongo PRIVATE PkgConfig::MONGOC)
endif()

# mqtt message transport (mvx#232) — gated by MVX_WITH_MQTT.  Built as its own
# shared library, dlopen'd by mvx-msgd, so libmosquitto is linked HERE and
# nowhere else: not into the runtime, not into the shell, not into a compiled
# BASIC program.  Absent, messaging still works on one host through the
# built-in loop transport.
set(MVX_MQTT_ENABLED OFF)
if(NOT MVX_WITH_MQTT STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO QUIET IMPORTED_TARGET libmosquitto)
  endif()
  if(NOT MOSQUITTO_FOUND)
    find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h
      HINTS /opt/homebrew/opt/mosquitto/include /usr/local/opt/mosquitto/include)
    find_library(MOSQUITTO_LIBRARY mosquitto
      HINTS /opt/homebrew/opt/mosquitto/lib /usr/local/opt/mosquitto/lib)
    if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
      set(MOSQUITTO_FOUND TRUE)
    endif()
  endif()
  if(MOSQUITTO_FOUND)
    set(MVX_MQTT_ENABLED ON)
  elseif(MVX_WITH_MQTT STREQUAL "ON")
    message(FATAL_ERROR "MVX_WITH_MQTT=ON but libmosquitto was not found")
  else()
    message(STATUS "libmosquitto not found, skipping the mqtt transport")
  endif()
endif()
if(MVX_MQTT_ENABLED)
  add_library(mvxmsg_mqtt SHARED runtime/src/mvx_msg_mqtt.c)
  target_include_directories(mvxmsg_mqtt PRIVATE runtime/include)
  set_target_properties(mvxmsg_mqtt PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib PREFIX "lib")
  if(TARGET PkgConfig::MOSQUITTO)
    target_link_libraries(mvxmsg_mqtt PRIVATE PkgConfig::MOSQUITTO)
    message(STATUS "Building mqtt transport against ${MOSQUITTO_LIBRARIES}")
  else()
    target_include_directories(mvxmsg_mqtt PRIVATE ${MOSQUITTO_INCLUDE_DIR})
    target_link_libraries(mvxmsg_mqtt PRIVATE ${MOSQUITTO_LIBRARY})
    message(STATUS "Building mqtt transport against ${MOSQUITTO_LIBRARY}")
  endif()
endif()

# --- the networked LMDB daemon (optional) ------------------------------
if(MVX_WITH_DAEMON)
  add_executable(mvx-lmdbd daemon/mvxd.c daemon/sha256.c)
  target_include_directories(mvx-lmdbd PRIVATE ${LMDB_INCLUDE_DIR})
  target_link_libraries(mvx-lmdbd PRIVATE ${LMDB_LIBRARY})
  set_target_properties(mvx-lmdbd PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

  # mvx-lmdbd-admin: standalone credential provisioning for the daemon
  # host (libc + vendored SHA-256 only — no MVX runtime, no LMDB).
  add_executable(mvx-lmdbd-admin daemon/mvxd_admin.c daemon/sha256.c)
  set_target_properties(mvx-lmdbd-admin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# --- the session registry (mvx#226) ------------------------------------
# Not gated behind an option and not part of MVX_WITH_DAEMON: it is libc and a
# poll loop with no third-party dependency, so it builds wherever the runtime
# does.  A transport that needs a library arrives later as its own dlopen'd
# driver, and that is what gets an option.
add_executable(mvx-msgd daemon/mvx_msgd.c daemon/msgdrv_loop.c)
target_include_directories(mvx-msgd PRIVATE runtime/include)
# It dlopen's its transport driver, and links nothing else -- see mvxrt above
# for why the dependency has to be spelled out (mvx#203).
target_link_libraries(mvx-msgd PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(mvx-msgd PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- mvx-git-adopt: adopt a plain-git checkout into a native account ----
add_executable(mvx-git-adopt tools/mvx_git_adopt.c)
target_link_libraries(mvx-git-adopt PRIVATE mvxrt ${CMAKE_DL_LIBS})
set_target_properties(mvx-git-adopt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- mvx-doc-migrate: pre-#157 record blobs -> the document form -------
add_executable(mvx-doc-migrate tools/mvx_doc_migrate.c)
target_link_libraries(mvx-doc-migrate PRIVATE mvxrt ${CMAKE_DL_LIBS})
set_target_properties(mvx-doc-migrate PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- mvx-launch: the executable half of a cataloged program (mvx#248) --
# Holds no program code.  A copy of it is published as CATALOG/<name>, and it
# loads CATALOG/<name>.so beside itself -- so a program is compiled once and
# exists on disk once, instead of being built both as an executable and as a
# library.
add_executable(mvx-launch tools/mvx_launch.c)
target_link_libraries(mvx-launch PRIVATE mvxrt ${CMAKE_DL_LIBS})
set_target_properties(mvx-launch PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- classic TCL shell -------------------------------------------------
add_executable(mvx tcl/mvx_tcl.c)
target_link_libraries(mvx PRIVATE mvxrt ${CMAKE_DL_LIBS})
find_library(EDITLINE_LIBRARY edit)
if(EDITLINE_LIBRARY)
  target_compile_definitions(mvx PRIVATE HAVE_EDITLINE)
  target_link_libraries(mvx PRIVATE ${EDITLINE_LIBRARY})
endif()
set_target_properties(mvx PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- system account: the source of truth for standard verbs ------------
# Master VOC lives in the repo as a directory file (system/VOC, one text
# record per verb); the build compiles verbs/*.b into system/CATALOG.
# Accounts hold only local VOC entries and fall back to this.
set(MVX_SYS_DIR ${CMAKE_BINARY_DIR}/system)
file(GLOB MVX_VERB_SOURCES ${CMAKE_SOURCE_DIR}/verbs/*.b)
set(MVX_SYS_VERBS "")
# --catalog, the same way the CATALOG verb publishes anything (mvx#248): a
# main program has to be loadable, so the runtime can run it in an existing
# process instead of forking, and the driver decides what that takes -- one
# file on macOS, where an executable is also loadable, and two where it is
# not.  The verbs need it as much as anything a user catalogs: they are ALL
# main programs, so leaving them out would make a standard verb the one kind
# of EXECUTE that still has to spawn.
#
# BYPRODUCTS rather than a second OUTPUT, because whether there IS a second
# file is the driver's decision and not one CMake can see.
foreach(vsrc ${MVX_VERB_SOURCES})
  get_filename_component(vname ${vsrc} NAME_WE)
  add_custom_command(OUTPUT ${MVX_SYS_DIR}/CATALOG/${vname}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MVX_SYS_DIR}/CATALOG
    COMMAND $<TARGET_FILE:mvx-basic> --catalog ${vsrc}
            -o ${MVX_SYS_DIR}/CATALOG/${vname}
    BYPRODUCTS ${MVX_SYS_DIR}/CATALOG/${vname}${CMAKE_SHARED_LIBRARY_SUFFIX}
    DEPENDS mvx-basic mvx-launch mvxrt mvx_crt mvxdrv_dir mvxdrv_lmdb ${vsrc}
    COMMENT "Cataloging ${vname}")
  list(APPEND MVX_SYS_VERBS ${MVX_SYS_DIR}/CATALOG/${vname})
endforeach()
# Configured packages (MVX_PACKAGES): build each and install into the system
# account (its verbs, extension libs, aggregated EXPORTS), always available.
set(MVX_PKG_INSTALL "")
if(MVX_PACKAGES)
  set(MVX_PKG_INSTALL
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/install-pkgs.sh
            ${MVX_SYS_DIR} ${CMAKE_SOURCE_DIR} ${MVX_PACKAGES})
  message(STATUS "  system packages    : ${MVX_PACKAGES}")
endif()
add_custom_target(system-account ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory ${MVX_SYS_DIR}/VOC
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/system/VOC ${MVX_SYS_DIR}/VOC
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/system/VOC.DICT ${MVX_SYS_DIR}/VOC.DICT
  COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_SOURCE_DIR}/system/.mvx ${MVX_SYS_DIR}/.mvx
  ${MVX_PKG_INSTALL}
  DEPENDS ${MVX_SYS_VERBS} mvx-basic mvxrt mvx_crt mvxdrv_dir mvxdrv_lmdb)

# --- tests -------------------------------------------------------------
add_custom_target(check
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/test.sh
  DEPENDS mvx-basic mvx system-account mvxdrv_dir mvxdrv_lmdb mvx_crt
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  USES_TERMINAL)

# Program-entry object, shipped standalone so shared-library builds never
# pick up a main().  The driver links it explicitly for executables.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/lib/mvx_crt.o
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/lib
  COMMAND ${CMAKE_C_COMPILER} -std=c11 -O2
          -I${CMAKE_SOURCE_DIR}/runtime/include
          -c ${CMAKE_SOURCE_DIR}/runtime/src/mvx_crt.c
          -o ${CMAKE_BINARY_DIR}/lib/mvx_crt.o
  DEPENDS runtime/src/mvx_crt.c runtime/include/mvx_runtime.h)
add_custom_target(mvx_crt ALL DEPENDS ${CMAKE_BINARY_DIR}/lib/mvx_crt.o)

# --- compiler ----------------------------------------------------------
add_executable(mvx-basic
  compiler/src/main.cpp
  compiler/src/preprocess.cpp
  compiler/src/lexer.cpp
  compiler/src/parser.cpp
  compiler/src/codegen.cpp)
target_include_directories(mvx-basic SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(mvx-basic PRIVATE ${LLVM_DEFINITIONS_LIST})
set_target_properties(mvx-basic PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_dependencies(mvx-basic mvxrt mvx_crt)

llvm_map_components_to_libnames(LLVM_LIBS
  core support passes native irreader targetparser)
target_link_libraries(mvx-basic PRIVATE ${LLVM_LIBS})
# The compiler reports the same version as the runtime, without linking it:
# mvx-basic does not depend on libmvxrt and should not start now (#117).
target_compile_definitions(mvx-basic PRIVATE MVX_VERSION="${MVX_VERSION}")
target_include_directories(mvx-basic PRIVATE runtime/include)

# --- install -----------------------------------------------------------
# A relocatable prefix: bin/ holds the executables, lib/ the shared
# runtime and dlopen'd drivers, share/mvx/system the standard verbs.
# Everything is located at run time relative to libmvxrt (mvx_runtime_dir),
# so the whole prefix can be moved as a unit.  Executables reach libmvxrt
# through a loader-relative rpath; the compiler and drivers keep resolving
# ../lib relative to their own path (already the case), and the verbs it
# catalogs get an rpath into this lib on install.  Layout is fixed to
# bin/lib (not GNUInstallDirs) so ../lib always matches the compiler's
# assumption on every platform.
if(APPLE)
  set(MVX_INSTALL_RPATH "@loader_path/../lib")
  set(MVX_INSTALL_RPATH_LIB "@loader_path")
else()
  set(MVX_INSTALL_RPATH "\$ORIGIN/../lib")
  set(MVX_INSTALL_RPATH_LIB "\$ORIGIN")
endif()

# mvx-launch is installed even though nothing invokes it by name: --catalog
# copies it beside a cataloged program, so a tree without it cannot catalog
# on a platform that needs the loader (mvx#248).
set(MVX_INSTALL_BINS mvx mvx-basic mvx-git-adopt mvx-msgd mvx-launch)
if(MVX_WITH_DAEMON)
  list(APPEND MVX_INSTALL_BINS mvx-lmdbd mvx-lmdbd-admin)
endif()

# EVERY INSTALLED BINARY, NOT THREE OF THEM (mvx#297).  The rpath used to name
# mvx, mvx-basic and mvx-git-adopt, so the published v0.4.0 shipped an
# mvx-launch that could not find libmvxrt.so and an mvx-lmdbd that could not
# find liblmdb -- neither would start from an unpacked tarball.  mvx-launch got
# away with it because libmvxrt's constructor puts the runtime directory on the
# loader path for children (runtime/src/mvx_ctx.c) and mvx is what spawns it;
# mvx-lmdbd has no parent to inherit from and simply failed.  A binary we
# install into bin/ resolves ../lib, full stop.
set_target_properties(${MVX_INSTALL_BINS} PROPERTIES
  INSTALL_RPATH "${MVX_INSTALL_RPATH}")
install(TARGETS ${MVX_INSTALL_BINS} RUNTIME DESTINATION bin)

set(MVX_INSTALL_DRIVERS mvxrt mvxc mvxdrv_dir mvxdrv_lmdb mvxdrv_lmdbnet)
if(MVX_MQTT_ENABLED)
  list(APPEND MVX_INSTALL_DRIVERS mvxmsg_mqtt)
endif()
if(MVX_POSTGRES_ENABLED)
  list(APPEND MVX_INSTALL_DRIVERS mvxdrv_postgres)
endif()
if(MVX_MONGO_ENABLED)
  list(APPEND MVX_INSTALL_DRIVERS mvxdrv_mongo)
endif()
if(MVX_SQLITE_ENABLED)
  list(APPEND MVX_INSTALL_DRIVERS mvxdrv_sqlite)
endif()
if(MVX_MYSQL_ENABLED)
  list(APPEND MVX_INSTALL_DRIVERS mvxdrv_mysql)
endif()
# AND THE LIBRARIES BESIDE THEM (mvx#297).  A driver is dlopen'd by absolute
# path, so the loader resolves ITS dependencies with ITS own rpath and not the
# program's -- which is why libmvxdrv_lmdb could not see a liblmdb sitting in
# the same directory, and why libmvxc could not see libmvxrt.  $ORIGIN is that
# directory.
set_target_properties(${MVX_INSTALL_DRIVERS} PROPERTIES
  INSTALL_RPATH "${MVX_INSTALL_RPATH_LIB}")
install(TARGETS ${MVX_INSTALL_DRIVERS} LIBRARY DESTINATION lib)
install(FILES ${CMAKE_BINARY_DIR}/lib/mvx_crt.o DESTINATION lib)

# Public runtime headers, so a native-extension package (build-native.sh or a
# NATIVE manifest) can compile against an installed toolchain instead of an mvx
# source tree: its build finds them at <prefix>/include, the install-prefix
# counterpart of the source tree's runtime/include.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/runtime/include/
        DESTINATION include FILES_MATCHING PATTERN "*.h")

# The client library's header, the one external contract with an ABI promise.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/client/include/
        DESTINATION include FILES_MATCHING PATTERN "*.h")

# --- configuration summary ---------------------------------------------
message(STATUS "")
message(STATUS "MVX components:")
message(STATUS "  postgres driver : ${MVX_POSTGRES_ENABLED}")
message(STATUS "  mongo driver    : ${MVX_MONGO_ENABLED}")
message(STATUS "  lmdbd daemon    : ${MVX_WITH_DAEMON}")
message(STATUS "  mqtt transport  : ${MVX_MQTT_ENABLED}")
message(STATUS "")

set(MVX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
set(MVX_BINARY_DIR ${CMAKE_BINARY_DIR})
configure_file(${CMAKE_SOURCE_DIR}/scripts/install-system.cmake.in
               ${CMAKE_BINARY_DIR}/install-system.cmake @ONLY)
# included by install-system.cmake from its own directory
configure_file(${CMAKE_SOURCE_DIR}/scripts/pkgver.cmake
               ${CMAKE_BINARY_DIR}/pkgver.cmake COPYONLY)
configure_file(${CMAKE_SOURCE_DIR}/scripts/pkgdeps.cmake
               ${CMAKE_BINARY_DIR}/pkgdeps.cmake COPYONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/install-system.cmake)
