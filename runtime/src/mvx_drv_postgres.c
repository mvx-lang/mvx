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

/* postgres driver — a MultiValue file on a PostgreSQL table.
 *
 * Each account/namespace is a schema; each file is a table
 * (id BYTEA PRIMARY KEY, rec BYTEA) in it, so records round-trip
 * byte-exact (marks and all).  The connection is a named profile
 * (BINDINGS `ORDERS @pgmain`, .mvx-private/connections carries
 * driver/address/dbname/user/password/namespace) — the same indirection
 * the lmdbnet driver uses.
 *
 * Minimal contract only: no native secondary indexes (the runtime falls
 * back to a scan) and no lock authority (the process-local lock table
 * applies) in this first cut — both are follow-ups.
 */
#include "../include/mvx_driver.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNS 8
static struct {
    char loc[1024];
    PGconn *conn;
} g_conns[MAX_CONNS];
static int g_nconns;

struct mvx_cursor {
    mv_value *ids;
    int64_t n, pos;
};

typedef struct {
    mvx_file_base base;
    PGconn *conn;
    char schema[128];
    char table[256];
} pg_file;

static const mvx_driver mvx_driver_postgres;

/* Keep libpq NOTICEs (e.g. "schema already exists") off the client. */
static void noop_notice(void *arg, const char *message) {
    (void)arg;
    (void)message;
}

/* Connect (once per location).  loc is "@connname" (resolved from the
   connection profile) or a raw libpq conninfo string; the namespace maps
   to a schema. */
static PGconn *pg_connect(const char *loc, char *schema, size_t scap,
                          char *err, size_t errlen) {
    for (int i = 0; i < g_nconns; i++)
        if (strcmp(g_conns[i].loc, loc) == 0) {
            /* schema still needed by the caller */
            break;
        }

    /* resolve the schema regardless of cache hit */
    if (loc[0] == '@') {
        const char *cn = loc + 1;
        if (!mvx_conn_lookup(cn, "namespace", schema, scap))
            mvx_account_namespace(schema, scap);
    } else {
        mvx_account_namespace(schema, scap);
    }

    for (int i = 0; i < g_nconns; i++)
        if (strcmp(g_conns[i].loc, loc) == 0) return g_conns[i].conn;

    if (g_nconns >= MAX_CONNS) {
        snprintf(err, errlen, "postgres: too many connections");
        return NULL;
    }

    PGconn *c;
    if (loc[0] == '@') {
        const char *cn = loc + 1;
        char address[256] = "", dbname[128] = "", user[128] = "",
             password[256] = "";
        mvx_conn_lookup(cn, "address", address, sizeof address);
        mvx_conn_lookup(cn, "dbname", dbname, sizeof dbname);
        mvx_conn_lookup(cn, "user", user, sizeof user);
        mvx_conn_lookup(cn, "password", password, sizeof password);
        char host[256] = "", port[16] = "5432";
        const char *colon = strrchr(address, ':');
        if (colon) {
            size_t hl = (size_t)(colon - address);
            if (hl >= sizeof host) hl = sizeof host - 1;
            memcpy(host, address, hl);
            host[hl] = '\0';
            snprintf(port, sizeof port, "%s", colon + 1);
        } else {
            snprintf(host, sizeof host, "%s", address);
        }
        const char *keys[] = {"host", "port",     "dbname",
                              "user", "password", NULL};
        const char *vals[] = {host, port, dbname, user, password, NULL};
        c = PQconnectdbParams(keys, vals, 0);
    } else {
        c = PQconnectdb(loc);
    }
    if (!c || PQstatus(c) != CONNECTION_OK) {
        snprintf(err, errlen, "postgres: %s",
                 c ? PQerrorMessage(c) : "connection failed");
        if (c) PQfinish(c);
        return NULL;
    }
    PQsetNoticeProcessor(c, noop_notice, NULL);   /* swallow NOTICEs */
    snprintf(g_conns[g_nconns].loc, sizeof g_conns[0].loc, "%s", loc);
    g_conns[g_nconns].conn = c;
    g_nconns++;
    return c;
}

/* "schema"."table", identifiers safely quoted. */
static void qualify(PGconn *c, const char *schema, const char *table,
                    char *out, size_t cap) {
    char *s = PQescapeIdentifier(c, schema, strlen(schema));
    char *t = PQescapeIdentifier(c, table, strlen(table));
    snprintf(out, cap, "%s.%s", s ? s : "\"\"", t ? t : "\"\"");
    if (s) PQfreemem(s);
    if (t) PQfreemem(t);
}

/* Split "loc\nspec" into loc and the bare file spec. */
static const char *split_spec(const char *spec, char *loc, size_t cap) {
    const char *nl = strchr(spec, '\n');
    if (!nl) {
        loc[0] = '\0';
        return spec;
    }
    size_t n = (size_t)(nl - spec);
    if (n >= cap) n = cap - 1;
    memcpy(loc, spec, n);
    loc[n] = '\0';
    return nl + 1;
}

/* An IMMUTABLE helper for the blob attribute expression: convert_from is only
   STABLE, so the raw split_part expression cannot go in an index.  Wrapping
   it in a function we declare IMMUTABLE (safe — the database encoding is
   fixed) makes both the expression index and the push-down query index-
   eligible, and using the same schema-qualified function in both makes them
   match.  attr N is the field between the (N-1)th and Nth field mark. */
static void pg_ensure_attr_fn(PGconn *c, const char *schema) {
    char *qs = PQescapeIdentifier(c, schema, strlen(schema));
    char sql[512];
    snprintf(sql, sizeof sql,
             "CREATE OR REPLACE FUNCTION %s.mvx_attr(bytea, integer) "
             "RETURNS text LANGUAGE sql IMMUTABLE PARALLEL SAFE AS "
             "$f$ SELECT split_part(convert_from($1,'LATIN1'),chr(254),$2) $f$",
             qs ? qs : "\"\"");
    if (qs) PQfreemem(qs);
    PGresult *r = PQexec(c, sql);
    if (r) PQclear(r);
}

static mvx_file *pg_open(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *rspec = split_spec(spec, loc, sizeof loc);
    char schema[128];
    PGconn *c = pg_connect(loc, schema, sizeof schema, err, errlen);
    if (!c) return NULL;
    char qt[512];
    qualify(c, schema, rspec, qt, sizeof qt);
    const char *pv[1] = {qt};             /* "schema"."table" as text */
    PGresult *r = PQexecParams(c, "SELECT to_regclass($1)", 1, NULL, pv,
                               NULL, NULL, 0);
    int exists = r && PQresultStatus(r) == PGRES_TUPLES_OK &&
                 PQntuples(r) == 1 && !PQgetisnull(r, 0, 0);
    if (r) PQclear(r);
    if (!exists) return NULL;             /* not found: normal ELSE path */

    pg_file *f = calloc(1, sizeof(pg_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_postgres;
    f->base.spec = strdup(spec);
    f->conn = c;
    snprintf(f->schema, sizeof f->schema, "%s", schema);
    snprintf(f->table, sizeof f->table, "%s", rspec);
    pg_ensure_attr_fn(c, schema);         /* for blob expression indexes */
    return (mvx_file *)f;
}

static void pg_close(mvx_file *fh) {
    pg_file *f = (pg_file *)fh;
    free(f->base.spec);
    free(f);                              /* the PGconn is pooled */
}

static int pg_read(mvx_file *fh, const char *id, int64_t idlen,
                   mv_value *rec) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[640];
    snprintf(sql, sizeof sql, "SELECT rec FROM %s WHERE id=$1", qt);
    const char *pv[1] = {id};
    int pl[1] = {(int)idlen};
    int pf[1] = {1};                      /* binary id */
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 1);
    int ok = r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1;
    if (ok)
        mv_set_str(rec, PQgetvalue(r, 0, 0), PQgetlength(r, 0, 0));
    if (r) PQclear(r);
    return ok;
}

