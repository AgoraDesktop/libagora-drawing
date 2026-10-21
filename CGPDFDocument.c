/*
 * CGPDFDocument.c — opening the file: the header, the cross-reference, the trailer and the page tree.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **A PDF IS READ BACKWARDS, WHICH IS THE ONE STRUCTURAL FACT THAT MAKES IT A PDF.** The last thing in the file
 * is `startxref`, a byte offset to the newest cross-reference section; that section names every object's offset
 * and its `/Prev`, and the chain of `/Prev` links is the history of the file's edits. This file walks that chain
 * FROM NEWEST TO OLDEST and never overwrites an answer an earlier section gave — which is why an incremental
 * update that changes one page makes the old bytes irrelevant without removing them.
 *
 * **BOTH SPELLINGS OF THE CROSS-REFERENCE ARE READ.** A classic `xref` section is a text table followed by
 * `trailer`; a PDF 1.5 cross-reference STREAM is an object whose data is a table of fixed-width fields, and it
 * is also the trailer. A hybrid file has both, with `/XRefStm`, and this reader fills the gaps from the second
 * the same way it fills them from `/Prev`.
 *
 * **OBJECT STREAMS ARE THE OTHER PDF 1.5 CHANGE AND THEY MOVE THE OBJECTS THEMSELVES.** An object in an object
 * stream has no offset in the file; the cross-reference says "number 7 is at index 0 in stream 6". `fn_load_
 * object_stream` inflates stream 6 once and keeps its objects, which is the only way to resolve them without
 * re-inflating the stream per object — the difference between linear and quadratic on a file that packs all of
 * its dictionaries into one stream, as most modern files do.
 *
 * **THE PAGE TREE IS FLATTENED ONCE, AT OPEN.** `/Kids` is a tree and inheritance may live at any level;
 * `cg_pdf_page_info` holds the effective boxes and rotation, so `CGPDFPageGetBoxRect` is a field read rather
 * than a walk. The walk is depth-capped and count-capped, because `/Kids` in a damaged file can point at itself.
 *
 * **AN ENCRYPTED FILE OPENS AND REFUSES.** Opening it is how `IsEncrypted` gets to answer; the refusal is on
 * the pages, catalog and info, because there is no decryption here.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGPDFPage.h>

#include <CoreFoundation/CFRuntime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* the cross-reference table                                                 */
/* ------------------------------------------------------------------------- */

static bool fn_xref_grow(cg_pdf_doc *doc, size_t need)
{
	if (need <= doc->xref_cap) {
		return true;
	}
	{
		size_t cap = doc->xref_cap ? doc->xref_cap : 64;
		cg_pdf_xref_entry *grown;

		while (cap < need) {
			cap *= 2;
		}
		grown = realloc(doc->xref, cap * sizeof *grown);
		if (grown == NULL) {
			return false;
		}
		memset(grown + doc->xref_cap, 0, (cap - doc->xref_cap) * sizeof *grown);
		doc->xref = grown;
		doc->xref_cap = cap;
		if (doc->xref_count < cap) {
			doc->xref_count = cap;
		}
	}
	return true;
}

/* NEWEST WINS, so a later section never replaces an entry an earlier one set. */
static void fn_xref_set(cg_pdf_doc *doc, unsigned num, unsigned char type, unsigned long long a,
			unsigned long long b)
{
	if (!fn_xref_grow(doc, (size_t)num + 1)) {
		return;
	}
	if (doc->xref[num].present) {
		return;
	}
	doc->xref[num].present = 1;
	doc->xref[num].type = type;
	doc->xref[num].a = a;
	doc->xref[num].b = b;
}

static void fn_trailer_merge(cg_pdf_doc *doc, cg_pdf_obj *trailer)
{
	size_t i;

	if (trailer == NULL || trailer->type != kCGPDFObjectTypeDictionary) {
		return;
	}
	if (doc->trailer == NULL) {
		doc->trailer = cg_pdf_obj_new(doc, kCGPDFObjectTypeDictionary);
		if (doc->trailer == NULL) {
			return;
		}
	}
	for (i = 0; i < trailer->u.dict.count; i++) {
		const char *key = trailer->u.dict.entries[i].key;
		bool present = false;
		size_t j;

		for (j = 0; j < doc->trailer->u.dict.count; j++) {
			if (strcmp(doc->trailer->u.dict.entries[j].key, key) == 0) {
				present = true;
				break;
			}
		}
		if (!present) {
			if (!cg_pdf_dict_set(doc->trailer, (char *)key, trailer->u.dict.entries[i].value)) {
				return;
			}
		}
	}
}

