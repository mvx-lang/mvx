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
#include "mvx_driver.h"

#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_EDITLINE
#include <histedit.h>
#include <poll.h>
#include <errno.h>
#endif


static mvx_ctx *g_ctx;
static mv_value g_voc, g_sysvoc;
static int g_voc_state, g_sysvoc_state; /* 0 untried, 1 open, -1 absent */

/* The runtime's answer, so a verb is found in the same system account its
   CALLs will be resolved from (mvx#210). */
static const char *system_dir(void) { return mvx_system_dir(); }

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

/* Open a file by spec into `voc`.  The shell keeps this because it opens the
   account's VOC for its OWN purposes -- `.C` macros live there, and `.X` can
   name any file -- which is a shell concern and not resolution. */
static int voc_open(mv_value *voc, const char *spec) {
    mv_value s;
    mv_init(&s);
    mv_set_str(&s, spec, (int64_t)strlen(spec));
    mv_init(voc);
    int ok = mvx_open(g_ctx, NULL, &s, voc) ? 1 : -1;
    mv_clear(&s);
    return ok;
}

static mv_value g_voc;
static int g_voc_state;

/* RESOLUTION IS THE RUNTIME'S (mvx_voc_lookup, mvx#248).  It used to live
   here, and had to move when EXECUTE stopped spawning a shell to do it: a
   compiled program can EXECUTE with nothing above it, so the runtime has to
   be able to resolve a verb by itself, or EXECUTE would work under `mvx` and
   nowhere else.  One implementation, two callers. */
static int voc_lookup(const char *verb, char *path, size_t cap) {
    return mvx_voc_lookup(g_ctx, verb, path, cap);
}

/* Run a cataloged verb and return its process exit status, so a verb (e.g.
   CHECK) can signal failure to a script or CI: `mvx -c 'CHECK ...'` exits with
   the verb's code (STOP <code>). */
/* Show any messages waiting for this session (mvx#228).
 *
 * BETWEEN COMMANDS, NEVER DURING ONE.  This is called just before the prompt
 * and just after a verb returns, which are the two moments when nothing else
 * owns the screen.  There is no signal, no timer and no second thread: a
 * message can wait a few seconds for the operator to press Enter, and that is
 * a far better trade than writing over a running program's display -- or
 * than an interrupted read(), which mv_input treats as end of input.
 *
 * Costs nothing when no registry is running: mvx_msg_pending() answers -1
 * without talking to anything. */

/* RUN A VERB IN THIS PROCESS (mvx#248).
 *
 * The prompt used to fork and exec, which made every verb a stranger to the
 * session it was typed into: it reopened every file, took its own locks, and
 * could only be handed the select list sideways through a file.  A
 * TRANSACTION typed at the prompt could not span two verbs at all, because
 * the second one was a different process with a different store.
 *
 * Now the verb runs at a LEVEL on the session: its own unnamed COMMON, STATUS
 * and sentence, sharing everything the session owns.  So a select list simply
 * persists, a transaction spans as many verbs as it takes to finish, and a
 * program that opened files into COMMON has them still open for the next verb
 * -- which is how MV sites have always been laid out.
 *
 * THE ABORT STOPS HERE.  A fault takes a calling PROGRAM with it, but not the
 * prompt: on UniData an ABORT in a verb returns you to TCL rather than
 * logging you out, so this level catches one instead of passing it on.
 * Without that, every ABORT and every runtime fault would end the session.
 *
 * A verb with no loadable form -- any account cataloged before mvx#248 --
 * still forks and execs, exactly as before. */
