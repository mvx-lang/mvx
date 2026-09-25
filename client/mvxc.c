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

/* mvxc.c — the MVX client library (#289), in-process over libmvxrt.
 *
 * Everything here is a thin translation: an opaque handle in, an mv_value out,
 * a status back.  The two rules from mvxc.h are the only design in it -- every
 * result is owned by the handle that returned it, and nothing aborts the host.
 */
#include "mvxc.h"
#include "mvx_runtime.h"

#include <stdlib.h>
#include <string.h>

#define AM "\xfe"
#define VM "\xfd"

struct mvxc_session {
    mvx_ctx *ctx;
    char    *err;       /* last failure, owned */
    char    *nextid;    /* mvxc_next's answer, owned and replaced */
};

struct mvxc_file {
    mvxc_session *s;
    mv_value      fvar;
    int           open;
};

/* A value owns every string it has handed out, until it is modified or freed.
 * That is what lets two accessors be live at once, which is the thing callers
 * do constantly -- printf("%s %s", attr(r,1), attr(r,2)). */
struct mvxc_val {
    mv_value  v;
    char    **held;
    int       n, cap;
};

/* --- small helpers ------------------------------------------------------ */

static void set_err(mvxc_session *s, const char *msg) {
    if (!s) return;
    free(s->err);
    s->err = msg ? strdup(msg) : NULL;
}

static void drop_held(mvxc_val *v) {
    for (int i = 0; i < v->n; i++) free(v->held[i]);
    v->n = 0;
}

/* Copy `p` into the value's held set and return it.  NULL only on OOM, which
 * a caller reads as an empty field rather than a crash. */
static const char *hold(mvxc_val *v, const char *p, int64_t len) {
    if (v->n == v->cap) {
        int cap = v->cap ? v->cap * 2 : 8;
        char **h = realloc(v->held, (size_t)cap * sizeof *h);
        if (!h) return "";
        v->held = h;
        v->cap = cap;
    }
    char *c = malloc((size_t)len + 1);
    if (!c) return "";
    if (len) memcpy(c, p, (size_t)len);
    c[len] = '\0';
    v->held[v->n++] = c;
    return c;
}

/* The bytes of `src`, held by `owner`. */
static const char *chars_held(mvxc_val *owner, const mv_value *src) {
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(src, nb, sizeof nb, &p);
    return hold(owner, p, n < 0 ? 0 : n);
}

static void tmp_str(mv_value *t, const char *s) {
    mv_init(t);
    mv_set_str(t, s ? s : "", s ? (int64_t)strlen(s) : 0);
}

/* --- session ------------------------------------------------------------ */

mvxc_session *mvxc_connect(const char *account, mvxc_status *st) {
    mvxc_session *s = calloc(1, sizeof *s);
    if (!s) { if (st) *st = MVXC_ERROR; return NULL; }
    s->ctx = mvx_ctx_create();
    if (!s->ctx) { free(s); if (st) *st = MVXC_ERROR; return NULL; }

    /* A CLIENT IS A RUNNING PROGRAM, NOT A PROMPT.  The shell sets its base
     * level to -1 so the first verb typed answers @LEVEL 0 -- "nothing is
     * above me, I own the screen".  A program reached through this library
     * must never answer that: the C caller owns the screen, and an interactive
     * routine that prompts has to be able to decline.  Base 0 makes the
     * session itself the program, so an mvxc_execute lands on 1, which is
     * exactly what EXECUTE ... CAPTURING gives from inside a program (#270,
     * and the reason mvx#264 needed it).  Set explicitly rather than left to
     * calloc, because it is a decision and not a default. */
    mvx_ctx_set_base_level(s->ctx, 0);

    if (account && *account && !mvx_logto(s->ctx, account)) {
        set_err(s, "cannot enter the account");
        mvx_ctx_destroy(s->ctx);
        free(s->err);
        free(s);
        if (st) *st = MVXC_NOTFOUND;
        return NULL;
    }
    if (st) *st = MVXC_OK;
    return s;
}

void mvxc_disconnect(mvxc_session *s) {
    if (!s) return;
    mvx_ctx_destroy(s->ctx);
    free(s->err);
    free(s->nextid);
    free(s);
}

const char *mvxc_error(mvxc_session *s) {
    return (s && s->err) ? s->err : "";
}

const char *mvxc_version(void) { return mvx_version(); }

/* --- values ------------------------------------------------------------- */

