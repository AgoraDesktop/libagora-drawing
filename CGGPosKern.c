/*
 * CGGPosKern.c — the pair adjustments a modern font keeps in GPOS.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **WHY THIS EXISTS: THE `kern` TABLE IS OBSOLETE AND THE FONTS PEOPLE USE ARE NOT.** A font's opinion that an `o`
 * belongs closer under a `T` than their advances put it used to live in a `kern` table, and FreeType reads that one
 * (`FT_Get_Kerning`, which `cg_font_kern` asks first). Modern faces keep the same opinion in **GPOS** — the OpenType
 * layout table — and MEASURED, the typeface this desktop actually draws with has NO `kern` table AT ALL: Inter's
 * pairs are all in GPOS, so every one of them was invisible to the engine's kerning door and the text was set
 * unkerned. Pango, which has HarfBuzz behind it, applies them: `To` closes by 1.02 pixels at 13px, `AV` by 0.89.
 *
 * **THIS IS NOT A SHAPER AND DOES NOT PRETEND TO BE ONE.** It reads ONE feature — `kern` — out of ONE script's
 * feature list, and only the two PairPos subtable formats that carry pair adjustments. Ligatures, marks, contextual
 * positioning, script-specific shaping and the `GSUB` table are all absent, and a caller that needs them needs
 * HarfBuzz rather than this file. WHAT IT IS FOR is the case that matters to a desktop's own text: a UI face whose
 * pairs are in GPOS and whose letters therefore do not sit where its designer put them.
 *
 * THE ANSWER IS IN THE FONT'S UNITS, like every other metric door here — `CGPosValue` is a 16.16-free integer in
 * design units, and the caller scales it by the size over the em.
 *
 * EVERY READ IS BOUNDS-CHECKED against the table's own length, because the bytes come from a FILE a caller chose.
 * A malformed table answers "no adjustment" rather than reading past the end: a font is not code, and a desktop that
 * crashed on a corrupt one would be a worse failure than one that set the text too loosely.
 */
#include <CoreGraphics/CGFont_internal.h>

#include <stddef.h>
#include <stdint.h>

/* THE FOUR-BYTE TAGS, as the tables spell them. */
#define FN_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define FN_TAG_KERN FN_TAG('k', 'e', 'r', 'n')
#define FN_TAG_GPOS FN_TAG('G', 'P', 'O', 'S')

/* THE LOOKUP TYPES THIS FILE UNDERSTANDS: a pair adjustment, and the extension wrapper that exists so a lookup can
 * be more than 64K from the start of the table. Everything else is skipped by its type, which is what makes a font
 * with mark or contextual lookups in the same feature list harmless. */
#define FN_LOOKUP_PAIR 2
#define FN_LOOKUP_EXTENSION 9

/* THE EIGHT FIELDS A ValueRecord CAN CARRY, IN THE ORDER THEY APPEAR, and the bit each one is set by. EVERY ONE IS
 * TWO BYTES IN A NON-DEVICE TABLE, which is why the size arithmetic is a popcount and not a table of widths. */
#define FN_VALUE_X_ADVANCE 0x0004u

static int fn_u16(const unsigned char *g, size_t length, size_t off, unsigned *out)
{
	if (off + 2u > length) {
		return 0;
	}
	*out = ((unsigned)g[off] << 8) | (unsigned)g[off + 1];
	return 1;
}

static int fn_i16(const unsigned char *g, size_t length, size_t off, int *out)
{
	unsigned v;

	if (!fn_u16(g, length, off, &v)) {
		return 0;
	}
	*out = (v & 0x8000u) ? (int)v - 0x10000 : (int)v;
	return 1;
}

static int fn_u32(const unsigned char *g, size_t length, size_t off, uint32_t *out)
{
	if (off + 4u > length) {
		return 0;
	}
	*out = ((uint32_t)g[off] << 24) | ((uint32_t)g[off + 1] << 16) | ((uint32_t)g[off + 2] << 8)
	       | (uint32_t)g[off + 3];
	return 1;
}

