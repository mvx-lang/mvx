/*
 * MVX — a native compiler and runtime for Pick/MultiValue BASIC.
 * Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only (see LICENSE).
 */

/* Records as documents (#157): the record -> document -> record round trip.
 *
 * The acceptance criterion for this representation is simply that a record
 * comes back as it was written, so that is what these assert — byte-exact,
 * including the cases that are easy to get subtly wrong:
 *
 *   ALIGNMENT.  An interior or leading empty is not "nothing": dropping one
 *   shifts every value after it, and the record still looks well-formed
 *   afterwards, which is what makes it dangerous.
 *
 *   THE BASE64 WRAPPER.  A value whose bytes are not UTF-8 is wrapped so it is
 *   self-describing.  The test that matters is the converse: a LITERAL value
 *   that happens to look like base64 must stay text and come back unchanged.
 *   Stored as a bare string, nothing could tell the two apart.
 *
 *   TEXT STAYS TEXT.  Valid UTF-8 goes in as its own bytes, not \u escapes —
 *   the whole point is a field the backend can compare and index, so the
 *   document itself is asserted, not only the round trip.
 */
#include "mvx_doc.h"
#include "mvx_map.h"
#include <stdio.h>
#include <string.h>

#define AM "\xFE"
#define VM "\xFD"
#define SM "\xFC"

static int pass = 0, fail = 0;

static void show(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xFE) fputs("^", stdout);
        else if (c == 0xFD) fputs("]", stdout);
        else if (c == 0xFC) fputs("\\", stdout);
        else if (c < 0x20 || c == 0x7f) printf("<%02x>", c);
        else putchar((int)c);
    }
}

/* rec -> doc -> rec, asserting the bytes are identical. */
static void trip(const char *name, const char *in, size_t inlen) {
    mv_value rec, doc, back;
    mv_init(&rec); mv_init(&doc); mv_init(&back);
    mv_set_str(&rec, in, (int64_t)inlen);
    mvx_doc_encode(&doc, &rec);
    mvx_doc_decode(&back, &doc);

    char nb[64]; const char *bp;
    int64_t bl = mv_val_chars(&back, nb, sizeof nb, &bp);
    char nb2[64]; const char *dp;
    int64_t dl = mv_val_chars(&doc, nb2, sizeof nb2, &dp);

    if (bl == (int64_t)inlen && memcmp(bp, in, inlen) == 0) {
        pass++;
        printf("  ok   %-34s -> %.*s\n", name, (int)dl, dp);
    } else {
        fail++;
        printf("  FAIL %s\n       in   [", name); show(in, inlen);
        printf("]\n       doc  [%.*s]\n       back [", (int)dl, dp);
        show(bp, (size_t)bl); printf("]\n");
    }
    mv_clear(&rec); mv_clear(&doc); mv_clear(&back);
}

/* The document text itself, for the properties a round trip cannot show. */
static void doc_has(const char *name, const char *in, size_t inlen,
                    const char *want) {
    mv_value rec, doc;
    mv_init(&rec); mv_init(&doc);
    mv_set_str(&rec, in, (int64_t)inlen);
    mvx_doc_encode(&doc, &rec);
    char nb[64]; const char *dp;
    int64_t dl = mv_val_chars(&doc, nb, sizeof nb, &dp);
    if (dl == (int64_t)strlen(want) && memcmp(dp, want, (size_t)dl) == 0) {
        pass++; printf("  ok   %-34s %s\n", name, want);
    } else {
        fail++;
        printf("  FAIL %s\n       want [%s]\n       got  [%.*s]\n",
               name, want, (int)dl, dp);
    }
    mv_clear(&rec); mv_clear(&doc);
}

#define T(name, lit) trip(name, lit, sizeof(lit) - 1)
#define D(name, lit, want) doc_has(name, lit, sizeof(lit) - 1, want)


/* ------------------------------------------------------------ mapped form */