mvxc_val *mvxc_new(void) {
    mvxc_val *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    /* EMPTY STRING, NOT UNASSIGNED.  mv_init leaves MV_UNASSIGNED, which the
     * runtime warns about and coerces the first time anything reads it -- and
     * compiled BASIC never hands one out, because its variables are assigned
     * before use.  A caller of this library creates values constantly and then
     * passes them straight in (a fresh record to fill, a capture to receive
     * output), so unassigned is the normal case here and must not be.  "" is
     * also what a new MV value means. */
    mv_init(&v->v);
    mv_set_str(&v->v, "", 0);
    return v;
}

mvxc_val *mvxc_new_str(const char *s) {
    return mvxc_from_bytes(s, s ? strlen(s) : 0);
}

mvxc_val *mvxc_from_bytes(const char *p, size_t n) {
    mvxc_val *v = mvxc_new();
    if (v) mv_set_str(&v->v, p ? p : "", (int64_t)n);
    return v;
}

void mvxc_free(mvxc_val *v) {
    if (!v) return;
    drop_held(v);
    free(v->held);
    mv_clear(&v->v);
    free(v);
}

const char *mvxc_str(mvxc_val *v) {
    if (!v) return "";
    return chars_held(v, &v->v);
}

const char *mvxc_bytes(mvxc_val *v, size_t *n) {
    if (!v) { if (n) *n = 0; return ""; }
    char nb[40];
    const char *p;
    int64_t len = mv_val_chars(&v->v, nb, sizeof nb, &p);
    if (len < 0) len = 0;
    if (n) *n = (size_t)len;
    return hold(v, p, len);
}

static const char *extract(mvxc_val *v, int64_t a, int64_t m, int64_t sv) {
    if (!v) return "";
    mv_value out;
    mv_init(&out);
    mv_extract_fn(&out, &v->v, a, m, sv);
    const char *r = chars_held(v, &out);
    mv_clear(&out);
    return r;
}

const char *mvxc_attr  (mvxc_val *v, int a)               { return extract(v, a, 0, 0); }
const char *mvxc_val_at(mvxc_val *v, int a, int m)        { return extract(v, a, m, 0); }
const char *mvxc_sub   (mvxc_val *v, int a, int m, int s) { return extract(v, a, m, s); }

int mvxc_dcount(mvxc_val *v, int a) {
    if (!v) return 0;
    mv_value d, src;
    mv_init(&src);
    if (a > 0) {
        mv_extract_fn(&src, &v->v, a, 0, 0);
        tmp_str(&d, VM);
    } else {
        mv_copy(&src, &v->v);
        tmp_str(&d, AM);
    }
    int64_t n = mv_dcount_fn(&src, &d);
    mv_clear(&src);
    mv_clear(&d);
    return (int)n;
}

/* Every mutation releases the strings handed out before it: they described the
 * value as it was, and keeping them alive would make a stale read look live. */
static void replace_at(mvxc_val *v, int64_t a, int64_t m, int64_t sv,
                       const char *s) {
    if (!v) return;
    drop_held(v);
    mv_value nv;
    tmp_str(&nv, s);
    mv_replace_fn(&v->v, &v->v, a, m, sv, &nv);
    mv_clear(&nv);
}

void mvxc_set(mvxc_val *v, const char *s) {
    if (!v) return;
    drop_held(v);
    mv_set_str(&v->v, s ? s : "", s ? (int64_t)strlen(s) : 0);
}

void mvxc_set_attr(mvxc_val *v, int a, const char *s)        { replace_at(v, a, 0, 0, s); }
void mvxc_set_val (mvxc_val *v, int a, int m, const char *s) { replace_at(v, a, m, 0, s); }

void mvxc_ins_val(mvxc_val *v, int a, int m, const char *s) {
    if (!v) return;
    drop_held(v);
    mv_value nv;
    tmp_str(&nv, s);
    mv_insert_fn(&v->v, &v->v, a, m, 0, &nv);
    mv_clear(&nv);
}

void mvxc_del_val(mvxc_val *v, int a, int m) {
    if (!v) return;
    drop_held(v);
    mv_delete_fn(&v->v, &v->v, a, m, 0);
}

/* --- files and records --------------------------------------------------- */

