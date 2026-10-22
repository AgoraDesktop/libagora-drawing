/*
 * CGPDFWriter.c — recording drawing calls into content streams, and writing the file.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **THE FILE IS THREE LAYERS AND THEY ARE SEPARATE ON PURPOSE.**
 *
 *   1. A content buffer per page, appended to by the recording doors. Every paint carries its own colour, line
 *      state and effective alpha, so recording never has to mirror the graphics state — the one thing that
 *      would have to be kept in step with `q`/`Q` is emitted at the moment it is used.
 *   2. An object table. Pages, content streams, images and ExtGStates are numbered when they are created and
 *      written in that order; only the catalog, the page tree and the info dictionary wait, because their
 *      contents are not known until the last page has ended.
 *   3. The serialiser: header, objects with their byte offsets, a classic cross-reference table, the trailer.
 *
 * **A PATH IS REPLAYED FROM `CGPathApply`, NOT FROM THE CALLS THAT BUILT IT.** This is why the recorder can live
 * beside the rasterizer without duplicating path construction: the context's path is already the thing the
 * caller meant, quads included (raised to cubics here with the same two-thirds rule the flattener uses).
 *
 * **IMAGE PIXELS ARE READ THROUGH THE SAME LAYOUT DOOR THE BLITTER USES** (`cg_image_layout`), converted to
 * 8-bit RGB, Flate-compressed, and emitted as an image XObject; an image with alpha gets a DeviceGray /SMask.
 * A premultiplied source is un-premultiplied first, because PDF's samples are straight.
 *
 * THE COORDINATES ARE THE CONTEXT'S USER SPACE, WHICH IS PDF'S. No flip is introduced: `CGContext`'s user space
 * is y-up and so is a PDF page's, and `cm` replays the caller's transforms.
 */
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGPDFWriter_internal.h>
#include <CoreGraphics/CGEPSWriter.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGImage_internal.h>
#include <CoreGraphics/CGFont_internal.h>
#include <CoreGraphics/CGPDFCFFSubset.h>
#include <CoreGraphics/CGPDFSubset.h>
#include <CoreGraphics/CGPattern_internal.h>
#include <CoreGraphics/CGGradient_internal.h>
#include <CoreGraphics/CGShading_internal.h>
#include <CoreGraphics/CGFunction_internal.h>

#include <zlib.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A PDF stitching function is a handful of segments; this is far past any real ramp the CG object holds. */
#define CG_PDF_GRADIENT_MAX 256

/* ------------------------------------------------------------------------- */
/* byte buffers                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
	unsigned char *data;
	size_t len;
	size_t cap;
} cg_buf;

static void fn_buf_free(cg_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static bool fn_buf_reserve(cg_buf *b, size_t extra)
{
	size_t need = b->len + extra + 1;

	if (need <= b->cap) {
		return true;
	}
	{
		size_t cap = b->cap ? b->cap : 256;
		unsigned char *grown;

		while (cap < need) {
			cap *= 2;
		}
		grown = realloc(b->data, cap);
		if (grown == NULL) {
			return false;
		}
		b->data = grown;
		b->cap = cap;
	}
	return true;
}

static void fn_buf_put(cg_buf *b, const void *data, size_t len)
{
	if (len == 0 || !fn_buf_reserve(b, len)) {
		return;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
}

static void fn_buf_printf(cg_buf *b, const char *format, ...)
{
	char small[1024];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(small, sizeof small, format, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if ((size_t)n < sizeof small) {
		fn_buf_put(b, small, (size_t)n);
		return;
	}
	{
		char *big = malloc((size_t)n + 1);

		if (big == NULL) {
			return;
		}
		va_start(ap, format);
		vsnprintf(big, (size_t)n + 1, format, ap);
		va_end(ap);
		fn_buf_put(b, big, (size_t)n);
		free(big);
	}
}

/* ------------------------------------------------------------------------- */
/* the writer                                                                */
/* ------------------------------------------------------------------------- */

struct cg_pdf_resource {
	char name[24];
	unsigned number;
};

struct cg_pdf_pattern_entry {
	CGPatternRef pattern;
	unsigned object;
	char name[24];
};

struct cg_pdf_content_frame {
	cg_buf content;
	unsigned pattern_object;
	CGPatternRef pattern;
};

struct cg_pdf_group_frame {
	cg_buf content;
	unsigned object;
	CGFloat alpha;
	CGBlendMode blend;
	CGRect bbox;
};

struct cg_pdf_gstate {
	int ca;			/* thousandths */
	int CA;
	int blend;		/* CGBlendMode */
	unsigned number;
	char name[24];
};

/* ONE 255-GLYPH SHARD OF A CFF FONT. A PDF Type 1 font selects glyphs by one-byte code through an /Encoding, so a
 * CFF face with more glyphs than that is split across several font resources; each shard owns its own
 * dictionaries and its own code→glyph table. */
struct cg_pdf_shard {
	unsigned object, descriptor, font_file, tounicode;
	char name[24];
	unsigned max_code;
	unsigned codes[256];
	unsigned widths[256];
	char names[256][64];
	bool used[256];
};

/* ONE EMBEDDED FONT: the program, the dictionaries that name it and the glyphs actually used. */
struct cg_pdf_font_resource {
	CGFontRef font;		/* retained */
	unsigned object;	/* the Type0 font */
	unsigned descendant;	/* the CIDFontType2 */
	unsigned descriptor;
	unsigned font_file;
	char name[24];
	char base_name[96];
	int upem;
	int ascent;
	int descent;
	int cap_height;
	size_t glyph_count;
	unsigned *to_unicode;	/* [old glyph] -> Unicode, zero where the cmap has no name */
	unsigned *old_to_cid;	/* [old glyph] -> CID, zero when unused */
	unsigned *cid_to_old;	/* [CID] -> old glyph, CIDs start at 1 */
	unsigned *widths;	/* [CID], 1/1000 em */
	size_t cid_count;
	size_t cid_cap;
	/* CFF (Type 1C) EMBEDDING: the shards, and the glyph→(shard, code) assignment. */
	int kind;			/* 0 = TrueType/CIDFontType2, 1 = CFF/Type1 */
	unsigned *cff_code;		/* [old glyph] -> code, zero when unused */
	unsigned *cff_shard;		/* [old glyph] -> shard index + 1 */
	struct cg_pdf_shard *shards;
	size_t shard_count;
	size_t shard_cap;
};
struct cg_pdf_writer {
	CGDataConsumerRef consumer;
	CGRect default_media;
	CFDictionaryRef auxiliary;
	CFDataRef metadata;

	cg_buf content;
	bool page_open;
	CGRect page_media;
	CGRect page_boxes[5];
	bool page_box_set[5];

	cg_buf *objects;	/* object n is objects[n - 1] */
	size_t object_count;
	size_t object_cap;

	unsigned *pages;
	size_t page_count;
	size_t page_cap;

	struct cg_pdf_resource *images;
	size_t image_count;
	size_t image_cap;
	struct cg_pdf_resource *shadings;
	size_t shading_count;
	size_t shading_cap;
	struct cg_pdf_pattern_entry *patterns;
	size_t pattern_count;
	size_t pattern_cap;
	struct cg_pdf_content_frame *frames;
	size_t frame_count;
	size_t frame_cap;
	struct cg_pdf_group_frame *groups;
	size_t group_count;
	size_t group_cap;
	struct cg_pdf_gstate *gstates;
	size_t gstate_count;
	size_t gstate_cap;
	struct cg_pdf_font_resource *fonts;
	size_t font_count;
	size_t font_cap;
	/* NON-NULL FOR AN EPS WRITER: the same context seam, a different serialiser. */
	struct cg_eps_writer *eps;
	unsigned next_image;
	bool warned_no_embed;
	bool warned_shadow_unsupported;

	bool closed;
	bool failed;
};

static unsigned fn_new_object(struct cg_pdf_writer *w)
{
	if (w->object_count == w->object_cap) {
		size_t cap = w->object_cap ? w->object_cap * 2 : 32;
		cg_buf *grown = realloc(w->objects, cap * sizeof *grown);

		if (grown == NULL) {
			w->failed = true;
			return 0;
		}
		w->objects = grown;
		w->object_cap = cap;
	}
	memset(&w->objects[w->object_count], 0, sizeof w->objects[w->object_count]);
	w->object_count++;
	return (unsigned)w->object_count;
}

static void fn_object_printf(struct cg_pdf_writer *w, unsigned number, const char *format, ...)
{
	char small[2048];
	va_list ap;
	int n;

	if (number == 0 || number > w->object_count) {
		return;
	}
	va_start(ap, format);
	n = vsnprintf(small, sizeof small, format, ap);
	va_end(ap);
	if (n > 0) {
		fn_buf_put(&w->objects[number - 1], small,
			   (size_t)(n < (int)sizeof small ? n : (int)sizeof small - 1));
	}
}

static void fn_object_stream(struct cg_pdf_writer *w, unsigned number, const char *dict, const unsigned char *data,
			     size_t len)
{
	fn_object_printf(w, number, "<< %s /Length %zu >>\nstream\n", dict, len);
	fn_buf_put(&w->objects[number - 1], data, len);
	fn_buf_put(&w->objects[number - 1], "\nendstream", 10);
}

static void fn_content_printf(struct cg_pdf_writer *w, const char *format, ...)
{
	char small[2048];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(small, sizeof small, format, ap);
	va_end(ap);
	if (n > 0) {
		fn_buf_put(&w->content, small, (size_t)(n < (int)sizeof small ? n : (int)sizeof small - 1));
	}
}

/* PDF HAS NO EXPONENT NOTATION, so every number is written as a fixed-point decimal. Four places is a
 * hundredth of a point, which is finer than any page description this tree produces. */
static void fn_number(cg_buf *b, double value)
{
	if (value > -0.00005 && value < 0.00005) {
		value = 0.0;
	}
	fn_buf_printf(b, "%.4f", value);
}

static void fn_hex_string(cg_buf *b, CFStringRef string)
{
	CFIndex length;
	CFIndex i;

	if (string == NULL) {
		fn_buf_put(b, "<>", 2);
		return;
	}
	length = CFStringGetLength(string);
	fn_buf_put(b, "<FEFF", 5);
	for (i = 0; i < length; i++) {
		UniChar c = CFStringGetCharacterAtIndex(string, i);

		fn_buf_printf(b, "%04X", (unsigned)c);
	}
	fn_buf_put(b, ">", 1);
}

/* ------------------------------------------------------------------------- */
/* creation and page life                                                    */
/* ------------------------------------------------------------------------- */

bool cg_pdf_writer_box_from_dict(CFDictionaryRef dict, CFStringRef key, CGRect *out)
{
	CFDataRef data;
	CGRect box;

	if (dict == NULL || key == NULL || out == NULL) {
		return false;
	}
	data = CFDictionaryGetValue(dict, key);
	if (data == NULL || CFGetTypeID(data) != CFDataGetTypeID() ||
	    CFDataGetLength(data) != (CFIndex)sizeof(CGRect)) {
		return false;
	}
	memcpy(&box, CFDataGetBytePtr(data), sizeof box);
	if (box.size.width <= 0 || box.size.height <= 0) {
		return false;
	}
	*out = box;
	return true;
}

struct cg_pdf_writer *cg_pdf_writer_create(CGDataConsumerRef consumer, const CGRect *mediaBox,
					   CFDictionaryRef auxiliaryInfo)
{
	struct cg_pdf_writer *w;
	CGRect box;

	if (consumer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextCreate needs a data consumer\n");
		return NULL;
	}
	w = calloc(1, sizeof *w);
	if (w == NULL) {
		return NULL;
	}
	w->consumer = CGDataConsumerRetain(consumer);
	w->auxiliary = auxiliaryInfo != NULL ? (CFDictionaryRef)CFRetain(auxiliaryInfo) : NULL;
	w->default_media = CGRectMake(0, 0, 612, 792);
	if (!cg_pdf_writer_box_from_dict(auxiliaryInfo, kCGPDFContextMediaBox, &box) && mediaBox != NULL) {
		box = *mediaBox;
	}
	if (box.size.width > 0 && box.size.height > 0) {
		w->default_media = box;
	}
	w->page_media = w->default_media;

	/* THE TWO OBJECTS WHOSE CONTENT IS ONLY KNOWN AT THE END. */
	fn_new_object(w);	/* 1: catalog */
	fn_new_object(w);	/* 2: page tree */
	if (w->failed) {
		cg_pdf_writer_free(w);
		return NULL;
	}
	return w;
}

