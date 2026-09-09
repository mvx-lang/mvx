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

#include "lexer.h"
#include "parser.h"   // CompileError

#include <cctype>
#include <unordered_map>

namespace mvx {

static const std::unordered_map<std::string, Tok> kKeywords = {
    {"IF", Tok::KwIf},       {"THEN", Tok::KwThen},   {"ELSE", Tok::KwElse},
    {"END", Tok::KwEnd},     {"FOR", Tok::KwFor},     {"NEXT", Tok::KwNext},
    {"TO", Tok::KwTo},       {"STEP", Tok::KwStep},   {"LOOP", Tok::KwLoop},
    {"REPEAT", Tok::KwRepeat}, {"WHILE", Tok::KwWhile}, {"UNTIL", Tok::KwUntil},
    {"DO", Tok::KwDo},       {"DIM", Tok::KwDim},     {"PRINT", Tok::KwPrint},
    {"CRT", Tok::KwCrt},     {"CALL", Tok::KwCall},
    {"SUBROUTINE", Tok::KwSubroutine},                {"RETURN", Tok::KwReturn},
    {"STOP", Tok::KwStop},   {"ABORT", Tok::KwAbort},   {"GOTO", Tok::KwGoto},   {"GO", Tok::KwGo},
    {"GOSUB", Tok::KwGosub}, {"BEGIN", Tok::KwBegin}, {"CASE", Tok::KwCase},
    {"LOCATE", Tok::KwLocate}, {"INPUT", Tok::KwInput}, {"MAT", Tok::KwMat},
    {"COMMON", Tok::KwCommon}, {"COM", Tok::KwCommon},
    {"OPEN", Tok::KwOpen},     {"READ", Tok::KwRead},
    {"READU", Tok::KwReadu},   {"WRITE", Tok::KwWrite},
    {"WRITEU", Tok::KwWriteu}, {"DELETE", Tok::KwDelete},
    {"READV", Tok::KwReadv},   {"READVU", Tok::KwReadvu},
    {"WRITEV", Tok::KwWritev}, {"WRITEVU", Tok::KwWritevu},
    {"MATREAD", Tok::KwMatread},   {"MATREADU", Tok::KwMatreadu},
    {"MATWRITE", Tok::KwMatwrite}, {"MATWRITEU", Tok::KwMatwriteu},
    {"MATPARSE", Tok::KwMatparse}, {"MATBUILD", Tok::KwMatbuild},
    {"USING", Tok::KwUsing},
    {"LOCKED", Tok::KwLocked},
    {"RELEASE", Tok::KwRelease}, {"SELECT", Tok::KwSelect},
    {"READNEXT", Tok::KwReadnext}, {"FROM", Tok::KwFrom},
    {"ON", Tok::KwOn},
    {"EXECUTE", Tok::KwExecute},   {"PERFORM", Tok::KwExecute},
    {"CAPTURING", Tok::KwCapturing}, {"RETURNING", Tok::KwReturning},
    {"FORMLIST", Tok::KwFormlist}, {"ECHO", Tok::KwEcho},
    {"EQUATE", Tok::KwEquate}, {"EQU", Tok::KwEquate},
    {"LITERALLY", Tok::KwLit}, {"LIT", Tok::KwLit},
    {"AND", Tok::KwAnd},     {"OR", Tok::KwOr},
    {"NOT", Tok::KwNot},
    {"MATCHES", Tok::KwMatches}, {"MATCH", Tok::KwMatches},
    {"NULL", Tok::KwNull},
    {"CONTINUE", Tok::KwContinue}, {"EXIT", Tok::KwExit},
    {"FUNCTION", Tok::KwFunction}, {"DEFFUN", Tok::KwDeffun},
    {"CALLING", Tok::KwCalling},
    // word-form comparators normalise to the symbol tokens
    {"EQ", Tok::Eq}, {"NE", Tok::Ne}, {"LT", Tok::Lt},
    {"LE", Tok::Le}, {"GT", Tok::Gt}, {"GE", Tok::Ge},
};

std::vector<Token> lex(const std::string &src, const std::string &item) {
    std::vector<Token> out;
    size_t i = 0, n = src.size();
    int line = 1;
    bool atStmtStart = true;   // '*' / '!' comments only start a statement

    auto push = [&](Tok k) {
        out.push_back({k, line, "", 0, 0.0});
        atStmtStart = (k == Tok::Eol || k == Tok::Semi);
    };

    while (i < n) {
        char c = src[i];

        if (c == '\n') {
            if (!out.empty() && out.back().kind != Tok::Eol) push(Tok::Eol);
            else atStmtStart = true;
            line++; i++;
            continue;
        }
        if (c == '\r' || c == ' ' || c == '\t') { i++; continue; }

        if (atStmtStart && (c == '*' || c == '!')) {           // comment line
            while (i < n && src[i] != '\n') i++;
            continue;
        }

        if (isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && isdigit((unsigned char)src[i + 1]))) {
            size_t start = i;
            bool isFloat = false;
            while (i < n && isdigit((unsigned char)src[i])) i++;
            if (i < n && src[i] == '.' && i + 1 < n &&
                isdigit((unsigned char)src[i + 1])) {
                isFloat = true; i++;
                while (i < n && isdigit((unsigned char)src[i])) i++;
            }
            std::string text = src.substr(start, i - start);
            Token t{isFloat ? Tok::FltLit : Tok::IntLit, line, text, 0, 0.0};
            if (isFloat) t.fval = std::stod(text);
            else         t.ival = std::stoll(text);
            out.push_back(t);
            atStmtStart = false;
            continue;
        }

        if (isalpha((unsigned char)c) || c == '@' || c == '_') {
            size_t start = i;
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '.' ||
                             src[i] == '_' || src[i] == '@' || src[i] == '$'))
                i++;
            std::string word = src.substr(start, i - start);
            // identifiers may contain '.', but a trailing '.' belongs to
            // punctuation, not the name
            while (!word.empty() && word.back() == '.') { word.pop_back(); i--; }
            std::string upper;
            for (char w : word) upper += (char)toupper((unsigned char)w);

