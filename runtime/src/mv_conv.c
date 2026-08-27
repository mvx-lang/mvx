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

/* OCONV / ICONV conversions and FMT format masks.
 *
 * Codes implemented (the classic core):
 *   D{y}{sep}  dates — internal day number, day 0 = 31 DEC 1967
 *   MT{S}      time — seconds since midnight to HH:MM[:SS] and back
 *   MD{n}[,][$] masked decimal — descale by 10^n, thousands, currency
 *   MCU / MCL / MCT  case conversions
 * STATUS() reflects the outcome: 0 ok, 1 bad input, 2 bad code.
 */
#include "mvx_runtime.h"
#include "mv_bytes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *p; int64_t len; } span;

static span val_span(const mv_value *v, char *buf, size_t cap) {
    switch (v->tag) {
    case MV_STR:
        return (span){mv_str_bytes(v->s), v->s->len};
    case MV_INT:
        return (span){buf,
                      (int64_t)snprintf(buf, cap, "%lld", (long long)v->i)};
    case MV_DBL: {
        double d = v->d;
        int64_t n;
        if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15)
            n = (int64_t)snprintf(buf, cap, "%lld", (long long)d);
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

void mvx_ctx_set_status(mvx_ctx *ctx, int64_t s);   /* in mvx_ctx.c */

/* ------------------------------------------------------------- calendar */
/* Civil-days conversion (proleptic Gregorian); Pick day 0 = 1967-12-31,
   which is Unix day -732. */

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int64_t *m, int64_t *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static const char *kMonths[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

/* ----------------------------------------------------------- numeric in */

static int span_num(span s, double *out) {
    char tmp[64];
    if (s.len == 0 || s.len >= (int64_t)sizeof tmp) return 0;
    memcpy(tmp, s.p, (size_t)s.len);
    tmp[s.len] = '\0';
    char *end = NULL;
    double d = strtod(tmp, &end);
    if (end == tmp + s.len) { *out = d; return 1; }
    return 0;
}

/* ----------------------------------------------------------------- OCONV */

static int oconv_date(mvx_ctx *ctx, mv_value *dst, span in, span code) {
    double dv;
    if (!span_num(in, &dv)) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, in.p, in.len);
        return 1;
    }
    int64_t y, m, d;
    civil_from_days((int64_t)dv - 732, &y, &m, &d);

    int ylen = 4;
    char sep = 0;
    int64_t k = 1;
    if (k < code.len && isdigit((unsigned char)code.p[k]))
        ylen = code.p[k++] - '0';
    if (k < code.len) sep = code.p[k];

    char buf[32];
    int n;
    if (sep) {
        if (ylen == 2)
            n = snprintf(buf, sizeof buf, "%02lld%c%02lld%c%02lld",
                         (long long)m, sep, (long long)d, sep,
                         (long long)(y % 100));
        else
            n = snprintf(buf, sizeof buf, "%02lld%c%02lld%c%04lld",
                         (long long)m, sep, (long long)d, sep, (long long)y);
    } else {
        n = snprintf(buf, sizeof buf, "%02lld %s %04lld", (long long)d,
                     kMonths[m - 1], (long long)y);
    }
    mv_set_str(dst, buf, n);
    return 1;
}

static int oconv_time(mvx_ctx *ctx, mv_value *dst, span in, span code) {
    double dv;
    if (!span_num(in, &dv)) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, in.p, in.len);
        return 1;
    }
    int64_t secs = ((int64_t)dv % 86400 + 86400) % 86400;
    int withSecs = code.len >= 3 &&
                   toupper((unsigned char)code.p[2]) == 'S';
    char buf[16];
    int n;
    if (withSecs)
        n = snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld",
                     (long long)(secs / 3600), (long long)(secs / 60 % 60),
                     (long long)(secs % 60));
    else
        n = snprintf(buf, sizeof buf, "%02lld:%02lld",
                     (long long)(secs / 3600), (long long)(secs / 60 % 60));
    mv_set_str(dst, buf, n);
    return 1;
}

/* Map-projection ISO renderers: an internal Pick date (day count) or time
   (seconds past midnight) rendered as ISO-8601 text for typed DATE/TIME
   columns.  Postgres parses these unambiguously, unlike the locale-shaped
   display conversions.  Return the length written, or 0 (with an empty
   string) when the input is not a clean number, so the caller stores NULL. */