static int pg_write(mvx_file *fh, const char *id, int64_t idlen,
                    const mv_value *rec) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[768];
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (id, rec) VALUES ($1,$2) "
             "ON CONFLICT (id) DO UPDATE SET rec=EXCLUDED.rec",
             qt);
    char nb[40];
    const char *rp;
    int64_t rl = mv_val_chars(rec, nb, sizeof nb, &rp);
    const char *pv[2] = {id, rp};
    int pl[2] = {(int)idlen, (int)rl};
    int pf[2] = {1, 1};                   /* binary id + rec */
    PGresult *r = PQexecParams(f->conn, sql, 2, NULL, pv, pl, pf, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

static int pg_del(mvx_file *fh, const char *id, int64_t idlen) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[640];
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE id=$1", qt);
    const char *pv[1] = {id};
    int pl[1] = {(int)idlen};
    int pf[1] = {1};
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 0);
    int deleted = r && PQresultStatus(r) == PGRES_COMMAND_OK &&
                  atoi(PQcmdTuples(r)) > 0;
    if (r) PQclear(r);
    return deleted;
}

/* Snapshot the id list up front (a short read), then stream it. */
static mvx_cursor *pg_select_begin(mvx_file *fh) {
    pg_file *f = (pg_file *)fh;
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select");
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[640];
    snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    PGresult *r = PQexecParams(f->conn, sql, 0, NULL, NULL, NULL, NULL, 1);
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK) {
        int n = PQntuples(r);
        cur->ids = calloc(n ? n : 1, sizeof(mv_value));
        if (!cur->ids) mvx_fatal("out of memory in postgres select");
        for (int i = 0; i < n; i++) {
            mv_init(&cur->ids[cur->n]);
            mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                       PQgetlength(r, i, 0));
            cur->n++;
        }
    }
    if (r) PQclear(r);
    return cur;
}

static int pg_select_next(mvx_cursor *c, mv_value *id) {
    if (!c || c->pos >= c->n) return 0;
    mv_copy(id, &c->ids[c->pos++]);
    return 1;
}