static bool fn_read_int(cg_pdf_lexer *lx, long long *out)
{
	/* THE NUMBER READER IS THE OBJECT PARSER'S, reached through a one-object parse so that the two cannot
	 * disagree about what a number is. A real is truncated, which is what a file that writes `12.0` for an
	 * offset means. */
	cg_pdf_obj *obj;

	cg_pdf_lexer_skip_space(lx);
	if (lx->pos >= lx->size) {
		return false;
	}
	obj = cg_pdf_parse_object(lx, 0);
	if (obj == NULL) {
		return false;
	}
	if (obj->type == kCGPDFObjectTypeInteger) {
		*out = obj->u.integer;
		return true;
	}
	if (obj->type == kCGPDFObjectTypeReal) {
		*out = (long long)obj->u.real;
		return true;
	}
	return false;
}

static bool fn_read_xref_section(cg_pdf_doc *doc, size_t offset, size_t *prev);

/* A CROSS-REFERENCE STREAM'S TABLE. `W` gives the byte width of each of the three fields and `Index` the
 * subsections; the bytes themselves are the stream's DECODED data, so a Flate-encoded table is already inflated
 * by the time this reads it. */
static bool fn_read_xref_stream(cg_pdf_doc *doc, cg_pdf_obj *stream, size_t *prev)
{
	cg_pdf_obj *dict = stream->u.stream.dict;
	cg_pdf_obj *w;
	cg_pdf_obj *index = NULL;
	unsigned long long widths[3] = { 1, 4, 2 };
	CFDataRef data;
	const unsigned char *bytes;
	size_t size;
	size_t at = 0;
	size_t subsection = 0;

	w = cg_pdf_dict_get(doc, dict, "W");
	if (w == NULL || w->type != kCGPDFObjectTypeArray || w->u.array.count < 3) {
		fprintf(stderr, "CG-REFUSE: a cross-reference stream has no usable /W array\n");
		return false;
	}
	{
		size_t i;

		for (i = 0; i < 3; i++) {
			double value;

			if (!cg_pdf_number(doc, w->u.array.items[i], &value) || value < 0 || value > 8) {
				fprintf(stderr, "CG-REFUSE: a cross-reference stream has a /W field wider than 8 "
						"bytes, which no PDF can use\n");
				return false;
			}
			widths[i] = (unsigned long long)value;
		}
	}
	index = cg_pdf_dict_get(doc, dict, "Index");
	data = cg_pdf_stream_data(doc, stream);
	if (data == NULL) {
		return false;
	}
	bytes = CFDataGetBytePtr(data);
	size = (size_t)CFDataGetLength(data);

	{
		size_t entries_left = 0;
		unsigned long long first = 0;
		bool have_index = index != NULL && index->type == kCGPDFObjectTypeArray;

		for (;;) {
			size_t row_width = (size_t)(widths[0] + widths[1] + widths[2]);
			unsigned long long fields[3] = { 1, 0, 0 };
			size_t field;

			if (entries_left == 0) {
				if (have_index) {
					if (subsection * 2 + 1 >= index->u.array.count) {
						break;
					}
					{
						double d;

						if (!cg_pdf_number(doc, index->u.array.items[subsection * 2], &d)) {
							break;
						}
						first = (unsigned long long)d;
						if (!cg_pdf_number(doc, index->u.array.items[subsection * 2 + 1], &d)) {
							break;
						}
						entries_left = (size_t)d;
					}
					subsection++;
				} else {
					double d;

					if (subsection > 0) {
						break;
					}
					subsection = 1;
					if (!cg_pdf_number(doc, cg_pdf_dict_get(doc, dict, "Size"), &d)) {
						break;
					}
					first = 0;
					entries_left = (size_t)d;
				}
				if (row_width == 0) {
					break;
				}
			}
			if (at + row_width > size) {
				break;
			}
			for (field = 0; field < 3; field++) {
				unsigned long long value = 0;
				size_t k;

				for (k = 0; k < (size_t)widths[field]; k++) {
					value = (value << 8) | bytes[at++];
				}
				fields[field] = value;
			}
			/* A ZERO WIDTH FOR THE TYPE FIELD MEANS TYPE 1, which is how a file says "these are all
			 * plain objects" without spending a byte per row. */
			if (widths[0] == 0) {
				fields[0] = 1;
			}
			fn_xref_set(doc, (unsigned)first, (unsigned char)fields[0], fields[1], fields[2]);
			first++;
			entries_left--;
		}
	}
	CFRelease(data);

	if (prev != NULL) {
		cg_pdf_obj *p = cg_pdf_dict_get(doc, dict, "Prev");
		double d;

		*prev = (p != NULL && cg_pdf_number(doc, p, &d) && d >= 0) ? (size_t)d : 0;
	}
	return true;
}