struct cg_pdf_writer *cg_pdf_writer_create_eps(CGDataConsumerRef consumer, const CGRect *bounds,
					       CFDictionaryRef auxiliaryInfo)
{
	struct cg_pdf_writer *w = cg_pdf_writer_create(consumer, bounds, auxiliaryInfo);

	if (w == NULL) {
		return NULL;
	}
	w->eps = cg_eps_writer_create(consumer, bounds, auxiliaryInfo);
	if (w->eps == NULL) {
		cg_pdf_writer_free(w);
		return NULL;
	}
	return w;
}

size_t cg_pdf_writer_page_count(struct cg_pdf_writer *w)
{
	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_page_count(w->eps);
	}
	return w != NULL ? w->page_count : 0;
}

static void fn_reset_page_resources(struct cg_pdf_writer *w)
{
	w->image_count = 0;
	w->gstate_count = 0;
	w->next_image = 1;
}

bool cg_pdf_writer_begin_page(struct cg_pdf_writer *w, const CGRect *mediaBox)
{
	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_begin_page(w->eps, mediaBox);
	}
	if (w == NULL || w->closed) {
		return false;
	}
	if (w->page_open) {
		cg_pdf_writer_end_page(w);
	}
	w->content.len = 0;
	if (w->content.data != NULL) {
		w->content.data[0] = '\0';
	}
	w->page_media = mediaBox != NULL && mediaBox->size.width > 0 && mediaBox->size.height > 0 ?
			*mediaBox : w->default_media;
	memset(w->page_box_set, 0, sizeof w->page_box_set);
	w->page_boxes[CG_PDF_BOX_MEDIA] = w->page_media;
	w->page_box_set[CG_PDF_BOX_MEDIA] = true;
	fn_reset_page_resources(w);
	w->page_open = true;
	return true;
}

void cg_pdf_writer_set_page_box(struct cg_pdf_writer *w, int which, CGRect box)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_set_page_box(w->eps, which, box);
		return;
	}
	if (w == NULL || which < 0 || which > 4 || box.size.width <= 0 || box.size.height <= 0) {
		return;
	}
	w->page_boxes[which] = box;
	w->page_box_set[which] = true;
}

static void fn_resource_add(struct cg_pdf_resource **list, size_t *count, size_t *cap, const char *name,
			    unsigned number)
{
	if (*count == *cap) {
		size_t grown_cap = *cap ? *cap * 2 : 16;
		struct cg_pdf_resource *grown = realloc(*list, grown_cap * sizeof *grown);

		if (grown == NULL) {
			return;
		}
		*list = grown;
		*cap = grown_cap;
	}
	snprintf((*list)[*count].name, sizeof (*list)[*count].name, "%s", name);
	(*list)[*count].number = number;
	(*count)++;
}

static void fn_build_resource_dict(cg_buf *b, const char *key, const struct cg_pdf_resource *entries, size_t count)
{
	size_t i;

	if (count == 0) {
		return;
	}
	fn_buf_printf(b, "/%s << ", key);
	for (i = 0; i < count; i++) {
		fn_buf_printf(b, "/%s %u 0 R ", entries[i].name, entries[i].number);
	}
	fn_buf_printf(b, ">> ");
}

static void fn_build_gstate_dict(cg_buf *b, const struct cg_pdf_gstate *entries, size_t count)
{
	size_t i;

	if (count == 0) {
		return;
	}
	fn_buf_printf(b, "/ExtGState << ");
	for (i = 0; i < count; i++) {
		fn_buf_printf(b, "/%s %u 0 R ", entries[i].name, entries[i].number);
	}
	fn_buf_printf(b, ">> ");
}

/* THE RESOURCES OF THE CURRENT CONTENT, built in one place because a page and a tiling pattern need the same
 * dictionary. A PATTERN'S /Resources MUST NAME EVERYTHING ITS TILE USES — the objects are document-level, but a
 * form-like content stream does not inherit the page's resource dictionary — so this is called after the tile has
 * been recorded and includes whatever the callback added. */
static void fn_build_resources(struct cg_pdf_writer *w, cg_buf *out)
{
	size_t f;

	fn_buf_printf(out, "/ProcSet [/PDF] ");
	fn_build_resource_dict(out, "XObject", w->images, w->image_count);
	fn_build_resource_dict(out, "Shading", w->shadings, w->shading_count);
	if (w->pattern_count > 0) {
		size_t p;

		fn_buf_printf(out, "/Pattern << ");
		for (p = 0; p < w->pattern_count; p++) {
			fn_buf_printf(out, "/%s %u 0 R ", w->patterns[p].name, w->patterns[p].object);
		}
		fn_buf_printf(out, ">> ");
	}
	fn_build_gstate_dict(out, w->gstates, w->gstate_count);
	if (w->font_count > 0) {
		fn_buf_printf(out, "/Font << ");
		for (f = 0; f < w->font_count; f++) {
			if (w->fonts[f].kind == 1) {
				size_t shard;

				for (shard = 0; shard < w->fonts[f].shard_count; shard++) {
					fn_buf_printf(out, "/%s %u 0 R ", w->fonts[f].shards[shard].name,
						      w->fonts[f].shards[shard].object);
				}
			} else {
				fn_buf_printf(out, "/%s %u 0 R ", w->fonts[f].name, w->fonts[f].object);
			}
		}
		fn_buf_printf(out, ">> ");
	}
}

void cg_pdf_writer_end_page(struct cg_pdf_writer *w)
{
	unsigned content_num;
	unsigned page_num;
	cg_buf resources = { 0 };

	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_end_page(w->eps);
		return;
	}
	if (w == NULL || !w->page_open) {
		return;
	}
	content_num = fn_new_object(w);
	fn_object_stream(w, content_num, "", w->content.data, w->content.len);

	fn_build_resources(w, &resources);

	page_num = fn_new_object(w);
	if (page_num != 0) {
		cg_buf *body = &w->objects[page_num - 1];

		fn_buf_printf(body, "<< /Type /Page /Parent 2 0 R /MediaBox [");
		fn_number(body, w->page_media.origin.x);
		fn_buf_put(body, " ", 1);
		fn_number(body, w->page_media.origin.y);
		fn_buf_put(body, " ", 1);
		fn_number(body, w->page_media.origin.x + w->page_media.size.width);
		fn_buf_put(body, " ", 1);
		fn_number(body, w->page_media.origin.y + w->page_media.size.height);
		fn_buf_put(body, "]", 1);
		{
			static const char *names[5] = { "MediaBox", "CropBox", "BleedBox", "TrimBox", "ArtBox" };
			int which;

			for (which = 1; which < 5; which++) {
				CGRect box;

				if (!w->page_box_set[which]) {
					continue;
				}
				box = w->page_boxes[which];
				fn_buf_printf(body, " /%s [", names[which]);
				fn_number(body, box.origin.x);
				fn_buf_put(body, " ", 1);
				fn_number(body, box.origin.y);
				fn_buf_put(body, " ", 1);
				fn_number(body, box.origin.x + box.size.width);
				fn_buf_put(body, " ", 1);
				fn_number(body, box.origin.y + box.size.height);
				fn_buf_put(body, "]", 1);
			}
		}
		fn_buf_printf(body, " /Resources << ");
		if (resources.len > 0) {
			fn_buf_put(body, resources.data, resources.len);
		}
		fn_buf_printf(body, ">> /Contents %u 0 R >>", content_num);
	}
	fn_buf_free(&resources);

	if (w->page_count == w->page_cap) {
		size_t cap = w->page_cap ? w->page_cap * 2 : 8;
		unsigned *grown = realloc(w->pages, cap * sizeof *grown);

		if (grown == NULL) {
			w->failed = true;
			w->page_open = false;
			return;
		}
		w->pages = grown;
		w->page_cap = cap;
	}
	w->pages[w->page_count++] = page_num;
	w->page_open = false;
}

void cg_pdf_writer_set_metadata(struct cg_pdf_writer *w, CFDataRef metadata)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_set_metadata(w->eps, metadata);
		return;
	}
	if (w == NULL) {
		return;
	}
	if (w->metadata != NULL) {
		CFRelease(w->metadata);
	}
	w->metadata = metadata != NULL ? (CFDataRef)CFRetain(metadata) : NULL;
}

static void fn_warn_shadow_unsupported(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint, const char *what)
{
	if (paint != NULL && paint->shadow_active && !w->warned_shadow_unsupported) {
		fprintf(stderr, "CG-REFUSE: a shadow on %s is not recorded; this writer writes shadows for PATHS and "
				"TEXT only, because those are the paints with a silhouette it can offset\n", what);
		w->warned_shadow_unsupported = true;
	}
}

void cg_pdf_writer_refuse(struct cg_pdf_writer *w, const char *what)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_refuse(what);
		return;
	}
	(void)w;
	fprintf(stderr, "CG-REFUSE: CGPDFContext does not record %s; the page will lack it\n", what);
}

/* ------------------------------------------------------------------------- */
/* recording: state                                                          */
/* ------------------------------------------------------------------------- */

void cg_pdf_writer_save(struct cg_pdf_writer *w)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_save(w->eps);
		return;
	}
	if (w != NULL && w->page_open) {
		fn_content_printf(w, "q\n");
	}
}

void cg_pdf_writer_restore(struct cg_pdf_writer *w)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_restore(w->eps);
		return;
	}
	if (w != NULL && w->page_open) {
		fn_content_printf(w, "Q\n");
	}
}

void cg_pdf_writer_ctm(struct cg_pdf_writer *w, CGAffineTransform t)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_ctm(w->eps, t);
		return;
	}
	if (w == NULL || !w->page_open) {
		return;
	}
	fn_content_printf(w, "%.4f %.4f %.4f %.4f %.4f %.4f cm\n", (double)t.a, (double)t.b, (double)t.c,
			  (double)t.d, (double)t.tx, (double)t.ty);
}

/* ------------------------------------------------------------------------- */
/* recording: paths                                                          */
/* ------------------------------------------------------------------------- */

struct fn_path_ctx {
	cg_buf *out;
	CGPoint current;
	CGPoint start;
	bool started;
};

static void fn_path_move(struct fn_path_ctx *ctx, CGPoint p)
{
	fn_number(ctx->out, p.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, p.y);
	fn_buf_put(ctx->out, " m\n", 3);
	ctx->current = p;
	ctx->start = p;
	ctx->started = true;
}

static void fn_path_line(struct fn_path_ctx *ctx, CGPoint p)
{
	fn_number(ctx->out, p.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, p.y);
	fn_buf_put(ctx->out, " l\n", 3);
	ctx->current = p;
}

static void fn_path_curve(struct fn_path_ctx *ctx, CGPoint c1, CGPoint c2, CGPoint p)
{
	fn_number(ctx->out, c1.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, c1.y);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, c2.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, c2.y);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, p.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_number(ctx->out, p.y);
	fn_buf_put(ctx->out, " c\n", 3);
	ctx->current = p;
}

static void fn_path_apply(void *info, const CGPathElement *element)
{
	struct fn_path_ctx *ctx = info;
	const CGPoint *p = element->points;

	switch (element->type) {
	case kCGPathElementMoveToPoint:
		fn_path_move(ctx, p[0]);
		break;
	case kCGPathElementAddLineToPoint:
		if (!ctx->started) {
			fn_path_move(ctx, CGPointZero);
		}
		fn_path_line(ctx, p[0]);
		break;
	case kCGPathElementAddQuadCurveToPoint: {
		CGPoint c1;
		CGPoint c2;

		if (!ctx->started) {
			fn_path_move(ctx, CGPointZero);
		}
		c1.x = ctx->current.x + 2.0 / 3.0 * (p[0].x - ctx->current.x);
		c1.y = ctx->current.y + 2.0 / 3.0 * (p[0].y - ctx->current.y);
		c2.x = p[1].x + 2.0 / 3.0 * (p[0].x - p[1].x);
		c2.y = p[1].y + 2.0 / 3.0 * (p[0].y - p[1].y);
		fn_path_curve(ctx, c1, c2, p[1]);
		break;
	}
	case kCGPathElementAddCurveToPoint:
		if (!ctx->started) {
			fn_path_move(ctx, CGPointZero);
		}
		fn_path_curve(ctx, p[0], p[1], p[2]);
		break;
	case kCGPathElementCloseSubpath:
		fn_buf_put(ctx->out, "h\n", 2);
		ctx->current = ctx->start;
		break;
	}
}