static void pg_select_end(mvx_cursor *c) {
    if (!c) return;
    for (int64_t i = 0; i < c->n; i++) mv_clear(&c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t pg_select_count(mvx_cursor *c) { return c ? c->n : 0; }

/* Bulk backfill batching (#55): bracket the per-record UPDATE/DELETE/INSERT
   stream in one transaction on the file's connection, so a million-row backfill
   pays a handful of commits, not a million.  A COMMIT of a transaction that has
   already errored performs a ROLLBACK, so the failure path is safe. */
/* Atomicity for an operation the DRIVER owns end to end -- see the sqlite and
   mysql drivers for the reasoning; pg_map_child_apply has the same DELETE +
   N INSERT shape and the same half-applied failure. */
static int txn_begin(PGconn *c) {
    if (PQtransactionStatus(c) != PQTRANS_IDLE) return 0;
    PGresult *r = PQexec(c, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok ? 1 : 0;
}
static void txn_end(PGconn *c, int started, int ok) {
    if (!started) return;
    PGresult *r = PQexec(c, ok ? "COMMIT" : "ROLLBACK");
    if (r) PQclear(r);
}

static int pg_bulk_begin(mvx_file *fh) {
    pg_file *f = (pg_file *)fh;
    /* Already inside one -- a mapped write bracketing itself during a backfill
       -- so do nothing and let the outer transaction carry the atomicity.
       postgres would only warn ("there is already a transaction in progress")
       and carry on, which is worse: the inner COMMIT would then end the OUTER
       transaction early and a backfill would lose its batching. */
    if (PQtransactionStatus(f->conn) != PQTRANS_IDLE) return 0;
    PGresult *r = PQexec(f->conn, "BEGIN");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok ? 1 : 0;
}

/* Discard everything since bulk_begin -- see the sqlite driver. */
static int pg_rollback(mvx_file *fh) {
    pg_file *f = (pg_file *)fh;
    if (PQtransactionStatus(f->conn) == PQTRANS_IDLE) return 1;
    PGresult *r = PQexec(f->conn, "ROLLBACK");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

static int pg_bulk_commit(mvx_file *fh) {
    pg_file *f = (pg_file *)fh;
    if (PQtransactionStatus(f->conn) == PQTRANS_IDLE) return 1;
    PGresult *r = PQexec(f->conn, "COMMIT");
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

static int64_t pg_map_backfill(mvx_file *fh, const mvx_mapfield *cols,
                               const int64_t *anos, const char **convs,
                               const char **assocs, int nf, char *err,
                               size_t errlen) {
    pg_file *f = (pg_file *)fh;
    if (nf <= 0) return 0;
    for (int i = 0; i < nf; i++) {
        if (assocs[i][0]) return MVX_MAP_NOPUSH;         /* association -> loop */
        const char *t = cols[i].type;
        int conv = convs[i][0] != '\0';
        int ok = (!conv && (strcmp(t, "TEXT") == 0 ||
                            strcmp(t, "NUMERIC") == 0)) ||
                 strcmp(t, "DATE") == 0 || strcmp(t, "TIME") == 0;
        if (!ok) return MVX_MAP_NOPUSH;                  /* OCONV -> loop */
    }

    pg_ensure_attr_fn(f->conn, f->schema);              /* mvx_attr(rec,n) */
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qsch = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
    if (!qsch) { snprintf(err, errlen, "postgres: out of memory"); return -1; }

    size_t cap = 512 + (size_t)nf * 512;
    char *sql = malloc(cap);
    if (!sql) { PQfreemem(qsch); snprintf(err, errlen, "postgres: out of memory");
                return -1; }
    size_t p = (size_t)snprintf(sql, cap, "UPDATE %s SET ", qt);
    for (int i = 0; i < nf; i++) {
        char *qc = PQescapeIdentifier(f->conn, cols[i].name,
                                      strlen(cols[i].name));
        char a[160];                                     /* schema.mvx_attr(rec,n) */
        snprintf(a, sizeof a, "%s.mvx_attr(rec,%lld)", qsch, (long long)anos[i]);
        const char *t = cols[i].type;
        char expr[512];
        if (strcmp(t, "NUMERIC") == 0)
            snprintf(expr, sizeof expr, "NULLIF(%s,'')::numeric", a);
        else if (strcmp(t, "DATE") == 0)
            snprintf(expr, sizeof expr,
                     "CASE WHEN NULLIF(%s,'') IS NULL THEN NULL "
                     "ELSE DATE '1967-12-31' + (%s)::int END", a, a);
        else if (strcmp(t, "TIME") == 0)
            snprintf(expr, sizeof expr,
                     "CASE WHEN NULLIF(%s,'') IS NULL THEN NULL "
                     "ELSE TIME '00:00:00' + (%s)::int * interval '1 second' END",
                     a, a);
        else                                             /* TEXT, no conv */
            snprintf(expr, sizeof expr, "NULLIF(%s,'')", a);
        p += (size_t)snprintf(sql + p, cap - p, "%s%s=%s", i ? ", " : "",
                              qc ? qc : "\"\"", expr);
        if (qc) PQfreemem(qc);
    }
    PQfreemem(qsch);

    PGresult *r = PQexec(f->conn, sql);
    free(sql);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    int64_t n = ok ? atoll(PQcmdTuples(r)) : -1;
    if (!ok) snprintf(err, errlen, "postgres: %s", PQerrorMessage(f->conn));
    if (r) PQclear(r);
    return n;
}

static int pg_create(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *rspec = split_spec(spec, loc, sizeof loc);
    char schema[128];
    PGconn *c = pg_connect(loc, schema, sizeof schema, err, errlen);
    if (!c) return 0;
    char *qs = PQescapeIdentifier(c, schema, strlen(schema));
    char csql[256];
    snprintf(csql, sizeof csql, "CREATE SCHEMA IF NOT EXISTS %s",
             qs ? qs : "\"\"");
    if (qs) PQfreemem(qs);
    PGresult *r = PQexec(c, csql);
    if (r) PQclear(r);

    char qt[512];
    qualify(c, schema, rspec, qt, sizeof qt);
    char sql[700];
    snprintf(sql, sizeof sql,
             "CREATE TABLE %s (id bytea primary key, rec bytea)", qt);
    r = PQexec(c, sql);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok && r) {
        /* 42P07 = duplicate_table: create returns 0 if it exists */
        const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        if (!(sqlstate && strcmp(sqlstate, "42P07") == 0))
            snprintf(err, errlen, "postgres: %s", PQerrorMessage(c));
    }
    if (r) PQclear(r);
    return ok;
}

static int pg_remove(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *rspec = split_spec(spec, loc, sizeof loc);
    char schema[128];
    PGconn *c = pg_connect(loc, schema, sizeof schema, err, errlen);
    if (!c) return 0;
    char qt[512];
    qualify(c, schema, rspec, qt, sizeof qt);
    char sql[560];
    snprintf(sql, sizeof sql, "DROP TABLE %s", qt);
    PGresult *r = PQexec(c, sql);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

/* Relational mapping (#18): project attributes into columns / child
   tables.  NUMERIC fields get a numeric column; others are text. */
static const char *pg_sqltype(const char *t) {
    if (strcmp(t, "NUMERIC") == 0) return "numeric";
    if (strcmp(t, "DATE") == 0) return "date";
    if (strcmp(t, "TIME") == 0) return "time";
    return "text";                                    /* TEXT (default) */
}

static int pg_map_ensure(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                         char *err, size_t errlen) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    for (int i = 0; i < ncols; i++) {
        char *qc = PQescapeIdentifier(f->conn, cols[i].name,
                                      strlen(cols[i].name));
        char sql[768];
        snprintf(sql, sizeof sql,
                 "ALTER TABLE %s ADD COLUMN IF NOT EXISTS %s %s", qt,
                 qc ? qc : "\"\"", pg_sqltype(cols[i].type));
        if (qc) PQfreemem(qc);
        PGresult *r = PQexec(f->conn, sql);
        int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
        if (!ok) {
            snprintf(err, errlen, "postgres: %s", PQerrorMessage(f->conn));
            return 0;
        }
    }
    return 1;
}

static int pg_map_apply(mvx_file *fh, const char *id, int64_t idlen,
                        const mvx_mapfield *cols, const char **vals,
                        const int64_t *vlens, int ncols) {
    pg_file *f = (pg_file *)fh;
    if (ncols <= 0) return 1;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[4096];
    size_t p = 0;
    p += (size_t)snprintf(sql + p, sizeof sql - p, "UPDATE %s SET ", qt);
    for (int i = 0; i < ncols && p < sizeof sql; i++) {
        char *qc = PQescapeIdentifier(f->conn, cols[i].name,
                                      strlen(cols[i].name));
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s=$%d",
                              i ? ", " : "", qc ? qc : "\"\"", i + 1);
        if (qc) PQfreemem(qc);
    }
    snprintf(sql + p, sizeof sql - p, " WHERE id=$%d", ncols + 1);

    const char *pv[64];
    int pl[64], pf[64];
    if (ncols > 63) return 0;
    for (int i = 0; i < ncols; i++) {
        pv[i] = vals[i];
        pl[i] = (int)vlens[i];
        pf[i] = 0;                        /* text */
    }
    pv[ncols] = id;
    pl[ncols] = (int)idlen;
    pf[ncols] = 1;                        /* binary id */
    for (int i = 0; i < ncols; i++)
        if (vlens[i] == 0) pv[i] = NULL;  /* empty -> SQL NULL */
    PGresult *r = PQexecParams(f->conn, sql, ncols + 1, NULL, pv, pl, pf, 0);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

/* An association's child table: <schema>."<table>_<assoc>". */
static void child_qualify(pg_file *f, const char *assoc, char *out,
                          size_t cap) {
    char t[384];
    snprintf(t, sizeof t, "%s_%s", f->table, assoc);
    qualify(f->conn, f->schema, t, out, cap);
}

static int pg_map_child_ensure(mvx_file *fh, const char *assoc,
                               const mvx_mapfield *cols, int ncols,
                               char *err, size_t errlen) {
    pg_file *f = (pg_file *)fh;
    char qt[640];
    child_qualify(f, assoc, qt, sizeof qt);
    char sql[4096];
    size_t p = 0;
    p += (size_t)snprintf(sql + p, sizeof sql - p,
                          "CREATE TABLE IF NOT EXISTS %s (id bytea, seq int",
                          qt);
    for (int i = 0; i < ncols && p < sizeof sql; i++) {
        char *qc = PQescapeIdentifier(f->conn, cols[i].name,
                                      strlen(cols[i].name));
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", %s %s",
                              qc ? qc : "\"\"", pg_sqltype(cols[i].type));
        if (qc) PQfreemem(qc);
    }
    snprintf(sql + p, sizeof sql - p, ", PRIMARY KEY (id, seq))");
    PGresult *r = PQexec(f->conn, sql);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok && r)
        snprintf(err, errlen, "postgres: %s", PQerrorMessage(f->conn));
    if (r) PQclear(r);
    return ok;
}

static int pg_map_child_apply(mvx_file *fh, const char *id, int64_t idlen,
                              const char *assoc, const mvx_mapfield *cols,
                              int ncols, const char **vals,
                              const int64_t *vlens, int nrows) {
    pg_file *f = (pg_file *)fh;
    char qt[640];
    child_qualify(f, assoc, qt, sizeof qt);
    /* The DELETE and the INSERTs are one replacement, not a sequence. */
    int started = txn_begin(f->conn);

    /* replace: delete the record's rows, then insert the new ones */
    char dsql[720];
    snprintf(dsql, sizeof dsql, "DELETE FROM %s WHERE id=$1", qt);
    const char *dpv[1] = {id};
    int dpl[1] = {(int)idlen};
    int dpf[1] = {1};
    PGresult *dr = PQexecParams(f->conn, dsql, 1, NULL, dpv, dpl, dpf, 0);
    int ok = dr && PQresultStatus(dr) == PGRES_COMMAND_OK;
    if (dr) PQclear(dr);
    if (!ok) { txn_end(f->conn, started, 0); return 0; }

    /* column-name list for the INSERT */
    char collist[2048];
    size_t cp = 0;
    for (int c = 0; c < ncols; c++) {
        char *qc = PQescapeIdentifier(f->conn, cols[c].name,
                                      strlen(cols[c].name));
        cp += (size_t)snprintf(collist + cp, sizeof collist - cp, ", %s",
                               qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    }
    collist[sizeof collist - 1] = '\0';

    for (int r = 0; r < nrows; r++) {
        char isql[3072];
        size_t ip = 0;
        ip += (size_t)snprintf(isql + ip, sizeof isql - ip,
                               "INSERT INTO %s (id, seq%s) VALUES ($1, $2",
                               qt, collist);
        for (int c = 0; c < ncols; c++)
            ip += (size_t)snprintf(isql + ip, sizeof isql - ip, ", $%d",
                                   c + 3);
        snprintf(isql + ip, sizeof isql - ip, ")");

        char seqbuf[16];
        snprintf(seqbuf, sizeof seqbuf, "%d", r + 1);
        const char *pv[66];
        int pl[66], pf[66];
        if (ncols > 63) { txn_end(f->conn, started, 0); return 0; }
        pv[0] = id; pl[0] = (int)idlen; pf[0] = 1;          /* id (binary) */
        pv[1] = seqbuf; pl[1] = 0; pf[1] = 0;               /* seq (text) */
        for (int c = 0; c < ncols; c++) {
            int64_t vl = vlens[r * ncols + c];
            pv[c + 2] = vl > 0 ? vals[r * ncols + c] : NULL;  /* -> NULL */
            pl[c + 2] = (int)vl;
            pf[c + 2] = 0;
        }
        PGresult *ir =
            PQexecParams(f->conn, isql, ncols + 2, NULL, pv, pl, pf, 0);
        int iok = ir && PQresultStatus(ir) == PGRES_COMMAND_OK;
        if (ir) PQclear(ir);
        if (!iok) { txn_end(f->conn, started, 0); return 0; }
    }
    txn_end(f->conn, started, 1);
    return 1;
}

static int pg_map_drop(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                       const char **assocs, int nassocs, char *err,
                       size_t errlen) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    for (int i = 0; i < ncols; i++) {
        char *qc = PQescapeIdentifier(f->conn, cols[i].name,
                                      strlen(cols[i].name));
        char sql[768];
        snprintf(sql, sizeof sql,
                 "ALTER TABLE %s DROP COLUMN IF EXISTS %s", qt,
                 qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
        PGresult *r = PQexec(f->conn, sql);
        int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (!ok && r) snprintf(err, errlen, "postgres: %s",
                               PQerrorMessage(f->conn));
        if (r) PQclear(r);
        if (!ok) return 0;
    }
    for (int a = 0; a < nassocs; a++) {
        char cqt[640];
        child_qualify(f, assocs[a], cqt, sizeof cqt);
        char sql[720];
        snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s", cqt);
        PGresult *r = PQexec(f->conn, sql);
        int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
        if (r) PQclear(r);
        if (!ok) return 0;
    }
    return 1;
}

/* Native read-back: pull the mapped parent columns for one id. */
static int pg_map_read(mvx_file *fh, const char *id, int64_t idlen,
                       const mvx_mapfield *cols, int ncols,
                       char **vals, int64_t *lens) {
    pg_file *f = (pg_file *)fh;
    for (int i = 0; i < ncols; i++) { vals[i] = NULL; lens[i] = 0; }
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[4096];
    size_t p = 0;
    p += (size_t)snprintf(sql + p, sizeof sql - p, "SELECT ");
    if (ncols == 0) p += (size_t)snprintf(sql + p, sizeof sql - p, "1");
    for (int i = 0; i < ncols && p < sizeof sql; i++) {
        char *qc = PQescapeIdentifier(f->conn, cols[i].name,
                                      strlen(cols[i].name));
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s",
                              i ? ", " : "", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    }
    snprintf(sql + p, sizeof sql - p, " FROM %s WHERE id=$1", qt);
    const char *pv[1] = {id};
    int pl[1] = {(int)idlen}, pf[1] = {1};
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 0);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return -1;
    }
    if (PQntuples(r) == 0) { PQclear(r); return 0; }
    for (int i = 0; i < ncols; i++) {
        if (PQgetisnull(r, 0, i)) continue;
        int L = PQgetlength(r, 0, i);
        char *v = malloc((size_t)L + 1);
        if (!v) continue;
        memcpy(v, PQgetvalue(r, 0, i), (size_t)L);
        v[L] = '\0';
        vals[i] = v;
        lens[i] = L;
    }
    PQclear(r);
    return 1;
}

/* Native read-back: pull one association's child rows, ordered by seq. */
static int pg_map_child_read(mvx_file *fh, const char *id, int64_t idlen,
                             const char *assoc, const mvx_mapfield *cols,
                             int ncols, char ***cells, int64_t **lens,
                             int *nrows) {
    pg_file *f = (pg_file *)fh;
    *cells = NULL; *lens = NULL; *nrows = 0;
    char qt[640];
    child_qualify(f, assoc, qt, sizeof qt);
    char sql[4096];
    size_t p = 0;
    p += (size_t)snprintf(sql + p, sizeof sql - p, "SELECT ");
    if (ncols == 0) p += (size_t)snprintf(sql + p, sizeof sql - p, "1");
    for (int c = 0; c < ncols && p < sizeof sql; c++) {
        char *qc = PQescapeIdentifier(f->conn, cols[c].name,
                                      strlen(cols[c].name));
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s",
                              c ? ", " : "", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    }
    snprintf(sql + p, sizeof sql - p, " FROM %s WHERE id=$1 ORDER BY seq", qt);
    const char *pv[1] = {id};
    int pl[1] = {(int)idlen}, pf[1] = {1};
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 0);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return -1;
    }
    int nr = PQntuples(r);
    if (nr == 0 || ncols == 0) { PQclear(r); *nrows = nr; return 1; }
    size_t total = (size_t)nr * (size_t)ncols;
    char **cv = calloc(total, sizeof *cv);
    int64_t *cl = calloc(total, sizeof *cl);
    if (!cv || !cl) { free(cv); free(cl); PQclear(r); return -1; }
    for (int rr = 0; rr < nr; rr++)
        for (int c = 0; c < ncols; c++) {
            size_t idx = (size_t)rr * (size_t)ncols + (size_t)c;
            if (PQgetisnull(r, rr, c)) continue;
            int L = PQgetlength(r, rr, c);
            char *v = malloc((size_t)L + 1);
            if (!v) continue;
            memcpy(v, PQgetvalue(r, rr, c), (size_t)L);
            v[L] = '\0';
            cv[idx] = v;
            cl[idx] = L;
        }
    PQclear(r);
    *cells = cv; *lens = cl; *nrows = nr;
    return 1;
}