static bool fn_read_classic_xref(cg_pdf_doc *doc, cg_pdf_lexer *lx, size_t *prev)
{
	for (;;) {
		size_t save = lx->pos;
		long long start;
		long long count;

		cg_pdf_lexer_skip_space(lx);
		if (cg_pdf_lexer_keyword(lx, "trailer")) {
			break;
		}
		lx->pos = save;
		if (!fn_read_int(lx, &start) || !fn_read_int(lx, &count)) {
			fprintf(stderr, "CG-REFUSE: a cross-reference subsection header is not two integers\n");
			return false;
		}
		if (count < 0 || count > 10000000) {
			return false;
		}
		{
			long long i;

			for (i = 0; i < count; i++) {
				long long offset;
				long long generation;

				if (!fn_read_int(lx, &offset) || !fn_read_int(lx, &generation)) {
					fprintf(stderr, "CG-REFUSE: a cross-reference entry is not two integers\n");
					return false;
				}
				cg_pdf_lexer_skip_space(lx);
				if (lx->pos >= lx->size) {
					return false;
				}
				{
					unsigned char kind = lx->data[lx->pos++];

					if (kind == 'n') {
						fn_xref_set(doc, (unsigned)(start + i), 1, (unsigned long long)offset,
							    (unsigned long long)generation);
					} else if (kind == 'f') {
						fn_xref_set(doc, (unsigned)(start + i), 0, (unsigned long long)offset,
							    (unsigned long long)generation);
					} else {
						fprintf(stderr, "CG-REFUSE: a cross-reference entry ends in `%c` rather "
								"than `n` or `f`\n", kind);
						return false;
					}
				}
			}
		}
	}
	{
		cg_pdf_obj *trailer = cg_pdf_parse_object(lx, 0);

		if (trailer == NULL || trailer->type != kCGPDFObjectTypeDictionary) {
			fprintf(stderr, "CG-REFUSE: a classic cross-reference section has no trailer dictionary\n");
			return false;
		}
		fn_trailer_merge(doc, trailer);
		if (prev != NULL) {
			cg_pdf_obj *p = cg_pdf_dict_get(doc, trailer, "Prev");
			double d;

			*prev = (p != NULL && cg_pdf_number(doc, p, &d) && d >= 0) ? (size_t)d : 0;
		}
		/* A HYBRID FILE'S SUPPLEMENT: /XRefStm points at a cross-reference stream whose entries fill the
		 * gaps this table left. Its own /Prev is not followed — the classic section's is. */
		{
			cg_pdf_obj *stm = cg_pdf_dict_get(doc, trailer, "XRefStm");
			double d;

			if (stm != NULL && cg_pdf_number(doc, stm, &d) && d >= 0 && (size_t)d < doc->size) {
				size_t ignored = 0;

				fn_read_xref_section(doc, (size_t)d, &ignored);
			}
		}
	}
	return true;
}

static bool fn_read_xref_section(cg_pdf_doc *doc, size_t offset, size_t *prev)
{
	cg_pdf_lexer lx;
	size_t save;

	if (prev != NULL) {
		*prev = 0;
	}
	if (offset >= doc->size) {
		return false;
	}
	cg_pdf_lexer_init(&lx, doc, offset);
	save = lx.pos;
	cg_pdf_lexer_skip_space(&lx);
	if (cg_pdf_lexer_keyword(&lx, "xref")) {
		return fn_read_classic_xref(doc, &lx, prev);
	}
	lx.pos = save;
	{
		cg_pdf_obj *stream = cg_pdf_parse_indirect(&lx, NULL);

		if (stream == NULL || stream->type != kCGPDFObjectTypeStream) {
			fprintf(stderr, "CG-REFUSE: the byte offset a cross-reference section names holds "
					"neither a table nor a stream\n");
			return false;
		}
		if (!cg_pdf_name_is(doc, cg_pdf_dict_get(doc, stream->u.stream.dict, "Type"), "XRef")) {
			fprintf(stderr, "CG-REFUSE: a cross-reference stream is not marked /Type /XRef\n");
			return false;
		}
		fn_trailer_merge(doc, stream->u.stream.dict);
		return fn_read_xref_stream(doc, stream, prev);
	}
}

