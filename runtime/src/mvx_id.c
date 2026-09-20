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
/* Record ids as text (mvx#236).
 *
 * An MV id is a byte string: usually a key somebody typed, occasionally
 * something with a mark or a raw byte in it.  Stored as raw bytes it
 * round-trips perfectly and reads as X'494E562D323032362D303031' in every
 * tool that looks at the database -- which defeats much of the point of
 * putting the data somewhere people can query.
 *
 * So an id is stored as text, and THE ONLY BYTES ESCAPED ARE THE ONES THE
 * DATABASE CANNOT CARRY IN THE CHARACTER SET IT IS CURRENTLY USING.  That is
 * a property of the connection, not a guess: a UTF-8 database keeps every
 * valid UTF-8 sequence as itself and escapes only what is not one; a
 * single-byte database (LATIN1, SQL_ASCII, latin1, binary) keeps every byte.
 *
 *     INV-2026-001   ->  INV-2026-001
 *     Café           ->  Café              (valid UTF-8, so it stays)
 *     A<VM>B         ->  A%FDB             (0xFD is not UTF-8)
 *     A<VM>B         ->  A<VM>B            (... on a LATIN1 database)
 *     50% OFF        ->  50%25 OFF
 *
 * Two bytes are escaped whatever the character set:
 *
 *   '%'   because that is what makes decoding unambiguous.  Every id goes
 *         through the encoding, and a literal '%' is written '%25', so
 *         decoding is always the same operation and nothing has to mark an
 *         id as encoded or not.
 *   NUL   because no text column in any of the four backends carries one:
 *         SQLite truncates, Postgres rejects, MySQL and BSON are lax but
 *         every client in front of them is not.
 *
 * And one byte is escaped in one position: A TRAILING SPACE, as `%20'.  It is
 * perfectly representable; it is not reliably COMPARABLE, because a PAD SPACE
 * collation -- which is most of MySQL's, including utf8mb4_bin -- reads `A'
 * and `A ' as the same key.  Escaping only the last one is enough to keep
 * every distinct id distinct (no encoded id can then end in a space at all)
 * and costs three characters on the handful of ids that have one.
 *
 * Base64 was considered and rejected -- it would make every id unreadable to
 * solve a problem that affects almost none of them.
 *
 * THE CHARACTER SET IS A PROPERTY OF THE STORED DATA, NOT OF THE SESSION.
 * Decoding never depends on it -- percent-decoding is one operation, so an id
 * written under any character set reads back byte-exact forever.  Encoding
 * does depend on it, and that is where a changed character set would bite: an
 * id stored as the raw byte 0xFD under LATIN1 would be looked up as `%FD'
 * once the same database were UTF-8, and the row would simply not be found.
 * So a driver asks what the DATABASE is (postgres: server_encoding, which
 * cannot be altered after the database is created; mysql: the id column's own
 * charset), never what this connection happens to be set to, and pins its
 * session to match.  The class is stamped beside the file's format, and a file
 * whose stamp disagrees with the database is refused rather than half-read --
 * see mvx_id_csname.
 *
 * The encoding is the STORAGE form only.  Above the driver an id is bytes, as
 * it always was; nothing in the runtime or a BASIC program sees this.
 */

#include "mvx_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char hex[] = "0123456789ABCDEF";

/* How many bytes a UTF-8 sequence starting with this byte claims, or 0 when
   it cannot start one. */
static int utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 0;
}

