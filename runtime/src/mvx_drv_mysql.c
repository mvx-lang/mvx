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

/* MySQL / MariaDB storage driver.
 *
 * Built against the MariaDB Connector/C, which speaks the MySQL wire
 * protocol, so one driver serves both servers.
 *
 * Shape: postgres's (a pooled server connection per location, a table per
 * MV file) with sqlite's push-downs.  What differs is the dialect, and
 * two differences matter enough to state up front:
 *
 * NO STORED FUNCTION FOR ATTRIBUTE EXTRACTION.  postgres creates
 * mvx_attr() in the schema; that needs CREATE FUNCTION, which a hosted
 * MySQL account frequently does not have.  Here the extraction is an
 * ordinary expression built from SUBSTRING_INDEX, so the driver needs no
 * privilege beyond the tables it owns.
 *
 * EVERYTHING IS BINARY.  ids are VARBINARY, records LONGBLOB.  MV sorts
 * and compares bytes, and a blob column does exactly that with no
 * collation to get wrong -- which is also why the ORDER BY push-down
 * needs no COLLATE clause the way postgres needs COLLATE "C".
 */

#include "mvx_driver.h"
#include "mvx_runtime.h"

#include <mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNS 8
static struct {
    char loc[1024];
    MYSQL *db;
} g_conns[MAX_CONNS];
static int g_nconns;

struct mvx_cursor {
    mv_value *ids;
    int64_t n, pos;
};

typedef struct {
    mvx_file_base base;
    MYSQL *db;
    char table[256];
} my_file;

static const mvx_driver mvx_driver_mysql;

/* ------------------------------------------------------------ helpers */

static const char *split_spec(const char *spec, char *loc, size_t cap) {
    const char *nl = strchr(spec, '\n');
    if (!nl) { loc[0] = '\0'; return spec; }
    size_t n = (size_t)(nl - spec);
    if (n >= cap) n = cap - 1;
    memcpy(loc, spec, n);
    loc[n] = '\0';
    return nl + 1;
}

/* Backtick-quote an identifier, doubling any backtick within.  MV file
   names carry dots and dashes quite legitimately, so nothing goes into a
   statement unquoted. */
static void quote_ident(const char *s, char *out, size_t cap) {
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = '\0'; return; }
    out[o++] = '`';
    for (const char *p = s; *p && o + 3 < cap; p++) {
        if (*p == '`') { out[o++] = '`'; if (o + 2 >= cap) break; }
        out[o++] = *p;
    }
    out[o++] = '`';
    out[o] = '\0';
}

/* The nth @AM attribute of a column, as a SQL expression.
 *
 * SUBSTRING_INDEX(x, d, n) returns everything before the nth delimiter,
 * and with a negative count everything after it -- so nesting the two
 * picks out field n.  The IF guards the end: asked for a field past the
 * last one, SUBSTRING_INDEX would hand back the whole string, where MV
 * says "".  Field count is (delimiters + 1), and the delimiter count is
 * the length lost when they are all removed. */
static void attr_expr(const char *col, int64_t n, char *out, size_t cap) {
    snprintf(out, cap,
             "IF(%lld <= 1 + LENGTH(%s) - LENGTH(REPLACE(%s, CHAR(254), '')), "
             "SUBSTRING_INDEX(SUBSTRING_INDEX(%s, CHAR(254), %lld), CHAR(254), -1), "
             "'')",
             (long long)n, col, col, col, (long long)n);
}

/* The comparison expression for a field: a mapped column, or the blob
   attribute.  Shared by every push-down so they cannot disagree. */
static void field_expr(const char *col, int64_t attr, const char *tbl_alias,
                       char *out, size_t cap) {
    if (col && col[0]) {
        char qc[300];
        quote_ident(col, qc, sizeof qc);
        if (tbl_alias && tbl_alias[0]) snprintf(out, cap, "%s.%s", tbl_alias, qc);
        else snprintf(out, cap, "%s", qc);
    } else {
        char recref[64];
        snprintf(recref, sizeof recref, "%s%s",
                 (tbl_alias && tbl_alias[0]) ? tbl_alias : "",
                 (tbl_alias && tbl_alias[0]) ? ".rec" : "rec");
        attr_expr(recref, attr, out, cap);
    }
}

static const char *sql_op(const char *op) {
    if (!op || !op[0]) return NULL;
    if (op[0] == '=' && !op[1]) return "=";
    if (op[0] == '#' && !op[1]) return "<>";
    if (strcmp(op, ">") == 0)  return ">";
    if (strcmp(op, "<") == 0)  return "<";
    if (strcmp(op, ">=") == 0) return ">=";
    if (strcmp(op, "<=") == 0) return "<=";
    return NULL;
}

/* Parse a libpq-style conninfo ("host=... port=... user=... password=...
   dbname=...") -- the same shape the postgres binding uses, so an account
   moving between the two backends changes the driver name and little else. */
static void conf_get(const char *loc, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    size_t klen = strlen(key);
    for (const char *p = loc; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        const char *eq = strchr(p, '=');
        if (!eq) return;
        const char *end = eq + 1;
        while (*end && *end != ' ' && *end != '\t') end++;
        if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
            size_t n = (size_t)(end - eq - 1);
            if (n >= cap) n = cap - 1;
            memcpy(out, eq + 1, n);
            out[n] = '\0';
            return;
        }
        p = end;
    }
}