/* Build a %MAP% spec: fields are name VM attr VM conv VM type VM assoc,
   separated by AM. */
static void spec_add(mv_value *spec, const char *name, int attr,
                     const char *conv, const char *type, const char *assoc) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "%s\xFD%d\xFD%s\xFD%s\xFD%s",
                     name, attr, conv, type, assoc);
    char nb[64]; const char *p;
    int64_t have = mv_val_chars(spec, nb, sizeof nb, &p);
    char all[2048];
    int al = 0;
    if (have > 0) { memcpy(all, p, (size_t)have); al = (int)have; all[al++] = (char)0xFE; }
    memcpy(all + al, buf, (size_t)n); al += n;
    mv_set_str(spec, all, al);
}

static void mtrip(const char *name, const char *in, size_t inlen,
                  mv_value *spec) {
    mv_value rec, doc, back;
    mv_init(&rec); mv_init(&doc); mv_init(&back);
    mv_set_str(&rec, in, (int64_t)inlen);
    mvx_doc_encode_mapped(NULL, &doc, &rec, spec);
    mvx_doc_decode_mapped(NULL, &back, &doc, spec);
    char nb[64]; const char *bp; int64_t bl = mv_val_chars(&back, nb, sizeof nb, &bp);
    char nb2[64]; const char *dp; int64_t dl = mv_val_chars(&doc, nb2, sizeof nb2, &dp);
    if (bl == (int64_t)inlen && memcmp(bp, in, inlen) == 0) {
        pass++; printf("  ok   %-34s -> %.*s\n", name, (int)dl, dp);
    } else {
        fail++;
        printf("  FAIL %s\n       in   [", name); show(in, inlen);
        printf("]\n       doc  [%.*s]\n       back [", (int)dl, dp);
        show(bp, (size_t)bl); printf("]\n");
    }
    mv_clear(&rec); mv_clear(&doc); mv_clear(&back);
}

static void mdoc(const char *name, const char *in, size_t inlen,
                 mv_value *spec, const char *want) {
    mv_value rec, doc;
    mv_init(&rec); mv_init(&doc);
    mv_set_str(&rec, in, (int64_t)inlen);
    mvx_doc_encode_mapped(NULL, &doc, &rec, spec);
    char nb[64]; const char *dp; int64_t dl = mv_val_chars(&doc, nb, sizeof nb, &dp);
    if (dl == (int64_t)strlen(want) && memcmp(dp, want, (size_t)dl) == 0) {
        pass++; printf("  ok   %-34s %s\n", name, want);
    } else {
        fail++;
        printf("  FAIL %s\n       want [%s]\n       got  [%.*s]\n",
               name, want, (int)dl, dp);
    }
    mv_clear(&rec); mv_clear(&doc);
}

#define MT(name, lit, spec) mtrip(name, lit, sizeof(lit) - 1, spec)
#define MD(name, lit, spec, want) mdoc(name, lit, sizeof(lit) - 1, spec, want)

