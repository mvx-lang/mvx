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

// mvx driver: compile MVX BASIC to objects, executables, or shared
// subroutine libraries.
//
//   mvx -c prog.b -o prog.o          compile only
//   mvx prog.b sub.b -o prog         compile and link an executable
//   mvx -shared subs.b -o libsubs    compile and link a shared library
//
// Errors go to stderr as "item:line: message" — parseable; the BASIC verb
// will consume this later, so treat the format as an interface.

#include "codegen.h"
#include "parser.h"
#include "preprocess.h"

#include <map>
#include <set>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path exeDir() {
#ifdef __APPLE__
    char buf[4096];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0)
        return fs::canonical(buf).parent_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    return fs::current_path();
}

// The runtime ships next to the compiler: <root>/bin/mvx, <root>/lib/*.
fs::path runtimeLibDir() {
    fs::path lib = exeDir().parent_path() / "lib";
#ifdef __APPLE__
    const char *rt = "libmvxrt.dylib";
#else
    const char *rt = "libmvxrt.so";
#endif
    if (fs::exists(lib / rt)) return lib;
    std::cerr << "mvx-basic: cannot find runtime library near " << exeDir()
              << "\n";
    exit(1);
}

int usage() {
    std::cerr <<
        "usage: mvx-basic [options] file.b [file.b|file.o ...]\n"
        "  -c           compile to object only (no link)\n"
        "  -o <path>    output path\n"
        "  -shared      produce a shared subroutine library\n"
        "  -D NAME[=v]  define a preprocessor symbol ($IFDEF NAME)\n"
        "  -O0|-O1|-O2  optimisation level (default -O2)\n"
        "  --emit-llvm  also write textual IR beside each object\n";
    return 2;
}

// Collect the names in a package EXPORTS manifest ("NAME MINARGS MAXARGS").
void readExports(const fs::path &exportsFile, std::set<std::string> &out) {
    std::ifstream f(exportsFile);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        std::string name;
        if (is >> name && !name.empty() && name[0] != '#' && name[0] != '*')
            out.insert(name);
    }
}

// The expression functions available at compile time: from the always-on
// system-installed packages (aggregated <system>/EXPORTS) plus, when compiling
// in an account, that account's linked packages (PACKAGES -> each <pkg>/EXPORTS).
// No per-compile flag: inclusion is configuration.
std::set<std::string> loadExtFuncs() {
    std::set<std::string> out;
    fs::path sys;
    if (const char *s = getenv("MVXSYSTEM"); s && *s) sys = s;
    else sys = exeDir().parent_path() / "system";
    readExports(sys / "EXPORTS", out);

    if (const char *acct = getenv("MVXACCOUNT"); acct && *acct) {
        std::ifstream pf(fs::path(acct) / "PACKAGES");
        std::string dir;
        while (std::getline(pf, dir)) {
            while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r' ||
                                    dir.back() == ' '))
                dir.pop_back();
            if (!dir.empty()) readExports(fs::path(dir) / "EXPORTS", out);
        }
    }
    return out;
}

std::string shellQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