static MYSQL *my_connect(const char *loc, char *err, size_t errlen) {
    if (!loc || !loc[0]) {
        snprintf(err, errlen, "mysql: no connection info in the binding");
        return NULL;
    }
    for (int i = 0; i < g_nconns; i++)
        if (strcmp(g_conns[i].loc, loc) == 0) return g_conns[i].db;
    if (g_nconns == MAX_CONNS) {
        snprintf(err, errlen, "mysql: too many connections (max %d)", MAX_CONNS);
        return NULL;
    }
    char host[256], port[16], user[128], pass[256], dbn[128];
    conf_get(loc, "host", host, sizeof host);
    conf_get(loc, "port", port, sizeof port);
    conf_get(loc, "user", user, sizeof user);
    conf_get(loc, "password", pass, sizeof pass);
    conf_get(loc, "dbname", dbn, sizeof dbn);
    MYSQL *db = mysql_init(NULL);
    if (!db) { snprintf(err, errlen, "mysql: mysql_init failed"); return NULL; }
    /* Reconnect, because an MV session sits idle across user think-time for
       far longer than wait_timeout and would otherwise come back to a dead
       handle mid-sentence. */
    my_bool yes = 1;
    mysql_optionsv(db, MYSQL_OPT_RECONNECT, &yes);
    if (!mysql_real_connect(db, host[0] ? host : "127.0.0.1",
                            user[0] ? user : "root", pass,
                            dbn[0] ? dbn : NULL,
                            port[0] ? (unsigned)atoi(port) : 3306, NULL, 0)) {
        snprintf(err, errlen, "mysql: %s", mysql_error(db));
        mysql_close(db);
        return NULL;
    }
    snprintf(g_conns[g_nconns].loc, sizeof g_conns[0].loc, "%s", loc);
    g_conns[g_nconns].db = db;
    g_nconns++;
    return db;
}

static int exec_sql(MYSQL *db, const char *sql) {
    return mysql_real_query(db, sql, (unsigned long)strlen(sql)) == 0;
}

static int table_exists(MYSQL *db, const char *name) {
    char esc[520];
    mysql_real_escape_string(db, esc, name, (unsigned long)strlen(name));
    char sql[900];
    snprintf(sql, sizeof sql,
             "SELECT 1 FROM information_schema.tables "
             "WHERE table_schema = DATABASE() AND table_name = '%s'", esc);
    if (!exec_sql(db, sql)) return 0;
    MYSQL_RES *r = mysql_store_result(db);
    int found = r && mysql_num_rows(r) > 0;
    if (r) mysql_free_result(r);
    return found;
}

/* Run a statement whose one bound parameter is a byte string, collecting
   the id column into a cursor.  Every push-down ends here, so they share
   one snapshot-then-stream shape and one place that binds safely. */
static mvx_cursor *run_ids(MYSQL *db, const char *sql,
                           const char **vals, const int64_t *lens, int nb) {
    MYSQL_STMT *st = mysql_stmt_init(db);
    if (!st) return NULL;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st);
        return NULL;
    }
    MYSQL_BIND bind[32];
    unsigned long blen[32];
    memset(bind, 0, sizeof bind);
    for (int i = 0; i < nb && i < 32; i++) {
        blen[i] = (unsigned long)lens[i];
        bind[i].buffer_type = MYSQL_TYPE_BLOB;
        bind[i].buffer = (void *)vals[i];
        bind[i].buffer_length = blen[i];
        bind[i].length = &blen[i];
    }
    if (nb && mysql_stmt_bind_param(st, bind) != 0) {
        mysql_stmt_close(st);
        return NULL;
    }
    if (mysql_stmt_execute(st) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st);
        return NULL;
    }
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in mysql cursor");
    char buf[4096];
    unsigned long outlen = 0;
    my_bool isnull = 0, trunc = 0;
    MYSQL_BIND ob;
    memset(&ob, 0, sizeof ob);
    ob.buffer_type = MYSQL_TYPE_BLOB;
    ob.buffer = buf;
    ob.buffer_length = sizeof buf;
    ob.length = &outlen;
    ob.is_null = &isnull;
    ob.error = &trunc;
    if (mysql_stmt_bind_result(st, &ob) != 0) {
        mysql_stmt_close(st);
        free(c);
        return NULL;
    }
    int64_t cap = 0;
    while (mysql_stmt_fetch(st) == 0 || trunc) {
        if (c->n == cap) {
            cap = cap ? cap * 2 : 64;
            mv_value *g = realloc(c->ids, (size_t)cap * sizeof(mv_value));
            if (!g) mvx_fatal("out of memory in mysql cursor");
            c->ids = g;
        }
        mv_init(&c->ids[c->n]);
        mv_set_str(&c->ids[c->n], buf, (int64_t)(outlen < sizeof buf ? outlen : sizeof buf));
        c->n++;
        trunc = 0;
    }
    mysql_stmt_close(st);
    return c;
}

/* ---------------------------------------------------- record operations */