static bool fn_serialise_path(cg_buf *out, CGPathRef path)
{
	struct fn_path_ctx ctx;

	if (path == NULL) {
		return false;
	}
	ctx.out = out;
	ctx.current = CGPointZero;
	ctx.start = CGPointZero;
	ctx.started = false;
	CGPathApply(path, &ctx, fn_path_apply);
	return ctx.started;
}

/* ------------------------------------------------------------------------- */
/* recording: paint state                                                    */
/* ------------------------------------------------------------------------- */

static const char *fn_blend_name(CGBlendMode mode)
{
	switch (mode) {
	case kCGBlendModeMultiply: return "Multiply";
	case kCGBlendModeScreen: return "Screen";
	case kCGBlendModeOverlay: return "Overlay";
	case kCGBlendModeDarken: return "Darken";
	case kCGBlendModeLighten: return "Lighten";
	case kCGBlendModeColorDodge: return "ColorDodge";
	case kCGBlendModeColorBurn: return "ColorBurn";
	case kCGBlendModeHardLight: return "HardLight";
	case kCGBlendModeSoftLight: return "SoftLight";
	case kCGBlendModeDifference: return "Difference";
	case kCGBlendModeExclusion: return "Exclusion";
	case kCGBlendModeHue: return "Hue";
	case kCGBlendModeSaturation: return "Saturation";
	case kCGBlendModeColor: return "Color";
	case kCGBlendModeLuminosity: return "Luminosity";
	default: return "Normal";
	}
}

static double fn_clamp01(double v)
{
	return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static unsigned fn_gstate_for(struct cg_pdf_writer *w, double ca, double CA, CGBlendMode blend)
{
	int ca_i = (int)(fn_clamp01(ca) * 1000.0 + 0.5);
	int CA_i = (int)(fn_clamp01(CA) * 1000.0 + 0.5);
	int blend_i = (int)blend;
	size_t i;
	unsigned number;
	char name[24];

	for (i = 0; i < w->gstate_count; i++) {
		if (w->gstates[i].ca == ca_i && w->gstates[i].CA == CA_i && w->gstates[i].blend == blend_i) {
			return w->gstates[i].number;
		}
	}
	if (w->gstate_count == w->gstate_cap) {
		size_t cap = w->gstate_cap ? w->gstate_cap * 2 : 8;
		struct cg_pdf_gstate *grown = realloc(w->gstates, cap * sizeof *grown);

		if (grown == NULL) {
			return 0;
		}
		w->gstates = grown;
		w->gstate_cap = cap;
	}
	number = fn_new_object(w);
	if (number == 0) {
		return 0;
	}
	snprintf(name, sizeof name, "GS%u", number);
	fn_object_printf(w, number, "<< /Type /ExtGState /ca %.3f /CA %.3f /BM /%s >>",
			 (double)ca_i / 1000.0, (double)CA_i / 1000.0, fn_blend_name(blend));
	{
		struct cg_pdf_gstate *entry = &w->gstates[w->gstate_count++];

		entry->ca = ca_i;
		entry->CA = CA_i;
		entry->blend = blend_i;
		entry->number = number;
		snprintf(entry->name, sizeof entry->name, "%s", name);
	}
	return number;
}

static struct cg_pdf_pattern_entry *fn_pattern_lookup(struct cg_pdf_writer *w, CGPatternRef pattern);

static void fn_emit_gstate(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint, bool fill, bool stroke)
{
	double ca = fn_clamp01(paint->alpha *
			       (fill ? (paint->fill_pattern != NULL ? paint->fill_pattern_alpha : paint->fill[3])
				     : 1.0));
	double CA = fn_clamp01(paint->alpha *
			       (stroke ? (paint->stroke_pattern != NULL ? paint->stroke_pattern_alpha
								       : paint->stroke[3]) : 1.0));
	unsigned number;

	/* **ALWAYS EMITTED, INCLUDING THE OPAQUE CASE.** A `gs` is part of the PDF graphics state, and a restore
	 * is not the only way it can be left non-default: the 50%-alpha fill that precedes an opaque one would
	 * otherwise stay at /ca 0.5, because this recorder does not mirror `q`/`Q` into its own idea of the
	 * graphics state. Emitting the effective pair with every paint is one line of content per paint and can
	 * never be out of step. */
	number = fn_gstate_for(w, ca, CA, paint->blend);
	if (number != 0) {
		fn_content_printf(w, "/GS%u gs\n", number);
	}
}

static bool fn_emit_fill_colour(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint)
{
	if (paint->fill_pattern != NULL) {
		struct cg_pdf_pattern_entry *entry = fn_pattern_lookup(w, paint->fill_pattern);

		if (entry == NULL) {
			cg_pdf_writer_refuse(w, "a fill with a pattern that was not recorded");
			return false;
		}
		fn_content_printf(w, "/Pattern cs /%s scn\n", entry->name);
		return true;
	}
	fn_content_printf(w, "%.4f %.4f %.4f rg\n", (double)fn_clamp01(paint->fill[0]),
			  (double)fn_clamp01(paint->fill[1]), (double)fn_clamp01(paint->fill[2]));
	return true;
}

static bool fn_emit_stroke_colour(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint)
{
	if (paint->stroke_pattern != NULL) {
		struct cg_pdf_pattern_entry *entry = fn_pattern_lookup(w, paint->stroke_pattern);

		if (entry == NULL) {
			cg_pdf_writer_refuse(w, "a stroke with a pattern that was not recorded");
			return false;
		}
		fn_content_printf(w, "/Pattern CS /%s SCN\n", entry->name);
	} else {
		fn_content_printf(w, "%.4f %.4f %.4f RG\n", (double)fn_clamp01(paint->stroke[0]),
				  (double)fn_clamp01(paint->stroke[1]), (double)fn_clamp01(paint->stroke[2]));
	}
	fn_content_printf(w, "%.4f w\n%d J\n%d j\n%.4f M\n", (double)paint->line_width, paint->cap, paint->join,
			  (double)paint->miter_limit);
	if (paint->dash_count > 0) {
		size_t i;

		fn_content_printf(w, "[");
		for (i = 0; i < paint->dash_count; i++) {
			fn_content_printf(w, "%.4f ", (double)paint->dash[i]);
		}
		fn_content_printf(w, "] %.4f d\n", (double)paint->dash_phase);
	} else {
		fn_content_printf(w, "[] 0 d\n");
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* recording: painting                                                       */
/* ------------------------------------------------------------------------- */

static const char *fn_paint_operator(int operation)
{
	static const char *operators[] = { "f", "f*", "S", "B", "B*" };

	return operation >= 0 && operation <= 4 ? operators[operation] : "";
}

/* **A HARD SHADOW: THE SAME PATH, OFFSET, IN THE SHADOW COLOUR, PAINTED FIRST.** PDF has no shadow primitive and no
 * blur; with a zero blur the shadow of a filled or stroked path is exactly that silhouette, so it can be written
 * rather than refused. Images and shadings have no path to silhouette and are named at their doors below. */
static void fn_emit_shadow_path(struct cg_pdf_writer *w, CGPathRef path, int operation,
				const cg_pdf_paint_state *paint)
{
	bool fill = operation == CG_PDF_PAINT_FILL || operation == CG_PDF_PAINT_EOFILL ||
		    operation == CG_PDF_PAINT_FILLSTROKE || operation == CG_PDF_PAINT_EOFILLSTROKE;
	bool stroke = operation == CG_PDF_PAINT_STROKE || operation == CG_PDF_PAINT_FILLSTROKE ||
		      operation == CG_PDF_PAINT_EOFILLSTROKE;
	cg_pdf_paint_state shadow = *paint;

	shadow.shadow_active = false;
	shadow.fill_pattern = NULL;
	shadow.stroke_pattern = NULL;
	shadow.fill[0] = shadow.stroke[0] = paint->shadow_rgba[0];
	shadow.fill[1] = shadow.stroke[1] = paint->shadow_rgba[1];
	shadow.fill[2] = shadow.stroke[2] = paint->shadow_rgba[2];
	shadow.fill[3] = shadow.stroke[3] = paint->shadow_rgba[3];

	fn_content_printf(w, "q\n1 0 0 1 %.4f %.4f cm\n", (double)paint->shadow_offset[0],
			  (double)paint->shadow_offset[1]);
	fn_emit_gstate(w, &shadow, fill, stroke);
	if (fill) {
		(void)fn_emit_fill_colour(w, &shadow);
	}
	if (stroke) {
		(void)fn_emit_stroke_colour(w, &shadow);
	}
	if (fn_serialise_path(&w->content, path)) {
		fn_content_printf(w, "%s\n", fn_paint_operator(operation));
	}
	fn_content_printf(w, "Q\n");
}

void cg_pdf_writer_paint_path(struct cg_pdf_writer *w, CGPathRef path, int operation,
			      const cg_pdf_paint_state *paint)
{
	static const char *operators[] = { "f", "f*", "S", "B", "B*" };
	bool fill = operation == CG_PDF_PAINT_FILL || operation == CG_PDF_PAINT_EOFILL ||
		    operation == CG_PDF_PAINT_FILLSTROKE || operation == CG_PDF_PAINT_EOFILLSTROKE;
	bool stroke = operation == CG_PDF_PAINT_STROKE || operation == CG_PDF_PAINT_FILLSTROKE ||
		      operation == CG_PDF_PAINT_EOFILLSTROKE;

	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_paint_path(w->eps, path, operation, paint);
		return;
	}
	if (w == NULL || paint == NULL || operation < 0 || operation > 4) {
		return;
	}
	if (!w->page_open) {
		fprintf(stderr, "CG-REFUSE: a drawing call on a CGPDFContext with no page open was not "
				"recorded (call CGPDFContextBeginPage)\n");
		return;
	}
	if (paint->shadow_active) {
		fn_emit_shadow_path(w, path, operation, paint);
	}
	fn_emit_gstate(w, paint, fill, stroke);
	if (fill && !fn_emit_fill_colour(w, paint)) {
		return;
	}
	if (stroke && !fn_emit_stroke_colour(w, paint)) {
		return;
	}
	if (!fn_serialise_path(&w->content, path)) {
		return;
	}
	fn_content_printf(w, "%s\n", operators[operation]);
}

void cg_pdf_writer_clip_path(struct cg_pdf_writer *w, CGPathRef path, bool even_odd)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_clip_path(w->eps, path, even_odd);
		return;
	}
	if (w == NULL || !w->page_open) {
		return;
	}
	if (!fn_serialise_path(&w->content, path)) {
		return;
	}
	fn_content_printf(w, "%s n\n", even_odd ? "W*" : "W");
}

void cg_pdf_writer_clip_rect(struct cg_pdf_writer *w, CGRect rect)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_clip_rect(w->eps, rect);
		return;
	}
	if (w == NULL || !w->page_open) {
		return;
	}
	fn_content_printf(w, "%.4f %.4f %.4f %.4f re W n\n", (double)rect.origin.x, (double)rect.origin.y,
			  (double)rect.size.width, (double)rect.size.height);
}

void cg_pdf_writer_clear_rect(struct cg_pdf_writer *w, CGRect rect)
{
	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_clear_rect(w->eps, rect);
		return;
	}
	if (w == NULL || !w->page_open) {
		return;
	}
	/* A PDF PAGE IS WHITE PAPER: "clear to transparent" is not a thing it can mean, so the closest honest
	 * reading is white. */
	fn_content_printf(w, "1 1 1 rg %.4f %.4f %.4f %.4f re f\n", (double)rect.origin.x, (double)rect.origin.y,
			  (double)rect.size.width, (double)rect.size.height);
}

static bool fn_compress(const unsigned char *data, size_t len, unsigned char **out, size_t *out_len)
{
	uLongf bound = compressBound((uLong)len);
	unsigned char *dest = malloc(bound ? bound : 1);

	if (dest == NULL) {
		return false;
	}
	if (compress2(dest, &bound, data, (uLong)len, Z_DEFAULT_COMPRESSION) != Z_OK) {
		free(dest);
		return false;
	}
	*out = dest;
	*out_len = (size_t)bound;
	return true;
}

static void fn_write_image_object(struct cg_pdf_writer *w, unsigned number, const char *colour_space, size_t width,
				  size_t height, unsigned smask, const unsigned char *data, size_t len)
{
	cg_buf *body = &w->objects[number - 1];

	fn_buf_printf(body, "<< /Type /XObject /Subtype /Image /Width %zu /Height %zu /ColorSpace /%s "
			    "/BitsPerComponent 8 /Filter /FlateDecode", width, height, colour_space);
	if (smask != 0) {
		fn_buf_printf(body, " /SMask %u 0 R", smask);
	}
	fn_buf_printf(body, " /Length %zu >>\nstream\n", len);
	fn_buf_put(body, data, len);
	fn_buf_put(body, "\nendstream", 10);
}