/* ------------------------------------------------------------------------- */
/* the header and the load                                                   */
/* ------------------------------------------------------------------------- */

static void fn_read_header(cg_pdf_doc *doc)
{
	size_t limit = doc->size < 1024 ? doc->size : 1024;
	size_t i;

	for (i = 0; i + 5 <= limit; i++) {
		if (memcmp(doc->bytes + i, "%PDF-", 5) == 0) {
			size_t at = i + 5;
			int major = 0;
			int minor = 0;

			while (at < doc->size && doc->bytes[at] >= '0' && doc->bytes[at] <= '9') {
				major = major * 10 + (doc->bytes[at] - '0');
				at++;
			}
			if (at < doc->size && doc->bytes[at] == '.') {
				at++;
				while (at < doc->size && doc->bytes[at] >= '0' && doc->bytes[at] <= '9') {
					minor = minor * 10 + (doc->bytes[at] - '0');
					at++;
				}
			}
			doc->major = major;
			doc->minor = minor;
			return;
		}
	}
}

static bool fn_find_startxref(cg_pdf_doc *doc, size_t *out)
{
	size_t window = doc->size < 4096 ? doc->size : 4096;
	size_t start = doc->size - window;
	size_t i;

	if (doc->size < 9) {
		return false;
	}
	for (i = doc->size - 9;; i--) {
		if (memcmp(doc->bytes + i, "startxref", 9) == 0) {
			cg_pdf_lexer lx;

			cg_pdf_lexer_init(&lx, doc, i + 9);
			{
				long long value;

				if (fn_read_int(&lx, &value) && value >= 0) {
					*out = (size_t)value;
					return true;
				}
			}
			return false;
		}
		if (i == start) {
			break;
		}
	}
	return false;
}

static bool fn_load_xref(cg_pdf_doc *doc)
{
	size_t offset;
	size_t guard = 0;
	size_t previous = (size_t)-1;

	if (!fn_find_startxref(doc, &offset)) {
		fprintf(stderr, "CG-REFUSE: this file has no `startxref`, so it has no cross-reference and "
				"there is nowhere to start reading it\n");
		return false;
	}
	while (offset != 0) {
		size_t next = 0;

		if (++guard > 256) {
			fprintf(stderr, "CG-REFUSE: more than 256 cross-reference sections in the /Prev chain — "
					"this is a loop\n");
			return false;
		}
		if (offset >= doc->size) {
			fprintf(stderr, "CG-REFUSE: a /Prev offset points past the end of the file\n");
			break;
		}
		/* `/Prev` NAMES AN EARLIER SECTION. An offset that does not move backwards is the loop a
		 * damaged file uses to hang a reader, and stopping keeps the sections already merged. */
		if (offset >= previous) {
			break;
		}
		previous = offset;
		if (!fn_read_xref_section(doc, offset, &next)) {
			break;
		}
		offset = next;
	}
	doc->cache = calloc(doc->xref_count ? doc->xref_count : 1, sizeof *doc->cache);
	return doc->cache != NULL;
}

/* ------------------------------------------------------------------------- */
/* object resolution, including compressed objects                           */
/* ------------------------------------------------------------------------- */

static void fn_compressed_add(cg_pdf_doc *doc, unsigned num, cg_pdf_obj *obj)
{
	if (doc->compressed_count == doc->compressed_cap) {
		size_t cap = doc->compressed_cap ? doc->compressed_cap * 2 : 16;
		void *grown = realloc(doc->compressed, cap * sizeof *doc->compressed);

		if (grown == NULL) {
			return;
		}
		doc->compressed = grown;
		doc->compressed_cap = cap;
	}
	doc->compressed[doc->compressed_count].num = num;
	doc->compressed[doc->compressed_count].obj = obj;
	doc->compressed_count++;
}

static cg_pdf_obj *fn_compressed_find(cg_pdf_doc *doc, unsigned num)
{
	size_t i;

	for (i = 0; i < doc->compressed_count; i++) {
		if (doc->compressed[i].num == num) {
			return doc->compressed[i].obj;
		}
	}
	return NULL;
}

