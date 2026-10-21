/*
 * CGPDFSubset.c — the TrueType subsetter: closure, renumbering, table rebuild and checksums.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE PROGRAM IS READ THROUGH FREETYPE'S OWN SFNT TABLE DOOR (`FT_Load_Sfnt_Table`), not by walking the file
 * again: the face has already parsed the directory and checked the table bounds, and a second parser here would
 * be a second place for the two to disagree. What this file owns is the WRITE side — the part FreeType does not
 * do — and the two conversions that make a subset valid: glyph renumbering (with composite records rewritten)
 * and the `loca`/`hmtx`/`maxp`/`hhea` counts that must agree with it.
 *
 * THE CHECKSUM IS PART OF THE FORMAT, not a formality: `head.checkSumAdjustment` makes the whole font sum to
 * 0xB1B0AFBA, and every table directory entry carries its table's checksum. A subset with stale checksums is
 * accepted by some readers and rejected by others, which is worse than being rejected everywhere.
 */
#include <CoreGraphics/CGPDFSubset.h>
#include <CoreGraphics/CGFont_internal.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cg_pdf_subset {
	unsigned char *data;
	size_t len;
	unsigned *old_to_new;	/* indexed by OLD glyph id */
	size_t old_count;
	size_t glyph_count;
};

static uint16_t fn_u16(const unsigned char *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int16_t fn_i16(const unsigned char *p)
{
	return (int16_t)fn_u16(p);
}

static uint32_t fn_u32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void fn_put16(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 8);
	p[1] = (unsigned char)v;
}

static void fn_put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

/* A TABLE READ ONCE INTO MEMORY. NULL for a table the face does not have, which is how the optional ones are
 * asked for. */
static unsigned char *fn_table(FT_Face face, uint32_t tag, size_t *length)
{
	FT_ULong len = 0;
	unsigned char *data;

	if (FT_Load_Sfnt_Table(face, (FT_ULong)tag, 0, NULL, &len) != 0 || len == 0) {
		return NULL;
	}
	data = malloc(len);
	if (data == NULL) {
		return NULL;
	}
	if (FT_Load_Sfnt_Table(face, (FT_ULong)tag, 0, data, &len) != 0) {
		free(data);
		return NULL;
	}
	*length = (size_t)len;
	return data;
}

static uint32_t fn_checksum(const unsigned char *data, size_t len)
{
	uint32_t sum = 0;
	size_t i;

	for (i = 0; i < len; i += 4) {
		uint32_t word = 0;

		word |= (uint32_t)(i + 0 < len ? data[i + 0] : 0) << 24;
		word |= (uint32_t)(i + 1 < len ? data[i + 1] : 0) << 16;
		word |= (uint32_t)(i + 2 < len ? data[i + 2] : 0) << 8;
		word |= (uint32_t)(i + 3 < len ? data[i + 3] : 0);
		sum += word;
	}
	return sum;
}

static bool fn_loca(const unsigned char *loca, size_t length, int long_format, unsigned gid, uint32_t *offset)
{
	size_t need = long_format ? ((size_t)gid + 1) * 4 : ((size_t)gid + 1) * 2;

	if (need > length) {
		return false;
	}
	*offset = long_format ? fn_u32(loca + (size_t)gid * 4) : (uint32_t)fn_u16(loca + (size_t)gid * 2) * 2;
	return true;
}

/* REWRITE THE COMPONENT INDICES OF A COMPOSITE GLYPH IN PLACE. The record layout is fixed by the `glyf` table:
 * flags, component index, then arguments and an optional transform whose width the flags decide. */
