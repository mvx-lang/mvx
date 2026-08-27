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

/* Dynamic-array operations: extract / replace / insert / delete /
 * locate over attribute (0xFE), value (0xFD), and subvalue (0xFC)
 * marks, plus LEN / COUNT / DCOUNT.
 */
#include "mvx_runtime.h"
#include "mv_intern.h"
#include "mv_bytes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AM ((char)0xFE)
#define VM ((char)0xFD)
#define SM ((char)0xFC)

typedef struct { const char *p; int64_t len; } span;

/* String view of a value; numeric tags render into buf. */
/* mv_itoa64 — an int64 as decimal, without snprintf.
 *
 * WHY THIS EXISTS.  Every dynamic-array edit converts its value to characters
 * first, and a numeric value went through snprintf("%lld") to get there.  On
 * the banked sieve that is 58 MILLION calls to format the single character
 * "0" -- measured, not guessed -- and snprintf has to parse a format string
 * and walk a varargs list before it writes a byte.
 *
 * The single-digit case is separate because it is overwhelmingly the common
 * one in MV code: flags, counts and small subscripts.
 */
static inline int64_t mv_itoa64(char *buf, int64_t v) {
    if (v >= 0 && v <= 9) { buf[0] = (char)('0' + v); return 1; }
    char tmp[24];
    int n = 0;
    uint64_t u;
    int neg = v < 0;
    /* -INT64_MIN overflows; go through unsigned. */
    u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    do { tmp[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u);
    int64_t len = 0;
    if (neg) buf[len++] = '-';
    while (n) buf[len++] = tmp[--n];
    return len;
}

static span val_span(const mv_value *v, char *buf, size_t cap) {
    switch (v->tag) {
    case MV_STR:
        return (span){mv_str_bytes(v->s), v->s->len};
    case MV_INT:
        return (span){buf, mv_itoa64(buf, v->i)};
    case MV_DBL: {
        double d = v->d;
        int64_t n;
        if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15)
            n = mv_itoa64(buf, (int64_t)d);
        else {
            n = (int64_t)snprintf(buf, cap, "%.4f", d);
            while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
            if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
        }
        return (span){buf, n};
    }
    default:
        return (span){"", 0};
    }
}

static int64_t span_count(span s, char mark) {
    if (s.len == 0) return 0;
    int64_t n = 1;
    for (int64_t i = 0; i < s.len; i++)
        if (s.p[i] == mark) n++;
    return n;
}

/* Span of the idx-th (1-based) mark-separated element; empty span at the
   end when idx is past the last element.

   A plain byte loop, not memchr per element: dynamic-array elements are
   routinely one or two bytes, and at that size a call into memchr costs more
   than the comparison it saves. */
static span nth_span(span s, char mark, int64_t idx) {
    const char *p = s.p, *end = s.p + s.len;
    int64_t n = 1;
    while (n < idx) {
        while (p < end && *p != mark) p++;
        if (p == end) return (span){end, 0};
        p++;
        n++;
    }
    const char *q = p;
    while (q < end && *q != mark) q++;
    return (span){p, q - p};
}

/* ------------------------------------------------------- element index */

/* Walking to element N is linear, so a loop over a long dynamic array is
   quadratic.  Where a range is big enough to pay for it, record the offset of
   every IX_STRIDE-th element so a lookup walks at most IX_STRIDE-1 marks, and
   record the element count so a bounds check costs nothing.

   The threshold is in bytes, not elements: what an index saves is scanning,
   and a long attribute holding two values is just as expensive to walk past
   as a short one holding fifty.  An index covers one byte range at one mark
   level, and a subscript touches several levels in turn, so a string keeps a
   short chain of them -- one per level -- rather than a single slot the
   levels would evict each other from.  All of it is dropped the moment the
   bytes move. */
#define IX_MIN_BYTES 128
#define IX_STRIDE    16
#define IX_MAX_CHAIN 3