static void mapped(void) {
    mv_value spec;
    mv_init(&spec);
    mv_set_str(&spec, "", 0);
    spec_add(&spec, "CUST", 1, "", "TEXT", "");
    spec_add(&spec, "QTY",   2, "", "TEXT", "LINE_ITEMS");
    spec_add(&spec, "PRICE", 3, "", "TEXT", "LINE_ITEMS");

    printf("doc-roundtrip: the mapped form\n");
    MD("names come from the dictionary", "C1" AM "5" AM "10", &spec,
       "{\"CUST\":\"C1\",\"LINE_ITEMS\":[{\"QTY\":\"5\",\"PRICE\":\"10\"}]}");
    MT("and it round-trips",            "C1" AM "5" AM "10", &spec);
    MD("an association is rows of objects",
       "C1" AM "5" VM "6" AM "10" VM "20", &spec,
       "{\"CUST\":\"C1\",\"LINE_ITEMS\":[{\"QTY\":\"5\",\"PRICE\":\"10\"},"
       "{\"QTY\":\"6\",\"PRICE\":\"20\"}]}");
    MT("rows round-trip",               "C1" AM "5" VM "6" AM "10" VM "20", &spec);

    printf("doc-roundtrip: the ragged association — the dangerous one\n");
    /* QTY has three values, PRICE two.  The association has THREE rows: MV pads
       the short member.  Zipping until the first runs out would drop the third
       line item and leave a well-formed record behind. */
    MD("the row count is the maximum",
       "C1" AM "5" VM "6" VM "7" AM "10" VM "20", &spec,
       "{\"CUST\":\"C1\",\"LINE_ITEMS\":[{\"QTY\":\"5\",\"PRICE\":\"10\"},"
       "{\"QTY\":\"6\",\"PRICE\":\"20\"},{\"QTY\":\"7\",\"PRICE\":\"\"}]}");
    MT("and the third line item survives",
       "C1" AM "5" VM "6" VM "7" AM "10" VM "20", &spec);
    MT("the short member may be the first",
       "C1" AM "5" VM "6" AM "10" VM "20" VM "30", &spec);
    MT("an interior empty in a member",
       "C1" AM "5" VM VM "7" AM "10" VM "20" VM "30", &spec);

    printf("doc-roundtrip: attributes the mapping does not name\n");
    /* They keep their ordinal keys, which is what lets native mode drop the
       blob: holding the un-mapped attributes is the blob's remaining job. */
    MD("unmapped attributes keep ordinals",
       "C1" AM "5" AM "10" AM "extra", &spec,
       "{\"CUST\":\"C1\",\"LINE_ITEMS\":[{\"QTY\":\"5\",\"PRICE\":\"10\"}],"
       "\"4\":\"extra\"}");
    MT("and they round-trip",           "C1" AM "5" AM "10" AM "extra", &spec);
    MT("including a multivalued one",
       "C1" AM "5" AM "10" AM "a" VM "b", &spec);
    mv_clear(&spec);

    printf("doc-roundtrip: every value is a string, whatever the type says\n");
    /* The document is the only copy, so a value has to come back as written.
       A JSON number cannot promise that (007 -> 7, -0 -> 0, and 9.90 -> 9.9 on
       a backend whose JSON numbers are doubles), so nothing is stored as one.
       Ordering moves to the query, where the value is cast — always guarded. */
    mv_value ns;
    mv_init(&ns); mv_set_str(&ns, "", 0);
    spec_add(&ns, "N", 1, "", "NUMERIC", "");
    MD("a plain integer is still text",  "990",  &ns, "{\"N\":\"990\"}");
    MD("and a decimal keeps its scale",  "9.90", &ns, "{\"N\":\"9.90\"}");
    MD("a leading zero survives",        "007",  &ns, "{\"N\":\"007\"}");
    MD("so does minus zero",             "-0",   &ns, "{\"N\":\"-0\"}");
    /* These three used to be mangled or lost by the coercion to the declared
       type — 1e3 became 13, +5 became 5, and N/A did not fit at all and
       arrived EMPTY.  With no blob behind the document that last one was a
       value gone for good, which is why nothing is coerced on the way in. */
    MD("an exponent is not rewritten",   "1e3",  &ns, "{\"N\":\"1e3\"}");
    MD("nor a leading plus",             "+5",   &ns, "{\"N\":\"+5\"}");
    MD("a non-number is KEPT, not lost", "N/A",  &ns, "{\"N\":\"N/A\"}");
    MT("990 round-trips",  "990",  &ns);
    MT("9.90 round-trips", "9.90", &ns);
    MT("007 round-trips",  "007",  &ns);
    MT("-0 round-trips",   "-0",   &ns);
    MT("1e3 round-trips",  "1e3",  &ns);
    MT("+5 round-trips",   "+5",   &ns);
    MT("N/A round-trips",  "N/A",  &ns);
    mv_clear(&ns);

    /* A TEXT field stores exactly the same thing, which is the point: the
       declared type no longer changes what is stored, only how it is read. */
    mv_value ts;
    mv_init(&ts); mv_set_str(&ts, "", 0);
    spec_add(&ts, "N", 1, "", "TEXT", "");
    MD("TEXT stores the same bytes",     "9.90", &ts, "{\"N\":\"9.90\"}");
    MD("and the same for a non-number",  "N/A",  &ts, "{\"N\":\"N/A\"}");
    mv_clear(&ts);
}

