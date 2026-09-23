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

/* PROCs: a VOC record that BUILDS a command and then runs it (mvx#271).
 *
 * The companion to mvx#269.  Where a paragraph IS a list of sentences, a PROC
 * is a small machine that assembles one character by character and executes
 * what it assembled.  It is the older Pick mechanism; paragraphs are the
 * later, friendlier replacement, and new code on the systems that have both
 * is not written in PROC.  It is here because an account ported from one of
 * them arrives carrying PROCs, and `cmd' framework code still uses them.
 *
 *     DPQ
 *     001 PQ
 *     002 S2          <- move the input pointer to word 2 of the command line
 *     003 HCOUNT      <- build "COUNT " into the output buffer
 *     004 A           <- copy the word under the pointer in after it
 *     005 P           <- execute what was built, and carry on
 *
 * MEASURED ON UniData 8.3 AND ScarletDME 2.6-6, which agree on every opcode
 * below -- so this is the language, not one system's reading of it:
 *
 *   - `O<text>' writes text to the terminal.
 *   - `H<text>' APPENDS to the output buffer; several H lines concatenate.
 *   - `P' executes the buffer AND RETURNS -- a line after it still runs, so a
 *     PROC may build and run several commands in turn.
 *   - `X' stops.
 *   - `A' copies the word under the input pointer into the buffer; `S<n>'
 *     puts the pointer on word n.  WORD 1 IS THE VERB, so a PROC's first
 *     argument is word 2 -- `S2' is the common opening line.
 *   - `IF A<n> = <value> <command>' runs <command> when it matches, and falls
 *     through when it does not.  `IF # A<n> <command>' tests for absent.
 *   - a line may begin with a numeric LABEL (`10 Omatched'), and `GO <label>'
 *     jumps to it.
 *
 * NOT IMPLEMENTED, deliberately: the secondary input buffer, `T' terminal
 * control, `STON'/`STOFF' input stacking, and the `[]' call.  They are the
 * parts of PROC that exist because it had no other way to do things MVX
 * already does, and nothing measured needed them.  An unknown opcode SAYS SO
 * rather than being skipped, so a PROC that needs one fails loudly instead of
 * doing half its job.
 */

#include "mvx_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_WORDS 64
#define OUT_CAP   4096

typedef struct {
    char out[OUT_CAP];          /* the command being built */
    size_t olen;
    char *word[MAX_WORDS];      /* the invoking sentence, word 1 = the verb */
    int nwords;
    int ptr;                    /* input pointer, 1-based */
} proc_state;

static void out_append(proc_state *st, const char *s) {
    while (*s && st->olen + 1 < OUT_CAP) st->out[st->olen++] = *s++;
    st->out[st->olen] = '\0';
}

static const char *word_at(proc_state *st, int n) {
    if (n < 1 || n > st->nwords) return "";
    return st->word[n - 1];
}

static void split_sentence(proc_state *st, const char *sentence) {
    st->nwords = 0;
    const char *p = sentence;
    while (*p && st->nwords < MAX_WORDS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t n = (size_t)(p - s);
        char *w = malloc(n + 1);
        if (!w) break;
        memcpy(w, s, n);
        w[n] = '\0';
        st->word[st->nwords++] = w;
    }
}

static void free_words(proc_state *st) {
    for (int i = 0; i < st->nwords; i++) free(st->word[i]);
    st->nwords = 0;
}

static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Run the buffer as a sentence, then empty it -- which is what the systems do:
   a second P with nothing built runs nothing. */
static void do_execute(mvx_ctx *ctx, proc_state *st) {
    if (st->olen == 0) return;
    mv_value s, rc;
    mv_init(&s);
    mv_init(&rc);
    mv_set_str(&s, st->out, (int64_t)st->olen);
    int aborted = 0;
    mvx_execute_trapping(ctx, &s, NULL, &rc, &aborted);
    mv_clear(&s);
    mv_clear(&rc);
    st->olen = 0;
    st->out[0] = '\0';
}

/* `A<n>' names a word outright; a bare `A' takes the one under the pointer and
   steps past it, which is how a PROC walks its arguments. */
static void do_copy(proc_state *st, const char *arg) {
    arg = skip_ws(arg);
    if (isdigit((unsigned char)*arg)) {
        out_append(st, word_at(st, atoi(arg)));
        return;
    }
    out_append(st, word_at(st, st->ptr));
    st->ptr++;
}

/* The condition of an IF, measured in two forms:
     IF A2 = YES <command>        matches word 2 against a literal
     IF # A2 <command>            true when word 2 is absent or empty
   Returns 1 when the command should run, and points *rest at it. */