struct mv_ix {
    struct mv_ix *next;
    char    mark;
    int64_t base, blen;     /* the indexed range, as an offset into data */
    int64_t n;              /* elements in it */
    int64_t noff;
    int64_t off[];          /* off[k]: start of element k*IX_STRIDE + 1 */
};

void mvx_ix_drop(mv_string *s) {
    if (!s) return;
    struct mv_ix *ix = s->ix;
    while (ix) { struct mv_ix *nx = ix->next; free(ix); ix = nx; }
    s->ix = NULL;
}

static struct mv_ix *ix_build(mv_string *st, span sp, char mark) {
    int64_t n = span_count(sp, mark);
    int64_t noff = (n + IX_STRIDE - 1) / IX_STRIDE;
    if (noff < 1) noff = 1;
    struct mv_ix *ix = malloc(sizeof *ix + (size_t)noff * sizeof(int64_t));
    if (!ix) return NULL;
    ix->mark = mark;
    ix->base = sp.p - mv_str_wbytes(st);
    ix->blen = sp.len;
    ix->n    = n;
    ix->noff = noff;
    const char *p = sp.p, *end = sp.p + sp.len;
    int64_t elem = 0, k = 0;
    ix->off[k++] = 0;
    while (p < end && k < noff) {
        if (*p == mark) {
            elem++;
            if (elem % IX_STRIDE == 0) ix->off[k++] = (p + 1) - sp.p;
        }
        p++;
    }
    while (k < noff) ix->off[k++] = sp.len;

    ix->next = st->ix;
    st->ix = ix;
    /* Keep the chain short; three levels is all a subscript can name. */
    struct mv_ix *q = ix;
    for (int i = 0; i < IX_MAX_CHAIN - 1 && q; i++) q = q->next;
    if (q && q->next) {
        struct mv_ix *drop = q->next;
        q->next = NULL;
        while (drop) { struct mv_ix *nx = drop->next; free(drop); drop = nx; }
    }
    return ix;
}

/* The index for this range and level, building one if it is worth it. */
static struct mv_ix *ix_for(mv_string *st, span sp, char mark) {
    if (!st) return NULL;
    int64_t base = sp.p - mv_str_wbytes(st);
    for (struct mv_ix *ix = st->ix; ix; ix = ix->next)
        if (ix->mark == mark && ix->base == base && ix->blen == sp.len)
            return ix;
    if (sp.len < IX_MIN_BYTES) return NULL;
    return ix_build(st, sp, mark);
}

/* nth_span, using the index when there is one.  The last element needs no
   forward scan at all: it runs to the end of the range by definition. */
static span nth_span_ix(mv_string *st, span sp, char mark, int64_t idx) {
    struct mv_ix *ix = ix_for(st, sp, mark);
    if (!ix || idx > ix->n) return nth_span(sp, mark, idx);
    int64_t b = (idx - 1) / IX_STRIDE;
    int64_t rem = (idx - 1) % IX_STRIDE;
    const char *p = sp.p + ix->off[b], *end = sp.p + sp.len;
    while (rem-- > 0) {
        while (p < end && *p != mark) p++;
        if (p < end) p++;
    }
    if (idx == ix->n) return (span){p, end - p};
    const char *q = p;
    while (q < end && *q != mark) q++;
    return (span){p, q - p};
}

/* nth_span_ix, reporting whether idx was within the element count, so a
   caller needing bounds does not have to count them in a separate pass. */
static span nth_span_chk(mv_string *st, span sp, char mark, int64_t idx,
                         int *ok) {
    struct mv_ix *ix = ix_for(st, sp, mark);
    if (ix) {
        *ok = idx <= ix->n;
        return *ok ? nth_span_ix(st, sp, mark, idx) : (span){sp.p + sp.len, 0};
    }
    /* no index: walk, and notice running off the end on the way.
       A BYTE LOOP, and memchr is SLOWER here -- measured, 144 passes down to
       100 on the banked sieve.  The elements are one character and a mark, so
       every scan is two or three bytes: the call and the vector setup cost
       more than the loop they replace.  memchr wins on long fields, and this
       is not that. */
    const char *p = sp.p, *end = sp.p + sp.len;
    int64_t n = 1;
    while (n < idx) {
        while (p < end && *p != mark) p++;
        if (p == end) { *ok = 0; return (span){end, 0}; }
        p++;
        n++;
    }
    *ok = 1;
    const char *q = p;
    while (q < end && *q != mark) q++;
    return (span){p, q - p};
}

