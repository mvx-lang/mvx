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

/* SQLite storage driver.
 *
 * ONE DATABASE FILE HOLDS THE WHOLE ACCOUNT: every MV file is a table in
 * it.  That is the point of this backend rather than an implementation
 * detail — it is what makes the cross-file work cheap.  postgres has to
 * ask whether two files share a database before it dares a JOIN
 * (pg_same_db); here they always do, so TRANS() push-down, cross-file
 * ordering and LISTF are available unconditionally and need no server.
 *
 * Between the dir driver (no query capability at all) and postgres (a
 * server to run), this is the backend that does real listing, filtering
 * and sorting with nothing to administer.
 *
 * Spec: "<dbpath>\n<table>" — the location is the database file, the
 * rest of the spec is the table.  Both arrive from the account's
 * BINDINGS record, opaque to the runtime (ARCHITECTURE.md 4.4).
 *
 * SORTING NEEDS THE MAPPING.  mvx_orderselect only pushes ORDER BY when
 * the order field resolves to a mapped column (map_order_col), so the
 * relational-mapping capability below is not an optional extra here —
 * it is what makes sorting server-side at all.
 */

#include "mvx_driver.h"
#include "mvx_runtime.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNS 8
static struct {
    char path[1024];
    sqlite3 *db;
} g_conns[MAX_CONNS];
static int g_nconns;

struct mvx_cursor {
    mv_value *ids;
    int64_t n, pos;
};

typedef struct {
    mvx_file_base base;
    sqlite3 *db;
    char table[256];
} sq_file;

static const mvx_driver mvx_driver_sqlite;

/* ------------------------------------------------------------ helpers */

/* "loc\nspec" — the location (database path) is everything up to the
   first newline.  Same shape the other bound drivers use. */
static const char *split_spec(const char *spec, char *loc, size_t cap) {
    const char *nl = strchr(spec, '\n');
    if (!nl) { loc[0] = '\0'; return spec; }
    size_t n = (size_t)(nl - spec);
    if (n >= cap) n = cap - 1;
    memcpy(loc, spec, n);
    loc[n] = '\0';
    return nl + 1;
}

/* Quote an SQL identifier: wrap in double quotes, doubling any within.
   SQLite has no PQescapeIdentifier, and MV file names legitimately carry
   dots and dashes, so every identifier goes through here. */
static void quote_ident(const char *s, char *out, size_t cap) {
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = '\0'; return; }
    out[o++] = '"';
    for (const char *p = s; *p && o + 3 < cap; p++) {
        if (*p == '"') { out[o++] = '"'; if (o + 2 >= cap) break; }
        out[o++] = *p;
    }
    out[o++] = '"';
    out[o] = '\0';
}

/* mvx_attr(rec, n) — the nth @AM-delimited attribute of a record, as
   text.  Registered as a C function rather than built out of SQL string
   functions: it is exact on bytes, it is fast, and being DETERMINISTIC
   it can be used in an expression index, which is what makes a WITH on
   an unmapped attribute index-eligible.  Attribute n is the field
   between the (n-1)th and nth mark, 1-based, matching MV. */
static void fn_mvx_attr(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 2) { sqlite3_result_null(ctx); return; }
    const unsigned char *rec = sqlite3_value_blob(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    int want = sqlite3_value_int(argv[1]);
    if (!rec || want < 1) { sqlite3_result_text(ctx, "", 0, SQLITE_STATIC); return; }
    int field = 1, start = 0;
    for (int i = 0; i <= len; i++) {
        int mark = (i == len) || rec[i] == (unsigned char)0xFE;   /* @AM */
        if (!mark) continue;
        if (field == want) {
            sqlite3_result_text(ctx, (const char *)rec + start, i - start,
                                SQLITE_TRANSIENT);
            return;
        }
        field++;
        start = i + 1;
    }
    sqlite3_result_text(ctx, "", 0, SQLITE_STATIC);   /* past the end */
}

/* mvx_vm_has(keys, id) — 1 when `id` equals one of the @VM-separated
   elements of `keys`.  Classic TRANS maps element-wise over a multivalued
   key, so a source record matches when ANY of its key values points at the
   target.  postgres spells this string_to_array(...) = ANY(...); SQLite has
   no array type, and doing it with instr() would match substrings — "10"
   inside "100" — so it is an exact byte comparison here instead. */
static void fn_mvx_vm_has(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc != 2) { sqlite3_result_int(ctx, 0); return; }
    const unsigned char *keys = sqlite3_value_blob(argv[0]);
    int klen = sqlite3_value_bytes(argv[0]);
    const unsigned char *id = sqlite3_value_blob(argv[1]);
    int ilen = sqlite3_value_bytes(argv[1]);
    if (!keys || !id) { sqlite3_result_int(ctx, 0); return; }
    int start = 0;
    for (int i = 0; i <= klen; i++) {
        if (i != klen && keys[i] != (unsigned char)0xFD) continue;   /* @VM */
        int len = i - start;
        if (len == ilen && memcmp(keys + start, id, (size_t)ilen) == 0) {
            sqlite3_result_int(ctx, 1);
            return;
        }
        start = i + 1;
    }
    sqlite3_result_int(ctx, 0);
}

/* Open (once per database file) and cache.  Every table in this file is
   an MV file of the same account, which is what keeps joins co-located. */
