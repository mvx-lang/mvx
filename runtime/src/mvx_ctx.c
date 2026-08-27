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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE           /* glibc: dladdr / Dl_info */
#endif

#include "mvx_runtime.h"
#include "mv_bytes.h"

#include <dlfcn.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Directory holding libmvxrt itself, found at run time from the loaded
   library's own path.  Everything else in an MVX install is located
   relative to it — storage drivers sit in the same directory, the
   binaries in ../bin, the system account in ../share/mvx/system — so a
   whole install relocates as a unit with no baked-in absolute paths.
   Returns "" if the path cannot be determined. */
const char *mvx_runtime_dir(void) {
    static char dir[4096];
    static int done = 0;
    if (done) return dir;
    done = 1;
    Dl_info info;
    if (dladdr((void *)mvx_runtime_dir, &info) && info.dli_fname &&
        info.dli_fname[0]) {
        char tmp[4096];
        snprintf(tmp, sizeof tmp, "%s", info.dli_fname);
        snprintf(dir, sizeof dir, "%s", dirname(tmp));
    } else {
        dir[0] = '\0';
    }
    return dir;
}

/* Point the dynamic loader at libmvxrt's own directory for child
   processes.  Cataloged verbs are separate executables the shell spawns;
   they carry an rpath into the install's lib, but a *moved* prefix leaves
   that rpath stale.  Because every such process is a child of one that
   has libmvxrt loaded, exporting the current runtime directory on the
   loader path lets each spawned verb find libmvxrt wherever the prefix
   now lives — so the whole install stays relocatable, not just the top
   binaries.  Drivers are dlopen'd by absolute path and are unaffected. */
__attribute__((constructor))
static void mvx_export_loader_path(void) {
    const char *rtd = mvx_runtime_dir();
    if (!rtd[0]) return;
#ifdef __APPLE__
    const char *var = "DYLD_LIBRARY_PATH";
#else
    const char *var = "LD_LIBRARY_PATH";
#endif
    const char *cur = getenv(var);
    if (cur && cur[0]) {
        /* already leading with our dir?  leave it be */
        size_t n = strlen(rtd);
        if (strncmp(cur, rtd, n) == 0 && (cur[n] == ':' || cur[n] == '\0'))
            return;
        char buf[8192];
        snprintf(buf, sizeof buf, "%s:%s", rtd, cur);
        setenv(var, buf, 1);
    } else {
        setenv(var, rtd, 1);
    }
}

/* Session context.  The parameter exists in every ABI signature so that
   session identity, locks, and the privilege gate can land here later
   without breaking compiled code.  It currently carries output state,
   the STATUS() flag, and COMMON block storage. */

typedef struct common_slot {
    mv_value v;
    mv_array *arr;
} common_slot;

/* Slots are handed out by address and held for the life of compiled
   code, so storage must never move: slots live in fixed-size chunks
   allocated on demand, never realloc'd. */
#define COMMON_CHUNK 64
#define COMMON_MAX_CHUNKS 1024

typedef struct common_block {
    char *name;
    common_slot *chunks[COMMON_MAX_CHUNKS];
    struct common_block *next;
} common_block;

struct mvx_ctx {
    int64_t print_col;      /* current output column, for comma zones */
    int64_t status;         /* STATUS() value, set by conversions */
    common_block *commons;
    void *store;            /* storage state, owned by mvx_store.c */
};

void *mvx_ctx_store_get(mvx_ctx *ctx) { return ctx->store; }
void  mvx_ctx_store_set(mvx_ctx *ctx, void *p) { ctx->store = p; }

mvx_ctx *mvx_ctx_create(void) {
    mvx_ctx *ctx = calloc(1, sizeof(mvx_ctx));
    if (!ctx) mvx_fatal("out of memory creating context");
    return ctx;
}