void mv_extract_fn(mv_value *dst, const mv_value *src, int64_t a,
                   int64_t v, int64_t s) {
    char nb[40];
    span sp = val_span(src, nb, sizeof nb);
    mv_string *st = src->tag == MV_STR ? src->s : NULL;
    if (a > 0) sp = nth_span_ix(st, sp, AM, a);
    if (v > 0) sp = nth_span_ix(st, sp, VM, v);
    if (s > 0) sp = nth_span_ix(st, sp, SM, s);
    mv_set_str(dst, sp.p, sp.len);
}

/* ------------------------------------------------------- modify engine */

typedef struct { char *d; int64_t len, cap; } dbuf;

static void bput(dbuf *b, const char *p, int64_t n) {
    if (n <= 0) return;
    if (b->len + n > b->cap) {
        int64_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n) nc *= 2;
        char *nd = realloc(b->d, (size_t)nc);
        if (!nd) mvx_fatal("out of memory in dynamic-array operation");
        b->d = nd;
        b->cap = nc;
    }
    memcpy(b->d + b->len, p, (size_t)n);
    b->len += n;
}

static void bch(dbuf *b, char c) { bput(b, &c, 1); }

static void bmarks(dbuf *b, char c, int64_t n) {
    while (n-- > 0) bch(b, c);
}

enum { OP_REPL, OP_INS, OP_DEL };

/* Apply op at the nesting level named by idx[0..nlev-1]; marks runs
   parallel {AM, VM, SM}.  Non-final levels navigate (padding with marks
   past the end); the final level edits.  idx < 0 appends at that level. */
static void modify(dbuf *out, span s, const char *marks,
                   const int64_t *idx, int nlev, span val, int op) {
    char mark = marks[0];
    int64_t i = idx[0];
    int64_t cnt = span_count(s, mark);

    if (nlev == 1) {
        if (op == OP_DEL) {
            if (cnt == 0 || i < 1 || i > cnt) {
                bput(out, s.p, s.len);
                return;
            }
            span e = nth_span(s, mark, i);
            if (i == cnt) {
                int64_t plen = e.p - s.p;
                if (plen > 0) plen -= 1;            /* preceding mark */
                bput(out, s.p, plen);
            } else {
                bput(out, s.p, e.p - s.p);
                const char *rest = e.p + e.len + 1; /* following mark */
                bput(out, rest, s.p + s.len - rest);
            }
            return;
        }
        if (i < 0) {                                /* append */
            bput(out, s.p, s.len);
            if (cnt > 0) bch(out, mark);
            bput(out, val.p, val.len);
            return;
        }
        if (i == 0) i = 1;
        if (i > cnt) {                              /* pad then place */
            bput(out, s.p, s.len);
            bmarks(out, mark, cnt ? i - cnt : i - 1);
            bput(out, val.p, val.len);
            return;
        }
        span e = nth_span(s, mark, i);
        if (op == OP_REPL) {
            bput(out, s.p, e.p - s.p);
            bput(out, val.p, val.len);
            bput(out, e.p + e.len, s.p + s.len - (e.p + e.len));
        } else {                                    /* OP_INS */
            bput(out, s.p, e.p - s.p);
            bput(out, val.p, val.len);
            bch(out, mark);
            bput(out, e.p, s.p + s.len - e.p);
        }
        return;
    }

    if (i < 0) {
        bput(out, s.p, s.len);
        if (cnt > 0) bch(out, mark);
        modify(out, (span){s.p, 0}, marks + 1, idx + 1, nlev - 1, val, op);
        return;
    }
    if (i == 0) i = 1;
    if (i > cnt) {
        bput(out, s.p, s.len);
        bmarks(out, mark, cnt ? i - cnt : i - 1);
        modify(out, (span){s.p, 0}, marks + 1, idx + 1, nlev - 1, val, op);
        return;
    }
    span e = nth_span(s, mark, i);
    bput(out, s.p, e.p - s.p);
    modify(out, e, marks + 1, idx + 1, nlev - 1, val, op);
    bput(out, e.p + e.len, s.p + s.len - (e.p + e.len));
}

