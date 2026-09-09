/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.  There is NO WARRANTY, to
 * the extent permitted by law; see the LICENSE file for details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/* Language-extension packages (#54).
 *
 * A package may ship a native (C) shared library that adds BASIC functions —
 * value-returning intrinsics usable in expressions (X = FOO(a)) as well as via
 * CALL.  The lib advertises itself with a single entry point returning a table
 * of named functions; the runtime dlopen's it (like a storage driver) from the
 * account's LIB/, each linked package's LIB/ (the PACKAGES record), and the
 * system account's LIB/, and dispatches by name.  Binary-only distribution works
 * because the lib is loaded and resolved entirely at run time.
 *
 * A function receives its result slot and its arguments separately:
 *   void fn(mvx_ctx *ctx, mv_value *ret, int32_t argc, mv_value **argv);
 * ret holds the return value (a statement CALL passes a scratch value); the
 * inputs are argv[0..argc).
 */
#ifndef MVX_EXT_H
#define MVX_EXT_H

#include "mvx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mvx_extfn_t)(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                            mv_value **argv);

typedef struct mvx_extfn {
    const char *name;                   /* BASIC-callable name */
    int32_t minargs, maxargs;           /* input arity (excludes ret) */
    mvx_extfn_t fn;
} mvx_extfn;

typedef struct mvx_ext {
    const char *name;                   /* extension / package name */
    int32_t nfns;
    const mvx_extfn *fns;
} mvx_ext;

/* Every extension library exports exactly one entry point; it returns NULL if
   `abi` is not one it supports, otherwise its function table. */
#define MVX_EXT_ABI 1
typedef const mvx_ext *(*mvx_ext_entry_fn)(int abi);
const mvx_ext *mvx_ext_entry(int abi);

/* Built-in extension tables: compiled into libmvxrt rather than dlopen'd, so
   their functions exist with no package installed and nothing to link (#169).
   Each provider returns its table; mvx_ext.c registers them before any library
   is loaded, so a package cannot shadow a built-in name. */
const mvx_ext *mvx_json_builtin(void);  /* JSONENCODE / JSONDECODE */

/* Runtime side (mvx_ext.c), linked into libmvxrt. */
void mvx_ext_load_libs(void);           /* dlopen package LIB/ libs (shared with CALL) */
int  mvx_ext_has(const char *name);     /* is `name` a registered extension function? */
void mvx_ext_invoke(mvx_ctx *ctx, const char *name, mv_value *ret,
                    int32_t argc, mv_value **argv);   /* fatal on unknown / bad arity */

#ifdef __cplusplus
}
#endif
#endif /* MVX_EXT_H */