void mvx_ctx_destroy(mvx_ctx *ctx) {
    mvx_store_shutdown(ctx);
    common_block *b = ctx->commons;
    while (b) {
        common_block *next = b->next;
        for (int c = 0; c < COMMON_MAX_CHUNKS; c++) {
            if (!b->chunks[c]) continue;
            for (int i = 0; i < COMMON_CHUNK; i++) {
                mv_clear(&b->chunks[c][i].v);
                mv_arr_destroy(b->chunks[c][i].arr);
            }
            free(b->chunks[c]);
        }
        free(b->name);
        free(b);
        b = next;
    }
    free(ctx);
}

void mvx_ctx_set_status(mvx_ctx *ctx, int64_t s) { ctx->status = s; }

int64_t mvx_status(mvx_ctx *ctx) { return ctx->status; }

/* ------------------------------------------------------- COMMON blocks */

static common_block *common_get(mvx_ctx *ctx, const char *name) {
    for (common_block *b = ctx->commons; b; b = b->next)
        if (strcmp(b->name, name) == 0) return b;
    common_block *b = calloc(1, sizeof(common_block));
    if (!b) mvx_fatal("out of memory creating COMMON block");
    b->name = strdup(name);
    b->next = ctx->commons;
    ctx->commons = b;
    return b;
}

static common_slot *common_slot_at(mvx_ctx *ctx, const char *name,
                                   int64_t idx) {
    common_block *b = common_get(ctx, name);
    if (idx < 0 || idx >= (int64_t)COMMON_CHUNK * COMMON_MAX_CHUNKS)
        mvx_fatal("COMMON index %lld out of range", (long long)idx);
    int64_t c = idx / COMMON_CHUNK;
    if (!b->chunks[c]) {
        b->chunks[c] = calloc(COMMON_CHUNK, sizeof(common_slot));
        if (!b->chunks[c]) mvx_fatal("out of memory extending COMMON block");
    }
    return &b->chunks[c][idx % COMMON_CHUNK];
}

mv_value *mvx_common_scalar(mvx_ctx *ctx, const char *block, int64_t idx) {
    return &common_slot_at(ctx, block, idx)->v;
}

mv_array *mvx_common_arr(mvx_ctx *ctx, const char *block, int64_t idx,
                         int64_t d1, int64_t d2) {
    common_slot *s = common_slot_at(ctx, block, idx);
    if (!s->arr) s->arr = mv_arr_create(d1, d2);
    return s->arr;
}

/* --------------------------------------------------------------- output */

static int64_t num_len_probe(const char *p) { return (int64_t)strlen(p); }

void mv_print(mvx_ctx *ctx, const mv_value *v) {
    char buf[40];
    switch (v->tag) {
    case MV_STR:
        fwrite(mv_str_bytes(v->s), 1, (size_t)v->s->len, stdout);
        for (int64_t k = 0; k < v->s->len; k++)
            ctx->print_col = (mv_str_bytes(v->s)[k] == '\n') ? 0 : ctx->print_col + 1;
        return;
    case MV_INT:
        snprintf(buf, sizeof buf, "%lld", (long long)v->i);
        break;
    case MV_DBL: {
        double d = v->d;
        if (d == (double)(int64_t)d && d >= -1e15 && d <= 1e15)
            snprintf(buf, sizeof buf, "%lld", (long long)d);
        else {
            int n = snprintf(buf, sizeof buf, "%.4f", d);
            while (n > 0 && buf[n - 1] == '0') buf[--n] = '\0';
            if (n > 0 && buf[n - 1] == '.') buf[--n] = '\0';
        }
        break;
    }
    default:
        buf[0] = '\0';
        break;
    }
    fputs(buf, stdout);
    ctx->print_col += num_len_probe(buf);
}

void mv_print_nl(mvx_ctx *ctx) {
    fputc('\n', stdout);
    ctx->print_col = 0;
}

void mv_print_tab(mvx_ctx *ctx) {
    /* Classic 18-column print zones. */
    int64_t next = ((ctx->print_col / 18) + 1) * 18;
    while (ctx->print_col < next) {
        fputc(' ', stdout);
        ctx->print_col++;
    }
}

/* Environment variable, or "" when unset. */
void mv_env(mv_value *dst, const mv_value *name) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(name, nb, sizeof nb, &p);
    char key[256];
    if (n <= 0 || n >= (int64_t)sizeof key) {
        mv_set_str(dst, "", 0);
        return;
    }
    memcpy(key, p, (size_t)n);
    key[n] = '\0';
    const char *v = getenv(key);
    if (!v) v = "";
    mv_set_str(dst, v, (int64_t)strlen(v));
}

