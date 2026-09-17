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

/* Fine-grained OS-command permissions (ARCHITECTURE.md 8.4).
 *
 * The privilege tiers (mvx_exec.c) are all-or-nothing: raw Unix needs the
 * whole `unrestricted` tier — full shell.  This adds a narrow path: a
 * program may run a SPECIFIC external command (argv-style, never a shell)
 * when a grant permits it, without unrestricted.
 *
 * Grants are group-scoped whitelist/denylist lines:
 *
 *     permit <group> = mkdir tar rm         # this group may run these
 *     permit *       = uname                 # * = any user/group
 *     deny   <group> = rm -r -R --recursive  # ...but never rm WITH these switches
 *     deny   *       = shutdown               # ...and never this command at all
 *
 * A `permit` grants a command (any arguments).  A `deny` is a hard override
 * that wins over any permit: with no switches it forbids the command outright,
 * with switches it forbids the command only when one of those switches is
 * present.  Switches match by identity, INCLUDING bundled short options — a
 * deny of `-r` also blocks `-fr` / `-rf` — and long options (`--recursive`,
 * `--recursive=…`).  A command is matched by basename (`tar` covers `/bin/tar`).
 *
 * Grants come from three files, unioned, in increasing authority:
 *   1. `<account>/.mvx`                     — the packager's declaration.
 *   2. `<account>/.mvx-private/permissions` — the account's site policy
 *                                             (git-ignored, fs-protected).
 *   3. `<system>/.mvx-private/permissions`  — the SYSTEM-account layer: the
 *                                             admin's per-user/group override,
 *                                             outside every account so it cannot
 *                                             be self-granted (8.3).  <system>
 *                                             is mvx_system_dir().
 * A `deny` anywhere wins over any `permit`, so the system layer can lock a
 * command (or a switch) down for a user/group regardless of what an account
 * grants itself; the account files can only NARROW, not escalate past a system
 * deny.  A grant's `<who>` matches the caller's OS groups OR their username
 * (so the system layer can scope per user), plus `*` for anyone.
 *
 * A grant may instead be bound to a PROGRAM's identity (8.4 constraint 3):
 *
 *     permit prog:MVPKG = mkdir tar mkpkg   # the program blessed as MVPKG
 *
 * applies for ANY user, but only when the running verb's binary matches the
 * program blessed as that name.  Blessings live ONLY in the system layer
 * (`<system>/.mvx-private/programs`), so a user cannot self-bless:
 *
 *     program MVPKG = <sha256-of-approved-binary>
 *
 * The check hashes the running binary (sha256) and compares.  Re-cataloging the
 * program changes the binary, hence its hash — the grant no longer matches until
 * the admin re-blesses the new hash.
 */
#include "mvx_runtime.h"
#include "sha256.h"

#include <grp.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>            /* _NSGetExecutablePath */
#endif

#define MAX_RULES 512
#define MAX_GROUPS 256
#define GRP_LEN 64
#define CMD_LEN 128
#define SHORTS_LEN 64
#define MAX_LONGS 8
#define LONG_LEN 48

/* A rule: a group, a command (basename), and — for a deny with switches — the
   short-option letters and long-option names it forbids (empty => the whole
   command). */
struct rule {
    char who[GRP_LEN];      /* a group/username, "*", or (is_prog) a program name */
    int is_prog;            /* who names a blessed program, matched by binary hash */
    char cmd[CMD_LEN];
    char shorts[SHORTS_LEN];
    char longs[MAX_LONGS][LONG_LEN];
    int nlongs;
    int has_switches;
};

static struct rule permits[MAX_RULES];
static int npermits = 0;
static struct rule denies[MAX_RULES];
static int ndenies = 0;
/* Blessed programs: name -> the sha256 (hex) of its approved binary.  Only the
   system layer may bless (admins whitelist a program), so no self-blessing. */
static struct { char name[GRP_LEN]; char hash[65]; } blessings[MAX_RULES];
static int nblessings = 0;
static char mygroups[MAX_GROUPS][GRP_LEN];
static int nmygroups = 0;
static int loaded = 0;

static const char *acct_root(void) {
    const char *a = getenv("MVXACCOUNT");
    return (a && a[0]) ? a : ".";
}

/* Copy the next whitespace/comma-delimited token; advance *p.  0 if none. */
static int next_tok(const char **p, char *out, size_t cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t' || *s == ',') s++;
    const char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != ',' && *e != '\n' && *e != '\r') e++;
    size_t n = (size_t)(e - s);
    if (n == 0) { *p = e; return 0; }
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    *p = e;
    return 1;
}

