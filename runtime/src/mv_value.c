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

#include "mvx_runtime.h"
#include "mv_intern.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- fatal */

void mvx_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("mvx runtime: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(70); /* EX_SOFTWARE */
}

void mvx_stop(void) {
    exit(0);
}

/* STOP <code> — end the program with a process exit status (a verb like CHECK
   uses it to gate CI).  Clamp to a byte, the portable exit-code range. */
void mvx_exit(int32_t code) {
    exit(code & 0xFF);
}

void mvx_arity_check(const char *name, int32_t expected, int32_t got) {
    if (expected != got)
        mvx_fatal("CALL %s: %d argument(s) passed, subroutine takes %d",
                  name, got, expected);
}

/* -------------------------------------------------------------- strings */

static mv_string *str_alloc_cap(int64_t len, int64_t cap) {
    if (cap < len) cap = len;
    mv_string *s = malloc(sizeof(mv_string) + (size_t)cap + 1);
    if (!s) mvx_fatal("out of memory allocating string of length %lld",
                      (long long)len);
    s->refs = 1;
    s->len  = len;
    s->cap  = cap;
    s->ix   = NULL;
    s->data[len] = '\0';
    return s;
}

static mv_string *str_alloc(int64_t len) { return str_alloc_cap(len, len); }

mv_string *mvx_str_alloc_cap(int64_t len, int64_t cap) {
    return str_alloc_cap(len, cap);
}

static void str_release(mv_string *s) {
    if (s && --s->refs == 0) { mvx_ix_drop(s); free(s); }
}

/* ------------------------------------------------------- value lifecycle */

void mv_init(mv_value *v) {
    v->tag = MV_UNASSIGNED;
    v->i = 0;
    v->d = 0.0;
    v->s = NULL;
}

void mv_clear(mv_value *v) {
    if (v->tag == MV_STR) str_release(v->s);
    mv_init(v);
}

void mv_set_int(mv_value *v, int64_t i) {
    if (v->tag == MV_STR) str_release(v->s);
    v->tag = MV_INT;
    v->i = i;
    v->s = NULL;
}

void mv_set_dbl(mv_value *v, double d) {
    if (v->tag == MV_STR) str_release(v->s);
    v->tag = MV_DBL;
    v->d = d;
    v->s = NULL;
}

/* Reuse the value's own buffer whenever it owns it outright and the bytes
   fit.  Dynamic-array work rewrites the same variable over and over, so this
   is the difference between an allocation per operation and none at all.
   memmove rather than memcpy: p may point into the buffer being written,
   as in X = X<1,2>. */
void mv_set_str(mv_value *v, const char *p, int64_t len) {
    if (v->tag == MV_STR && v->s->refs == 1 && v->s->cap >= len) {
        mv_string *s = v->s;
        memmove(s->data, p, (size_t)len);
        s->len = len;
        s->data[len] = '\0';
        mvx_ix_drop(s);
        return;
    }
    mv_string *s = str_alloc(len);
    memcpy(s->data, p, (size_t)len);
    if (v->tag == MV_STR) str_release(v->s);
    v->tag = MV_STR;
    v->s = s;
}

void mv_copy(mv_value *dst, const mv_value *src) {
    if (dst == src) return;
    if (src->tag == MV_STR) src->s->refs++;
    if (dst->tag == MV_STR) str_release(dst->s);
    *dst = *src;
}

/* -------------------------------------------------------------- coercion */

static void warn_unassigned(void) {
    static int warned = 0;
    if (!warned) {
        fputs("mvx runtime: warning: unassigned variable used, zero assumed\n",
              stderr);
        warned = 1;
    }
}

/* Does the string parse fully as a number?  MV numeric-string rules:
   optional sign, digits, optional decimal part.  Leading/trailing blanks
   are not numeric in jBASE comparison semantics. */
static int str_as_num(const mv_string *s, double *out_d, int64_t *out_i,
                      int *is_int) {
    if (!s || s->len == 0) return 0;
    char *end = NULL;
    const char *p = s->data;
    long long iv = strtoll(p, &end, 10);
    if (end == p + s->len) { *out_i = iv; *is_int = 1; *out_d = (double)iv; return 1; }
    end = NULL;
    double dv = strtod(p, &end);
    if (end == p + s->len) { *out_d = dv; *is_int = 0; return 1; }
    return 0;
}

