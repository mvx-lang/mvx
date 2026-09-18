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

#pragma once

#include "ast.h"

#include <set>
#include <string>
#include <vector>

namespace mvx {

struct CodegenOptions {
    int  optLevel = 2;
    bool emitLLVM = false;      // write textual IR next to the object
    // DWARF is emitted always and STRIPPED here when it is not wanted (-g0),
    // rather than skipped as it is generated: debug info is a headline feature
    // and the emission path has to stay the one that is exercised.  One code
    // path to be correct, and -g0 removes the result (mvx#223).
    bool debugInfo = true;
    // Output-line -> source line for DWARF (1-based; empty = identity).
    // Lets $INCLUDE'd programs still map to the right source lines.
    std::vector<int> dwarfLines;
    // Package extension functions: a call to one of these dispatches to the
    // runtime extension registry (mvx_ext_invoke) rather than mvx_call.
    std::set<std::string> extFuncs;
};

// Compile one parsed program to a native object file.
// Throws CompileError on semantic errors.
void compileToObject(const Program &prog, const std::string &outPath,
                     const CodegenOptions &opts);

} // namespace mvx
