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

/* Records as documents (#157) — the unmapped representation.  See mvx_doc.h
   for the shape and why it is that shape. */

#include "mvx_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The marks.  mv_dyn.c keeps its own file-local copies of these; they are the
   wire format, not an implementation detail either file owns. */
#define AM ((char)0xFE)
#define VM ((char)0xFD)
#define SM ((char)0xFC)

/* ------------------------------------------------------------------ buffer */

typedef struct { char *p; size_t len, cap; } dbuf;

static int db_reserve(dbuf *b, size_t add) {
    if (b->len + add <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + add) cap *= 2;
    char *np = realloc(b->p, cap);
    if (!np) { free(b->p); b->p = NULL; b->cap = b->len = 0; return 0; }
    b->p = np; b->cap = cap; return 1;
}

static void db_raw(dbuf *b, const char *s, size_t n) {
    if (!db_reserve(b, n)) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
}

/* ------------------------------------------------------------------- leaves */

/* Is s..n well-formed UTF-8?  This is what decides a value's representation,
   and nothing else does — which is what makes the round trip deterministic:
   the same bytes always encode the same way, whatever record they are in.
   Rejects overlong forms, surrogates and > U+10FFFF, so "valid" here means
   what a backend's JSON parser will also accept. */
static int doc_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; ) {
        unsigned char c = s[i];
        size_t need;
        unsigned long cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; }
        else return 0;                       /* continuation or 5+ byte lead */
        if (i + need >= n) return 0;                     /* truncated */
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (need == 1 && cp < 0x80) return 0;            /* overlong */
        if (need == 2 && cp < 0x800) return 0;
        if (need == 3 && cp < 0x10000) return 0;
        if (cp > 0x10FFFF) return 0;
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;      /* lone surrogate */
        i += need + 1;
    }
    return 1;
}

/* A JSON string literal.  Valid UTF-8 passes through as ITS OWN BYTES, escaping
   only what JSON requires — the point is that the backend sees natural text it
   can compare and index, so escaping every non-ASCII byte to \u00XX (which is
   what the JSONENCODE codec does for Latin-1 fidelity) would defeat the
   exercise. */
static void db_str(dbuf *b, const char *s, size_t n) {
    db_raw(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  db_raw(b, "\\\"", 2); break;
        case '\\': db_raw(b, "\\\\", 2); break;
        case '\b': db_raw(b, "\\b", 2);  break;
        case '\f': db_raw(b, "\\f", 2);  break;
        case '\n': db_raw(b, "\\n", 2);  break;
        case '\r': db_raw(b, "\\r", 2);  break;
        case '\t': db_raw(b, "\\t", 2);  break;
        default:
            if (c < 0x20) {                  /* JSON's own escaping covers these,
                                                so control characters stay text */
                char u[7];
                snprintf(u, sizeof u, "\\u%04x", c);
                db_raw(b, u, 6);
            } else {
                char ch = (char)c;
                db_raw(b, &ch, 1);
            }
        }
    }
    db_raw(b, "\"", 1);
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void db_b64(dbuf *b, const char *s, size_t n) {
    db_raw(b, "{\"$b64\":\"", 9);
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    char q[4];
    for (; i + 2 < n; i += 3) {
        unsigned long v = ((unsigned long)u[i] << 16) | (u[i+1] << 8) | u[i+2];
        q[0] = B64[(v >> 18) & 63]; q[1] = B64[(v >> 12) & 63];
        q[2] = B64[(v >> 6) & 63];  q[3] = B64[v & 63];
        db_raw(b, q, 4);
    }
    if (i < n) {
        unsigned long v = (unsigned long)u[i] << 16;
        if (i + 1 < n) v |= (unsigned long)u[i+1] << 8;
        q[0] = B64[(v >> 18) & 63]; q[1] = B64[(v >> 12) & 63];
        q[2] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        q[3] = '=';
        db_raw(b, q, 4);
    }
    db_raw(b, "\"}", 2);
}

static int b64val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* base64 -> bytes appended to b.  Returns 0 on any character that is not
   base64, so a malformed document is rejected rather than silently decoded to
   something shorter than it should be. */
static int db_unb64(dbuf *b, const char *s, size_t n) {
    unsigned long acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '=') break;
        int v = b64val(c);
        if (v < 0) return 0;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            char out = (char)((acc >> bits) & 0xFF);
            db_raw(b, &out, 1);
        }
    }
    return 1;
}

/* One leaf value: text when the bytes are valid UTF-8, otherwise wrapped. */
static void db_leaf(dbuf *b, const char *s, size_t n) {
    if (doc_utf8((const unsigned char *)s, n)) db_str(b, s, n);
    else db_b64(b, s, n);
}

/* --------------------------------------------------------------- encode */

/* Trailing empties carry no alignment, so trim them; interior and leading ones
   are exactly what holds the positions apart and are kept. */
static int64_t trim_trailing(const char *s, int64_t n, char delim) {
    while (n > 0 && s[n - 1] == delim) n--;
    return n;
}