static void fn_remap_composite(unsigned char *glyph, size_t length, const unsigned *old_to_new, size_t old_count)
{
	size_t at;

	if (length < 10 || fn_i16(glyph) >= 0) {
		return;
	}
	at = 10;
	while (at + 4 <= length) {
		uint16_t flags = fn_u16(glyph + at);
		unsigned component = fn_u16(glyph + at + 2);
		unsigned mapped = component < old_count ? old_to_new[component] : 0xFFFFFFFFu;

		if (mapped == 0xFFFFFFFFu) {
			mapped = 0;
		}
		fn_put16(glyph + at + 2, mapped);
		at += 4;
		at += (flags & 0x0001) ? 4 : 2;	/* ARG_1_AND_2_ARE_WORDS */
		if (flags & 0x0008) {
			at += 2;		/* WE_HAVE_A_SCALE */
		} else if (flags & 0x0040) {
			at += 4;		/* WE_HAVE_AN_X_AND_Y_SCALE */
		} else if (flags & 0x0080) {
			at += 8;		/* WE_HAVE_A_TWO_BY_TWO */
		}
		if (!(flags & 0x0020)) {	/* MORE_COMPONENTS */
			break;
		}
	}
}

struct fn_table_entry {
	uint32_t tag;
	unsigned char *data;
	size_t len;
	uint32_t checksum;
	size_t offset;
};

static int fn_table_compare(const void *a, const void *b)
{
	const struct fn_table_entry *ta = a;
	const struct fn_table_entry *tb = b;

	return ta->tag < tb->tag ? -1 : (ta->tag > tb->tag ? 1 : 0);
}