int64_t mv_get_int(const mv_value *v) {
    switch (v->tag) {
    case MV_INT: return v->i;
    case MV_DBL: return (int64_t)v->d;
    case MV_STR: {
        double d; int64_t i; int is_int;
        if (str_as_num(v->s, &d, &i, &is_int)) return is_int ? i : (int64_t)d;
        mvx_fatal("non-numeric value \"%s\" used in numeric context", v->s->data);
    }
    default: warn_unassigned(); return 0;
    }
}

double mv_get_dbl(const mv_value *v) {
    switch (v->tag) {
    case MV_INT: return (double)v->i;
    case MV_DBL: return v->d;
    case MV_STR: {
        double d; int64_t i; int is_int;
        if (str_as_num(v->s, &d, &i, &is_int)) return d;
        mvx_fatal("non-numeric value \"%s\" used in numeric context", v->s->data);
    }
    default: warn_unassigned(); return 0.0;
    }
}

int64_t mv_truth(const mv_value *v) {
    switch (v->tag) {
    case MV_INT: return v->i != 0;
    case MV_DBL: return v->d != 0.0;
    case MV_STR: {
        double d; int64_t i; int is_int;
        if (str_as_num(v->s, &d, &i, &is_int))
            return is_int ? i != 0 : d != 0.0;
        return v->s->len != 0;   /* non-numeric, non-empty string is true */
    }
    default: warn_unassigned(); return 0;
    }
}

/* ------------------------------------------------------------ arithmetic */

/* Fast path: both ints stay int (with overflow promotion to double). */
#define ARITH_BODY(name, int_expr, dbl_op)                                   \
    void name(mv_value *dst, const mv_value *a, const mv_value *b) {         \
        if (a->tag == MV_INT && b->tag == MV_INT) {                          \
            int64_t x = a->i, y = b->i, r;                                   \
            int_expr;                                                        \
            mv_set_int(dst, r);                                              \
            return;                                                          \
        }                                                                    \
        mv_set_dbl(dst, mv_get_dbl(a) dbl_op mv_get_dbl(b));                 \
    }

ARITH_BODY(mv_add,
           if (__builtin_add_overflow(x, y, &r)) {
               mv_set_dbl(dst, (double)x + (double)y); return; }, +)
ARITH_BODY(mv_sub,
           if (__builtin_sub_overflow(x, y, &r)) {
               mv_set_dbl(dst, (double)x - (double)y); return; }, -)
ARITH_BODY(mv_mul,
           if (__builtin_mul_overflow(x, y, &r)) {
               mv_set_dbl(dst, (double)x * (double)y); return; }, *)

void mv_div(mv_value *dst, const mv_value *a, const mv_value *b) {
    double denom = mv_get_dbl(b);
    if (denom == 0.0) mvx_fatal("division by zero");
    if (a->tag == MV_INT && b->tag == MV_INT && a->i % b->i == 0) {
        mv_set_int(dst, a->i / b->i);
        return;
    }
    mv_set_dbl(dst, mv_get_dbl(a) / denom);
}

void mv_pow(mv_value *dst, const mv_value *a, const mv_value *b) {
    mv_set_dbl(dst, pow(mv_get_dbl(a), mv_get_dbl(b)));
}

void mv_neg(mv_value *dst, const mv_value *a) {
    if (a->tag == MV_INT) mv_set_int(dst, -a->i);
    else                  mv_set_dbl(dst, -mv_get_dbl(a));
}

/* --------------------------------------------------------- stringification */

/* MV numeric formatting: integers plain; doubles trimmed of trailing
   zeros, PRECISION 4 by default (classic Pick). */
static int64_t num_to_buf(const mv_value *v, char *buf, size_t cap) {
    if (v->tag == MV_INT)
        return (int64_t)snprintf(buf, cap, "%lld", (long long)v->i);
    double d = v->d;
    if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15)
        return (int64_t)snprintf(buf, cap, "%lld", (long long)d);
    int64_t n = (int64_t)snprintf(buf, cap, "%.4f", d);
    while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
    if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
    return n;
}

/* Borrow a string view of v.  Returns pointer + len; uses caller's buf
   for numeric tags. */
static const char *val_chars(const mv_value *v, char *buf, size_t cap,
                             int64_t *len) {
    switch (v->tag) {
    case MV_STR: *len = v->s->len; return v->s->data;
    case MV_INT:
    case MV_DBL: *len = num_to_buf(v, buf, cap); return buf;
    default:     warn_unassigned(); *len = 0; return "";
    }
}

int64_t mv_val_chars(const mv_value *v, char *numbuf, size_t cap,
                     const char **out) {
    int64_t len;
    *out = val_chars(v, numbuf, cap, &len);
    return len;
}

