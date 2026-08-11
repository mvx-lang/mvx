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

/* Spawning and the privilege gate.
 *
 * THE GATE LIVES HERE, IN THE RUNTIME (ARCHITECTURE.md 8.1): anyone who
 * can compile can call these primitives directly, so a check anywhere
 * shallower would be decorative.  One gate covers every path — the TCL
 * `!` builtin, EXECUTE, and the compiler spawn.
 *
 * Tiers (8.2): restricted < developer < unrestricted, default DENY.
 * Spawning cataloged verbs inside the account is the platform working
 * and is allowed at every tier; compiling needs developer.  Raw Unix (`!`,
 * SH, OSEXEC) below the unrestricted tier runs ONLY a plain single command
 * the permit whitelist allows (mvx_perm.c), argv-style; a shell string
 * (pipes, redirection, substitution, globs, chaining) needs unrestricted.
 * The tier comes from $MVXPRIV — the development stand-in for system-level
 * configuration outside the account (8.3); an env var set by the login
 * environment is not writable from inside account data, which is the
 * property that matters.
 *
 * All spawns are argv-style (execv), never through a shell, except the
 * unrestricted-only raw-string passthrough: with argument vectors,
 * metacharacters in user input are inert (8.4).  A permit denial returns
 * a negative status the caller surfaces as an error (the TCL exits 126, so
 * a BASIC EXECUTE ... RETURNING sees the failure).
 */
#include "mvx_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define AM ((char)0xFE)

#ifndef MVX_BIN_DIR
#define MVX_BIN_DIR "."
#endif

enum { TIER_RESTRICTED = 0, TIER_DEVELOPER = 1, TIER_UNRESTRICTED = 2 };

static int priv_tier(void) {
    const char *p = getenv("MVXPRIV");
    if (!p) return TIER_RESTRICTED;                 /* default deny */
    if (strcasecmp(p, "unrestricted") == 0) return TIER_UNRESTRICTED;
    if (strcasecmp(p, "developer") == 0) return TIER_DEVELOPER;
    return TIER_RESTRICTED;
}

static const char *bin_dir(void) {
    const char *p = getenv("MVXBIN");
    if (p && p[0]) return p;
    /* Binaries live in ../bin from libmvxrt (../lib): relocatable. */
    const char *rtd = mvx_runtime_dir();
    if (rtd[0]) {
        static char b[4096];
        snprintf(b, sizeof b, "%s/../bin", rtd);
        return b;
    }
    return MVX_BIN_DIR;
}

/* Spawn argv, optionally capturing stdout into a dynamic array
   (newlines become attribute marks).  Returns the exit status, or -1
   on spawn failure. */
