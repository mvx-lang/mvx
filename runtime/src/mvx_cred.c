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

/* Account credential store (.mvx-private).
 *
 * Backend secrets — networked-LMDB namespace tokens, Postgres
 * user/password — must never travel in git-committed account config
 * (BINDINGS, .mvx, VOC).  They live instead in a per-account directory
 * `.mvx-private/`, which the git tooling ignores, so a clone/BUILD
 * provisions an account without carrying its credentials.
 *
 * The store is one file, `.mvx-private/credentials`, plain text, **one
 * field per line** — the first three whitespace tokens are the
 * non-secret reference (driver, target, key) and the rest of the line
 * is `field=value`, where the value runs to end-of-line and may contain
 * spaces and other characters (a CI step can write an arbitrary GitHub
 * Actions secret verbatim):
 *
 *     mongo    mvxdb-a:27017 SALES  token=abc123
 *     postgres db:5432       mvx    user=app
 *     postgres db:5432       mvx    password=p@ss w0rd :/@
 *
 * BINDINGS names only (driver, target, key); a driver resolves the
 * secret with mvx_cred_lookup().  An environment variable overrides the
 * file so containers can inject secrets with no file on disk:
 * MVXCRED_<DRIVER>_<KEY>_<FIELD>, upper-cased, non-alphanumerics as '_'.
 */
#include "mvx_runtime.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AM ((char)0xFE)

static const char *acct_root(void) {
    const char *a = getenv("MVXACCOUNT");
    return (a && a[0]) ? a : ".";
}

static void priv_dir(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/.mvx-private", acct_root());
}

static void cred_path(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/.mvx-private/credentials", acct_root());
}

/* Warn once if the store is readable beyond its owner — a bearer secret
   is only as private as its file permissions (ssh-style). */
static void warn_perms(const char *path) {
    static int warned = 0;
    struct stat sb;
    if (warned || stat(path, &sb) != 0) return;
    if (sb.st_mode & (S_IRWXG | S_IRWXO)) {
        fprintf(stderr,
                "mvx: warning: %s is group/other-accessible; "
                "credentials should be private (chmod 600)\n",
                path);
        warned = 1;
    }
}

/* Copy the first whitespace-delimited token at *p into out; advance *p
   past it and any trailing spaces.  Returns 0 if none. */
static int next_tok(const char **p, char *out, size_t cap) {
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
    size_t n = (size_t)(e - s);
    if (n == 0) return 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
    while (*e == ' ' || *e == '\t') e++;
    *p = e;
    return 1;
}

/* Parse one stored line into (driver, target, key, field) plus a pointer
   to the value, which runs to end-of-line (may contain spaces).  Returns
   1 on a valid entry, 0 for blank / comment / malformed. */
static int parse_entry(const char *ln, char *d, size_t dc, char *t,
                       size_t tc, char *k, size_t kc, char *field,
                       size_t fc, const char **val, size_t *vlen) {
    const char *p = ln;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') return 0;
    if (!next_tok(&p, d, dc) || !next_tok(&p, t, tc) || !next_tok(&p, k, kc))
        return 0;
    const char *rem = p;                 /* field=value, value to EOL */
    const char *e = rem;
    while (*e && *e != '\n' && *e != '\r') e++;
    const char *eq = memchr(rem, '=', (size_t)(e - rem));
    if (!eq) return 0;
    size_t fn = (size_t)(eq - rem);
    if (fn >= fc) fn = fc - 1;
    memcpy(field, rem, fn);
    field[fn] = '\0';
    *val = eq + 1;
    *vlen = (size_t)(e - (eq + 1));
    return 1;
}

/* MVXCRED_<DRIVER>_<KEY>_<FIELD>, sanitised. */
static int env_override(const char *driver, const char *key,
                        const char *field, char *out, size_t outlen) {
    char name[256];
    int n = snprintf(name, sizeof name, "MVXCRED_%s_%s_%s", driver, key,
                     field);
    if (n <= 0 || (size_t)n >= sizeof name) return 0;
    for (char *c = name; *c; c++)
        *c = isalnum((unsigned char)*c) ? (char)toupper((unsigned char)*c)
                                        : '_';
    const char *v = getenv(name);
    if (!v) return 0;
    snprintf(out, outlen, "%s", v);
    return 1;
}

/* Look up one field of a credential.  env override -> file -> miss.
   Returns 1 and fills out on success. */
int mvx_cred_lookup(const char *driver, const char *target,
                    const char *key, const char *field, char *out,
                    size_t outlen) {
    if (env_override(driver, key, field, out, outlen)) return 1;

    char path[4096];
    cred_path(path, sizeof path);
    warn_perms(path);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char ln[2048];
    int found = 0;
    while (!found && fgets(ln, sizeof ln, fp)) {
        char d[64], t[512], k[256], f[64];
        const char *val;
        size_t vlen;
        if (!parse_entry(ln, d, sizeof d, t, sizeof t, k, sizeof k, f,
                         sizeof f, &val, &vlen))
            continue;
        if (strcmp(d, driver) || strcmp(t, target) || strcmp(k, key) ||
            strcmp(f, field))
            continue;
        if (vlen >= outlen) vlen = outlen - 1;
        memcpy(out, val, vlen);
        out[vlen] = '\0';
        found = 1;
    }
    fclose(fp);
    return found;
}

