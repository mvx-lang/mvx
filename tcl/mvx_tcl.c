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

/* MVX classic TCL — the dispatch engine, and only the dispatch engine.
 *
 * This C program implements: the prompt, the builtin table, VOC lookup,
 * and spawning cataloged verb executables.  Verbs themselves are BASIC
 * programs (ARCHITECTURE.md 6.2); this surface is meant to freeze once
 * complete.
 *
 * Builtins (shell-internal by nature, cannot be verbs):
 *   OFF / QUIT / BYE   end the session
 *   ! <command>        raw passthrough to Unix
 *
 * The privilege gate lives in the runtime exec primitive (mvx_unix_cmd,
 * ARCHITECTURE.md 8.1), not here — a check in the shell would be
 * decorative.  Below the unrestricted tier a `!` command runs only if the
 * permit whitelist allows it (argv-style, no shell); a denial returns < 0,
 * which we surface as exit 126 so a script or BASIC EXECUTE sees the error.
 *
 * Dispatch order: builtin table, then VOC, then not-found.
 * VOC verb record: attr 1 = "V", attr 2 = executable path relative to
 * the account directory.  The command sentence reaches the verb via
 * $MVX_SENTENCE (the SENTENCE() intrinsic).
 */
#include "mvx_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_EDITLINE
#include <editline/readline.h>
#endif

#ifndef MVX_SYSTEM_DIR
#define MVX_SYSTEM_DIR "."
#endif

static mvx_ctx *g_ctx;
static mv_value g_voc, g_sysvoc;
static int g_voc_state, g_sysvoc_state; /* 0 untried, 1 open, -1 absent */

static const char *system_dir(void) {
    const char *p = getenv("MVXSYSTEM");
    if (p && p[0]) return p;
    /* Relative to libmvxrt (../lib): the install layout first, then the
       dev build tree, then the compile-time default. */
    const char *rtd = mvx_runtime_dir();
    if (rtd[0]) {
        static char buf[4096];
        const char *cand[] = { "/../share/mvx/system", "/../system" };
        for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++) {
            snprintf(buf, sizeof buf, "%s%s", rtd, cand[i]);
            struct stat sb;
            if (stat(buf, &sb) == 0 && S_ISDIR(sb.st_mode)) return buf;
        }
    }
    return MVX_SYSTEM_DIR;
}

/* Read one line from fd 0 unbuffered.  Verbs share this stdin; stdio
   readahead here would swallow input meant for them (and theirs would
   swallow ours — mv_input reads the same way).  Returns 0 on EOF with
   nothing read. */
static int read_line_raw(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) {
            if (n == 0) return 0;
            break;
        }
        if (c == '\n') break;
        if (n < cap - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    return 1;
}

/* Current account identity, refreshed on entry and by LOGTO. */
static char g_acct_path[4096] = "?";
static char g_acct_base[256] = "?";

/* The .mvx descriptor is the authoritative account marker: the VOC may
   be a named DB inside the LMDB env (or on a daemon), so there is no
   guaranteed physical file otherwise.  These older markers still count,
   so pre-.mvx accounts are recognised and upgraded. */
static int has_descriptor(void) {
    struct stat sb;
    return stat(".mvx", &sb) == 0;
}

static int has_markers(void) {
    static const char *markers[] = {
        "mvxdata.lmdb", "VOC", "CATALOG", "BP", "PACKAGES", "BINDINGS",
        NULL
    };
    struct stat sb;
    for (int i = 0; markers[i]; i++)
        if (stat(markers[i], &sb) == 0) return 1;
    return 0;
}

static int is_account(void) { return has_descriptor() || has_markers(); }

/* Read the account name from .mvx (name = value), else "". */
static void descriptor_name(char *out, size_t cap) {
    out[0] = '\0';
    FILE *fp = fopen(".mvx", "r");
    if (!fp) return;
    char ln[512];
    while (fgets(ln, sizeof ln, fp)) {
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "name", 4) != 0) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        eq++;
        while (*eq == ' ' || *eq == '\t') eq++;
        size_t n = strlen(eq);
        while (n && (eq[n - 1] == '\n' || eq[n - 1] == '\r' ||
                     eq[n - 1] == ' '))
            eq[--n] = '\0';
        snprintf(out, cap, "%s", eq);
        break;
    }
    fclose(fp);
}