void cg_pdf_writer_image(struct cg_pdf_writer *w, CGRect rect, CGImageRef image, const cg_pdf_paint_state *paint)
{
	size_t width;
	size_t height;
	size_t bpc;
	size_t bpr;
	int stored = 0;
	int map[4];
	int straight = 0;
	size_t components;
	CGColorSpaceRef space;
	CGBitmapInfo info;
	CGImageAlphaInfo alpha_info;
	bool has_alpha;
	CGDataProviderRef provider;
	CFDataRef data;
	const unsigned char *src;
	unsigned char *rgb;
	unsigned char *alpha = NULL;
	bool varying_alpha = false;
	size_t i;
	unsigned base_num;
	unsigned mask_num = 0;
	char name[24];
	unsigned char *compressed = NULL;
	size_t compressed_len = 0;
	unsigned char *alpha_compressed = NULL;
	size_t alpha_compressed_len = 0;

	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_image(w->eps, rect, image, paint);
		return;
	}
	if (w == NULL || image == NULL || paint == NULL) {
		return;
	}
	fn_warn_shadow_unsupported(w, paint, "an image");
	if (!w->page_open) {
		fprintf(stderr, "CG-REFUSE: an image on a CGPDFContext with no page open was not recorded\n");
		return;
	}
	width = CGImageGetWidth(image);
	height = CGImageGetHeight(image);
	bpc = CGImageGetBitsPerComponent(image);
	bpr = CGImageGetBytesPerRow(image);
	space = CGImageGetColorSpace(image);
	info = CGImageGetBitmapInfo(image);
	alpha_info = (CGImageAlphaInfo)(info & kCGBitmapAlphaInfoMask);
	/* `map[3]` NAMES THE SKIPPED COMPONENT TOO ON A `NoneSkip*` CHART — the layout door is right and the
	 * reader has to ask the alpha KIND. Treating a skip byte as alpha made every opaque RGBA image an
	 * invisible one, its /SMask all zeros. */
	has_alpha = alpha_info == kCGImageAlphaPremultipliedLast || alpha_info == kCGImageAlphaLast ||
		    alpha_info == kCGImageAlphaPremultipliedFirst || alpha_info == kCGImageAlphaFirst;
	if (width == 0 || height == 0 || space == NULL) {
		return;
	}
	components = CGColorSpaceGetNumberOfComponents(space);
	if (bpc != 8 || (components != 1 && components != 3)) {
		fprintf(stderr, "CG-REFUSE: an image with %zu components at %zu bits is not embedded by the PDF "
				"context (8-bit grey and RGB are)\n", components, bpc);
		return;
	}
	cg_image_layout(space, (CGImageAlphaInfo)(info & kCGBitmapAlphaInfoMask),
			(uint32_t)(info & kCGBitmapByteOrderMask), map, &stored, &straight);
	if (stored <= 0) {
		fprintf(stderr, "CG-REFUSE: an image whose channel layout this library cannot read was not "
				"embedded\n");
		return;
	}
	provider = CGImageGetDataProvider(image);
	if (provider == NULL) {
		return;
	}
	data = CGDataProviderCopyData(provider);
	if (data == NULL) {
		return;
	}
	src = CFDataGetBytePtr(data);
	if ((size_t)CFDataGetLength(data) < bpr * height) {
		CFRelease(data);
		return;
	}
	rgb = calloc(width * height, 3);
	if (rgb == NULL) {
		CFRelease(data);
		return;
	}
	if (has_alpha && map[3] >= 0) {
		alpha = calloc(width * height, 1);
		if (alpha == NULL) {
			free(rgb);
			CFRelease(data);
			return;
		}
	}
	for (i = 0; i < width * height; i++) {
		const unsigned char *pixel = src + (i / width) * bpr + (i % width) * (size_t)stored;
		unsigned char a = (has_alpha && map[3] >= 0) ? pixel[map[3]] : 255;
		unsigned char r = components == 1 ? pixel[map[0]] : pixel[map[2]];
		unsigned char g = components == 1 ? pixel[map[0]] : pixel[map[1]];
		unsigned char b = components == 1 ? pixel[map[0]] : pixel[map[0]];

		if (has_alpha && map[3] >= 0 && !straight && a > 0 && a < 255) {
			/* PDF SAMPLES ARE STRAIGHT; a premultiplied source is divided back out. */
			r = (unsigned char)((r * 255 + a / 2) / a);
			g = (unsigned char)((g * 255 + a / 2) / a);
			b = (unsigned char)((b * 255 + a / 2) / a);
		}
		rgb[i * 3] = r;
		rgb[i * 3 + 1] = g;
		rgb[i * 3 + 2] = b;
		if (alpha != NULL) {
			alpha[i] = a;
			if (a != 255) {
				varying_alpha = true;
			}
		}
	}
	CFRelease(data);
	if (!varying_alpha && alpha != NULL) {
		free(alpha);
		alpha = NULL;
	}
	if (!fn_compress(rgb, width * height * 3, &compressed, &compressed_len)) {
		free(rgb);
		free(alpha);
		return;
	}
	free(rgb);
	if (alpha != NULL) {
		if (!fn_compress(alpha, width * height, &alpha_compressed, &alpha_compressed_len)) {
			free(alpha);
			free(compressed);
			return;
		}
		free(alpha);
	}

	if (alpha_compressed != NULL) {
		mask_num = fn_new_object(w);
		if (mask_num != 0) {
			fn_write_image_object(w, mask_num, "DeviceGray", width, height, 0, alpha_compressed,
					      alpha_compressed_len);
		}
	}
	base_num = fn_new_object(w);
	if (base_num == 0) {
		free(compressed);
		free(alpha_compressed);
		return;
	}
	fn_write_image_object(w, base_num, components == 1 ? "DeviceGray" : "DeviceRGB", width, height, mask_num,
			      compressed, compressed_len);
	free(compressed);
	free(alpha_compressed);

	snprintf(name, sizeof name, "Im%u", w->next_image++);
	fn_resource_add(&w->images, &w->image_count, &w->image_cap, name, base_num);

	fn_content_printf(w, "q\n");
	fn_emit_gstate(w, paint, true, false);
	fn_content_printf(w, "%.4f 0 0 %.4f %.4f %.4f cm\n/%s Do\nQ\n", (double)rect.size.width,
			  (double)rect.size.height, (double)rect.origin.x, (double)rect.origin.y, name);
}

/* ------------------------------------------------------------------------- */
/* tiling patterns                                                           */
/* ------------------------------------------------------------------------- */

static struct cg_pdf_pattern_entry *fn_pattern_lookup(struct cg_pdf_writer *w, CGPatternRef pattern)
{
	size_t i;

	for (i = 0; i < w->pattern_count; i++) {
		if (w->patterns[i].pattern == pattern) {
			return &w->patterns[i];
		}
	}
	return NULL;
}

static void fn_emit_pattern(struct cg_pdf_writer *w, unsigned object, CGPatternRef pattern, cg_buf *tile)
{
	void *info = NULL;
	CGRect bounds = CGRectZero;
	CGAffineTransform matrix = CGAffineTransformIdentity;
	CGFloat x_step = 0;
	CGFloat y_step = 0;
	CGPatternCallbacks callbacks;
	cg_buf resources = { 0 };

	cg_pattern_state(pattern, &info, &bounds, &matrix, &x_step, &y_step, &callbacks);
	fn_build_resources(w, &resources);
	fn_object_printf(w, object,
			 "<< /Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 "
			 "/BBox [%.4f %.4f %.4f %.4f] /XStep %.4f /YStep %.4f "
			 "/Matrix [%.4f %.4f %.4f %.4f %.4f %.4f] /Resources << %s >> /Length %zu >>\nstream\n",
			 (double)bounds.origin.x, (double)bounds.origin.y,
			 (double)(bounds.origin.x + bounds.size.width),
			 (double)(bounds.origin.y + bounds.size.height), (double)x_step, (double)y_step,
			 (double)matrix.a, (double)matrix.b, (double)matrix.c, (double)matrix.d,
			 (double)matrix.tx, (double)matrix.ty,
			 resources.data != NULL ? (const char *)resources.data : "", tile->len);
	fn_buf_put(&w->objects[object - 1], tile->data, tile->len);
	fn_buf_put(&w->objects[object - 1], "\nendstream", 10);
	fn_buf_free(&resources);
	(void)info;
	(void)callbacks;
}

bool cg_pdf_writer_begin_pattern(struct cg_pdf_writer *w, CGPatternRef pattern)
{
	struct cg_pdf_content_frame *frame;
	unsigned object;

	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_begin_pattern(w->eps, pattern);
	}
	if (w == NULL || pattern == NULL || !w->page_open) {
		return false;
	}
	if (fn_pattern_lookup(w, pattern) != NULL) {
		return false;
	}
	if (w->frame_count >= 16) {
		cg_pdf_writer_refuse(w, "a pattern nested more than 16 deep");
		return false;
	}
	if (w->frame_count == w->frame_cap) {
		size_t cap = w->frame_cap ? w->frame_cap * 2 : 4;
		struct cg_pdf_content_frame *grown = realloc(w->frames, cap * sizeof *grown);

		if (grown == NULL) {
			return false;
		}
		w->frames = grown;
		w->frame_cap = cap;
	}
	object = fn_new_object(w);
	if (object == 0) {
		return false;
	}
	frame = &w->frames[w->frame_count++];
	frame->content = w->content;
	memset(&w->content, 0, sizeof w->content);
	frame->pattern = pattern;
	frame->pattern_object = object;
	return true;
}

void cg_pdf_writer_end_pattern(struct cg_pdf_writer *w, CGPatternRef pattern)
{
	struct cg_pdf_content_frame *frame;
	cg_buf tile;
	unsigned object;

	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_end_pattern(w->eps, pattern);
		return;
	}
	if (w == NULL || w->frame_count == 0 || pattern == NULL) {
		return;
	}
	frame = &w->frames[w->frame_count - 1];
	if (frame->pattern != pattern) {
		fprintf(stderr, "CG-REFUSE: a pattern's content frame does not match the pattern ending it\n");
		return;
	}
	tile = w->content;
	w->content = frame->content;
	object = frame->pattern_object;
	w->frame_count--;

	fn_emit_pattern(w, object, pattern, &tile);
	fn_buf_free(&tile);

	if (w->pattern_count == w->pattern_cap) {
		size_t cap = w->pattern_cap ? w->pattern_cap * 2 : 8;
		struct cg_pdf_pattern_entry *grown = realloc(w->patterns, cap * sizeof *grown);

		if (grown == NULL) {
			return;
		}
		w->patterns = grown;
		w->pattern_cap = cap;
	}
	{
		struct cg_pdf_pattern_entry *entry = &w->patterns[w->pattern_count++];

		entry->pattern = CGPatternRetain(pattern);
		entry->object = object;
		snprintf(entry->name, sizeof entry->name, "P%u", object);
	}
}

/* ------------------------------------------------------------------------- */
/* transparency layers, as isolated transparency groups                      */
/* ------------------------------------------------------------------------- */

bool cg_pdf_writer_begin_group(struct cg_pdf_writer *w, CGRect bbox, CGFloat alpha, CGBlendMode blend)
{
	struct cg_pdf_group_frame *frame;
	unsigned object;

	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_begin_group(w->eps, bbox, alpha, blend);
	}
	if (w == NULL || !w->page_open) {
		return false;
	}
	if (w->group_count >= 8) {
		cg_pdf_writer_refuse(w, "a transparency layer nested more than 8 deep");
		return false;
	}
	if (w->group_count == w->group_cap) {
		size_t cap = w->group_cap ? w->group_cap * 2 : 4;
		struct cg_pdf_group_frame *grown = realloc(w->groups, cap * sizeof *grown);

		if (grown == NULL) {
			return false;
		}
		w->groups = grown;
		w->group_cap = cap;
	}
	object = fn_new_object(w);
	if (object == 0) {
		return false;
	}
	frame = &w->groups[w->group_count++];
	frame->content = w->content;
	memset(&w->content, 0, sizeof w->content);
	frame->object = object;
	frame->alpha = alpha;
	frame->blend = blend;
	frame->bbox = bbox;
	return true;
}

bool cg_pdf_writer_in_group(struct cg_pdf_writer *w)
{
	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_in_group(w->eps);
	}
	return w != NULL && w->group_count > 0;
}