static sqlite3 *sq_connect(const char *path, char *err, size_t errlen) {
    if (!path || !path[0]) {
        snprintf(err, errlen, "sqlite: no database path in the binding");
        return NULL;
    }
    for (int i = 0; i < g_nconns; i++)
        if (strcmp(g_conns[i].path, path) == 0) return g_conns[i].db;
    if (g_nconns == MAX_CONNS) {
        snprintf(err, errlen, "sqlite: too many open databases (max %d)",
                 MAX_CONNS);
        return NULL;
    }
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        snprintf(err, errlen, "sqlite: %s", db ? sqlite3_errmsg(db) : "open failed");
        if (db) sqlite3_close(db);
        return NULL;
    }
    /* WAL so a reader does not block the writer: MV sessions read while
       another commits constantly.  busy_timeout rather than an immediate
       SQLITE_BUSY, because a lock here is contention, not an error. */
    sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
    sqlite3_busy_timeout(db, 5000);
    sqlite3_create_function(db, "mvx_attr", 2,
                            SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            fn_mvx_attr, NULL, NULL);
    sqlite3_create_function(db, "mvx_vm_has", 2,
                            SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL,
                            fn_mvx_vm_has, NULL, NULL);
    snprintf(g_conns[g_nconns].path, sizeof g_conns[0].path, "%s", path);
    g_conns[g_nconns].db = db;
    g_nconns++;
    return db;
}

/* Does this table exist?  open() must not create — creation is explicit. */
static int table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

/* Run one statement, no results wanted. */
static int exec_sql(sqlite3 *db, const char *sql) {
    char *msg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &msg);
    if (msg) sqlite3_free(msg);
    return rc == SQLITE_OK;
}

/* Atomicity for an operation the DRIVER owns end to end.
 *
 * map_child_apply is a DELETE followed by one INSERT per row: interrupted
 * halfway it leaves an association with some of its values and not the rest,
 * which is worse than either extreme because nothing reports it.  These wrap
 * such a sequence.
 *
 * begin() returns whether it actually started a transaction -- inside a
 * backfill one is already open, and then the outer one carries the atomicity
 * and only its owner may commit.  end() rolls back on failure, which is the
 * part a plain commit-or-nothing cannot express. */
static int txn_begin(sqlite3 *db) {
    if (!sqlite3_get_autocommit(db)) return 0;   /* already inside one */
    return exec_sql(db, "BEGIN") ? 1 : 0;
}
static void txn_end(sqlite3 *db, int started, int ok) {
    if (!started) return;
    exec_sql(db, ok ? "COMMIT" : "ROLLBACK");
}

/* Collect an id column into a cursor.  Every push-down below ends here,
   so they all share one snapshot-then-stream shape. */
static mvx_cursor *cursor_from(sqlite3_stmt *st) {
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in sqlite cursor");
    int64_t cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (c->n == cap) {
            cap = cap ? cap * 2 : 64;
            mv_value *g = realloc(c->ids, (size_t)cap * sizeof(mv_value));
            if (!g) mvx_fatal("out of memory in sqlite cursor");
            c->ids = g;
        }
        const void *b = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        mv_init(&c->ids[c->n]);
        mv_set_str(&c->ids[c->n], (const char *)b, n);
        c->n++;
    }
    return c;
}

/* ---------------------------------------------------- record operations */

static mvx_file *sq_open(const char *spec, char *err, size_t errlen) {
    char path[1024];
    const char *tbl = split_spec(spec, path, sizeof path);
    sqlite3 *db = sq_connect(path, err, errlen);
    if (!db) return NULL;
    if (!table_exists(db, tbl)) return NULL;   /* not found: normal ELSE path */
    sq_file *f = calloc(1, sizeof(sq_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_sqlite;
    f->base.spec = strdup(spec);
    f->db = db;
    snprintf(f->table, sizeof f->table, "%s", tbl);
    return (mvx_file *)f;
}

static void sq_close(mvx_file *fh) {
    sq_file *f = (sq_file *)fh;
    free(f->base.spec);
    free(f);                                  /* the sqlite3 * is pooled */
}

static int sq_read(mvx_file *fh, const char *id, int64_t idlen, mv_value *rec) {
    sq_file *f = (sq_file *)fh;
    char qt[300], sql[400];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "SELECT rec FROM %s WHERE id = ?1", qt);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_blob(st, 1, id, (int)idlen, SQLITE_STATIC);
    int got = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *b = sqlite3_column_blob(st, 0);
        mv_set_str(rec, (const char *)b, sqlite3_column_bytes(st, 0));
        got = 1;
    }
    sqlite3_finalize(st);
    return got;
}

static int sq_write(mvx_file *fh, const char *id, int64_t idlen,
                    const mv_value *rec) {
    sq_file *f = (sq_file *)fh;
    char qt[300], sql[500];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (id, rec) VALUES (?1, ?2) "
             "ON CONFLICT(id) DO UPDATE SET rec = excluded.rec", qt);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    char buf[256];
    const char *rp;
    int64_t rl = mv_val_chars((mv_value *)rec, buf, sizeof buf, &rp);
    sqlite3_bind_blob(st, 1, id, (int)idlen, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, rp, (int)rl, SQLITE_STATIC);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

static int sq_del(mvx_file *fh, const char *id, int64_t idlen) {
    sq_file *f = (sq_file *)fh;
    char qt[300], sql[400];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "DELETE FROM %s WHERE id = ?1", qt);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_blob(st, 1, id, (int)idlen, SQLITE_STATIC);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    int n = sqlite3_changes(f->db);
    sqlite3_finalize(st);
    return ok && n > 0;
}