/* The TCL command line that invoked this program, set by the shell. */
void mv_sentence(mvx_ctx *ctx, mv_value *dst) {
    (void)ctx;
    const char *s = getenv("MVX_SENTENCE");
    if (!s) s = "";
    mv_set_str(dst, s, (int64_t)strlen(s));
}

/* @USER.TYPE — the R83/UniData session-type test, rendered on Unix.  0 when a
   terminal is attached (an interactive line), else 2 ("other background": a
   pipe, the networked daemon, an EXECUTE capture — mvx's phantom-equivalents).
   Portable BASIC can then guard terminal-only work (e.g. a login-time modal)
   with @USER.TYPE = 0 on mvx exactly as on UniData/UniVerse. */
void mv_user_type(mvx_ctx *ctx, mv_value *dst) {
    (void)ctx;
    mv_set_int(dst, isatty(0) ? 0 : 2);
}

/* ---------------------------------------------------------------- input */

/* INPUT reads fd 0 one byte at a time, deliberately unbuffered: a
   program often shares stdin with the TCL session that spawned it, and
   buffered readahead would swallow lines meant for the shell.  EOF
   with nothing pending ends the program — otherwise a piped INPUT
   loop would spin forever on "". */
void mv_input(mvx_ctx *ctx, mv_value *dst) {
    (void)ctx;
    fflush(stdout);
    char buf[4096];
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) {
            if (n == 0) mvx_stop();     /* EOF: end of program input */
            break;
        }
        if (c == '\n') break;
        if (c != '\r' && n < sizeof buf - 1) buf[n++] = c;
    }
    mv_set_str(dst, buf, (int64_t)n);
}

/* ------------------------------------------------------------ intrinsics */

static void time_of_day(int64_t *secs, int64_t *msecs) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm lt;
    time_t t = tv.tv_sec;
    localtime_r(&t, &lt);               /* classic TIME() is local time */
    int64_t since_midnight =
        (int64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    *secs  = since_midnight;
    *msecs = since_midnight * 1000 + tv.tv_usec / 1000;
}

void mv_time(mv_value *dst) {
    int64_t s, ms;
    time_of_day(&s, &ms);
    mv_set_int(dst, s);
}

/* RND(n): 0 .. n-1, classic. */
int64_t mv_rnd_fn(int64_t n) {
    static int seeded;
    if (!seeded) {
        srandom((unsigned)(time(NULL) ^ getpid()));
        seeded = 1;
    }
    if (n <= 0) return 0;
    return (int64_t)(random() % n);
}

/* DATE(): internal date, day 0 = 31 DEC 1967, local time. */
int64_t mv_date_fn(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    int64_t y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + 732;
}

/* Live terminal size: ioctl first (tracks window resizes), then the
   COLUMNS/LINES environment, then the classic 80x24. */
static void term_size(int64_t *w, int64_t *h) {
    struct winsize ws;
    for (int fd = 1; fd >= 0; fd--) {
        if (isatty(fd) && ioctl(fd, TIOCGWINSZ, &ws) == 0 &&
            ws.ws_col > 0) {
            *w = ws.ws_col;
            *h = ws.ws_row;
            return;
        }
    }
    const char *c = getenv("COLUMNS");
    const char *l = getenv("LINES");
    *w = (c && atoi(c) > 0) ? atoi(c) : 80;
    *h = (l && atoi(l) > 0) ? atoi(l) : 24;
}

void mv_system_fn(mvx_ctx *ctx, mv_value *dst, const mv_value *code) {
    int64_t s, ms;
    int64_t tw, th;
    switch (mv_get_int(code)) {
    case 2:                        /* terminal width, classic */
        term_size(&tw, &th);
        mv_set_int(dst, tw);
        return;
    case 3:                        /* terminal depth, classic */
        term_size(&tw, &th);
        mv_set_int(dst, th);
        return;
    case 11:                       /* select list active? */
        mv_set_int(dst, mvx_list_active(ctx));
        return;
    case 12:                       /* ms since midnight — later-MV extension;
                                      classic TIME() is whole seconds, too
                                      coarse for benchmarking */
        time_of_day(&s, &ms);
        mv_set_int(dst, ms);
        return;
    default:
        mvx_fatal("SYSTEM(%lld) not implemented",
                  (long long)mv_get_int(code));
    }
}