/* ONE INFLATE FOR THE WHOLE STREAM, THEN EVERY OBJECT IN IT. The header is `N` pairs of object number and
 * offset, both decimal, and the offsets are from the byte AFTER the header. */
static bool fn_load_object_stream(cg_pdf_doc *doc, unsigned stream_number)
{
	cg_pdf_obj *stream = cg_pdf_object_number(doc, stream_number);
	cg_pdf_obj *n_obj;
	double n_value;
	double first_value;
	CFDataRef data;
	const unsigned char *bytes;
	size_t size;
	long long n;
	long long first;
	cg_pdf_lexer header;
	long long i;

	if (stream == NULL || stream->type != kCGPDFObjectTypeStream) {
		return false;
	}
	n_obj = cg_pdf_dict_get(doc, stream->u.stream.dict, "N");
	if (n_obj == NULL || !cg_pdf_number(doc, n_obj, &n_value) ||
	    !cg_pdf_number(doc, cg_pdf_dict_get(doc, stream->u.stream.dict, "First"), &first_value)) {
		fprintf(stderr, "CG-REFUSE: an object stream lacks /N or /First\n");
		return false;
	}
	n = (long long)n_value;
	first = (long long)first_value;
	if (n <= 0 || n > 1000000 || first < 0) {
		return false;
	}
	data = cg_pdf_stream_data(doc, stream);
	if (data == NULL) {
		return false;
	}
	bytes = CFDataGetBytePtr(data);
	size = (size_t)CFDataGetLength(data);
	if ((size_t)first > size) {
		CFRelease(data);
		return false;
	}
	memset(&header, 0, sizeof header);
	header.doc = doc;
	header.data = bytes;
	header.size = size;
	header.pos = 0;
	for (i = 0; i < n; i++) {
		long long number;
		long long offset;
		cg_pdf_lexer object_lexer;

		if (!fn_read_int(&header, &number) || !fn_read_int(&header, &offset)) {
			break;
		}
		memset(&object_lexer, 0, sizeof object_lexer);
		object_lexer.doc = doc;
		object_lexer.data = bytes;
		object_lexer.size = size;
		object_lexer.pos = (size_t)first + (size_t)offset;
		{
			cg_pdf_obj *obj = cg_pdf_parse_object(&object_lexer, 0);

			if (obj != NULL) {
				fn_compressed_add(doc, (unsigned)number, obj);
			}
		}
	}
	CFRelease(data);
	return true;
}

cg_pdf_obj *cg_pdf_object_number(cg_pdf_doc *doc, unsigned num)
{
	cg_pdf_xref_entry *entry;
	cg_pdf_obj *obj = NULL;

	if (doc->cache != NULL && num < doc->xref_count && doc->cache[num] != NULL) {
		return doc->cache[num];
	}
	obj = fn_compressed_find(doc, num);
	if (obj != NULL) {
		return obj;
	}
	if (num >= doc->xref_count || !doc->xref[num].present) {
		return NULL;
	}
	entry = &doc->xref[num];
	if (entry->type == 1) {
		cg_pdf_lexer lx;
		unsigned parsed = 0;

		if (entry->a >= doc->size) {
			return NULL;
		}
		cg_pdf_lexer_init(&lx, doc, (size_t)entry->a);
		obj = cg_pdf_parse_indirect(&lx, &parsed);
		if (obj != NULL && parsed != num) {
			fprintf(stderr, "CG-REFUSE: the cross-reference says object %u is at offset %llu, and "
					"the object there is %u (the later one is believed)\n", num,
				entry->a, parsed);
		}
	} else if (entry->type == 2) {
		fn_load_object_stream(doc, (unsigned)entry->a);
		obj = fn_compressed_find(doc, num);
	}
	if (doc->cache != NULL && num < doc->xref_count) {
		doc->cache[num] = obj;
	}
	return obj;
}

/* ------------------------------------------------------------------------- */
/* the page tree                                                             */
/* ------------------------------------------------------------------------- */

typedef struct fn_inherited {
	cg_pdf_obj *media;
	cg_pdf_obj *crop;
	cg_pdf_obj *bleed;
	cg_pdf_obj *trim;
	cg_pdf_obj *art;
	cg_pdf_obj *rotate;
	cg_pdf_obj *resources;
} fn_inherited;

static bool fn_pages_push(cg_pdf_doc *doc, const cg_pdf_page_info *info)
{
	if (doc->page_count == doc->page_cap) {
		size_t cap = doc->page_cap ? doc->page_cap * 2 : 8;
		cg_pdf_page_info *grown = realloc(doc->pages, cap * sizeof *grown);

		if (grown == NULL) {
			return false;
		}
		doc->pages = grown;
		doc->page_cap = cap;
	}
	doc->pages[doc->page_count++] = *info;
	return true;
}