static int run_verb(const char *path, const char *line) {
#ifdef __APPLE__
    static const char *libsfx = ".dylib";
#else
    static const char *libsfx = ".so";
#endif
    char lp[4200];
    snprintf(lp, sizeof lp, "%s%s", path, libsfx);
    void *h = dlopen(lp, RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    mvx_program_fn fn = h ? (mvx_program_fn)dlsym(h, "mvx_main") : NULL;
    if (fn) {
        int aborted = 0;
        int64_t rc = mvx_level_run_at_prompt(g_ctx, fn, line, &aborted);
        return (int)rc;
    }

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

/* ------------------------------------------------------------ #114
 * The R83 command stack.
 *
 * Semantics are D3's, from the Pick Systems Reference Manual's "dot stack"
 * and "tcl stack" entries -- not invented here, because a half-right stack
 * re-executes the wrong line, which is worse than having none.  What that
 * manual specifies, and what this implements:
 *
 *   - Every UNIQUE command typed at the prompt is saved.  "Unique" is strict:
 *     `who` appears once however often it is used.
 *   - Re-executing an entry, or editing one, moves it to the TOP.  That is
 *     what keeps the stack compact, and it means entry numbers shift.
 *   - Entry 1 is the top, i.e. the most recent.
 *   - The stack outlives the session; D3 keys it by user-id rather than by
 *     terminal, and ~/.mvx_history is the closest thing we have to that.
 *
 * D3 has no size limit and tells the operator to prune by hand.  We keep a
 * cap because we rewrite the whole file on every command and an unbounded
 * one would eventually cost real time at every prompt.
 *
 * Not implemented: the macro forms .C, .CO, `.X name` and `.X file name`.
 * They need a macro processor, which MVX has not got yet (#177).
 *
 * `!str` -- search the stack and execute -- is NOT taken: `!` is already the
 * documented shell escape here, and silently changing it would break the
 * thing an operator is most likely to have in a script. */

static int  macro_run(mv_value *f, const char *name, const char *args);
static int  macro_create(const char *name, const char *list, int overwrite);

#define STACK_MAX 500
static char *g_stack[STACK_MAX];
static int   g_nstack;                /* g_stack[0] is entry 1, the top */
static char  g_stackfile[4096];
static int   g_stack_depth;           /* .X re-entry guard */

/* Trim exactly as command() does before it dispatches.  Without this a
   stray trailing space makes "COUNT VOC " a different entry from
   "COUNT VOC", the uniqueness rule stops collapsing them, and the stack
   fills with near-duplicates -- which is the opposite of what it is for. */
static char *stack_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t'))
        s[--n] = '\0';
    return s;
}

static void stack_load(void) {
    if (!g_stackfile[0]) return;
    FILE *fp = fopen(g_stackfile, "rb");
    if (!fp) return;
    char buf[4096];
    while (g_nstack < STACK_MAX && fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        /* libedit wrote this file before the stack existed, and its own
           format leads with a version line.  Skip it rather than offering
           the operator "_HiStOrY_V2_" as entry 1. */
        if (n == 0 || strcmp(buf, "_HiStOrY_V2_") == 0) continue;
        g_stack[g_nstack++] = strdup(buf);
    }
    fclose(fp);
}

static void stack_save(void) {
    if (!g_stackfile[0]) return;
    FILE *fp = fopen(g_stackfile, "wb");
    if (!fp) return;
    for (int i = 0; i < g_nstack; i++) fprintf(fp, "%s\n", g_stack[i]);
    fclose(fp);
}

/* Add a command, D3-style: an identical entry anywhere is removed first, so
   the command ends up at the top exactly once. */
static void stack_push(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    for (int i = 0; i < g_nstack; i++)
        if (strcmp(g_stack[i], cmd) == 0) {
            free(g_stack[i]);
            memmove(&g_stack[i], &g_stack[i + 1],
                    (size_t)(g_nstack - i - 1) * sizeof g_stack[0]);
            g_nstack--;
            break;
        }
    if (g_nstack == STACK_MAX) { free(g_stack[--g_nstack]); }
    memmove(&g_stack[1], &g_stack[0], (size_t)g_nstack * sizeof g_stack[0]);
    g_stack[0] = strdup(cmd);
    g_nstack++;
    stack_save();
}

/* Move entry i (0-based) to the top.  Both .X and a successful edit do this. */
static void stack_totop(int i) {
    if (i <= 0 || i >= g_nstack) return;
    char *e = g_stack[i];
    memmove(&g_stack[1], &g_stack[0], (size_t)i * sizeof g_stack[0]);
    g_stack[0] = e;
}

static void stack_delete(int i) {
    if (i < 0 || i >= g_nstack) return;
    free(g_stack[i]);
    memmove(&g_stack[i], &g_stack[i + 1],
            (size_t)(g_nstack - i - 1) * sizeof g_stack[0]);
    g_nstack--;
}

static void stack_list(int from, int to) {     /* 1-based, inclusive */
    if (from < 1) from = 1;
    if (to > g_nstack) to = g_nstack;
    for (int i = from; i <= to; i++)
        printf("%3d %s\n", i, g_stack[i - 1]);
    fflush(stdout);
}

/* Replace str1 with str2 in s; `all` does every occurrence.  Returns a fresh
   string, or NULL when str1 does not appear (so the caller can say so rather
   than silently rewriting nothing). */
static char *stack_subst(const char *s, const char *a, const char *b, int all) {
    if (!a[0]) return NULL;
    size_t la = strlen(a), lb = strlen(b), n = 0;
    for (const char *p = s; (p = strstr(p, a)) != NULL; p += la) { n++; if (!all) break; }
    if (n == 0) return NULL;
    char *out = malloc(strlen(s) + n * (lb > la ? lb - la : 0) + 1);
    if (!out) return NULL;
    char *w = out;
    const char *p = s;
    while (*p) {
        const char *h = strstr(p, a);
        if (!h || (!all && w != out)) { strcpy(w, p); break; }
        memcpy(w, p, (size_t)(h - p)); w += h - p;
        memcpy(w, b, lb);              w += lb;
        p = h + la;
        if (!*p) *w = '\0';
    }
    return out;
}

#ifdef HAVE_EDITLINE
static char *el_prompt(EditLine *e) {   /* libedit asks for the prompt */
    (void)e;
    static char p[300];
    snprintf(p, sizeof p, "%s> ", g_acct_base);
    return p;
}
static EditLine *g_el;                /* .R needs to type INTO the next prompt */
static int g_recalled;                /* a stack entry is waiting in the line */
#endif

/* AN ATTACHED RECORD IS AN OFFER, NOT AN INSTRUCTION (mvx#238).
 *
 * Attribute 9 of a message carries, by value mark: file, id, program, mode.
 * Positions 5 and beyond are free for an application; the shell reads the
 * four it knows and ignores the rest, which is what lets the convention grow
 * without every reader having to be taught first.
 *
 * What the shell does with it is PUT THE COMMAND ON THE STACK and say so.
 * The record opens when the person at the terminal recalls it and presses
 * return -- never because a message arrived.  That distinction is the whole
 * design: a sender can hand you a screen, and cannot take yours. */
static void msg_offer(const char *payload) {
    if (!payload || !payload[0]) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", payload);
    char *v[8] = {0};
    int nv = 0;
    for (char *tok = buf; nv < 8; nv++) {
        v[nv] = tok;
        char *mark = strchr(tok, '\xfd');       /* value mark */
        if (!mark) { nv++; break; }
        *mark = '\0';
        tok = mark + 1;
    }
    const char *file = nv > 0 ? v[0] : "";
    const char *id = nv > 1 ? v[1] : "";
    const char *prog = nv > 2 ? v[2] : "";
    const char *mode = nv > 3 ? v[3] : "";
    if (!file[0] || !id[0]) return;             /* nothing openable */

    /* The sender says what it is FOR, and the verb follows from that: CT to
       look at a record, ED to change one.  A named program wins -- an
       application knows its own screens better than the shell does. */
    char cmd[1024];
    if (prog[0])
        snprintf(cmd, sizeof cmd, "%s %s %s", prog, file, id);
    else if (strcmp(mode, "edit") == 0)
        snprintf(cmd, sizeof cmd, "ED %s %s", file, id);
    else
        snprintf(cmd, sizeof cmd, "CT %s %s", file, id);

    stack_push(cmd);
    printf("     attached: %s %s — on the stack as \"%s\"", file, id, cmd);
#ifdef HAVE_EDITLINE
    if (g_el && isatty(0)) printf(" (.1 to recall)");
#endif
    printf("\n");
}

/* How long the prompt waits before looking in its inbox.  Short enough that
   an arriving message feels immediate, long enough that an idle shell is not
   doing anything worth measuring -- one PEEK over a unix socket per tick, and
   only while a session is actually registered. */
#define MSG_POLL_MS 250

static void drain_messages(void);

#ifdef HAVE_EDITLINE
/* Read a line, and let a message land ON the prompt while it is being typed
 * (mvx#238).
 *
 * The drain either side of el_gets already shows a message before the prompt
 * and after a verb; what it cannot do is show one that arrives while the
 * operator is sitting there, because el_gets is blocked in read().  So the
 * SHELL owns the wait: EL_UNBUFFERED hands libedit's blocking read back to
 * us, poll() waits on the terminal with a deadline, and a tick that finds
 * something prints it and asks the editor to redraw underneath.  A
 * half-typed command survives -- which is the same guarantee a full-screen
 * program gets, arrived at the same way: nothing writes to the screen except
 * between one keystroke and the next.
 *
 * STILL NO SIGNAL HANDLER.  This is a deadline on a poll, not an interrupt:
 * mv_input treats an interrupted read as EOF and read_byte cannot tell EINTR
 * from a timeout, so a signal-driven version of this would kill programs
 * sitting at INPUT.
 *
 * Pushed text (a stack recall, an M macro) goes the old way, because
 * EL_UNBUFFERED returns after ONE character of pushback and the loop would
 * have to know how many are left.  A recall waits for a keystroke to show a
 * message, which is exactly when the operator is looking anyway. */
static const char *prompt_gets(int *n, int pushed) {
    /* START FROM BUFFERED, ALWAYS.  libedit clears the line only when
       EL_UNBUFFERED changes, so a completed command has to be dropped before
       the next one begins -- without this every keystroke re-submits the
       previous line with one more character on the end. */
    el_set(g_el, EL_UNBUFFERED, 0);
    if (pushed || mvx_msg_port() <= 0) {
        /* No registry, or a recall to display: libedit does its own wait. */
        return el_gets(g_el, n);
    }
    el_set(g_el, EL_UNBUFFERED, 1);
    for (;;) {
        struct pollfd p = {0, POLLIN, 0};
        int r = poll(&p, 1, MSG_POLL_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            return el_gets(g_el, n);      /* poll is broken; wait the old way */
        }
        if (r == 0) {                     /* idle: this is the whole point */
            if (mvx_msg_pending() > 0) {
                drain_messages();
                el_set(g_el, EL_REFRESH); /* the half-typed line comes back */
            }
            continue;
        }
        const char *l = el_gets(g_el, n);
        if (!l) return NULL;
        /* Unbuffered: el_gets returns what it has, which is a whole line only
           once the return key is in it. */
        if (*n > 0 && memchr(l, '\n', (size_t)*n)) return l;
    }
}
#endif

static void drain_messages(void) {
    mv_value m;
    mv_init(&m);
    while (mvx_msg_pending() > 0) {
        mvx_msg_read(&m);
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(&m, nb, sizeof nb, &p);
        if (n <= 0) break;
        /* class \xfe port \xfe user \xfe account \xfe host \xfe sent \xfe text */
        char buf[1024];
        if ((size_t)n >= sizeof buf) n = (int64_t)sizeof buf - 1;
        memcpy(buf, p, (size_t)n);
        buf[n] = '\0';
        char *f[9] = {0};
        int nf = 0;
        for (char *tok = buf; nf < 9; nf++) {
            f[nf] = tok;
            char *mark = strchr(tok, '\xfe');
            if (!mark) { nf++; break; }
            *mark = '\0';
            tok = mark + 1;
        }
        long cls = nf > 1 ? strtol(f[1], NULL, 10) : 0;
        const char *port = nf > 2 ? f[2] : "?";
        const char *user = nf > 3 ? f[3] : "?";
        const char *text = nf > 7 ? f[7] : "";
        if (cls & 32)               /* signed: say who it is from */
            printf("\n[%s] %s: %s\n", port, user, text);
        else
            printf("\n%s\n", text);
        if (nf > 8) msg_offer(f[8]);
        if (cls & 16) fputc('\a', stdout);   /* bell */
        fflush(stdout);
    }
    mv_clear(&m);
}

/* Offer entry i for editing.  On a terminal that means seeding the next
   prompt with it, which is what "display and allow modification" means when
   the line editor is the editor.  Off a terminal there is nothing to edit
   into, so print it -- a script can then see what it would have got. */
static void stack_recall(int i) {
    if (i < 0 || i >= g_nstack) { printf("[1310] no such stack entry\n"); return; }
#ifdef HAVE_EDITLINE
    /* el_push types the entry into the next prompt, where it can be edited
       like anything else -- which is what the manual's "display and allow
       modification" means when the line editor IS the editor.
       (libedit's readline-compat layer cannot do this: it calls
       rl_startup_hook / rl_pre_input_hook but rl_insert_text from inside
       them never reaches the line buffer.  Measured, not assumed -- which is
       why this shell drives the native API.) */
    if (g_el && isatty(0)) {
        el_push(g_el, g_stack[i]);
        g_recalled = 1;               /* the next read must display it */
        return;
    }
#endif
    printf("%3d %s\n", i + 1, g_stack[i]);
    fflush(stdout);
}

static void stack_help(void) {
    printf(
      ".?              this list\n"
      ".L              list the stack\n"
      ".L n            list the top n entries\n"
      ".L m-n          list entries m through n\n"
      ".R              recall the top entry for editing\n"
      ".R n            recall entry n for editing\n"
      ".R n/old/new    replace old with new in entry n\n"
      ".RU n/old/new   replace every old with new in entry n\n"
      ".DE             delete the top entry\n"
      ".DE n           delete the top n entries\n"
      ".DE n/str       delete any of the top n entries containing str\n"
      ".X              execute the top entry\n"
      ".X n{,n}        execute entry n (and it moves to the top)\n"
      ".n{,n}          same as .X n\n"
      ".X name         execute macro `name` from VOC\n"
      ".X file name    execute macro `name` from `file`\n"
      ".C name n{,n}   make a macro from those stack entries\n"
      ".CO name n{,n}  the same, replacing one that exists\n");
    fflush(stdout);
}

static int command(char *line);       /* .X runs an entry back through dispatch */

/* Handle a dot command.  Returns 1 when the line was one (with *rc set),
   0 when it was not and normal dispatch should take it. */
static int stack_command(const char *line, int *rc) {
    if (line[0] != '.') return 0;
    *rc = 0;
    const char *p = line + 1;
    char op[4] = "";
    size_t on = 0;
    while (*p && !isdigit((unsigned char)*p) && *p != ' ' && on < sizeof op - 1)
        op[on++] = (char)toupper((unsigned char)*p++);
    op[on] = '\0';
    while (*p == ' ') p++;

    if (strcmp(op, "?") == 0) { stack_help(); return 1; }

    if (strcmp(op, "L") == 0) {
        if (!*p) { stack_list(1, g_nstack); return 1; }
        int m = 0, n = 0;
        if (sscanf(p, "%d-%d", &m, &n) == 2) stack_list(m, n);
        else if (sscanf(p, "%d", &n) == 1)   stack_list(1, n);
        else { printf("[1311] .L takes a count or a m-n range\n"); *rc = 2; }
        return 1;
    }

    if (strcmp(op, "R") == 0 || strcmp(op, "RU") == 0) {
        int all = op[1] == 'U';
        if (!*p) { stack_recall(0); return 1; }
        int n = 0;
        const char *slash = strchr(p, '/');
        if (sscanf(p, "%d", &n) != 1 || n < 1 || n > g_nstack) {
            printf("[1310] no such stack entry\n"); *rc = 2; return 1;
        }
        if (!slash) { stack_recall(n - 1); return 1; }
        char a[512] = "", b[512] = "";
        const char *s2 = strchr(slash + 1, '/');
        if (!s2) { printf("[1311] .R n/old/new needs both parts\n"); *rc = 2; return 1; }
        snprintf(a, sizeof a, "%.*s", (int)(s2 - slash - 1), slash + 1);
        snprintf(b, sizeof b, "%s", s2 + 1);
        char *nw = stack_subst(g_stack[n - 1], a, b, all);
        if (!nw) { printf("[1312] \"%s\" is not in entry %d\n", a, n); *rc = 2; return 1; }
        free(g_stack[n - 1]);
        g_stack[n - 1] = nw;
        stack_totop(n - 1);            /* an edited entry moves to the top */
        stack_save();
        stack_list(1, 1);
        return 1;
    }

    if (strcmp(op, "DE") == 0) {
        if (!*p) { stack_delete(0); stack_save(); return 1; }
        int n = 0;
        const char *slash = strchr(p, '/');
        if (sscanf(p, "%d", &n) != 1 || n < 1) {
            printf("[1311] .DE takes a count\n"); *rc = 2; return 1;
        }
        if (n > g_nstack) n = g_nstack;
        if (slash) {                   /* only those of the top n containing str */
            for (int i = n - 1; i >= 0; i--)
                if (strstr(g_stack[i], slash + 1)) stack_delete(i);
        } else {
            for (int i = 0; i < n; i++) stack_delete(0);
        }
        stack_save();
        return 1;
    }

    if (strcmp(op, "C") == 0 || strcmp(op, "CO") == 0) {
        /* .C name n{,n} -- the manual's table writes this as ".C n{,n}" and
           omits the name, but create-macro takes one and .X needs one to
           call, so the name comes first here.  Stated rather than guessed
           at silently (#177). */
        char nm[128] = "";
        int k = 0;
        while (*p && *p != ' ' && k < (int)sizeof nm - 1) nm[k++] = *p++;
        nm[k] = '\0';
        while (*p == ' ') p++;
        if (!nm[0] || !*p) {
            printf("[1311] .%s takes a macro name and stack entries\n", op);
            *rc = 2; return 1;
        }
        *rc = macro_create(nm, p, strcmp(op, "CO") == 0);
        return 1;
    }

    /* .X, .X n{,n} and the bare .n{,n} the manual writes as .{X} n{,n} */
    int isx = strcmp(op, "X") == 0;
    if (!isx && op[0] != '\0') return 0;      /* .SOMETHINGELSE is not ours */

    /* .X name, and .X file name -- an argument that is not a number is a
       macro, not a stack entry. */
    if (isx && *p && !isdigit((unsigned char)*p)) {
        char w1[256] = "", w2[256] = "";
        int k = 0;
        while (*p && *p != ' ' && k < (int)sizeof w1 - 1) w1[k++] = *p++;
        w1[k] = '\0';
        while (*p == ' ') p++;
        k = 0;
        while (*p && *p != ' ' && k < (int)sizeof w2 - 1) w2[k++] = *p++;
        w2[k] = '\0';
        mv_value f;
        const char *nm;
        if (w2[0]) {                       /* .X file name */
            if (voc_open(&f, w1) <= 0) {
                printf("[1325] cannot open file \"%s\"\n", w1);
                *rc = 2; return 1;
            }
            nm = w2;
        } else {
            if (g_voc_state == 0) g_voc_state = voc_open(&g_voc, "VOC");
            if (g_voc_state <= 0) { printf("[1323] no VOC in this account\n");
                                    *rc = 2; return 1; }
            f = g_voc;
            nm = w1;
        }
        int r = macro_run(&f, nm, NULL);
        if (w2[0]) mv_clear(&f);
        if (r < 0) { printf("[1326] no macro \"%s\"\n", nm); *rc = 2; }
        else *rc = r;
        return 1;
    }
    if (g_stack_depth > 8) {
        printf("[1313] the stack is executing itself; stopping\n");
        *rc = 2; return 1;
    }
    if (!*p) {                                /* .X -- the top entry */
        if (g_nstack == 0) { printf("[1310] the stack is empty\n"); *rc = 2; return 1; }
        char *cp = strdup(g_stack[0]);
        printf("%s\n", cp);
        g_stack_depth++;
        *rc = command(cp);
        g_stack_depth--;
        free(cp);
        return 1;
    }
    for (const char *q = p; *q; ) {           /* n{,n} */
        int n = 0;
        if (sscanf(q, "%d", &n) != 1 || n < 1 || n > g_nstack) {
            printf("[1310] no such stack entry\n"); *rc = 2; return 1;
        }
        char *cp = strdup(g_stack[n - 1]);
        stack_totop(n - 1);                   /* .X pops the entry to the top */
        stack_save();
        printf("%s\n", cp);
        g_stack_depth++;
        *rc = command(cp);
        g_stack_depth--;
        free(cp);
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
    }
    return 1;
}


/* ------------------------------------------------------------ #177
 * TCL macros.
 *
 * D3's model, from the Pick Systems Reference Manual entries "macros" and
 * "create-macro":
 *
 *   001  M{ comment}   or   N{ comment}
 *   002  <a TCL command>
 *   003  <another>
 *
 * Stored in the master dictionary -- VOC here -- under the macro's own name,
 * and run by typing that name.  `N` (non-stop) runs each command straight
 * off; `M` (modify) shows each one at the prompt first so it can be edited
 * before it goes, which is the whole point of the type.  Parameters typed
 * after the name are appended to the FIRST command only; the manual is
 * explicit that they do not reach the others.
 *
 * Not implemented: the manual also says additional VALUES in an attribute
 * are stacked input to that attribute's command.  Feeding a verb its stdin
 * means a pipe through the fork/exec path, which is a separate mechanism
 * from anything here, so a macro with multivalued attributes runs value 1
 * and says so rather than quietly dropping the rest. */

#define MACRO_MAXCMD 64
static char *g_mqueue[MACRO_MAXCMD];    /* an M macro's pending commands */
static int   g_mqn, g_mqi;

/* Read `name` from `f` and, if it is a macro, hand back its record and mode.
   Returns 'M', 'N', or 0 when the item is missing or is not a macro. */
static char macro_read(mv_value *f, const char *name, mv_value *rec) {
    mv_value id, a1;
    mv_init(&id); mv_init(&a1);
    mv_set_str(&id, name, (int64_t)strlen(name));
    char mode = 0;
    if (mvx_read(g_ctx, rec, f, &id, 0)) {
        mv_extract_fn(&a1, rec, 1, 0, 0);
        char nb[64];
        const char *p;
        int64_t n = mv_val_chars(&a1, nb, sizeof nb, &p);
        /* "M" or "N", optionally followed by a blank and a comment. */
        if (n >= 1 && (n == 1 || p[1] == ' ')) {
            char c = (char)toupper((unsigned char)p[0]);
            if (c == 'M' || c == 'N') mode = c;
        }
    }
    mv_clear(&id); mv_clear(&a1);
    return mode;
}

/* Queue an M macro's commands so each is offered at a prompt in turn. */
static void macro_queue(char **cmds, int n) {
    for (int i = 0; i < g_mqn; i++) free(g_mqueue[i]);
    g_mqn = g_mqi = 0;
    for (int i = 0; i < n && i < MACRO_MAXCMD; i++)
        g_mqueue[g_mqn++] = strdup(cmds[i]);
}

/* Run a macro.  `args` is whatever followed the name and goes on the first
   command; `f` is the file it came from. */
static int macro_run(mv_value *f, const char *name, const char *args) {
    mv_value rec;
    mv_init(&rec);
    char mode = macro_read(f, name, &rec);
    if (!mode) { mv_clear(&rec); return -1; }

    char *cmds[MACRO_MAXCMD];
    int n = 0, truncated = 0, stacked = 0;
    for (int64_t a = 2; n < MACRO_MAXCMD; a++) {
        mv_value at, v1;
        mv_init(&at); mv_init(&v1);
        mv_extract_fn(&at, &rec, a, 0, 0);
        char nb[64];
        const char *p;
        int64_t ln = mv_val_chars(&at, nb, sizeof nb, &p);
        if (ln <= 0) { mv_clear(&at); mv_clear(&v1); break; }
        mv_extract_fn(&v1, &rec, a, 1, 0);      /* value 1 is the command */
        mv_value vm;                            /* is there stacked input? */
        mv_init(&vm);
        mv_extract_fn(&vm, &rec, a, 2, 0);
        char vb[8];
        const char *vp;
        if (mv_val_chars(&vm, vb, sizeof vb, &vp) > 0) stacked = 1;
        mv_clear(&vm);
        char cb[4096];
        const char *cp;
        int64_t cl = mv_val_chars(&v1, cb, sizeof cb, &cp);
        char *cmd = malloc((size_t)cl + (args ? strlen(args) : 0) + 2);
        if (cmd) {
            memcpy(cmd, cp, (size_t)cl);
            cmd[cl] = '\0';
            /* the manual: parameters reach the first command and no other */
            if (n == 0 && args && args[0]) { strcat(cmd, " "); strcat(cmd, args); }
            cmds[n++] = cmd;
        }
        mv_clear(&at); mv_clear(&v1);
        if (n == MACRO_MAXCMD) truncated = 1;
    }
    mv_clear(&rec);
    if (n == 0) { printf("[1320] macro \"%s\" has no commands\n", name); return 2; }
    if (stacked)
        printf("[1321] \"%s\": stacked input (extra values) is not run\n", name);
    if (truncated)
        printf("[1322] \"%s\": only the first %d commands were taken\n",
               name, MACRO_MAXCMD);

    int rc = 0;
    if (mode == 'M' && isatty(0)) {
        macro_queue(cmds, n);               /* offered at the prompt, in turn */
    } else {
        /* N, or M with nothing to display into: run them.  An M macro off a
           terminal still says what it is running, since that is the half of
           "display then execute" that survives without a prompt. */
        for (int i = 0; i < n; i++) {
            if (mode == 'M') { printf("%s\n", cmds[i]); fflush(stdout); }
            char *cp = strdup(cmds[i]);
            rc = command(cp);
            free(cp);
        }
    }
    for (int i = 0; i < n; i++) free(cmds[i]);
    return rc;
}

/* Build a macro from stack entries and file it in VOC. */
static int macro_create(const char *name, const char *list, int overwrite) {
    if (g_voc_state == 0) g_voc_state = voc_open(&g_voc, "VOC");
    if (g_voc_state <= 0) { printf("[1323] no VOC in this account\n"); return 2; }
    mv_value existing;
    mv_init(&existing);
    char had = macro_read(&g_voc, name, &existing);
    mv_clear(&existing);
    if (had && !overwrite) {
        printf("[415] '%s' exists on file.\n", name);   /* D3's own message */
        return 2;
    }
    mv_value rec, part;
    mv_init(&rec); mv_init(&part);
    mv_set_str(&rec, "M", 1);              /* create-macro's default type */
    int64_t a = 2;
    for (const char *q = list; *q; ) {
        int nth = 0;
        if (sscanf(q, "%d", &nth) != 1 || nth < 1 || nth > g_nstack) {
            printf("[1310] no such stack entry\n");
            mv_clear(&rec); mv_clear(&part);
            return 2;
        }
        mv_set_str(&part, g_stack[nth - 1], (int64_t)strlen(g_stack[nth - 1]));
        mv_replace_fn(&rec, &rec, a++, 0, 0, &part);
        while (*q && *q != ',') q++;
        if (*q == ',') q++;
    }
    if (a == 2) { printf("[1311] .C needs at least one stack entry\n");
                  mv_clear(&rec); mv_clear(&part); return 2; }
    mv_value id;
    mv_init(&id);
    mv_set_str(&id, name, (int64_t)strlen(name));
    /* mvx_write answers 0 for success and -2 for failure, so a plain
       truthiness test reads it exactly backwards -- it reported a failure
       for every macro it had just filed correctly. */
    int ok = mvx_write(g_ctx, &rec, &g_voc, &id, 0, 1) >= 0;
    mv_clear(&id); mv_clear(&rec); mv_clear(&part);
    if (!ok) { printf("[1324] could not file macro \"%s\"\n", name); return 2; }
    printf("%s created\n", name);          /* D3's own wording */
    return 0;
}

static int command(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '))
        line[--len] = '\0';
    if (len == 0) return 0;

    /* The command stack first: a dot command operates ON the stack and is
       not itself an entry, or .L would push .L and the operator would be
       reading their own bookkeeping back (#114). */
    int srsc = 0;
    if (stack_command(line, &srsc)) return srsc;

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
        /* THE RUNTIME OWNS THE MOVE (mvx#258).  It has to: a BASIC shell that
           replaces this one calls LOGTO() and an EXECUTE "LOGTO ..." from a
           program reaches the same entry point, so a copy here would be a
           second implementation of the hard part -- letting the old account
           go before entering the new one, and forgetting everything resolved
           per account.  What is left is the shell's own: its VOC handle for
           macros, and telling the operator where they now are. */
        if (!mvx_logto(g_ctx, arg)) return 2;   /* which runs its LOGIN */
        account_refresh();
        g_voc_state = 0;                /* the shell's own VOC, for .C macros */
        printf("now in account %s (%s)\n", g_acct_base, g_acct_path);
        fflush(stdout);
        return 0;
    }

    char path[1024];
    int r = voc_lookup(verb, path, sizeof path);
    if (r > 0)
        return run_verb(path, line);

    /* Not a verb -- but the same VOC name may be a macro, which is how a
       macro is activated in D3: you type its name.  D3 additionally makes
       an M-type name need quotes; we accept the quoted form so that habit
       still works, and the bare one too, because the quoting rule is an
       artefact of D3's parser rather than something worth reproducing. */
    if (g_voc_state == 0) g_voc_state = voc_open(&g_voc, "VOC");
    if (g_voc_state > 0) {
        char mname[128];
        const char *mp = line;
        size_t mn = 0;
        int quoted = (*mp == '"');
        if (quoted) mp++;
        while (*mp && (quoted ? *mp != '"' : (*mp != ' ' && *mp != '\t')) &&
               mn < sizeof mname - 1)
            mname[mn++] = *mp++;
        mname[mn] = '\0';
        if (quoted && *mp == '"') mp++;
        while (*mp == ' ' || *mp == '\t') mp++;
        if (mn && macro_run(&g_voc, mname, mp) >= 0) return 0;
    }
    if (r < 0)
        fprintf(stderr, "mvx: no VOC found in this account or the "
                        "system account (%s); only builtins are "
                        "available\n", system_dir());
    else
        fprintf(stderr, "verb \"%s\" not found\n", verb);
    return 127;                          /* command not found (Unix convention) */
}