/* Add one switch token ("-r", "-fr", "--recursive", "--foo=bar") to a rule. */
static void rule_add_switch(struct rule *r, const char *sw) {
    if (sw[0] == '-' && sw[1] == '-') {                 /* long option */
        char nm[LONG_LEN];
        int n = 0;
        for (const char *c = sw + 2; *c && *c != '=' && n < LONG_LEN - 1; c++) nm[n++] = *c;
        nm[n] = '\0';
        if (n && r->nlongs < MAX_LONGS) { snprintf(r->longs[r->nlongs++], LONG_LEN, "%s", nm); r->has_switches = 1; }
    } else if (sw[0] == '-' && sw[1]) {                 /* short option(s) */
        size_t have = strlen(r->shorts);
        for (const char *c = sw + 1; *c && have < SHORTS_LEN - 1; c++)
            if (!strchr(r->shorts, *c)) { r->shorts[have++] = *c; r->shorts[have] = '\0'; r->has_switches = 1; }
    }
}

/* Set a rule's subject from its token: a `prog:<name>` binds it to a blessed
   program (matched by binary hash); anything else is a group/username/`*`. */
static void set_who(struct rule *r, const char *tok) {
    if (strncmp(tok, "prog:", 5) == 0) { r->is_prog = 1; snprintf(r->who, GRP_LEN, "%s", tok + 5); }
    else snprintf(r->who, GRP_LEN, "%s", tok);
}

static void parse_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    char line[4096];
    while (fgets(line, sizeof line, fp)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        int is_permit = strncmp(p, "permit", 6) == 0 && (p[6] == ' ' || p[6] == '\t');
        int is_deny = strncmp(p, "deny", 4) == 0 && (p[4] == ' ' || p[4] == '\t');
        if (!is_permit && !is_deny) continue;
        p += is_permit ? 6 : 4;
        char grp[GRP_LEN];
        if (!next_tok(&p, grp, sizeof grp)) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        if (is_permit) {
            /* whole commands, any arguments */
            char cmd[CMD_LEN];
            while (next_tok(&p, cmd, sizeof cmd)) {
                if (npermits >= MAX_RULES) break;
                struct rule *r = &permits[npermits++];
                memset(r, 0, sizeof *r);
                set_who(r, grp);
                snprintf(r->cmd, CMD_LEN, "%s", cmd);
            }
        } else {
            /* one command, then the switches it is denied with (none = all) */
            char cmd[CMD_LEN];
            if (!next_tok(&p, cmd, sizeof cmd)) continue;
            if (ndenies >= MAX_RULES) continue;
            struct rule *r = &denies[ndenies++];
            memset(r, 0, sizeof *r);
            set_who(r, grp);
            snprintf(r->cmd, CMD_LEN, "%s", cmd);
            char sw[LONG_LEN + 4];
            while (next_tok(&p, sw, sizeof sw)) rule_add_switch(r, sw);
        }
    }
    fclose(fp);
}

/* Parse `program <name> = <sha256-hex>` lines — the system-layer bless list
   that maps a program name to its approved binary's hash. */
static void parse_blessings(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    char line[4096];
    while (fgets(line, sizeof line, fp)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "program", 7) != 0 || (p[7] != ' ' && p[7] != '\t')) continue;
        p += 7;
        char name[GRP_LEN];
        if (!next_tok(&p, name, sizeof name)) continue;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        char h[80];
        if (!next_tok(&p, h, sizeof h)) continue;
        if (nblessings >= MAX_RULES) continue;
        snprintf(blessings[nblessings].name, GRP_LEN, "%s", name);
        snprintf(blessings[nblessings].hash, 65, "%s", h);
        nblessings++;
    }
    fclose(fp);
}

/* The caller's OS groups, by name (supplementary + primary). */
static void load_groups(void) {
    int ng = getgroups(0, NULL);
    gid_t *gids = NULL;
    if (ng > 0) {
        gids = malloc((size_t)ng * sizeof(gid_t));
        if (gids && getgroups(ng, gids) < 0) ng = 0;
    } else {
        ng = 0;
    }
    struct passwd *pw = getpwuid(getuid());
    gid_t primary = pw ? pw->pw_gid : getgid();
    int have_primary = 0;
    for (int i = 0; i < ng; i++) {
        if (gids[i] == primary) have_primary = 1;
        struct group *gr = getgrgid(gids[i]);
        if (gr && gr->gr_name && nmygroups < MAX_GROUPS)
            snprintf(mygroups[nmygroups++], GRP_LEN, "%s", gr->gr_name);
    }
    if (!have_primary && nmygroups < MAX_GROUPS) {
        struct group *gr = getgrgid(primary);
        if (gr && gr->gr_name) snprintf(mygroups[nmygroups++], GRP_LEN, "%s", gr->gr_name);
    }
    /* the username is also an identity a grant may name (per-user scoping) */
    if (pw && pw->pw_name && nmygroups < MAX_GROUPS)
        snprintf(mygroups[nmygroups++], GRP_LEN, "%s", pw->pw_name);
    free(gids);
}

