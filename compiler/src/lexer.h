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

#include <cstdint>
#include <string>
#include <vector>

namespace mvx {

enum class Tok {
    Eof, Eol,
    Ident, IntLit, FltLit, StrLit,
    // punctuation / operators
    LParen, RParen, LBrack, RBrack, Comma, Semi, Colon,
    Plus, Minus, Star, Slash, Caret,
    Eq,             // '=' — assignment or equality by context
    Ne, Lt, Le, Gt, Ge,
    // keywords
    KwIf, KwThen, KwElse, KwEnd,
    KwFor, KwNext, KwTo, KwStep,
    KwLoop, KwRepeat, KwWhile, KwUntil, KwDo,
    KwDim, KwPrint, KwCrt,
    KwCall, KwSubroutine, KwReturn, KwStop, KwAbort,
    KwGoto, KwGo, KwGosub,
    KwBegin, KwCase, KwLocate, KwInput, KwMat, KwCommon,
    KwOpen, KwRead, KwReadu, KwWrite, KwWriteu, KwDelete, KwRelease,
    KwReadv, KwReadvu, KwWritev, KwWritevu,
    KwMatread, KwMatreadu, KwMatwrite, KwMatwriteu, KwLocked,
    KwMatparse, KwMatbuild, KwUsing,
    KwSelect, KwReadnext, KwFrom, KwOn,
    KwExecute, KwCapturing, KwReturning, KwFormlist, KwEcho,
    KwEquate, KwLit,
    KwAnd, KwOr, KwNot, KwMatches, KwNull, KwContinue, KwExit,
    KwFunction, KwDeffun, KwCalling,
};

struct Token {
    Tok kind;
    int line;
    std::string text;     // identifier / string literal contents
    int64_t ival = 0;
    double fval = 0.0;
};

// Lexes the whole source up front; parser indexes into the token stream.
// Throws CompileError (see parser.h) on malformed tokens.
std::vector<Token> lex(const std::string &src, const std::string &item);

} // namespace mvx
