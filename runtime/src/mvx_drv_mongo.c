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

/* mongo driver — a MultiValue file on a MongoDB collection.
 *
 * An MV record is a natural document, so each record is stored as
 * { _id: <id>, rec: <blob> } with both fields BinData, so ids and records
 * round-trip byte-exact (marks and all).  The account/namespace maps to a
 * database and each file to a collection.  The connection is a named profile
 * (BINDINGS `ORDERS @mongomain`, .mvx-private/connections carries
 * driver/address/namespace/user/password) — the same indirection the postgres
 * and lmdbnet drivers use, or a raw mongodb:// URI.
 *
 * Beyond the minimal contract this driver also provides, in *mirror* mode
 * (the record blob stays authoritative; the columns are a derived projection):
 *   - relational mapping (#62) — a mapped file's dict columns are projected
 *     into native BSON fields on the same document, so a record reads as
 *     { _id, rec, CUST_NAME: "…", BALANCE: 152.34, LINES: [ {…}, … ] }:
 *     parent columns are scalar fields, associations an embedded array of
 *     sub-documents (the Mongo-idiomatic nested form);
 *   - native indexes (#27/#62) — CREATE-INDEX builds a real Mongo index on a
 *     mapped column, and index_select answers an equality lookup from it;
 *   - WITH / COUNT push-down (#62) — an equality (=/#) filter on a mapped
 *     column runs in the backend, so a query need not stream every id.
 *
 * Still deferred to the client-side fallback (#62): native read-back
 * (map_read / native mode — mirror mode only), SUM / ORDER BY / multi-predicate
 * push-down, raw-blob-attribute push-down (Mongo cannot split the blob
 * server-side), TRANS() JOIN, EXPLAIN, transactional bulk batching, and lock
 * authority (the runtime's process-local lock table applies).
 */
#include "../include/mvx_driver.h"
#include "../include/mvx_doc.h"

#include <mongoc/mongoc.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CONNS 8
static struct {
    char loc[1024];
    mongoc_client_t *client;
} g_conns[MAX_CONNS];
static int g_nconns;

struct mvx_cursor {
    mv_value *ids;
    int64_t n, pos, cap;
};

typedef struct {
    mvx_file_base base;
    mongoc_client_t *client;
    char db[128];                       /* namespace -> database */
    char coll[256];                     /* file spec -> collection */
} mongo_file;

static const mvx_driver mvx_driver_mongo;

static void ensure_init(void) {
    static int done;
    if (!done) { mongoc_init(); done = 1; }
}

/* Split "loc\nspec" into loc (connection) and the bare file spec. */
static const char *split_spec(const char *spec, char *loc, size_t cap) {
    const char *nl = strchr(spec, '\n');
    if (!nl) { loc[0] = '\0'; return spec; }
    size_t n = (size_t)(nl - spec);
    if (n >= cap) n = cap - 1;
    memcpy(loc, spec, n);
    loc[n] = '\0';
    return nl + 1;
}

/* Connect (once per location).  loc is "@connname" (resolved from the
   connection profile) or a raw mongodb:// URI; the namespace maps to a
   database, filled into `db`. */
static mongoc_client_t *mongo_connect(const char *loc, char *db, size_t dbcap,
                                      char *err, size_t errlen) {
    ensure_init();

    /* resolve the database (namespace) regardless of connection cache */
    if (loc[0] == '@') {
        const char *cn = loc + 1;
        if (!mvx_conn_lookup(cn, "namespace", db, dbcap))
            mvx_account_namespace(db, dbcap);
    } else {
        mvx_account_namespace(db, dbcap);
    }
    if (!db[0]) snprintf(db, dbcap, "mvx");

    for (int i = 0; i < g_nconns; i++)
        if (strcmp(g_conns[i].loc, loc) == 0) return g_conns[i].client;

    if (g_nconns >= MAX_CONNS) {
        snprintf(err, errlen, "mongo: too many connections");
        return NULL;
    }

    char uri[1200];
    if (loc[0] == '@') {
        const char *cn = loc + 1;
        char address[256] = "", user[128] = "", password[256] = "";
        mvx_conn_lookup(cn, "address", address, sizeof address);
        mvx_conn_lookup(cn, "user", user, sizeof user);
        mvx_conn_lookup(cn, "password", password, sizeof password);
        if (!address[0]) snprintf(address, sizeof address, "localhost:27017");
        if (user[0])
            snprintf(uri, sizeof uri, "mongodb://%s:%s@%s", user, password,
                     address);
        else
            snprintf(uri, sizeof uri, "mongodb://%s", address);
    } else {
        snprintf(uri, sizeof uri, "%s", loc);   /* raw mongodb:// URI */
    }

    /* mongoc_client_new parses the URI itself and exists across driver v1 and
       v2, so one code path builds against Homebrew's libmongoc 2.x and apt's
       1.x alike; it returns NULL only on an unparseable URI. */
    mongoc_client_t *c = mongoc_client_new(uri);
    if (!c) {
        snprintf(err, errlen, "mongo: invalid connection URI '%s'", uri);
        return NULL;
    }
    snprintf(g_conns[g_nconns].loc, sizeof g_conns[0].loc, "%s", loc);
    g_conns[g_nconns].client = c;
    g_nconns++;
    return c;
}