/* Store secrets.  `fields` is one or more whitespace-separated
   field=value tokens (as typed to SET-CREDENTIAL); each becomes its own
   line, upserted per (driver, target, key, field).  Ensures 0700 dir /
   0600 file. */
int64_t mvx_setcred(mvx_ctx *ctx, const mv_value *driver,
                    const mv_value *target, const mv_value *key,
                    const mv_value *fields) {
    (void)ctx;
    char db[64], tb[512], kb[256], fb[1024];
    const char *dp, *tp, *kp, *fp2;
    mv_val_chars(driver, db, sizeof db, &dp);
    mv_val_chars(target, tb, sizeof tb, &tp);
    mv_val_chars(key, kb, sizeof kb, &kp);
    mv_val_chars(fields, fb, sizeof fb, &fp2);
    char drv[64], tgt[512], ky[256], flds[1024];
    snprintf(drv, sizeof drv, "%s", dp);
    snprintf(tgt, sizeof tgt, "%s", tp);
    snprintf(ky, sizeof ky, "%s", kp);
    snprintf(flds, sizeof flds, "%s", fp2);
    if (!drv[0] || !tgt[0] || !ky[0]) return 0;

    /* split `flds` into field=value tokens */
    char fname[16][64], ftok[16][512];
    int nf = 0;
    const char *fp = flds;
    char tok[512];
    while (nf < 16 && next_tok(&fp, tok, sizeof tok)) {
        const char *eq = strchr(tok, '=');
        if (!eq || eq == tok) continue;
        size_t kn = (size_t)(eq - tok);
        if (kn >= sizeof fname[0]) kn = sizeof fname[0] - 1;
        memcpy(fname[nf], tok, kn);
        fname[nf][kn] = '\0';
        snprintf(ftok[nf], sizeof ftok[nf], "%s", tok);
        nf++;
    }
    if (nf == 0) return 0;

    char dir[4096], path[4096], tmp[4096];
    priv_dir(dir, sizeof dir);
    cred_path(path, sizeof path);
    mkdir(dir, 0700);                    /* harmless if it exists */
    chmod(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *in = fopen(path, "r");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        if (in) fclose(in);
        return 0;
    }
    FILE *out = fdopen(fd, "w");
    if (!out) {
        close(fd);
        if (in) fclose(in);
        return 0;
    }
    if (in) {
        char ln[2048];
        while (fgets(ln, sizeof ln, in)) {
            char d[64], t[512], k[256], f[64];
            const char *val;
            size_t vlen;
            int drop = 0;
            if (parse_entry(ln, d, sizeof d, t, sizeof t, k, sizeof k, f,
                            sizeof f, &val, &vlen) &&
                !strcmp(d, drv) && !strcmp(t, tgt) && !strcmp(k, ky)) {
                for (int i = 0; i < nf; i++)
                    if (!strcmp(f, fname[i])) { drop = 1; break; }
            }
            if (!drop) fputs(ln, out);
        }
        fclose(in);
    }
    for (int i = 0; i < nf; i++)
        fprintf(out, "%s %s %s %s\n", drv, tgt, ky, ftok[i]);
    fclose(out);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    chmod(path, 0600);
    return 1;
}

/* An @AM list of stored entries with every field value masked, for the
   LIST-CREDENTIALS verb — the raw secrets never reach BASIC. */
void mvx_listcred(mvx_ctx *ctx, mv_value *dst) {
    (void)ctx;
    char path[4096];
    cred_path(path, sizeof path);
    warn_perms(path);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    FILE *fp = fopen(path, "r");
    if (fp) {
        char ln[2048];
        while (fgets(ln, sizeof ln, fp)) {
            char d[64], t[512], k[256], f[64];
            const char *val;
            size_t vlen;
            if (!parse_entry(ln, d, sizeof d, t, sizeof t, k, sizeof k, f,
                             sizeof f, &val, &vlen))
                continue;
            char line[2048];
            int m = snprintf(line, sizeof line, "%s %s %s %s=****", d, t, k,
                             f);
            if (m < 0) continue;
            size_t ll = (size_t)m;
            if (len + ll + 1 > cap) {
                cap = cap ? cap * 2 : 256;
                while (cap < len + ll + 1) cap *= 2;
                buf = realloc(buf, cap);
                if (!buf) mvx_fatal("out of memory in LISTCRED");
            }
            if (len) buf[len++] = AM;
            memcpy(buf + len, line, ll);
            len += ll;
        }
        fclose(fp);
    }
    mv_set_str(dst, buf ? buf : "", (int64_t)len);
    free(buf);
}
