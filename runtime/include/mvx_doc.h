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

/* Records as documents (#157).
 *
 * A record is stored as a JSON document rather than an opaque blob, so that an
 * UNMAPPED attribute is an ordinary field the backend can filter and index:
 *
 *     postgres   doc->>'3'                     GIN-indexable, no CREATE FUNCTION
 *     sqlite     json_extract(doc,'$."3"')     built in
 *     mysql      doc->>'$."3"'                 generated-column index pattern
 *     mongo      { "3": ... }                  a field, so filterable at all
 *
 * That is the point of this: today each driver carries its own contraption for
 * splitting the blob (postgres installs an IMMUTABLE mvx_attr() into the schema,
 * mysql cannot index raw attributes at all, mongo cannot split a blob at all).
 *
 * THE SHAPE — the three MV levels map onto JSON nesting directly.  Field names
 * are attribute ordinals, which is all MV has without a dictionary:
 *
 *     1: Ada                      { "1": "Ada",
 *     2: 5 v 6 v 7                  "2": ["5", "6", "7"],
 *     3: 10 s 11 v 20               "3": [["10", "11"], ["20"]] }
 *
 * A single value stays a SCALAR, not a one-element array: in MV a single value
 * and a one-element multivalue are the same bytes, so there is no distinction
 * to preserve and scalar-when-one gives one canonical form instead of two
 * spellings of it.
 *
 * ALIGNMENT is the invariant.  A value at position 3 must still be at position
 * 3, so interior and leading empties are kept — dropping one does not lose
 * "nothing", it shifts everything after it.  Trailing empties hold nothing
 * apart and may be trimmed.
 *
 * LEAVES ARE TEXT, with base64 for the rest.  A value whose bytes are not valid
 * UTF-8 is wrapped, per value rather than per record, so one binary field does
 * not take a whole record out of the backend's reach:
 *
 *     { "1": "Ada", "2": { "$b64": "3q2+7w==" } }
 *
 * The wrapper is self-describing because a leaf here is always a string, never
 * an object — so an object at a leaf can only mean "encoded".  A sentinel
 * prefix would instead need every literal value checked and escaped on the way
 * in, putting the cost on the common case to save the rare one.
 */
#ifndef MVX_DOC_H
#define MVX_DOC_H

#include "mvx_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* rec -> JSON document text (unmapped form: ordinal keys). */
void mvx_doc_encode(mv_value *dst, const mv_value *rec);

/* JSON document text -> rec.  Anything the encoder produced round-trips
   byte-exactly, which is the whole acceptance criterion; a document that is not
   well-formed yields an empty record rather than a partial one. */
void mvx_doc_decode(mv_value *dst, const mv_value *doc);

/* The MAPPED form, driven by a %MAP% spec: the dictionary supplies names, and
 * an association becomes an array of objects rather than parallel arrays —
 * the same information the relational child table holds, in the shape a
 * document store wants.
 *
 *     { "CUST": "C1",
 *       "LINE_ITEMS": [ { "QTY": "5", "PRICE": "10" },
 *                       { "QTY": "6", "PRICE": "20" } ] }
 *
 * ATTRIBUTES THE MAPPING DOES NOT COVER KEEP THEIR ORDINAL KEYS, so the
 * document carries the whole record.  That is what lets native mode drop the
 * blob: the blob's remaining job today is exactly to hold the un-mapped
 * attributes (mvx_store.c, map_recompose), and a document that carries them
 * has no such job left.
 *
 * A ragged association has as many rows as its LONGEST member, with shorter
 * members contributing "" for the positions they do not reach.  That is
 * map_child_apply's rule (max map_vcount across the members), reused rather
 * than restated: taking the minimum, or zipping until the first field runs
 * out, silently drops a line item and leaves a well-formed record behind.
 *
 * Round-trip exactness holds for identity fields.  A converted field is only
 * as reversible as its conversion — MD/MR/ML have dropped the raw digits by
 * the time they are stored — which is why #158 makes native mode take one
 * mapping per attribute and name the one to keep.
 */
void mvx_doc_encode_mapped(mvx_ctx *ctx, mv_value *dst, const mv_value *rec,
                           const mv_value *spec);
void mvx_doc_decode_mapped(mvx_ctx *ctx, mv_value *dst, const mv_value *doc,
                           const mv_value *spec);

#ifdef __cplusplus
}
#endif
#endif