static mvx_file *my_open(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *tbl = split_spec(spec, loc, sizeof loc);
    MYSQL *db = my_connect(loc, err, errlen);
    if (!db) return NULL;
    if (!table_exists(db, tbl)) return NULL;   /* not found: normal ELSE path */
    my_file *f = calloc(1, sizeof(my_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_mysql;
    f->base.spec = strdup(spec);
    f->db = db;
    snprintf(f->table, sizeof f->table, "%s", tbl);
    return (mvx_file *)f;
}

static void my_close(mvx_file *fh) {
    my_file *f = (my_file *)fh;
    free(f->base.spec);
    free(f);                                  /* the MYSQL * is pooled */
}

static int my_read(mvx_file *fh, const char *id, int64_t idlen, mv_value *rec) {
    my_file *f = (my_file *)fh;
    char qt[300], sql[400];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "SELECT rec FROM %s WHERE id = ?", qt);
    MYSQL_STMT *st = mysql_stmt_init(f->db);
    if (!st) return 0;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st); return 0;
    }
    MYSQL_BIND ib;
    unsigned long il = (unsigned long)idlen;
    memset(&ib, 0, sizeof ib);
    ib.buffer_type = MYSQL_TYPE_BLOB;
    ib.buffer = (void *)id; ib.buffer_length = il; ib.length = &il;
    mysql_stmt_bind_param(st, &ib);
    if (mysql_stmt_execute(st) != 0 || mysql_stmt_store_result(st) != 0) {
        mysql_stmt_close(st); return 0;
    }
    /* Two-step fetch: ask for the length with a zero-length buffer, then
       fetch the column into one that fits.  A record has no size limit
       worth assuming, and a fixed buffer would silently truncate. */
    unsigned long outlen = 0;
    my_bool isnull = 0, trunc = 0;
    MYSQL_BIND ob;
    memset(&ob, 0, sizeof ob);
    ob.buffer_type = MYSQL_TYPE_BLOB;
    ob.buffer = NULL; ob.buffer_length = 0;
    ob.length = &outlen; ob.is_null = &isnull; ob.error = &trunc;
    mysql_stmt_bind_result(st, &ob);
    int got = 0;
    if (mysql_stmt_fetch(st) != MYSQL_NO_DATA) {
        char *buf = malloc(outlen ? outlen : 1);
        if (!buf) mvx_fatal("out of memory in mysql read");
        ob.buffer = buf; ob.buffer_length = outlen;
        if (mysql_stmt_fetch_column(st, &ob, 0, 0) == 0) {
            mv_set_str(rec, buf, (int64_t)outlen);
            got = 1;
        }
        free(buf);
    }
    mysql_stmt_close(st);
    return got;
}

static int my_write(mvx_file *fh, const char *id, int64_t idlen,
                    const mv_value *rec) {
    my_file *f = (my_file *)fh;
    char qt[300], sql[500];
    quote_ident(f->table, qt, sizeof qt);
    /* Upsert: MV's WRITE replaces whatever was there. */
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (id, rec) VALUES (?, ?) "
             "ON DUPLICATE KEY UPDATE rec = VALUES(rec)", qt);
    MYSQL_STMT *st = mysql_stmt_init(f->db);
    if (!st) return 0;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st); return 0;
    }
    char buf[256];
    const char *rp;
    int64_t rl = mv_val_chars((mv_value *)rec, buf, sizeof buf, &rp);
    MYSQL_BIND b[2];
    unsigned long il = (unsigned long)idlen, rlen = (unsigned long)rl;
    memset(b, 0, sizeof b);
    b[0].buffer_type = MYSQL_TYPE_BLOB; b[0].buffer = (void *)id;
    b[0].buffer_length = il; b[0].length = &il;
    b[1].buffer_type = MYSQL_TYPE_BLOB; b[1].buffer = (void *)rp;
    b[1].buffer_length = rlen; b[1].length = &rlen;
    mysql_stmt_bind_param(st, b);
    int ok = mysql_stmt_execute(st) == 0;
    mysql_stmt_close(st);
    return ok;
}

static int my_del(mvx_file *fh, const char *id, int64_t idlen) {
    my_file *f = (my_file *)fh;
    char qt[300], sql[400];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE id = ?", qt);
    MYSQL_STMT *st = mysql_stmt_init(f->db);
    if (!st) return 0;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st); return 0;
    }
    MYSQL_BIND ib;
    unsigned long il = (unsigned long)idlen;
    memset(&ib, 0, sizeof ib);
    ib.buffer_type = MYSQL_TYPE_BLOB; ib.buffer = (void *)id;
    ib.buffer_length = il; ib.length = &il;
    mysql_stmt_bind_param(st, &ib);
    int ok = mysql_stmt_execute(st) == 0;
    my_ulonglong n = mysql_stmt_affected_rows(st);
    mysql_stmt_close(st);
    return ok && n > 0;
}

/* -------------------------------------------------------------- select */

static mvx_cursor *my_select_begin(mvx_file *fh) {
    my_file *f = (my_file *)fh;
    char qt[300], sql[400];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    return run_ids(f->db, sql, NULL, NULL, 0);
}

static int my_select_next(mvx_cursor *c, mv_value *id) {
    if (!c || c->pos >= c->n) return 0;
    mv_copy(id, &c->ids[c->pos++]);
    return 1;
}

static void my_select_end(mvx_cursor *c) {
    if (!c) return;
    for (int64_t i = 0; i < c->n; i++) mv_clear(&c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t my_select_count(mvx_cursor *c) { return c ? c->n : 0; }

/* --------------------------------------------------- file lifecycle */

static int my_create(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *tbl = split_spec(spec, loc, sizeof loc);
    MYSQL *db = my_connect(loc, err, errlen);
    if (!db) return 0;
    if (table_exists(db, tbl)) return 0;      /* already exists */
    char qt[300], sql[600];
    quote_ident(tbl, qt, sizeof qt);
    /* VARBINARY, not BLOB: a primary key needs a bounded length.  255 is
       generous for an MV item-id and leaves the row well inside the index
       limit.  The record itself is a LONGBLOB and unbounded. */
    snprintf(sql, sizeof sql,
             "CREATE TABLE %s (id VARBINARY(255) NOT NULL PRIMARY KEY, "
             "rec LONGBLOB) ENGINE=InnoDB", qt);
    if (!exec_sql(db, sql)) {
        snprintf(err, errlen, "mysql: %s", mysql_error(db));
        return 0;
    }
    return 1;
}

static int my_remove(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *tbl = split_spec(spec, loc, sizeof loc);
    MYSQL *db = my_connect(loc, err, errlen);
    if (!db) return 0;
    if (!table_exists(db, tbl)) return 0;
    char qt[300], sql[400];
    quote_ident(tbl, qt, sizeof qt);
    snprintf(sql, sizeof sql, "DROP TABLE %s", qt);
    if (!exec_sql(db, sql)) {
        snprintf(err, errlen, "mysql: %s", mysql_error(db));
        return 0;
    }
    return 1;
}

/* Whole-account LISTF: a table is an MV file if it has a `rec` column --
   the same test postgres uses, so association child tables and anything
   else sharing the database stay out without matching on names. */
static int my_names(const char *loc, mv_value *out, char *err, size_t errlen) {
    MYSQL *db = my_connect(loc, err, errlen);
    if (!db) return 0;
    if (!exec_sql(db,
            "SELECT table_name FROM information_schema.columns "
            "WHERE table_schema = DATABASE() AND column_name = 'rec' "
            "ORDER BY table_name")) {
        snprintf(err, errlen, "mysql: %s", mysql_error(db));
        return 0;
    }
    MYSQL_RES *r = mysql_store_result(db);
    if (!r) { snprintf(err, errlen, "mysql: %s", mysql_error(db)); return 0; }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r))) {
        unsigned long *fl = mysql_fetch_lengths(r);
        if (!row[0]) continue;
        size_t nl = fl ? (size_t)fl[0] : strlen(row[0]);
        if (len + nl + 1 > cap) {
            cap = cap ? cap * 2 : 256;
            while (cap < len + nl + 1) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) mvx_fatal("out of memory in mysql names");
            buf = nb;
        }
        if (len) buf[len++] = (char)0xFE;     /* @AM */
        memcpy(buf + len, row[0], nl);
        len += nl;
    }
    mysql_free_result(r);
    mv_set_str(out, buf ? buf : "", (int64_t)len);
    free(buf);
    return 1;
}