static int utf8_valid(const unsigned char *p, int64_t left, int n) {
    if (n <= 0 || left < n) return 0;
    for (int i = 1; i < n; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    /* Reject an overlong form: it decodes to a character that has a shorter
       spelling, so keeping it would mean two encodings of one id. */
    if (n == 2 && (p[0] & 0x1E) == 0) return 0;
    if (n == 3 && p[0] == 0xE0 && (p[1] & 0x20) == 0) return 0;
    if (n == 4 && p[0] == 0xF0 && (p[1] & 0x30) == 0) return 0;
    return 1;
}

/* mvx_id_charset: name the character set a connection reports.  Anything
   unrecognised is treated as ASCII-only, which is the safe direction: a few
   more escapes, never a byte the database will not take. */
int mvx_id_charset(const char *name) {
    if (!name || !*name) return MVX_ID_CS_ASCII;
    if (strcasecmp(name, "UTF8") == 0 || strcasecmp(name, "UTF-8") == 0 ||
        strcasecmp(name, "utf8mb4") == 0 || strcasecmp(name, "utf8mb3") == 0 ||
        strcasecmp(name, "unicode") == 0)
        return MVX_ID_CS_UTF8;
    /* Every byte is a character: nothing needs escaping at all. */
    if (strcasecmp(name, "SQL_ASCII") == 0 || strcasecmp(name, "binary") == 0 ||
        strncasecmp(name, "LATIN", 5) == 0 || strncasecmp(name, "WIN", 3) == 0 ||
        strncasecmp(name, "ISO_8859", 8) == 0 ||
        strncasecmp(name, "KOI8", 4) == 0 || strcasecmp(name, "ascii") == 0)
        return strcasecmp(name, "ascii") == 0 ? MVX_ID_CS_ASCII
                                              : MVX_ID_CS_BYTE;
    return MVX_ID_CS_ASCII;
}

/* The stable name of a character set CLASS, for stamping beside a file's
   format.  Deliberately the class and not the vendor's name: LATIN1 and
   WIN1252 encode an id identically, so recording "latin1" would make a
   harmless SET look like a change of format. */
const char *mvx_id_csname(int cs) {
    return cs == MVX_ID_CS_UTF8 ? "utf8"
         : cs == MVX_ID_CS_BYTE ? "byte"
                                : "ascii";
}

/* mvx_id_encode: bytes -> the stored text form, for the given character set.
   Returns the length written, or -1 when it does not fit (the caller sizes
   the buffer at 3x + 1, which always does). */
int64_t mvx_id_encode(const char *id, int64_t idlen, int cs, char *out,
                      size_t cap) {
    const unsigned char *p = (const unsigned char *)id;
    size_t w = 0;
    for (int64_t i = 0; i < idlen;) {
        unsigned char c = p[i];
        int keep;
        int n = 1;
        if (c == '%' || c == 0)
            keep = 0;                   /* escaped in every character set */
        else if (c == ' ' && i == idlen - 1)
            keep = 0;                   /* a trailing space; see above */
        else if (c < 0x80)
            keep = 1;                   /* ASCII is ASCII everywhere */
        else if (cs == MVX_ID_CS_BYTE)
            keep = 1;
        else if (cs == MVX_ID_CS_UTF8 &&
                 utf8_valid(p + i, idlen - i, (n = utf8_len(c))))
            keep = 1;
        else
            keep = 0;

        if (keep) {
            if (w + (size_t)n >= cap) return -1;
            memcpy(out + w, p + i, (size_t)n);
            w += (size_t)n;
            i += n;
        } else {
            if (w + 3 >= cap) return -1;
            out[w++] = '%';
            out[w++] = hex[c >> 4];
            out[w++] = hex[c & 15];
            i++;
        }
    }
    out[w] = '\0';
    return (int64_t)w;
}

static int unhex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* mvx_id_decode: the stored text form -> bytes.  One decoder for every
   character set -- which is the reason '%' is always escaped.  A '%' that is
   not followed by two hex digits is taken literally rather than rejected: an
   id written by hand into the database should come back as what it says, not
   as an error. */
int64_t mvx_id_decode(const char *txt, int64_t txtlen, char *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)txt;
    size_t w = 0;
    for (int64_t i = 0; i < txtlen;) {
        if (p[i] == '%' && i + 2 < txtlen) {
            int hi = unhex(p[i + 1]), lo = unhex(p[i + 2]);
            if (hi >= 0 && lo >= 0) {
                if (w + 1 > cap) return -1;
                out[w++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        if (w + 1 > cap) return -1;
        out[w++] = (char)p[i++];
    }
    return (int64_t)w;
}
