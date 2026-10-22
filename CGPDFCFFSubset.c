/*
 * CGPDFCFFSubset.c — the non-CID CFF subsetter.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * THE FORMAT IS A SET OF INDEXES AND OFFSETS, and the only arithmetic that must be exactly right is where
 * each rebuilt piece lands: Top DICT offsets are absolute from the start of the CFF data, while the Private
 * DICT's `Subrs` offset is relative to the Private DICT. That is why the Private DICT and its Local Subrs are
 * copied as ONE REGION: the relative offset inside the copied bytes stays valid with no rewriting.
 *
 * THE TOP DICT IS WALKED TWICE. The first walk measures the dictionary with placeholder offsets (every offset
 * is written as a fixed five-byte integer, so the length does not depend on the value); the layout is then
 * known, and the second walk writes the real offsets into the same number of bytes.
 */
#include <CoreGraphics/CGPDFCFFSubset.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	unsigned char *data;
	size_t len;
	size_t cap;
} cg_cbuf;

static void fn_put(cg_cbuf *b, const void *p, size_t n)
{
	if (b->len + n > b->cap) {
		size_t cap = b->cap ? b->cap : 1024;
		unsigned char *grown;

		while (cap < b->len + n) {
			cap *= 2;
		}
		grown = realloc(b->data, cap);
		if (grown == NULL) {
			return;
		}
		b->data = grown;
		b->cap = cap;
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

static void fn_byte(cg_cbuf *b, unsigned v)
{
	unsigned char c = (unsigned char)v;

	fn_put(b, &c, 1);
}

static void fn_u16(cg_cbuf *b, unsigned v)
{
	fn_byte(b, v >> 8);
	fn_byte(b, v);
}

static void fn_u32(cg_cbuf *b, uint32_t v)
{
	fn_byte(b, v >> 24);
	fn_byte(b, v >> 16);
	fn_byte(b, v >> 8);
	fn_byte(b, v);
}

static void fn_free(cg_cbuf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

static uint16_t fn_ru16(const unsigned char *p)
{
	return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t fn_ru32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

struct cg_pdf_cff_subset {
	unsigned char *data;
	size_t len;
};

/* ------------------------------------------------------------------------- */
/* INDEX                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
	size_t count;
	unsigned off_size;
	const unsigned char *offsets;
	const unsigned char *data;
	size_t data_len;
	size_t end;
} cff_index;

static unsigned fn_index_offset(const unsigned char *p, unsigned size)
{
	unsigned v = 0;
	unsigned i;

	for (i = 0; i < size; i++) {
		v = (v << 8) | p[i];
	}
	return v;
}

static bool fn_index_parse(const unsigned char *d, size_t n, size_t at, cff_index *index)
{
	size_t off_bytes;
	unsigned first;
	unsigned last;

	if (at + 3 > n) {
		return false;
	}
	memset(index, 0, sizeof *index);
	index->count = fn_ru16(d + at);
	at += 2;
	if (index->count == 0) {
		index->end = at;
		return true;
	}
	index->off_size = d[at++];
	if (index->off_size < 1 || index->off_size > 4) {
		return false;
	}
	off_bytes = (index->count + 1) * index->off_size;
	if (at + off_bytes > n) {
		return false;
	}
	index->offsets = d + at;
	at += off_bytes;
	first = fn_index_offset(index->offsets, index->off_size);
	last = fn_index_offset(index->offsets + index->count * index->off_size, index->off_size);
	if (first < 1 || last < first) {
		return false;
	}
	index->data = d + at;
	index->data_len = (size_t)(last - 1);
	if (index->data_len > n - at) {
		return false;
	}
	index->end = at + index->data_len;
	return true;
}

static bool fn_index_entry(const cff_index *index, size_t i, const unsigned char **p, size_t *len)
{
	unsigned a;
	unsigned b;

	if (i >= index->count) {
		return false;
	}
	a = fn_index_offset(index->offsets + i * index->off_size, index->off_size);
	b = fn_index_offset(index->offsets + (i + 1) * index->off_size, index->off_size);
	if (a < 1 || b < a || (size_t)(b - 1) > index->data_len) {
		return false;
	}
	*p = index->data + (a - 1);
	*len = (size_t)(b - a);
	return true;
}

static void fn_index_write(cg_cbuf *out, const unsigned char **parts, const size_t *lengths, size_t count)
{
	size_t i;

	if (count == 0) {
		fn_u16(out, 0);
		return;
	}
	fn_u16(out, (unsigned)count);
	fn_byte(out, 4);	/* an offset size of four bytes covers every real program */
	fn_u32(out, 1);
	{
		size_t offset = 1;

		for (i = 0; i < count; i++) {
			offset += lengths[i];
			fn_u32(out, (uint32_t)offset);
		}
	}
	for (i = 0; i < count; i++) {
		fn_put(out, parts[i], lengths[i]);
	}
}

/* ------------------------------------------------------------------------- */
/* DICT operands and operators                                               */
/* ------------------------------------------------------------------------- */

/* ONE OPERAND, ADVANCED. `integer` is false for a real or a fixed-point number, whose value is not used by
 * anything this file reads. */
static bool fn_operand(const unsigned char *d, size_t n, size_t *at, long *value, bool *integer)
{
	unsigned b0;

	if (*at >= n) {
		return false;
	}
	b0 = d[(*at)++];
	*integer = true;
	if (b0 >= 32 && b0 <= 246) {
		*value = (long)b0 - 139;
		return true;
	}
	if (b0 >= 247 && b0 <= 250) {
		if (*at >= n) return false;
		*value = ((long)b0 - 247) * 256 + d[(*at)++] + 108;
		return true;
	}
	if (b0 >= 251 && b0 <= 254) {
		if (*at >= n) return false;
		*value = -((long)b0 - 251) * 256 - d[(*at)++] - 108;
		return true;
	}
	if (b0 == 28) {
		if (*at + 2 > n) return false;
		*value = (int16_t)fn_ru16(d + *at);
		*at += 2;
		return true;
	}
	if (b0 == 29) {
		if (*at + 4 > n) return false;
		*value = (int32_t)fn_ru32(d + *at);
		*at += 4;
		return true;
	}
	if (b0 == 30) {
		while (*at < n) {
			unsigned nib = d[*at];

			*at += 1;
			if ((nib & 0x0F) == 0x0F || (nib >> 4) == 0x0F) {
				break;
			}
		}
		*integer = false;
		return true;
	}
	if (b0 == 255) {
		if (*at + 4 > n) return false;
		*at += 4;
		*integer = false;
		return true;
	}
	return false;
}

struct cff_topdict {
	long charset;
	long encoding;
	long charstrings;
	long private_size;
	long private_offset;
	bool has_charset;
	bool has_encoding;
	bool has_charstrings;
	bool has_private;
	bool cid;
	bool unsupported;
};

static bool fn_parse_topdict(const unsigned char *d, size_t n, struct cff_topdict *top)
{
	size_t at = 0;
	long operands[48];
	size_t count = 0;
	bool integer = true;

	memset(top, 0, sizeof *top);
	while (at < n) {
		unsigned b0 = d[at];

		if (b0 <= 21) {
			unsigned op = b0;
			unsigned op2 = 0;

			at++;
			if (op == 12) {
				if (at >= n) return false;
				op2 = d[at++];
			}
			if (op == 15 && count > 0) {
				top->charset = operands[count - 1];
				top->has_charset = true;
			} else if (op == 16 && count > 0) {
				top->encoding = operands[count - 1];
				top->has_encoding = true;
			} else if (op == 17 && count > 0) {
				top->charstrings = operands[count - 1];
				top->has_charstrings = true;
			} else if (op == 18 && count >= 2) {
				top->private_size = operands[count - 2];
				top->private_offset = operands[count - 1];
				top->has_private = true;
			} else if (op == 12 && op2 == 30) {
				top->cid = true;
			} else if (op == 12 && (op2 == 36 || op2 == 37)) {
				top->unsupported = true;
			}
			count = 0;
			continue;
		}
		{
			long value = 0;

			if (count >= sizeof operands / sizeof operands[0]) {
				return false;
			}
			if (!fn_operand(d, n, &at, &value, &integer)) {
				return false;
			}
			if (integer) {
				operands[count++] = value;
			} else {
				/* A REAL HAS NO PLACE IN OFFSETS OR SIZES; keep a placeholder so the count stays right. */
				operands[count++] = 0;
			}
		}
	}
	return true;
}

/* COPY EVERY TOP-DICT ENTRY THAT THIS FILE DOES NOT REPLACE, byte for byte. */
static void fn_copy_top_entries(cg_cbuf *out, const unsigned char *d, size_t n)
{
	size_t at = 0;
	size_t start = 0;

	while (at < n) {
		unsigned b0 = d[at];

		if (b0 <= 21) {
			unsigned op = b0;
			unsigned op2 = 0;
			size_t after;

			at++;
			if (op == 12) {
				if (at >= n) return;
				op2 = d[at++];
			}
			after = at;
			if (op != 15 && op != 16 && op != 17 && op != 18 &&
			    !(op == 12 && (op2 == 30 || op2 == 36 || op2 == 37))) {
				fn_put(out, d + start, after - start);
			}
			start = after;
			continue;
		}
		{
			long value;
			bool integer;

			if (!fn_operand(d, n, &at, &value, &integer)) {
				return;
			}
		}
	}
}

static void fn_put_int32(cg_cbuf *b, long value)
{
	fn_byte(b, 29);
	fn_u32(b, (uint32_t)value);
}

static void fn_build_topdict(cg_cbuf *out, const unsigned char *orig, size_t orig_len, long charset_offset,
			     long charstrings_offset, long private_size, long private_offset)
{
	fn_copy_top_entries(out, orig, orig_len);
	if (charset_offset >= 0) {
		fn_put_int32(out, charset_offset);
		fn_byte(out, 15);
	}
	if (charstrings_offset >= 0) {
		fn_put_int32(out, charstrings_offset);
		fn_byte(out, 17);
	}
	if (private_size >= 0) {
		fn_put_int32(out, private_size);
		fn_put_int32(out, private_offset);
		fn_byte(out, 18);
	}
}

/* ------------------------------------------------------------------------- */
/* charset                                                                   */
/* ------------------------------------------------------------------------- */

static bool fn_charset_sids(const unsigned char *d, size_t n, bool has_charset, long offset, size_t num_glyphs,
			    unsigned **out)
{
	unsigned *sids;
	size_t gid;

	if (!has_charset || offset == 0) {
		/* ISOAdobe: glyph i is SID i, which is only defined to 228. */
		if (num_glyphs > 229) {
			return false;
		}
		sids = calloc(num_glyphs ? num_glyphs : 1, sizeof *sids);
		if (sids == NULL) {
			return false;
		}
		for (gid = 1; gid < num_glyphs; gid++) {
			sids[gid] = (unsigned)gid;
		}
		*out = sids;
		return true;
	}
	if (offset == 1 || offset == 2) {
		/* THE EXPERT CHARSETS ARE A DIFFERENT TABLE, and they are not a subsetting target here. */
		return false;
	}
	if ((size_t)offset >= n) {
		return false;
	}
	sids = calloc(num_glyphs ? num_glyphs : 1, sizeof *sids);
	if (sids == NULL) {
		return false;
	}
	{
		size_t at = (size_t)offset;
		unsigned format = d[at++];

		if (format == 0) {
			for (gid = 1; gid < num_glyphs; gid++) {
				if (at + 2 > n) {
					free(sids);
					return false;
				}
				sids[gid] = fn_ru16(d + at);
				at += 2;
			}
		} else if (format == 1 || format == 2) {
			gid = 1;
			while (gid < num_glyphs) {
				unsigned first;
				unsigned left;
				unsigned k;

				if (at + (format == 1 ? 3 : 4) > n) {
					free(sids);
					return false;
				}
				first = fn_ru16(d + at);
				at += 2;
				left = format == 1 ? d[at] : fn_ru16(d + at);
				at += format == 1 ? 1 : 2;
				for (k = 0; k <= left && gid < num_glyphs; k++) {
					sids[gid++] = first + k;
				}
			}
		} else {
			free(sids);
			return false;
		}
	}
	*out = sids;
	return true;
}

/* ------------------------------------------------------------------------- */
/* a Type 2 charstring's seac test                                           */
/* ------------------------------------------------------------------------- */

/* TRUE WHEN THE CHARSTRING ENDS WITH `endchar` AFTER FOUR OPERANDS — the deprecated `seac` form, which names
 * its base and accent by STANDARD ENCODING CODE rather than by glyph index. A subset renumbers glyphs, so a
 * charstring that uses seac cannot be kept safely; the caller embeds the whole program instead. */
static bool fn_charstring_has_seac(const unsigned char *p, size_t n)
{
	size_t at = 0;
	int depth = 0;

	while (at < n) {
		unsigned b0 = p[at];

		if (b0 >= 32 && b0 <= 246) {
			at++;
			depth++;
		} else if (b0 >= 247 && b0 <= 254) {
			at += 2;
			depth++;
		} else if (b0 == 28) {
			at += 3;
			depth++;
		} else if (b0 == 255) {
			at += 5;
			depth++;
		} else if (b0 == 30) {
			at++;
			while (at < n) {
				unsigned nib = p[at++];

				if ((nib & 0x0F) == 0x0F || (nib >> 4) == 0x0F) {
					break;
				}
			}
			depth++;
		} else if (b0 == 12) {
			at += 2;
			depth = 0;
		} else if (b0 == 14) {
			if (depth == 4) {
				return true;
			}
			depth = 0;
			at++;
		} else {
			/* EVERY OTHER OPERATOR CONSUMES ITS OPERANDS; the depth is not tracked through subrs, which is
			 * why this test is only ever used to ABORT a subset, never to permit one. */
			at++;
			depth = 0;
		}
		if (at > n) {
			return true;	/* malformed is unsafe */
		}
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* building the subset                                                       */
/* ------------------------------------------------------------------------- */

static int fn_compare_glyphs(const void *a, const void *b)
{
	unsigned ga = *(const unsigned *)a;
	unsigned gb = *(const unsigned *)b;

	return ga < gb ? -1 : (ga > gb ? 1 : 0);
}

cg_pdf_cff_subset *cg_pdf_cff_subset_create(const unsigned char *program, size_t length, const unsigned *glyphs,
					    size_t count)
{
	cff_index name_index;
	cff_index top_index;
	cff_index string_index;
	cff_index global_index;
	cff_index char_index;
	const unsigned char *top_dict = NULL;
	size_t top_dict_len = 0;
	struct cff_topdict top;
	size_t header_size;
	unsigned *sids = NULL;
	unsigned *used = NULL;
	size_t used_count = 0;
	size_t i;
	unsigned char *private_region = NULL;
	size_t private_len = 0;
	const unsigned char **parts = NULL;
	size_t *lengths = NULL;
	cg_cbuf new_chars = { 0 };
	cg_cbuf new_charset = { 0 };
	cg_cbuf top_place = { 0 };
	cg_cbuf top_real = { 0 };
	cg_cbuf top_index_out = { 0 };
	size_t name_span;
	size_t string_span;
	size_t global_span;
	size_t top_index_len;
	size_t char_offset;
	size_t charset_offset;
	size_t private_offset;
	size_t total;
	unsigned char *data = NULL;
	cg_pdf_cff_subset *subset = NULL;

	if (program == NULL || length < 4 || glyphs == NULL || count == 0) {
		return NULL;
	}
	header_size = program[2];
	if (header_size < 4 || header_size > length) {
		return NULL;
	}
	if (!fn_index_parse(program, length, header_size, &name_index) ||
	    !fn_index_parse(program, length, name_index.end, &top_index) || top_index.count != 1 ||
	    !fn_index_entry(&top_index, 0, &top_dict, &top_dict_len) ||
	    !fn_index_parse(program, length, top_index.end, &string_index) ||
	    !fn_index_parse(program, length, string_index.end, &global_index)) {
		return NULL;
	}
	if (!fn_parse_topdict(top_dict, top_dict_len, &top) || top.cid || top.unsupported ||
	    !top.has_charstrings || top.charstrings < 0 || (size_t)top.charstrings >= length) {
		return NULL;
	}
	if (!fn_index_parse(program, length, (size_t)top.charstrings, &char_index) || char_index.count == 0 ||
	    char_index.count > 65535) {
		return NULL;
	}
	if (!fn_charset_sids(program, length, top.has_charset, top.charset, char_index.count, &sids)) {
		goto done;
	}
	/* THE USED GLYPHS, SORTED AND DEDUPLICATED, WITH .notdef ALWAYS FIRST. */
	used = malloc((count + 1) * sizeof *used);
	if (used == NULL) {
		goto done;
	}
	used[used_count++] = 0;
	for (i = 0; i < count; i++) {
		if (glyphs[i] < char_index.count) {
			used[used_count++] = glyphs[i];
		}
	}
	qsort(used, used_count, sizeof *used, fn_compare_glyphs);
	{
		size_t write = 0;

		for (i = 0; i < used_count; i++) {
			if (write == 0 || used[i] != used[write - 1]) {
				used[write++] = used[i];
			}
		}
		used_count = write;
	}
	if (used_count >= char_index.count) {
		goto done;	/* every glyph is used; the full program is the subset */
	}
	parts = calloc(used_count, sizeof *parts);
	lengths = calloc(used_count, sizeof *lengths);
	if (parts == NULL || lengths == NULL) {
		goto done;
	}
	for (i = 0; i < used_count; i++) {
		if (!fn_index_entry(&char_index, used[i], &parts[i], &lengths[i])) {
			goto done;
		}
		if (fn_charstring_has_seac(parts[i], lengths[i])) {
			goto done;
		}
	}
	/* THE PRIVATE DICT, WITH ITS LOCAL SUBRS, COPIED AS ONE REGION. */
	if (top.has_private) {
		size_t region_end;

		if (top.private_size < 0 || top.private_offset < 0 ||
		    (size_t)top.private_offset + (size_t)top.private_size > length) {
			goto done;
		}
		region_end = (size_t)top.private_offset + (size_t)top.private_size;
		{
			const unsigned char *pd = program + top.private_offset;
			size_t pn = (size_t)top.private_size;
			size_t at = 0;
			long entry_value = 0;
			bool entry_integer = false;

			while (at < pn) {
				if (pd[at] <= 21) {
					unsigned op = pd[at];

					at++;
					if (op == 12) {
						at++;
					} else if (op == 19 && entry_integer && entry_value >= 0) {
						cff_index subrs;

						/* THE LOCAL SUBRS LIVE OUTSIDE THE PRIVATE DICT'S DECLARED SIZE; the offset is
						 * relative to the Private DICT's start, so it is parsed against the whole program. */
						if (fn_index_parse(program, length,
								   (size_t)top.private_offset + (size_t)entry_value, &subrs)) {
							/* `subrs.end` is already an absolute index into `program`. */
							if (subrs.end > region_end) {
								region_end = subrs.end;
							}
						}
					}
					entry_integer = false;
					continue;
				}
				{
					long value;
					bool integer;

					if (!fn_operand(pd, pn, &at, &value, &integer)) {
						break;
					}
					entry_integer = integer;
					entry_value = integer ? value : 0;
				}
			}
		}
		if (region_end > length || region_end <= (size_t)top.private_offset) {
			goto done;
		}
		private_len = region_end - (size_t)top.private_offset;
		private_region = malloc(private_len);
		if (private_region == NULL) {
			goto done;
		}
		memcpy(private_region, program + top.private_offset, private_len);
	}
	/* THE NEW CharStrings INDEX AND A FORMAT-0 charset WITH THE OLD SIDs. */
	fn_index_write(&new_chars, parts, lengths, used_count);
	fn_byte(&new_charset, 0);
	for (i = 1; i < used_count; i++) {
		fn_u16(&new_charset, sids[used[i]]);
	}
	/* MEASURE THE TOP DICT WITH PLACEHOLDERS; the offsets are fixed five-byte integers. */
	fn_build_topdict(&top_place, top_dict, top_dict_len, 0, 0, private_len ? (long)private_len : -1, 0);
	top_index_len = 2 + 1 + 4 * 2 + top_place.len;
	name_span = name_index.end - header_size;
	string_span = string_index.end - top_index.end;
	global_span = global_index.end - string_index.end;
	char_offset = header_size + name_span + top_index_len + string_span + global_span;
	charset_offset = char_offset + new_chars.len;
	private_offset = charset_offset + new_charset.len;
	if (char_offset > 0x7FFFFFFF || charset_offset > 0x7FFFFFFF || private_offset > 0x7FFFFFFF) {
		goto done;
	}
	fn_build_topdict(&top_real, top_dict, top_dict_len, (long)charset_offset, (long)char_offset,
			 private_len ? (long)private_len : -1, (long)private_offset);
	if (top_real.len != top_place.len) {
		goto done;
	}
	{
		const unsigned char *top_parts[1] = { top_real.data };
		size_t top_lengths[1] = { top_real.len };

		fn_index_write(&top_index_out, top_parts, top_lengths, 1);
	}
	total = private_offset + private_len;
	data = calloc(total ? total : 1, 1);
	if (data == NULL) {
		goto done;
	}
	memcpy(data, program, header_size);
	memcpy(data + header_size, program + header_size, name_span);
	memcpy(data + header_size + name_span, top_index_out.data, top_index_out.len);
	memcpy(data + header_size + name_span + top_index_out.len, program + top_index.end, string_span);
	memcpy(data + header_size + name_span + top_index_out.len + string_span, program + string_index.end,
	       global_span);
	memcpy(data + char_offset, new_chars.data, new_chars.len);
	memcpy(data + charset_offset, new_charset.data, new_charset.len);
	if (private_len > 0) {
		memcpy(data + private_offset, private_region, private_len);
	}
	subset = calloc(1, sizeof *subset);
	if (subset == NULL) {
		free(data);
		goto done;
	}
	subset->data = data;
	subset->len = total;
	data = NULL;

done:
	free(data);
	free(private_region);
	free(parts);
	free(lengths);
	free(sids);
	free(used);
	fn_free(&new_chars);
	fn_free(&new_charset);
	fn_free(&top_place);
	fn_free(&top_real);
	fn_free(&top_index_out);
	return subset;
}

void cg_pdf_cff_subset_free(cg_pdf_cff_subset *subset)
{
	if (subset == NULL) {
		return;
	}
	free(subset->data);
	free(subset);
}

const unsigned char *cg_pdf_cff_subset_data(cg_pdf_cff_subset *subset, size_t *length)
{
	if (subset == NULL) {
		return NULL;
	}
	if (length != NULL) {
		*length = subset->len;
	}
	return subset->data;
}