/* Write .mvx for the current account (idempotent). */
static void write_descriptor(const char *name) {
    FILE *fp = fopen(".mvx", "w");
    if (!fp) return;
    fprintf(fp, "# MVX account descriptor\nname = %s\nversion = 1\n",
            name);
    fclose(fp);
}

static void descriptor_name(char *out, size_t cap);

static void account_refresh(void) {
    if (!getcwd(g_acct_path, sizeof g_acct_path))
        snprintf(g_acct_path, sizeof g_acct_path, "?");
    const char *b = strrchr(g_acct_path, '/');
    snprintf(g_acct_base, sizeof g_acct_base, "%s",
             b && b[1] ? b + 1 : g_acct_path);
    /* a name in .mvx overrides the directory basename */
    char nm[256];
    descriptor_name(nm, sizeof nm);
    if (nm[0]) snprintf(g_acct_base, sizeof g_acct_base, "%s", nm);
    setenv("MVXACCTPATH", g_acct_path, 1);
}

static int voc_open(mv_value *voc, const char *spec) {
    mv_value s;
    mv_init(&s);
    mv_set_str(&s, spec, (int64_t)strlen(spec));
    mv_init(voc);
    int ok = mvx_open(g_ctx, NULL, &s, voc) ? 1 : -1;
    mv_clear(&s);
    return ok;
}

/* Read a V-record from the given VOC; path receives attribute 2. */
static int voc_read(mv_value *voc, const char *verb, char *path,
                    size_t cap) {
    mv_value id, rec, a1, a2;
    mv_init(&id); mv_init(&rec); mv_init(&a1); mv_init(&a2);
    mv_set_str(&id, verb, (int64_t)strlen(verb));
    int found = 0;
    if (mvx_read(g_ctx, &rec, voc, &id, 0)) {
        mv_extract_fn(&a1, &rec, 1, 0, 0);
        mv_extract_fn(&a2, &rec, 2, 0, 0);
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(&a1, nb, sizeof nb, &p);
        if (n >= 1 && (p[0] == 'V' || p[0] == 'v')) {
            n = mv_val_chars(&a2, nb, sizeof nb, &p);
            if (n > 0 && (size_t)n < cap) {
                memcpy(path, p, (size_t)n);
                path[n] = '\0';
                found = 1;
            }
        }
    }
    mv_clear(&id); mv_clear(&rec); mv_clear(&a1); mv_clear(&a2);
    return found;
}

/* Linked packages: the account's PACKAGES record (one path per line,
   maintained by LINK-PKG / UNLINK-PKG) names package directories whose
   VOCs join the resolution chain.  Reloaded when the file changes, so
   a LINK-PKG takes effect in the same session. */
#define MAX_PKGS 16
static char g_pkgs[MAX_PKGS][1024];
static mv_value g_pkgvoc[MAX_PKGS];
static int g_pkgvoc_state[MAX_PKGS];
static int g_npkgs;
static long long g_pkg_stamp = -1;

static void pkgs_reload(void) {
    struct stat sb;
    long long mt = 0;
    if (stat("PACKAGES", &sb) == 0) {
        /* Nanosecond stamp + size: whole-second mtime misses a LINK-PKG
           landing in the same second as the previous reload. */
#ifdef __APPLE__
        mt = (long long)sb.st_mtimespec.tv_sec * 1000000000LL +
             sb.st_mtimespec.tv_nsec + sb.st_size;
#else
        mt = (long long)sb.st_mtim.tv_sec * 1000000000LL +
             sb.st_mtim.tv_nsec + sb.st_size;
#endif
    }
    if (mt == g_pkg_stamp) return;
    g_pkg_stamp = mt;
    for (int i = 0; i < g_npkgs; i++)
        if (g_pkgvoc_state[i] > 0) mv_clear(&g_pkgvoc[i]);
    g_npkgs = 0;
    FILE *fp = fopen("PACKAGES", "r");
    if (!fp) return;
    char ln[1024];
    while (fgets(ln, sizeof ln, fp) && g_npkgs < MAX_PKGS) {
        size_t n = strlen(ln);
        while (n && (ln[n - 1] == '\n' || ln[n - 1] == '\r' ||
                     ln[n - 1] == ' '))
            ln[--n] = '\0';
        if (n == 0) continue;
        snprintf(g_pkgs[g_npkgs], sizeof g_pkgs[0], "%s", ln);
        g_pkgvoc_state[g_npkgs] = 0;
        g_npkgs++;
    }
    fclose(fp);
}