int main(int argc, char **argv) {
    bool compileOnly = false, shared = false;
    mvx::CodegenOptions cg;
    std::string outPath;
    std::vector<std::string> sources, objects;
    // Preprocessor symbols: MVX identifies this compiler to portable
    // source ($IFDEF MVX ... $ELSE ... $ENDIF).
    //
    // ENGINE says something different and more useful to portable source: the
    // record-git engine is callable in-process here, as an ordinary
    // subroutine.  That is true of MVX and of jBASE (which reaches C with
    // DEFC and can hand it the session), and false of UniData and UniVerse,
    // where a handler has to do the work in BASIC instead.  A handler that
    // only wants to know "can I just call the engine?" should ask that rather
    // than name a platform -- naming platforms is how jBASE ended up excluded
    // from things it can do (mv_git#114).
    std::map<std::string, std::string> defines;
    defines["MVX"] = "";
    defines["ENGINE"] = "";

    // Package extension functions available to this compile (config, not a flag).
    std::set<std::string> extFuncs = loadExtFuncs();
    cg.extFuncs = extFuncs;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "-c") compileOnly = true;
        else if (a == "-shared") shared = true;
        else if (a == "-o") {
            if (++i >= argc) return usage();
            outPath = argv[i];
        }
        else if (a == "-O0") cg.optLevel = 0;
        else if (a == "-O1") cg.optLevel = 1;
        else if (a == "-O2") cg.optLevel = 2;
        else if (a == "--emit-llvm") cg.emitLLVM = true;
        else if (a.rfind("-D", 0) == 0) {               // -DNAME[=value] or -D NAME
            std::string def = a.size() > 2 ? a.substr(2) : "";
            if (def.empty()) { if (++i >= argc) return usage(); def = argv[i]; }
            auto eq = def.find('=');
            if (eq == std::string::npos) defines[def] = "";
            else defines[def.substr(0, eq)] = def.substr(eq + 1);
        }
        else if (!a.empty() && a[0] == '-') return usage();
        else if (a.size() > 2 && a.substr(a.size() - 2) == ".o")
            objects.push_back(a);
        else sources.push_back(a);
    }
    if (sources.empty() && objects.empty()) return usage();
    if (compileOnly && shared) {
        std::cerr << "mvx-basic: -c and -shared are mutually exclusive\n";
        return 2;
    }
    if (compileOnly && sources.size() > 1 && !outPath.empty()) {
        std::cerr << "mvx-basic: -c with -o takes a single source file\n";
        return 2;
    }

    // Compile each source to an object.
    std::vector<std::string> linkObjects = objects;
    fs::path tmpDir;
    for (const std::string &src : sources) {
        std::ifstream in(src);
        if (!in) {
            std::cerr << "mvx-basic: cannot open " << src << "\n";
            return 1;
        }
        std::stringstream ss;
        ss << in.rdbuf();

        std::string obj;
        if (compileOnly) {
            obj = outPath.empty()
                      ? fs::path(src).stem().string() + ".o"
                      : outPath;
        } else {
            if (tmpDir.empty()) {
                std::string tmpl =
                    (fs::temp_directory_path() / "mvx-XXXXXX").string();
                std::vector<char> buf(tmpl.begin(), tmpl.end());
                buf.push_back('\0');
                if (!mkdtemp(buf.data())) {
                    std::cerr << "mvx-basic: cannot create temp directory\n";
                    return 1;
                }
                tmpDir = buf.data();
            }
            // filename(), not stem(): MV item names contain dots
            // (CMD.ADD, CMD.RUN) and stem() would collide them all
            // onto one object file.
            obj = (tmpDir / (fs::path(src).filename().string() + ".o"))
                      .string();
        }

        mvx::PPResult ppr;
        try {
            // Preprocess errors already carry the real file and line.
            ppr = mvx::preprocess(ss.str(), src, defines);
        } catch (const mvx::CompileError &e) {
            std::cerr << e.item << ":" << e.line << ": " << e.what() << "\n";
            return 1;
        }
        try {
            mvx::Program prog = mvx::parse(ppr.text, src, extFuncs);
            cg.dwarfLines.clear();
            cg.dwarfLines.reserve(ppr.map.size());
            for (const auto &m : ppr.map) cg.dwarfLines.push_back(m.dwarf);
            mvx::compileToObject(prog, obj, cg);
        } catch (const mvx::CompileError &e) {
            // Parse/codegen errors carry an output line; map it back to
            // the original source (an included file, past an $INCLUDE).
            std::string item = e.item;
            int line = e.line;
            if (line >= 1 && line <= (int)ppr.map.size()) {
                item = ppr.map[line - 1].file;
                line = ppr.map[line - 1].line;
            }
            std::cerr << item << ":" << line << ": " << e.what() << "\n";
            return 1;
        }
        linkObjects.push_back(obj);
    }

    if (compileOnly) return 0;

    if (outPath.empty())
        outPath = shared ? "libmvxsubs" : "a.out";

    fs::path lib = runtimeLibDir();
    std::string cmd = "cc";
    if (shared) {
#ifdef __APPLE__
        cmd += " -dynamiclib -undefined dynamic_lookup";
#else
        cmd += " -shared";
#endif
    }
    for (const std::string &o : linkObjects) cmd += " " + shellQuote(o);
    if (!shared) {
        cmd += " " + shellQuote((lib / "mvx_crt.o").string());
        // Executables link the shared runtime dynamically: its code maps
        // once and is shared across every mvx process, and the whole of
        // it is loaded, so dlopen'd subroutine libraries and storage
        // drivers resolve mv_*/mvx_* from that single copy — including
        // functions the host's own code never referenced.  An rpath to
        // the lib directory lets the program find libmvxrt at run time
        // wherever it is placed.  (Subroutine libraries still link no
        // runtime: they share the host's one copy, so per-image state —
        // the LMDB env handle, the driver registry — stays single.)
        cmd += " -L" + shellQuote(lib.string()) + " -lmvxrt";
        cmd += " -Wl,-rpath," + shellQuote(lib.string());
    }
#ifndef __APPLE__
    cmd += " -ldl";                     // dlopen for storage drivers
    cmd += " -lm";                      // libm for lowered math intrinsics
    if (!shared)
        cmd += " -rdynamic";            // expose mvx_sub_* to runtime CALL
#endif
    cmd += " -o " + shellQuote(outPath);

    int rc = std::system(cmd.c_str());

#ifdef __APPLE__
    // Mach-O keeps debug info in the object files; bundle it into a .dSYM
    // before the temp objects are removed, or BASIC-level debugging is
    // silently lost.
    if (rc == 0)
        std::system(("dsymutil " + shellQuote(outPath) +
                     " >/dev/null 2>&1").c_str());
#endif

    if (!tmpDir.empty()) {
        std::error_code ec;
        fs::remove_all(tmpDir, ec);
    }
    if (rc != 0) {
        std::cerr << "mvx-basic: link failed\n";
        return 1;
    }
    return 0;
}
