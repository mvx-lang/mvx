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

/* JSON encode/decode for records (#24), built into the runtime (#169).
 *
 * A record maps to a JSON object driven by a mapping (the single mapper,
 * mvx_map.h): single-valued attributes become scalar keys, each association
 * becomes an array of objects (one per value position).  Keys are the mapped
 * field names, lowercased.  Conversions and typing come for free through
 * map_cell (out) / map_uncell (in); only mapped attributes round-trip. */

#include "mvx_map.h"
#include "mvx_ext.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ buffer */

typedef struct { char *p; size_t len, cap; } jbuf;

static void jb_reserve(jbuf *b, size_t add) {
    if (b->len + add <= b->cap) return;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + add) cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np) { free(b->p); b->p = NULL; b->cap = 0; b->len = 0; return; }
    b->p = np;
    b->cap = cap;
}

static void jb_raw(jbuf *b, const char *s, size_t n) {
    jb_reserve(b, n);
    if (!b->p) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* Append s (n bytes) as a JSON string literal (with quotes), escaping the
   control set, `"`/`\`, and non-ASCII bytes as \u00XX so output is valid,
   byte-exact-round-tripping JSON for MVX's Latin-1 data.  When `lower`, keys
   are lowercased first. */
static void jb_str(jbuf *b, const char *s, size_t n, int lower) {
    jb_raw(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (lower) c = (unsigned char)tolower(c);
        switch (c) {
        case '"':  jb_raw(b, "\\\"", 2); break;
        case '\\': jb_raw(b, "\\\\", 2); break;
        case '\b': jb_raw(b, "\\b", 2);  break;
        case '\f': jb_raw(b, "\\f", 2);  break;
        case '\n': jb_raw(b, "\\n", 2);  break;
        case '\r': jb_raw(b, "\\r", 2);  break;
        case '\t': jb_raw(b, "\\t", 2);  break;
        default:
            if (c < 0x20 || c >= 0x7f) {
                char u[7];
                snprintf(u, sizeof u, "\\u%04x", c);
                jb_raw(b, u, 6);
            } else {
                char ch = (char)c;
                jb_raw(b, &ch, 1);
            }
        }
    }
    jb_raw(b, "\"", 1);
}

/* Emit a mapped cell as a JSON value: NUMERIC -> the number verbatim; DATE/TIME
   and TEXT -> a quoted string; empty -> null (NUMERIC/DATE/TIME) or "" (TEXT). */
static void jb_value(jbuf *b, const char *type, const char *cell, int64_t cl) {
    int isnum = strcmp(type, "NUMERIC") == 0;
    int isdt = strcmp(type, "DATE") == 0 || strcmp(type, "TIME") == 0;
    if (cl <= 0) {
        if (isnum || isdt) jb_raw(b, "null", 4);
        else jb_raw(b, "\"\"", 2);
        return;
    }
    if (isnum) jb_raw(b, cell, (size_t)cl);
    else jb_str(b, cell, (size_t)cl, 0);
}

/* ------------------------------------------------------------------ parser */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jvtype;

typedef struct jval {
    jvtype t;
    char *s;                     /* STR (unescaped) / NUM (raw text) bytes */
    size_t slen;
    struct jval **items;         /* ARR elements, or OBJ values */
    char **keys;                 /* OBJ keys */
    size_t *klens;
    size_t n;                    /* element/pair count */
} jval;

static void jfree(jval *v) {
    if (!v) return;
    free(v->s);
    for (size_t i = 0; i < v->n; i++) {
        jfree(v->items ? v->items[i] : NULL);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->klens);
    free(v);
}

static const char *jws(const char *p, const char *e) {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static jval *jparse(const char **pp, const char *e);

/* Parse a JSON string body (after the opening quote), unescaping into a fresh
   buffer; leaves *pp after the closing quote.  Returns malloc'd bytes + len. */
static char *jstr(const char **pp, const char *e, size_t *outlen) {
    const char *p = *pp;
    jbuf b = {0};
    while (p < e && *p != '"') {
        if (*p == '\\' && p + 1 < e) {
            p++;
            char c = *p++;
            switch (c) {
            case '"':  jb_raw(&b, "\"", 1); break;
            case '\\': jb_raw(&b, "\\", 1); break;
            case '/':  jb_raw(&b, "/", 1);  break;
            case 'b':  jb_raw(&b, "\b", 1); break;
            case 'f':  jb_raw(&b, "\f", 1); break;
            case 'n':  jb_raw(&b, "\n", 1); break;
            case 'r':  jb_raw(&b, "\r", 1); break;
            case 't':  jb_raw(&b, "\t", 1); break;
            case 'u': {
                /* TWO RANGES.  \u00XX is a BYTE: jb_str above escapes every
                   non-ASCII byte that way, one per byte, so decoding it back to
                   a byte is what makes this codec byte-exact for MVX's Latin-1
                   data.  That half is unchanged.

                   Above U+00FF the escape cannot have come from jb_str -- it
                   never emits one -- so it came from a real JSON producer and
                   means a CODEPOINT.  It used to be truncated with `v & 0xff`,
                   turning U+2014 into byte 0x14, a control character; the
                   registry's package manifests carry \u2014 and it arrived as
                   one wrong byte.  Emitted as UTF-8 now, which is the same
                   split the portable BASIC codec makes (udt/JSONDECODE, QUNI),
                   so the two implementations agree on every input. */
                unsigned v = 0;
                for (int k = 0; k < 4 && p < e; k++, p++) {
                    char h = *p; v <<= 4;
                    if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                }
                /* A codepoint above U+FFFF arrives as a SURROGATE PAIR (a high
                   in D800-DBFF then a low in DC00-DFFF).  Encoding the halves
                   separately produces two characters that are not the one sent,
                   and neither is valid alone, so recombine first. */
                if (v >= 0xD800 && v <= 0xDBFF && (size_t)(e - p) >= 6
                    && p[0] == '\\' && p[1] == 'u') {
                    unsigned lo = 0; const char *q = p + 2; int k = 0;
                    for (; k < 4 && q < e; k++, q++) {
                        char h = *q; lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                        else break;
                    }
                    if (k == 4 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        v = 0x10000u + ((v - 0xD800u) << 10) + (lo - 0xDC00u);
                        p = q;
                    }
                }
                /* No byte produced here can be 252, 253 or 254 -- valid UTF-8
                   never uses them -- so a decoded value cannot forge a
                   subvalue, value or attribute mark. */
                if (v <= 0xFF) {
                    char ch = (char)v;
                    jb_raw(&b, &ch, 1);
                } else if (v < 0x800) {
                    char u2[2] = { (char)(0xC0 | (v >> 6)),
                                   (char)(0x80 | (v & 0x3F)) };
                    jb_raw(&b, u2, 2);
                } else if (v < 0x10000) {
                    char u3[3] = { (char)(0xE0 | (v >> 12)),
                                   (char)(0x80 | ((v >> 6) & 0x3F)),
                                   (char)(0x80 | (v & 0x3F)) };
                    jb_raw(&b, u3, 3);
                } else {
                    char u4[4] = { (char)(0xF0 | (v >> 18)),
                                   (char)(0x80 | ((v >> 12) & 0x3F)),
                                   (char)(0x80 | ((v >> 6) & 0x3F)),
                                   (char)(0x80 | (v & 0x3F)) };
                    jb_raw(&b, u4, 4);
                }
                break;
            }
            default: { char ch = c; jb_raw(&b, &ch, 1); }
            }
        } else {
            jb_raw(&b, p, 1);
            p++;
        }
    }
    if (p < e && *p == '"') p++;
    *pp = p;
    *outlen = b.len;
    if (!b.p) { b.p = malloc(1); if (b.p) b.p[0] = '\0'; }
    return b.p;
}

static jval *jparse(const char **pp, const char *e) {
    const char *p = jws(*pp, e);
    if (p >= e) { *pp = p; return NULL; }
    jval *v = calloc(1, sizeof *v);
    if (!v) { *pp = e; return NULL; }
    if (*p == '"') {
        p++;
        v->t = JV_STR;
        v->s = jstr(&p, e, &v->slen);
    } else if (*p == '{') {
        p++;
        v->t = JV_OBJ;
        p = jws(p, e);
        while (p < e && *p != '}') {
            p = jws(p, e);
            if (p >= e || *p != '"') break;
            p++;
            size_t kl;
            char *k = jstr(&p, e, &kl);
            p = jws(p, e);
            if (p < e && *p == ':') p++;
            jval *cv = jparse(&p, e);
            /* grow parallel arrays */
            char **nk = realloc(v->keys, (v->n + 1) * sizeof *nk);
            size_t *nl = realloc(v->klens, (v->n + 1) * sizeof *nl);
            jval **ni = realloc(v->items, (v->n + 1) * sizeof *ni);
            if (nk) v->keys = nk;
            if (nl) v->klens = nl;
            if (ni) v->items = ni;
            if (nk && nl && ni) {
                v->keys[v->n] = k;
                v->klens[v->n] = kl;
                v->items[v->n] = cv;
                v->n++;
            } else { free(k); jfree(cv); }
            p = jws(p, e);
            if (p < e && *p == ',') { p++; continue; }
            break;
        }
        p = jws(p, e);
        if (p < e && *p == '}') p++;
    } else if (*p == '[') {
        p++;
        v->t = JV_ARR;
        p = jws(p, e);
        while (p < e && *p != ']') {
            jval *cv = jparse(&p, e);
            jval **ni = realloc(v->items, (v->n + 1) * sizeof *ni);
            if (ni) { v->items = ni; v->items[v->n++] = cv; }
            else jfree(cv);
            p = jws(p, e);
            if (p < e && *p == ',') { p++; continue; }
            break;
        }
        p = jws(p, e);
        if (p < e && *p == ']') p++;
    } else if (strncmp(p, "true", 4) == 0) {
        v->t = JV_BOOL; p += 4;
    } else if (strncmp(p, "false", 5) == 0) {
        v->t = JV_BOOL; p += 5;
    } else if (strncmp(p, "null", 4) == 0) {
        v->t = JV_NULL; p += 4;
    } else {
        const char *s = p;
        while (p < e && (*p == '-' || *p == '+' || *p == '.' ||
                         (*p >= '0' && *p <= '9') || *p == 'e' || *p == 'E'))
            p++;
        v->t = JV_NUM;
        v->slen = (size_t)(p - s);
        v->s = malloc(v->slen + 1);
        if (v->s) { memcpy(v->s, s, v->slen); v->s[v->slen] = '\0'; }
    }
    *pp = p;
    return v;
}

static jval *jobj_get(jval *o, const char *key, size_t klen) {
    if (!o || o->t != JV_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++)
        if (o->klens[i] == klen && memcmp(o->keys[i], key, klen) == 0)
            return o->items[i];
    return NULL;
}

/* Lowercase a field name into buf (for keyed lookup, matching encode). */
static const char *lower_key(const char *s, size_t n, char *buf, size_t cap) {
    size_t i = 0;
    for (; i < n && i < cap - 1; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[i] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ public */

static void mvx_jsonencode(mvx_ctx *ctx, mv_value *dst, const mv_value *rec,
                    const mv_value *spec) {
    char sb[64];
    const char *sp;
    int64_t sl = mv_val_chars(spec, sb, sizeof sb, &sp);
    mapmeta m;
    memset(&m, 0, sizeof m);
    if (sl > 0) map_parse(sp, sl, &m);

    mv_value av, ov, code;
    mv_init(&av); mv_init(&ov); mv_init(&code);
    static char cell[8192];
    jbuf b = {0};
    jb_raw(&b, "{", 1);
    int first = 1;

    for (int i = 0; i < m.nf; i++) {
        if (m.assocs[i][0] != '\0') continue;
        if (!first) jb_raw(&b, ",", 1);
        first = 0;
        jb_str(&b, m.names[i], strlen(m.names[i]), 1);
        jb_raw(&b, ":", 1);
        int64_t cl = map_cell(ctx, rec, m.anos[i], 0, m.convs[i], m.types[i],
                              &av, &ov, &code, cell, sizeof cell);
        jb_value(&b, m.types[i], cell, cl);
    }

    char *an[MAP_MAXA];
    int am[MAP_MAXA][MAP_MAXF], anm[MAP_MAXA];
    int na = map_group_assoc(&m, an, am, anm);
    for (int a = 0; a < na; a++) {
        if (!first) jb_raw(&b, ",", 1);
        first = 0;
        jb_str(&b, an[a], strlen(an[a]), 1);
        jb_raw(&b, ":[", 2);
        int nv = 0;
        for (int k = 0; k < anm[a]; k++) {
            int vc = map_vcount(rec, m.anos[am[a][k]], &av);
            if (vc > nv) nv = vc;
        }
        for (int seq = 1; seq <= nv; seq++) {
            if (seq > 1) jb_raw(&b, ",", 1);
            jb_raw(&b, "{", 1);
            for (int k = 0; k < anm[a]; k++) {
                int i = am[a][k];
                if (k) jb_raw(&b, ",", 1);
                jb_str(&b, m.names[i], strlen(m.names[i]), 1);
                jb_raw(&b, ":", 1);
                int64_t cl = map_cell(ctx, rec, m.anos[i], seq, m.convs[i],
                                      m.types[i], &av, &ov, &code, cell,
                                      sizeof cell);
                jb_value(&b, m.types[i], cell, cl);
            }
            jb_raw(&b, "}", 1);
        }
        jb_raw(&b, "]", 1);
    }

    jb_raw(&b, "}", 1);
    mv_set_str(dst, b.p ? b.p : "{}", (int64_t)(b.p ? b.len : 2));
    free(b.p);
    free(m.buf);
    mv_clear(&av); mv_clear(&ov); mv_clear(&code);
}

static void mvx_jsondecode(mvx_ctx *ctx, mv_value *dst, const mv_value *json,
                    const mv_value *spec) {
    char sb[64];
    const char *sp;
    int64_t sl = mv_val_chars(spec, sb, sizeof sb, &sp);
    mapmeta m;
    memset(&m, 0, sizeof m);
    if (sl > 0) map_parse(sp, sl, &m);

    char jbf[64];
    const char *jp;
    int64_t jl = mv_val_chars(json, jbf, sizeof jbf, &jp);
    const char *cur = jp;
    jval *root = jparse(&cur, jp + jl);

    mv_set_str(dst, "", 0);
    mv_value val, tmp, code;
    mv_init(&val); mv_init(&tmp); mv_init(&code);
    char kbuf[128];

    for (int i = 0; root && root->t == JV_OBJ && i < m.nf; i++) {
        if (m.assocs[i][0] != '\0') continue;
        const char *k = lower_key(m.names[i], strlen(m.names[i]), kbuf,
                                  sizeof kbuf);
        jval *v = jobj_get(root, k, strlen(k));
        if (!v || v->t == JV_NULL) continue;
        map_uncell(ctx, m.types[i], m.convs[i], v->s, (int64_t)v->slen,
                   &val, &tmp, &code);
        mv_replace_fn(dst, dst, m.anos[i], 0, 0, &val);
    }

    if (root && root->t == JV_OBJ) {
        char *an[MAP_MAXA];
        int am[MAP_MAXA][MAP_MAXF], anm[MAP_MAXA];
        int na = map_group_assoc(&m, an, am, anm);
        for (int a = 0; a < na; a++) {
            const char *k = lower_key(an[a], strlen(an[a]), kbuf, sizeof kbuf);
            jval *arr = jobj_get(root, k, strlen(k));
            if (!arr || arr->t != JV_ARR) continue;
            for (size_t r = 0; r < arr->n; r++) {
                jval *elem = arr->items[r];
                if (!elem || elem->t != JV_OBJ) continue;
                for (int kk = 0; kk < anm[a]; kk++) {
                    int i = am[a][kk];
                    const char *mk = lower_key(m.names[i], strlen(m.names[i]),
                                               kbuf, sizeof kbuf);
                    jval *mv = jobj_get(elem, mk, strlen(mk));
                    if (!mv || mv->t == JV_NULL) continue;
                    map_uncell(ctx, m.types[i], m.convs[i], mv->s,
                               (int64_t)mv->slen, &val, &tmp, &code);
                    mv_replace_fn(dst, dst, m.anos[i], (int64_t)r + 1, 0, &val);
                }
            }
        }
    }

    jfree(root);
    free(m.buf);
    mv_clear(&val); mv_clear(&tmp); mv_clear(&code);
}

/* -------------------------------------------------------- extension table */
/* The package's BASIC-callable surface (#54): JSONENCODE / JSONDECODE become
   expression functions dispatched through the runtime extension registry.  The
   result slot is `ret`; the record/json and the mapping are argv[0]/argv[1]. */

static void ext_jsonencode(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                           mv_value **argv) {
    (void)argc;
    mvx_jsonencode(ctx, ret, argv[0], argv[1]);
}

static void ext_jsondecode(mvx_ctx *ctx, mv_value *ret, int32_t argc,
                           mv_value **argv) {
    (void)argc;
    mvx_jsondecode(ctx, ret, argv[0], argv[1]);
}

static const mvx_extfn json_fns[] = {
    {"JSONENCODE", 2, 2, ext_jsonencode},
    {"JSONDECODE", 2, 2, ext_jsondecode},
};
static const mvx_ext json_ext = {"json", 2, json_fns};

/* BUILT IN, not dlopen'd (#169).  This was an extension library shipped by the
   json package, which mvx carried as a submodule and built into the system
   account -- so JSONENCODE/JSONDECODE existed only because that submodule was
   checked out.  The codec now lives in libmvxrt and registers itself, so the
   names are always available: no package, no LIB/ lookup, nothing to link.
   The interface is unchanged -- the same two 2-argument expression functions
   the pure-BASIC arm presents on udt/uv/jbase, so `J = JSONENCODE(R, SPEC)`
   means the same thing everywhere. */
const mvx_ext *mvx_json_builtin(void) { return &json_ext; }