/* Resolution: account VOC (local overrides), then linked packages in
   listed order, then the system account's master VOC.  Foreign verbs
   execute by path from their own CATALOG but run in the user's
   account (cwd). */
static int voc_lookup(const char *verb, char *path, size_t cap) {
    if (g_voc_state == 0) g_voc_state = voc_open(&g_voc, "VOC");
    if (g_voc_state > 0 && voc_read(&g_voc, verb, path, cap))
        return 1;

    pkgs_reload();
    for (int i = 0; i < g_npkgs; i++) {
        if (g_pkgvoc_state[i] == 0) {
            char pv[1152];
            snprintf(pv, sizeof pv, "%s/VOC", g_pkgs[i]);
            g_pkgvoc_state[i] = voc_open(&g_pkgvoc[i], pv);
        }
        if (g_pkgvoc_state[i] > 0) {
            char rel[1024];
            if (voc_read(&g_pkgvoc[i], verb, rel, sizeof rel)) {
                snprintf(path, cap, "%s/%s", g_pkgs[i], rel);
                return 1;
            }
        }
    }

    if (g_sysvoc_state == 0) {
        char sysvoc[4096];
        snprintf(sysvoc, sizeof sysvoc, "%s/VOC", system_dir());
        g_sysvoc_state = voc_open(&g_sysvoc, sysvoc);
    }
    if (g_sysvoc_state > 0) {
        char rel[1024];
        if (voc_read(&g_sysvoc, verb, rel, sizeof rel)) {
            snprintf(path, cap, "%s/%s", system_dir(), rel);
            return 1;
        }
    }
    return (g_voc_state < 0 && g_sysvoc_state < 0) ? -1 : 0;
}

/* Run a cataloged verb and return its process exit status, so a verb (e.g.
   CHECK) can signal failure to a script or CI: `mvx -c 'CHECK ...'` exits with
   the verb's code (STOP <code>). */
static int run_verb(const char *path, const char *line) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("mvx: fork");
        return 1;
    }
    if (pid == 0) {
        setenv("MVX_SENTENCE", line, 1);
        char *dup = strdup(line);
        char *argv[64];
        int n = 0;
        for (char *t = strtok(dup, " \t"); t && n < 63;
             t = strtok(NULL, " \t"))
            argv[n++] = t;
        argv[n] = NULL;
        execv(path, argv);
        fprintf(stderr, "mvx: cannot execute %s\n", path);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* Execute one TCL line; return a process-style status (0 ok) so the -c
   one-shot can exit with a verb's code. */
static int command(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0) return 0;

    if (line[0] == '!') {               /* raw Unix — runtime-gated */
        int64_t rc = mvx_unix_cmd(g_ctx, line + 1);
        return rc < 0 ? 126 : (int)rc;  /* propagate: 126 = denied, else the cmd's exit */
    }

    char verb[128];
    size_t vn = 0;
    for (const char *p = line; *p && *p != ' ' && *p != '\t' &&
                               vn < sizeof verb - 1; p++)
        verb[vn++] = (char)toupper((unsigned char)*p);
    verb[vn] = '\0';

    if (strcmp(verb, "OFF") == 0 || strcmp(verb, "QUIT") == 0 ||
        strcmp(verb, "BYE") == 0)
        exit(0);

    if (strcmp(verb, "SH") == 0) {      /* interactive shell — gated */
        const char *sh = getenv("SHELL");
        int64_t rc = mvx_unix_cmd(g_ctx, sh && sh[0] ? sh : "/bin/sh");
        return rc < 0 ? 126 : (int)rc;
    }

    if (strcmp(verb, "LOGTO") == 0) {   /* switch accounts */
        const char *arg = line;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        while (*arg == ' ' || *arg == '\t') arg++;
        if (!*arg) {
            fprintf(stderr, "usage: LOGTO account-directory\n");
            return 2;
        }
        if (chdir(arg) != 0) {
            fprintf(stderr, "LOGTO: cannot enter account %s\n", arg);
            return 2;
        }
        account_refresh();
        g_voc_state = 0;                /* re-resolve in the new account */
        g_pkg_stamp = -1;
        g_npkgs = 0;
        const char *sess = getenv("MVXSESSION");
        if (sess && sess[0]) {          /* select lists don't cross LOGTO */
            FILE *fp = fopen(sess, "wb");
            if (fp) fclose(fp);
        }
        printf("now in account %s (%s)\n", g_acct_base, g_acct_path);
        fflush(stdout);
        return 0;
    }

    char path[1024];
    int r = voc_lookup(verb, path, sizeof path);
    if (r > 0)
        return run_verb(path, line);
    if (r < 0)
        fprintf(stderr, "mvx: no VOC found in this account or the "
                        "system account (%s); only builtins are "
                        "available\n", system_dir());
    else
        fprintf(stderr, "verb \"%s\" not found\n", verb);
    return 127;                          /* command not found (Unix convention) */
}