static int my_bulk_begin(mvx_file *fh) {
    return exec_sql(((my_file *)fh)->db, "START TRANSACTION");
}
static int my_bulk_commit(mvx_file *fh) {
    MYSQL *db = ((my_file *)fh)->db;
    if (exec_sql(db, "COMMIT")) return 1;
    exec_sql(db, "ROLLBACK");
    return 0;
}

/* ------------------------------------------- relational mapping (#18)
 *
 * The mapped columns are what ORDER BY, WITH and SUM push down onto: the
 * runtime only offers select_order a column it can find in the mapping,
 * so without this capability sorting stays in the verb. */

static const char *my_sqltype(const char *t) {
    /* DECIMAL, not DOUBLE: MV money and quantities are decimal, and a
       binary float would make SUM disagree with the verb's own arithmetic
       in the last place.  VARBINARY for everything else, so comparison and
       ORDER BY are byte-wise like MV -- and bounded, because MySQL cannot
       index a full-length blob.  A mapped field longer than this is
       truncated IN THE COLUMN ONLY; the record blob keeps the whole value,
       and native reads come back from the column, so keep mapped fields
       inside it. */
    if (t && strcmp(t, "NUMERIC") == 0) return "DECIMAL(38,10)";
    return "VARBINARY(255)";
}

/* 1060 = ER_DUP_FIELDNAME: adding a column that is already there is what
   makes map_ensure idempotent, not an error. */
static int add_column(MYSQL *db, const char *qt, const char *name,
                      const char *type, char *err, size_t errlen) {
    char qc[300], sql[800];
    quote_ident(name, qc, sizeof qc);
    snprintf(sql, sizeof sql, "ALTER TABLE %s ADD COLUMN %s %s", qt, qc, type);
    if (exec_sql(db, sql)) return 1;
    if (mysql_errno(db) == 1060) return 1;
    if (err) snprintf(err, errlen, "mysql: %s", mysql_error(db));
    return 0;
}

static int my_map_ensure(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                         char *err, size_t errlen) {
    my_file *f = (my_file *)fh;
    char qt[300];
    quote_ident(f->table, qt, sizeof qt);
    for (int i = 0; i < ncols; i++)
        if (!add_column(f->db, qt, cols[i].name, my_sqltype(cols[i].type),
                        err, errlen)) return 0;
    return 1;
}

static int my_map_apply(mvx_file *fh, const char *id, int64_t idlen,
                        const mvx_mapfield *cols, const char **vals,
                        const int64_t *vlens, int ncols) {
    my_file *f = (my_file *)fh;
    if (ncols < 1) return 1;
    if (ncols > 60) return 0;                 /* bind array bound */
    char qt[300], sql[8192];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "UPDATE %s SET ", qt);
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s = ?", i ? ", " : "", qc);
        if (p >= sizeof sql) return 0;
    }
    snprintf(sql + p, sizeof sql - p, " WHERE id = ?");
    MYSQL_STMT *st = mysql_stmt_init(f->db);
    if (!st) return 0;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st); return 0;
    }
    MYSQL_BIND b[61];
    unsigned long bl[61];
    my_bool nulls[61];
    memset(b, 0, sizeof b);
    for (int i = 0; i < ncols; i++) {
        nulls[i] = vals[i] ? 0 : 1;
        bl[i] = vals[i] ? (unsigned long)vlens[i] : 0;
        b[i].buffer_type = MYSQL_TYPE_BLOB;
        b[i].buffer = (void *)vals[i];
        b[i].buffer_length = bl[i];
        b[i].length = &bl[i];
        b[i].is_null = &nulls[i];
    }
    bl[ncols] = (unsigned long)idlen;
    b[ncols].buffer_type = MYSQL_TYPE_BLOB;
    b[ncols].buffer = (void *)id;
    b[ncols].buffer_length = bl[ncols];
    b[ncols].length = &bl[ncols];
    if (mysql_stmt_bind_param(st, b) != 0) { mysql_stmt_close(st); return 0; }
    int ok = mysql_stmt_execute(st) == 0;
    mysql_stmt_close(st);
    return ok;
}

static void child_name(my_file *f, const char *assoc, char *out, size_t cap) {
    snprintf(out, cap, "%s__%s", f->table, assoc);
}