/* Index name for a mapped column: "<table>_<item>_idx" (schema-scoped). */
static char *pg_index_ident(pg_file *f, const char *item) {
    char nm[512];
    snprintf(nm, sizeof nm, "%s_%s_idx", f->table, item);
    return PQescapeIdentifier(f->conn, nm, strlen(nm));
}

/* CREATE INDEX on the mapped column (the mapping already stores/maintains
   it).  Returns the row count, or -1 if the column is absent / on error. */
static int pg_index_create(mvx_file *fh, const char *item, const char *col,
                           int64_t attr) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qi = pg_index_ident(f, item);
    /* A mapped identity column indexes directly; any other field indexes the
       expression the blob push-down uses, so it too becomes an index scan. */
    char target[400];
    if (col && col[0]) {
        char *qc = PQescapeIdentifier(f->conn, col, strlen(col));
        snprintf(target, sizeof target, "(%s)", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    } else {
        char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
        snprintf(target, sizeof target, "(%s.mvx_attr(rec,%lld))",
                 qs ? qs : "\"\"", (long long)attr);
        if (qs) PQfreemem(qs);
    }
    char sql[1200];
    snprintf(sql, sizeof sql, "CREATE INDEX IF NOT EXISTS %s ON %s %s",
             qi ? qi : "\"\"", qt, target);
    PGresult *r = PQexec(f->conn, sql);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    if (qi) PQfreemem(qi);
    if (!ok) return -1;
    char csql[560];
    snprintf(csql, sizeof csql, "SELECT count(*) FROM %s", qt);
    PGresult *cr = PQexec(f->conn, csql);
    int n = (cr && PQresultStatus(cr) == PGRES_TUPLES_OK && PQntuples(cr))
                ? atoi(PQgetvalue(cr, 0, 0))
                : 0;
    if (cr) PQclear(cr);
    return n;
}