int64_t mvx_iso_date_str(const char *in, int64_t len, char *out, size_t cap) {
    double dv;
    if (!span_num((span){in, len}, &dv)) { if (cap) out[0] = '\0'; return 0; }
    int64_t y, m, d;
    civil_from_days((int64_t)dv - 732, &y, &m, &d);
    int n = snprintf(out, cap, "%04lld-%02lld-%02lld",
                     (long long)y, (long long)m, (long long)d);
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

int64_t mvx_iso_time_str(const char *in, int64_t len, char *out, size_t cap) {
    double dv;
    if (!span_num((span){in, len}, &dv)) { if (cap) out[0] = '\0'; return 0; }
    int64_t secs = ((int64_t)dv % 86400 + 86400) % 86400;
    int n = snprintf(out, cap, "%02lld:%02lld:%02lld",
                     (long long)(secs / 3600), (long long)(secs / 60 % 60),
                     (long long)(secs % 60));
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

/* Inverse of the ISO renderers: parse an ISO-8601 date/time back to the
   Pick internal value (day count / seconds past midnight) as a plain
   number, for native-mode read recomposition.  Return the length written,
   or 0 when the text does not parse (leaving an empty string). */
static int64_t iso_uint(const char **p, const char *e, int64_t *out) {
    int64_t v = 0, n = 0;
    while (*p < e && **p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; n++; }
    *out = v;
    return n;
}

int64_t mvx_iso_date_intern(const char *in, int64_t len, char *out, size_t cap) {
    const char *p = in, *e = in + len;
    int64_t y, m, d;
    if (!iso_uint(&p, e, &y) || p >= e || *p++ != '-') { if (cap) out[0] = '\0'; return 0; }
    if (!iso_uint(&p, e, &m) || p >= e || *p++ != '-') { if (cap) out[0] = '\0'; return 0; }
    if (!iso_uint(&p, e, &d) || m < 1 || m > 12 || d < 1 || d > 31) { if (cap) out[0] = '\0'; return 0; }
    int n = snprintf(out, cap, "%lld", (long long)(days_from_civil(y, m, d) + 732));
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

int64_t mvx_iso_time_intern(const char *in, int64_t len, char *out, size_t cap) {
    const char *p = in, *e = in + len;
    int64_t h, mi, s = 0;
    if (!iso_uint(&p, e, &h) || p >= e || *p++ != ':') { if (cap) out[0] = '\0'; return 0; }
    if (!iso_uint(&p, e, &mi)) { if (cap) out[0] = '\0'; return 0; }
    if (p < e && *p == ':') { p++; iso_uint(&p, e, &s); }
    int n = snprintf(out, cap, "%lld", (long long)(h * 3600 + mi * 60 + s));
    return (n > 0 && (size_t)n < cap) ? n : 0;
}

static int oconv_md(mvx_ctx *ctx, mv_value *dst, span in, span code) {
    double dv;
    if (!span_num(in, &dv)) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, in.p, in.len);
        return 1;
    }
    int64_t k = 2;
    int dec = 0;
    if (k < code.len && isdigit((unsigned char)code.p[k]))
        dec = code.p[k++] - '0';
    if (k < code.len && isdigit((unsigned char)code.p[k])) k++;  /* MDnn */
    int thousands = 0, dollar = 0;
    for (; k < code.len; k++) {
        if (code.p[k] == ',') thousands = 1;
        else if (code.p[k] == '$') dollar = 1;
    }

    int neg = dv < 0;
    if (neg) dv = -dv;
    double scale = 1;
    for (int i = 0; i < dec; i++) scale *= 10;
    long long whole = (long long)(dv / scale);
    long long frac = (long long)(dv - (double)whole * scale + 0.5);
    if (frac >= (long long)scale && dec > 0) { whole++; frac = 0; }

    char digits[32];
    int dn = snprintf(digits, sizeof digits, "%lld", whole);
    char buf[64];
    int n = 0;
    if (neg) buf[n++] = '-';
    if (dollar) buf[n++] = '$';
    for (int i = 0; i < dn; i++) {
        buf[n++] = digits[i];
        int remaining = dn - 1 - i;
        if (thousands && remaining > 0 && remaining % 3 == 0)
            buf[n++] = ',';
    }
    if (dec > 0)
        n += snprintf(buf + n, sizeof buf - (size_t)n, ".%0*lld", dec,
                      frac);
    mv_set_str(dst, buf, n);
    return 1;
}

static int conv_case(mv_value *dst, span in, char kind) {
    char *buf = malloc(in.len ? (size_t)in.len : 1);
    if (!buf) mvx_fatal("out of memory in MC conversion");
    int newWord = 1;
    for (int64_t i = 0; i < in.len; i++) {
        unsigned char c = (unsigned char)in.p[i];
        switch (kind) {
        case 'U': buf[i] = (char)toupper(c); break;
        case 'L': buf[i] = (char)tolower(c); break;
        default:                                    /* T: title case */
            if (isalpha(c)) {
                buf[i] = (char)(newWord ? toupper(c) : tolower(c));
                newWord = 0;
            } else {
                buf[i] = (char)c;
                newWord = 1;
            }
            break;
        }
    }
    mv_set_str(dst, buf, in.len);
    free(buf);
    return 1;
}

void mv_oconv(mvx_ctx *ctx, mv_value *dst, const mv_value *src,
              const mv_value *code) {
    char ib[40], cb[40];
    span in = val_span(src, ib, sizeof ib);
    span c = val_span(code, cb, sizeof cb);
    mvx_ctx_set_status(ctx, 0);
    if (c.len >= 1) {
        char c0 = (char)toupper((unsigned char)c.p[0]);
        char c1 = c.len >= 2 ? (char)toupper((unsigned char)c.p[1]) : 0;
        char c2 = c.len >= 3 ? (char)toupper((unsigned char)c.p[2]) : 0;
        if (c0 == 'D') { oconv_date(ctx, dst, in, c); return; }
        if (c0 == 'M' && c1 == 'T') { oconv_time(ctx, dst, in, c); return; }
        if (c0 == 'M' && c1 == 'D') { oconv_md(ctx, dst, in, c); return; }
        if (c0 == 'M' && c1 == 'C' &&
            (c2 == 'U' || c2 == 'L' || c2 == 'T')) {
            conv_case(dst, in, c2);
            return;
        }
    }
    mvx_ctx_set_status(ctx, 2);
    mv_set_str(dst, in.p, in.len);
}

/* ----------------------------------------------------------------- ICONV */

static int month_lookup(const char *p, int64_t len) {
    if (len < 3) return 0;
    char m[4] = {(char)toupper((unsigned char)p[0]),
                 (char)toupper((unsigned char)p[1]),
                 (char)toupper((unsigned char)p[2]), 0};
    for (int i = 0; i < 12; i++)
        if (memcmp(m, kMonths[i], 3) == 0) return i + 1;
    return 0;
}

static int iconv_date(mvx_ctx *ctx, mv_value *dst, span in) {
    /* Split on / - space or period. */
    span part[3];
    int np = 0;
    const char *q = in.p, *end = in.p + in.len;
    while (q < end && np < 3) {
        const char *st = q;
        while (q < end && *q != '/' && *q != '-' && *q != ' ' && *q != '.')
            q++;
        part[np++] = (span){st, q - st};
        while (q < end &&
               (*q == '/' || *q == '-' || *q == ' ' || *q == '.'))
            q++;
    }
    int64_t y = 0, m = 0, d = 0;
    double a, b, cc;
    if (np == 3 && span_num(part[0], &a) && span_num(part[1], &b) &&
        span_num(part[2], &cc)) {
        m = (int64_t)a; d = (int64_t)b; y = (int64_t)cc;   /* US order */
    } else if (np == 3 && span_num(part[0], &a) &&
               month_lookup(part[1].p, part[1].len) &&
               span_num(part[2], &cc)) {
        d = (int64_t)a;
        m = month_lookup(part[1].p, part[1].len);
        y = (int64_t)cc;
    } else {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, "", 0);
        return 1;
    }
    if (y < 100) y += y < 68 ? 2000 : 1900;
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, "", 0);
        return 1;
    }
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%lld",
                     (long long)(days_from_civil(y, m, d) + 732));
    mv_set_str(dst, buf, n);
    return 1;
}