/* WHICH BACKEND HOLDS THIS ACCOUNT'S VOC (#187).
 *
 * VOC is the bootstrap file: it has to be opened before anything that could
 * describe it, so `.mvx` names it (`voc = <driver>`).  An account made before
 * that key existed names nothing, and since the default for an undeclared
 * account is now sqlite, such an account would be looked for in the wrong
 * place.
 *
 * So: if `.mvx` does not say, try lmdb -- which is what every account made
 * before this used -- and if VOC is there, record it and carry on.  The
 * account is fixed once and never asks again.
 *
 * If it is NOT there, we genuinely do not know, and guessing would put the
 * account's files somewhere nobody chose.  Ask, on a terminal.  Off one, say
 * so and change nothing: the same rule the driver-substitution prompt already
 * follows, because a prompt into a closed pipe is not a question.
 */
static void mvx_append_descriptor(const char *key, const char *val) {
    FILE *fp = fopen(".mvx", "a");
    if (!fp) return;
    fprintf(fp, "%s = %s\n", key, val);
    fclose(fp);
}

static void voc_backend_settle(void) {
    char declared[64];
    mvx_account_voc(declared, sizeof declared);
    if (declared[0]) return;                  /* already answered */
    if (!has_descriptor()) return;            /* not an account (yet) */

    if (mvx_backend_has_file("lmdb", "VOC")) { /* an account from before the key */
        mvx_append_descriptor("voc", "lmdb");
        return;
    }
    if (!isatty(0)) return;                   /* no terminal, no guessing */

    char avail[512];
    mvx_driver_names(avail, sizeof avail);
    for (char *c = avail; *c; c++) if (*c == ',') *c = ' ';
    printf("This account does not say which backend holds its VOC.\n");
    printf("  available: %s\n", avail[0] ? avail : "(none)");
    printf("  VOC backend (blank to leave unset): ");
    fflush(stdout);
    char ans[64];
    if (!read_line_raw(ans, sizeof ans)) return;
    size_t n = strlen(ans);
    while (n && (ans[n-1] == '\n' || ans[n-1] == '\r' || ans[n-1] == ' ')) ans[--n] = '\0';
    if (!ans[0]) return;
    if (!mvx_driver_available(ans)) {
        printf("  \"%s\" is not available here; leaving it unset.\n", ans);
        return;
    }
    mvx_append_descriptor("voc", ans);
    printf("  recorded voc = %s in .mvx\n", ans);
}