/* Locate the element X<a,v,s> names, failing if any level would have to be
   created.  Only the in-place path uses this; the general path pads. */
static int locate(mv_string *st, int64_t a, int64_t v, int64_t s_, span *out) {
    static const char marks[3] = {AM, VM, SM};
    span sp = {mv_str_bytes(st), st->len};
    int64_t idx[3];
    int n = 0;
    idx[n++] = a;
    if (v != 0 || s_ != 0) idx[n++] = v;
    if (s_ != 0) idx[n++] = s_;
    for (int i = 0; i < n; i++) {
        int64_t k = idx[i];
        if (k < 0) return 0;                    /* append: needs the rebuild */
        if (k == 0) k = 1;
        int ok;
        sp = nth_span_chk(st, sp, marks[i], k, &ok);
        if (!ok) return 0;
    }
    *out = sp;
    return 1;
}

/* Replace an existing element without rebuilding the string.

   X<a,v,s> = y compiles to dst == src (see codegen.cpp), so a value that owns
   its buffer outright can be patched where it stands.  Same-length is the
   cheapest case -- a byte copy, and the index stays exact -- and a length
   change is still only a memmove of the tail while the capacity holds.
   Returns 0 when the general path has to run instead. */
static int inplace_repl(mv_value *dst, const mv_value *src, int64_t a,
                        int64_t v, int64_t s_, const mv_value *val) {
    if (dst != src || dst->tag != MV_STR || dst->s->refs != 1) return 0;
    mv_string *st = dst->s;
    char vb[40];
    span vs = val_span(val, vb, sizeof vb);
    span e;
    if (!locate(st, a, v, s_, &e)) return 0;

    int64_t at = e.p - mv_str_wbytes(st);
    int64_t delta = vs.len - e.len;
    if (delta == 0) {
        memmove(mv_str_wbytes(st) + at, vs.p, (size_t)vs.len);   /* may overlap */
        return 1;                                       /* index still exact */
    }
    /* The value must not live in the bytes about to shift. */
    if (vs.p >= mv_str_wbytes(st) && vs.p < mv_str_wbytes(st) + st->len) return 0;
    if (st->len + delta > st->cap) return 0;
    char *tail = mv_str_wbytes(st) + at + e.len;
    memmove(tail + delta, tail, (size_t)(st->len - at - e.len));
    memcpy(mv_str_wbytes(st) + at, vs.p, (size_t)vs.len);
    st->len += delta;
    mv_str_wbytes(st)[st->len] = '\0';
    mvx_ix_drop(st);            /* every offset past the edit has moved */
    return 1;
}

static void run_op(mv_value *dst, const mv_value *src, int64_t a,
                   int64_t v, int64_t s, const mv_value *val, int op) {
    char nb[40], vb[40];
    span sp = val_span(src, nb, sizeof nb);
    span vs = val ? val_span(val, vb, sizeof vb) : (span){"", 0};
    static const char marks[3] = {AM, VM, SM};
    int64_t idx[3];
    int n = 0;
    idx[n++] = a;
    if (v != 0 || s != 0) idx[n++] = v;
    if (s != 0) idx[n++] = s;
    dbuf out = {0, 0, 0};
    modify(&out, sp, marks, idx, n, vs, op);
    /* Land the result in a buffer with room to spare, so the next edit of
       this variable can be an in-place patch rather than another rebuild. */
    if (dst->tag == MV_STR && dst->s->refs == 1 && dst->s->cap >= out.len) {
        mv_string *st = dst->s;
        memcpy(mv_str_wbytes(st), out.d ? out.d : "", (size_t)out.len);
        st->len = out.len;
        mv_str_wbytes(st)[out.len] = '\0';
        mvx_ix_drop(st);
    } else {
        mv_string *st = mvx_str_alloc_cap(out.len, out.len + out.len / 2 + 16);
        memcpy(mv_str_wbytes(st), out.d ? out.d : "", (size_t)out.len);
        mv_clear(dst);
        dst->tag = MV_STR;
        dst->s = st;
    }
    free(out.d);
}