void cg_pdf_writer_end_group(struct cg_pdf_writer *w)
{
	struct cg_pdf_group_frame *frame;
	cg_buf tile;
	cg_buf resources = { 0 };
	CGRect bbox;
	char name[24];

	if (w != NULL && w->eps != NULL) {
		cg_eps_writer_end_group(w->eps);
		return;
	}
	if (w == NULL || w->group_count == 0) {
		return;
	}
	frame = &w->groups[w->group_count - 1];
	tile = w->content;
	w->content = frame->content;
	w->group_count--;

	bbox = frame->bbox;
	if (bbox.size.width <= 0 || bbox.size.height <= 0) {
		bbox = w->page_media;
	}
	/* THE GROUP'S RESOURCES ARE BUILT BEFORE THE GROUP IS ADDED TO THE PAGE'S XOBJECTS, so a group does not list
	 * itself. Everything its content used is already in the page's lists. */
	fn_build_resources(w, &resources);
	fn_object_printf(w, frame->object,
			 "<< /Type /XObject /Subtype /Form /BBox [%.4f %.4f %.4f %.4f] "
			 "/Group << /S /Transparency /CS /DeviceRGB /I true >> /Resources << %s >> /Length %zu >>\nstream\n",
			 (double)bbox.origin.x, (double)bbox.origin.y,
			 (double)(bbox.origin.x + bbox.size.width),
			 (double)(bbox.origin.y + bbox.size.height),
			 resources.data != NULL ? (const char *)resources.data : "", tile.len);
	fn_buf_put(&w->objects[frame->object - 1], tile.data, tile.len);
	fn_buf_put(&w->objects[frame->object - 1], "\nendstream", 10);
	fn_buf_free(&resources);
	fn_buf_free(&tile);

	snprintf(name, sizeof name, "Fm%u", frame->object);
	fn_resource_add(&w->images, &w->image_count, &w->image_cap, name, frame->object);

	{
		cg_pdf_paint_state paint;

		memset(&paint, 0, sizeof paint);
		paint.alpha = frame->alpha;
		paint.blend = frame->blend;
		paint.fill[3] = 1.0;
		paint.stroke[3] = 1.0;
		fn_content_printf(w, "q\n");
		fn_emit_gstate(w, &paint, true, false);
		fn_content_printf(w, "/%s Do\nQ\n", name);
	}
}

/* ------------------------------------------------------------------------- */
/* gradients, as PDF shadings                                                 */
/* ------------------------------------------------------------------------- */

static void fn_gradient_segment(struct cg_pdf_writer *w, unsigned *function_objects, size_t *segment_count,
				const CGFloat c0[3], const CGFloat c1[3], cg_buf *bounds, cg_buf *encode)
{
	unsigned object = fn_new_object(w);

	if (object == 0) {
		return;
	}
	fn_object_printf(w, object,
			 "<< /FunctionType 2 /Domain [0 1] /C0 [%.4f %.4f %.4f] /C1 [%.4f %.4f %.4f] /N 1 >>",
			 (double)c0[0], (double)c0[1], (double)c0[2], (double)c1[0], (double)c1[1],
			 (double)c1[2]);
	if (function_objects != NULL) {
		function_objects[*segment_count] = object;
	}
	(*segment_count)++;
	fn_buf_put(encode, "0 1 ", 4);
}

bool cg_pdf_writer_gradient(struct cg_pdf_writer *w, CGGradientRef gradient, bool radial, CGPoint start0,
			    CGFloat radius0, CGPoint start1, CGFloat radius1,
			    CGGradientDrawingOptions options, const cg_pdf_paint_state *paint)
{
	size_t count;
	size_t i;
	CGFloat locations[CG_PDF_GRADIENT_MAX];
	CGFloat colours[CG_PDF_GRADIENT_MAX][3];
	size_t kept = 0;
	unsigned functions[CG_PDF_GRADIENT_MAX + 2];
	size_t segments = 0;
	cg_buf bounds = { 0 };
	cg_buf encode = { 0 };
	cg_buf function_list = { 0 };
	unsigned stitch;
	unsigned shading;
	char name[24];
	cg_pdf_paint_state gs_paint;

	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_gradient(w->eps, gradient, radial, start0, radius0, start1, radius1,
					      options, paint);
	}
	if (w == NULL || gradient == NULL || paint == NULL) {
		return false;
	}
	fn_warn_shadow_unsupported(w, paint, "a gradient");
	if (!w->page_open) {
		fprintf(stderr, "CG-REFUSE: a gradient on a CGPDFContext with no page open was not recorded\n");
		return false;
	}
	count = cg_gradient_stop_count(gradient);
	if (count < 2 || count > CG_PDF_GRADIENT_MAX) {
		cg_pdf_writer_refuse(w, "a gradient with an unusable stop count");
		return false;
	}
	/* PER-STOP ALPHA NEEDS A SOFT MASK, and a DeviceRGB function has no channel for it. The whole gradient is
	 * refused rather than drawn at the wrong opacity. */
	for (i = 0; i < count; i++) {
		CGFloat rgba[4];

		cg_gradient_stop(gradient, i, NULL, rgba);
		if (rgba[3] < 0.999) {
			cg_pdf_writer_refuse(w, "a gradient with per-stop alpha (a soft mask)");
			return false;
		}
	}
	/* THE STOPS, WITH DUPLICATE LOCATIONS COLLAPSED TO THE LATER COLOUR — a zero-width interval is a step, not
	 * a ramp, and a PDF stitching function's /Bounds must increase. */
	for (i = 0; i < count; i++) {
		CGFloat location;
		CGFloat rgba[4];

		cg_gradient_stop(gradient, i, &location, rgba);
		if (kept > 0 && location <= locations[kept - 1]) {
			colours[kept - 1][0] = rgba[0];
			colours[kept - 1][1] = rgba[1];
			colours[kept - 1][2] = rgba[2];
			continue;
		}
		locations[kept] = location;
		colours[kept][0] = rgba[0];
		colours[kept][1] = rgba[1];
		colours[kept][2] = rgba[2];
		kept++;
	}
	if (kept == 0) {
		return false;
	}
	/* BEFORE THE FIRST STOP AND AFTER THE LAST ARE CONSTANT COLOUR, which is what the CG ramp does; the
	 * shading's own /Extend covers the axes outside the two points. */
	if (kept == 1 || locations[0] > 0.0) {
		fn_gradient_segment(w, functions, &segments, colours[0], colours[0], &bounds, &encode);
	}
	for (i = 0; i + 1 < kept; i++) {
		if (i > 0 || locations[0] > 0.0) {
			fn_buf_printf(&bounds, "%.4f ", (double)locations[i]);
		}
		fn_gradient_segment(w, functions, &segments, colours[i], colours[i + 1], &bounds, &encode);
	}
	if (kept > 1 && locations[kept - 1] < 1.0) {
		fn_buf_printf(&bounds, "%.4f ", (double)locations[kept - 1]);
		fn_gradient_segment(w, functions, &segments, colours[kept - 1], colours[kept - 1], &bounds, &encode);
	}
	if (segments == 0) {
		fn_buf_free(&bounds);
		fn_buf_free(&encode);
		return false;
	}
	fn_buf_printf(&function_list, "[");
	for (i = 0; i < segments; i++) {
		fn_buf_printf(&function_list, "%u 0 R ", functions[i]);
	}
	fn_buf_printf(&function_list, "]");
	stitch = fn_new_object(w);
	if (stitch == 0) {
		fn_buf_free(&bounds);
		fn_buf_free(&encode);
		fn_buf_free(&function_list);
		return false;
	}
	fn_object_printf(w, stitch,
			 "<< /FunctionType 3 /Domain [0 1] /Functions %s /Bounds [%s] /Encode [%s] >>",
			 function_list.data != NULL ? (const char *)function_list.data : "[]",
			 bounds.data != NULL ? (const char *)bounds.data : "",
			 encode.data != NULL ? (const char *)encode.data : "");
	fn_buf_free(&bounds);
	fn_buf_free(&encode);
	fn_buf_free(&function_list);

	shading = fn_new_object(w);
	if (shading == 0) {
		return false;
	}
	{
		const char *extend_before = (options & kCGGradientDrawsBeforeStartLocation) ? "true" : "false";
		const char *extend_after = (options & kCGGradientDrawsAfterEndLocation) ? "true" : "false";

		if (radial) {
			fn_object_printf(w, shading,
					 "<< /ShadingType 3 /ColorSpace /DeviceRGB /Coords [%.4f %.4f %.4f "
					 "%.4f %.4f %.4f] /Function %u 0 R /Extend [%s %s] >>",
					 (double)start0.x, (double)start0.y, (double)radius0, (double)start1.x,
					 (double)start1.y, (double)radius1, stitch, extend_before, extend_after);
		} else {
			fn_object_printf(w, shading,
					 "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [%.4f %.4f %.4f "
					 "%.4f] /Function %u 0 R /Extend [%s %s] >>",
					 (double)start0.x, (double)start0.y, (double)start1.x, (double)start1.y,
					 stitch, extend_before, extend_after);
		}
	}
	snprintf(name, sizeof name, "Sh%u", shading);
	fn_resource_add(&w->shadings, &w->shading_count, &w->shading_cap, name, shading);

	/* THE GRADIENT'S OWN COLOURS ARE OPAQUE, so only the CONTEXT's alpha and blend apply. */
	gs_paint = *paint;
	gs_paint.fill[3] = 1.0;
	gs_paint.stroke[3] = 1.0;
	fn_content_printf(w, "q\n");
	fn_emit_gstate(w, &gs_paint, true, false);
	fn_content_printf(w, "/%s sh\nQ\n", name);
	return true;
}

/* ONE SHADING, SAMPLED. A CGFunction is a CALLBACK, not a PDF function expression, so the only portable way to
 * put it in a file is to evaluate it on a grid and write the samples down; 256 one-dimensional samples is what
 * the gradient path already considers a ramp, and the sampled function interpolates between them. */
bool cg_pdf_writer_shading(struct cg_pdf_writer *w, CGShadingRef shading, const cg_pdf_paint_state *paint)
{
	CGColorSpaceRef space;
	CGFunctionRef function;
	size_t components;
	unsigned char *samples;
	size_t sample_count = 256;
	unsigned char *compressed = NULL;
	size_t compressed_len = 0;
	unsigned function_object;
	unsigned shading_object;
	CGPoint start;
	CGPoint end;
	CGFloat start_radius = 0;
	CGFloat end_radius = 0;
	bool extend_start = false;
	bool extend_end = false;
	bool radial;
	char name[24];
	cg_pdf_paint_state gs_paint;
	cg_buf dict = { 0 };
	size_t i;

	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_shading(w->eps, shading, paint);
	}
	size_t c;

	if (w == NULL || shading == NULL || paint == NULL) {
		return false;
	}
	fn_warn_shadow_unsupported(w, paint, "a shading");
	if (!w->page_open) {
		fprintf(stderr, "CG-REFUSE: a shading on a CGPDFContext with no page open was not recorded\n");
		return false;
	}
	space = cg_shading_color_space(shading);
	function = cg_shading_function(shading);
	components = space != NULL ? CGColorSpaceGetNumberOfComponents(space) : 0;
	if (function == NULL || (components != 1 && components != 3 && components != 4)) {
		cg_pdf_writer_refuse(w, "a shading outside DeviceGray, DeviceRGB and DeviceCMYK");
		return false;
	}
	samples = malloc(sample_count * components);
	if (samples == NULL) {
		return false;
	}
	for (i = 0; i < sample_count; i++) {
		CGFloat input = (CGFloat)i / (CGFloat)(sample_count - 1);
		CGFloat output[4] = { 0, 0, 0, 0 };

		cg_function_evaluate(function, &input, output);
		for (c = 0; c < components; c++) {
			CGFloat value = output[c];

			if (value < 0) value = 0;
			if (value > 1) value = 1;
			samples[i * components + c] = (unsigned char)(value * 255.0 + 0.5);
		}
	}
	if (!fn_compress(samples, sample_count * components, &compressed, &compressed_len)) {
		free(samples);
		return false;
	}
	free(samples);

	function_object = fn_new_object(w);
	if (function_object == 0) {
		free(compressed);
		return false;
	}
	fn_object_printf(w, function_object,
			 "<< /FunctionType 0 /Domain [0 1] /Range [");
	for (c = 0; c < components; c++) {
		fn_buf_printf(&w->objects[function_object - 1], "0 1 ");
	}
	fn_object_printf(w, function_object,
			 "] /Size [%zu] /BitsPerSample 8 /Encode [0 255] /Filter /FlateDecode /Length %zu >>\nstream\n",
			 sample_count, compressed_len);
	fn_buf_put(&w->objects[function_object - 1], compressed, compressed_len);
	fn_buf_put(&w->objects[function_object - 1], "\nendstream", 10);
	free(compressed);

	cg_shading_geometry(shading, &start, &end, &start_radius, &end_radius);
	cg_shading_extend(shading, &extend_start, &extend_end);
	radial = cg_shading_is_radial(shading);
	shading_object = fn_new_object(w);
	if (shading_object == 0) {
		fn_buf_free(&dict);
		return false;
	}
	fn_buf_printf(&dict, "/ColorSpace /%s", components == 1 ? "DeviceGray" :
						     (components == 3 ? "DeviceRGB" : "DeviceCMYK"));
	if (radial) {
		fn_object_printf(w, shading_object,
				 "<< /ShadingType 3 %s /Coords [%.4f %.4f %.4f %.4f %.4f %.4f] /Function %u 0 R "
				 "/Extend [%s %s] >>",
				 (const char *)dict.data, (double)start.x, (double)start.y, (double)start_radius,
				 (double)end.x, (double)end.y, (double)end_radius, function_object,
				 extend_start ? "true" : "false", extend_end ? "true" : "false");
	} else {
		fn_object_printf(w, shading_object,
				 "<< /ShadingType 2 %s /Coords [%.4f %.4f %.4f %.4f] /Function %u 0 R "
				 "/Extend [%s %s] >>",
				 (const char *)dict.data, (double)start.x, (double)start.y, (double)end.x,
				 (double)end.y, function_object, extend_start ? "true" : "false",
				 extend_end ? "true" : "false");
	}
	fn_buf_free(&dict);
	snprintf(name, sizeof name, "Sh%u", shading_object);
	fn_resource_add(&w->shadings, &w->shading_count, &w->shading_cap, name, shading_object);

	gs_paint = *paint;
	gs_paint.fill[3] = 1.0;
	gs_paint.stroke[3] = 1.0;
	fn_content_printf(w, "q\n");
	fn_emit_gstate(w, &gs_paint, true, false);
	fn_content_printf(w, "/%s sh\nQ\n", name);
	return true;
}