/* Equality lookup on a mapped column, using its SQL index: the ids whose
   column equals key, snapshotted into a cursor (as pg_select_begin does). */
static mvx_cursor *pg_index_select(mvx_file *fh, const char *item,
                                   const char *key, int64_t klen) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qc = PQescapeIdentifier(f->conn, item, strlen(item));
    char sql[700];
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s = $1", qt,
             qc ? qc : "\"\"");
    if (qc) PQfreemem(qc);
    const char *pv[1] = {key};
    int pl[1] = {(int)klen}, pf[1] = {0};   /* text param -> column type */
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres index select");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres index select");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

static int pg_index_drop(mvx_file *fh, const char *item) {
    pg_file *f = (pg_file *)fh;
    char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
    char *qi = pg_index_ident(f, item);
    char sql[700];
    snprintf(sql, sizeof sql, "DROP INDEX IF EXISTS %s.%s",
             qs ? qs : "\"\"", qi ? qi : "\"\"");
    if (qs) PQfreemem(qs);
    if (qi) PQfreemem(qi);
    PGresult *r = PQexec(f->conn, sql);
    int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
    if (r) PQclear(r);
    return ok;
}

/* Server-side WITH push-down: ids whose column satisfies op/val, filtered in
   the backend.  "=" -> equality; "#" -> IS DISTINCT FROM (so NULL/empty rows
   count as not-equal, matching MV).  Snapshots ids into a cursor. */
static mvx_cursor *pg_select_where(mvx_file *fh, const char *col,
                                   const char *op, const char *val,
                                   int64_t vlen) {
    pg_file *f = (pg_file *)fh;
    const char *sqlop;
    if (strcmp(op, "=") == 0) sqlop = "=";
    else if (strcmp(op, "#") == 0) sqlop = "IS DISTINCT FROM";
    else return NULL;                     /* op not pushable -> caller scans */
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qc = PQescapeIdentifier(f->conn, col, strlen(col));
    char sql[760];
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s %s $1", qt,
             qc ? qc : "\"\"", sqlop);
    if (qc) PQfreemem(qc);
    const char *pv[1] = {val};
    int pl[1] = {(int)vlen}, pf[1] = {0};   /* text param -> column type */
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select_where");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres select_where");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

/* Server-side WITH push-down straight on the record blob: attribute `attr`
   is split_part(convert_from(rec,'LATIN1'), chr(254), attr) — the raw field
   between the (attr-1)th and attr'th field mark (byte 0xFE).  Comparing the
   raw attribute to the raw value is exact for any field type and needs no
   mapped column, but cannot use a column index. */
static mvx_cursor *pg_select_attr(mvx_file *fh, int64_t attr, const char *op,
                                  const char *val, int64_t vlen) {
    pg_file *f = (pg_file *)fh;
    const char *sqlop;
    int isrange = 0;
    if (strcmp(op, "=") == 0) sqlop = "=";
    else if (strcmp(op, "#") == 0) sqlop = "<>";   /* split_part never NULL */
    else if (strcmp(op, ">") == 0 || strcmp(op, "<") == 0 ||
             strcmp(op, ">=") == 0 || strcmp(op, "<=") == 0) {
        sqlop = op; isrange = 1;           /* numeric range, caller gated */
    } else return NULL;
    if (attr < 1) return NULL;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
    char sql[900];
    if (isrange)
        /* compare the raw internal value numerically, matching MV's numeric
           compare; an empty attribute becomes NULL and drops out. */
        snprintf(sql, sizeof sql,
                 "SELECT id FROM %s WHERE "
                 "NULLIF(%s.mvx_attr(rec,%lld),'')::numeric %s $1::numeric",
                 qt, qs ? qs : "\"\"", (long long)attr, sqlop);
    else
        snprintf(sql, sizeof sql,
                 "SELECT id FROM %s WHERE %s.mvx_attr(rec,%lld) %s $1",
                 qt, qs ? qs : "\"\"", (long long)attr, sqlop);
    if (qs) PQfreemem(qs);
    const char *pv[1] = {val};
    int pl[1] = {(int)vlen}, pf[1] = {0};   /* text value */
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, pl, pf, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select_attr");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres select_attr");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

/* Same Postgres database? (host+port+dbname) — a JOIN can only span tables
   reachable from one connection. */
static int pg_same_db(PGconn *a, PGconn *b) {
    const char *ha = PQhost(a), *hb = PQhost(b);
    const char *pa = PQport(a), *pb = PQport(b);
    const char *da = PQdb(a), *db = PQdb(b);
    return ha && hb && strcmp(ha, hb) == 0 &&
           pa && pb && strcmp(pa, pb) == 0 &&
           da && db && strcmp(da, db) == 0;
}

/* Co-located TRANS() JOIN: source ids whose foreign key (src attribute
   `sk`) points at a target row whose attribute `ta` equals val.  The whole
   filter runs in one JOIN on the source connection (the target table is in
   the same database), so only matching ids come back. */
