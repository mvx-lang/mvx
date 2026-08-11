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

#include "preprocess.h"
#include "parser.h"   // CompileError

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace mvx {

namespace {

namespace fs = std::filesystem;

constexpr int kMaxIncludeDepth = 32;

std::string upper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool identChar(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

// Replace whole-word macro names with their values, leaving string
// literals ("..." and '...') untouched.
std::string substitute(const std::string &line,
                       const std::map<std::string, std::string> &macros) {
    if (macros.empty()) return line;
    std::string out;
    out.reserve(line.size());
    char quote = 0;
    for (size_t i = 0, n = line.size(); i < n;) {
        char c = line[i];
        if (quote) {
            out += c;
            if (c == quote) quote = 0;
            i++;
        } else if (c == '"' || c == '\'') {
            quote = c;
            out += c;
            i++;
        } else if (identStart(c)) {
            size_t j = i;
            while (j < n && identChar(line[j])) j++;
            std::string id = line.substr(i, j - i);
            auto it = macros.find(id);
            out += (it != macros.end()) ? it->second : id;
            i = j;
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

// Symbol state shared across a file and everything it includes.
struct Shared {
    std::set<std::string> defined;
    std::map<std::string, std::string> macros;
    std::ostringstream out;
    std::vector<PPLine> map;
};

std::string readFile(const std::string &path, const std::string &fromItem,
                     int fromLine) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw CompileError(fromItem, fromLine,
                           "cannot open include \"" + path + "\"");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Resolve an $INCLUDE / $INSERT target against the including file's
// directory: "file item" -> dir/file/item (the directory-file layout),
// or a single "item" -> dir/item.  A ".b" suffix is tried as a fallback.
std::string resolveInclude(const fs::path &baseDir, const std::string &tok1,
                           const std::string &tok2) {
    auto hit = [](const fs::path &p) -> std::string {
        if (fs::exists(p)) return p.string();
        fs::path b = p; b += ".b";
        if (fs::exists(b)) return b.string();
        return "";
    };
    if (tok2.empty()) {                 // $INCLUDE item -> same dir as the source
        std::string r = hit(baseDir / tok1);
        return r.empty() ? (baseDir / tok1).string() : r;
    }
    // $INCLUDE file item (R83): `file` is a directory-file.  Try the source's
    // own directory first (co-located), then account-relative — a BP program
    // lives in <account>/BP, so a sibling file like BP.INC hangs off <account>.
    // This mirrors, for the directory-file case, where a real VOC file pointer
    // would send us.  (A future hash-file include would follow the VOC pointer
    // through the store engine instead of assuming the directory layout.)
    if (std::string r = hit(baseDir / tok1 / tok2); !r.empty()) return r;
    if (const char *a = std::getenv("MVXACCOUNT"); a && a[0])
        if (std::string r = hit(fs::path(a) / tok1 / tok2); !r.empty()) return r;
    if (std::string r = hit(baseDir.parent_path() / tok1 / tok2); !r.empty())
        return r;
    return (baseDir / tok1 / tok2).string();       // primary path for the error
}

void run(const std::string &src, const std::string &file, int fixedDwarf,
         int depth, Shared &sh);

void includeFile(const fs::path &baseDir, const std::string &tok1,
                 const std::string &tok2, const std::string &fromItem,
                 int fromLine, int childDwarf, int depth, Shared &sh) {
    if (depth >= kMaxIncludeDepth)
        throw CompileError(fromItem, fromLine, "include nesting too deep");
    std::string path = resolveInclude(baseDir, tok1, tok2);
    std::string text = readFile(path, fromItem, fromLine);
    run(text, path, childDwarf, depth + 1, sh);
}

void run(const std::string &src, const std::string &file, int fixedDwarf,
         int depth, Shared &sh) {
    struct Frame { bool parentActive, active, taken; };
    std::vector<Frame> stack;
    auto active = [&]() { return stack.empty() ? true : stack.back().active; };

    fs::path baseDir = fs::path(file).parent_path();
    std::string label = fs::path(file).filename().string();

    std::istringstream in(src);
    std::string line;
    int lineno = 0;
    auto emit = [&](const std::string &content) {
        sh.out << content << "\n";
        sh.map.push_back({label, lineno, fixedDwarf ? fixedDwarf : lineno});
    };

    while (std::getline(in, line)) {
        lineno++;
        size_t p = 0;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;

        if (p < line.size() && line[p] == '$') {
            size_t d = p + 1;
            while (d < line.size() && (line[d] == ' ' || line[d] == '\t')) d++;
            size_t w = d;
            while (w < line.size() && identChar(line[w])) w++;
            std::string dir = upper(line.substr(d, w - d));

            size_t a = w;
            while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) a++;
            std::string rest = line.substr(a);
            while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ' ||
                                     rest.back() == '\t'))
                rest.pop_back();
            std::string name, value;
            size_t k = 0;
            while (k < rest.size() && rest[k] != ' ' && rest[k] != '\t')
                name += rest[k++];
            while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) k++;
            value = rest.substr(k);

            if (dir == "DEFINE") {
                if (active()) {
                    if (name.empty())
                        throw CompileError(file, lineno, "$DEFINE needs a name");
                    sh.defined.insert(name);
                    if (!value.empty()) sh.macros[name] = value;
                    else sh.macros.erase(name);
                }
            } else if (dir == "UNDEFINE" || dir == "UNDEF") {
                if (active()) { sh.defined.erase(name); sh.macros.erase(name); }
            } else if (dir == "IFDEF" || dir == "IFNDEF") {
                if (name.empty())
                    throw CompileError(file, lineno, "$" + dir + " needs a name");
                bool parent = active();
                bool cond = sh.defined.count(name) > 0;
                if (dir == "IFNDEF") cond = !cond;
                bool on = parent && cond;
                stack.push_back({parent, on, on});
            } else if (dir == "ELSE") {
                if (stack.empty())
                    throw CompileError(file, lineno, "$ELSE without $IFDEF");
                Frame &f = stack.back();
                f.active = f.parentActive && !f.taken;
                if (f.active) f.taken = true;
            } else if (dir == "ENDIF") {
                if (stack.empty())
                    throw CompileError(file, lineno, "$ENDIF without $IFDEF");
                stack.pop_back();
            } else if (dir == "INCLUDE" || dir == "INSERT") {
                if (active()) {
                    // first token of `value` is the item when two are given
                    std::string tok2;
                    for (char c : value) {
                        if (c == ' ' || c == '\t') break;
                        tok2 += c;
                    }
                    if (name.empty())
                        throw CompileError(file, lineno,
                                           "$" + dir + " needs a target");
                    int childDwarf = fixedDwarf ? fixedDwarf : lineno;
                    includeFile(baseDir, name, tok2, file, lineno, childDwarf,
                                depth, sh);
                    // the directive line is replaced by the include, so
                    // it produces no output line of its own
                    continue;
                }
                emit("");                          // inactive: blank it
                continue;
            } else {
                throw CompileError(file, lineno, "unknown directive $" + dir);
            }
            emit("");                              // blank the directive line
            continue;
        }

        if (active()) emit(substitute(line, sh.macros));
        else emit("");                             // blank the inactive line
    }
    if (!stack.empty())
        throw CompileError(file, lineno, "unterminated $IFDEF / $IFNDEF");
}

} // namespace

PPResult preprocess(const std::string &src, const std::string &path,
                    const std::map<std::string, std::string> &predefined) {
    Shared sh;
    for (const auto &kv : predefined) {
        sh.defined.insert(kv.first);
        if (!kv.second.empty()) sh.macros[kv.first] = kv.second;
    }
    run(src, path, 0, 0, sh);
    return {sh.out.str(), std::move(sh.map)};
}

} // namespace mvx