/* A collection handle for one operation (cheap; destroy after). */
static mongoc_collection_t *coll_of(mongo_file *f) {
    return mongoc_client_get_collection(f->client, f->db, f->coll);
}

/* Selector { _id: <id> } as BinData. */
static void sel_id(bson_t *sel, const char *id, int64_t idlen) {
    bson_init(sel);
    bson_append_binary(sel, "_id", 3, BSON_SUBTYPE_BINARY,
                       (const uint8_t *)id, (uint32_t)idlen);
}

static mvx_file *mongo_open(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *rspec = split_spec(spec, loc, sizeof loc);
    char db[128];
    mongoc_client_t *c = mongo_connect(loc, db, sizeof db, err, errlen);
    if (!c) return NULL;
    mongoc_database_t *d = mongoc_client_get_database(c, db);
    bson_error_t berr;
    bool exists = mongoc_database_has_collection(d, rspec, &berr);
    mongoc_database_destroy(d);
    if (!exists) return NULL;             /* not found: normal ELSE path */

    mongo_file *f = calloc(1, sizeof(mongo_file));
    if (!f) mvx_fatal("out of memory opening %s", spec);
    f->base.driver = &mvx_driver_mongo;
    f->base.spec = strdup(spec);
    f->client = c;
    snprintf(f->db, sizeof f->db, "%s", db);
    snprintf(f->coll, sizeof f->coll, "%s", rspec);
    return (mvx_file *)f;
}

static void mongo_close(mvx_file *fh) {
    mongo_file *f = (mongo_file *)fh;
    free(f->base.spec);
    free(f);                              /* the client is pooled */
}

static int mongo_create(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *rspec = split_spec(spec, loc, sizeof loc);
    char db[128];
    mongoc_client_t *c = mongo_connect(loc, db, sizeof db, err, errlen);
    if (!c) return 0;
    mongoc_database_t *d = mongoc_client_get_database(c, db);
    bson_error_t berr;
    if (mongoc_database_has_collection(d, rspec, &berr)) {
        mongoc_database_destroy(d);
        return 0;                         /* already exists (not an error) */
    }
    mongoc_collection_t *coll =
        mongoc_database_create_collection(d, rspec, NULL, &berr);
    mongoc_database_destroy(d);
    if (!coll) {
        snprintf(err, errlen, "mongo: %s", berr.message);
        return 0;
    }
    mongoc_collection_destroy(coll);
    return 1;
}

static int mongo_remove(const char *spec, char *err, size_t errlen) {
    char loc[1024];
    const char *rspec = split_spec(spec, loc, sizeof loc);
    char db[128];
    mongoc_client_t *c = mongo_connect(loc, db, sizeof db, err, errlen);
    if (!c) return 0;
    mongoc_database_t *d = mongoc_client_get_database(c, db);
    bson_error_t berr;
    int exists = mongoc_database_has_collection(d, rspec, &berr);
    mongoc_database_destroy(d);
    if (!exists) return 0;                /* nothing to drop */
    mongoc_collection_t *coll = mongoc_client_get_collection(c, db, rspec);
    bool ok = mongoc_collection_drop(coll, &berr);
    mongoc_collection_destroy(coll);
    if (!ok) snprintf(err, errlen, "mongo: %s", berr.message);
    return ok ? 1 : 0;
}

static int mongo_read(mvx_file *fh, const char *id, int64_t idlen,
                      mv_value *rec) {
    mongo_file *f = (mongo_file *)fh;
    mongoc_collection_t *coll = coll_of(f);
    bson_t filter, opts;
    sel_id(&filter, id, idlen);
    bson_init(&opts);
    bson_append_int64(&opts, "limit", 5, 1);
    mongoc_cursor_t *cur =
        mongoc_collection_find_with_opts(coll, &filter, &opts, NULL);
    const bson_t *doc;
    int found = 0;
    if (mongoc_cursor_next(cur, &doc)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "doc") &&
            BSON_ITER_HOLDS_DOCUMENT(&it)) {
            /* Back through JSON: every value in the representation is a string
               (#157), so relaxed extended JSON of this subdocument is plain
               JSON and mvx_doc_decode is its exact inverse. */
            uint32_t dlen = 0;
            const uint8_t *ddata = NULL;
            bson_iter_document(&it, &dlen, &ddata);
            bson_t sub;
            if (bson_init_static(&sub, ddata, dlen)) {
                size_t jlen = 0;
                char *js = bson_as_relaxed_extended_json(&sub, &jlen);
                if (js) {
                    mv_value jv;
                    mv_init(&jv);
                    mv_set_str(&jv, js, (int64_t)jlen);
                    mvx_doc_decode(rec, &jv);
                    mv_clear(&jv);
                    bson_free(js);
                    found = 1;
                }
            }
        }
    }
    mongoc_cursor_destroy(cur);
    bson_destroy(&filter);
    bson_destroy(&opts);
    mongoc_collection_destroy(coll);
    return found;
}

