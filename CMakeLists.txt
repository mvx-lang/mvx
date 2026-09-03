cmake_minimum_required(VERSION 3.24)
project(mvx LANGUAGES C CXX)

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
# with no LINK-PKG.  Defaults to `json` (the JSON codec is now a package —
# JSONENCODE/JSONDECODE come from packages/json).  Set to "" to exclude it, or
# add more (semicolon-separated) to bundle them into a distribution.
set(MVX_PACKAGES "json;http" CACHE STRING "Packages built into the system account")

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
  runtime/src/mvx_ext.c
  runtime/src/mvx_store.c
  runtime/src/mvx_exec.c
  runtime/src/mvx_perm.c
  runtime/src/mvx_cred.c
  runtime/src/mvx_conn.c
  runtime/src/mvx_call.c
  runtime/src/mvx_term.c
  runtime/src/mvx_os.c
  runtime/src/mvx_acct.c
  daemon/sha256.c)
target_include_directories(mvxrt PUBLIC runtime/include)
target_include_directories(mvxrt PRIVATE daemon)   # sha256.h for program identity
# The runtime calls libm (sin/pow/sqrt/... for the numeric intrinsics);
# glibc keeps libm separate, so link it explicitly (a no-op on macOS,
# where it lives in libSystem).
find_library(M_LIBRARY m)
if(M_LIBRARY)
  target_link_libraries(mvxrt PRIVATE ${M_LIBRARY})
endif()
target_compile_definitions(mvxrt PRIVATE
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

# --- mvx-git-adopt: adopt a plain-git checkout into a native account ----
add_executable(mvx-git-adopt tools/mvx_git_adopt.c)
target_link_libraries(mvx-git-adopt PRIVATE mvxrt)
set_target_properties(mvx-git-adopt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- classic TCL shell -------------------------------------------------
add_executable(mvx tcl/mvx_tcl.c)
target_link_libraries(mvx PRIVATE mvxrt ${CMAKE_DL_LIBS})
find_library(EDITLINE_LIBRARY edit)
if(EDITLINE_LIBRARY)
  target_compile_definitions(mvx PRIVATE HAVE_EDITLINE)
  target_link_libraries(mvx PRIVATE ${EDITLINE_LIBRARY})
endif()
target_compile_definitions(mvx PRIVATE
  MVX_SYSTEM_DIR="${CMAKE_BINARY_DIR}/system")
set_target_properties(mvx PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- system account: the source of truth for standard verbs ------------
# Master VOC lives in the repo as a directory file (system/VOC, one text
# record per verb); the build compiles verbs/*.b into system/CATALOG.
# Accounts hold only local VOC entries and fall back to this.
set(MVX_SYS_DIR ${CMAKE_BINARY_DIR}/system)
file(GLOB MVX_VERB_SOURCES ${CMAKE_SOURCE_DIR}/verbs/*.b)
set(MVX_SYS_VERBS "")
foreach(vsrc ${MVX_VERB_SOURCES})
  get_filename_component(vname ${vsrc} NAME_WE)
  add_custom_command(OUTPUT ${MVX_SYS_DIR}/CATALOG/${vname}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MVX_SYS_DIR}/CATALOG
    COMMAND $<TARGET_FILE:mvx-basic> ${vsrc} -o ${MVX_SYS_DIR}/CATALOG/${vname}
    DEPENDS mvx-basic mvxrt mvx_crt mvxdrv_dir mvxdrv_lmdb ${vsrc}
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
else()
  set(MVX_INSTALL_RPATH "\$ORIGIN/../lib")
endif()
set_target_properties(mvx mvx-basic mvx-git-adopt PROPERTIES
  INSTALL_RPATH "${MVX_INSTALL_RPATH}")

set(MVX_INSTALL_BINS mvx mvx-basic mvx-git-adopt)
if(MVX_WITH_DAEMON)
  list(APPEND MVX_INSTALL_BINS mvx-lmdbd mvx-lmdbd-admin)
endif()
install(TARGETS ${MVX_INSTALL_BINS} RUNTIME DESTINATION bin)

set(MVX_INSTALL_DRIVERS mvxrt mvxdrv_dir mvxdrv_lmdb mvxdrv_lmdbnet)
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
install(TARGETS ${MVX_INSTALL_DRIVERS} LIBRARY DESTINATION lib)
install(FILES ${CMAKE_BINARY_DIR}/lib/mvx_crt.o DESTINATION lib)

# Public runtime headers, so a native-extension package (build-native.sh or a
# NATIVE manifest) can compile against an installed toolchain instead of an mvx
# source tree: its build finds them at <prefix>/include, the install-prefix
# counterpart of the source tree's runtime/include.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/runtime/include/
        DESTINATION include FILES_MATCHING PATTERN "*.h")

# --- configuration summary ---------------------------------------------
message(STATUS "")
message(STATUS "MVX components:")
message(STATUS "  postgres driver : ${MVX_POSTGRES_ENABLED}")
message(STATUS "  mongo driver    : ${MVX_MONGO_ENABLED}")
message(STATUS "  lmdbd daemon    : ${MVX_WITH_DAEMON}")
message(STATUS "")

set(MVX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
set(MVX_BINARY_DIR ${CMAKE_BINARY_DIR})
configure_file(${CMAKE_SOURCE_DIR}/scripts/install-system.cmake.in
               ${CMAKE_BINARY_DIR}/install-system.cmake @ONLY)
install(SCRIPT ${CMAKE_BINARY_DIR}/install-system.cmake)