static int my_map_child_ensure(mvx_file *fh, const char *assoc,
                               const mvx_mapfield *cols, int ncols,
                               char *err, size_t errlen) {
    my_file *f = (my_file *)fh;
    char nm[512], qn[600], sql[4096];
    child_name(f, assoc, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    size_t p = (size_t)snprintf(sql, sizeof sql,
        "CREATE TABLE IF NOT EXISTS %s (id VARBINARY(255) NOT NULL, "
        "seq INT NOT NULL", qn);
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", %s %s",
                              qc, my_sqltype(cols[i].type));
        if (p >= sizeof sql) return 0;
    }
    snprintf(sql + p, sizeof sql - p,
             ", PRIMARY KEY (id, seq)) ENGINE=InnoDB");
    if (!exec_sql(f->db, sql)) {
        snprintf(err, errlen, "mysql: %s", mysql_error(f->db));
        return 0;
    }
    /* Columns can arrive after the table (a mapping gains a field). */
    for (int i = 0; i < ncols; i++)
        add_column(f->db, qn, cols[i].name, my_sqltype(cols[i].type), NULL, 0);
    return 1;
}

static int my_map_child_apply(mvx_file *fh, const char *id, int64_t idlen,
                              const char *assoc, const mvx_mapfield *cols,
                              int ncols, const char **vals,
                              const int64_t *vlens, int nrows) {
    my_file *f = (my_file *)fh;
    if (ncols > 60) return 0;
    char nm[512], qn[600], dsql[700];
    child_name(f, assoc, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    snprintf(dsql, sizeof dsql, "DELETE FROM %s WHERE id = ?", qn);
    MYSQL_STMT *ds = mysql_stmt_init(f->db);
    if (ds && mysql_stmt_prepare(ds, dsql, (unsigned long)strlen(dsql)) == 0) {
        MYSQL_BIND ib;
        unsigned long il = (unsigned long)idlen;
        memset(&ib, 0, sizeof ib);
        ib.buffer_type = MYSQL_TYPE_BLOB; ib.buffer = (void *)id;
        ib.buffer_length = il; ib.length = &il;
        mysql_stmt_bind_param(ds, &ib);
        mysql_stmt_execute(ds);
    }
    if (ds) mysql_stmt_close(ds);
    if (nrows < 1) return 1;

    char sql[8192];
    size_t p = (size_t)snprintf(sql, sizeof sql, "INSERT INTO %s (id, seq", qn);
    for (int c = 0; c < ncols; c++) {
        char qc[300];
        quote_ident(cols[c].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", %s", qc);
        if (p >= sizeof sql) return 0;
    }
    p += (size_t)snprintf(sql + p, sizeof sql - p, ") VALUES (?, ?");
    for (int c = 0; c < ncols; c++)
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", ?");
    snprintf(sql + p, sizeof sql - p, ")");

    MYSQL_STMT *st = mysql_stmt_init(f->db);
    if (!st) return 0;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st); return 0;
    }
    int ok = 1;
    for (int r = 0; r < nrows && ok; r++) {
        MYSQL_BIND b[62];
        unsigned long bl[62];
        my_bool nulls[62];
        int seq = r + 1;
        memset(b, 0, sizeof b);
        bl[0] = (unsigned long)idlen;
        b[0].buffer_type = MYSQL_TYPE_BLOB; b[0].buffer = (void *)id;
        b[0].buffer_length = bl[0]; b[0].length = &bl[0];
        b[1].buffer_type = MYSQL_TYPE_LONG; b[1].buffer = &seq;
        for (int c = 0; c < ncols; c++) {
            const char *v = vals[(size_t)r * ncols + c];
            nulls[c] = v ? 0 : 1;
            bl[c + 2] = v ? (unsigned long)vlens[(size_t)r * ncols + c] : 0;
            b[c + 2].buffer_type = MYSQL_TYPE_BLOB;
            b[c + 2].buffer = (void *)v;
            b[c + 2].buffer_length = bl[c + 2];
            b[c + 2].length = &bl[c + 2];
            b[c + 2].is_null = &nulls[c];
        }
        if (mysql_stmt_bind_param(st, b) != 0) { ok = 0; break; }
        ok = mysql_stmt_execute(st) == 0;
    }
    mysql_stmt_close(st);
    return ok;
}

static int my_map_drop(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                       const char **assocs, int nassocs, char *err,
                       size_t errlen) {
    my_file *f = (my_file *)fh;
    (void)err; (void)errlen;
    char qt[300];
    quote_ident(f->table, qt, sizeof qt);
    for (int i = 0; i < ncols; i++) {
        char qc[300], sql[800];
        quote_ident(cols[i].name, qc, sizeof qc);
        snprintf(sql, sizeof sql, "ALTER TABLE %s DROP COLUMN %s", qt, qc);
        exec_sql(f->db, sql);                 /* absent column: nothing to do */
    }
    for (int i = 0; i < nassocs; i++) {
        char nm[512], qn[600], sql[800];
        child_name(f, assocs[i], nm, sizeof nm);
        quote_ident(nm, qn, sizeof qn);
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s", qn);
        exec_sql(f->db, sql);
    }
    return 1;
}

/* Native read-back, so a column written by something other than MVX is what
   a READ sees.  Every non-NULL cell is malloc'd; the runtime frees. */
