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

/* Named connection profiles (.mvx-private/connections).
 *
 * A connection is a named alias for a remote backend: its driver,
 * address, namespace, and secrets.  BINDINGS references it by name
 * (`ORDERS @salesdb`), so the deployment-specific host and credentials
 * live in one local place — move the daemon and only the connection
 * profile changes, not the committed bindings.
 *
 *     .mvx-private/connections, one field per line, value to end-of-line:
 *     salesdb  driver     postgres
 *     salesdb  address    mvxdb:5432
 *     salesdb  namespace  SALES
 *     salesdb  token      abc123
 *
 * An environment variable overrides the file for a field:
 * MVXCONN_<CONN>_<FIELD>, upper-cased, non-alphanumerics as '_'.
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

static const char *conn_acct_root(void) {
    const char *a = getenv("MVXACCOUNT");
    return (a && a[0]) ? a : ".";
}

static void conn_dir(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/.mvx-private", conn_acct_root());
}

static void conn_path(char *buf, size_t cap) {
    snprintf(buf, cap, "%s/.mvx-private/connections", conn_acct_root());
}

static void conn_warn_perms(const char *path) {
    static int warned = 0;
    struct stat sb;
    if (warned || stat(path, &sb) != 0) return;
    if (sb.st_mode & (S_IRWXG | S_IRWXO)) {
        fprintf(stderr,
                "mvx: warning: %s is group/other-accessible; connection "
                "profiles hold secrets and should be private (chmod 600)\n",
                path);
        warned = 1;
    }
}

static int conn_tok(const char **p, char *out, size_t cap) {
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

/* Parse `conn field value-to-EOL`. */
static int conn_entry(const char *ln, char *conn, size_t cc, char *field,
                      size_t fc, const char **val, size_t *vlen) {
    const char *p = ln;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') return 0;
    if (!conn_tok(&p, conn, cc) || !conn_tok(&p, field, fc)) return 0;
    const char *e = p;
    while (*e && *e != '\n' && *e != '\r') e++;
    *val = p;
    *vlen = (size_t)(e - p);
    return 1;
}

static int conn_env(const char *conn, const char *field, char *out,
                    size_t outlen) {
    char name[256];
    int n = snprintf(name, sizeof name, "MVXCONN_%s_%s", conn, field);
    if (n <= 0 || (size_t)n >= sizeof name) return 0;
    for (char *c = name; *c; c++)
        *c = isalnum((unsigned char)*c) ? (char)toupper((unsigned char)*c)
                                        : '_';
    const char *v = getenv(name);
    if (!v) return 0;
    snprintf(out, outlen, "%s", v);
    return 1;
}

static int is_secret(const char *field) {
    return strcmp(field, "token") == 0 || strcmp(field, "password") == 0 ||
           strcmp(field, "passwd") == 0 || strcmp(field, "pass") == 0 ||
           strcmp(field, "secret") == 0 || strcmp(field, "key") == 0;
}

/* One field of a connection: env override -> file -> miss. */
int mvx_conn_lookup(const char *conn, const char *field, char *out,
                    size_t outlen) {
    if (conn_env(conn, field, out, outlen)) return 1;
    char path[4096];
    conn_path(path, sizeof path);
    conn_warn_perms(path);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char ln[2048];
    int found = 0;
    while (!found && fgets(ln, sizeof ln, fp)) {
        char c[128], f[64];
        const char *val;
        size_t vlen;
        if (!conn_entry(ln, c, sizeof c, f, sizeof f, &val, &vlen))
            continue;
        if (strcmp(c, conn) || strcmp(f, field)) continue;
        if (vlen >= outlen) vlen = outlen - 1;
        memcpy(out, val, vlen);
        out[vlen] = '\0';
        found = 1;
    }
    fclose(fp);
    return found;
}

/* Upsert connection fields.  `fields` is whitespace-separated
   field=value tokens; each becomes its own line, per (conn, field). */
int64_t mvx_setconn(mvx_ctx *ctx, const mv_value *conn,
                    const mv_value *fields) {
    (void)ctx;
    char cb[128], fb[1024];
    const char *cp, *fp0;
    mv_val_chars(conn, cb, sizeof cb, &cp);
    mv_val_chars(fields, fb, sizeof fb, &fp0);
    char cn[128], flds[1024];
    snprintf(cn, sizeof cn, "%s", cp);
    snprintf(flds, sizeof flds, "%s", fp0);
    if (!cn[0]) return 0;

    char fname[16][64], fval[16][512];
    int nf = 0;
    const char *fp = flds;
    char tok[512];
    while (nf < 16 && conn_tok(&fp, tok, sizeof tok)) {
        const char *eq = strchr(tok, '=');
        if (!eq || eq == tok) continue;
        size_t kn = (size_t)(eq - tok);
        if (kn >= sizeof fname[0]) kn = sizeof fname[0] - 1;
        memcpy(fname[nf], tok, kn);
        fname[nf][kn] = '\0';
        snprintf(fval[nf], sizeof fval[nf], "%s", eq + 1);
        nf++;
    }
    if (nf == 0) return 0;

    char dir[4096], path[4096], tmp[4096];
    conn_dir(dir, sizeof dir);
    conn_path(path, sizeof path);
    mkdir(dir, 0700);
    chmod(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    FILE *in = fopen(path, "r");
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { if (in) fclose(in); return 0; }
    FILE *out = fdopen(fd, "w");
    if (!out) { close(fd); if (in) fclose(in); return 0; }
    if (in) {
        char ln[2048];
        while (fgets(ln, sizeof ln, in)) {
            char c[128], f[64];
            const char *val;
            size_t vlen;
            int drop = 0;
            if (conn_entry(ln, c, sizeof c, f, sizeof f, &val, &vlen) &&
                strcmp(c, cn) == 0)
                for (int i = 0; i < nf; i++)
                    if (strcmp(f, fname[i]) == 0) { drop = 1; break; }
            if (!drop) fputs(ln, out);
        }
        fclose(in);
    }
    for (int i = 0; i < nf; i++)
        fprintf(out, "%s %s %s\n", cn, fname[i], fval[i]);
    fclose(out);
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    chmod(path, 0600);
    return 1;
}

/* An @AM list of connection fields, secret values masked. */
void mvx_listconn(mvx_ctx *ctx, mv_value *dst) {
    (void)ctx;
    char path[4096];
    conn_path(path, sizeof path);
    conn_warn_perms(path);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    FILE *fp = fopen(path, "r");
    if (fp) {
        char ln[2048];
        while (fgets(ln, sizeof ln, fp)) {
            char c[128], f[64];
            const char *val;
            size_t vlen;
            if (!conn_entry(ln, c, sizeof c, f, sizeof f, &val, &vlen))
                continue;
            char line[2048];
            int m;
            if (is_secret(f))
                m = snprintf(line, sizeof line, "%s %s=****", c, f);
            else
                m = snprintf(line, sizeof line, "%s %s=%.*s", c, f,
                             (int)vlen, val);
            if (m < 0) continue;
            size_t ll = (size_t)m;
            if (ll >= sizeof line) ll = sizeof line - 1;
            if (len + ll + 1 > cap) {
                cap = cap ? cap * 2 : 256;
                while (cap < len + ll + 1) cap *= 2;
                buf = realloc(buf, cap);
                if (!buf) mvx_fatal("out of memory in LISTCONN");
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