void mv_replace_fn(mv_value *dst, const mv_value *src, int64_t a,
                   int64_t v, int64_t s, const mv_value *val) {
    if (inplace_repl(dst, src, a, v, s, val)) return;
    run_op(dst, src, a, v, s, val, OP_REPL);
}

void mv_insert_fn(mv_value *dst, const mv_value *src, int64_t a,
                  int64_t v, int64_t s, const mv_value *val) {
    run_op(dst, src, a, v, s, val, OP_INS);
}

void mv_delete_fn(mv_value *dst, const mv_value *src, int64_t a,
                  int64_t v, int64_t s) {
    run_op(dst, src, a, v, s, NULL, OP_DEL);
}

/* --------------------------------------------------------------- LOCATE */

static int num_parse(span s, double *out) {
    char tmp[64];
    if (s.len == 0 || s.len >= (int64_t)sizeof tmp) return 0;
    memcpy(tmp, s.p, (size_t)s.len);
    tmp[s.len] = '\0';
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (end == tmp + s.len) { *out = d; return 1; }
    return 0;
}

static int cmp_left(span a, span b) {
    int64_t min = a.len < b.len ? a.len : b.len;
    int c = memcmp(a.p, b.p, (size_t)min);
    if (c) return c;
    return (a.len > b.len) - (a.len < b.len);
}

static int cmp_right(span a, span b) {
    double x, y;
    if (num_parse(a, &x) && num_parse(b, &y)) return (x > y) - (x < y);
    if (a.len != b.len) return a.len < b.len ? -1 : 1;
    return memcmp(a.p, b.p, (size_t)a.len);
}

int64_t mv_locate_fn(const mv_value *item, const mv_value *src, int64_t a,
                     int64_t v, const mv_value *order, int64_t *pos) {
    char nb[40], ib[40], ob[40];
    span sp = val_span(src, nb, sizeof nb);
    char mark = AM;
    if (a > 0) { sp = nth_span(sp, AM, a); mark = VM; }
    if (a > 0 && v > 0) { sp = nth_span(sp, VM, v); mark = SM; }
    span it = val_span(item, ib, sizeof ib);

    char dir = 0, just = 'L';
    if (order) {
        span os = val_span(order, ob, sizeof ob);
        if (os.len >= 1) dir = (char)toupper((unsigned char)os.p[0]);
        if (os.len >= 2) just = (char)toupper((unsigned char)os.p[1]);
        if (dir != 'A' && dir != 'D') dir = 0;
    }

    int64_t n = span_count(sp, mark);
    const char *q = sp.p, *end = sp.p + sp.len;
    for (int64_t k = 1; k <= n; k++) {
        const char *m = memchr(q, mark, (size_t)(end - q));
        span e = {q, (m ? m : end) - q};
        if (e.len == it.len && memcmp(e.p, it.p, (size_t)e.len) == 0) {
            *pos = k;
            return 1;
        }
        if (dir) {
            int c = (just == 'R') ? cmp_right(it, e) : cmp_left(it, e);
            if ((dir == 'A' && c < 0) || (dir == 'D' && c > 0)) {
                *pos = k;                           /* insertion point */
                return 0;
            }
        }
        q = m ? m + 1 : end;
    }
    *pos = n + 1;
    return 0;
}

/* -------------------------------------------------- LEN / COUNT / DCOUNT */

int64_t mv_len_fn(const mv_value *v) {
    char nb[40];
    span s = val_span(v, nb, sizeof nb);
    return s.len;
}