/* ------------------------------------------------------------------------- */
/* text, as text, when the face can be embedded                               */
/* ------------------------------------------------------------------------- */

static void fn_font_name(CGFontRef font, char *out, size_t size, unsigned fallback)
{
	CFStringRef name = CGFontCopyPostScriptName(font);

	if (name == NULL || !CFStringGetCString(name, out, (CFIndex)size, kCFStringEncodingUTF8)) {
		snprintf(out, size, "AgoraFont%u", fallback);
	}
	if (name != NULL) {
		CFRelease(name);
	}
	/* A PDF NAME CANNOT HOLD SPACE OR A DELIMITER WITHOUT `#`-ESCAPING, and no real PostScript name has one. */
	{
		size_t i;

		for (i = 0; out[i] != '\0'; i++) {
			if (out[i] == ' ' || out[i] == '(' || out[i] == ')' || out[i] == '<' || out[i] == '>' ||
			    out[i] == '[' || out[i] == ']' || out[i] == '/' || out[i] == '%' || out[i] == '#') {
				out[i] = '-';
			}
		}
	}
}

static int fn_metric_1000(int value, int upem)
{
	return (int)((double)value * 1000.0 / (double)upem + (value >= 0 ? 0.5 : -0.5));
}

/* THE FONT RESOURCE, CREATED ON FIRST USE. `NULL` means "draw the outline": the face has no program, its
 * outlines are CFF and CID-keyed (not a legal Type 1C), or its program cannot be read. The OBJECTS a TrueType
 * font needs are made here because the page's resource dictionary needs their numbers; a CFF font's objects are
 * per shard and wait for the first glyph. */
static struct cg_pdf_font_resource *fn_font_resource(struct cg_pdf_writer *w, CGFontRef font)
{
	size_t i;
	struct cg_pdf_font_resource *entry;
	char name[96];
	bool truetype;
	bool cff;

	if (font == NULL) {
		return NULL;
	}
	for (i = 0; i < w->font_count; i++) {
		if (w->fonts[i].font == font) {
			return &w->fonts[i];
		}
	}
	truetype = cg_font_has_truetype_outlines(font);
	cff = false;
	if (!truetype && cg_font_is_cff(font) && !cg_font_is_cid_keyed(font)) {
		CFDataRef program = cg_font_copy_cff_program(font);

		if (program != NULL) {
			cff = true;
			CFRelease(program);
		}
	}
	if (!truetype && !cff) {
		if (!w->warned_no_embed) {
			fprintf(stderr, "CG-REFUSE: this face's program is neither TrueType `glyf` nor a non-CID-keyed "
					"CFF; its text is written as glyph OUTLINES\n");
			w->warned_no_embed = true;
		}
		return NULL;
	}
	if (w->font_count == w->font_cap) {
		size_t cap = w->font_cap ? w->font_cap * 2 : 8;
		struct cg_pdf_font_resource *grown = realloc(w->fonts, cap * sizeof *grown);

		if (grown == NULL) {
			return NULL;
		}
		w->fonts = grown;
		w->font_cap = cap;
	}
	entry = &w->fonts[w->font_count];
	memset(entry, 0, sizeof *entry);
	entry->font = (CGFontRef)CGFontRetain(font);
	entry->upem = CGFontGetUnitsPerEm(font);
	if (entry->upem <= 0) {
		CGFontRelease(entry->font);
		return NULL;
	}
	entry->kind = cff ? 1 : 0;
	entry->ascent = fn_metric_1000(CGFontGetAscent(font), entry->upem);
	entry->descent = -abs(fn_metric_1000(CGFontGetDescent(font), entry->upem));
	entry->cap_height = fn_metric_1000(CGFontGetCapHeight(font), entry->upem);
	entry->glyph_count = CGFontGetNumberOfGlyphs(font);
	fn_font_name(font, name, sizeof name, w->font_count + 1);
	snprintf(entry->base_name, sizeof entry->base_name, "%s", name);
	entry->to_unicode = calloc(entry->glyph_count + 1, sizeof *entry->to_unicode);
	if (entry->to_unicode != NULL) {
		cg_font_unicode_map(font, entry->to_unicode, entry->glyph_count + 1);
	}
	if (cff) {
		entry->cff_code = calloc(entry->glyph_count + 1, sizeof *entry->cff_code);
		entry->cff_shard = calloc(entry->glyph_count + 1, sizeof *entry->cff_shard);
		if (entry->cff_code == NULL || entry->cff_shard == NULL) {
			free(entry->cff_code);
			free(entry->cff_shard);
			free(entry->to_unicode);
			CGFontRelease(entry->font);
			return NULL;
		}
	} else {
		entry->old_to_cid = calloc(entry->glyph_count + 1, sizeof *entry->old_to_cid);
		entry->font_file = fn_new_object(w);
		entry->descendant = fn_new_object(w);
		entry->descriptor = fn_new_object(w);
		entry->object = fn_new_object(w);
		if (entry->old_to_cid == NULL || entry->font_file == 0 || entry->descendant == 0 ||
		    entry->descriptor == 0 || entry->object == 0) {
			free(entry->old_to_cid);
			free(entry->to_unicode);
			CGFontRelease(entry->font);
			return NULL;
		}
		snprintf(entry->name, sizeof entry->name, "F%u", entry->object);
	}
	w->font_count++;
	return entry;
}

/* A NEW CFF SHARD WITH ITS FOUR OBJECTS. The bodies are written at Close, when the code table is complete. */
static struct cg_pdf_shard *fn_new_shard(struct cg_pdf_writer *w, struct cg_pdf_font_resource *font)
{
	struct cg_pdf_shard *shard;

	if (font->shard_count == font->shard_cap) {
		size_t cap = font->shard_cap ? font->shard_cap * 2 : 4;
		struct cg_pdf_shard *grown = realloc(font->shards, cap * sizeof *grown);

		if (grown == NULL) {
			return NULL;
		}
		font->shards = grown;
		font->shard_cap = cap;
	}
	shard = &font->shards[font->shard_count];
	memset(shard, 0, sizeof *shard);
	shard->font_file = fn_new_object(w);
	shard->descriptor = fn_new_object(w);
	shard->object = fn_new_object(w);
	shard->tounicode = fn_new_object(w);
	if (shard->font_file == 0 || shard->descriptor == 0 || shard->object == 0 || shard->tounicode == 0) {
		return NULL;
	}
	snprintf(shard->name, sizeof shard->name, "F%u", shard->object);
	font->shard_count++;
	return shard;
}

/* THE CODE FOR A CFF GLYPH, creating a shard when the last is full. Zero means "draw the outline": the glyph has
 * no name in the CFF charset, so a /Differences entry cannot select it. */
static unsigned fn_cff_code(struct cg_pdf_writer *w, struct cg_pdf_font_resource *font, CGGlyph glyph,
			    struct cg_pdf_shard **out_shard)
{
	struct cg_pdf_shard *shard;
	char name[64];
	unsigned code;
	int advance = 0;

	if (font->cff_code == NULL || glyph >= font->glyph_count) {
		return 0;
	}
	if (font->cff_code[glyph] != 0) {
		*out_shard = &font->shards[font->cff_shard[glyph] - 1];
		return font->cff_code[glyph];
	}
	if (cg_font_glyph_name(font->font, glyph, name, sizeof name) <= 0) {
		return 0;
	}
	if (font->shard_count == 0 || font->shards[font->shard_count - 1].max_code >= 255) {
		shard = fn_new_shard(w, font);
		if (shard == NULL) {
			return 0;
		}
	} else {
		shard = &font->shards[font->shard_count - 1];
	}
	code = ++shard->max_code;
	shard->codes[code] = glyph;
	shard->used[code] = true;
	snprintf(shard->names[code], sizeof shard->names[code], "%s", name);
	if (CGFontGetGlyphAdvances(font->font, &glyph, 1, &advance)) {
		shard->widths[code] = (unsigned)fn_metric_1000(advance, font->upem);
	} else {
		shard->widths[code] = 1000;
	}
	font->cff_code[glyph] = code;
	font->cff_shard[glyph] = (unsigned)font->shard_count;
	*out_shard = shard;
	return code;
}

/* THE CID FOR A GLYPH, assigned on first use. The content streams carry these CID numbers rather than glyph
 * indices, which is what lets the SUBSET renumber the program at Close without rewriting a page: the
 * /CIDToGIDMap written then is the only place the new numbering appears. */
static unsigned fn_cid_for(struct cg_pdf_font_resource *font, CGGlyph glyph)
{
	unsigned cid;

	if (font->old_to_cid == NULL || glyph >= font->glyph_count) {
		return 0;
	}
	if (font->old_to_cid[glyph] != 0) {
		return font->old_to_cid[glyph];
	}
	if (font->cid_count + 1 >= font->cid_cap) {
		size_t cap = font->cid_cap ? font->cid_cap * 2 : 32;
		unsigned *old_grown = realloc(font->cid_to_old, cap * sizeof *old_grown);
		unsigned *width_grown;

		if (old_grown == NULL) {
			return 0;
		}
		font->cid_to_old = old_grown;
		width_grown = realloc(font->widths, cap * sizeof *width_grown);
		if (width_grown == NULL) {
			return 0;
		}
		font->widths = width_grown;
		font->cid_cap = cap;
	}
	{
		int advance = 0;
		int width = 1000;

		if (CGFontGetGlyphAdvances(font->font, &glyph, 1, &advance)) {
			width = fn_metric_1000(advance, font->upem);
		}
		cid = (unsigned)++font->cid_count;
		font->cid_to_old[cid] = glyph;
		font->widths[cid] = (unsigned)width;
		font->old_to_cid[glyph] = cid;
	}
	return cid;
}