static void load(void) {
    if (loaded) return;
    loaded = 1;
    char path[4096];
    snprintf(path, sizeof path, "%s/.mvx", acct_root());
    parse_file(path);
    snprintf(path, sizeof path, "%s/.mvx-private/permissions", acct_root());
    parse_file(path);
    /* the system-account layer — the admin's authoritative per-user/group
       override, outside every account (8.3). */
    /* Where every other part of the runtime looks (mvx#210).  This used to
       fall back to the build tree baked in at configure time, so on an
       installed toolchain the system layer -- the admin's overrides and the
       program blessings -- was silently never read. */
    const char *sys = mvx_system_dir();
    if (sys && sys[0]) {
        snprintf(path, sizeof path, "%s/.mvx-private/permissions", sys);
        parse_file(path);
        snprintf(path, sizeof path, "%s/.mvx-private/programs", sys);
        parse_blessings(path);
    }
    load_groups();
}

static int in_my_groups(const char *g) {
    if (strcmp(g, "*") == 0) return 1;
    for (int i = 0; i < nmygroups; i++)
        if (strcmp(mygroups[i], g) == 0) return 1;
    return 0;
}

/* The sha256 (hex) of the running verb's own binary — its identity — cached.
   "" if it cannot be determined (then no prog: grant can match). */
static const char *self_hash(void) {
    static char hex[65];
    static int done = 0;
    if (done) return hex;
    done = 1;
    hex[0] = '\0';
    char path[4096];
#ifdef __APPLE__
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) return hex;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return hex;
    path[n] = '\0';
#endif
    FILE *fp = fopen(path, "rb");
    if (!fp) return hex;
    sha256_ctx c;
    sha256_init(&c);
    unsigned char buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, fp)) > 0) sha256_update(&c, buf, r);
    fclose(fp);
    uint8_t dig[32];
    sha256_final(&c, dig);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dig[i]);
    return hex;
}

/* Does the running binary match the program blessed as `name`? */
static int is_blessed_program(const char *name) {
    const char *sh = self_hash();
    if (!sh[0]) return 0;
    for (int i = 0; i < nblessings; i++)
        if (strcmp(blessings[i].name, name) == 0 && strcmp(blessings[i].hash, sh) == 0)
            return 1;
    return 0;
}

/* Does a rule's subject match the current caller — a group/username/`*`, or a
   blessed program (by binary identity)? */
static int who_matches(const struct rule *r) {
    return r->is_prog ? is_blessed_program(r->who) : in_my_groups(r->who);
}

static const char *base_of(const char *cmd) {
    const char *slash = strrchr(cmd, '/');
    return slash ? slash + 1 : cmd;
}

/* Does argv element `e` (a switch) match one of the rule's forbidden switches?
   Handles bundled shorts (-fr matches a denied -r) and long options. */
static int switch_hits(const struct rule *r, const char *e) {
    if (e[0] != '-' || e[1] == '\0') return 0;          /* not a switch ("-" is stdin) */
    if (e[1] == '-') {                                  /* long: --name[=val] */
        if (e[2] == '\0') return 0;                     /* "--" ends options */
        char nm[LONG_LEN];
        int n = 0;
        for (const char *c = e + 2; *c && *c != '=' && n < LONG_LEN - 1; c++) nm[n++] = *c;
        nm[n] = '\0';
        for (int i = 0; i < r->nlongs; i++)
            if (strcmp(nm, r->longs[i]) == 0) return 1;
        return 0;
    }
    for (const char *c = e + 1; *c; c++)                /* bundled short letters */
        if (strchr(r->shorts, *c)) return 1;
    return 0;
}

/* 1 if the caller's groups may run argv[0] with these arguments; a permit must
   grant the command AND no matching deny rule may fire. */
int mvx_perm_allowed(char *const argv[]) {
    if (!argv || !argv[0] || !argv[0][0]) return 0;
    load();
    const char *base = base_of(argv[0]);

    int permitted = 0;
    for (int i = 0; i < npermits && !permitted; i++)
        if (strcmp(permits[i].cmd, base) == 0 && who_matches(&permits[i]))
            permitted = 1;
    if (!permitted) return 0;

    for (int i = 0; i < ndenies; i++) {
        const struct rule *r = &denies[i];
        if (strcmp(r->cmd, base) != 0 || !who_matches(r)) continue;
        if (!r->has_switches) return 0;                 /* whole-command deny */
        for (int a = 1; argv[a]; a++)
            if (switch_hits(r, argv[a])) return 0;      /* a forbidden switch is present */
    }
    return 1;
}