/* Emit one multivalue's worth: subvalues become a nested list, a single
   subvalue stays a scalar. */
static void enc_value(dbuf *b, const char *s, int64_t n) {
    const char *sv = memchr(s, SM, (size_t)n);
    if (!sv) { db_leaf(b, s, (size_t)n); return; }
    int64_t m = trim_trailing(s, n, SM);
    db_raw(b, "[", 1);
    int64_t start = 0, i = 0;
    int first = 1;
    for (i = 0; i <= m; i++) {
        if (i == m || s[i] == SM) {
            if (!first) db_raw(b, ",", 1);
            first = 0;
            db_leaf(b, s + start, (size_t)(i - start));
            start = i + 1;
        }
    }
    db_raw(b, "]", 1);
}

static void enc_attr(dbuf *b, const char *s, int64_t n) {
    const char *vm = memchr(s, VM, (size_t)n);
    if (!vm) {
        /* One value — and scalar-when-one applies only if that value is a LEAF.
           A single value that has subvalues must keep the outer list, or it
           encodes identically to several single-subvalue values:

               x SM SM z   one value, three subvalues
               x VM SM z   three values                 <- different record

           both collapsing to ["x","","z"].  MV tells them apart (A<1,1,3> vs
           A<1,3>), so the document has to as well.  With the list kept, depth
           is the level: a list at depth 0 is values, at depth 1 subvalues. */
        if (!memchr(s, SM, (size_t)n)) { db_leaf(b, s, (size_t)n); return; }
        db_raw(b, "[", 1);
        enc_value(b, s, n);
        db_raw(b, "]", 1);
        return;
    }
    int64_t m = trim_trailing(s, n, VM);
    db_raw(b, "[", 1);
    int64_t start = 0, i = 0;
    int first = 1;
    for (i = 0; i <= m; i++) {
        if (i == m || s[i] == VM) {
            if (!first) db_raw(b, ",", 1);
            first = 0;
            enc_value(b, s + start, i - start);
            start = i + 1;
        }
    }
    db_raw(b, "]", 1);
}

void mvx_doc_encode(mv_value *dst, const mv_value *rec) {
    char nb[64];
    const char *p;
    int64_t n = mv_val_chars(rec, nb, sizeof nb, &p);
    dbuf b = {0};
    db_raw(&b, "{", 1);
    if (n > 0) {
        /* An absent attribute and an empty one read identically in MV, so a
           trailing empty attribute need not be recorded. */
        int64_t m = trim_trailing(p, n, AM);
        int64_t start = 0, i;
        int first = 1, ano = 1;
        for (i = 0; i <= m; i++) {
            if (i == m || p[i] == AM) {
                int64_t len = i - start;
                if (len > 0) {                    /* omit empties: same meaning */
                    char key[24];
                    int kl = snprintf(key, sizeof key, "%d", ano);
                    if (!first) db_raw(&b, ",", 1);
                    first = 0;
                    db_str(&b, key, (size_t)kl);
                    db_raw(&b, ":", 1);
                    enc_attr(&b, p + start, len);
                }
                start = i + 1;
                ano++;
            }
        }
    }
    db_raw(&b, "}", 1);
    mv_set_str(dst, b.p ? b.p : "{}", (int64_t)(b.p ? b.len : 2));
    free(b.p);
}

/* --------------------------------------------------------------- decode */

/* A minimal reader over the document text.  Deliberately not the JSONENCODE
   parser: that one builds a tree of jvals, and here the structure is known
   (object of leaves / lists / lists of lists), so streaming straight into the
   record avoids allocating a parallel copy of every record read. */
typedef struct { const char *p, *e; int ok; } drd;

static void d_ws(drd *r) {
    while (r->p < r->e && (*r->p == ' ' || *r->p == '\t' ||
                           *r->p == '\n' || *r->p == '\r')) r->p++;
}

static int d_eat(drd *r, char c) {
    d_ws(r);
    if (r->p < r->e && *r->p == c) { r->p++; return 1; }
    return 0;
}

static void d_fail(drd *r) { r->ok = 0; }

/* A JSON string body into b (no quotes).  \uXXXX is emitted as UTF-8, which is
   the inverse of db_str: it escapes only control characters, so anything else
   arriving as \u came from a hand-written document and is still meant as text. */