bool cg_pdf_writer_text_glyph(struct cg_pdf_writer *w, CGFontRef font_ref, CGGlyph glyph, CGPoint pen,
			      CGFloat font_size, CGAffineTransform text_matrix, int mode,
			      const cg_pdf_paint_state *paint)
{
	struct cg_pdf_font_resource *font;
	const char *resource_name;
	int hex_digits;
	double em;
	double a;
	double b;
	double c;
	double d;
	double e;
	double f;
	unsigned cid;

	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_text_glyph(w->eps, font_ref, glyph, pen, font_size, text_matrix, mode, paint);
	}
	if (w == NULL || paint == NULL || !w->page_open || font_ref == NULL || font_size <= 0.0 ||
	    (mode != 0 && mode != 1 && mode != 2)) {
		return false;
	}
	font = fn_font_resource(w, font_ref);
	if (font == NULL) {
		return false;
	}
	if (font->kind == 1) {
		struct cg_pdf_shard *shard = NULL;

		cid = fn_cff_code(w, font, glyph, &shard);
		if (cid == 0 || shard == NULL) {
			return false;
		}
		resource_name = shard->name;
		hex_digits = 2;
	} else {
		cid = fn_cid_for(font, glyph);
		if (cid == 0) {
			return false;
		}
		resource_name = font->name;
		hex_digits = 4;
	}
	em = (double)font_size / (double)font->upem;
	a = text_matrix.a;
	b = text_matrix.b;
	c = text_matrix.c;
	d = text_matrix.d;
	e = pen.x + text_matrix.tx * em;
	f = pen.y + text_matrix.ty * em;

	/* THE SHADOW RUN FIRST, offset and in the shadow colour, with the same text operator as the glyph itself. */
	if (paint->shadow_active) {
		cg_pdf_paint_state shadow = *paint;

		shadow.shadow_active = false;
		shadow.fill_pattern = NULL;
		shadow.stroke_pattern = NULL;
		shadow.fill[0] = shadow.stroke[0] = paint->shadow_rgba[0];
		shadow.fill[1] = shadow.stroke[1] = paint->shadow_rgba[1];
		shadow.fill[2] = shadow.stroke[2] = paint->shadow_rgba[2];
		shadow.fill[3] = shadow.stroke[3] = paint->shadow_rgba[3];
		fn_content_printf(w, "q\n1 0 0 1 %.4f %.4f cm\n", (double)paint->shadow_offset[0],
				  (double)paint->shadow_offset[1]);
		fn_emit_gstate(w, &shadow, mode != 1, mode != 0);
		if (mode != 1) {
			(void)fn_emit_fill_colour(w, &shadow);
		}
		if (mode != 0) {
			(void)fn_emit_stroke_colour(w, &shadow);
		}
		fn_content_printf(w, "BT\n/%s %.4f Tf\n%d Tr\n%.4f %.4f %.4f %.4f %.4f %.4f Tm\n",
				  resource_name, (double)font_size, mode, a, b, c, d, e, f);
		if (hex_digits == 2) {
			fn_content_printf(w, "<%02X> Tj\nET\nQ\n", cid);
		} else {
			fn_content_printf(w, "<%04X> Tj\nET\nQ\n", cid);
		}
	}

	fn_emit_gstate(w, paint, mode != 1, mode != 0);
	if (mode != 1) {
		(void)fn_emit_fill_colour(w, paint);
	}
	if (mode != 0) {
		(void)fn_emit_stroke_colour(w, paint);
	}
	fn_content_printf(w, "BT\n/%s %.4f Tf\n%d Tr\n%.4f %.4f %.4f %.4f %.4f %.4f Tm\n",
			  resource_name, (double)font_size, mode, a, b, c, d, e, f);
	if (hex_digits == 2) {
		fn_content_printf(w, "<%02X> Tj\nET\n", cid);
	} else {
		fn_content_printf(w, "<%04X> Tj\nET\n", cid);
	}
	return true;
}


/* ------------------------------------------------------------------------- */
/* the font programs, at Close                                               */
/* ------------------------------------------------------------------------- */

static void fn_put_be16(unsigned char *p, unsigned value)
{
	p[0] = (unsigned char)(value >> 8);
	p[1] = (unsigned char)value;
}

static void fn_finalise_truetype_font(struct cg_pdf_writer *w, struct cg_pdf_font_resource *font);

/* A /Differences NAME, `#`-escaped for the bytes a PDF name cannot hold literally. */
static void fn_append_pdf_name(cg_buf *out, const char *name)
{
	size_t i;

	fn_buf_put(out, "/", 1);
	for (i = 0; name[i] != '\0'; i++) {
		unsigned char c = (unsigned char)name[i];

		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '.' || c == '_' || c == '-' || c == '+') {
			fn_buf_put(out, &c, 1);
		} else {
			fn_buf_printf(out, "#%02X", (unsigned)c);
		}
	}
}

/* THE CFF FONT, AS A TYPE 1 FONT WITH /Differences ENCODINGS. A PDF Type 1 font selects a glyph by a one-byte
 * code and a glyph NAME, which is exactly what a non-CID CFF charset provides; a face with more than 255 used
 * glyphs is split across shards, each a font resource of its own. The program is embedded whole (a CFF is small,
 * and subsetting one is a separate problem from subsetting `glyf`). */