int main(int argc, char **argv) {
    const char *acct = NULL;
    const char *one_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 && i + 1 < argc)
            acct = argv[++i];
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            one_cmd = argv[++i];
        else if (strcmp(argv[i], "--version") == 0) {
            /* The ABI belongs beside the version: it is what decides
               whether a compiled artifact still loads (#117). */
            printf("mvx %s (driver ABI %d)\n", mvx_version(), MVX_DRIVER_ABI);
            return 0;
        }
        else {
            fprintf(stderr, "usage: mvx [-a account] [-c command] [--version]\n");
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

    /* Register with the session registry, if one is running (mvx#226).  This
       is what gives the session its port -- @USERNO, WHO, and later the
       address a message is sent to.  The CONNECTION is the lease: it stays
       open for the life of the shell and the daemon frees the port when it
       drops, so a killed session needs no cleanup.

       A nested TCL (EXECUTE) inherits MVXMSGSESSION and attaches to the port
       its parent already holds rather than taking a second one.  With no
       daemon running this does nothing at all, quietly, and the shell behaves
       exactly as it did before. */
    if (mvx_msg_register() > 0) {
        char portbuf[16];
        snprintf(portbuf, sizeof portbuf, "%lld", (long long)mvx_msg_port());
        setenv("MVXPORT", portbuf, 1);
        setenv("MVXMSGSESSION", mvx_msg_session_id(), 1);
        setenv("MVXMSGTOKEN", mvx_msg_session_token(), 1);
    }

    /* upgrade a pre-.mvx account so the descriptor becomes canonical */
    if (!has_descriptor() && has_markers())
        write_descriptor(g_acct_base);
    voc_backend_settle();     /* and say which backend holds its VOC (#187) */

    /* THE ACCOUNT SETS ITSELF UP (mvx#264).  Entering an account runs its own
       VOC LOGIN, which is what UniData and UniVerse both do -- here and in
       mvx_logto, the two places a session enters one. Before the stack loads
       and before any -c command, so a LOGIN can prepare what they use. */
    mvx_login_run(g_ctx);

    /* Resolve and load the stack before anything runs.  Not gated on a
       terminal: .L and .X have to work down a pipe too, or the feature is
       both untestable and unavailable to a script.  MVXSTACK exists so a
       test can point somewhere other than the operator's real stack. */
    {
        const char *sf = getenv("MVXSTACK");
        if (sf && sf[0])
            snprintf(g_stackfile, sizeof g_stackfile, "%s", sf);
        else {
            const char *home = getenv("HOME");
            if (home && home[0])
                snprintf(g_stackfile, sizeof g_stackfile, "%s/.mvx_history", home);
        }
        stack_load();
    }

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

    /* One history, not two.  The stack and the line editor's up-arrow used
       to be separate lists over the same commands, so .L and ^P could
       disagree about what you had just run (#114).  The stack is the only
       store now; the editor is seeded from it, oldest first so the newest
       entry is the first one up-arrow reaches. */
#ifdef HAVE_EDITLINE
    History  *elh = NULL;
    HistEvent elev;
    if (tty) {
        g_el = el_init("mvx", stdin, stdout, stderr);
        elh  = history_init();
        history(elh, &elev, H_SETSIZE, STACK_MAX);
        el_set(g_el, EL_PROMPT, el_prompt);
        el_set(g_el, EL_EDITOR, "emacs");
        el_set(g_el, EL_SIGNAL, 1);
        el_set(g_el, EL_HIST, history, elh);
        /* oldest first, so the newest entry is the first one up-arrow reaches */
        for (int i = g_nstack - 1; i >= 0; i--)
            history(elh, &elev, H_ENTER, g_stack[i]);
    }
#endif

    char line[4096];
    for (;;) {
#ifdef HAVE_EDITLINE
        if (tty) {
            /* An M macro's commands wait here: each is typed into the
               prompt in turn so it can be edited before it goes, which is
               what the M type is for (#177). */
            drain_messages();
            account_refresh();   /* a verb may have LOGTO'd (mvx#258) */
            int pushed = 0;
            if (g_mqi < g_mqn) { el_push(g_el, g_mqueue[g_mqi++]); pushed = 1; }
            else if (g_mqn) { for (int i = 0; i < g_mqn; i++) free(g_mqueue[i]);
                              g_mqn = g_mqi = 0; }
            if (g_recalled) { pushed = 1; g_recalled = 0; }
            int eln = 0;
            const char *l = prompt_gets(&eln, pushed);
            if (!l || eln <= 0) break;
            snprintf(line, sizeof line, "%s", l);
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll-1] == '\n' || line[ll-1] == '\r'))
                line[--ll] = '\0';
            /* Push before running, so a command that fails is still recallable
               -- D3 stacks what was typed, not what worked, and a typo is
               exactly what you want back to fix. */
            char *t = stack_trim(line);
            if (t[0] && t[0] != '.') {
                stack_push(t);
                history(elh, &elev, H_ENTER, t);
            }
            command(line);
            drain_messages();        /* a long verb's messages, the moment it ends */
            continue;
        }
#else
        if (tty) {
            drain_messages();
            account_refresh();   /* a verb may have LOGTO'd (mvx#258) */
            printf("%s> ", g_acct_base);
            fflush(stdout);
        }
#endif
        if (!read_line_raw(line, sizeof line)) break;
        {   /* same rule as the interactive branch: what is entered at the
               prompt is stacked, and a dot command is not an entry */
            char tmp[sizeof line];
            snprintf(tmp, sizeof tmp, "%s", line);
            char *t = stack_trim(tmp);
            if (t[0] && t[0] != '.') stack_push(t);
        }
        command(line);
        /* Not only on a terminal: a scripted session is exactly where an
           unnoticed message would matter, and the suite drives TCL through a
           pipe. */
        drain_messages();
    }
    if (tty) fputc('\n', stdout);
#ifdef HAVE_EDITLINE
    if (g_el) { el_end(g_el); g_el = NULL; }
    if (elh) history_end(elh);
#endif
    mvx_ctx_destroy(g_ctx);
    if (sesspath[0]) unlink(sesspath);
    return 0;
}