static int64_t spawn(char *const argv[], mv_value *capture, int use_path) {
    int fds[2] = {-1, -1};
    if (capture && pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (capture) {
            close(fds[0]);
            dup2(fds[1], 1);
            close(fds[1]);
        }
        if (use_path) execvp(argv[0], argv);   /* bare command -> PATH search */
        else          execv(argv[0], argv);    /* our own absolute-path spawns */
        fprintf(stderr, "mvx: cannot execute %s\n", argv[0]);
        _exit(127);
    }

    if (capture) {
        close(fds[1]);
        char *buf = NULL;
        size_t len = 0, cap = 0;
        char chunk[4096];
        ssize_t n;
        while ((n = read(fds[0], chunk, sizeof chunk)) > 0) {
            if (len + (size_t)n > cap) {
                cap = cap ? cap * 2 : 8192;
                while (cap < len + (size_t)n) cap *= 2;
                char *nb = realloc(buf, cap);
                if (!nb) mvx_fatal("out of memory in EXECUTE capture");
                buf = nb;
            }
            memcpy(buf + len, chunk, (size_t)n);
            len += (size_t)n;
        }
        close(fds[0]);
        while (len > 0 && buf[len - 1] == '\n') len--;
        for (size_t i = 0; i < len; i++)
            if (buf[i] == '\n') buf[i] = AM;
        mv_set_str(capture, buf ? buf : "", (int64_t)len);
        free(buf);
    }

    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* --- raw Unix (the `!` / SH path) --------------------------------------
   The unrestricted tier runs the string as a full shell, unchecked.  BELOW it,
   `!` is no longer all-or-nothing: a PLAIN single command (no shell syntax) is
   permit-checked exactly like OSEXEC and, if the whitelist allows it, run
   argv-style — never a shell, so metacharacters in arguments are inert.  A
   string that genuinely needs a shell (pipes, redirection, substitution, globs,
   chaining) still requires unrestricted.  A denial returns -1, which the caller
   (a BASIC EXECUTE, the TCL) surfaces as an error the program can handle. */

/* Split a plain command into argv, stripping '...' / "..." quoting.  Returns
   argc, or -1 if the string needs a REAL shell: any UNQUOTED shell metacharacter
   (pipe/redirect/chain/subshell/substitution/glob/brace/tilde/backslash/newline),
   an unterminated quote, or an overflow. */
static int shell_tokenize(const char *cmd, char *out, size_t ocap,
                          char **av, int maxargs) {
    int argc = 0;
    size_t o = 0;
    const char *p = cmd;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= maxargs - 1) return -1;
        av[argc++] = out + o;
        while (*p && *p != ' ' && *p != '\t') {
            char c = *p;
            if (c == '\'' || c == '"') {                 /* literal quoted run */
                char q = c;
                for (p++; *p && *p != q; p++) {
                    if (o + 1 >= ocap) return -1;
                    out[o++] = *p;
                }
                if (*p != q) return -1;                  /* unterminated quote */
                p++;
                continue;
            }
            if (strchr("|&;<>()`$*?[]{}~#\\\n\r", c)) return -1;  /* needs a shell */
            if (o + 1 >= ocap) return -1;
            out[o++] = c;
            p++;
        }
        if (o + 1 >= ocap) return -1;
        out[o++] = '\0';
    }
    av[argc] = NULL;
    return argc;
}

int64_t mvx_unix_cmd(mvx_ctx *ctx, const char *cmd) {
    (void)ctx;
    if (priv_tier() >= TIER_UNRESTRICTED) {              /* full shell, unchecked */
        int st = system(cmd);
        return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    }
    char work[8192];
    char *av[256];
    int argc = shell_tokenize(cmd, work, sizeof work, av, 256);
    if (argc < 0) {
        fprintf(stderr,
                "not allowed: '!' needs the unrestricted tier for shell syntax "
                "(pipes, redirection, substitution, globs, chaining) — a plain "
                "command runs only if a 'permit' grant allows it\n");
        return -1;
    }
    if (argc == 0) return 0;
    if (!mvx_perm_allowed(av)) {
        fprintf(stderr,
                "not allowed: '%s' is not permitted (no matching 'permit' grant "
                "in .mvx / .mvx-private for your groups, or a 'deny' rule blocks "
                "this command or one of its switches)\n", av[0]);
        return -1;
    }
    return spawn(av, NULL, 1);                           /* argv-style, PATH search */
}

/* --- fine-grained external command (the OSEXEC path) -------------------
   Run one specific command argv-style — NEVER a shell, so metacharacters
   in argument fields are inert (8.4).  `argv` is an FM-delimited dynamic
   array: field 1 is the command (searched on PATH), the rest are its
   arguments.  Allowed when the caller's OS groups are granted the command
   by the .mvx / .mvx-private whitelist (mvx_perm.c), or unconditionally at
   the unrestricted tier.  Returns the exit status, or -1 on deny/failure. */