static mvx_cursor *pg_select_join(mvx_file *srch, int64_t sk,
                                  const char *src_keycol, mvx_file *tgth,
                                  int64_t ta, const char *tgt_col,
                                  const char *op, const char *val,
                                  int64_t vlen) {
    if (strcmp(op, "=") != 0) return NULL;   /* only equality for now */
    pg_file *s = (pg_file *)srch, *t = (pg_file *)tgth;
    if (!pg_same_db(s->conn, t->conn)) return NULL;   /* not joinable */
    char sqt[512], tqt[512];
    qualify(s->conn, s->schema, s->table, sqt, sizeof sqt);
    qualify(s->conn, t->schema, t->table, tqt, sizeof tqt);
    /* Prefer a mapped identity column over the blob split_part: it can use an
       index and, in native mode, is the authoritative value. */
    char skexpr[320], taexpr[320];
    if (src_keycol && src_keycol[0]) {
        char *qc = PQescapeIdentifier(s->conn, src_keycol, strlen(src_keycol));
        snprintf(skexpr, sizeof skexpr, "s.%s", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    } else {
        char *qs = PQescapeIdentifier(s->conn, s->schema, strlen(s->schema));
        snprintf(skexpr, sizeof skexpr, "%s.mvx_attr(s.rec,%lld)",
                 qs ? qs : "\"\"", (long long)sk);
        if (qs) PQfreemem(qs);
    }
    if (tgt_col && tgt_col[0]) {
        char *qc = PQescapeIdentifier(t->conn, tgt_col, strlen(tgt_col));
        snprintf(taexpr, sizeof taexpr, "t.%s", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    } else {
        char *qs = PQescapeIdentifier(t->conn, t->schema, strlen(t->schema));
        snprintf(taexpr, sizeof taexpr, "%s.mvx_attr(t.rec,%lld)",
                 qs ? qs : "\"\"", (long long)ta);
        if (qs) PQfreemem(qs);
    }
    /* The source key may be multivalued (@VM-separated); classic TRANS maps
       element-wise, so a record matches when ANY of its key values points at a
       target row satisfying the filter.  Split the key on @VM (chr 253) and
       match any element — a single-valued key is a one-element array, so this
       is identical for the scalar case.  DISTINCT collapses a source record
       that matches through more than one of its key values. */
    char sql[1600];
    snprintf(sql, sizeof sql,
             "SELECT DISTINCT s.id FROM %s s JOIN %s t "
             "ON convert_from(t.id,'LATIN1') = "
             "ANY(string_to_array(%s, chr(253))) WHERE %s = $1",
             sqt, tqt, skexpr, taexpr);
    const char *pv[1] = {val};
    int pl[1] = {(int)vlen}, pf[1] = {0};
    PGresult *r = PQexecParams(s->conn, sql, 1, NULL, pv, pl, pf, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select_join");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres select_join");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

/* Co-located TRANS() ORDER BY: the source ids ordered by the value TRANS()
   looks up.  Reproduces mvx_trans() exactly — the (possibly multivalued)
   source key is unnested on @VM (chr 253), each element left-joined to its
   target row, and the target attribute for each is re-joined with @VM in key
   order (a miss -> "" for control 'X', the key element for 'C').  Ordered as
   text (COLLATE "C") to match MV's byte sort, with s.id breaking ties so a
   top-N is deterministic, LIMIT applied server-side. */
static mvx_cursor *pg_select_join_order(mvx_file *srch, int64_t sk,
                                        const char *src_keycol, mvx_file *tgth,
                                        int64_t ta, const char *tgt_col,
                                        char ctl, int otext, int64_t limit) {
    pg_file *s = (pg_file *)srch, *t = (pg_file *)tgth;
    if (!pg_same_db(s->conn, t->conn)) return NULL;   /* not joinable */
    char sqt[512], tqt[512];
    qualify(s->conn, s->schema, s->table, sqt, sizeof sqt);
    qualify(s->conn, t->schema, t->table, tqt, sizeof tqt);
    char skexpr[320], taexpr[320];
    if (src_keycol && src_keycol[0]) {
        char *qc = PQescapeIdentifier(s->conn, src_keycol, strlen(src_keycol));
        snprintf(skexpr, sizeof skexpr, "s.%s", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    } else {
        char *qs = PQescapeIdentifier(s->conn, s->schema, strlen(s->schema));
        snprintf(skexpr, sizeof skexpr, "%s.mvx_attr(s.rec,%lld)",
                 qs ? qs : "\"\"", (long long)sk);
        if (qs) PQfreemem(qs);
    }
    if (tgt_col && tgt_col[0]) {
        char *qc = PQescapeIdentifier(t->conn, tgt_col, strlen(tgt_col));
        snprintf(taexpr, sizeof taexpr, "t.%s", qc ? qc : "\"\"");
        if (qc) PQfreemem(qc);
    } else {
        char *qs = PQescapeIdentifier(t->conn, t->schema, strlen(t->schema));
        snprintf(taexpr, sizeof taexpr, "%s.mvx_attr(t.rec,%lld)",
                 qs ? qs : "\"\"", (long long)ta);
        if (qs) PQfreemem(qs);
    }
    const char *missexpr = (ctl == 'C') ? "k.kv" : "''";
    const char *coll = otext ? " COLLATE \"C\"" : "";
    char limbuf[32] = "";
    if (limit > 0) snprintf(limbuf, sizeof limbuf, " LIMIT %lld", (long long)limit);
    char sql[2000];
    snprintf(sql, sizeof sql,
             "SELECT s.id FROM %s s ORDER BY COALESCE(("
             "SELECT string_agg(CASE WHEN t.id IS NOT NULL THEN %s ELSE %s END,"
             " chr(253) ORDER BY k.ord) "
             "FROM unnest(string_to_array(%s, chr(253))) "
             "WITH ORDINALITY AS k(kv, ord) "
             "LEFT JOIN %s t ON convert_from(t.id,'LATIN1') = k.kv), '')%s, s.id%s",
             sqt, taexpr, missexpr, skexpr, tqt, coll, limbuf);
    PGresult *r = PQexecParams(s->conn, sql, 0, NULL, NULL, NULL, NULL, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select_join_order");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres select_join_order");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

/* Server-side COUNT: count(*) with no filter, or filtered by a mapped column
   or the raw blob attribute — one row back instead of a stream of ids. */
static int64_t pg_count_where(mvx_file *fh, const char *col, int64_t attr,
                              const char *op, const char *val, int64_t vlen) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[900];
    const char *pv[1] = {val};
    int pl[1] = {(int)vlen}, pf[1] = {0};
    int nparam = 0;
    if (!op || !op[0]) {
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", qt);
    } else {
        const char *sqlop;
        if (op[0] == '=' && !op[1]) sqlop = "=";
        else if (op[0] == '#' && !op[1]) sqlop = col ? "IS DISTINCT FROM" : "<>";
        else return -1;
        char expr[400];
        if (col && col[0]) {
            char *qc = PQescapeIdentifier(f->conn, col, strlen(col));
            snprintf(expr, sizeof expr, "%s", qc ? qc : "\"\"");
            if (qc) PQfreemem(qc);
        } else {
            char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
            snprintf(expr, sizeof expr, "%s.mvx_attr(rec,%lld)",
                     qs ? qs : "\"\"", (long long)attr);
            if (qs) PQfreemem(qs);
        }
        snprintf(sql, sizeof sql, "SELECT count(*) FROM %s WHERE %s %s $1",
                 qt, expr, sqlop);
        nparam = 1;
    }
    PGresult *r = PQexecParams(f->conn, sql, nparam, NULL, pv, pl, pf, 0);
    int64_t n = -1;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1)
        n = strtoll(PQgetvalue(r, 0, 0), NULL, 10);
    if (r) PQclear(r);
    return n;
}

/* Server-side SUM of a numeric column, optionally filtered — one value back.
   The mapped NUMERIC column holds the display value, so this is the total a
   report would show. */
static int pg_sum_where(mvx_file *fh, const char *sumcol, const char *fcol,
                        int64_t fattr, const char *fop, const char *fval,
                        int64_t fvlen, char *out, size_t cap) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qsum = PQescapeIdentifier(f->conn, sumcol, strlen(sumcol));
    char sql[1000];
    size_t p = (size_t)snprintf(sql, sizeof sql,
                                "SELECT COALESCE(sum(%s),0)::text FROM %s",
                                qsum ? qsum : "\"\"", qt);
    if (qsum) PQfreemem(qsum);
    const char *pv[1] = {fval};
    int pl[1] = {(int)fvlen}, pf[1] = {0};
    int nparam = 0;
    if (fop && fop[0]) {
        const char *sqlop;
        if (fop[0] == '=' && !fop[1]) sqlop = "=";
        else if (fop[0] == '#' && !fop[1]) sqlop = fcol ? "IS DISTINCT FROM" : "<>";
        else return 0;
        char expr[400];
        if (fcol && fcol[0]) {
            char *qc = PQescapeIdentifier(f->conn, fcol, strlen(fcol));
            snprintf(expr, sizeof expr, "%s", qc ? qc : "\"\"");
            if (qc) PQfreemem(qc);
        } else {
            char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
            snprintf(expr, sizeof expr, "%s.mvx_attr(rec,%lld)",
                     qs ? qs : "\"\"", (long long)fattr);
            if (qs) PQfreemem(qs);
        }
        snprintf(sql + p, sizeof sql - p, " WHERE %s %s $1", expr, sqlop);
        nparam = 1;
    }
    PGresult *r = PQexecParams(f->conn, sql, nparam, NULL, pv, pl, pf, 0);
    int ok = 0;
    if (r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1) {
        snprintf(out, cap, "%s", PQgetvalue(r, 0, 0));
        ok = 1;
    }
    if (r) PQclear(r);
    return ok;
}

/* ORDER BY / LIMIT push-down (optionally filtered): the ids ordered by a
   mapped column — COLLATE "C" for text to match MV's byte sort — and limited
   server-side, so a top-N fetches N ids instead of the whole file. */
static mvx_cursor *pg_select_order(mvx_file *fh, const char *fcol,
                                   int64_t fattr, const char *fop,
                                   const char *fval, int64_t fvlen,
                                   const char *ocol, int otext,
                                   int64_t limit) {
    pg_file *f = (pg_file *)fh;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char *qo = PQescapeIdentifier(f->conn, ocol, strlen(ocol));
    char sql[1200];
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    const char *pv[1] = {fval};
    int pl[1] = {(int)fvlen}, pf[1] = {0};
    int nparam = 0;
    if (fop && fop[0]) {
        const char *sqlop;
        if (fop[0] == '=' && !fop[1]) sqlop = "=";
        else if (fop[0] == '#' && !fop[1]) sqlop = fcol ? "IS DISTINCT FROM" : "<>";
        else { if (qo) PQfreemem(qo); return NULL; }
        char expr[400];
        if (fcol && fcol[0]) {
            char *qc = PQescapeIdentifier(f->conn, fcol, strlen(fcol));
            snprintf(expr, sizeof expr, "%s", qc ? qc : "\"\"");
            if (qc) PQfreemem(qc);
        } else {
            char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
            snprintf(expr, sizeof expr, "%s.mvx_attr(rec,%lld)",
                     qs ? qs : "\"\"", (long long)fattr);
            if (qs) PQfreemem(qs);
        }
        p += (size_t)snprintf(sql + p, sizeof sql - p, " WHERE %s %s $1",
                              expr, sqlop);
        nparam = 1;
    }
    p += (size_t)snprintf(sql + p, sizeof sql - p, " ORDER BY %s%s",
                          qo ? qo : "\"\"", otext ? " COLLATE \"C\"" : "");
    if (limit > 0)
        snprintf(sql + p, sizeof sql - p, " LIMIT %lld", (long long)limit);
    if (qo) PQfreemem(qo);
    PGresult *r = PQexecParams(f->conn, sql, nparam, NULL, pv, pl, pf, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select_order");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres select_order");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

/* Multi-condition WITH: SELECT id WHERE p1 AND p2 AND ... — each predicate a
   column or the mvx_attr blob expression, numeric for a range. */
static mvx_cursor *pg_select_multi(mvx_file *fh, const mvx_pred *preds,
                                   int npred) {
    pg_file *f = (pg_file *)fh;
    if (npred < 1 || npred > 32) return NULL;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[4000];
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE ", qt);
    const char *pv[32];
    int pl[32], pf[32];
    for (int i = 0; i < npred; i++) {
        const mvx_pred *q = &preds[i];
        const char *sqlop;
        if (strcmp(q->op, "=") == 0) sqlop = "=";
        else if (strcmp(q->op, "#") == 0) sqlop = q->col ? "IS DISTINCT FROM" : "<>";
        else if (strcmp(q->op, ">") == 0 || strcmp(q->op, "<") == 0 ||
                 strcmp(q->op, ">=") == 0 || strcmp(q->op, "<=") == 0)
            sqlop = q->op;
        else return NULL;
        char expr[400];
        if (q->col && q->col[0]) {
            char *qc = PQescapeIdentifier(f->conn, q->col, strlen(q->col));
            snprintf(expr, sizeof expr, "%s", qc ? qc : "\"\"");
            if (qc) PQfreemem(qc);
        } else {
            char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
            if (q->numeric)
                snprintf(expr, sizeof expr, "NULLIF(%s.mvx_attr(rec,%lld),'')::numeric",
                         qs ? qs : "\"\"", (long long)q->attr);
            else
                snprintf(expr, sizeof expr, "%s.mvx_attr(rec,%lld)",
                         qs ? qs : "\"\"", (long long)q->attr);
            if (qs) PQfreemem(qs);
        }
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s %s $%d%s",
                              i ? " AND " : "", expr, sqlop, i + 1,
                              q->numeric ? "::numeric" : "");
        pv[i] = q->val;
        pl[i] = (int)q->vlen;
        pf[i] = 0;
    }
    PGresult *r = PQexecParams(f->conn, sql, npred, NULL, pv, pl, pf, 1);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) PQclear(r);
        return NULL;
    }
    mvx_cursor *cur = calloc(1, sizeof(mvx_cursor));
    if (!cur) mvx_fatal("out of memory in postgres select_multi");
    int n = PQntuples(r);
    cur->ids = calloc(n ? n : 1, sizeof(mv_value));
    if (!cur->ids) mvx_fatal("out of memory in postgres select_multi");
    for (int i = 0; i < n; i++) {
        mv_init(&cur->ids[cur->n]);
        mv_set_str(&cur->ids[cur->n], PQgetvalue(r, i, 0),
                   PQgetlength(r, i, 0));
        cur->n++;
    }
    PQclear(r);
    return cur;
}