int64_t mv_count_fn(const mv_value *src, const mv_value *what) {
    char nb[40], wb[40];
    span s = val_span(src, nb, sizeof nb);
    span w = val_span(what, wb, sizeof wb);
    if (w.len == 0 || s.len < w.len) return 0;
    int64_t n = 0;
    const char *p = s.p, *end = s.p + s.len;
    while (p + w.len <= end) {
        const char *m = memmem(p, (size_t)(end - p), w.p, (size_t)w.len);
        if (!m) break;
        n++;
        p = m + w.len;                              /* non-overlapping */
    }
    return n;
}

int64_t mv_dcount_fn(const mv_value *src, const mv_value *delim) {
    char nb[40];
    span s = val_span(src, nb, sizeof nb);
    if (s.len == 0) return 0;
    return mv_count_fn(src, delim) + 1;
}

/* ---- SUM / MAXIMUM / MINIMUM -------------------------------------------- */

static int64_t fmt_num(double d, char *buf, size_t cap) {
    if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15)
        return snprintf(buf, cap, "%lld", (long long)d);
    int64_t n = snprintf(buf, cap, "%.4f", d);
    while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
    if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
    return n;
}

/* SUM: reduce the lowest delimiter level by summation, keeping the higher
   structure — "1\xFD2\xFE3\xFD4" -> "3\xFE7"; non-numeric fields count 0. */
void mv_sum(mv_value *dst, const mv_value *src) {
    char nb[40];
    span s = val_span(src, nb, sizeof nb);
    char level = AM;
    int hasV = 0, hasS = 0;
    for (int64_t i = 0; i < s.len; i++) {
        unsigned char c = (unsigned char)s.p[i];
        if (c == 0xFD) hasV = 1; else if (c == 0xFC) hasS = 1;
    }
    if (hasS) level = SM; else if (hasV) level = VM;
    dbuf out = {0, 0, 0};
    const char *end = s.p + s.len, *fs = s.p;
    double sum = 0;
    char b[40];
    for (const char *p = s.p;; p++) {
        int atend = (p == end);
        unsigned char m = atend ? 0 : (unsigned char)*p;
        if (atend || m == 0xFE || m == 0xFD || m == 0xFC) {
            double d;
            span f = {fs, p - fs};
            if (num_parse(f, &d)) sum += d;
            if (atend) { bput(&out, b, fmt_num(sum, b, sizeof b)); break; }
            fs = p + 1;
            if (m == (unsigned char)level) continue;   /* same level */
            bput(&out, b, fmt_num(sum, b, sizeof b));
            bch(&out, (char)m);
            sum = 0;
        }
    }
    mv_set_str(dst, out.d ? out.d : "", out.len);
    free(out.d);
}

/* MAXIMUM / MINIMUM: the largest / smallest numeric field at any level,
   or "" when the value has no numeric field. */
static void mv_extreme(mv_value *dst, const mv_value *src, int want_max) {
    char nb[40];
    span s = val_span(src, nb, sizeof nb);
    const char *end = s.p + s.len, *fs = s.p;
    double best = 0;
    int have = 0;
    for (const char *p = s.p;; p++) {
        int atend = (p == end);
        unsigned char m = atend ? 0 : (unsigned char)*p;
        if (atend || m == 0xFE || m == 0xFD || m == 0xFC) {
            double d;
            span f = {fs, p - fs};
            if (num_parse(f, &d)) {
                if (!have || (want_max ? d > best : d < best)) best = d;
                have = 1;
            }
            if (atend) break;
            fs = p + 1;
        }
    }
    if (!have) { mv_set_str(dst, "", 0); return; }
    char b[40];
    mv_set_str(dst, b, fmt_num(best, b, sizeof b));
}

void mv_max_fn(mv_value *dst, const mv_value *src) { mv_extreme(dst, src, 1); }
void mv_min_fn(mv_value *dst, const mv_value *src) { mv_extreme(dst, src, 0); }