static int mongo_write(mvx_file *fh, const char *id, int64_t idlen,
                       const mv_value *rec) {
    mongo_file *f = (mongo_file *)fh;
    mongoc_collection_t *coll = coll_of(f);
    char nb[40];
    const char *rp;
    int64_t rl = mv_val_chars(rec, nb, sizeof nb, &rp);
    /* Update only `doc` (upserting {_id, doc} when absent) rather than
       replacing the whole document, so any mapped columns projected onto it
       survive a write that did not change them — the runtime's mapping
       projection re-applies only the *changed* columns afterwards.
       The record is stored as a real BSON SUBDOCUMENT, not a blob: that is
       what gives mongo a raw-attribute filter at all (doc.3), which it has
       never had, because there is no server-side way to split a blob (#157). */
    bson_t sel, set, update, opts;
    sel_id(&sel, id, idlen);
    bson_init(&set);
    mv_value jdoc;
    mv_init(&jdoc);
    mvx_doc_encode(&jdoc, rec);
    char jb[40];
    const char *jp;
    int64_t jl = mv_val_chars(&jdoc, jb, sizeof jb, &jp);
    bson_error_t jerr;
    bson_t *body = bson_new_from_json((const uint8_t *)jp, (ssize_t)jl, &jerr);
    if (body) {
        bson_append_document(&set, "doc", 3, body);
        bson_destroy(body);
    }
    mv_clear(&jdoc);
    (void)rp; (void)rl;
    bson_init(&update);
    bson_append_document(&update, "$set", 4, &set);
    bson_init(&opts);
    bson_append_bool(&opts, "upsert", 6, true);
    bson_error_t berr;
    bool ok = mongoc_collection_update_one(coll, &sel, &update, &opts, NULL,
                                           &berr);
    bson_destroy(&sel);
    bson_destroy(&set);
    bson_destroy(&update);
    bson_destroy(&opts);
    mongoc_collection_destroy(coll);
    return ok ? 1 : 0;
}

static int mongo_del(mvx_file *fh, const char *id, int64_t idlen) {
    mongo_file *f = (mongo_file *)fh;
    mongoc_collection_t *coll = coll_of(f);
    bson_t sel, reply;
    sel_id(&sel, id, idlen);
    bson_error_t berr;
    int deleted = 0;
    if (mongoc_collection_delete_one(coll, &sel, NULL, &reply, &berr)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, &reply, "deletedCount"))
            deleted = bson_iter_as_int64(&it) > 0;
        bson_destroy(&reply);
    }
    bson_destroy(&sel);
    mongoc_collection_destroy(coll);
    return deleted;
}

/* Snapshot the _ids matching `filter` (projecting only _id — a short read),
   then stream them.  Shared by the full select and every push-down. */
static mvx_cursor *query_ids(mongo_file *f, const bson_t *filter) {
    mvx_cursor *c = calloc(1, sizeof(mvx_cursor));
    if (!c) mvx_fatal("out of memory in mongo select");
    mongoc_collection_t *coll = coll_of(f);
    bson_t opts, proj;
    bson_init(&proj);
    bson_append_int32(&proj, "_id", 3, 1);
    bson_init(&opts);
    bson_append_document(&opts, "projection", 10, &proj);
    mongoc_cursor_t *mc =
        mongoc_collection_find_with_opts(coll, filter, &opts, NULL);
    const bson_t *doc;
    while (mongoc_cursor_next(mc, &doc)) {
        bson_iter_t it;
        if (!bson_iter_init_find(&it, doc, "_id") || !BSON_ITER_HOLDS_BINARY(&it))
            continue;
        bson_subtype_t st;
        uint32_t len;
        const uint8_t *data;
        bson_iter_binary(&it, &st, &len, &data);
        if (c->n == c->cap) {
            c->cap = c->cap ? c->cap * 2 : 64;
            c->ids = realloc(c->ids, (size_t)c->cap * sizeof(mv_value));
            if (!c->ids) mvx_fatal("out of memory in mongo select");
        }
        mv_init(&c->ids[c->n]);
        mv_set_str(&c->ids[c->n], (const char *)data, (int64_t)len);
        c->n++;
    }
    mongoc_cursor_destroy(mc);
    bson_destroy(&opts);
    bson_destroy(&proj);
    mongoc_collection_destroy(coll);
    return c;
}

/* Snapshot the id list up front (a short read), then stream it. */
static mvx_cursor *mongo_select_begin(mvx_file *fh) {
    bson_t filter;
    bson_init(&filter);                   /* {} — every document */
    mvx_cursor *c = query_ids((mongo_file *)fh, &filter);
    bson_destroy(&filter);
    return c;
}