static int fn_normalise_rotation(long long angle)
{
	angle %= 360;
	if (angle < 0) {
		angle += 360;
	}
	angle -= angle % 90;
	return (int)angle;
}

static void fn_add_page(cg_pdf_doc *doc, cg_pdf_obj *page, const fn_inherited *inh)
{
	cg_pdf_page_info info;
	bool ok = false;
	CGRect media;
	CGRect crop;
	long long rotation = 0;

	memset(&info, 0, sizeof info);
	info.dict = page;
	info.resources = inh->resources;
	media = cg_pdf_rect_from_object(doc, inh->media, &ok);
	if (!ok) {
		fprintf(stderr, "CG-REFUSE: a page has no /MediaBox and no ancestor supplies one — the page "
				"is counted with an empty box rather than an invented size\n");
		media = CGRectZero;
	}
	crop = cg_pdf_rect_from_object(doc, inh->crop, &ok);
	if (!ok) {
		crop = media;
	} else {
		CGRect clipped = CGRectIntersection(crop, media);

		if (!CGRectIsNull(clipped)) {
			crop = clipped;
		}
	}
	info.media = media;
	info.crop = crop;
	{
		CGRect box = cg_pdf_rect_from_object(doc, inh->bleed, &ok);

		info.bleed = ok ? box : crop;
		box = cg_pdf_rect_from_object(doc, inh->trim, &ok);
		info.trim = ok ? box : crop;
		box = cg_pdf_rect_from_object(doc, inh->art, &ok);
		info.art = ok ? box : crop;
	}
	if (cg_pdf_integer(doc, inh->rotate, &rotation)) {
		info.rotate = fn_normalise_rotation(rotation);
	}
	fn_pages_push(doc, &info);
}

static void fn_walk_pages(cg_pdf_doc *doc, cg_pdf_obj *node, fn_inherited inh, int depth)
{
	cg_pdf_obj *kids;
	cg_pdf_obj *type;
	size_t i;

	if (depth > 64 || node == NULL) {
		fprintf(stderr, "CG-REFUSE: the page tree is deeper than 64 levels or has a missing node — "
				"the walk stops there\n");
		return;
	}
	if (doc->page_count > 1000000) {
		return;
	}
	{
		cg_pdf_obj *value;

		value = cg_pdf_dict_get(doc, node, "MediaBox");
		if (value != NULL) inh.media = value;
		value = cg_pdf_dict_get(doc, node, "CropBox");
		if (value != NULL) inh.crop = value;
		value = cg_pdf_dict_get(doc, node, "BleedBox");
		if (value != NULL) inh.bleed = value;
		value = cg_pdf_dict_get(doc, node, "TrimBox");
		if (value != NULL) inh.trim = value;
		value = cg_pdf_dict_get(doc, node, "ArtBox");
		if (value != NULL) inh.art = value;
		value = cg_pdf_dict_get(doc, node, "Rotate");
		if (value != NULL) inh.rotate = value;
		value = cg_pdf_dict_get(doc, node, "Resources");
		if (value != NULL) inh.resources = value;
	}
	type = cg_pdf_dict_get(doc, node, "Type");
	kids = cg_pdf_dict_get(doc, node, "Kids");
	if ((type != NULL && cg_pdf_name_is(doc, type, "Pages")) ||
	    (kids != NULL && kids->type == kCGPDFObjectTypeArray)) {
		size_t count;

		if (kids == NULL || kids->type != kCGPDFObjectTypeArray) {
			return;
		}
		count = kids->u.array.count;
		for (i = 0; i < count; i++) {
			cg_pdf_obj *kid = cg_pdf_resolve(doc, kids->u.array.items[i]);

			fn_walk_pages(doc, kid, inh, depth + 1);
		}
		return;
	}
	fn_add_page(doc, node, &inh);
}

static void fn_build_pages(cg_pdf_doc *doc)
{
	fn_inherited inh;
	cg_pdf_obj *root;

	if (doc->pages_built) {
		return;
	}
	doc->pages_built = true;
	if (doc->encrypted) {
		return;
	}
	root = cg_pdf_dict_get(doc, doc->catalog, "Pages");
	if (root == NULL) {
		fprintf(stderr, "CG-REFUSE: this document's catalog has no /Pages, so it has no pages\n");
		return;
	}
	memset(&inh, 0, sizeof inh);
	fn_walk_pages(doc, root, inh, 0);
}