int main(int argc, char **argv) {
    const char *acct = NULL;
    const char *one_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            acct = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            one_cmd = argv[++i];
        else {
            fprintf(stderr, "usage: mvx [-a account] [-c command]\n");
            return 2;
        }
    }

    /* Account resolution: -a flag, then $MVXACCOUNT, then cwd
       (ARCHITECTURE.md 7.2 — a parameter, not a mode). */
    if (!acct) acct = getenv("MVXACCOUNT");
    if (!acct || !acct[0]) acct = ".";
    if (chdir(acct) != 0) {
        fprintf(stderr, "mvx: cannot enter account %s\n", acct);
        return 1;
    }
    setenv("MVXACCOUNT", ".", 1);       /* children resolve from cwd */

    /* The session owns the select-list handoff file (7.3).  A nested
       TCL (spawned by EXECUTE) inherits the outer session rather than
       starting its own, so select lists flow across nesting. */
    static char sesspath[256];
    if (!getenv("MVXSESSION")) {
        snprintf(sesspath, sizeof sesspath, "/tmp/mvxsess.XXXXXX");
        int fd = mkstemp(sesspath);
        if (fd >= 0) {
            close(fd);
            setenv("MVXSESSION", sesspath, 1);
        }
    }

    g_ctx = mvx_ctx_create();
    account_refresh();

    /* upgrade a pre-.mvx account so the descriptor becomes canonical */
    if (!has_descriptor() && has_markers())
        write_descriptor(g_acct_base);

    if (one_cmd) {                      /* ssh/cron style: -c and out */
        char *dup = strdup(one_cmd);
        int rc = command(dup);          /* propagate the verb's exit status */
        free(dup);
        mvx_ctx_destroy(g_ctx);
        if (sesspath[0]) unlink(sesspath);
        return rc;
    }

    account_refresh();

    int tty = isatty(0);

    /* UniData-style: don't silently turn a fresh directory into an
       account.  On an interactive session in a directory that has no
       account markers yet, ask before creating one. */
    if (tty && !is_account()) {
        printf("Directory %s is not an MVX account.\n", g_acct_path);
        printf("Create one here? (y/N) ");
        fflush(stdout);
        char ans[64];
        if (!read_line_raw(ans, sizeof ans) ||
            (ans[0] != 'y' && ans[0] != 'Y')) {
            printf("No account created.\n");
            mvx_ctx_destroy(g_ctx);
            if (sesspath[0]) unlink(sesspath);
            return 0;
        }
        mv_value voc;
        mv_init(&voc);
        mv_set_str(&voc, "VOC", 3);
        if (mvx_createfile(g_ctx, &voc, NULL)) {
            write_descriptor(g_acct_base);
            printf("Created MVX account in %s\n", g_acct_path);
        } else
            printf("could not create the account\n");
        mv_clear(&voc);
    }

    if (tty) {
        printf("MVX TCL — account %s (%s)\n", g_acct_base, g_acct_path);
        fflush(stdout);
    }

#ifdef HAVE_EDITLINE
    char histfile[4096] = "";
    if (tty) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(histfile, sizeof histfile, "%s/.mvx_history", home);
            read_history(histfile);
        }
    }
#endif

    char line[4096];
    for (;;) {
#ifdef HAVE_EDITLINE
        if (tty) {
            char prompt[300];
            snprintf(prompt, sizeof prompt, "%s> ", g_acct_base);
            char *l = readline(prompt);
            if (!l) break;
            if (*l) {
                add_history(l);
                if (histfile[0]) write_history(histfile);
            }
            snprintf(line, sizeof line, "%s", l);
            free(l);
            command(line);
            continue;
        }
#else
        if (tty) {
            printf("%s> ", g_acct_base);
            fflush(stdout);
        }
#endif
        if (!read_line_raw(line, sizeof line)) break;
        command(line);
    }
    if (tty) fputc('\n', stdout);
    mvx_ctx_destroy(g_ctx);
    if (sesspath[0]) unlink(sesspath);
    return 0;
}