cg_pdf_subset *cg_pdf_subset_create(CGFontRef font, const unsigned *glyphs, size_t count)
{
	FT_Face face = cg_font_face(font);
	unsigned char *head = NULL;
	unsigned char *hhea = NULL;
	unsigned char *maxp = NULL;
	unsigned char *loca = NULL;
	unsigned char *glyf = NULL;
	unsigned char *hmtx = NULL;
	size_t head_len = 0, hhea_len = 0, maxp_len = 0, loca_len = 0, glyf_len = 0, hmtx_len = 0;
	size_t num_glyphs;
	int long_loca;
	unsigned num_hmetrics;
	unsigned char *keep = NULL;
	unsigned *queue = NULL;
	size_t queue_count = 0;
	size_t queue_cap = 0;
	unsigned *old_to_new = NULL;
	unsigned *new_to_old = NULL;
	size_t new_count;
	unsigned char *new_loca = NULL;
	unsigned char *new_glyf = NULL;
	size_t new_glyf_len = 0;
	size_t new_glyf_cap = 0;
	unsigned char *new_hmtx = NULL;
	struct fn_table_entry tables[16];
	size_t table_count = 0;
	cg_pdf_subset *subset = NULL;
	size_t i;
	size_t total;

	if (face == NULL || !FT_IS_SFNT(face)) {
		return NULL;
	}
	head = fn_table(face, FT_MAKE_TAG('h', 'e', 'a', 'd'), &head_len);
	hhea = fn_table(face, FT_MAKE_TAG('h', 'h', 'e', 'a'), &hhea_len);
	maxp = fn_table(face, FT_MAKE_TAG('m', 'a', 'x', 'p'), &maxp_len);
	loca = fn_table(face, FT_MAKE_TAG('l', 'o', 'c', 'a'), &loca_len);
	glyf = fn_table(face, FT_MAKE_TAG('g', 'l', 'y', 'f'), &glyf_len);
	hmtx = fn_table(face, FT_MAKE_TAG('h', 'm', 't', 'x'), &hmtx_len);
	if (head == NULL || hhea == NULL || maxp == NULL || loca == NULL || glyf == NULL || hmtx == NULL ||
	    head_len < 54 || hhea_len < 36 || maxp_len < 6) {
		goto done;
	}
	num_glyphs = fn_u16(maxp + 4);
	long_loca = fn_i16(head + 50) == 1;
	num_hmetrics = fn_u16(hhea + 34);
	if (num_glyphs == 0) {
		goto done;
	}

	/* THE CLOSURE: .notdef, everything the document selected, and every component a composite names. */
	keep = calloc(num_glyphs, 1);
	old_to_new = malloc(num_glyphs * sizeof *old_to_new);
	if (keep == NULL || old_to_new == NULL) {
		goto done;
	}
	for (i = 0; i < num_glyphs; i++) {
		old_to_new[i] = 0xFFFFFFFFu;
	}
	keep[0] = 1;
	queue_cap = count + 16;
	queue = malloc(queue_cap * sizeof *queue);
	if (queue == NULL) {
		goto done;
	}
	queue[queue_count++] = 0;
	for (i = 0; i < count; i++) {
		unsigned gid = glyphs[i];

		if (gid >= num_glyphs) {
			continue;
		}
		if (!keep[gid]) {
			keep[gid] = 1;
			if (queue_count == queue_cap) {
				size_t cap = queue_cap * 2;
				unsigned *grown = realloc(queue, cap * sizeof *grown);

				if (grown == NULL) {
					goto done;
				}
				queue = grown;
				queue_cap = cap;
			}
			queue[queue_count++] = gid;
		}
	}
	{
		size_t at_queue;

		for (at_queue = 0; at_queue < queue_count; at_queue++) {
			unsigned gid = queue[at_queue];
			uint32_t start;
			uint32_t end;
			unsigned char *g;
			size_t length;
			size_t at;

			if (!fn_loca(loca, loca_len, long_loca, gid, &start) ||
			    !fn_loca(loca, loca_len, long_loca, gid + 1, &end) || end < start ||
			    (size_t)end > glyf_len) {
				continue;
			}
			g = glyf + start;
			length = (size_t)(end - start);
			if (length < 10 || fn_i16(g) >= 0) {
				continue;
			}
			at = 10;
			while (at + 4 <= length) {
				uint16_t flags = fn_u16(g + at);
				unsigned component = fn_u16(g + at + 2);

				if (component < num_glyphs && !keep[component]) {
					keep[component] = 1;
					if (queue_count == queue_cap) {
						size_t cap = queue_cap * 2;
						unsigned *grown = realloc(queue, cap * sizeof *grown);

						if (grown == NULL) {
							goto done;
						}
						queue = grown;
						queue_cap = cap;
					}
					queue[queue_count++] = component;
				}
				at += 4;
				at += (flags & 0x0001) ? 4 : 2;
				if (flags & 0x0008) {
					at += 2;
				} else if (flags & 0x0040) {
					at += 4;
				} else if (flags & 0x0080) {
					at += 8;
				}
				if (!(flags & 0x0020)) {
					break;
				}
			}
		}
	}

	/* THE NUMBERING: 0 stays 0; kept glyphs take 1..n in ascending old order. */
	old_to_new[0] = 0;
	new_count = 1;
	for (i = 1; i < num_glyphs; i++) {
		if (keep[i]) {
			old_to_new[i] = (unsigned)new_count++;
		}
	}
	if (new_count > 0xFFFF) {
		goto done;
	}
	new_to_old = malloc(new_count * sizeof *new_to_old);
	if (new_to_old == NULL) {
		goto done;
	}
	new_to_old[0] = 0;
	for (i = 1; i < num_glyphs; i++) {
		if (old_to_new[i] != 0xFFFFFFFFu) {
			new_to_old[old_to_new[i]] = (unsigned)i;
		}
	}

	/* THE GLYF TABLE, with every composite's component indices rewritten. */
	for (i = 0; i < new_count; i++) {
		unsigned old = new_to_old[i];
		uint32_t start;
		uint32_t end;
		size_t length;

		if (!fn_loca(loca, loca_len, long_loca, old, &start) ||
		    !fn_loca(loca, loca_len, long_loca, old + 1, &end) || end < start ||
		    (size_t)end > glyf_len) {
			start = end = 0;
		}
		length = (size_t)(end - start);
		if (new_glyf_len + length + 4 > new_glyf_cap) {
			size_t cap = new_glyf_cap ? new_glyf_cap : 4096;
			unsigned char *grown;

			while (cap < new_glyf_len + length + 4) {
				cap *= 2;
			}
			grown = realloc(new_glyf, cap);
			if (grown == NULL) {
				goto done;
			}
			new_glyf = grown;
			new_glyf_cap = cap;
		}
		/* THE OFFSET IS RECORDED BEFORE THE GLYPH IS COPIED, so the next iteration's `new_loca` write is
		 * this glyph's start. `new_loca` has room for new_count + 1 entries. */
		if (i == 0) {
			new_loca = calloc(new_count + 1, 4);
			if (new_loca == NULL) {
				goto done;
			}
		}
		fn_put32(new_loca + i * 4, (uint32_t)new_glyf_len);
		if (length > 0) {
			memcpy(new_glyf + new_glyf_len, glyf + start, length);
			fn_remap_composite(new_glyf + new_glyf_len, length, old_to_new, num_glyphs);
			new_glyf_len += length;
		}
	}
	fn_put32(new_loca + new_count * 4, (uint32_t)new_glyf_len);

	/* THE HMTX TABLE: one long metric per kept glyph, with advances taken from the old long metrics or the
	 * old trailing LSB array. */
	new_hmtx = calloc(new_count, 4);
	if (new_hmtx == NULL) {
		goto done;
	}
	for (i = 0; i < new_count; i++) {
		unsigned old = new_to_old[i];
		unsigned advance = 1000;
		int lsb = 0;

		if (old < num_hmetrics) {
			if ((size_t)(old + 1) * 4 <= hmtx_len) {
				advance = fn_u16(hmtx + old * 4);
				lsb = fn_i16(hmtx + old * 4 + 2);
			}
		} else {
			size_t lsb_at;

			if (num_hmetrics > 0 && (size_t)num_hmetrics * 4 <= hmtx_len) {
				advance = fn_u16(hmtx + (num_hmetrics - 1) * 4);
			}
			lsb_at = (size_t)num_hmetrics * 4 + (size_t)(old - num_hmetrics) * 2;
			if (lsb_at + 2 <= hmtx_len) {
				lsb = fn_i16(hmtx + lsb_at);
			}
		}
		fn_put16(new_hmtx + i * 4, advance);
		fn_put16(new_hmtx + i * 4 + 2, (unsigned)(uint16_t)lsb);
	}

	/* THE REQUIRED TABLES. head is copied with its checksum adjustment zeroed and long `loca` declared;
	 * maxp and hhea get the new counts. */
	fn_put32(head + 8, 0);
	fn_put16(head + 50, 1);
	fn_put16(maxp + 4, (unsigned)new_count);
	fn_put16(hhea + 34, (unsigned)new_count);

#define ADD_TABLE(tag_value, data_ptr, data_len) \
	do { \
		tables[table_count].tag = (tag_value); \
		tables[table_count].data = (data_ptr); \
		tables[table_count].len = (data_len); \
		table_count++; \
	} while (0)
	ADD_TABLE(FT_MAKE_TAG('h', 'e', 'a', 'd'), head, head_len);
	ADD_TABLE(FT_MAKE_TAG('h', 'h', 'e', 'a'), hhea, hhea_len);
	ADD_TABLE(FT_MAKE_TAG('m', 'a', 'x', 'p'), maxp, maxp_len);
	ADD_TABLE(FT_MAKE_TAG('l', 'o', 'c', 'a'), new_loca, (new_count + 1) * 4);
	ADD_TABLE(FT_MAKE_TAG('g', 'l', 'y', 'f'), new_glyf, new_glyf_len);
	ADD_TABLE(FT_MAKE_TAG('h', 'm', 't', 'x'), new_hmtx, new_count * 4);
	{
		static const uint32_t optional[] = {
			FT_MAKE_TAG('c', 'v', 't', ' '),
			FT_MAKE_TAG('f', 'p', 'g', 'm'),
			FT_MAKE_TAG('p', 'r', 'e', 'p'),
			FT_MAKE_TAG('g', 'a', 's', 'p'),
			FT_MAKE_TAG('n', 'a', 'm', 'e'),
			FT_MAKE_TAG('O', 'S', '/', '2')
		};
		size_t k;

		for (k = 0; k < sizeof optional / sizeof optional[0]; k++) {
			size_t length = 0;
			unsigned char *data = fn_table(face, optional[k], &length);

			if (data != NULL) {
				ADD_TABLE(optional[k], data, length);
			}
		}
	}
#undef ADD_TABLE
	qsort(tables, table_count, sizeof tables[0], fn_table_compare);

	/* THE SFNT: offset table, directory, then aligned tables. */
	{
		unsigned n = (unsigned)table_count;
		unsigned search = 1;
		unsigned entry = 0;

		while ((search << 1) <= n) {
			search <<= 1;
			entry++;
		}
		total = 12 + (size_t)n * 16;
		for (i = 0; i < table_count; i++) {
			tables[i].offset = (total + 3) & ~(size_t)3;
			tables[i].checksum = fn_checksum(tables[i].data, tables[i].len);
			total = tables[i].offset + ((tables[i].len + 3) & ~(size_t)3);
		}
		subset = calloc(1, sizeof *subset);
		if (subset == NULL) {
			goto done;
		}
		subset->data = calloc(total, 1);
		if (subset->data == NULL) {
			free(subset);
			subset = NULL;
			goto done;
		}
		subset->len = total;
		fn_put32(subset->data + 0, 0x00010000);
		fn_put16(subset->data + 4, n);
		fn_put16(subset->data + 6, search * 16);
		fn_put16(subset->data + 8, entry);
		fn_put16(subset->data + 10, n * 16 - search * 16);
		for (i = 0; i < table_count; i++) {
			unsigned char *dir = subset->data + 12 + i * 16;

			fn_put32(dir + 0, tables[i].tag);
			fn_put32(dir + 4, tables[i].checksum);
			fn_put32(dir + 8, (uint32_t)tables[i].offset);
			fn_put32(dir + 12, (uint32_t)tables[i].len);
			memcpy(subset->data + tables[i].offset, tables[i].data, tables[i].len);
		}
	}
	/* THE CHECK-SUM ADJUSTMENT, computed last with the field zero. */
	{
		uint32_t sum = fn_checksum(subset->data, subset->len);
		uint32_t adjustment = 0xB1B0AFBAu - sum;
		size_t head_offset = 0;

		for (i = 0; i < table_count; i++) {
			if (tables[i].tag == FT_MAKE_TAG('h', 'e', 'a', 'd')) {
				head_offset = tables[i].offset;
				break;
			}
		}
		fn_put32(subset->data + head_offset + 8, adjustment);
	}
	subset->old_to_new = old_to_new;
	old_to_new = NULL;
	subset->old_count = num_glyphs;
	subset->glyph_count = new_count;

done:
	/* EVERY TABLE ENTRY OWNS ITS BYTES (the required ones came from `fn_table`, the built ones from malloc, the
	 * optional ones from `fn_table`), so ONE walk frees them. Before the core tables are listed, the inputs are
	 * still loose and are freed by name. The assembled font is a copy, so a success frees everything here. */
	if (table_count > 0) {
		for (i = 0; i < table_count; i++) {
			free(tables[i].data);
		}
	} else {
		free(head);
		free(hhea);
		free(maxp);
		free(loca);
		free(glyf);
		free(hmtx);
		free(new_loca);
		free(new_glyf);
		free(new_hmtx);
	}
	if (subset == NULL) {
		free(old_to_new);
	} else if (subset->data == NULL) {
		free(subset);
		subset = NULL;
	} else {
		/* `new_loca`, `new_glyf` and `new_hmtx` are in `tables` and were freed above; only the inputs and the
		 * numbering that did not travel with the subset remain. */
	}
	free(new_to_old);
	free(keep);
	free(queue);
	return subset;
}

void cg_pdf_subset_free(cg_pdf_subset *subset)
{
	if (subset == NULL) {
		return;
	}
	free(subset->data);
	free(subset->old_to_new);
	free(subset);
}

unsigned cg_pdf_subset_new_gid(cg_pdf_subset *subset, unsigned old_gid)
{
	if (subset == NULL || old_gid >= subset->old_count) {
		return 0xFFFFFFFFu;
	}
	return subset->old_to_new[old_gid];
}

size_t cg_pdf_subset_glyph_count(cg_pdf_subset *subset)
{
	return subset != NULL ? subset->glyph_count : 0;
}

const unsigned char *cg_pdf_subset_data(cg_pdf_subset *subset, size_t *length)
{
	if (subset == NULL) {
		return NULL;
	}
	if (length != NULL) {
		*length = subset->len;
	}
	return subset->data;
}