static int my_map_read(mvx_file *fh, const char *id, int64_t idlen,
                       const mvx_mapfield *cols, int ncols,
                       char **vals, int64_t *lens) {
    my_file *f = (my_file *)fh;
    if (ncols < 1) return 0;
    char qt[300], sql[8192], esc[600];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT ");
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s", i ? ", " : "", qc);
        if (p >= sizeof sql) return -1;
    }
    mysql_real_escape_string(f->db, esc, id, (unsigned long)idlen);
    snprintf(sql + p, sizeof sql - p, " FROM %s WHERE id = '%s'", qt, esc);
    if (!exec_sql(f->db, sql)) return -1;
    MYSQL_RES *r = mysql_store_result(f->db);
    if (!r) return -1;
    MYSQL_ROW row = mysql_fetch_row(r);
    int got = 0;
    if (row) {
        unsigned long *fl = mysql_fetch_lengths(r);
        got = 1;
        for (int i = 0; i < ncols; i++) {
            if (!row[i]) { vals[i] = NULL; lens[i] = 0; continue; }
            size_t n = fl ? (size_t)fl[i] : strlen(row[i]);
            vals[i] = malloc(n + 1);
            if (!vals[i]) mvx_fatal("out of memory in mysql map_read");
            memcpy(vals[i], row[i], n);
            vals[i][n] = '\0';
            lens[i] = (int64_t)n;
        }
    }
    mysql_free_result(r);
    return got;
}

static int my_map_child_read(mvx_file *fh, const char *id, int64_t idlen,
                             const char *assoc, const mvx_mapfield *cols,
                             int ncols, char ***cells, int64_t **lens,
                             int *nrows) {
    my_file *f = (my_file *)fh;
    *cells = NULL; *lens = NULL; *nrows = 0;
    if (ncols < 1) return -1;
    char nm[512], qn[600], sql[8192], esc[600];
    child_name(f, assoc, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT ");
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s", i ? ", " : "", qc);
        if (p >= sizeof sql) return -1;
    }
    mysql_real_escape_string(f->db, esc, id, (unsigned long)idlen);
    snprintf(sql + p, sizeof sql - p,
             " FROM %s WHERE id = '%s' ORDER BY seq", qn, esc);
    if (!exec_sql(f->db, sql)) return -1;
    MYSQL_RES *r = mysql_store_result(f->db);
    if (!r) return -1;
    int n = (int)mysql_num_rows(r);
    char **cv = NULL; int64_t *cl = NULL;
    if (n > 0) {
        cv = calloc((size_t)n * ncols, sizeof(char *));
        cl = calloc((size_t)n * ncols, sizeof(int64_t));
        if (!cv || !cl) mvx_fatal("out of memory in mysql map_child_read");
    }
    MYSQL_ROW row;
    int ri = 0;
    while ((row = mysql_fetch_row(r))) {
        unsigned long *fl = mysql_fetch_lengths(r);
        for (int i = 0; i < ncols; i++) {
            size_t k = (size_t)ri * ncols + i;
            if (!row[i]) { cv[k] = NULL; cl[k] = 0; continue; }
            size_t ln = fl ? (size_t)fl[i] : strlen(row[i]);
            cv[k] = malloc(ln + 1);
            if (!cv[k]) mvx_fatal("out of memory in mysql map_child_read");
            memcpy(cv[k], row[i], ln);
            cv[k][ln] = '\0';
            cl[k] = (int64_t)ln;
        }
        ri++;
    }
    mysql_free_result(r);
    *cells = cv; *lens = cl; *nrows = ri;
    return 1;
}

/* ------------------------------------------------------------ indexes */

static void index_name(my_file *f, const char *item, char *out, size_t cap) {
    snprintf(out, cap, "mvxix_%s", item);    /* per-table namespace in MySQL */
}

/* Only a MAPPED COLUMN can be indexed here.  MySQL 8.0.13+ has functional
   indexes and MariaDB needs a generated column for the same thing; rather
   than depend on a version or silently build something different on each,
   the raw-attribute case declines and the caller scans.  A dictionary field
   worth indexing is worth mapping. */