static int mongo_select_next(mvx_cursor *c, mv_value *id) {
    if (!c || c->pos >= c->n) return 0;
    mv_copy(id, &c->ids[c->pos++]);
    return 1;
}

static void mongo_select_end(mvx_cursor *c) {
    if (!c) return;
    for (int64_t i = 0; i < c->n; i++) mv_clear(&c->ids[i]);
    free(c->ids);
    free(c);
}

static int64_t mongo_select_count(mvx_cursor *c) { return c ? c->n : 0; }

/* LISTF: the collections in this account's database, @AM-separated. */
static int mongo_names(const char *loc, mv_value *out, char *err,
                       size_t errlen) {
    char db[128];
    mongoc_client_t *c = mongo_connect(loc, db, sizeof db, err, errlen);
    if (!c) return 0;
    mongoc_database_t *d = mongoc_client_get_database(c, db);
    bson_error_t berr;
    char **names = mongoc_database_get_collection_names_with_opts(d, NULL, &berr);
    mongoc_database_destroy(d);
    if (!names) {
        snprintf(err, errlen, "mongo: %s", berr.message);
        return 0;
    }
    char buf[8192];
    size_t p = 0;
    for (int i = 0; names[i]; i++) {
        int n = snprintf(buf + p, sizeof buf - p, "%s%s", p ? "\xFE" : "",
                         names[i]);
        if (n < 0 || (size_t)n >= sizeof buf - p) break;
        p += (size_t)n;
    }
    bson_strfreev(names);
    mv_set_str(out, buf, (int64_t)p);
    return 1;
}

/* --- relational mapping / native indexes / push-down (#62) ----------------

   Mirror mode: the runtime keeps the record blob authoritative and hands the
   driver the projected column values; we render them as native BSON fields on
   the same { _id, rec } document so the record reads relationally in Mongo and
   equality filters/indexes can run server-side. */

/* Parse [p,plen) as an MV numeric literal.  0 = not numeric; 1 = integer
   (*iv); 2 = real (*dv).  The mapper emits canonical numeric text for a
   NUMERIC column, so this recognises exactly those. */
static int parse_number(const char *p, int64_t plen, int64_t *iv, double *dv) {
    if (plen <= 0 || plen >= 64) return 0;
    char buf[64];
    memcpy(buf, p, (size_t)plen);
    buf[plen] = '\0';
    char *end;
    errno = 0;
    long long ll = strtoll(buf, &end, 10);
    if (end == buf + plen && errno == 0) { *iv = (int64_t)ll; return 1; }
    errno = 0;
    double d = strtod(buf, &end);
    if (end == buf + plen && errno == 0) { *dv = d; return 2; }
    return 0;
}

/* Append value [val,vlen) under `key` into `doc`, typed by the mapping type:
   a NUMERIC value that parses becomes a BSON number (so it sorts/sums and
   displays as a number), everything else a UTF-8 string. */
static void append_typed(bson_t *doc, const char *key, const char *val,
                         int64_t vlen, const char *type) {
    if (type && strcmp(type, "NUMERIC") == 0) {
        int64_t iv;
        double dv;
        int k = parse_number(val, vlen, &iv, &dv);
        if (k == 1) { bson_append_int64(doc, key, -1, iv); return; }
        if (k == 2) { bson_append_double(doc, key, -1, dv); return; }
    }
    bson_append_utf8(doc, key, -1, val, (int)vlen);
}

/* Build the filter { col: {$in|$nin: [<string>, <number?>]} } for a "="/"#"
   push-down.  Both representations go in the list so a NUMERIC column (stored
   as a BSON number) and a TEXT column (stored as a string) both match; $nin
   also matches a missing field, so "#" counts an empty attribute as not-equal
   the way MV does. */
static void build_pred(bson_t *filter, const char *col, const char *op,
                       const char *val, int64_t vlen) {
    int64_t iv;
    double dv;
    int k = parse_number(val, vlen, &iv, &dv);
    int eq = (op[0] == '=');
    bson_t inner, arr;
    bson_append_document_begin(filter, col, -1, &inner);
    bson_append_array_begin(&inner, eq ? "$in" : "$nin", -1, &arr);
    bson_append_utf8(&arr, "0", 1, val, (int)vlen);
    if (k == 1) bson_append_int64(&arr, "1", 1, iv);
    else if (k == 2) bson_append_double(&arr, "1", 1, dv);
    bson_append_array_end(&inner, &arr);
    bson_append_document_end(filter, &inner);
}

/* No schema to create in a schemaless store — mapping columns appear on the
   document the first time map_apply sets them. */
static int mongo_map_ensure(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                            char *err, size_t errlen) {
    (void)fh; (void)cols; (void)ncols; (void)err; (void)errlen;
    return 1;
}

/* Project a record's parent columns onto its document: $set the non-empty
   ones, $unset the empties (so a value cleared on re-projection disappears). */