mvxc_file *mvxc_open(mvxc_session *s, const char *name, const char *dict,
                     mvxc_status *st) {
    if (!s || !name) { if (st) *st = MVXC_ERROR; return NULL; }
    mvxc_file *f = calloc(1, sizeof *f);
    if (!f) { if (st) *st = MVXC_ERROR; return NULL; }
    f->s = s;
    mv_init(&f->fvar);

    mv_value d, sp;
    tmp_str(&d, dict ? dict : "");
    tmp_str(&sp, name);
    int64_t ok = mvx_open(s->ctx, &d, &sp, &f->fvar);
    mv_clear(&d);
    mv_clear(&sp);

    if (!ok) {
        set_err(s, "cannot open the file");
        mv_clear(&f->fvar);
        free(f);
        if (st) *st = MVXC_NOTFOUND;
        return NULL;
    }
    f->open = 1;
    if (st) *st = MVXC_OK;
    return f;
}

void mvxc_close(mvxc_file *f) {
    if (!f) return;
    if (f->open) mvx_close(f->s->ctx, &f->fvar);
    mv_clear(&f->fvar);
    free(f);
}

static mvxc_val *read_common(mvxc_file *f, const char *id, int64_t lock,
                             mvxc_status *st) {
    if (!f || !id) { if (st) *st = MVXC_ERROR; return NULL; }
    mvxc_val *v = mvxc_new();
    if (!v) { if (st) *st = MVXC_ERROR; return NULL; }
    mv_value i;
    tmp_str(&i, id);
    int64_t got = mvx_read(f->s->ctx, &v->v, &f->fvar, &i, lock);
    mv_clear(&i);
    if (!got) {
        /* A MISS IS NOT AN ERROR, and the distinction matters to a caller
         * deciding between "create it" and "something is wrong".  A lock held
         * elsewhere is reported apart for the same reason. */
        mvxc_free(v);
        if (st) *st = (lock && mvx_status(f->s->ctx) == 2) ? MVXC_LOCKED
                                                           : MVXC_NOTFOUND;
        return NULL;
    }
    if (st) *st = MVXC_OK;
    return v;
}

mvxc_val *mvxc_read(mvxc_file *f, const char *id, mvxc_status *st) {
    return read_common(f, id, 0, st);
}

mvxc_status mvxc_read_into(mvxc_file *f, const char *id, mvxc_val *dst) {
    if (!f || !id || !dst) return MVXC_ERROR;
    /* The strings dst handed out described what it used to hold, and it is
     * about to hold something else -- release them before the read, not after,
     * so a caller cannot see them survive a successful one. */
    drop_held(dst);
    mv_value i;
    tmp_str(&i, id);
    int64_t got = mvx_read(f->s->ctx, &dst->v, &f->fvar, &i, 0);
    mv_clear(&i);
    return got ? MVXC_OK : MVXC_NOTFOUND;
}

mvxc_val *mvxc_readu(mvxc_file *f, const char *id, int wait, mvxc_status *st) {
    return read_common(f, id, wait ? 1 : 2, st);
}

mvxc_status mvxc_write(mvxc_file *f, const char *id, mvxc_val *rec) {
    if (!f || !id || !rec) return MVXC_ERROR;
    mv_value i;
    tmp_str(&i, id);
    /* onerr = 1: a failed write must COME BACK, not take the process with it.
     * Without it the runtime calls mvx_fatal (mvx#245) and a host program dies
     * because a mapped column would not project. */
    int64_t ok = mvx_write(f->s->ctx, &rec->v, &f->fvar, &i, 0, 1);
    mv_clear(&i);
    if (ok < 0) { set_err(f->s, "the write was rolled back"); return MVXC_ERROR; }
    return MVXC_OK;
}

mvxc_status mvxc_delete(mvxc_file *f, const char *id) {
    if (!f || !id) return MVXC_ERROR;
    mv_value i;
    tmp_str(&i, id);
    int64_t gone = mvx_delete_rec(f->s->ctx, &f->fvar, &i);
    mv_clear(&i);
    return gone ? MVXC_OK : MVXC_NOTFOUND;
}

void mvxc_release(mvxc_file *f, const char *id) {
    if (!f || !id) return;
    mv_value i;
    tmp_str(&i, id);
    mvx_release(f->s->ctx, &f->fvar, &i);
    mv_clear(&i);
}

/* --- file administration -------------------------------------------------- */