            if (upper == "REM" && atStmtStart) {               // REM comment
                while (i < n && src[i] != '\n') i++;
                continue;
            }
            auto kw = kKeywords.find(upper);
            if (kw != kKeywords.end()) push(kw->second);
            else {
                out.push_back({Tok::Ident, line, upper, 0, 0.0});
                atStmtStart = false;
            }
            continue;
        }

        if (c == '"' || c == '\'') {
            char quote = c;
            size_t start = ++i;
            while (i < n && src[i] != quote && src[i] != '\n') i++;
            if (i >= n || src[i] != quote)
                throw CompileError(item, line, "unterminated string literal");
            out.push_back({Tok::StrLit, line, src.substr(start, i - start), 0, 0.0});
            i++;
            atStmtStart = false;
            continue;
        }

        auto two = [&](char a, char b) {
            return c == a && i + 1 < n && src[i + 1] == b;
        };

        // C-style comments — an MVX extension (impossible token
        // sequences in classic code, so no ambiguity).  Newlines inside
        // a block comment still terminate statements: stripping them
        // would quietly invent line continuation.
        if (two('/', '/')) {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (two('/', '*')) {
            i += 2;
            for (;;) {
                if (i >= n)
                    throw CompileError(item, line,
                                       "unterminated /* comment");
                if (src[i] == '*' && i + 1 < n && src[i + 1] == '/') {
                    i += 2;
                    break;
                }
                if (src[i] == '\n') {
                    if (!out.empty() && out.back().kind != Tok::Eol)
                        push(Tok::Eol);
                    else
                        atStmtStart = true;
                    line++;
                }
                i++;
            }
            continue;
        }
        if (two('<', '=') || two('=', '<')) { push(Tok::Le); i += 2; continue; }
        if (two('>', '=') || two('=', '>')) { push(Tok::Ge); i += 2; continue; }
        if (two('<', '>') || two('>', '<')) { push(Tok::Ne); i += 2; continue; }

        switch (c) {
        case '(': push(Tok::LParen); break;
        case ')': push(Tok::RParen); atStmtStart = false; break;
        case '[': push(Tok::LBrack); break;
        case ']': push(Tok::RBrack); atStmtStart = false; break;
        case ',': push(Tok::Comma);  break;
        case ';': push(Tok::Semi);   break;
        case ':': push(Tok::Colon);  break;
        case '+': push(Tok::Plus);   break;
        case '-': push(Tok::Minus);  break;
        case '*': push(Tok::Star);   break;
        case '/': push(Tok::Slash);  break;
        case '^': push(Tok::Caret);  break;
        case '=': push(Tok::Eq);     break;
        case '#': push(Tok::Ne);     break;
        case '<': push(Tok::Lt);     break;
        case '>': push(Tok::Gt);     break;
        default:
            throw CompileError(item, line,
                               std::string("unexpected character '") + c + "'");
        }
        if (c == ')') atStmtStart = false;
        i++;
    }
    if (out.empty() || out.back().kind != Tok::Eol)
        out.push_back({Tok::Eol, line, "", 0, 0.0});
    out.push_back({Tok::Eof, line, "", 0, 0.0});
    return out;
}

} // namespace mvx