static int mongo_map_apply(mvx_file *fh, const char *id, int64_t idlen,
                           const mvx_mapfield *cols, const char **vals,
                           const int64_t *vlens, int ncols) {
    mongo_file *f = (mongo_file *)fh;
    if (ncols <= 0) return 1;
    bson_t set, unset, update;
    bson_init(&set);
    bson_init(&unset);
    int nset = 0, nunset = 0;
    for (int i = 0; i < ncols; i++) {
        if (vlens[i] > 0) {
            append_typed(&set, cols[i].name, vals[i], vlens[i], cols[i].type);
            nset++;
        } else {
            bson_append_utf8(&unset, cols[i].name, -1, "", 0);
            nunset++;
        }
    }
    bson_init(&update);
    if (nset) bson_append_document(&update, "$set", 4, &set);
    if (nunset) bson_append_document(&update, "$unset", 6, &unset);
    bson_t sel;
    sel_id(&sel, id, idlen);
    mongoc_collection_t *coll = coll_of(f);
    bson_error_t berr;
    bool ok = mongoc_collection_update_one(coll, &sel, &update, NULL, NULL,
                                           &berr);
    mongoc_collection_destroy(coll);
    bson_destroy(&sel);
    bson_destroy(&set);
    bson_destroy(&unset);
    bson_destroy(&update);
    return ok ? 1 : 0;
}

/* No child collections: an association is an embedded array (below). */
static int mongo_map_child_ensure(mvx_file *fh, const char *assoc,
                                  const mvx_mapfield *cols, int ncols,
                                  char *err, size_t errlen) {
    (void)fh; (void)assoc; (void)cols; (void)ncols; (void)err; (void)errlen;
    return 1;
}

/* Replace one association's rows as an embedded array of sub-documents:
   $set { <assoc>: [ {col: val, …}, … ] }.  An empty cell is omitted from its
   row; nrows == 0 sets an empty array (clears prior rows). */
static int mongo_map_child_apply(mvx_file *fh, const char *id, int64_t idlen,
                                 const char *assoc, const mvx_mapfield *cols,
                                 int ncols, const char **vals,
                                 const int64_t *vlens, int nrows) {
    mongo_file *f = (mongo_file *)fh;
    bson_t set, arr, update;
    bson_init(&set);
    bson_append_array_begin(&set, assoc, -1, &arr);
    for (int r = 0; r < nrows; r++) {
        char rk[16];
        snprintf(rk, sizeof rk, "%d", r);
        bson_t row;
        bson_append_document_begin(&arr, rk, -1, &row);
        for (int c = 0; c < ncols; c++) {
            int64_t vl = vlens[(int64_t)r * ncols + c];
            if (vl <= 0) continue;
            append_typed(&row, cols[c].name, vals[(int64_t)r * ncols + c], vl,
                         cols[c].type);
        }
        bson_append_document_end(&arr, &row);
    }
    bson_append_array_end(&set, &arr);
    bson_init(&update);
    bson_append_document(&update, "$set", 4, &set);
    bson_t sel;
    sel_id(&sel, id, idlen);
    mongoc_collection_t *coll = coll_of(f);
    bson_error_t berr;
    bool ok = mongoc_collection_update_one(coll, &sel, &update, NULL, NULL,
                                           &berr);
    mongoc_collection_destroy(coll);
    bson_destroy(&sel);
    bson_destroy(&set);
    bson_destroy(&update);
    return ok ? 1 : 0;
}

/* Tear a mapping down: $unset the parent columns and the association arrays
   from every document. */
static int mongo_map_drop(mvx_file *fh, const mvx_mapfield *cols, int ncols,
                          const char **assocs, int nassocs, char *err,
                          size_t errlen) {
    mongo_file *f = (mongo_file *)fh;
    if (ncols <= 0 && nassocs <= 0) return 1;
    bson_t unset, update, sel;
    bson_init(&unset);
    for (int i = 0; i < ncols; i++)
        bson_append_utf8(&unset, cols[i].name, -1, "", 0);
    for (int a = 0; a < nassocs; a++)
        bson_append_utf8(&unset, assocs[a], -1, "", 0);
    bson_init(&update);
    bson_append_document(&update, "$unset", 6, &unset);
    bson_init(&sel);                      /* {} — every document */
    mongoc_collection_t *coll = coll_of(f);
    bson_error_t berr;
    bool ok = mongoc_collection_update_many(coll, &sel, &update, NULL, NULL,
                                            &berr);
    mongoc_collection_destroy(coll);
    bson_destroy(&sel);
    bson_destroy(&unset);
    bson_destroy(&update);
    if (!ok) snprintf(err, errlen, "mongo: %s", berr.message);
    return ok ? 1 : 0;
}