/* HOW MANY BYTES A ValueRecord OF THIS FORMAT OCCUPIES — two for each field the format sets. */
static size_t fn_value_size(unsigned format)
{
	size_t size = 0;
	int bit;

	for (bit = 0; bit < 8; bit++) {
		if (format & (1u << bit)) {
			size += 2u;
		}
	}
	return size;
}

/* **THE X ADVANCE OF A ValueRecord, OR ZERO.** A record's fields are packed in bit order and the ones this table
 * does not set are ABSENT rather than zero, so the offset of XAdvance is however many set fields precede it. A
 * format without that bit carries no advance at all, which is the common case for the SECOND glyph of a pair (its
 * adjustment is the first glyph's). */
static int fn_value_x_advance(const unsigned char *g, size_t length, size_t off, unsigned format, int *out)
{
	size_t at = off;
	int bit;

	*out = 0;
	for (bit = 0; bit < 8; bit++) {
		if (!(format & (1u << bit))) {
			continue;
		}
		if ((1u << bit) == FN_VALUE_X_ADVANCE) {
			return fn_i16(g, length, at, out);
		}
		at += 2u;
	}
	return 1;
}

/* IS `glyph` IN THIS COVERAGE TABLE, AND AT WHAT INDEX? Both formats, because a font may use either: format 1 is a
 * sorted list of glyphs and format 2 is a sorted list of ranges. */
static int fn_coverage_index(const unsigned char *g, size_t length, size_t off, unsigned glyph, int *index)
{
	unsigned format, count, i;

	if (!fn_u16(g, length, off, &format) || !fn_u16(g, length, off + 2u, &count)) {
		return 0;
	}
	if (format == 1u) {
		for (i = 0; i < count; i++) {
			unsigned value;

			if (!fn_u16(g, length, off + 4u + 2u * (size_t)i, &value)) {
				return 0;
			}
			if (value == glyph) {
				*index = (int)i;
				return 1;
			}
		}
		return 0;
	}
	if (format == 2u) {
		for (i = 0; i < count; i++) {
			size_t at = off + 4u + 6u * (size_t)i;
			unsigned start, end, base;

			if (!fn_u16(g, length, at, &start) || !fn_u16(g, length, at + 2u, &end)
			    || !fn_u16(g, length, at + 4u, &base)) {
				return 0;
			}
			if (glyph >= start && glyph <= end) {
				*index = (int)(base + (glyph - start));
				return 1;
			}
		}
		return 0;
	}
	return 0;
}

/* THE CLASS OF A GLYPH, WHICH IS 0 WHEN THE TABLE DOES NOT SAY — class 0 is the "everything else" class by
 * definition, so an absent glyph is not an error. */
static int fn_class_of(const unsigned char *g, size_t length, size_t off, unsigned glyph, unsigned *out)
{
	unsigned format, count, i;

	*out = 0;
	if (!fn_u16(g, length, off, &format) || !fn_u16(g, length, off + 2u, &count)) {
		return 0;
	}
	if (format == 1u) {
		unsigned start, value;

		if (!fn_u16(g, length, off + 2u, &start) || !fn_u16(g, length, off + 4u, &count)) {
			return 0;
		}
		if (glyph < start || glyph >= start + count) {
			return 1;
		}
		if (!fn_u16(g, length, off + 6u + 2u * (size_t)(glyph - start), &value)) {
			return 0;
		}
		*out = value;
		return 1;
	}
	if (format == 2u) {
		for (i = 0; i < count; i++) {
			size_t at = off + 4u + 6u * (size_t)i;
			unsigned start, end, value;

			if (!fn_u16(g, length, at, &start) || !fn_u16(g, length, at + 2u, &end)
			    || !fn_u16(g, length, at + 4u, &value)) {
				return 0;
			}
			if (glyph >= start && glyph <= end) {
				*out = value;
				return 1;
			}
		}
		return 1;
	}
	return 0;
}