/* ------------------------------------------------------------------------- */
/* the object                                                                */
/* ------------------------------------------------------------------------- */

struct CGPDFDocument {
	CFRuntimeBase base;
	cg_pdf_doc *pdf;
	CGPDFPageRef *pages;
	size_t page_capacity;
};

static void fn_document_finalize(CFTypeRef object)
{
	CGPDFDocumentRef document = (CGPDFDocumentRef)object;
	size_t i;

	if (document->pages != NULL) {
		for (i = 0; i < document->page_capacity; i++) {
			if (document->pages[i] != NULL) {
				CGPDFPageRelease(document->pages[i]);
			}
		}
		free(document->pages);
		document->pages = NULL;
	}
	if (document->pdf != NULL) {
		cg_pdf_doc_free(document->pdf);
		document->pdf = NULL;
	}
}

CGTypeID CGPDFDocumentGetTypeID(void)
{
	static CFTypeID type_id = 0;

	if (type_id == 0) {
		static const CFRuntimeClass klass = {
			0,
			"CGPDFDocument",
			NULL,
			NULL,
			fn_document_finalize,
			NULL,
			NULL,
			NULL,
			NULL
		};

		type_id = _CFRuntimeRegisterClass(&klass);
	}
	return (CGTypeID)type_id;
}

static CGPDFDocumentRef fn_document_wrap(cg_pdf_doc *pdf)
{
	CGPDFDocumentRef document;

	if (pdf == NULL) {
		return NULL;
	}
	document = (CGPDFDocumentRef)_CFRuntimeCreateInstance(
		kCFAllocatorDefault, CGPDFDocumentGetTypeID(),
		(CFIndex)(sizeof(struct CGPDFDocument) - sizeof(CFRuntimeBase)), NULL);
	if (document == NULL) {
		cg_pdf_doc_free(pdf);
		return NULL;
	}
	document->pdf = pdf;
	return document;
}

/* THE ONE LOADER. Everything a document knows is decided here: the header, the cross-reference chain, the
 * trailer merge, the catalog and the flattened pages. A file that fails at any structural step is refused by
 * name rather than opened as an empty document, because "zero pages" is a claim and not an error report. */
static CGPDFDocumentRef fn_document_open(CFDataRef data)
{
	cg_pdf_doc *pdf;

	if (data == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFDocument was given no bytes\n");
		return NULL;
	}
	pdf = calloc(1, sizeof *pdf);
	if (pdf == NULL) {
		return NULL;
	}
	pdf->data = (CFDataRef)CFRetain(data);
	pdf->bytes = CFDataGetBytePtr(data);
	pdf->size = (size_t)CFDataGetLength(data);
	fn_read_header(pdf);
	if (!fn_load_xref(pdf)) {
		fprintf(stderr, "CG-REFUSE: this file's cross-reference could not be read, so no object has "
				"an address\n");
		cg_pdf_doc_free(pdf);
		return NULL;
	}
	if (pdf->trailer == NULL) {
		fprintf(stderr, "CG-REFUSE: this file has no trailer dictionary\n");
		cg_pdf_doc_free(pdf);
		return NULL;
	}
	pdf->encrypted = cg_pdf_dict_get(pdf, pdf->trailer, "Encrypt") != NULL;
	pdf->catalog = cg_pdf_dict_get(pdf, pdf->trailer, "Root");
	pdf->info = cg_pdf_dict_get(pdf, pdf->trailer, "Info");
	if (pdf->catalog == NULL) {
		fprintf(stderr, "CG-REFUSE: this file's trailer has no /Root, so it has no catalog\n");
	}
	fn_build_pages(pdf);
	return fn_document_wrap(pdf);
}

CGPDFDocumentRef CGPDFDocumentCreateWithProvider(CGDataProviderRef provider)
{
	CFDataRef data;
	CGPDFDocumentRef document;

	if (provider == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFDocumentCreateWithProvider needs a provider\n");
		return NULL;
	}
	data = CGDataProviderCopyData(provider);
	if (data == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFDocument could not read the provider's bytes\n");
		return NULL;
	}
	document = fn_document_open(data);
	CFRelease(data);
	return document;
}