int64_t mvx_run(mvx_ctx *ctx, const mv_value *argv, mv_value *capture) {
    (void)ctx;
    char nb[40];
    const char *ap;
    int64_t al = mv_val_chars(argv, nb, sizeof nb, &ap);
    if (al <= 0) return -1;

    char *buf = malloc((size_t)al + 1);
    if (!buf) return -1;
    memcpy(buf, ap, (size_t)al);
    buf[al] = '\0';

    int argc = 1;
    for (int64_t i = 0; i < al; i++) if (buf[i] == AM) argc++;
    char **av = calloc((size_t)argc + 1, sizeof(char *));
    if (!av) { free(buf); return -1; }
    int k = 0;
    av[k++] = buf;
    for (int64_t i = 0; i < al; i++)
        if (buf[i] == AM) { buf[i] = '\0'; av[k++] = buf + i + 1; }
    av[k] = NULL;

    if (!av[0][0]) { free(av); free(buf); return -1; }

    if (priv_tier() < TIER_UNRESTRICTED && !mvx_perm_allowed(av)) {
        fprintf(stderr,
                "not allowed: '%s' is not permitted (no matching 'permit' "
                "grant in .mvx / .mvx-private for your groups, or a 'deny' "
                "rule blocks this command or one of its switches)\n",
                av[0]);
        free(av);
        free(buf);
        return -1;
    }
    int64_t st = spawn(av, capture, 1);
    free(av);
    free(buf);
    return st;
}

/* --- native filesystem primitives (permit-gated) -----------------------
   MKDIR / RMTREE / UNTAR let a program do the common install-time file ops
   with no external command and no shell — so a package need not be granted
   raw Unix for them.  They are still mutating and powerful (a recursive
   delete, an archive that writes a tree), so below the unrestricted tier
   each needs a permit for its op name (`mkdir`, `rmtree`, `untar`), which a
   site grants to the admin/dev groups that install packages.  UNAME and other
   read-only info stay ungated, like OSREAD.  Path arguments are used as-is
   (no shell), so there is nothing to inject. */

/* Permitted to perform native op `op`?  Unrestricted bypasses; otherwise the
   op name must be granted like an external command (mvx_perm.c). */
static int perm_op(const char *op) {
    if (priv_tier() >= TIER_UNRESTRICTED) return 1;
    char *av[2] = {(char *)op, NULL};
    return mvx_perm_allowed(av);
}

static void copy_path(const mv_value *v, char *out, size_t cap) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(v, nb, sizeof nb, &p);
    snprintf(out, cap, "%.*s", (int)n, p);
}

/* Make a directory and any missing parents (like `mkdir -p`). 1 ok, 0 fail. */
static int make_dirs(char *path) {
    for (char *s = path + 1; *s; s++)
        if (*s == '/') { *s = '\0'; if (mkdir(path, 0777) != 0 && errno != EEXIST) { *s = '/'; return 0; } *s = '/'; }
    return (mkdir(path, 0777) == 0 || errno == EEXIST) ? 1 : 0;
}

/* MKDIR(path) -> 1 created/exists, 0 failure, -1 denied. */
int64_t mvx_mkdir(mvx_ctx *ctx, const mv_value *path) {
    (void)ctx;
    if (!perm_op("mkdir")) {
        fprintf(stderr, "not allowed: MKDIR requires a 'mkdir' permit for your groups\n");
        return -1;
    }
    char buf[4096];
    copy_path(path, buf, sizeof buf);
    if (!buf[0]) return 0;
    return make_dirs(buf);
}

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) {
    (void)sb; (void)t; (void)f;
    return remove(p);           /* files then, with FTW_DEPTH, their dirs */
}

/* RMTREE(path) -> 1 removed (or already gone), 0 failure, -1 denied. */
int64_t mvx_rmtree(mvx_ctx *ctx, const mv_value *path) {
    (void)ctx;
    if (!perm_op("rmtree")) {
        fprintf(stderr, "not allowed: RMTREE requires a 'rmtree' permit for your groups\n");
        return -1;
    }
    char buf[4096];
    copy_path(path, buf, sizeof buf);
    if (!buf[0]) return 0;
    if (nftw(buf, rm_cb, 16, FTW_DEPTH | FTW_PHYS) == 0) return 1;
    return (errno == ENOENT) ? 1 : 0;    /* nothing to remove is success */
}

/* Run argv (argv-style, PATH search) capturing up to cap-1 bytes of stdout into
   buf (NUL-terminated); stderr is discarded.  Returns exit status, or -1. */