static int d_string(drd *r, dbuf *b) {
    if (!d_eat(r, '"')) return 0;
    while (r->p < r->e && *r->p != '"') {
        if (*r->p == '\\') {
            r->p++;
            if (r->p >= r->e) return 0;
            char c = *r->p++;
            char out;
            switch (c) {
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case '/': out = '/'; break;
            case 'b': out = '\b'; break;
            case 'f': out = '\f'; break;
            case 'n': out = '\n'; break;
            case 'r': out = '\r'; break;
            case 't': out = '\t'; break;
            case 'u': {
                if (r->e - r->p < 4) return 0;
                unsigned long cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = *r->p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return 0;
                }
                char u[4];
                int ul = 0;
                if (cp < 0x80) u[ul++] = (char)cp;
                else if (cp < 0x800) {
                    u[ul++] = (char)(0xC0 | (cp >> 6));
                    u[ul++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    u[ul++] = (char)(0xE0 | (cp >> 12));
                    u[ul++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    u[ul++] = (char)(0x80 | (cp & 0x3F));
                }
                db_raw(b, u, (size_t)ul);
                continue;
            }
            default: return 0;
            }
            db_raw(b, &out, 1);
            continue;
        }
        db_raw(b, r->p, 1);
        r->p++;
    }
    if (r->p >= r->e) return 0;
    r->p++;                                   /* closing quote */
    return 1;
}

/* A leaf: a string, or the {"$b64":"..."} wrapper.  An object at a leaf can
   only mean "encoded" — that is what makes the wrapper unambiguous against a
   literal value that happens to look like base64. */
static int d_leaf(drd *r, dbuf *out) {
    d_ws(r);
    if (r->p < r->e && *r->p == '{') {
        r->p++;
        dbuf key = {0}, val = {0};
        int ok = d_string(r, &key) && d_eat(r, ':') && d_string(r, &val) &&
                 d_eat(r, '}') &&
                 key.len == 4 && memcmp(key.p, "$b64", 4) == 0 &&
                 db_unb64(out, val.p ? val.p : "", val.len);
        free(key.p); free(val.p);
        if (!ok) { d_fail(r); return 0; }
        return 1;
    }
    if (!d_string(r, out)) { d_fail(r); return 0; }
    return 1;
}

/* One attribute's worth of document into the record buffer, joining nested
   lists with VM / SM.  depth 0 = the attribute (VM), depth 1 = a value (SM). */
static void d_level(drd *r, dbuf *out, int depth) {
    d_ws(r);
    if (r->p < r->e && *r->p == '[') {
        if (depth > 1) { d_fail(r); return; }   /* MV has three levels, no more */
        r->p++;
        d_ws(r);
        int first = 1;
        char sep = depth == 0 ? VM : SM;
        if (r->p < r->e && *r->p == ']') { r->p++; return; }
        for (;;) {
            if (!first) db_raw(out, &sep, 1);
            first = 0;
            d_level(r, out, depth + 1);
            if (!r->ok) return;
            d_ws(r);
            if (d_eat(r, ',')) continue;
            if (d_eat(r, ']')) return;
            d_fail(r);
            return;
        }
    }
    d_leaf(r, out);
}

void mvx_doc_decode(mv_value *dst, const mv_value *doc) {
    char nb[64];
    const char *p;
    int64_t n = mv_val_chars(doc, nb, sizeof nb, &p);
    drd r = { p, p + (n > 0 ? n : 0), 1 };

    /* Attributes are collected by ordinal and joined at the end, so a document
       whose keys are out of order (nothing forbids it) still rebuilds the
       record with each value at its own attribute. */
    dbuf *slot = NULL;
    int nslot = 0, cap = 0;

    if (!d_eat(&r, '{')) { mv_set_str(dst, "", 0); return; }
    d_ws(&r);
    if (!(r.p < r.e && *r.p == '}')) {
        for (;;) {
            dbuf key = {0};
            if (!d_string(&r, &key)) { free(key.p); r.ok = 0; break; }
            if (!d_eat(&r, ':')) { free(key.p); r.ok = 0; break; }
            long ano = 0;
            int good = key.len > 0;
            for (size_t i = 0; i < key.len; i++) {
                if (key.p[i] < '0' || key.p[i] > '9') { good = 0; break; }
                ano = ano * 10 + (key.p[i] - '0');
            }
            free(key.p);
            if (!good || ano < 1 || ano > 1000000) { r.ok = 0; break; }
            if (ano > cap) {
                int nc = cap ? cap : 8;
                while (nc < ano) nc *= 2;
                dbuf *ns = realloc(slot, (size_t)nc * sizeof *ns);
                if (!ns) { r.ok = 0; break; }
                memset(ns + cap, 0, (size_t)(nc - cap) * sizeof *ns);
                slot = ns; cap = nc;
            }
            if (ano > nslot) nslot = (int)ano;
            d_level(&r, &slot[ano - 1], 0);
            if (!r.ok) break;
            if (d_eat(&r, ',')) continue;
            if (d_eat(&r, '}')) break;
            r.ok = 0;
            break;
        }
    } else r.p++;

    dbuf rec = {0};
    if (r.ok) {
        for (int i = 0; i < nslot; i++) {
            if (i) db_raw(&rec, (const char[]){AM}, 1);
            if (slot[i].p) db_raw(&rec, slot[i].p, slot[i].len);
        }
    }
    mv_set_str(dst, rec.p ? rec.p : "", (int64_t)(rec.p ? rec.len : 0));
    free(rec.p);
    for (int i = 0; i < cap; i++) free(slot[i].p);
    free(slot);
}