CGPDFDocumentRef CGPDFDocumentCreateWithURL(CFURLRef url)
{
	CGDataProviderRef provider;
	CGPDFDocumentRef document;

	if (url == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFDocumentCreateWithURL needs a URL\n");
		return NULL;
	}
	provider = CGDataProviderCreateWithURL(url);
	if (provider == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFDocument could not open the file this URL names\n");
		return NULL;
	}
	document = CGPDFDocumentCreateWithProvider(provider);
	CGDataProviderRelease(provider);
	return document;
}

void CGPDFDocumentRetain(CGPDFDocumentRef document)
{
	if (document != NULL) {
		CFRetain(document);
	}
}

void CGPDFDocumentRelease(CGPDFDocumentRef document)
{
	if (document != NULL) {
		CFRelease(document);
	}
}

size_t CGPDFDocumentGetNumberOfPages(CGPDFDocumentRef document)
{
	if (document == NULL || document->pdf == NULL) {
		return 0;
	}
	return document->pdf->encrypted ? 0 : document->pdf->page_count;
}

CGPDFPageRef CGPDFDocumentGetPage(CGPDFDocumentRef document, size_t pageNumber)
{
	cg_pdf_doc *pdf;

	if (document == NULL || document->pdf == NULL || pageNumber == 0) {
		return NULL;
	}
	pdf = document->pdf;
	if (pdf->encrypted) {
		fprintf(stderr, "CG-REFUSE: CGPDFDocumentGetPage on an encrypted document — this reader has no "
				"decryption, and a page of ciphertext drawn as content would be a lie\n");
		return NULL;
	}
	if (pageNumber > pdf->page_count) {
		return NULL;
	}
	if (document->pages == NULL) {
		document->page_capacity = pageNumber;
		document->pages = calloc(document->page_capacity, sizeof *document->pages);
		if (document->pages == NULL) {
			return NULL;
		}
	}
	if (pageNumber > document->page_capacity) {
		size_t cap = pageNumber;
		CGPDFPageRef *grown = realloc(document->pages, cap * sizeof *document->pages);

		if (grown == NULL) {
			return NULL;
		}
		memset(grown + document->page_capacity, 0,
		       (cap - document->page_capacity) * sizeof *grown);
		document->pages = grown;
		document->page_capacity = cap;
	}
	if (document->pages[pageNumber - 1] == NULL) {
		document->pages[pageNumber - 1] = cg_pdf_page_create(document, pdf, pageNumber - 1);
	}
	return document->pages[pageNumber - 1];
}

void CGPDFDocumentGetVersion(CGPDFDocumentRef document, int *majorVersion, int *minorVersion)
{
	if (majorVersion != NULL) {
		*majorVersion = document != NULL && document->pdf != NULL ? document->pdf->major : 0;
	}
	if (minorVersion != NULL) {
		*minorVersion = document != NULL && document->pdf != NULL ? document->pdf->minor : 0;
	}
}

CGPDFDictionaryRef CGPDFDocumentGetInfo(CGPDFDocumentRef document)
{
	if (document == NULL || document->pdf == NULL || document->pdf->encrypted) {
		return NULL;
	}
	return (CGPDFDictionaryRef)document->pdf->info;
}

CGPDFDictionaryRef CGPDFDocumentGetCatalog(CGPDFDocumentRef document)
{
	if (document == NULL || document->pdf == NULL || document->pdf->encrypted) {
		return NULL;
	}
	return (CGPDFDictionaryRef)document->pdf->catalog;
}

CGPDFArrayRef CGPDFDocumentGetID(CGPDFDocumentRef document)
{
	if (document == NULL || document->pdf == NULL || document->pdf->trailer == NULL) {
		return NULL;
	}
	return (CGPDFArrayRef)cg_pdf_dict_get(document->pdf, document->pdf->trailer, "ID");
}

bool CGPDFDocumentIsEncrypted(CGPDFDocumentRef document)
{
	return document != NULL && document->pdf != NULL && document->pdf->encrypted;
}

bool CGPDFDocumentIsUnlocked(CGPDFDocumentRef document)
{
	return document != NULL && document->pdf != NULL && !document->pdf->encrypted;
}

bool CGPDFDocumentGetAllowsPrinting(CGPDFDocumentRef document)
{
	return document != NULL && document->pdf != NULL && !document->pdf->encrypted;
}

bool CGPDFDocumentGetAllowsCopying(CGPDFDocumentRef document)
{
	return document != NULL && document->pdf != NULL && !document->pdf->encrypted;
}