static int capture_cmd(char *const argv[], char *buf, size_t cap) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, 2); close(dn); }
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fds[1]);
    size_t len = 0;
    ssize_t n;
    while (len + 1 < cap && (n = read(fds[0], buf + len, cap - 1 - len)) > 0)
        len += (size_t)n;
    char sink[4096];                        /* drain the rest so tar can finish */
    while (read(fds[0], sink, sizeof sink) > 0) {}
    close(fds[0]);
    buf[len] = '\0';
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* True iff every entry in a `tar -tzf` listing sits under ONE common top-level
   directory — the account-shaped-package convention (git/…).  A contents-at-root
   archive (any file or a second dir at the top) returns 0. */
static int single_top_dir(const char *listing) {
    char top[256];
    size_t tl = 0;
    int have = 0;
    for (const char *p = listing; *p; ) {
        const char *nl = strchr(p, '\n');
        const char *end = nl ? nl : p + strlen(p);
        const char *s = p;
        if (s + 2 <= end && s[0] == '.' && s[1] == '/') s += 2;   /* drop ./ */
        const char *slash = s;
        while (slash < end && *slash != '/') slash++;
        if (slash == end) {
            if (s < end) return 0;           /* a top-level file -> not wrapped */
        } else {
            size_t clen = (size_t)(slash - s) + 1;   /* "<name>/" */
            if (clen >= sizeof top) return 0;
            if (!have) { memcpy(top, s, clen); tl = clen; have = 1; }
            else if (clen != tl || memcmp(s, top, tl) != 0) return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return have;
}

/* UNTAR(tarball, destdir) -> exit status (0 ok), -1 denied.  Creates destdir,
   then extracts argv-style (no shell).  A tar wrapped in a single top-level
   directory is unwrapped (--strip-components=1) so a package's files land at
   destdir either way; a contents-at-root tar is extracted as-is. */
int64_t mvx_untar(mvx_ctx *ctx, const mv_value *tarball, const mv_value *dest) {
    (void)ctx;
    if (!perm_op("untar")) {
        fprintf(stderr, "not allowed: UNTAR requires a 'untar' permit for your groups\n");
        return -1;
    }
    char tar[4096], dst[4096];
    copy_path(tarball, tar, sizeof tar);
    copy_path(dest, dst, sizeof dst);
    if (!tar[0] || !dst[0]) return -1;
    make_dirs(dst);

    char listing[65536];
    char *lav[] = {"tar", "-tzf", tar, NULL};
    int strip = (capture_cmd(lav, listing, sizeof listing) == 0) &&
                single_top_dir(listing);

    char *strip_av[] = {"tar", "-xzf", tar, "--strip-components=1", "-C", dst, NULL};
    char *flat_av[]  = {"tar", "-xzf", tar, "-C", dst, NULL};
    return spawn(strip ? strip_av : flat_av, NULL, 1);
}

/* --- editor spawn (the VI verb) — unrestricted --------------------------
   Runs the configured interactive editor ($MVXEDITOR, then $VISUAL,
   then $EDITOR, then vi) on one file, argv-style.  It is a general
   exec (an editor's own shell escapes make it one), so it needs the
   unrestricted tier — the built-in ED remains the tier-safe editor. */
int64_t mvx_editfile(mvx_ctx *ctx, const mv_value *path) {
    (void)ctx;
    if (priv_tier() < TIER_UNRESTRICTED) {
        fprintf(stderr,
                "not allowed: external editing requires unrestricted "
                "privilege (use ED instead)\n");
        return -1;
    }
    const char *ed = getenv("MVXEDITOR");
    if (!ed || !ed[0]) ed = getenv("VISUAL");
    if (!ed || !ed[0]) ed = getenv("EDITOR");
    if (!ed || !ed[0]) ed = "vi";

    char nb[40];
    const char *pp;
    int64_t pl = mv_val_chars(path, nb, sizeof nb, &pp);
    char pathbuf[4096];
    snprintf(pathbuf, sizeof pathbuf, "%.*s", (int)pl, pp);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *argv[] = {(char *)ed, pathbuf, NULL};
        execvp(ed, argv);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* TMPNAM() -> a unique temp path (the file is not created). */
void mvx_tmpnam(mv_value *dst) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";
    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%s/mvxedit.XXXXXX", dir);
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);             /* claim the name, keep the path */
    mv_set_str(dst, tmpl, (int64_t)strlen(tmpl));
}

/* --- compile — the narrow primitive (developer+) -----------------------
   Takes structured arguments and builds the argv itself: there is
   nothing to inject (8.2). mode: "c" object, "exe" executable,
   "shared" subroutine library. */

int64_t mvx_compile(mvx_ctx *ctx, const mv_value *mode,
                    const mv_value *src, const mv_value *out) {
    (void)ctx;
    if (priv_tier() < TIER_DEVELOPER) {
        fprintf(stderr,
                "not allowed: compiling requires developer privilege\n");
        return -1;
    }
    char mb[40], sb[40], ob[40];
    const char *mp, *sp, *op;
    mv_val_chars(mode, mb, sizeof mb, &mp);
    int64_t sl = mv_val_chars(src, sb, sizeof sb, &sp);
    int64_t ol = mv_val_chars(out, ob, sizeof ob, &op);
    if (sl == 0 || ol == 0) return -1;

    char mvx[4096], srcbuf[1024], outbuf[1024];
    snprintf(mvx, sizeof mvx, "%s/mvx-basic", bin_dir());
    snprintf(srcbuf, sizeof srcbuf, "%.*s", (int)sl, sp);
    snprintf(outbuf, sizeof outbuf, "%.*s", (int)ol, op);
    if (mp[0] == 's' || mp[0] == 'S') {
        /* Shared subroutine libraries get the platform suffix so BASIC callers
           stay portable — unless the caller already supplied it.  Test for the
           suffix itself, not merely a '.': MV subroutine names routinely contain
           dots (MVPKG.CONFIG, GIT.ADD), and a name-dot must not be mistaken for
           an extension, or the lib is written without the suffix the loader
           (mvx_ext.c) scans for and the subroutine never resolves. */
#ifdef __APPLE__
        const char *suf = ".dylib";
#else
        const char *suf = ".so";
#endif
        size_t obl = strlen(outbuf), sfl = strlen(suf);
        if (obl < sfl || strcmp(outbuf + obl - sfl, suf) != 0)
            strncat(outbuf, suf, sizeof outbuf - obl - 1);
    }

    /* Ensure the output directory exists — CATALOG/ and LIB/ are not
       pre-created in a fresh account, so a plain CATALOG would otherwise
       fail to link into a missing directory. */
    char *slashp = strrchr(outbuf, '/');
    if (slashp) {
        *slashp = '\0';
        mkdir(outbuf, 0777);            /* harmless if it already exists */
        *slashp = '/';
    }

    char *argv[8];
    int n = 0;
    argv[n++] = mvx;
    if (mp[0] == 'c' || mp[0] == 'C') argv[n++] = "-c";
    else if (mp[0] == 's' || mp[0] == 'S') argv[n++] = "-shared";
    argv[n++] = srcbuf;
    argv[n++] = "-o";
    argv[n++] = outbuf;
    argv[n] = NULL;
    return spawn(argv, NULL, 0);
}

/* --- EXECUTE — run a TCL sentence (allowed at every tier) --------------
   Dispatch stays in one place: spawn mvx -c <sentence>.  A `!` in
   the sentence is gated inside the child by this same module. */

int64_t mvx_execute(mvx_ctx *ctx, const mv_value *sentence,
                    mv_value *capture, mv_value *rc) {
    (void)ctx;
    char nb[40];
    const char *sp;
    int64_t sl = mv_val_chars(sentence, nb, sizeof nb, &sp);

    char tcl[4096], sent[4096];
    snprintf(tcl, sizeof tcl, "%s/mvx", bin_dir());
    snprintf(sent, sizeof sent, "%.*s", (int)sl, sp);

    char *argv[5];
    argv[0] = tcl;
    argv[1] = "-c";
    argv[2] = sent;
    argv[3] = NULL;
    int64_t st = spawn(argv, capture, 0);
    if (rc) mv_set_int(rc, st);
    return st == 0;
}