static int iconv_time(mvx_ctx *ctx, mv_value *dst, span in) {
    long long h = 0, m = 0, s = 0;
    char tmp[32];
    if (in.len == 0 || in.len >= (int64_t)sizeof tmp) goto bad;
    memcpy(tmp, in.p, (size_t)in.len);
    tmp[in.len] = '\0';
    if (sscanf(tmp, "%lld:%lld:%lld", &h, &m, &s) < 2) goto bad;
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) goto bad;
    {
        char buf[16];
        int n = snprintf(buf, sizeof buf, "%lld", h * 3600 + m * 60 + s);
        mv_set_str(dst, buf, n);
    }
    return 1;
bad:
    mvx_ctx_set_status(ctx, 1);
    mv_set_str(dst, "", 0);
    return 1;
}

static int iconv_md(mvx_ctx *ctx, mv_value *dst, span in, span code) {
    int dec = 0;
    if (code.len >= 3 && isdigit((unsigned char)code.p[2]))
        dec = code.p[2] - '0';
    char tmp[64];
    int64_t n = 0;
    for (int64_t i = 0; i < in.len && n < (int64_t)sizeof tmp - 1; i++) {
        char ch = in.p[i];
        if (ch == '$' || ch == ',' || ch == ' ') continue;
        tmp[n++] = ch;
    }
    tmp[n] = '\0';
    char *endp = NULL;
    double d = strtod(tmp, &endp);
    if (n == 0 || endp != tmp + n) {
        mvx_ctx_set_status(ctx, 1);
        mv_set_str(dst, "", 0);
        return 1;
    }
    double scale = 1;
    for (int i = 0; i < dec; i++) scale *= 10;
    char buf[32];
    int bn = snprintf(buf, sizeof buf, "%lld",
                      (long long)(d * scale + (d < 0 ? -0.5 : 0.5)));
    mv_set_str(dst, buf, bn);
    return 1;
}