/* Does an index named "<item>_1" (as index_create names them) exist? */
static int mongo_has_index(mongo_file *f, const char *item) {
    char idxname[192];
    snprintf(idxname, sizeof idxname, "%s_1", item);
    mongoc_collection_t *coll = coll_of(f);
    mongoc_cursor_t *cur = mongoc_collection_find_indexes_with_opts(coll, NULL);
    const bson_t *doc;
    int found = 0;
    while (cur && mongoc_cursor_next(cur, &doc)) {
        bson_iter_t it;
        if (bson_iter_init_find(&it, doc, "name") && BSON_ITER_HOLDS_UTF8(&it)) {
            uint32_t l;
            const char *nm = bson_iter_utf8(&it, &l);
            if (nm && strcmp(nm, idxname) == 0) { found = 1; break; }
        }
    }
    if (cur) mongoc_cursor_destroy(cur);
    mongoc_collection_destroy(coll);
    return found;
}

/* Build a native Mongo index on a mapped column (the mapping already stores
   and maintains the field).  Indexes a mapped column only — Mongo cannot
   express the blob-attribute expression index the SQL driver uses — so a
   NULL/empty `col` (an unmapped attribute) returns -1.  Returns the row
   count.  The index is named "<item>_1" to match index_drop/has_index. */
static int mongo_index_create(mvx_file *fh, const char *item, const char *col,
                              int64_t attr) {
    (void)attr;
    mongo_file *f = (mongo_file *)fh;
    if (!col || !col[0]) return -1;
    char idxname[192];
    snprintf(idxname, sizeof idxname, "%s_1", item);
    /* createIndexes command — portable across libmongoc 1.x and 2.x. */
    bson_t cmd, indexes, model, key, reply;
    bson_error_t berr;
    bson_init(&cmd);
    bson_append_utf8(&cmd, "createIndexes", -1, f->coll, -1);
    bson_append_array_begin(&cmd, "indexes", -1, &indexes);
    bson_append_document_begin(&indexes, "0", 1, &model);
    bson_append_document_begin(&model, "key", 3, &key);
    bson_append_int32(&key, col, -1, 1);
    bson_append_document_end(&model, &key);
    bson_append_utf8(&model, "name", 4, idxname, -1);
    bson_append_document_end(&indexes, &model);
    bson_append_array_end(&cmd, &indexes);
    bool ok = mongoc_client_command_simple(f->client, f->db, &cmd, NULL, &reply,
                                           &berr);
    bson_destroy(&cmd);
    bson_destroy(&reply);
    if (!ok) return -1;
    mongoc_collection_t *coll = coll_of(f);
    bson_t empty;
    bson_init(&empty);
    int64_t n = mongoc_collection_count_documents(coll, &empty, NULL, NULL,
                                                  NULL, &berr);
    bson_destroy(&empty);
    mongoc_collection_destroy(coll);
    return n < 0 ? 0 : (int)n;
}

/* Equality lookup on a mapped column via its Mongo index.  The runtime only
   calls this for an identity TEXT column named `item`; require the index to
   exist so a missing one returns NULL (the runtime then scans). */
static mvx_cursor *mongo_index_select(mvx_file *fh, const char *item,
                                      const char *key, int64_t klen) {
    mongo_file *f = (mongo_file *)fh;
    if (!mongo_has_index(f, item)) return NULL;
    bson_t filter;
    bson_init(&filter);
    bson_append_utf8(&filter, item, -1, key, (int)klen);
    mvx_cursor *c = query_ids(f, &filter);
    bson_destroy(&filter);
    return c;
}

static int mongo_index_drop(mvx_file *fh, const char *item) {
    mongo_file *f = (mongo_file *)fh;
    char idxname[192];
    snprintf(idxname, sizeof idxname, "%s_1", item);
    bson_t cmd, reply;
    bson_error_t berr;
    bson_init(&cmd);
    bson_append_utf8(&cmd, "dropIndexes", -1, f->coll, -1);
    bson_append_utf8(&cmd, "index", 5, idxname, -1);
    bool ok = mongoc_client_command_simple(f->client, f->db, &cmd, NULL, &reply,
                                           &berr);
    bson_destroy(&cmd);
    bson_destroy(&reply);
    return ok ? 1 : 0;                    /* an absent index errors -> 0 */
}

/* Server-side WITH push-down: the ids whose mapped column satisfies "="/"#". */
/* A predicate on a RAW attribute of the document — the push-down this driver
 * has never had, because there was no server-side way to split a blob (#157).
 *
 * MONGO MATCHES ARRAY ELEMENTS NATIVELY, and that is the trap here.  A
 * multivalued attribute is a JSON array, so the obvious {"doc.3": "6"} matches
 * a record whose attribute 3 is ["5","6"] — while sqlite and postgres, which
 * compare the whole attribute, do not.  Letting that through would make WITH
 * mean something different on mongo than everywhere else, which is exactly the
 * backend-specific behaviour non-negotiable 6 rules out.
 *
 * So an equality is constrained to a SCALAR attribute, and a not-equal admits
 * arrays (a multivalued attribute is indeed not equal to a single value).  The
 * result agrees with the other backends value for value.
 *
 * When the any-value question is settled (it is a real improvement mongo could
 * offer for free), all four backends change together — not this one alone. */