void mv_cat(mv_value *dst, const mv_value *a, const mv_value *b) {
    char ba[40], bb[40];
    int64_t la, lb;
    const char *pa = val_chars(a, ba, sizeof ba, &la);
    const char *pb = val_chars(b, bb, sizeof bb, &lb);
    /* S = S : X against a buffer this value owns and that already has the
       room: append in place.  With the headroom below, building a record up
       one field at a time stops being quadratic. */
    if (dst == a && dst->tag == MV_STR && dst->s->refs == 1 &&
        pa == dst->s->data && dst->s->cap >= la + lb) {
        mv_string *s = dst->s;
        memmove(s->data + la, pb, (size_t)lb);
        s->len = la + lb;
        s->data[s->len] = '\0';
        mvx_ix_drop(s);
        return;
    }
    int64_t need = la + lb;
    mv_string *s = str_alloc_cap(need, need + need / 2 + 16);
    memcpy(s->data, pa, (size_t)la);
    memcpy(s->data + la, pb, (size_t)lb);
    if (dst->tag == MV_STR) str_release(dst->s);
    dst->tag = MV_STR;
    dst->s = s;
}

/* ------------------------------------------------------------- comparison */

int64_t mv_compare(const mv_value *a, const mv_value *b) {
    /* Numeric compare when both sides are numeric (including numeric
       strings) — MV semantics. */
    double da, db;
    int num_a = 1, num_b = 1;

    switch (a->tag) {
    case MV_INT: da = (double)a->i; break;
    case MV_DBL: da = a->d; break;
    case MV_STR: { int64_t i; int ii;
        num_a = str_as_num(a->s, &da, &i, &ii); break; }
    default: warn_unassigned(); da = 0; break;
    }
    switch (b->tag) {
    case MV_INT: db = (double)b->i; break;
    case MV_DBL: db = b->d; break;
    case MV_STR: { int64_t i; int ii;
        num_b = str_as_num(b->s, &db, &i, &ii); break; }
    default: warn_unassigned(); db = 0; break;
    }

    if (num_a && num_b)
        return (da > db) - (da < db);

    char ba[40], bb[40];
    int64_t la, lb;
    const char *pa = val_chars(a, ba, sizeof ba, &la);
    const char *pb = val_chars(b, bb, sizeof bb, &lb);
    int64_t min = la < lb ? la : lb;
    int c = memcmp(pa, pb, (size_t)min);
    if (c) return c;
    return (la > lb) - (la < lb);
}

/* --------------------------------------------------------------- arrays */

struct mv_array {
    int64_t  d1, d2;      /* d2 == 0 → one-dimensional */
    mv_value elems[];
};

mv_array *mv_arr_create(int64_t d1, int64_t d2) {
    if (d1 < 1 || d2 < 0)
        mvx_fatal("DIM with non-positive dimension (%lld, %lld)",
                  (long long)d1, (long long)d2);
    int64_t n = d1 * (d2 ? d2 : 1);
    mv_array *a = malloc(sizeof(mv_array) + (size_t)n * sizeof(mv_value));
    if (!a) mvx_fatal("out of memory in DIM(%lld,%lld)",
                      (long long)d1, (long long)d2);
    a->d1 = d1;
    a->d2 = d2;
    /* MV initialises DIM'd elements to empty string / zero; unassigned
       with quiet zero-coercion matches, without a malloc per element. */
    memset(a->elems, 0, (size_t)n * sizeof(mv_value));
    return a;
}

void mv_arr_destroy(mv_array *a) {
    if (!a) return;
    int64_t n = a->d1 * (a->d2 ? a->d2 : 1);
    for (int64_t k = 0; k < n; k++)
        if (a->elems[k].tag == MV_STR) str_release(a->elems[k].s);
    free(a);
}

void mv_arr_fill(mv_array *a, const mv_value *val) {
    int64_t n = a->d1 * (a->d2 ? a->d2 : 1);
    for (int64_t k = 0; k < n; k++)
        mv_copy(&a->elems[k], val);
}

void mv_arr_copy(mv_array *dst, const mv_array *src) {
    if (dst->d1 != src->d1 || dst->d2 != src->d2)
        mvx_fatal("MAT copy between arrays of different dimensions "
                  "(%lld,%lld) and (%lld,%lld)",
                  (long long)dst->d1, (long long)dst->d2,
                  (long long)src->d1, (long long)src->d2);
    int64_t n = dst->d1 * (dst->d2 ? dst->d2 : 1);
    for (int64_t k = 0; k < n; k++)
        mv_copy(&dst->elems[k], &src->elems[k]);
}

