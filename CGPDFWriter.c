/*
 * CGPDFWriter.c — recording drawing calls into content streams, and writing the file.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
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
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGImage_internal.h>
#include <CoreGraphics/CGFont_internal.h>

#include <zlib.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

struct cg_pdf_gstate {
	int ca;			/* thousandths */
	int CA;
	int blend;		/* CGBlendMode */
	unsigned number;
	char name[24];
};

/* ONE EMBEDDED FONT: the program, the two dictionaries that name it and the widths of the glyphs actually used. */
struct cg_pdf_font_resource {
	CGFontRef font;		/* retained */
	unsigned object;	/* the Type0 font */
	unsigned descendant;	/* the CIDFontType2, whose /W is left open until Close */
	char name[24];
	char base_name[96];
	int upem;
	unsigned char *used;	/* one byte per glyph, so a width is appended once */
	size_t used_count;
	size_t glyph_count;
	unsigned *to_unicode;	/* glyph index -> Unicode, zero where the cmap has no name */
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
	struct cg_pdf_gstate *gstates;
	size_t gstate_count;
	size_t gstate_cap;
	struct cg_pdf_font_resource *fonts;
	size_t font_count;
	size_t font_cap;
	unsigned next_image;
	bool warned_no_embed;

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

size_t cg_pdf_writer_page_count(struct cg_pdf_writer *w)
{
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

void cg_pdf_writer_end_page(struct cg_pdf_writer *w)
{
	unsigned content_num;
	unsigned page_num;
	cg_buf resources = { 0 };

	if (w == NULL || !w->page_open) {
		return;
	}
	content_num = fn_new_object(w);
	fn_object_stream(w, content_num, "", w->content.data, w->content.len);

	fn_buf_printf(&resources, "/ProcSet [/PDF] ");
	fn_build_resource_dict(&resources, "XObject", w->images, w->image_count);
	fn_build_gstate_dict(&resources, w->gstates, w->gstate_count);
	if (w->font_count > 0) {
		size_t f;

		fn_buf_printf(&resources, "/Font << ");
		for (f = 0; f < w->font_count; f++) {
			fn_buf_printf(&resources, "/%s %u 0 R ", w->fonts[f].name, w->fonts[f].object);
		}
		fn_buf_printf(&resources, ">> ");
	}

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
	if (w == NULL) {
		return;
	}
	if (w->metadata != NULL) {
		CFRelease(w->metadata);
	}
	w->metadata = metadata != NULL ? (CFDataRef)CFRetain(metadata) : NULL;
}

void cg_pdf_writer_refuse(struct cg_pdf_writer *w, const char *what)
{
	(void)w;
	fprintf(stderr, "CG-REFUSE: CGPDFContext does not record %s; the page will lack it\n", what);
}

/* ------------------------------------------------------------------------- */
/* recording: state                                                          */
/* ------------------------------------------------------------------------- */

void cg_pdf_writer_save(struct cg_pdf_writer *w)
{
	if (w != NULL && w->page_open) {
		fn_content_printf(w, "q\n");
	}
}

void cg_pdf_writer_restore(struct cg_pdf_writer *w)
{
	if (w != NULL && w->page_open) {
		fn_content_printf(w, "Q\n");
	}
}

void cg_pdf_writer_ctm(struct cg_pdf_writer *w, CGAffineTransform t)
{
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

static void fn_emit_gstate(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint, bool fill, bool stroke)
{
	double ca = fn_clamp01(paint->alpha * (fill ? paint->fill[3] : 1.0));
	double CA = fn_clamp01(paint->alpha * (stroke ? paint->stroke[3] : 1.0));
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

static void fn_emit_fill_colour(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint)
{
	fn_content_printf(w, "%.4f %.4f %.4f rg\n", (double)fn_clamp01(paint->fill[0]),
			  (double)fn_clamp01(paint->fill[1]), (double)fn_clamp01(paint->fill[2]));
}

static void fn_emit_stroke_colour(struct cg_pdf_writer *w, const cg_pdf_paint_state *paint)
{
	fn_content_printf(w, "%.4f %.4f %.4f RG\n", (double)fn_clamp01(paint->stroke[0]),
			  (double)fn_clamp01(paint->stroke[1]), (double)fn_clamp01(paint->stroke[2]));
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
}

/* ------------------------------------------------------------------------- */
/* recording: painting                                                       */
/* ------------------------------------------------------------------------- */

void cg_pdf_writer_paint_path(struct cg_pdf_writer *w, CGPathRef path, int operation,
			      const cg_pdf_paint_state *paint)
{
	static const char *operators[] = { "f", "f*", "S", "B", "B*" };
	bool fill = operation == CG_PDF_PAINT_FILL || operation == CG_PDF_PAINT_EOFILL ||
		    operation == CG_PDF_PAINT_FILLSTROKE || operation == CG_PDF_PAINT_EOFILLSTROKE;
	bool stroke = operation == CG_PDF_PAINT_STROKE || operation == CG_PDF_PAINT_FILLSTROKE ||
		      operation == CG_PDF_PAINT_EOFILLSTROKE;

	if (w == NULL || paint == NULL || operation < 0 || operation > 4) {
		return;
	}
	if (!w->page_open) {
		fprintf(stderr, "CG-REFUSE: a drawing call on a CGPDFContext with no page open was not "
				"recorded (call CGPDFContextBeginPage)\n");
		return;
	}
	fn_emit_gstate(w, paint, fill, stroke);
	if (fill) {
		fn_emit_fill_colour(w, paint);
	}
	if (stroke) {
		fn_emit_stroke_colour(w, paint);
	}
	if (!fn_serialise_path(&w->content, path)) {
		return;
	}
	fn_content_printf(w, "%s\n", operators[operation]);
}

void cg_pdf_writer_clip_path(struct cg_pdf_writer *w, CGPathRef path, bool even_odd)
{
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
	if (w == NULL || !w->page_open) {
		return;
	}
	fn_content_printf(w, "%.4f %.4f %.4f %.4f re W n\n", (double)rect.origin.x, (double)rect.origin.y,
			  (double)rect.size.width, (double)rect.size.height);
}

void cg_pdf_writer_clear_rect(struct cg_pdf_writer *w, CGRect rect)
{
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

	if (w == NULL || image == NULL || paint == NULL) {
		return;
	}
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

/* THE FONT RESOURCE, CREATED ON FIRST USE. NULL means "draw the outline": the face has no program, or its
 * outlines are CFF rather than `glyf`, so a CIDFontType2 pointing at it would be a lie. */
static struct cg_pdf_font_resource *fn_font_resource(struct cg_pdf_writer *w, CGFontRef font)
{
	CGDataProviderRef provider;
	CFDataRef program;
	size_t i;
	unsigned file_obj;
	unsigned descriptor_obj;
	unsigned descendant_obj;
	unsigned object;
	struct cg_pdf_font_resource *entry;
	char name[96];
	int upem;
	int ascent;
	int descent;
	int cap_height;

	if (font == NULL) {
		return NULL;
	}
	for (i = 0; i < w->font_count; i++) {
		if (w->fonts[i].font == font) {
			return &w->fonts[i];
		}
	}
	if (!cg_font_has_truetype_outlines(font)) {
		if (!w->warned_no_embed) {
			fprintf(stderr, "CG-REFUSE: a font whose outlines are not TrueType `glyf` is written as glyph "
					"OUTLINES; its program is not a /FontFile2 and pretending it is would break the "
					"reader\n");
			w->warned_no_embed = true;
		}
		return NULL;
	}
	provider = cg_font_copy_provider(font);
	if (provider == NULL) {
		return NULL;
	}
	program = CGDataProviderCopyData(provider);
	CGDataProviderRelease(provider);
	if (program == NULL) {
		return NULL;
	}
	upem = CGFontGetUnitsPerEm(font);
	if (upem <= 0) {
		CFRelease(program);
		return NULL;
	}
	ascent = fn_metric_1000(CGFontGetAscent(font), upem);
	/* PDF'S /Descent IS NEGATIVE (below the baseline); this library's CGFont door answers the magnitude. */
	descent = -abs(fn_metric_1000(CGFontGetDescent(font), upem));
	cap_height = fn_metric_1000(CGFontGetCapHeight(font), upem);

	if (w->font_count == w->font_cap) {
		size_t cap = w->font_cap ? w->font_cap * 2 : 8;
		struct cg_pdf_font_resource *grown = realloc(w->fonts, cap * sizeof *grown);

		if (grown == NULL) {
			CFRelease(program);
			return NULL;
		}
		w->fonts = grown;
		w->font_cap = cap;
	}
	entry = &w->fonts[w->font_count];
	memset(entry, 0, sizeof *entry);

	file_obj = fn_new_object(w);
	descendant_obj = fn_new_object(w);
	descriptor_obj = fn_new_object(w);
	object = fn_new_object(w);		/* the Type0 font */
	if (file_obj == 0 || descendant_obj == 0 || descriptor_obj == 0 || object == 0) {
		CFRelease(program);
		return NULL;
	}
	entry->object = object;
	entry->descendant = descendant_obj;
	entry->font = (CGFontRef)CGFontRetain(font);
	entry->upem = upem;
	entry->glyph_count = CGFontGetNumberOfGlyphs(font);
	entry->used = calloc(entry->glyph_count + 1, 1);
	entry->to_unicode = calloc(entry->glyph_count + 1, sizeof *entry->to_unicode);
	fn_font_name(font, name, sizeof name, object);
	snprintf(entry->base_name, sizeof entry->base_name, "%s", name);
	if (entry->to_unicode != NULL) {
		cg_font_unicode_map(font, entry->to_unicode, entry->glyph_count + 1);
	}
	snprintf(entry->name, sizeof entry->name, "F%u", object);
	w->font_count++;

	{
		/* `/Length1` IS THE UNCOMPRESSED PROGRAM LENGTH and must be a NUMBER; the stream helper adds /Length. */
		char dict[64];

		snprintf(dict, sizeof dict, "/Length1 %zu", (size_t)CFDataGetLength(program));
		fn_object_stream(w, file_obj, dict, CFDataGetBytePtr(program),
				 (size_t)CFDataGetLength(program));
	}
	CFRelease(program);
	fn_object_printf(w, descriptor_obj,
			 "<< /Type /FontDescriptor /FontName /%s /Flags 32 /FontBBox [-200 -300 1200 900] "
			 "/ItalicAngle 0 /Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %u 0 R >>",
			 name, ascent, descent, cap_height, file_obj);
	fn_object_printf(w, descendant_obj,
			 "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s /CIDSystemInfo << /Registry (Adobe) "
			 "/Ordering (Identity) /Supplement 0 >> /FontDescriptor %u 0 R /CIDToGIDMap /Identity "
			 "/DW 1000 /W [", name, descriptor_obj);
	fn_object_printf(w, object,
			 "<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H /DescendantFonts "
			 "[%u 0 R] >>", name, descendant_obj);
	return entry;
}

bool cg_pdf_writer_text_glyph(struct cg_pdf_writer *w, CGFontRef font, CGGlyph glyph, CGPoint pen,
			      CGFloat font_size, CGAffineTransform text_matrix, int mode,
			      const cg_pdf_paint_state *paint)
{
	struct cg_pdf_font_resource *resource;
	double em;
	double a;
	double b;
	double c;
	double d;
	double e;
	double f;

	if (w == NULL || paint == NULL || !w->page_open || font == NULL || font_size <= 0.0 ||
	    (mode != 0 && mode != 1 && mode != 2)) {
		return false;
	}
	resource = fn_font_resource(w, font);
	if (resource == NULL) {
		return false;
	}
	if (resource->used != NULL && glyph < CGFontGetNumberOfGlyphs(font) && !resource->used[glyph]) {
		int advance = 0;
		int width = 1000;

		resource->used[glyph] = 1;
		if (CGFontGetGlyphAdvances(font, &glyph, 1, &advance)) {
			width = fn_metric_1000(advance, resource->upem);
		}
		fn_object_printf(w, resource->descendant, "%u [%d] ", (unsigned)glyph, width);
		resource->used_count++;
	}

	fn_emit_gstate(w, paint, mode != 1, mode != 0);
	if (mode != 1) {
		fn_emit_fill_colour(w, paint);
	}
	if (mode != 0) {
		fn_emit_stroke_colour(w, paint);
	}
	em = (double)font_size / (double)resource->upem;
	a = text_matrix.a;
	b = text_matrix.b;
	c = text_matrix.c;
	d = text_matrix.d;
	e = pen.x + text_matrix.tx * em;
	f = pen.y + text_matrix.ty * em;
	fn_content_printf(w, "BT\n/%s %.4f Tf\n%d Tr\n%.4f %.4f %.4f %.4f %.4f %.4f Tm\n<%04X> Tj\nET\n",
			  resource->name, (double)font_size, mode, a, b, c, d, e, f, (unsigned)glyph);
	return true;
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

	if (w == NULL || w->closed) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextClose was called on a closed or missing context\n");
		return false;
	}
	if (w->page_open) {
		cg_pdf_writer_end_page(w);
	}
	/* EVERY FONT'S /W WAS LEFT OPEN SO A LATE GLYPH COULD BE ADDED; close them all now, and give each one the
	 * /ToUnicode CMap that makes its glyph IDs searchable rather than merely drawable. */
	{
		size_t f;

		for (f = 0; f < w->font_count; f++) {
			struct cg_pdf_font_resource *font = &w->fonts[f];
			cg_buf cmap = { 0 };
			unsigned tounicode;
			size_t g;
			size_t written = 0;

			fn_object_printf(w, font->descendant, "] >>");
			fn_buf_printf(&cmap, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
					     "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
					     "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
					     "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
			for (g = 0; g < font->glyph_count; g++) {
				if (font->used != NULL && !font->used[g]) {
					continue;
				}
				if (font->to_unicode == NULL || font->to_unicode[g] == 0 ||
				    font->to_unicode[g] > 0xFFFF) {
					continue;
				}
				if (written % 100 == 0) {
					if (written > 0) {
						fn_buf_printf(&cmap, "endbfchar\n");
					}
					fn_buf_printf(&cmap, "100 beginbfchar\n");
				}
				fn_buf_printf(&cmap, "<%04X> <%04X>\n", (unsigned)g, font->to_unicode[g]);
				written++;
			}
			if (written > 0) {
				fn_buf_printf(&cmap, "endbfchar\n");
			}
			fn_buf_printf(&cmap, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
			tounicode = fn_new_object(w);
			if (tounicode != 0) {
				fn_object_stream(w, tounicode, "", cmap.data, cmap.len);
				/* THE TYPE0 BODY IS REBUILT, NOT APPENDED: the first version of this left the dictionary
				 * written at creation in front of the one carrying /ToUnicode, and a reader stops at the first
				 * `>>` — so the CMap existed and no font pointed at it. */
				w->objects[font->object - 1].len = 0;
				fn_object_printf(w, font->object,
						 "<< /Type /Font /Subtype /Type0 /BaseFont /%s /Encoding /Identity-H "
						 "/DescendantFonts [%u 0 R] /ToUnicode %u 0 R >>",
						 font->base_name, font->descendant, tounicode);
			}
			fn_buf_free(&cmap);
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
	for (i = 0; i < w->object_count; i++) {
		fn_buf_free(&w->objects[i]);
	}
	free(w->objects);
	free(w->pages);
	free(w->images);
	free(w->gstates);
	{
		size_t f;

		for (f = 0; f < w->font_count; f++) {
			CGFontRelease(w->fonts[f].font);
			free(w->fonts[f].used);
			free(w->fonts[f].to_unicode);
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