static void build_attr_pred(bson_t *filter, int64_t attr, const char *op,
                            const char *val, int64_t vlen) {
    char path[32];
    snprintf(path, sizeof path, "doc.%lld", (long long)attr);
    if (op[0] == '=') {
        bson_t arr, eqd, notd, typd;
        bson_append_array_begin(filter, "$and", 4, &arr);
        bson_append_document_begin(&arr, "0", 1, &eqd);
        bson_append_utf8(&eqd, path, -1, val, (int)vlen);
        bson_append_document_end(&arr, &eqd);
        bson_append_document_begin(&arr, "1", 1, &notd);
        bson_t fieldd, notinner;
        bson_append_document_begin(&notd, path, -1, &fieldd);
        bson_append_document_begin(&fieldd, "$not", 4, &notinner);
        bson_append_utf8(&notinner, "$type", 5, "array", 5);
        bson_append_document_end(&fieldd, &notinner);
        bson_append_document_end(&notd, &fieldd);
        bson_append_document_end(&arr, &notd);
        bson_append_array_end(filter, &arr);
        (void)typd;
    } else {                              /* '#' — not equal */
        bson_t arr, isarr, ned;
        bson_append_array_begin(filter, "$or", 3, &arr);
        bson_append_document_begin(&arr, "0", 1, &isarr);
        bson_t f1;
        bson_append_document_begin(&isarr, path, -1, &f1);
        bson_append_utf8(&f1, "$type", 5, "array", 5);
        bson_append_document_end(&isarr, &f1);
        bson_append_document_end(&arr, &isarr);
        bson_append_document_begin(&arr, "1", 1, &ned);
        bson_t f2;
        bson_append_document_begin(&ned, path, -1, &f2);
        bson_append_utf8(&f2, "$ne", 3, val, (int)vlen);
        bson_append_document_end(&ned, &f2);
        bson_append_document_end(&arr, &ned);
        bson_append_array_end(filter, &arr);
    }
}

/* WITH on an un-mapped attribute, in the backend. */
static mvx_cursor *mongo_select_attr(mvx_file *fh, int64_t attr, const char *op,
                                     const char *val, int64_t vlen) {
    if (!op || !((op[0] == '=' || op[0] == '#') && !op[1])) return NULL;
    if (attr < 1) return NULL;
    mongo_file *f = (mongo_file *)fh;
    bson_t filter;
    bson_init(&filter);
    build_attr_pred(&filter, attr, op, val, vlen);
    mvx_cursor *c = query_ids(f, &filter);
    bson_destroy(&filter);
    return c;
}

static mvx_cursor *mongo_select_where(mvx_file *fh, const char *col,
                                      const char *op, const char *val,
                                      int64_t vlen) {
    if (!op || !((op[0] == '=' || op[0] == '#') && !op[1])) return NULL;
    mongo_file *f = (mongo_file *)fh;
    bson_t filter;
    bson_init(&filter);
    build_pred(&filter, col, op, val, vlen);
    mvx_cursor *c = query_ids(f, &filter);
    bson_destroy(&filter);
    return c;
}

/* Server-side COUNT: matching records counted in the backend.  op NULL/""
   counts all; a mapped column filters with "="/"#".  A raw-attribute filter
   (col NULL) cannot be pushed -> -1 (the runtime counts by scanning). */
static int64_t mongo_count_where(mvx_file *fh, const char *col, int64_t attr,
                                 const char *op, const char *val,
                                 int64_t vlen) {
    mongo_file *f = (mongo_file *)fh;
    bson_t filter;
    bson_init(&filter);
    if (op && op[0]) {
        if (!((op[0] == '=' || op[0] == '#') && !op[1])) {
            bson_destroy(&filter);
            return -1;
        }
        if (col && col[0]) build_pred(&filter, col, op, val, vlen);
        else if (attr >= 1) build_attr_pred(&filter, attr, op, val, vlen);
        else { bson_destroy(&filter); return -1; }
    }
    mongoc_collection_t *coll = coll_of(f);
    bson_error_t berr;
    int64_t n = mongoc_collection_count_documents(coll, &filter, NULL, NULL,
                                                  NULL, &berr);
    mongoc_collection_destroy(coll);
    bson_destroy(&filter);
    return n;                             /* -1 on backend error */
}

/* ------------------------------------------------------- doc migration */

/* Convert every pre-#157 collection in this database to the document form.
   No schema to alter here — the change is per document: encode the `rec`
   BinData into a `doc` subdocument and unset `rec`.  Mongo has no
   multi-document transaction on a standalone server, so this converts a
   document at a time and is written to be re-runnable: a document that
   already has `doc` is left alone, so an interrupted run is finished by
   running it again. */
