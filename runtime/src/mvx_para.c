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

/* Paragraphs: a VOC record that IS a list of sentences (mvx#269).
 *
 * MVX had one executable VOC record type -- `V', naming a compiled program.
 * UniData and UniVerse also have paragraphs, and an account ported from
 * either arrives carrying them.  mvx#264 made it concrete: a LOGIN on those
 * systems is a paragraph, because a cataloged program cannot be one at all on
 * UniData, where CATALOG writes to CTLG and creates no VOC record.
 *
 * A paragraph is not a new calling convention -- it is invoked as a sentence,
 * exactly like a verb -- so everything that runs a sentence gets paragraphs at
 * once: the prompt, EXECUTE, and mvx#264's LOGIN.
 *
 * MEASURED ON UniData 8.3 rather than invented, because every one of these
 * was a guess I would otherwise have got wrong:
 *
 *   - sentences are NOT echoed.  A paragraph of two DISPLAYs printed two
 *     lines, not four.  (The `COUNT CLIENTS' echo that made me think
 *     otherwise came from COUNT itself.)
 *   - `*' begins a comment line, skipped.
 *   - `<<text>>' PROMPTS THE OPERATOR with `text', and the answer is
 *     substituted where it stood.  It is not parameter substitution: a
 *     paragraph containing `<<%1>>' invoked as `PARA HELLO' prompted with the
 *     literal `%1' and ate the next line of input.
 *   - THE SAME PROMPT IS ASKED ONCE.  `<<Say it>> and <<Say it>>' asked one
 *     question and used the answer twice.
 *   - A SENTENCE THAT FAILS DOES NOT STOP THE PARAGRAPH.  An unknown verb in
 *     the middle reported, naming the paragraph, and the next line still ran.
 *   - paragraphs nest: one may invoke another.
 */

#include "mvx_runtime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ANSWERS 32

typedef struct {
    char *prompt[MAX_ANSWERS];
    char *answer[MAX_ANSWERS];
    int n;
} answers;

static void answers_free(answers *a) {
    for (int i = 0; i < a->n; i++) { free(a->prompt[i]); free(a->answer[i]); }
    a->n = 0;
}

/* ASK ONCE PER DISTINCT PROMPT.  Measured: the same text twice is one
   question, and the answer stands wherever it appeared. */
static const char *answer_for(mvx_ctx *ctx, answers *a, const char *prompt) {
    for (int i = 0; i < a->n; i++)
        if (strcmp(a->prompt[i], prompt) == 0) return a->answer[i];

    fputs(prompt, stdout);
    fputc(' ', stdout);
    fflush(stdout);
    mv_value in;
    mv_init(&in);
    mv_input(ctx, &in);
    char nb[40];
    const char *p;
    int64_t n = mv_val_chars(&in, nb, sizeof nb, &p);
    char *got = malloc((size_t)n + 1);
    if (got) { memcpy(got, p, (size_t)n); got[n] = '\0'; }
    mv_clear(&in);
    if (!got) return "";

    if (a->n >= MAX_ANSWERS) {          /* keep answering, stop remembering */
        static char last[256];
        snprintf(last, sizeof last, "%s", got);
        free(got);
        return last;
    }
    a->prompt[a->n] = strdup(prompt);
    a->answer[a->n] = got;
    if (!a->prompt[a->n]) { free(got); return ""; }
    return a->answer[a->n++];
}

/* Replace every <<text>> with what the operator said to `text'. */
static void substitute(mvx_ctx *ctx, answers *a, const char *in,
                       char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < cap; ) {
        if (p[0] == '<' && p[1] == '<') {
            const char *end = strstr(p + 2, ">>");
            if (end) {
                char prompt[256];
                size_t n = (size_t)(end - (p + 2));
                if (n >= sizeof prompt) n = sizeof prompt - 1;
                memcpy(prompt, p + 2, n);
                prompt[n] = '\0';
                const char *ans = answer_for(ctx, a, prompt);
                while (*ans && o + 1 < cap) out[o++] = *ans++;
                p = end + 2;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

static int blank_or_comment(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return *s == '\0' || *s == '*';
}

/* Run `rec' as a paragraph.  Its name is only for the message an unhandled
   sentence produces, which is what UniData prints too ("In Paragraph: X."). */
static void para_exec(mvx_ctx *ctx, const char *name, const mv_value *rec) {
    answers a;
    a.n = 0;
    /* How many attributes.  mv_dcount_fn needs the mark to count, and takes
       no NULL for it -- val_span would dereference it. */
    mv_value am;
    mv_init(&am);
    { char m = (char)0xFE; mv_set_str(&am, &m, 1); }
    int64_t nattr = mv_dcount_fn(rec, &am);
    mv_clear(&am);
    for (int64_t i = 2; i <= nattr; i++) {
        mv_value line;
        mv_init(&line);
        mv_extract_fn(&line, rec, i, 0, 0);
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(&line, nb, sizeof nb, &p);
        char raw[4096];
        snprintf(raw, sizeof raw, "%.*s", (int)n, p);
        mv_clear(&line);
        if (blank_or_comment(raw)) continue;

        char sent[4096];
        substitute(ctx, &a, raw, sent, sizeof sent);
        if (blank_or_comment(sent)) continue;

        /* A SENTENCE THAT FAILS DOES NOT STOP THE PARAGRAPH (measured).  The
           message names the paragraph, because "Not a verb" on its own leaves
           an operator hunting for what ran it. */
        mv_value s, rc;
        mv_init(&s);
        mv_init(&rc);
        mv_set_str(&s, sent, (int64_t)strlen(sent));
        int aborted = 0;
        if (!mvx_execute_trapping(ctx, &s, NULL, &rc, &aborted) || aborted)
            fprintf(stderr, "In paragraph %s: %s\n", name, sent);
        mv_clear(&s);
        mv_clear(&rc);
    }
    answers_free(&a);
}

/* Is `verb' a paragraph, and if so run it.  1 handled, 0 not a paragraph.
   `local_only' restricts the search to the account's own VOC, which is what
   LOGIN needs (mvx#264). */
int mvx_para_try(mvx_ctx *ctx, const char *verb, int local_only) {
    mv_value rec;
    mv_init(&rec);
    int is_para = 0;
    if (mvx_voc_record(ctx, verb, &rec, local_only)) {
        mv_value a1;
        mv_init(&a1);
        mv_extract_fn(&a1, &rec, 1, 0, 0);
        char nb[40];
        const char *p;
        int64_t n = mv_val_chars(&a1, nb, sizeof nb, &p);
        if (n >= 2 && toupper((unsigned char)p[0]) == 'P' &&
            toupper((unsigned char)p[1]) == 'A')
            is_para = 1;
        mv_clear(&a1);
    }
    if (is_para) para_exec(ctx, verb, &rec);
    mv_clear(&rec);
    return is_para;
}