mvxc_status mvxc_create_file(mvxc_session *s, const char *name,
                             const char *type) {
    if (!s || !name) return MVXC_ERROR;
    mv_value sp, ty;
    tmp_str(&sp, name);
    int64_t ok;
    if (type && *type) {
        tmp_str(&ty, type);
        ok = mvx_createfile(s->ctx, &sp, &ty);
        mv_clear(&ty);
    } else {
        ok = mvx_createfile(s->ctx, &sp, NULL);
    }
    mv_clear(&sp);
    if (!ok) { set_err(s, "the file could not be created"); return MVXC_ERROR; }
    return MVXC_OK;
}

mvxc_status mvxc_delete_file(mvxc_session *s, const char *name) {
    if (!s || !name) return MVXC_ERROR;
    mv_value sp;
    tmp_str(&sp, name);
    int64_t ok = mvx_deletefile(s->ctx, &sp);
    mv_clear(&sp);
    if (!ok) { set_err(s, "no such file"); return MVXC_NOTFOUND; }
    return MVXC_OK;
}

mvxc_val *mvxc_files(mvxc_session *s) {
    if (!s) return NULL;
    mvxc_val *v = mvxc_new();
    if (!v) return NULL;
    mvx_filelist(s->ctx, &v->v);
    return v;
}

int mvxc_openaccount(void) { return mvx_openaccount(); }

int mvxc_voc_class(const char *type) {
    if (!type) return 0;
    return mvx_voc_class(type, (int64_t)strlen(type));
}

int mvxc_cataloged(mvxc_session *s, const char *name) {
    (void)s;
    if (!name) return 0;
    mv_value nm;
    tmp_str(&nm, name);
    int64_t r = mv_cataloged_fn(&nm);
    mv_clear(&nm);
    return r ? 1 : 0;
}

/* --- the select list ------------------------------------------------------ */

mvxc_status mvxc_select(mvxc_file *f) {
    if (!f) return MVXC_ERROR;
    mvx_select(f->s->ctx, &f->fvar);
    return MVXC_OK;
}

const char *mvxc_next(mvxc_session *s) {
    if (!s) return NULL;
    mv_value id;
    mv_init(&id);
    int64_t got = mvx_readnext(s->ctx, &id);
    if (!got) { mv_clear(&id); return NULL; }
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(&id, nb, sizeof nb, &p);
    if (n < 0) n = 0;
    char *c = malloc((size_t)n + 1);
    if (c) { if (n) memcpy(c, p, (size_t)n); c[n] = '\0'; }
    mv_clear(&id);
    free(s->nextid);
    s->nextid = c;
    return c ? c : "";
}

/* --- running things -------------------------------------------------------- */

mvxc_status mvxc_call(mvxc_session *s, const char *name, int argc,
                      mvxc_val **argv) {
    if (!s || !name) return MVXC_ERROR;
    /* ASK BEFORE CALLING.  An unresolved CALL is a runtime error on mvx and
     * traps into the DEBUGGER on jBASE (mv_package#54); from a library it must
     * be a return value either way. */
    if (!mvxc_cataloged(s, name)) {
        set_err(s, "subroutine is not cataloged");
        return MVXC_NOTFOUND;
    }

    mv_value **av = NULL;
    if (argc > 0) {
        av = calloc((size_t)argc, sizeof *av);
        if (!av) return MVXC_ERROR;
        for (int i = 0; i < argc; i++) {
            if (!argv || !argv[i]) { free(av); return MVXC_ERROR; }
            /* the callee writes through these, and the caller sees it AFTER
             * the call returns -- never during, so a wire can copy them. */
            drop_held(argv[i]);
            av[i] = &argv[i]->v;
        }
    }
    mvx_call(s->ctx, name, argc, av);
    free(av);
    return MVXC_OK;
}

mvxc_status mvxc_execute(mvxc_session *s, const char *sentence,
                         mvxc_val *capture) {
    if (!s || !sentence) return MVXC_ERROR;
    mv_value sent, rc;
    tmp_str(&sent, sentence);
    tmp_str(&rc, "");            /* assigned, for the same reason */
    if (capture) drop_held(capture);
    /* The sentence runs a level above the session, so anything it reaches
     * answers @LEVEL >= 1 and an interactive routine knows to decline.  That
     * is mvx_execute's own behaviour; it works here because mvxc_connect made
     * the session a program rather than a prompt. */
    int64_t ok = mvx_execute(s->ctx, &sent, capture ? &capture->v : NULL, &rc);
    mv_clear(&sent);
    mv_clear(&rc);
    if (!ok) { set_err(s, "the sentence could not be run"); return MVXC_DENIED; }
    return MVXC_OK;
}