static int mongo_migrate_docs(const char *loc, char *err, size_t errlen) {
    char dbname[128] = "";
    mongoc_client_t *cl = mongo_connect(loc, dbname, sizeof dbname, err, errlen);
    if (!cl) return -1;
    mongoc_database_t *db = mongoc_client_get_database(cl, dbname);
    bson_error_t berr;
    char **colls = mongoc_database_get_collection_names_with_opts(db, NULL, &berr);
    if (!colls) {
        snprintf(err, errlen, "mongo: %s", berr.message);
        mongoc_database_destroy(db);
        return -1;
    }
    int done = 0;
    for (int i = 0; colls[i]; i++) {
        mongoc_collection_t *coll =
            mongoc_client_get_collection(cl, dbname, colls[i]);
        bson_t filter;
        bson_init(&filter);
        bson_t ex;
        bson_append_document_begin(&filter, "rec", 3, &ex);
        bson_append_bool(&ex, "$exists", 7, true);
        bson_append_document_end(&filter, &ex);
        mongoc_cursor_t *cur =
            mongoc_collection_find_with_opts(coll, &filter, NULL, NULL);
        const bson_t *d;
        int converted = 0, failed = 0;
        while (!failed && mongoc_cursor_next(cur, &d)) {
            bson_iter_t it, idit;
            if (bson_iter_init_find(&it, d, "doc")) continue;   /* already done */
            if (!bson_iter_init_find(&it, d, "rec") ||
                !BSON_ITER_HOLDS_BINARY(&it) ||
                !bson_iter_init_find(&idit, d, "_id")) continue;
            bson_subtype_t st;
            uint32_t rl = 0;
            const uint8_t *rp = NULL;
            bson_iter_binary(&it, &st, &rl, &rp);
            mv_value rec, jdoc;
            mv_init(&rec); mv_init(&jdoc);
            mv_set_str(&rec, (const char *)rp, (int64_t)rl);
            mvx_doc_encode(&jdoc, &rec);
            char nb[64];
            const char *jp;
            int64_t jl = mv_val_chars(&jdoc, nb, sizeof nb, &jp);
            bson_error_t je;
            bson_t *body = bson_new_from_json((const uint8_t *)jp, (ssize_t)jl, &je);
            if (body) {
                bson_t sel, set, unset, update;
                bson_init(&sel);
                bson_append_value(&sel, "_id", 3, bson_iter_value(&idit));
                bson_init(&update);
                bson_init(&set);
                bson_append_document(&set, "doc", 3, body);
                bson_append_document(&update, "$set", 4, &set);
                bson_init(&unset);
                bson_append_int32(&unset, "rec", 3, 1);
                bson_append_document(&update, "$unset", 6, &unset);
                if (!mongoc_collection_update_one(coll, &sel, &update, NULL,
                                                  NULL, &berr))
                    failed = 1;
                else converted++;
                bson_destroy(&sel); bson_destroy(&set);
                bson_destroy(&unset); bson_destroy(&update);
                bson_destroy(body);
            } else failed = 1;
            mv_clear(&rec); mv_clear(&jdoc);
        }
        mongoc_cursor_destroy(cur);
        bson_destroy(&filter);
        mongoc_collection_destroy(coll);
        if (failed) {
            snprintf(err, errlen, "mongo: %s: %s", colls[i], berr.message);
            bson_strfreev(colls);
            mongoc_database_destroy(db);
            return -1;
        }
        if (converted) done++;
    }
    bson_strfreev(colls);
    mongoc_database_destroy(db);
    return done;
}

static const mvx_driver mvx_driver_mongo = {
    .name = "mongo",
    .open = mongo_open,
    .close = mongo_close,
    .read = mongo_read,
    .write = mongo_write,
    .del = mongo_del,
    .select_begin = mongo_select_begin,
    .select_next = mongo_select_next,
    .select_end = mongo_select_end,
    .create = mongo_create,
    .remove = mongo_remove,
    .names = mongo_names,
    .select_count = mongo_select_count,
    /* Mirror-mode relational mapping (#62): columns/associations projected
       onto the { _id, rec } document as native BSON fields / embedded arrays. */
    .map_ensure = mongo_map_ensure,
    .map_apply = mongo_map_apply,
    .map_child_ensure = mongo_map_child_ensure,
    .map_child_apply = mongo_map_child_apply,
    .map_drop = mongo_map_drop,
    /* Native Mongo indexes on mapped columns (#27/#62); no write_ix/del_ix —
       the mapping maintains the field, Mongo maintains its own index. */
    .index_create = mongo_index_create,
    .index_select = mongo_index_select,
    .index_drop = mongo_index_drop,
    /* WITH / COUNT equality push-down on a mapped column (#62). */
    .select_where = mongo_select_where,
    .select_attr = mongo_select_attr,     /* raw attribute — new with #157 */
    .count_where = mongo_count_where,
    .migrate_docs = mongo_migrate_docs,   /* pre-#157 blob -> document */
    /* Deferred to the runtime's client-side fallback (#62): native read-back
       (map_read/map_child_read — mirror mode only), select_join, sum_where,
       select_order,
       select_multi, explain, bulk batching, map_backfill, and lock authority. */
};

const mvx_driver *mvx_driver_entry(int abi) {
    return abi == MVX_DRIVER_ABI ? &mvx_driver_mongo : NULL;
}