static int if_holds(proc_state *st, const char *s, const char **rest) {
    s = skip_ws(s);
    int negate_null = 0;
    if (*s == '#') { negate_null = 1; s = skip_ws(s + 1); }
    if (toupper((unsigned char)*s) != 'A') return -1;     /* not a form we know */
    s++;
    int n = atoi(s);
    while (isdigit((unsigned char)*s)) s++;
    const char *val = word_at(st, n);
    s = skip_ws(s);

    if (negate_null) { *rest = s; return val[0] == '\0'; }
    if (*s == '=' || *s == '#') {
        int want_equal = (*s == '=');
        s = skip_ws(s + 1);
        char lit[256];
        size_t i = 0;
        while (*s && *s != ' ' && *s != '\t' && i + 1 < sizeof lit) lit[i++] = *s++;
        lit[i] = '\0';
        *rest = skip_ws(s);
        int same = strcmp(val, lit) == 0;
        return want_equal ? same : !same;
    }
    *rest = s;                      /* `IF A2 <command>' -- true when present */
    return val[0] != '\0';
}

static void run_line(mvx_ctx *ctx, proc_state *st, const char *line,
                     const char *name, int *stop, int *jump_to);

/* One opcode.  Split out so IF can run the command that follows its test. */
static void run_line(mvx_ctx *ctx, proc_state *st, const char *line,
                     const char *name, int *stop, int *jump_to) {
    line = skip_ws(line);
    if (!*line || *line == '*') return;

    char op = (char)toupper((unsigned char)line[0]);
    const char *arg = line + 1;

    if (strncasecmp(line, "IF", 2) == 0 &&
        (line[2] == ' ' || line[2] == '\t')) {
        const char *rest = NULL;
        int held = if_holds(st, line + 3, &rest);
        if (held < 0) {
            fprintf(stderr, "In proc %s: IF form not understood: %s\n", name, line);
            return;
        }
        if (held && rest) run_line(ctx, st, rest, name, stop, jump_to);
        return;
    }
    if (strncasecmp(line, "GO", 2) == 0 &&
        (line[2] == ' ' || line[2] == '\t')) {
        *jump_to = atoi(skip_ws(line + 3));
        return;
    }
    if (strncasecmp(line, "RI", 2) == 0) { st->ptr = 1; return; }
    if (strncasecmp(line, "RO", 2) == 0) { st->olen = 0; st->out[0] = '\0'; return; }

    switch (op) {
        case 'O': printf("%s\n", arg); fflush(stdout); return;
        case 'H': out_append(st, arg); return;
        case 'A': do_copy(st, arg); return;
        case 'S': st->ptr = atoi(skip_ws(arg)); return;
        case 'P': do_execute(ctx, st); return;
        case 'X': *stop = 1; return;
        default:
            /* SAY SO.  A PROC quietly skipping what it cannot do would run
               half its job and report success, which is worse than stopping. */
            fprintf(stderr, "In proc %s: unknown opcode: %s\n", name, line);
            return;
    }
}

/* A line may carry a numeric label: `10 Omatched'.  Returns the label, or 0,
   and points *body at what follows it. */
static int line_label(const char *line, const char **body) {
    const char *s = skip_ws(line);
    if (!isdigit((unsigned char)*s)) { *body = line; return 0; }
    int lab = atoi(s);
    while (isdigit((unsigned char)*s)) s++;
    *body = skip_ws(s);
    return lab;
}

void mvx_proc_exec(mvx_ctx *ctx, const char *name, const mv_value *rec,
                   const char *sentence) {
    proc_state st;
    memset(&st, 0, sizeof st);
    st.ptr = 1;
    split_sentence(&st, sentence ? sentence : name);

    mv_value am;
    mv_init(&am);
    { char m = (char)0xFE; mv_set_str(&am, &m, 1); }
    int64_t nattr = mv_dcount_fn(rec, &am);
    mv_clear(&am);

    int stop = 0;
    int64_t i = 2;
    int guard = 0;                  /* a GO loop must not hang the session */
    while (i <= nattr && !stop && guard++ < 100000) {
        mv_value line;
        mv_init(&line);
        mv_extract_fn(&line, rec, i, 0, 0);
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(&line, nb, sizeof nb, &p);
        char raw[OUT_CAP];
        snprintf(raw, sizeof raw, "%.*s", (int)n, p);
        mv_clear(&line);

        const char *body;
        line_label(raw, &body);
        int jump_to = 0;
        run_line(ctx, &st, body, name, &stop, &jump_to);

        if (jump_to) {              /* find the attribute carrying that label */
            int64_t target = 0;
            for (int64_t j = 2; j <= nattr; j++) {
                mv_value l2;
                mv_init(&l2);
                mv_extract_fn(&l2, rec, j, 0, 0);
                char nb2[40];
                const char *p2;
                int64_t n2 = mv_val_chars(&l2, nb2, sizeof nb2, &p2);
                char r2[256];
                snprintf(r2, sizeof r2, "%.*s", (int)n2, p2);
                mv_clear(&l2);
                const char *b2;
                if (line_label(r2, &b2) == jump_to) { target = j; break; }
            }
            /* TO the labelled line, not past it: `10 Omatched' carries the
               label AND the opcode, so landing after it would skip the thing
               the jump exists to reach. */
            if (target) { i = target; continue; }
            fprintf(stderr, "In proc %s: no label %d\n", name, jump_to);
        }
        i++;
    }
    if (guard >= 100000)
        fprintf(stderr, "In proc %s: stopped after 100000 steps\n", name);
    free_words(&st);
}