/* **THE PAIR ADJUSTMENT, OR `*applied` STAYS ZERO.** The two formats answer the same question two ways: format 1 is
 * a list of glyph pairs (binary-searched, because the format REQUIRES the records to be sorted by second glyph), and
 * format 2 is a matrix of CLASS pairs, which is how a font says "every capital before every lowercase 'o'-like
 * letter" without listing them. */
static int fn_pair_pos(const unsigned char *g, size_t length, size_t base, unsigned left, unsigned right, int *out,
		       int *applied)
{
	unsigned format;

	if (!fn_u16(g, length, base, &format)) {
		return 0;
	}
	*out = 0;
	*applied = 0;
	if (format == 1u) {
		unsigned coverage, vf1, vf2, pair_sets, index;
		size_t set;

		if (!fn_u16(g, length, base + 2u, &coverage) || !fn_u16(g, length, base + 4u, &vf1)
		    || !fn_u16(g, length, base + 6u, &vf2) || !fn_u16(g, length, base + 8u, &pair_sets)) {
			return 0;
		}
		{
			int at = 0;

			if (!fn_coverage_index(g, length, base + coverage, left, &at)) {
				return 1;	/* the glyph is not this subtable's business at all */
			}
			index = (unsigned)at;
		}
		if (index >= pair_sets || !fn_u16(g, length, base + 10u + 2u * (size_t)index, &coverage)) {
			return 0;
		}
		set = base + coverage;
		{
			unsigned count, lo, hi;
			size_t stride = 2u + fn_value_size(vf1) + fn_value_size(vf2);

			if (!fn_u16(g, length, set, &count)) {
				return 0;
			}
			lo = 0;
			hi = count;
			while (lo < hi) {
				unsigned mid = lo + (hi - lo) / 2u;
				unsigned second;

				if (!fn_u16(g, length, set + 2u + stride * (size_t)mid, &second)) {
					return 0;
				}
				if (second == right) {
					*applied = 1;
					return fn_value_x_advance(g, length, set + 2u + stride * (size_t)mid + 2u, vf1,
								  out);
				}
				if (second < right) {
					lo = mid + 1u;
				} else {
					hi = mid;
				}
			}
		}
		return 1;
	}
	if (format == 2u) {
		unsigned coverage, vf1, vf2, cd1, cd2, class1_count, class2_count, k1, k2;
		size_t record;

		if (!fn_u16(g, length, base + 2u, &coverage) || !fn_u16(g, length, base + 4u, &vf1)
		    || !fn_u16(g, length, base + 6u, &vf2) || !fn_u16(g, length, base + 8u, &cd1)
		    || !fn_u16(g, length, base + 10u, &cd2) || !fn_u16(g, length, base + 12u, &class1_count)
		    || !fn_u16(g, length, base + 14u, &class2_count)) {
			return 0;
		}
		{
			int at = 0;

			if (!fn_coverage_index(g, length, base + coverage, left, &at)) {
				return 1;	/* not this subtable's first glyph */
			}
		}
		if (!fn_class_of(g, length, base + cd1, left, &k1)
		    || !fn_class_of(g, length, base + cd2, right, &k2)) {
			return 0;
		}
		if (k1 >= class1_count || k2 >= class2_count) {
			return 0;	/* a malformed matrix rather than a pair this font has an opinion about */
		}
		record = base + 16u + ((size_t)k1 * (size_t)class2_count + (size_t)k2)
				      * (fn_value_size(vf1) + fn_value_size(vf2));
		*applied = 1;
		return fn_value_x_advance(g, length, record, vf1, out);
	}
	return 0;
}

/* ONE LOOKUP, BY ITS INDEX IN THE LOOKUP LIST. `*applied` says whether any of its subtables was this glyph pair's
 * business — which is the difference between "the font kerned this pair by zero" and "the font has no opinion", and
 * it is what lets the caller stop at the first lookup that matches instead of summing every lookup's answer. */