int main(void) {
    printf("doc-roundtrip: the shape\n");
    D("a single value is a scalar",   "Ada",            "{\"1\":\"Ada\"}");
    D("a multivalue is a list",       "5" VM "6" VM "7",
      "{\"1\":[\"5\",\"6\",\"7\"]}");
    /* NOTE the second element is a scalar, where #157's illustration writes
       ["20"].  scalar-when-one is stated there as a rule and the nesting
       example predates it; applied uniformly it gives one canonical form
       instead of two spellings.  Raised on the issue. */
    D("subvalues nest",               "10" SM "11" VM "20",
      "{\"1\":[[\"10\",\"11\"],\"20\"]}");
    D("one value with subvalues keeps its list", "x" SM "y",
      "{\"1\":[[\"x\",\"y\"]]}");
    D("two values do not",            "x" VM "y",
      "{\"1\":[\"x\",\"y\"]}");
    D("attributes are ordinals",      "Ada" AM "5" VM "6",
      "{\"1\":\"Ada\",\"2\":[\"5\",\"6\"]}");

    printf("doc-roundtrip: alignment — the empties that hold positions apart\n");
    T("an interior empty",            "a" VM VM "c");
    T("a leading empty",              VM "b");
    T("two interior empties",         "a" VM VM VM "d");
    T("an interior empty subvalue",   "x" SM SM "z");
    T("an empty attribute in the middle", "a" AM AM "c");
    D("the interior empty is a position", "a" VM VM "c",
      "{\"1\":[\"a\",\"\",\"c\"]}");

    printf("doc-roundtrip: values\n");
    T("plain text",                   "Ada Lovelace");
    T("an empty record",              "");
    T("quotes and backslash",         "he said \"hi\"\\");
    T("a newline and a tab",          "a\nb\tc");
    T("utf-8 text",                   "A \xE2\x80\x94 B");
    D("utf-8 stays text, not escapes","caf\xC3\xA9",  "{\"1\":\"caf\xC3\xA9\"}");

    printf("doc-roundtrip: bytes that are not text\n");
    T("invalid utf-8",                "\xDE\xAD\xBE\xEF");
    T("a lone continuation byte",     "\x80");
    T("a truncated sequence",         "\xE2\x80");
    /* 0xFE/0xFD/0xFC are the marks — they ARE the record structure, so they
       cannot occur inside a value and binary test data must avoid them.  An
       earlier draft of this test used \xFF\xFE and was quietly asserting on a
       two-attribute record. */
    D("it wraps, per value",          "ok" VM "\xFF",
      "{\"1\":[\"ok\",{\"$b64\":\"/w==\"}]}");
    T("binary beside text",           "ok" VM "\xFF" VM "fine");
    T("a valid 2-byte sequence is text", "\xDE\xAD");   /* U+07AD, not binary */

    printf("doc-roundtrip: the wrapper is unambiguous\n");
    /* Exactly the value that a bare-string encoding could not tell from an
       encoded one.  It must stay text and come back as itself. */
    T("a literal that looks like b64", "REVBRA==");
    D("and it is stored as text",      "REVBRA==", "{\"1\":\"REVBRA==\"}");
    T("a literal $b64-ish string",     "{\"$b64\":\"x\"}");

    mapped();

    printf("\ndoc-roundtrip: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