static void fn_finalise_cff_font(struct cg_pdf_writer *w, struct cg_pdf_font_resource *font)
{
	CFDataRef program = cg_font_copy_cff_program(font->font);
	size_t shard_index;
	unsigned *used = NULL;
	size_t used_count = 0;
	size_t used_cap = 0;
	cg_pdf_cff_subset *subset = NULL;
	const unsigned char *program_bytes;
	size_t program_len;
	const char *base_name = font->base_name;
	char tagged_name[128];

	if (program == NULL) {
		return;
	}
	/* THE USED GLYPHS ACROSS EVERY SHARD. The subsetter keeps the CFF's NAMES and SIDs, so /Differences
	 * continues to select the same glyphs after the CharStrings INDEX is renumbered. */
	for (shard_index = 0; shard_index < font->shard_count; shard_index++) {
		struct cg_pdf_shard *shard = &font->shards[shard_index];
		unsigned code;

		for (code = 1; code <= shard->max_code; code++) {
			if (!shard->used[code]) {
				continue;
			}
			if (used_count == used_cap) {
				size_t cap = used_cap ? used_cap * 2 : 16;
				unsigned *grown = realloc(used, cap * sizeof *grown);

				if (grown == NULL) {
					free(used);
					CFRelease(program);
					return;
				}
				used = grown;
				used_cap = cap;
			}
			used[used_count++] = shard->codes[code];
		}
	}
	subset = cg_pdf_cff_subset_create(CFDataGetBytePtr(program), (size_t)CFDataGetLength(program), used,
					  used_count);
	free(used);
	program_bytes = subset != NULL ? cg_pdf_cff_subset_data(subset, &program_len) :
					 CFDataGetBytePtr(program);
	if (subset == NULL) {
		program_len = (size_t)CFDataGetLength(program);
	} else {
		/* THE SIX-LETTER TAG TELLS A READER THESE GLYPH INDICES ARE THE SUBSET'S OWN NUMBERING. */
		snprintf(tagged_name, sizeof tagged_name, "AGORAC+%s", font->base_name);
		base_name = tagged_name;
	}
	for (shard_index = 0; shard_index < font->shard_count; shard_index++) {
		struct cg_pdf_shard *shard = &font->shards[shard_index];
		cg_buf widths = { 0 };
		cg_buf differences = { 0 };
		cg_buf cmap = { 0 };
		unsigned code;
		size_t written = 0;

		fn_object_stream(w, shard->font_file, "/Subtype /Type1C", program_bytes, program_len);
		fn_object_printf(w, shard->descriptor,
				 "<< /Type /FontDescriptor /FontName /%s /Flags 32 /FontBBox [-200 -300 1200 900] "
				 "/ItalicAngle 0 /Ascent %d /Descent %d /CapHeight %d /StemV 80 "
				 "/FontFile3 %u 0 R >>",
				 base_name, font->ascent, font->descent, font->cap_height, shard->font_file);
		fn_buf_printf(&widths, "[");
		for (code = 1; code <= shard->max_code; code++) {
			fn_buf_printf(&widths, "%u ", shard->used[code] ? shard->widths[code] : 0);
		}
		fn_buf_printf(&widths, "]");
		fn_buf_printf(&differences, "[");
		for (code = 1; code <= shard->max_code; code++) {
			if (!shard->used[code]) {
				continue;
			}
			fn_buf_printf(&differences, "%u ", code);
			fn_append_pdf_name(&differences, shard->names[code]);
			fn_buf_put(&differences, " ", 1);
		}
		fn_buf_printf(&differences, "]");

		fn_buf_printf(&cmap, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
				     "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
				     "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
				     "1 begincodespacerange\n<00> <FF>\nendcodespacerange\n");
		for (code = 1; code <= shard->max_code; code++) {
			unsigned old = shard->used[code] ? shard->codes[code] : 0;
			unsigned unicode = (old != 0 && font->to_unicode != NULL && old <= font->glyph_count)
						   ? font->to_unicode[old] : 0;

			if (unicode == 0 || unicode > 0xFFFF) {
				continue;
			}
			if (written % 100 == 0) {
				if (written > 0) {
					fn_buf_printf(&cmap, "endbfchar\n");
				}
				fn_buf_printf(&cmap, "100 beginbfchar\n");
			}
			fn_buf_printf(&cmap, "<%02X> <%04X>\n", code, unicode);
			written++;
		}
		if (written > 0) {
			fn_buf_printf(&cmap, "endbfchar\n");
		}
		fn_buf_printf(&cmap, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
		fn_object_stream(w, shard->tounicode, "", cmap.data, cmap.len);
		fn_buf_free(&cmap);

		fn_object_printf(w, shard->object,
				 "<< /Type /Font /Subtype /Type1 /BaseFont /%s /FirstChar 1 /LastChar %u "
				 "/Widths %s /Encoding << /Type /Encoding /Differences %s >> "
				 "/FontDescriptor %u 0 R /ToUnicode %u 0 R >>",
				 base_name, shard->max_code,
				 widths.data != NULL ? (const char *)widths.data : "[]",
				 differences.data != NULL ? (const char *)differences.data : "[]",
				 shard->descriptor, shard->tounicode);
		fn_buf_free(&widths);
		fn_buf_free(&differences);
	}
	cg_pdf_cff_subset_free(subset);
	CFRelease(program);
}

static void fn_finalise_font(struct cg_pdf_writer *w, struct cg_pdf_font_resource *font)
{
	if (font->kind == 1) {
		fn_finalise_cff_font(w, font);
	} else {
		fn_finalise_truetype_font(w, font);
	}
}

/* THE TYPE0 FONT, ITS DESCENDANT, ITS DESCRIPTOR, THE SUBSETTED PROGRAM, THE CID→GLYPH MAP AND THE TOUNICODE
 * CMAP. Everything here waits for Close because the used-glyph set is only complete then. */
static void fn_finalise_truetype_font(struct cg_pdf_writer *w, struct cg_pdf_font_resource *font)
{
	unsigned *old_glyphs = NULL;
	cg_pdf_subset *subset = NULL;
	const unsigned char *program = NULL;
	size_t program_len = 0;
	CFDataRef full_program = NULL;
	unsigned char *map = NULL;
	size_t map_len = 0;
	unsigned map_object = 0;
	unsigned tounicode = 0;
	cg_buf widths = { 0 };
	cg_buf cmap = { 0 };
	size_t i;
	size_t written = 0;
	const char *base_name = font->base_name;
	char tagged_name[128];

	if (font->cid_count > 0) {
		old_glyphs = malloc(font->cid_count * sizeof *old_glyphs);
		if (old_glyphs != NULL) {
			for (i = 1; i <= font->cid_count; i++) {
				old_glyphs[i - 1] = font->cid_to_old[i];
			}
			subset = cg_pdf_subset_create(font->font, old_glyphs, font->cid_count);
		}
	}
	if (subset != NULL) {
		program = cg_pdf_subset_data(subset, &program_len);
		/* THE SIX-LETTER SUBSET TAG, which is what tells a reader (and pdffonts) that the glyph indices are
		 * this document's own numbering rather than the original face's. */
		snprintf(tagged_name, sizeof tagged_name, "AGORAA+%s", font->base_name);
		base_name = tagged_name;
	} else {
		/* THE SUBSETTER COULD NOT READ THIS PROGRAM, so the whole thing is embedded and the CID map points at
		 * the ORIGINAL glyph indices. Larger, and still exact. */
		CGDataProviderRef provider = cg_font_copy_provider(font->font);

		if (provider != NULL) {
			full_program = CGDataProviderCopyData(provider);
			CGDataProviderRelease(provider);
			if (full_program != NULL) {
				program = CFDataGetBytePtr(full_program);
				program_len = (size_t)CFDataGetLength(full_program);
			}
		}
	}

	/* THE CID→GLYPH MAP, two bytes per CID, CID 0 first (always 0). */
	map_len = (font->cid_count + 1) * 2;
	map = calloc(map_len ? map_len : 2, 1);
	if (map != NULL) {
		for (i = 1; i <= font->cid_count; i++) {
			unsigned new_gid = (unsigned)i;

			if (subset != NULL) {
				new_gid = cg_pdf_subset_new_gid(subset, font->cid_to_old[i]);
				if (new_gid == 0xFFFFFFFFu) {
					new_gid = 0;
				}
			} else {
				new_gid = font->cid_to_old[i];
			}
			fn_put_be16(map + i * 2, new_gid);
		}
		map_object = fn_new_object(w);
		if (map_object != 0) {
			fn_object_stream(w, map_object, "", map, map_len);
		}
	}
	free(map);

	if (program != NULL) {
		char dict[64];

		snprintf(dict, sizeof dict, "/Length1 %zu", program_len);
		fn_object_stream(w, font->font_file, dict, program, program_len);
	}
	fn_object_printf(w, font->descriptor,
			 "<< /Type /FontDescriptor /FontName /%s /Flags 32 /FontBBox [-200 -300 1200 900] "
			 "/ItalicAngle 0 /Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %u 0 R >>",
			 base_name, font->ascent, font->descent, font->cap_height, font->font_file);

	fn_buf_printf(&widths, "[");
	for (i = 1; i <= font->cid_count; i++) {
		fn_buf_printf(&widths, "%u [%u] ", (unsigned)i, font->widths[i]);
	}
	fn_buf_printf(&widths, "]");
	w->objects[font->descendant - 1].len = 0;
	fn_object_printf(w, font->descendant,
			 "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s /CIDSystemInfo << /Registry (Adobe) "
			 "/Ordering (Identity) /Supplement 0 >> /FontDescriptor %u 0 R /CIDToGIDMap %u 0 R "
			 "/DW 1000 /W %s >>",
			 base_name, font->descriptor, map_object,
			 widths.data != NULL ? (const char *)widths.data : "[]");
	fn_buf_free(&widths);

	fn_buf_printf(&cmap, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
			     "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
			     "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
			     "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
	for (i = 1; i <= font->cid_count; i++) {
		unsigned old = font->cid_to_old[i];
		unsigned code = font->to_unicode != NULL && old <= font->glyph_count ? font->to_unicode[old] : 0;

		if (code == 0 || code > 0xFFFF) {
			continue;
		}
		if (written % 100 == 0) {
			if (written > 0) {
				fn_buf_printf(&cmap, "endbfchar\n");
			}
			fn_buf_printf(&cmap, "100 beginbfchar\n");
		}
		fn_buf_printf(&cmap, "<%04X> <%04X>\n", (unsigned)i, code);
		written++;
	}
	if (written > 0) {
		fn_buf_printf(&cmap, "endbfchar\n");
	}
	fn_buf_printf(&cmap, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
	tounicode = fn_new_object(w);
	if (tounicode != 0) {
		fn_object_stream(w, tounicode, "", cmap.data, cmap.len);
	}
	fn_buf_free(&cmap);

	w->objects[font->object - 1].len = 0;
	if (tounicode != 0) {
		fn_object_printf(w, font->object,
				 "<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H "
				 "/DescendantFonts [%u 0 R] /ToUnicode %u 0 R >>",
				 base_name, font->descendant, tounicode);
	} else {
		fn_object_printf(w, font->object,
				 "<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H "
				 "/DescendantFonts [%u 0 R] >>",
				 base_name, font->descendant);
	}

	if (full_program != NULL) {
		CFRelease(full_program);
	}
	cg_pdf_subset_free(subset);
	free(old_glyphs);
}

/* ------------------------------------------------------------------------- */
/* writing the file                                                          */
/* ------------------------------------------------------------------------- */

static bool fn_write(struct cg_pdf_writer *w, const void *data, size_t len, size_t *position)
{
	const unsigned char *bytes = data;
	size_t done = 0;

	while (done < len) {
		size_t took = CGDataConsumerPutBytes(w->consumer, bytes + done, len - done);

		if (took == 0) {
			return false;
		}
		done += took;
	}
	*position += len;
	return true;
}

static void fn_write_info(struct cg_pdf_writer *w, unsigned number)
{
	static const struct {
		CFStringRef key;
		const char *name;
	} fields[] = {
		{ NULL, "Title" }, { NULL, "Author" }, { NULL, "Subject" }, { NULL, "Keywords" },
		{ NULL, "Creator" }, { NULL, "Producer" }
	};
	CFStringRef keys[6];
	cg_buf *body = &w->objects[number - 1];
	size_t i;

	keys[0] = kCGPDFContextTitle;
	keys[1] = kCGPDFContextAuthor;
	keys[2] = kCGPDFContextSubject;
	keys[3] = kCGPDFContextKeywords;
	keys[4] = kCGPDFContextCreator;
	keys[5] = kCGPDFContextProducer;
	fn_buf_put(body, "<<", 2);
	for (i = 0; i < 6; i++) {
		CFTypeRef value = w->auxiliary != NULL ? CFDictionaryGetValue(w->auxiliary, keys[i]) : NULL;

		if (value != NULL && CFGetTypeID(value) == CFStringGetTypeID()) {
			fn_buf_printf(body, " /%s ", fields[i].name);
			fn_hex_string(body, (CFStringRef)value);
		}
	}
	fn_buf_put(body, " >>", 3);
}

bool cg_pdf_writer_close(struct cg_pdf_writer *w)
{
	size_t position = 0;
	unsigned info_num = 0;
	unsigned metadata_num = 0;
	size_t i;
	unsigned char header[32];
	size_t header_len;
	size_t xref_at;
	unsigned char id[16];
	time_t now;

	if (w != NULL && w->eps != NULL) {
		return cg_eps_writer_close(w->eps);
	}
	if (w == NULL || w->closed) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextClose was called on a closed or missing context\n");
		return false;
	}
	if (w->page_open) {
		cg_pdf_writer_end_page(w);
	}
	/* EVERY FONT'S SUBSET, MAP AND CMAP IS BUILT NOW, when the used-glyph set is complete. */
	{
		size_t f;

		for (f = 0; f < w->font_count; f++) {
			fn_finalise_font(w, &w->fonts[f]);
		}
	}
	w->closed = true;
	if (w->page_count == 0) {
		fprintf(stderr, "CG-REFUSE: a CGPDFContext with no pages is not written; every PDF needs at least "
				"one page\n");
		return false;
	}
	if (w->metadata != NULL) {
		metadata_num = fn_new_object(w);
		if (metadata_num != 0) {
			fn_object_stream(w, metadata_num, "/Type /Metadata /Subtype /XML", CFDataGetBytePtr(w->metadata),
					 (size_t)CFDataGetLength(w->metadata));
		}
	}
	if (w->auxiliary != NULL) {
		info_num = fn_new_object(w);
		if (info_num != 0) {
			fn_write_info(w, info_num);
		}
	}
	/* THE PAGE TREE, NOW THAT EVERY PAGE HAS A NUMBER. */
	{
		cg_buf *pages = &w->objects[1];

		fn_buf_printf(pages, "<< /Type /Pages /Kids [");
		for (i = 0; i < w->page_count; i++) {
			fn_buf_printf(pages, "%u 0 R ", w->pages[i]);
		}
		fn_buf_printf(pages, "] /Count %zu >>", w->page_count);
	}
	/* THE CATALOG. */
	{
		cg_buf *catalog = &w->objects[0];

		fn_buf_printf(catalog, "<< /Type /Catalog /Pages 2 0 R");
		if (metadata_num != 0) {
			fn_buf_printf(catalog, " /Metadata %u 0 R", metadata_num);
		}
		fn_buf_put(catalog, " >>", 3);
	}

	header_len = (size_t)snprintf((char *)header, sizeof header, "%%PDF-1.4\n%%\xE2\xE3\xCF\xD3\n");
	if (!fn_write(w, header, header_len, &position)) {
		w->failed = true;
		return false;
	}
	{
		size_t *offsets = calloc(w->object_count ? w->object_count : 1, sizeof *offsets);

		if (offsets == NULL) {
			w->failed = true;
			return false;
		}
		for (i = 0; i < w->object_count; i++) {
			char prefix[64];
			int n;

			offsets[i] = position;
			n = snprintf(prefix, sizeof prefix, "%zu 0 obj\n", i + 1);
			if (!fn_write(w, prefix, (size_t)n, &position) ||
			    !fn_write(w, w->objects[i].data, w->objects[i].len, &position) ||
			    !fn_write(w, "\nendobj\n", 8, &position)) {
				free(offsets);
				w->failed = true;
				return false;
			}
		}
		xref_at = position;
		{
			char head[64];
			int n = snprintf(head, sizeof head, "xref\n0 %zu\n0000000000 65535 f \n", w->object_count + 1);

			if (!fn_write(w, head, (size_t)n, &position)) {
				free(offsets);
				w->failed = true;
				return false;
			}
		}
		for (i = 0; i < w->object_count; i++) {
			char entry[32];
			int n = snprintf(entry, sizeof entry, "%010zu 00000 n \n", offsets[i]);

			if (!fn_write(w, entry, (size_t)n, &position)) {
				free(offsets);
				w->failed = true;
				return false;
			}
		}
		free(offsets);
	}
	/* AN ID FROM THE CLOCK AND THE OBJECT COUNT — the file is new every time, and a PDF's /ID is only
	 * promised to be unique to the document, not derived from anything in particular. */
	now = time(NULL);
	memset(id, 0, sizeof id);
	memcpy(id, &now, sizeof now < sizeof id ? sizeof now : sizeof id);
	memcpy(id + sizeof now, &w->object_count, sizeof w->object_count < 8 ? sizeof w->object_count : 8);
	{
		char trailer[512];
		int n;
		char info_reference[32] = "";

		if (info_num != 0) {
			snprintf(info_reference, sizeof info_reference, " /Info %u 0 R", info_num);
		}
		n = snprintf(trailer, sizeof trailer,
			     "trailer\n<< /Size %zu /Root 1 0 R%s /ID [<%02X%02X%02X%02X%02X%02X%02X%02X"
			     "%02X%02X%02X%02X%02X%02X%02X%02X> <%02X%02X%02X%02X%02X%02X%02X%02X"
			     "%02X%02X%02X%02X%02X%02X%02X%02X>] >>\nstartxref\n%zu\n%%%%EOF\n",
			     w->object_count + 1, info_reference, id[0], id[1], id[2], id[3], id[4], id[5], id[6],
			     id[7], id[8], id[9], id[10], id[11], id[12], id[13], id[14], id[15], id[0], id[1],
			     id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11], id[12], id[13],
			     id[14], id[15], xref_at);
		if (n <= 0 || !fn_write(w, trailer, (size_t)n, &position)) {
			w->failed = true;
			return false;
		}
	}
	return !w->failed;
}

void cg_pdf_writer_free(struct cg_pdf_writer *w)
{
	size_t i;

	if (w == NULL) {
		return;
	}
	if (w->eps != NULL) {
		cg_eps_writer_free(w->eps);
		w->eps = NULL;
	}
	for (i = 0; i < w->object_count; i++) {
		fn_buf_free(&w->objects[i]);
	}
	free(w->objects);
	free(w->pages);
	free(w->images);
	free(w->shadings);
	{
		size_t p;

		for (p = 0; p < w->pattern_count; p++) {
			CGPatternRelease(w->patterns[p].pattern);
		}
		free(w->patterns);
	}
	{
		size_t fr;

		for (fr = 0; fr < w->frame_count; fr++) {
			fn_buf_free(&w->frames[fr].content);
		}
		free(w->frames);
	}
	{
		size_t gr;

		for (gr = 0; gr < w->group_count; gr++) {
			fn_buf_free(&w->groups[gr].content);
		}
		free(w->groups);
	}
	free(w->gstates);
	{
		size_t f;

		for (f = 0; f < w->font_count; f++) {
			CGFontRelease(w->fonts[f].font);
			free(w->fonts[f].to_unicode);
			free(w->fonts[f].old_to_cid);
			free(w->fonts[f].cid_to_old);
			free(w->fonts[f].widths);
			free(w->fonts[f].cff_code);
			free(w->fonts[f].cff_shard);
			free(w->fonts[f].shards);
		}
		free(w->fonts);
	}
	fn_buf_free(&w->content);
	if (w->auxiliary != NULL) {
		CFRelease(w->auxiliary);
	}
	if (w->metadata != NULL) {
		CFRelease(w->metadata);
	}
	if (w->consumer != NULL) {
		CGDataConsumerRelease(w->consumer);
	}
	free(w);
}