static int fn_apply_lookup(const unsigned char *g, size_t length, size_t lookup_list, unsigned index, unsigned left,
			   unsigned right, int *out, int *applied)
{
	unsigned count, type, sub_count, i;
	size_t lookup;

	*applied = 0;
	if (!fn_u16(g, length, lookup_list, &count) || index >= count) {
		return 0;
	}
	if (!fn_u16(g, length, lookup_list + 2u + 2u * (size_t)index, &type)) {
		return 0;
	}
	lookup = lookup_list + (size_t)type;
	if (!fn_u16(g, length, lookup, &type) || !fn_u16(g, length, lookup + 4u, &sub_count)) {
		return 0;
	}
	for (i = 0; i < sub_count; i++) {
		unsigned offset;

		if (!fn_u16(g, length, lookup + 6u + 2u * (size_t)i, &offset)) {
			return 0;
		}
		if (type == FN_LOOKUP_PAIR) {
			if (!fn_pair_pos(g, length, lookup + (size_t)offset, left, right, out, applied)) {
				return 0;
			}
		} else if (type == FN_LOOKUP_EXTENSION) {
			unsigned ext_type;
			uint32_t ext_offset;

			if (!fn_u16(g, length, lookup + (size_t)offset + 2u, &ext_type)
			    || !fn_u32(g, length, lookup + (size_t)offset + 4u, &ext_offset)) {
				return 0;
			}
			if (ext_type == FN_LOOKUP_PAIR
			    && !fn_pair_pos(g, length, lookup + (size_t)offset + (size_t)ext_offset, left, right, out,
					    applied)) {
				return 0;
			}
		}
		if (*applied) {
			return 1;
		}
	}
	return 1;
}

/* **THE DOOR: THE `kern` FEATURE'S ADJUSTMENT FOR A PAIR, IN FONT UNITS, OR ZERO.** The table is searched by its
 * FEATURE TAG rather than through a script and a language system, which is a simplification worth stating: a font
 * may carry several `kern` features for different scripts, and this asks all of them in the order the feature list
 * holds them — which is the order a shaper would try them in for a Latin run, since the default script's features
 * come first in practice. A font that needed the difference would need a shaper. */
int cg_gpos_kern(const unsigned char *g, size_t length, unsigned left, unsigned right)
{
	unsigned version, feature_list, lookup_list, feature_count, i;

	if (g == NULL || length < 10u) {
		return 0;
	}
	if (!fn_u32(g, length, 0u, &version) || (version >> 16) != 1u) {
		return 0;	/* version 1.1 differs only by an extra field this file does not read */
	}
	if (!fn_u16(g, length, 6u, &feature_list) || !fn_u16(g, length, 8u, &lookup_list)
	    || !fn_u16(g, length, feature_list, &feature_count)) {
		return 0;
	}
	for (i = 0; i < feature_count; i++) {
		size_t record = feature_list + 2u + 6u * (size_t)i;
		uint32_t tag;
		unsigned offset, lookup_count, j;

		if (!fn_u32(g, length, record, &tag) || !fn_u16(g, length, record + 4u, &offset)) {
			return 0;
		}
		if (tag != FN_TAG_KERN) {
			continue;
		}
		if (!fn_u16(g, length, feature_list + (size_t)offset + 2u, &lookup_count)) {
			return 0;
		}
		for (j = 0; j < lookup_count; j++) {
			unsigned index;
			int value = 0, applied = 0;

			if (!fn_u16(g, length, feature_list + (size_t)offset + 4u + 2u * (size_t)j, &index)) {
				return 0;
			}
			if (!fn_apply_lookup(g, length, lookup_list, index, left, right, &value, &applied)) {
				return 0;
			}
			if (applied) {
				return value;
			}
		}
	}
	return 0;
}