/* DESCRIBE support: render the SQL that select_multi / select_order would run,
   without executing it, for the verbs' DESCRIBE modifier.  Literals are inlined
   (escaped) rather than shown as $N placeholders so the plan reads as runnable
   SQL.  Same predicate shapes as pg_select_multi plus the ORDER BY / LIMIT tail
   from pg_select_order. */
static int pg_explain(mvx_file *fh, const mvx_pred *preds, int npred,
                      const char *ocol, int otext, int64_t limit,
                      char *out, size_t cap) {
    pg_file *f = (pg_file *)fh;
    if (npred < 0 || npred > 32) return 0;
    char qt[512];
    qualify(f->conn, f->schema, f->table, qt, sizeof qt);
    char sql[4000];
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    for (int i = 0; i < npred; i++) {
        const mvx_pred *q = &preds[i];
        const char *sqlop;
        if (strcmp(q->op, "=") == 0) sqlop = "=";
        else if (strcmp(q->op, "#") == 0) sqlop = q->col ? "IS DISTINCT FROM" : "<>";
        else if (strcmp(q->op, ">") == 0 || strcmp(q->op, "<") == 0 ||
                 strcmp(q->op, ">=") == 0 || strcmp(q->op, "<=") == 0)
            sqlop = q->op;
        else return 0;
        char expr[400];
        if (q->col && q->col[0]) {
            char *qc = PQescapeIdentifier(f->conn, q->col, strlen(q->col));
            snprintf(expr, sizeof expr, "%s", qc ? qc : "\"\"");
            if (qc) PQfreemem(qc);
        } else {
            char *qs = PQescapeIdentifier(f->conn, f->schema, strlen(f->schema));
            if (q->numeric)
                snprintf(expr, sizeof expr, "NULLIF(%s.mvx_attr(rec,%lld),'')::numeric",
                         qs ? qs : "\"\"", (long long)q->attr);
            else
                snprintf(expr, sizeof expr, "%s.mvx_attr(rec,%lld)",
                         qs ? qs : "\"\"", (long long)q->attr);
            if (qs) PQfreemem(qs);
        }
        char *lit = PQescapeLiteral(f->conn, q->val, (size_t)q->vlen);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s %s %s%s",
                              i ? " AND " : " WHERE ", expr, sqlop,
                              lit ? lit : "''", q->numeric ? "::numeric" : "");
        if (lit) PQfreemem(lit);
    }
    if (ocol && ocol[0]) {
        char *qo = PQescapeIdentifier(f->conn, ocol, strlen(ocol));
        p += (size_t)snprintf(sql + p, sizeof sql - p, " ORDER BY %s%s",
                              qo ? qo : "\"\"", otext ? " COLLATE \"C\"" : "");
        if (qo) PQfreemem(qo);
    }
    if (limit > 0)
        snprintf(sql + p, sizeof sql - p, " LIMIT %lld", (long long)limit);
    snprintf(out, cap, "%s", sql);
    return 1;
}