static int my_index_create(mvx_file *fh, const char *item, const char *col,
                           int64_t attr) {
    my_file *f = (my_file *)fh;
    (void)attr;
    if (!col || !col[0]) return -1;
    char qt[300], qc[300], nm[512], qn[600], sql[1400];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(col, qc, sizeof qc);
    index_name(f, item, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    snprintf(sql, sizeof sql, "CREATE INDEX %s ON %s (%s)", qn, qt, qc);
    /* 1061 = ER_DUP_KEYNAME: already indexed is success, not failure. */
    if (!exec_sql(f->db, sql) && mysql_errno(f->db) != 1061) return -1;
    char csql[400];
    snprintf(csql, sizeof csql, "SELECT COUNT(*) FROM %s", qt);
    int n = 0;
    if (exec_sql(f->db, csql)) {
        MYSQL_RES *r = mysql_store_result(f->db);
        MYSQL_ROW row = r ? mysql_fetch_row(r) : NULL;
        if (row && row[0]) n = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    return n;
}

/* NULL = no such index, which the runtime distinguishes from an empty
   cursor -- so look the index up and recover the column it covers. */
static mvx_cursor *my_index_select(mvx_file *fh, const char *item,
                                   const char *key, int64_t klen) {
    my_file *f = (my_file *)fh;
    char nm[512], et[520], en[520], sql[900];
    index_name(f, item, nm, sizeof nm);
    mysql_real_escape_string(f->db, et, f->table, (unsigned long)strlen(f->table));
    mysql_real_escape_string(f->db, en, nm, (unsigned long)strlen(nm));
    snprintf(sql, sizeof sql,
             "SELECT column_name FROM information_schema.statistics "
             "WHERE table_schema = DATABASE() AND table_name = '%s' "
             "AND index_name = '%s' ORDER BY seq_in_index LIMIT 1", et, en);
    if (!exec_sql(f->db, sql)) return NULL;
    MYSQL_RES *r = mysql_store_result(f->db);
    MYSQL_ROW row = r ? mysql_fetch_row(r) : NULL;
    char col[256] = "";
    if (row && row[0]) snprintf(col, sizeof col, "%s", row[0]);
    if (r) mysql_free_result(r);
    if (!col[0]) return NULL;                 /* no such index */
    char qt[300], qc[300], q[900];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(col, qc, sizeof qc);
    snprintf(q, sizeof q, "SELECT id FROM %s WHERE %s = ?", qt, qc);
    return run_ids(f->db, q, &key, &klen, 1);
}

static int my_index_drop(mvx_file *fh, const char *item) {
    my_file *f = (my_file *)fh;
    char qt[300], nm[512], qn[600], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    index_name(f, item, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    snprintf(sql, sizeof sql, "DROP INDEX %s ON %s", qn, qt);
    return exec_sql(f->db, sql) || mysql_errno(f->db) == 1091;  /* absent = done */
}

/* ----------------------------------------------------- WITH push-down */

static mvx_cursor *my_select_where(mvx_file *fh, const char *col,
                                   const char *op, const char *val,
                                   int64_t vlen) {
    my_file *f = (my_file *)fh;
    const char *o = sql_op(op);
    if (!o || !col || !col[0]) return NULL;
    char qt[300], ex[400], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    field_expr(col, 0, NULL, ex, sizeof ex);
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s %s ?", qt, ex, o);
    return run_ids(f->db, sql, &val, &vlen, 1);
}

static mvx_cursor *my_select_attr(mvx_file *fh, int64_t attr, const char *op,
                                  const char *val, int64_t vlen) {
    my_file *f = (my_file *)fh;
    const char *o = sql_op(op);
    if (!o || attr < 1) return NULL;
    char qt[300], ex[400], sql[1200];
    quote_ident(f->table, qt, sizeof qt);
    field_expr(NULL, attr, NULL, ex, sizeof ex);
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s %s ?", qt, ex, o);
    return run_ids(f->db, sql, &val, &vlen, 1);
}

static mvx_cursor *my_select_multi(mvx_file *fh, const mvx_pred *preds,
                                   int npred) {
    my_file *f = (my_file *)fh;
    if (npred < 1 || npred > 32) return NULL;
    char qt[300], sql[8192];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE ", qt);
    const char *vals[32];
    int64_t lens[32];
    for (int i = 0; i < npred; i++) {
        const char *o = sql_op(preds[i].op);
        if (!o) return NULL;
        char ex[600];
        field_expr(preds[i].col, preds[i].attr, NULL, ex, sizeof ex);
        /* A numeric comparison casts both sides, so 9 < 10 rather than
           "10" < "9" -- the distinction MV draws between a numeric and a
           text field. */
        if (preds[i].numeric)
            p += (size_t)snprintf(sql + p, sizeof sql - p,
                                  "%sCAST(%s AS DECIMAL(38,10)) %s "
                                  "CAST(? AS DECIMAL(38,10))",
                                  i ? " AND " : "", ex, o);
        else
            p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s %s ?",
                                  i ? " AND " : "", ex, o);
        if (p >= sizeof sql) return NULL;
        vals[i] = preds[i].val;
        lens[i] = preds[i].vlen;
    }
    return run_ids(f->db, sql, vals, lens, npred);
}

/* ORDER BY / LIMIT.  No COLLATE clause: the columns are VARBINARY, which
   already orders by bytes -- which is what MV's sort is. */
static mvx_cursor *my_select_order(mvx_file *fh, const char *fcol,
                                   int64_t fattr, const char *fop,
                                   const char *fval, int64_t fvlen,
                                   const char *ocol, int otext,
                                   int64_t limit) {
    my_file *f = (my_file *)fh;
    (void)otext;
    if (!ocol || !ocol[0]) return NULL;
    char qt[300], qo[300], sql[1600];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(ocol, qo, sizeof qo);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    int nb = 0;
    if (fop && fop[0]) {
        const char *o = sql_op(fop);
        if (!o) return NULL;
        char ex[600];
        field_expr(fcol, fattr, NULL, ex, sizeof ex);
        p += (size_t)snprintf(sql + p, sizeof sql - p, " WHERE %s %s ?", ex, o);
        nb = 1;
    }
    p += (size_t)snprintf(sql + p, sizeof sql - p, " ORDER BY %s", qo);
    if (limit > 0)
        snprintf(sql + p, sizeof sql - p, " LIMIT %lld", (long long)limit);
    return run_ids(f->db, sql, &fval, &fvlen, nb);
}

/* --------------------------------------------------- COUNT and SUM */

static int64_t my_count_where(mvx_file *fh, const char *col, int64_t attr,
                              const char *op, const char *val, int64_t vlen) {
    my_file *f = (my_file *)fh;
    char qt[300], sql[1200];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s", qt);
    int nb = 0;
    if (op && op[0]) {
        const char *o = sql_op(op);
        if (!o) return -1;
        char ex[600];
        field_expr(col, attr, NULL, ex, sizeof ex);
        snprintf(sql + p, sizeof sql - p, " WHERE %s %s ?", ex, o);
        nb = 1;
    }
    MYSQL_STMT *st = mysql_stmt_init(f->db);
    if (!st) return -1;
    if (mysql_stmt_prepare(st, sql, (unsigned long)strlen(sql)) != 0) {
        mysql_stmt_close(st); return -1;
    }
    MYSQL_BIND ib;
    unsigned long il = (unsigned long)vlen;
    if (nb) {
        memset(&ib, 0, sizeof ib);
        ib.buffer_type = MYSQL_TYPE_BLOB; ib.buffer = (void *)val;
        ib.buffer_length = il; ib.length = &il;
        mysql_stmt_bind_param(st, &ib);
    }
    int64_t n = -1;
    if (mysql_stmt_execute(st) == 0) {
        long long out = 0;
        MYSQL_BIND ob;
        memset(&ob, 0, sizeof ob);
        ob.buffer_type = MYSQL_TYPE_LONGLONG; ob.buffer = &out;
        if (mysql_stmt_bind_result(st, &ob) == 0 &&
            mysql_stmt_store_result(st) == 0 &&
            mysql_stmt_fetch(st) == 0)
            n = (int64_t)out;
    }
    mysql_stmt_close(st);
    return n;
}

static int my_sum_where(mvx_file *fh, const char *sumcol, const char *fcol,
                        int64_t fattr, const char *fop, const char *fval,
                        int64_t fvlen, char *out, size_t cap) {
    my_file *f = (my_file *)fh;
    if (!sumcol || !sumcol[0]) return 0;
    char qt[300], qs[300], sql[1400], esc[600];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(sumcol, qs, sizeof qs);
    size_t p = (size_t)snprintf(sql, sizeof sql,
        "SELECT COALESCE(SUM(CAST(%s AS DECIMAL(38,10))),0) FROM %s", qs, qt);
    if (fop && fop[0]) {
        const char *o = sql_op(fop);
        if (!o) return 0;
        char ex[600];
        field_expr(fcol, fattr, NULL, ex, sizeof ex);
        mysql_real_escape_string(f->db, esc, fval, (unsigned long)fvlen);
        snprintf(sql + p, sizeof sql - p, " WHERE %s %s '%s'", ex, o, esc);
    }
    if (!exec_sql(f->db, sql)) return 0;
    MYSQL_RES *r = mysql_store_result(f->db);
    MYSQL_ROW row = r ? mysql_fetch_row(r) : NULL;
    int ok = 0;
    if (row && row[0]) { snprintf(out, cap, "%s", row[0]); ok = 1; }
    if (r) mysql_free_result(r);
    return ok;
}

/* --------------------------------------------- cross-file TRANS() JOIN */

static mvx_cursor *my_select_join(mvx_file *srch, int64_t sk,
                                  const char *src_keycol, mvx_file *tgth,
                                  int64_t ta, const char *tgt_col,
                                  const char *op, const char *val,
                                  int64_t vlen) {
    if (!op || strcmp(op, "=") != 0) return NULL;    /* equality only */
    my_file *s = (my_file *)srch, *t = (my_file *)tgth;
    if (s->db != t->db) return NULL;                 /* not co-located */
    char sqt[300], tqt[300], skx[600], tax[600];
    quote_ident(s->table, sqt, sizeof sqt);
    quote_ident(t->table, tqt, sizeof tqt);
    field_expr(src_keycol, sk, "s", skx, sizeof skx);
    field_expr(tgt_col, ta, "t", tax, sizeof tax);
    /* The source key may be multivalued: classic TRANS maps element-wise,
       so a record matches when ANY of its @VM elements points at a target
       row satisfying the filter.  Wrapping both sides in the delimiter
       makes LOCATE an EXACT element test -- without it "10" would match
       inside "100".  DISTINCT collapses a source record matching through
       more than one of its key values. */
    char sql[2400];
    snprintf(sql, sizeof sql,
             "SELECT DISTINCT s.id FROM %s s JOIN %s t "
             "ON LOCATE(CONCAT(CHAR(253), t.id, CHAR(253)), "
             "CONCAT(CHAR(253), %s, CHAR(253))) > 0 "
             "WHERE %s = ?", sqt, tqt, skx, tax);
    return run_ids(s->db, sql, &val, &vlen, 1);
}

/* ------------------------------------------------------------ explain */

static int my_explain(mvx_file *fh, const mvx_pred *preds, int npred,
                      const char *ocol, int otext, int64_t limit,
                      char *out, size_t cap) {
    my_file *f = (my_file *)fh;
    (void)otext;
    char qt[300], sql[6000];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    for (int i = 0; i < npred; i++) {
        const char *o = sql_op(preds[i].op);
        if (!o) return 0;
        char ex[600];
        field_expr(preds[i].col, preds[i].attr, NULL, ex, sizeof ex);
        if (preds[i].numeric)
            p += (size_t)snprintf(sql + p, sizeof sql - p,
                                  "%sCAST(%s AS DECIMAL(38,10)) %s "
                                  "CAST(? AS DECIMAL(38,10))",
                                  i ? " AND " : " WHERE ", ex, o);
        else
            p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s %s ?",
                                  i ? " AND " : " WHERE ", ex, o);
        if (p >= sizeof sql) return 0;
    }
    if (ocol && ocol[0]) {
        char qo[300];
        quote_ident(ocol, qo, sizeof qo);
        p += (size_t)snprintf(sql + p, sizeof sql - p, " ORDER BY %s", qo);
    }
    if (limit > 0)
        snprintf(sql + p, sizeof sql - p, " LIMIT %lld", (long long)limit);
    /* No driver-name prefix: the runtime renders "<driver>: <plan>". */
    snprintf(out, cap, "%s", sql);
    return 1;
}

/* ------------------------------------------------------------- vtable */

static const mvx_driver mvx_driver_mysql = {
    "mysql",
    my_open, my_close,
    my_read, my_write, my_del,
    my_select_begin, my_select_next, my_select_end,
    my_create, my_remove,
    my_names,                             /* whole-account LISTF */
    NULL, NULL,                           /* the backend keeps the columns and
                                             their indexes itself */
    my_index_select, my_index_drop,
    NULL, NULL,                           /* locks: the runtime's table */
    my_map_ensure, my_map_apply,
    my_map_child_ensure, my_map_child_apply,
    my_map_drop,
    my_map_read, my_map_child_read,
    my_index_create,
    my_select_where,
    my_select_attr,
    my_select_join,                       /* cross-file TRANS() JOIN */
    my_count_where,
    my_sum_where,
    my_select_order,
    my_select_multi,
    my_explain,
    my_select_count,
    my_bulk_begin, my_bulk_commit,
    NULL,                                 /* map_backfill: the runtime's
                                             per-record loop is correct */
    NULL,                                 /* select_join_order: the verb sorts
                                             the reference itself */
};

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_mysql : NULL;
}