/* Silent char view of an array element (val_chars warns on unassigned,
   which DIM elements legitimately are). */
static const char *elem_chars(const mv_value *v, char *buf, size_t cap,
                              int64_t *len) {
    switch (v->tag) {
    case MV_STR: *len = v->s->len; return v->s->data;
    case MV_INT:
    case MV_DBL: *len = num_to_buf(v, buf, cap); return buf;
    default:     *len = 0; return "";
    }
}

/* First occurrence of the dl-byte delimiter d in [cur,end), or NULL. */
static const char *find_bytes(const char *cur, const char *end,
                              const char *d, int64_t dl) {
    for (const char *p = cur; p + dl <= end; p++)
        if (memcmp(p, d, (size_t)dl) == 0) return p;
    return NULL;
}

/* Delimiter bytes from an optional value; NULL or empty means a single
   @FM (0xFE).  buf backs a numeric delimiter's rendering. */
static const char *delim_chars(const mv_value *delim, char *buf, size_t cap,
                               int64_t *dl) {
    static const char fm = (char)0xFE;
    if (delim) {
        const char *d = val_chars(delim, buf, cap, dl);
        if (*dl > 0) return d;
    }
    *dl = 1;
    return &fm;
}

/* Split `src` on `delim` (default @FM) across the array's elements in
   order; the last element absorbs any overflow, short input leaves
   trailing elements empty.  Drives MATREAD and the MATPARSE statement. */
void mv_mat_parse(mv_array *a, const mv_value *src, const mv_value *delim) {
    int64_t n = a->d1 * (a->d2 ? a->d2 : 1);
    char nb[40], db[40];
    int64_t len, dl;
    const char *p = val_chars(src, nb, sizeof nb, &len);
    const char *d = delim_chars(delim, db, sizeof db, &dl);
    const char *cur = p, *end = p + len;
    for (int64_t k = 0; k < n; k++) {
        const char *fs = cur, *fe;
        if (k == n - 1) { fe = end; cur = end; }
        else {
            const char *m = find_bytes(cur, end, d, dl);
            fe = m ? m : end;
            cur = m ? m + dl : end;
        }
        mv_set_str(&a->elems[k], fs, fe - fs);
    }
}

/* Join the array's elements with `delim` (default @FM), then strip any
   trailing delimiter runs (so trailing empty elements — and a last
   element that ends in the delimiter — fall away).  Drives MATWRITE and
   the MATBUILD statement. */
void mv_mat_build(const mv_array *a, mv_value *dst, const mv_value *delim) {
    int64_t n = a->d1 * (a->d2 ? a->d2 : 1);
    if (n < 1) n = 1;
    char b[40], db[40];
    const char *p;
    int64_t l, dl;
    const char *d = delim_chars(delim, db, sizeof db, &dl);
    int64_t total = (n - 1) * dl;
    for (int64_t k = 0; k < n; k++) {
        elem_chars(&a->elems[k], b, sizeof b, &l);
        total += l;
    }
    mv_string *s = str_alloc(total);
    int64_t off = 0;
    for (int64_t k = 0; k < n; k++) {
        if (k) { memcpy(s->data + off, d, (size_t)dl); off += dl; }
        p = elem_chars(&a->elems[k], b, sizeof b, &l);
        memcpy(s->data + off, p, (size_t)l);
        off += l;
    }
    while (off >= dl && memcmp(s->data + off - dl, d, (size_t)dl) == 0)
        off -= dl;
    s->len = off;
    s->data[off] = '\0';
    if (dst->tag == MV_STR) str_release(dst->s);
    dst->tag = MV_STR;
    dst->s = s;
}

/* @FM record split/join used by MATREAD/MATWRITE. */
void mv_matparse(mv_array *a, const mv_value *rec) {
    mv_mat_parse(a, rec, NULL);
}
void mv_matbuild(const mv_array *a, mv_value *dst) {
    mv_mat_build(a, dst, NULL);
}

mv_value *mv_arr_elem(mv_array *a, int64_t i, int64_t j) {
    if (a->d2 == 0) {
        if (i < 1 || i > a->d1 || j != 0)
            mvx_fatal("array subscript (%lld) out of range 1..%lld",
                      (long long)i, (long long)a->d1);
        return &a->elems[i - 1];
    }
    if (i < 1 || i > a->d1 || j < 1 || j > a->d2)
        mvx_fatal("array subscript (%lld,%lld) out of range (1..%lld,1..%lld)",
                  (long long)i, (long long)j, (long long)a->d1, (long long)a->d2);
    return &a->elems[(i - 1) * a->d2 + (j - 1)];
}