void mv_int_fn(mv_value *dst, const mv_value *a) {
    mv_set_int(dst, mv_get_int(a));
}

void mv_sqrt_fn(mv_value *dst, const mv_value *a) {
    double d = mv_get_dbl(a);
    if (d < 0) mvx_fatal("SQRT of negative number");
    mv_set_dbl(dst, __builtin_sqrt(d));
}

void mv_abs_fn(mv_value *dst, const mv_value *a) {
    if (a->tag == MV_INT) mv_set_int(dst, a->i < 0 ? -a->i : a->i);
    else {
        double d = mv_get_dbl(a);
        mv_set_dbl(dst, d < 0 ? -d : d);
    }
}

/* ---------------------------------------- numeric-specialised support */

void *mvx_buf_create(int64_t nbytes) {
    void *p = calloc(1, (size_t)nbytes);
    if (!p) mvx_fatal("out of memory in DIM of %lld bytes", (long long)nbytes);
    return p;
}

void mvx_buf_destroy(void *p) { free(p); }

void mvx_narr_fail(int64_t i, int64_t j, int64_t d1, int64_t d2) {
    if (d2)
        mvx_fatal("array subscript (%lld,%lld) out of range (1..%lld,1..%lld)",
                  (long long)i, (long long)j, (long long)d1, (long long)d2);
    mvx_fatal("array subscript (%lld) out of range 1..%lld",
              (long long)i, (long long)d1);
}

double mvx_num_time(void) {
    mv_value v;
    mv_init(&v);
    mv_time(&v);
    return (double)v.i;
}

double mvx_num_system(mvx_ctx *ctx, double code) {
    mv_value c, r;
    mv_init(&c); mv_init(&r);
    mv_set_int(&c, (int64_t)code);
    mv_system_fn(ctx, &r, &c);
    return mv_get_dbl(&r);
}

double mvx_num_mod(double a, double b) {
    if (b == 0.0) return a;
    double r = __builtin_fmod(a, b);
    if (r != 0.0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

int64_t mvx_num_imod(int64_t a, int64_t b) {
    if (b == 0) return a;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

/* Numeric intrinsics.  __builtin_* map to libm without pulling in the
 * header.  Pick trigonometry works in degrees, not radians. */
#define MVX_DEG (3.14159265358979323846 / 180.0)
double mvx_num_pow(double a, double b) { return __builtin_pow(a, b); }
double mvx_num_ln(double x)   { return __builtin_log(x); }
double mvx_num_exp(double x)  { return __builtin_exp(x); }
double mvx_num_sin(double d)  { return __builtin_sin(d * MVX_DEG); }
double mvx_num_cos(double d)  { return __builtin_cos(d * MVX_DEG); }
double mvx_num_tan(double d)  { return __builtin_tan(d * MVX_DEG); }
double mvx_num_atan(double x) { return __builtin_atan(x) / MVX_DEG; }

void mv_mod_fn(mv_value *dst, const mv_value *a, const mv_value *b) {
    if (a->tag == MV_INT && b->tag == MV_INT) {
        if (b->i == 0) { mv_set_int(dst, a->i); return; }  /* MV: MOD(x,0)=x */
        int64_t r = a->i % b->i;
        if (r != 0 && ((r < 0) != (b->i < 0))) r += b->i;
        mv_set_int(dst, r);
        return;
    }
    double x = mv_get_dbl(a), y = mv_get_dbl(b);
    if (y == 0.0) { mv_set_dbl(dst, x); return; }
    double r = __builtin_fmod(x, y);
    if (r != 0.0 && ((r < 0) != (y < 0))) r += y;
    mv_set_dbl(dst, r);
}