/* Cross-process record locks (#16) via Postgres session-level advisory locks.
   The runtime's process-local lock table only arbitrates within one process;
   two MVX processes on the same PG-backed file need the backend to arbitrate.
   Connections are pooled one per location, so every file on a location shares
   one PGconn == one PG session: an advisory lock taken here is held for this
   process and seen as held by every other process (each its own session).

   The key is a 64-bit FNV-1a hash of the file spec + record id — the same
   composition the runtime's lock_key uses — so any session opening the same
   file hashes an id to the same advisory key.  pg_try_advisory_lock is
   non-blocking (returns whether it got it); the runtime does the blocking
   READU poll and the LOCKED single-try itself. */
static int64_t pg_advkey(mvx_file *fh, const char *id, int64_t idlen) {
    pg_file *f = (pg_file *)fh;
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = f->base.spec; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    h ^= 0x01;                            /* the lock_key spec/id separator */
    h *= 1099511628211ULL;
    for (int64_t i = 0; i < idlen; i++) {
        h ^= (unsigned char)id[i];
        h *= 1099511628211ULL;
    }
    return (int64_t)h;                    /* advisory keys are signed int8 */
}

static int pg_advisory(mvx_file *fh, const char *id, int64_t idlen,
                       const char *fn) {
    pg_file *f = (pg_file *)fh;
    char keytxt[24];
    snprintf(keytxt, sizeof keytxt, "%lld",
             (long long)pg_advkey(fh, id, idlen));
    char sql[48];
    snprintf(sql, sizeof sql, "SELECT %s($1)", fn);
    const char *pv[1] = {keytxt};
    PGresult *r = PQexecParams(f->conn, sql, 1, NULL, pv, NULL, NULL, 0);
    int ok = r && PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) == 1 &&
             PQgetvalue(r, 0, 0)[0] == 't';
    if (r) PQclear(r);
    return ok;
}

/* 1 = acquired, 0 = another session holds it (runtime retries or reports). */
static int pg_lock(mvx_file *fh, const char *id, int64_t idlen) {
    return pg_advisory(fh, id, idlen, "pg_try_advisory_lock");
}
static int pg_unlock(mvx_file *fh, const char *id, int64_t idlen) {
    return pg_advisory(fh, id, idlen, "pg_advisory_unlock");
}

/* Enumerate the account's files (#17): every table in the schema with a `rec`
   column is an MVX record table — the record files and their DICT.<file>
   dictionaries.  Association child tables (<file>_<assoc>) have (id, seq, …) and
   no `rec` column, so they are excluded here; the runtime drops the DICT.*
   entries.  Backs LISTF for a whole-account Postgres binding. */
static int pg_names(const char *loc, mv_value *out, char *err, size_t errlen) {
    char schema[128];
    PGconn *c = pg_connect(loc, schema, sizeof schema, err, errlen);
    if (!c) return 0;
    const char *pv[1] = {schema};
    PGresult *r = PQexecParams(c,
        "SELECT table_name FROM information_schema.columns "
        "WHERE table_schema=$1 AND column_name='rec' ORDER BY table_name",
        1, NULL, pv, NULL, NULL, 0);
    if (!r || PQresultStatus(r) != PGRES_TUPLES_OK) {
        if (r) { snprintf(err, errlen, "postgres: %s", PQerrorMessage(c));
                 PQclear(r); }
        return 0;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int n = PQntuples(r);
    for (int i = 0; i < n; i++) {
        const char *nm = PQgetvalue(r, i, 0);
        size_t nl = (size_t)PQgetlength(r, i, 0);
        if (len + nl + 1 > cap) {
            cap = cap ? cap * 2 : 256;
            while (cap < len + nl + 1) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) mvx_fatal("out of memory in postgres names");
            buf = nb;
        }
        if (len) buf[len++] = (char)0xFE;
        memcpy(buf + len, nm, nl);
        len += nl;
    }
    PQclear(r);
    mv_set_str(out, buf ? buf : "", (int64_t)len);
    free(buf);
    return 1;
}

static const mvx_driver mvx_driver_postgres = {
    "postgres",
    pg_open, pg_close,
    pg_read, pg_write, pg_del,
    pg_select_begin, pg_select_next, pg_select_end,
    pg_create, pg_remove,
    pg_names,                             /* whole-account LISTF */
    NULL, NULL,                           /* no MVX-maintained index writes */
    pg_index_select, pg_index_drop,       /* native SQL indexes on columns */
    pg_lock, pg_unlock,                   /* cross-process advisory locks */
    pg_map_ensure, pg_map_apply,          /* relational mapping: parent */
    pg_map_child_ensure, pg_map_child_apply,   /* association child tables */
    pg_map_drop,                          /* tear down a mapping */
    pg_map_read, pg_map_child_read,       /* native read-back */
    pg_index_create,                      /* CREATE INDEX on a mapped column */
    pg_select_where,                      /* WITH push-down on a column */
    pg_select_attr,                       /* WITH push-down on the blob */
    pg_select_join,                       /* co-located TRANS() JOIN */
    pg_count_where,                       /* server-side COUNT */
    pg_sum_where,                         /* server-side SUM */
    pg_select_order,                      /* ORDER BY / LIMIT push-down */
    pg_select_multi,                      /* multi-condition WITH (AND) */
    pg_explain,                           /* DESCRIBE: render SQL, don't run */
    pg_select_count,                      /* backfill progress total */
    pg_bulk_begin, pg_bulk_commit,        /* transactional backfill batching */
    pg_map_backfill,                      /* whole-mapping backfill push-down */
    pg_select_join_order,                 /* co-located TRANS() ORDER BY */
    pg_rollback,                          /* abort a failed logical write */
};

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_postgres : NULL;
}