void mv_iconv(mvx_ctx *ctx, mv_value *dst, const mv_value *src,
              const mv_value *code) {
    char ib[40], cb[40];
    span in = val_span(src, ib, sizeof ib);
    span c = val_span(code, cb, sizeof cb);
    mvx_ctx_set_status(ctx, 0);
    if (c.len >= 1) {
        char c0 = (char)toupper((unsigned char)c.p[0]);
        char c1 = c.len >= 2 ? (char)toupper((unsigned char)c.p[1]) : 0;
        char c2 = c.len >= 3 ? (char)toupper((unsigned char)c.p[2]) : 0;
        if (c0 == 'D') { iconv_date(ctx, dst, in); return; }
        if (c0 == 'M' && c1 == 'T') { iconv_time(ctx, dst, in); return; }
        if (c0 == 'M' && c1 == 'D') { iconv_md(ctx, dst, in, c); return; }
        if (c0 == 'M' && c1 == 'C' &&
            (c2 == 'U' || c2 == 'L' || c2 == 'T')) {
            conv_case(dst, in, c2);
            return;
        }
    }
    mvx_ctx_set_status(ctx, 2);
    mv_set_str(dst, in.p, in.len);
}

/* ------------------------------------------------------------------- FMT */
/* Mask: {L|R}{fill}{width} — fill '#' = space, '*' = asterisk,
   '%' = zero; fill optional (default space).  Unknown masks return the
   value unchanged. */

void mv_fmt(mv_value *dst, const mv_value *src, const mv_value *mask) {
    char ib[40], mb[40];
    span in = val_span(src, ib, sizeof ib);
    span mk = val_span(mask, mb, sizeof mb);

    if (mk.len < 2) { mv_set_str(dst, in.p, in.len); return; }
    char just = (char)toupper((unsigned char)mk.p[0]);
    if (just != 'L' && just != 'R') {
        mv_set_str(dst, in.p, in.len);
        return;
    }
    int64_t k = 1;
    char fill = ' ';
    if (mk.p[k] == '#') { fill = ' '; k++; }
    else if (mk.p[k] == '*') { fill = '*'; k++; }
    else if (mk.p[k] == '%') { fill = '0'; k++; }
    int64_t width = 0;
    int sawDigit = 0;
    for (; k < mk.len && isdigit((unsigned char)mk.p[k]); k++) {
        width = width * 10 + (mk.p[k] - '0');
        sawDigit = 1;
    }
    if (!sawDigit || k != mk.len || width <= 0) {
        mv_set_str(dst, in.p, in.len);
        return;
    }

    char *buf = malloc((size_t)width);
    if (!buf) mvx_fatal("out of memory in FMT");
    if (in.len >= width) {
        /* Truncate: L keeps the left, R keeps the right. */
        memcpy(buf, just == 'L' ? in.p : in.p + in.len - width,
               (size_t)width);
    } else {
        memset(buf, fill, (size_t)width);
        memcpy(just == 'L' ? buf : buf + width - in.len, in.p,
               (size_t)in.len);
    }
    mv_set_str(dst, buf, width);
    free(buf);
}