/* -------------------------------------------------------------- select */

static mvx_cursor *sq_select_begin(mvx_file *fh) {
    sq_file *f = (sq_file *)fh;
    char qt[300], sql[400];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    mvx_cursor *c = cursor_from(st);
    sqlite3_finalize(st);
    return c;
}

static int sq_select_next(mvx_cursor *c, mv_value *id) {
    if (!c || c->pos >= c->n) return 0;
    mv_copy(id, &c->ids[c->pos++]);
    return 1;
}

static void sq_select_end(mvx_cursor *c) {
    if (!c) return;
    for (int64_t i = 0; i < c->n; i++) mv_clear(&c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t sq_select_count(mvx_cursor *c) { return c ? c->n : 0; }

/* --------------------------------------------------- file lifecycle */

static int sq_create(const char *spec, char *err, size_t errlen) {
    char path[1024];
    const char *tbl = split_spec(spec, path, sizeof path);
    sqlite3 *db = sq_connect(path, err, errlen);
    if (!db) return 0;
    if (table_exists(db, tbl)) return 0;      /* already exists */
    char qt[300], sql[500];
    quote_ident(tbl, qt, sizeof qt);
    snprintf(sql, sizeof sql,
             "CREATE TABLE %s (id BLOB PRIMARY KEY, rec BLOB)", qt);
    if (!exec_sql(db, sql)) {
        snprintf(err, errlen, "sqlite: %s", sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

static int sq_remove(const char *spec, char *err, size_t errlen) {
    char path[1024];
    const char *tbl = split_spec(spec, path, sizeof path);
    sqlite3 *db = sq_connect(path, err, errlen);
    if (!db) return 0;
    if (!table_exists(db, tbl)) return 0;
    char qt[300], sql[400];
    quote_ident(tbl, qt, sizeof qt);
    snprintf(sql, sizeof sql, "DROP TABLE %s", qt);
    if (!exec_sql(db, sql)) {
        snprintf(err, errlen, "sqlite: %s", sqlite3_errmsg(db));
        return 0;
    }
    return 1;
}

/* Whole-account LISTF.  Every MV file of the account is a table in this
   one database, so the catalogue IS the file list — no directory walk
   and no server round trip.  Association child tables (name__assoc) are
   left out: they belong to a mapping, not to the account's file list. */
static int sq_names(const char *loc, mv_value *out, char *err, size_t errlen) {
    sqlite3 *db = sq_connect(loc, err, errlen);
    if (!db) return 0;
    /* A table is an MV file if it has a `rec` column — the same test the
       postgres driver uses.  That excludes association child tables and
       anything else sharing the database, without pattern-matching names
       that MV files are legitimately allowed to contain. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT m.name FROM sqlite_master m "
            "JOIN pragma_table_info(m.name) p "
            "WHERE m.type='table' AND p.name='rec' ORDER BY m.name",
            -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "sqlite: %s", sqlite3_errmsg(db));
        return 0;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(st, 0);
        if (!nm) continue;
        size_t nl = strlen(nm);
        if (len + nl + 1 > cap) {
            cap = cap ? cap * 2 : 256;
            while (cap < len + nl + 1) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) mvx_fatal("out of memory in sqlite names");
            buf = nb;
        }
        if (len) buf[len++] = (char)0xFE;      /* @AM */
        memcpy(buf + len, nm, nl);
        len += nl;
    }
    sqlite3_finalize(st);
    mv_set_str(out, buf ? buf : "", (int64_t)len);
    free(buf);
    return 1;
}

/* Transaction bracketing, for a backfill batch AND for one logical write.
 *
 * NESTING IS THE WHOLE DIFFICULTY.  A mapped WRITE touches the record, the
 * parent columns and a child table per association -- several statements that
 * must land together or not at all -- so the runtime brackets them.  But a
 * BACKFILL already holds a transaction open around thousands of such writes,
 * and SQLite refuses "cannot start a transaction within a transaction".
 *
 * sqlite3_get_autocommit() answers exactly the question: it is true when NO
 * transaction is open.  So an inner bracket becomes a no-op and the outer one
 * provides the atomicity, which is what nesting should mean.  begin() reports
 * whether it actually started one, and only that caller commits. */
static int sq_bulk_begin(mvx_file *fh) {
    sqlite3 *db = ((sq_file *)fh)->db;
    if (!sqlite3_get_autocommit(db)) return 0;   /* already inside one */
    return exec_sql(db, "BEGIN") ? 1 : 0;
}
/* Discard everything since bulk_begin.  The runtime calls this when a mapped
   write fails part way, rather than committing a record whose projection does
   not match it. */
static int sq_rollback(mvx_file *fh) {
    sqlite3 *db = ((sq_file *)fh)->db;
    if (sqlite3_get_autocommit(db)) return 1;    /* nothing open */
    return exec_sql(db, "ROLLBACK");
}

static int sq_bulk_commit(mvx_file *fh) {
    sqlite3 *db = ((sq_file *)fh)->db;
    if (sqlite3_get_autocommit(db)) return 1;    /* nothing open: nothing to do */
    if (exec_sql(db, "COMMIT")) return 1;
    exec_sql(db, "ROLLBACK");                 /* a failed batch rolls back */
    return 0;
}

/* ------------------------------------------- relational mapping (#18)
 *
 * A mapped file keeps its record blob AND columns projected out of it.
 * The columns are what ORDER BY, WITH and SUM push down onto: the
 * runtime only offers select_order a column it can find in the mapping
 * (map_order_col), so without this capability sorting stays in the verb.
 * Association fields go to a child table per association, keyed
 * (id, seq), so a multivalued field is rows rather than a packed cell. */

static const char *sq_sqltype(const char *t) {
    /* SQLite is dynamically typed, but the declared affinity still drives
       comparison and ordering: NUMERIC affinity sorts 9 before 10, TEXT
       sorts them the other way, and MV wants both depending on the field. */
    if (t && strcmp(t, "NUMERIC") == 0) return "NUMERIC";
    return "TEXT";
}

/* Add any missing columns.  ALTER TABLE ADD COLUMN is cheap in SQLite
   (metadata only), and re-adding an existing one is an error we ignore,
   which makes this idempotent the way map_ensure must be. */
static int sq_map_ensure(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                         char *err, size_t errlen) {
    sq_file *f = (sq_file *)fh;
    char qt[300];
    quote_ident(f->table, qt, sizeof qt);
    for (int i = 0; i < ncols; i++) {
        char qc[300], sql[800];
        quote_ident(cols[i].name, qc, sizeof qc);
        snprintf(sql, sizeof sql, "ALTER TABLE %s ADD COLUMN %s %s",
                 qt, qc, sq_sqltype(cols[i].type));
        char *msg = NULL;
        int rc = sqlite3_exec(f->db, sql, NULL, NULL, &msg);
        if (rc != SQLITE_OK && msg && !strstr(msg, "duplicate column")) {
            snprintf(err, errlen, "sqlite: %s", msg);
            sqlite3_free(msg);
            return 0;
        }
        if (msg) sqlite3_free(msg);
    }
    return 1;
}

static int sq_map_apply(mvx_file *fh, const char *id, int64_t idlen,
                        const mvx_mapfield *cols, const char **vals,
                        const int64_t *vlens, int ncols) {
    sq_file *f = (sq_file *)fh;
    if (ncols < 1) return 1;
    char qt[300];
    quote_ident(f->table, qt, sizeof qt);
    char sql[8192];
    size_t p = (size_t)snprintf(sql, sizeof sql, "UPDATE %s SET ", qt);
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s = ?%d",
                              i ? ", " : "", qc, i + 1);
        if (p >= sizeof sql) return 0;
    }
    snprintf(sql + p, sizeof sql - p, " WHERE id = ?%d", ncols + 1);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    for (int i = 0; i < ncols; i++) {
        if (!vals[i]) sqlite3_bind_null(st, i + 1);
        else sqlite3_bind_text(st, i + 1, vals[i], (int)vlens[i], SQLITE_STATIC);
    }
    sqlite3_bind_blob(st, ncols + 1, id, (int)idlen, SQLITE_STATIC);
    int ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

/* One child table per association: "<file>__<assoc>". */
static void child_name(sq_file *f, const char *assoc, char *out, size_t cap) {
    snprintf(out, cap, "%s__%s", f->table, assoc);
}

static int sq_map_child_ensure(mvx_file *fh, const char *assoc,
                               const mvx_mapfield *cols, int ncols,
                               char *err, size_t errlen) {
    sq_file *f = (sq_file *)fh;
    char nm[512], qn[600];
    child_name(f, assoc, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    char sql[4096];
    size_t p = (size_t)snprintf(sql, sizeof sql,
        "CREATE TABLE IF NOT EXISTS %s (id BLOB NOT NULL, seq INTEGER NOT NULL",
        qn);
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", %s %s",
                              qc, sq_sqltype(cols[i].type));
        if (p >= sizeof sql) return 0;
    }
    snprintf(sql + p, sizeof sql - p, ", PRIMARY KEY (id, seq))");
    if (!exec_sql(f->db, sql)) {
        snprintf(err, errlen, "sqlite: %s", sqlite3_errmsg(f->db));
        return 0;
    }
    /* Columns can arrive later than the table (a mapping gains a field). */
    for (int i = 0; i < ncols; i++) {
        char qc[300], asql[800];
        quote_ident(cols[i].name, qc, sizeof qc);
        snprintf(asql, sizeof asql, "ALTER TABLE %s ADD COLUMN %s %s",
                 qn, qc, sq_sqltype(cols[i].type));
        char *msg = NULL;
        sqlite3_exec(f->db, asql, NULL, NULL, &msg);
        if (msg) sqlite3_free(msg);          /* duplicate column: fine */
    }
    return 1;
}

/* Replace this record's rows for one association. */
static int sq_map_child_apply(mvx_file *fh, const char *id, int64_t idlen,
                              const char *assoc, const mvx_mapfield *cols,
                              int ncols, const char **vals,
                              const int64_t *vlens, int nrows) {
    sq_file *f = (sq_file *)fh;
    char nm[512], qn[600];
    child_name(f, assoc, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    /* The DELETE and the INSERTs are one replacement, not a sequence. */
    int started = txn_begin(f->db);

    char dsql[700];
    snprintf(dsql, sizeof dsql, "DELETE FROM %s WHERE id = ?1", qn);
    sqlite3_stmt *ds = NULL;
    if (sqlite3_prepare_v2(f->db, dsql, -1, &ds, NULL) != SQLITE_OK) {
        txn_end(f->db, started, 0);
        return 0;
    }
    sqlite3_bind_blob(ds, 1, id, (int)idlen, SQLITE_STATIC);
    sqlite3_step(ds);
    sqlite3_finalize(ds);
    if (nrows < 1) { txn_end(f->db, started, 1); return 1; }

    char sql[8192];
    size_t p = (size_t)snprintf(sql, sizeof sql, "INSERT INTO %s (id, seq", qn);
    for (int c = 0; c < ncols; c++) {
        char qc[300];
        quote_ident(cols[c].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", %s", qc);
        if (p >= sizeof sql) { txn_end(f->db, started, 0); return 0; }
    }
    p += (size_t)snprintf(sql + p, sizeof sql - p, ") VALUES (?1, ?2");
    for (int c = 0; c < ncols; c++)
        p += (size_t)snprintf(sql + p, sizeof sql - p, ", ?%d", c + 3);
    snprintf(sql + p, sizeof sql - p, ")");

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) {
        txn_end(f->db, started, 0);
        return 0;
    }
    int ok = 1;
    for (int r = 0; r < nrows && ok; r++) {
        sqlite3_reset(st);
        sqlite3_bind_blob(st, 1, id, (int)idlen, SQLITE_STATIC);
        sqlite3_bind_int(st, 2, r + 1);       /* seq is 1-based, like MV */
        for (int c = 0; c < ncols; c++) {
            const char *v = vals[(size_t)r * ncols + c];
            int64_t vl = vlens[(size_t)r * ncols + c];
            if (!v) sqlite3_bind_null(st, c + 3);
            else sqlite3_bind_text(st, c + 3, v, (int)vl, SQLITE_STATIC);
        }
        ok = sqlite3_step(st) == SQLITE_DONE;
    }
    sqlite3_finalize(st);
    txn_end(f->db, started, ok);
    return ok;
}

static int sq_map_drop(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                       const char **assocs, int nassocs, char *err,
                       size_t errlen) {
    sq_file *f = (sq_file *)fh;
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

/* Native read-back, so a column written by something other than MVX is
   what a READ sees.  Every non-NULL cell is malloc'd; the runtime frees. */
static int sq_map_read(mvx_file *fh, const char *id, int64_t idlen,
                       const mvx_mapfield *cols, int ncols,
                       char **vals, int64_t *lens) {
    sq_file *f = (sq_file *)fh;
    if (ncols < 1) return 0;
    char qt[300], sql[8192];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT ");
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s", i ? ", " : "", qc);
        if (p >= sizeof sql) return -1;
    }
    snprintf(sql + p, sizeof sql - p, " FROM %s WHERE id = ?1", qt);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_blob(st, 1, id, (int)idlen, SQLITE_STATIC);
    int rc = sqlite3_step(st), got = 0;
    if (rc == SQLITE_ROW) {
        got = 1;
        for (int i = 0; i < ncols; i++) {
            if (sqlite3_column_type(st, i) == SQLITE_NULL) {
                vals[i] = NULL; lens[i] = 0;
                continue;
            }
            const char *t = (const char *)sqlite3_column_text(st, i);
            int n = sqlite3_column_bytes(st, i);
            vals[i] = malloc((size_t)n + 1);
            if (!vals[i]) mvx_fatal("out of memory in sqlite map_read");
            memcpy(vals[i], t ? t : "", (size_t)n);
            vals[i][n] = '\0';
            lens[i] = n;
        }
    }
    sqlite3_finalize(st);
    return got;
}

static int sq_map_child_read(mvx_file *fh, const char *id, int64_t idlen,
                             const char *assoc, const mvx_mapfield *cols,
                             int ncols, char ***cells, int64_t **lens,
                             int *nrows) {
    sq_file *f = (sq_file *)fh;
    *cells = NULL; *lens = NULL; *nrows = 0;
    if (ncols < 1) return -1;
    char nm[512], qn[600], sql[8192];
    child_name(f, assoc, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT ");
    for (int i = 0; i < ncols; i++) {
        char qc[300];
        quote_ident(cols[i].name, qc, sizeof qc);
        p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s", i ? ", " : "", qc);
        if (p >= sizeof sql) return -1;
    }
    snprintf(sql + p, sizeof sql - p,
             " FROM %s WHERE id = ?1 ORDER BY seq", qn);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_blob(st, 1, id, (int)idlen, SQLITE_STATIC);
    int cap = 0, n = 0;
    char **cv = NULL; int64_t *cl = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            cv = realloc(cv, (size_t)cap * ncols * sizeof(char *));
            cl = realloc(cl, (size_t)cap * ncols * sizeof(int64_t));
            if (!cv || !cl) mvx_fatal("out of memory in sqlite map_child_read");
        }
        for (int i = 0; i < ncols; i++) {
            size_t k = (size_t)n * ncols + i;
            if (sqlite3_column_type(st, i) == SQLITE_NULL) {
                cv[k] = NULL; cl[k] = 0;
                continue;
            }
            const char *t = (const char *)sqlite3_column_text(st, i);
            int len = sqlite3_column_bytes(st, i);
            cv[k] = malloc((size_t)len + 1);
            if (!cv[k]) mvx_fatal("out of memory in sqlite map_child_read");
            memcpy(cv[k], t ? t : "", (size_t)len);
            cv[k][len] = '\0';
            cl[k] = len;
        }
        n++;
    }
    sqlite3_finalize(st);
    *cells = cv; *lens = cl; *nrows = n;
    return 1;
}

/* ------------------------------------------------------------ indexes
 *
 * The backend already stores and maintains the mapped columns, so an
 * index is one CREATE INDEX and there is no per-record backfill and no
 * write_ix/del_ix maintenance — which is why those two stay NULL below.
 * An unmapped attribute gets an expression index on mvx_attr(rec,n),
 * which is index-eligible because that function is DETERMINISTIC. */

static void index_name(sq_file *f, const char *item, char *out, size_t cap) {
    snprintf(out, cap, "mvxix_%s_%s", f->table, item);
}

static int sq_index_create(mvx_file *fh, const char *item,
                           const char *col, int64_t attr) {
    sq_file *f = (sq_file *)fh;
    char qt[300], nm[512], qn[600], expr[400], sql[1600];
    quote_ident(f->table, qt, sizeof qt);
    index_name(f, item, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    if (col && col[0]) {
        char qc[300];
        quote_ident(col, qc, sizeof qc);
        snprintf(expr, sizeof expr, "%s", qc);
    } else {
        if (attr < 1) return -1;
        snprintf(expr, sizeof expr, "mvx_attr(rec,%lld)", (long long)attr);
    }
    snprintf(sql, sizeof sql, "CREATE INDEX IF NOT EXISTS %s ON %s (%s)",
             qn, qt, expr);
    if (!exec_sql(f->db, sql)) return -1;
    /* The indexed row count, so the verb can report what it built. */
    char csql[400];
    snprintf(csql, sizeof csql, "SELECT count(*) FROM %s", qt);
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(f->db, csql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

/* NULL = no such index, which the runtime distinguishes from an empty
   cursor — so this checks the index exists before answering. */
static mvx_cursor *sq_index_select(mvx_file *fh, const char *item,
                                   const char *key, int64_t klen) {
    sq_file *f = (sq_file *)fh;
    char nm[512];
    index_name(f, item, nm, sizeof nm);
    sqlite3_stmt *chk = NULL;
    if (sqlite3_prepare_v2(f->db,
            "SELECT sql FROM sqlite_master WHERE type='index' AND name=?1",
            -1, &chk, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(chk, 1, nm, -1, SQLITE_STATIC);
    const char *idxsql = NULL;
    char kept[1024];
    kept[0] = '\0';
    if (sqlite3_step(chk) == SQLITE_ROW) {
        idxsql = (const char *)sqlite3_column_text(chk, 0);
        if (idxsql) snprintf(kept, sizeof kept, "%s", idxsql);
    }
    sqlite3_finalize(chk);
    if (!kept[0]) return NULL;                /* no such index */

    /* Recover the indexed expression from the index definition: it is
       what sits between the last '(' and the matching ')'. */
    char expr[400] = "";
    const char *op = strrchr(kept, '(');
    const char *cp = strrchr(kept, ')');
    if (op && cp && cp > op + 1) {
        size_t n = (size_t)(cp - op - 1);
        if (n >= sizeof expr) n = sizeof expr - 1;
        memcpy(expr, op + 1, n);
        expr[n] = '\0';
    } else return NULL;

    char qt[300], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s = ?1", qt, expr);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, key, (int)klen, SQLITE_STATIC);
    mvx_cursor *c = cursor_from(st);
    sqlite3_finalize(st);
    return c;
}

static int sq_index_drop(mvx_file *fh, const char *item) {
    sq_file *f = (sq_file *)fh;
    char nm[512], qn[600], sql[800];
    index_name(f, item, nm, sizeof nm);
    quote_ident(nm, qn, sizeof qn);
    snprintf(sql, sizeof sql, "DROP INDEX IF EXISTS %s", qn);
    return exec_sql(f->db, sql);
}

/* ----------------------------------------------------- WITH push-down */

/* The comparison expression for one side: a mapped column, or the blob
   attribute.  Shared by every push-down so they agree on what a field
   means — and so an expression index built on mvx_attr matches. */
static void field_expr(const char *col, int64_t attr, char *out, size_t cap) {
    if (col && col[0]) quote_ident(col, out, cap);
    else snprintf(out, cap, "mvx_attr(rec,%lld)", (long long)attr);
}

static const char *sql_op(const char *op) {
    if (!op || !op[0]) return NULL;
    if (op[0] == '=' && !op[1]) return "=";
    if (op[0] == '#' && !op[1]) return "IS NOT";
    if (strcmp(op, ">") == 0) return ">";
    if (strcmp(op, "<") == 0) return "<";
    if (strcmp(op, ">=") == 0) return ">=";
    if (strcmp(op, "<=") == 0) return "<=";
    return NULL;
}

static mvx_cursor *run_ids(sqlite3 *db, const char *sql,
                           const char **vals, const int64_t *lens, int nb) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    for (int i = 0; i < nb; i++)
        sqlite3_bind_text(st, i + 1, vals[i], (int)lens[i], SQLITE_STATIC);
    mvx_cursor *c = cursor_from(st);
    sqlite3_finalize(st);
    return c;
}

static mvx_cursor *sq_select_where(mvx_file *fh, const char *col,
                                   const char *op, const char *val,
                                   int64_t vlen) {
    sq_file *f = (sq_file *)fh;
    const char *o = sql_op(op);
    if (!o || !col || !col[0]) return NULL;
    char qt[300], qc[300], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(col, qc, sizeof qc);
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s %s ?1", qt, qc, o);
    return run_ids(f->db, sql, &val, &vlen, 1);
}

static mvx_cursor *sq_select_attr(mvx_file *fh, int64_t attr, const char *op,
                                  const char *val, int64_t vlen) {
    sq_file *f = (sq_file *)fh;
    const char *o = sql_op(op);
    if (!o || attr < 1) return NULL;
    char qt[300], ex[400], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    field_expr(NULL, attr, ex, sizeof ex);
    snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE %s %s ?1", qt, ex, o);
    return run_ids(f->db, sql, &val, &vlen, 1);
}

/* Multi-condition WITH: every predicate AND'd, each on a column or the
   blob, compared numerically where the dictionary says so. */
static mvx_cursor *sq_select_multi(mvx_file *fh, const mvx_pred *preds,
                                   int npred) {
    sq_file *f = (sq_file *)fh;
    if (npred < 1 || npred > 32) return NULL;
    char qt[300], sql[4096];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s WHERE ", qt);
    const char *vals[32];
    int64_t lens[32];
    for (int i = 0; i < npred; i++) {
        const char *o = sql_op(preds[i].op);
        if (!o) return NULL;
        char ex[400];
        field_expr(preds[i].col, preds[i].attr, ex, sizeof ex);
        /* A numeric comparison casts both sides, so 9 < 10 rather than
           "10" < "9" — the same distinction MV draws between a numeric
           and a text field. */
        if (preds[i].numeric)
            p += (size_t)snprintf(sql + p, sizeof sql - p,
                                  "%sCAST(%s AS REAL) %s CAST(?%d AS REAL)",
                                  i ? " AND " : "", ex, o, i + 1);
        else
            p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s %s ?%d",
                                  i ? " AND " : "", ex, o, i + 1);
        if (p >= sizeof sql) return NULL;
        vals[i] = preds[i].val;
        lens[i] = preds[i].vlen;
    }
    return run_ids(f->db, sql, vals, lens, npred);
}

/* ORDER BY / LIMIT.  MV sorts bytes, and SQLite's default BINARY
   collation is exactly that, so `otext` needs no special collation the
   way postgres needs COLLATE "C" — the note is here because the absence
   of a COLLATE clause otherwise looks like an oversight. */
static mvx_cursor *sq_select_order(mvx_file *fh, const char *fcol,
                                   int64_t fattr, const char *fop,
                                   const char *fval, int64_t fvlen,
                                   const char *ocol, int otext,
                                   int64_t limit) {
    sq_file *f = (sq_file *)fh;
    if (!ocol || !ocol[0]) return NULL;
    char qt[300], qo[300], sql[1400];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(ocol, qo, sizeof qo);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    int nb = 0;
    if (fop && fop[0]) {
        const char *o = sql_op(fop);
        if (!o) return NULL;
        char ex[400];
        field_expr(fcol, fattr, ex, sizeof ex);
        p += (size_t)snprintf(sql + p, sizeof sql - p, " WHERE %s %s ?1", ex, o);
        nb = 1;
    }
    p += (size_t)snprintf(sql + p, sizeof sql - p, " ORDER BY %s%s",
                          qo, otext ? " COLLATE BINARY" : "");
    if (limit > 0)
        snprintf(sql + p, sizeof sql - p, " LIMIT %lld", (long long)limit);
    return run_ids(f->db, sql, &fval, &fvlen, nb);
}

/* --------------------------------------------------- COUNT and SUM */

static int64_t sq_count_where(mvx_file *fh, const char *col, int64_t attr,
                              const char *op, const char *val, int64_t vlen) {
    sq_file *f = (sq_file *)fh;
    char qt[300], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", qt);
    int nb = 0;
    if (op && op[0]) {
        const char *o = sql_op(op);
        if (!o) return -1;
        char ex[400];
        field_expr(col, attr, ex, sizeof ex);
        snprintf(sql + p, sizeof sql - p, " WHERE %s %s ?1", ex, o);
        nb = 1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (nb) sqlite3_bind_text(st, 1, val, (int)vlen, SQLITE_STATIC);
    int64_t n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int sq_sum_where(mvx_file *fh, const char *sumcol, const char *fcol,
                        int64_t fattr, const char *fop, const char *fval,
                        int64_t fvlen, char *out, size_t cap) {
    sq_file *f = (sq_file *)fh;
    if (!sumcol || !sumcol[0]) return 0;
    char qt[300], qs[300], sql[900];
    quote_ident(f->table, qt, sizeof qt);
    quote_ident(sumcol, qs, sizeof qs);
    size_t p = (size_t)snprintf(sql, sizeof sql,
        "SELECT coalesce(sum(CAST(%s AS REAL)),0) FROM %s", qs, qt);
    int nb = 0;
    if (fop && fop[0]) {
        const char *o = sql_op(fop);
        if (!o) return 0;
        char ex[400];
        field_expr(fcol, fattr, ex, sizeof ex);
        snprintf(sql + p, sizeof sql - p, " WHERE %s %s ?1", ex, o);
        nb = 1;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(f->db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    if (nb) sqlite3_bind_text(st, 1, fval, (int)fvlen, SQLITE_STATIC);
    int ok = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        snprintf(out, cap, "%s", t ? t : "0");
        ok = 1;
    }
    sqlite3_finalize(st);
    return ok;
}

/* --------------------------------------------- cross-file TRANS() JOIN
 *
 * Whether two files can be joined is the question postgres has to ask
 * (pg_same_db) and usually cannot answer yes to.  Here every file of an
 * account is a table in ONE database, so the test is whether both handles
 * came from the same path — and because connections are pooled by path,
 * that is a pointer comparison.  This is the case this backend exists for. */
static mvx_cursor *sq_select_join(mvx_file *srch, int64_t sk,
                                  const char *src_keycol, mvx_file *tgth,
                                  int64_t ta, const char *tgt_col,
                                  const char *op, const char *val,
                                  int64_t vlen) {
    if (!op || strcmp(op, "=") != 0) return NULL;    /* equality only */
    sq_file *s = (sq_file *)srch, *t = (sq_file *)tgth;
    if (s->db != t->db) return NULL;                 /* not co-located */
    char sqt[300], tqt[300], skx[400], tax[400];
    quote_ident(s->table, sqt, sizeof sqt);
    quote_ident(t->table, tqt, sizeof tqt);
    /* A mapped identity column beats the blob expression: it can use an
       index and, in native mode, is the authoritative value. */
    if (src_keycol && src_keycol[0]) {
        char qc[300];
        quote_ident(src_keycol, qc, sizeof qc);
        snprintf(skx, sizeof skx, "s.%s", qc);
    } else snprintf(skx, sizeof skx, "mvx_attr(s.rec,%lld)", (long long)sk);
    if (tgt_col && tgt_col[0]) {
        char qc[300];
        quote_ident(tgt_col, qc, sizeof qc);
        snprintf(tax, sizeof tax, "t.%s", qc);
    } else snprintf(tax, sizeof tax, "mvx_attr(t.rec,%lld)", (long long)ta);
    /* DISTINCT collapses a source record that matches through more than one
       of its key values. */
    char sql[1600];
    snprintf(sql, sizeof sql,
             "SELECT DISTINCT s.id FROM %s s JOIN %s t "
             "ON mvx_vm_has(%s, t.id) WHERE %s = ?1", sqt, tqt, skx, tax);
    return run_ids(s->db, sql, &val, &vlen, 1);
}

/* ------------------------------------------------------------ explain
 *
 * Render the SQL this backend WOULD run, without running it — what the
 * verbs' DESCRIBE modifier shows.  Built from the same pieces as
 * select_multi/select_order so the description cannot drift from the
 * query: if this says it pushes, that is the statement that executes. */
static int sq_explain(mvx_file *fh, const mvx_pred *preds, int npred,
                      const char *ocol, int otext, int64_t limit,
                      char *out, size_t cap) {
    sq_file *f = (sq_file *)fh;
    char qt[300];
    quote_ident(f->table, qt, sizeof qt);
    char sql[4096];
    size_t p = (size_t)snprintf(sql, sizeof sql, "SELECT id FROM %s", qt);
    for (int i = 0; i < npred; i++) {
        const char *o = sql_op(preds[i].op);
        if (!o) return 0;                     /* cannot push: caller words it */
        char ex[400];
        field_expr(preds[i].col, preds[i].attr, ex, sizeof ex);
        if (preds[i].numeric)
            p += (size_t)snprintf(sql + p, sizeof sql - p,
                                  "%sCAST(%s AS REAL) %s CAST(? AS REAL)",
                                  i ? " AND " : " WHERE ", ex, o);
        else
            p += (size_t)snprintf(sql + p, sizeof sql - p, "%s%s %s ?",
                                  i ? " AND " : " WHERE ", ex, o);
        if (p >= sizeof sql) return 0;
    }
    if (ocol && ocol[0]) {
        char qo[300];
        quote_ident(ocol, qo, sizeof qo);
        p += (size_t)snprintf(sql + p, sizeof sql - p, " ORDER BY %s%s",
                              qo, otext ? " COLLATE BINARY" : "");
    }
    if (limit > 0)
        snprintf(sql + p, sizeof sql - p, " LIMIT %lld", (long long)limit);
    /* No driver-name prefix: the runtime already prepends it (mvx_store.c
       renders "<driver>: <plan>"), and adding one here printed it twice. */
    snprintf(out, cap, "%s", sql);
    return 1;
}

/* ------------------------------------------------------------- vtable */

static const mvx_driver mvx_driver_sqlite = {
    "sqlite",
    sq_open, sq_close,
    sq_read, sq_write, sq_del,
    sq_select_begin, sq_select_next, sq_select_end,
    sq_create, sq_remove,
    sq_names,                             /* whole-account LISTF */
    NULL, NULL,                           /* no MVX-maintained index writes:
                                             the backend keeps the columns and
                                             their indexes itself */
    sq_index_select, sq_index_drop,
    NULL, NULL,                           /* locks: the runtime's table.  A
                                             SQLite file is not an arbiter
                                             across sessions the way the daemon
                                             is, so claiming otherwise would be
                                             worse than not offering it */
    sq_map_ensure, sq_map_apply,
    sq_map_child_ensure, sq_map_child_apply,
    sq_map_drop,
    sq_map_read, sq_map_child_read,
    sq_index_create,
    sq_select_where,                      /* WITH on a mapped column */
    sq_select_attr,                       /* WITH on the record blob */
    sq_select_join,                       /* cross-file TRANS() JOIN */
    sq_count_where,
    sq_sum_where,
    sq_select_order,                      /* ORDER BY / LIMIT */
    sq_select_multi,                      /* multi-condition WITH (AND) */
    sq_explain,                           /* DESCRIBE */
    sq_select_count,
    sq_bulk_begin, sq_bulk_commit,
    NULL,                                 /* map_backfill: no push-down yet —
                                             the runtime's per-record loop is
                                             correct, just slower */
    NULL,                                 /* select_join_order: reproducing
                                             TRANS()'s @VM re-join in ordered
                                             SQL is a job of its own; the verb
                                             sorts the reference itself, which
                                             is correct and only slower */
    sq_rollback,                          /* abort a failed logical write */
};

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_sqlite : NULL;
}
