/*
 * CGEPSWriter.c — recording a page as Encapsulated PostScript.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE COMMON CASE IS EXACT. A path is the same sequence of `moveto`/`lineto`/`curveto`/`closepath` PostScript
 * has had since Level 1; a clip is `W n`; a dash is `setdash`; a flat colour is `setrgbcolor`; a glyph is its
 * outline (the caller's existing fallback), and an opaque image is `colorimage` over a PostScript hex string,
 * so no filter or inline-file protocol is needed.
 *
 * THE TWO PLACES POSTSCRIPT DIFFERS FROM PDF ARE NAMED HERE RATHER THAN PAPERED OVER. (1) It has ONE current
 * colour for both fill and stroke, so a fill-and-stroke paint emits the path twice when the two colours differ.
 * (2) It has no alpha, so a paint whose colour or context alpha is not 1, a blend mode other than Normal, a
 * soft-masked image, a transparency group and a tiling pattern are REFUSED with `EPS-REFUSE` and the rest of
 * the page is still written. A gradient or a CGShading becomes a Level 3 `shfill` with a sampled type 0
 * function, which is exact at 256 samples and does not depend on a stitching-function spelling.
 */
#include "CGEPSWriter.h"
#include "CGFunction_internal.h"
#include "CGGradient_internal.h"
#include "CGShading_internal.h"

#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGImage_internal.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGPattern.h>

#include <CoreFoundation/CFString.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EPS_GRADIENT_MAX 256

typedef struct {
	unsigned char *data;
	size_t len;
	size_t cap;
} eps_buf;

static void fn_buf_free(eps_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

static bool fn_buf_reserve(eps_buf *b, size_t extra)
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

static void fn_buf_put(eps_buf *b, const void *data, size_t len)
{
	if (len == 0 || !fn_buf_reserve(b, len)) {
		return;
	}
	memcpy(b->data + b->len, data, len);
	b->len += len;
	b->data[b->len] = '\0';
}

static void fn_buf_text(eps_buf *b, const char *text)
{
	fn_buf_put(b, text, strlen(text));
}

static void fn_buf_printf(eps_buf *b, const char *format, ...)
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

struct cg_eps_writer {
	CGDataConsumerRef consumer;
	CGRect bbox;
	CFDictionaryRef auxiliary;
	CFDataRef metadata;
	eps_buf body;
	bool page_open;
	bool page_done;
	bool closed;
	bool failed;
	bool warned_alpha;
	bool warned_blend;
	bool warned_shadow;
	bool warned_pattern;
	bool warned_boxes;
};

/* ------------------------------------------------------------------------- */
/* small spellings                                                           */
/* ------------------------------------------------------------------------- */

static void fn_num(eps_buf *b, double value)
{
	if (value > -0.00005 && value < 0.00005) {
		value = 0.0;
	}
	fn_buf_printf(b, "%.4f", value);
}

static void fn_refuse(struct cg_eps_writer *w, const char *what)
{
	(void)w;
	fprintf(stderr, "EPS-REFUSE: the EPS context does not write %s; the page will lack it\n", what);
}

void cg_eps_writer_refuse(const char *what)
{
	fn_refuse(NULL, what);
}

/* A COMMENT IS ONE LINE: a newline in a title would end it and leave the rest as PostScript. */
static void fn_comment_text(eps_buf *b, CFStringRef string)
{
	CFIndex length;
	CFIndex i;

	if (string == NULL) {
		return;
	}
	length = CFStringGetLength(string);
	for (i = 0; i < length; i++) {
		UniChar c = CFStringGetCharacterAtIndex(string, i);

		if (c == '\r' || c == '\n') {
			fn_buf_put(b, " ", 1);
		} else if (c >= 0x20 && c < 0x7F) {
			unsigned char byte = (unsigned char)c;

			fn_buf_put(b, &byte, 1);
		} else {
			fn_buf_put(b, "?", 1);
		}
	}
}

static void fn_hex_digits(eps_buf *b, const unsigned char *bytes, size_t count)
{
	static const char digits[] = "0123456789ABCDEF";
	size_t i;

	for (i = 0; i < count; i++) {
		char pair[2];

		pair[0] = digits[bytes[i] >> 4];
		pair[1] = digits[bytes[i] & 0x0F];
		fn_buf_put(b, pair, 2);
		if ((i + 1) % 32 == 0 && i + 1 < count) {
			fn_buf_put(b, "\n", 1);
		}
	}
}

/* A POSTSCRIPT HEX STRING, for a function's /DataSource. */
static void fn_hex_bytes(eps_buf *b, const unsigned char *bytes, size_t count)
{
	fn_buf_put(b, "<", 1);
	fn_hex_digits(b, bytes, count);
	fn_buf_put(b, ">\n", 2);
}


/* ------------------------------------------------------------------------- */
/* paths                                                                     */
/* ------------------------------------------------------------------------- */

struct fn_path_ctx {
	eps_buf *out;
	CGPoint current;
	CGPoint start;
	bool started;
};

static void fn_path_move(struct fn_path_ctx *ctx, CGPoint p)
{
	fn_num(ctx->out, p.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, p.y);
	fn_buf_text(ctx->out, " moveto\n");
	ctx->current = p;
	ctx->start = p;
	ctx->started = true;
}

static void fn_path_line(struct fn_path_ctx *ctx, CGPoint p)
{
	fn_num(ctx->out, p.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, p.y);
	fn_buf_text(ctx->out, " lineto\n");
	ctx->current = p;
}

static void fn_path_curve(struct fn_path_ctx *ctx, CGPoint c1, CGPoint c2, CGPoint p)
{
	fn_num(ctx->out, c1.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, c1.y);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, c2.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, c2.y);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, p.x);
	fn_buf_put(ctx->out, " ", 1);
	fn_num(ctx->out, p.y);
	fn_buf_text(ctx->out, " curveto\n");
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
		fn_buf_text(ctx->out, "closepath\n");
		ctx->current = ctx->start;
		break;
	}
}

static bool fn_serialise_path(eps_buf *out, CGPathRef path)
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
/* paint state                                                               */
/* ------------------------------------------------------------------------- */

static void fn_line_state(struct cg_eps_writer *w, const cg_pdf_paint_state *paint)
{
	size_t i;

	fn_buf_printf(&w->body, "%.4f setlinewidth\n", (double)paint->line_width);
	fn_buf_printf(&w->body, "%d setlinecap\n", paint->cap);
	fn_buf_printf(&w->body, "%d setlinejoin\n", paint->join);
	fn_buf_printf(&w->body, "%.4f setmiterlimit\n", (double)paint->miter_limit);
	if (paint->dash_count == 0) {
		fn_buf_text(&w->body, "[] 0 setdash\n");
	} else {
		fn_buf_put(&w->body, "[", 1);
		for (i = 0; i < paint->dash_count && i < 16; i++) {
			if (i != 0) {
				fn_buf_put(&w->body, " ", 1);
			}
			fn_num(&w->body, paint->dash[i]);
		}
		fn_buf_printf(&w->body, "] %.4f setdash\n", (double)paint->dash_phase);
	}
}

static void fn_colour(struct cg_eps_writer *w, const CGFloat rgba[4])
{
	fn_num(&w->body, rgba[0]);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rgba[1]);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rgba[2]);
	fn_buf_text(&w->body, " setrgbcolor\n");
}

static bool fn_transparent(struct cg_eps_writer *w, const cg_pdf_paint_state *paint)
{
	bool refused = false;

	if (paint->alpha < 0.9999) {
		if (!w->warned_alpha) {
			fn_refuse(w, "a context alpha other than 1 (PostScript has no alpha)");
			w->warned_alpha = true;
		}
		refused = true;
	}
	if (paint->fill[3] < 0.9999 || paint->stroke[3] < 0.9999) {
		if (!w->warned_alpha) {
			fn_refuse(w, "a colour alpha other than 1 (PostScript has no alpha)");
			w->warned_alpha = true;
		}
		refused = true;
	}
	if (paint->blend != kCGBlendModeNormal) {
		if (!w->warned_blend) {
			fn_refuse(w, "a blend mode other than Normal (PostScript has no blend modes)");
			w->warned_blend = true;
		}
		refused = true;
	}
	return refused;
}

/* ONE PATH IN ONE COLOUR. The path lives in `path` so a fill-and-stroke can say it twice, which is the only way
 * PostScript can use two colours for one path. */
static void fn_paint_one(struct cg_eps_writer *w, const eps_buf *path, int operation,
			 const CGFloat fill_colour[4], const CGFloat stroke_colour[4])
{
	bool fill = operation == CG_PDF_PAINT_FILL || operation == CG_PDF_PAINT_EOFILL ||
		    operation == CG_PDF_PAINT_FILLSTROKE || operation == CG_PDF_PAINT_EOFILLSTROKE;
	bool stroke = operation == CG_PDF_PAINT_STROKE || operation == CG_PDF_PAINT_FILLSTROKE ||
		      operation == CG_PDF_PAINT_EOFILLSTROKE;
	bool even = operation == CG_PDF_PAINT_EOFILL || operation == CG_PDF_PAINT_EOFILLSTROKE;

	if (fill) {
		fn_colour(w, fill_colour);
		fn_buf_put(&w->body, path->data, path->len);
		fn_buf_printf(&w->body, "%s\n", even ? "eofill" : "fill");
	}
	if (stroke) {
		fn_colour(w, stroke_colour);
		fn_buf_put(&w->body, path->data, path->len);
		fn_buf_put(&w->body, "stroke\n", 7);
	}
}

void cg_eps_writer_save(struct cg_eps_writer *w)
{
	if (w != NULL && w->page_open) {
		fn_buf_put(&w->body, "gsave\n", 6);
	}
}

void cg_eps_writer_restore(struct cg_eps_writer *w)
{
	if (w != NULL && w->page_open) {
		fn_buf_put(&w->body, "grestore\n", 9);
	}
}

void cg_eps_writer_ctm(struct cg_eps_writer *w, CGAffineTransform t)
{
	if (w == NULL || !w->page_open) {
		return;
	}
	fn_buf_put(&w->body, "[", 1);
	fn_num(&w->body, t.a);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, t.b);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, t.c);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, t.d);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, t.tx);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, t.ty);
	fn_buf_put(&w->body, "] concat\n", 9);
}

void cg_eps_writer_paint_path(struct cg_eps_writer *w, CGPathRef path, int operation,
			      const cg_pdf_paint_state *paint)
{
	eps_buf serialised = { 0 };

	if (w == NULL || paint == NULL || !w->page_open) {
		return;
	}
	if (paint->fill_pattern != NULL || paint->stroke_pattern != NULL) {
		if (!w->warned_pattern) {
			fn_refuse(w, "a tiling pattern (the EPS writer does not record tiles yet)");
			w->warned_pattern = true;
		}
		return;
	}
	if (!fn_serialise_path(&serialised, path)) {
		fn_buf_free(&serialised);
		return;
	}
	fn_line_state(w, paint);
	/* THE ALPHA IS REFUSED, THE INK IS NOT: every other part of the paint is exact, and PostScript's answer
	 * to an alpha it cannot express is the opaque colour, stated rather than silently dropped. */
	(void)fn_transparent(w, paint);
	if (paint->shadow_active) {
		CGAffineTransform offset = CGAffineTransformMakeTranslation(paint->shadow_offset[0],
									   paint->shadow_offset[1]);

		if (paint->shadow_rgba[3] < 0.9999 && !w->warned_shadow) {
			fn_refuse(w, "a shadow with alpha (PostScript has no alpha)");
			w->warned_shadow = true;
		}
		fn_buf_put(&w->body, "gsave\n", 6);
		fn_num(&w->body, offset.tx);
		fn_buf_put(&w->body, " ", 1);
		fn_num(&w->body, offset.ty);
		fn_buf_put(&w->body, " translate\n", 11);
		fn_paint_one(w, &serialised, operation, paint->shadow_rgba, paint->shadow_rgba);
		fn_buf_put(&w->body, "grestore\n", 9);
	}
	fn_paint_one(w, &serialised, operation, paint->fill, paint->stroke);
	fn_buf_free(&serialised);
}

void cg_eps_writer_clip_path(struct cg_eps_writer *w, CGPathRef path, bool even_odd)
{
	if (w == NULL || !w->page_open) {
		return;
	}
	if (!fn_serialise_path(&w->body, path)) {
		return;
	}
	fn_buf_printf(&w->body, "%s n\n", even_odd ? "W*" : "W");
}

void cg_eps_writer_clip_rect(struct cg_eps_writer *w, CGRect rect)
{
	if (w == NULL || !w->page_open) {
		return;
	}
	fn_num(&w->body, rect.origin.x);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.origin.y);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.size.width);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.size.height);
	fn_buf_text(&w->body, " rectclip\n");
}

void cg_eps_writer_clear_rect(struct cg_eps_writer *w, CGRect rect)
{
	if (w == NULL || !w->page_open) {
		return;
	}
	/* EPS IS WHITE PAPER TOO, for the same reason the PDF context says so at its own clear. */
	fn_buf_text(&w->body, "1 setgray\n");
	fn_num(&w->body, rect.origin.x);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.origin.y);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.size.width);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.size.height);
	fn_buf_text(&w->body, " rectfill\n");
}

/* ------------------------------------------------------------------------- */
/* images                                                                    */
/* ------------------------------------------------------------------------- */

static bool fn_image_opaque(CGImageRef image)
{
	CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);

	return alpha == kCGImageAlphaNone || alpha == kCGImageAlphaNoneSkipLast ||
	       alpha == kCGImageAlphaNoneSkipFirst;
}

void cg_eps_writer_image(struct cg_eps_writer *w, CGRect rect, CGImageRef image,
			 const cg_pdf_paint_state *paint)
{
	size_t width;
	size_t height;
	size_t bpc;
	size_t bpr;
	size_t components;
	size_t i;
	int map[4];
	int stored = 0;
	int straight = 0;
	CGColorSpaceRef space;
	CGBitmapInfo info;
	CGImageAlphaInfo alpha_info;
	bool has_alpha;
	CGDataProviderRef provider;
	CFDataRef data;
	const unsigned char *src;
	unsigned char *rgb;

	if (w == NULL || image == NULL || paint == NULL || !w->page_open) {
		return;
	}
	if (paint->alpha < 0.9999 || paint->blend != kCGBlendModeNormal) {
		fn_refuse(w, "an image with context alpha or a blend mode (PostScript has none)");
		return;
	}
	if (paint->shadow_active) {
		fn_refuse(w, "a shadow on an image (there is no path to silhouette)");
		return;
	}
	width = CGImageGetWidth(image);
	height = CGImageGetHeight(image);
	bpc = CGImageGetBitsPerComponent(image);
	bpr = CGImageGetBytesPerRow(image);
	space = CGImageGetColorSpace(image);
	info = CGImageGetBitmapInfo(image);
	alpha_info = (CGImageAlphaInfo)(info & kCGBitmapAlphaInfoMask);
	has_alpha = alpha_info == kCGImageAlphaPremultipliedLast || alpha_info == kCGImageAlphaLast ||
		    alpha_info == kCGImageAlphaPremultipliedFirst || alpha_info == kCGImageAlphaFirst;
	if (has_alpha) {
		fn_refuse(w, "an image with an alpha channel (PostScript has no alpha)");
		return;
	}
	if (width == 0 || height == 0 || space == NULL || rect.size.width <= 0 || rect.size.height <= 0) {
		return;
	}
	components = CGColorSpaceGetNumberOfComponents(space);
	if (bpc != 8 || (components != 1 && components != 3)) {
		fn_refuse(w, "an image that is not 8-bit grey or RGB");
		return;
	}
	cg_image_layout(space, alpha_info, (uint32_t)(info & kCGBitmapByteOrderMask), map, &stored,
			&straight);
	if (stored <= 0) {
		fn_refuse(w, "an image whose channel layout this library cannot read");
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
	rgb = malloc(width * height * 3);
	if (rgb == NULL) {
		CFRelease(data);
		return;
	}
	for (i = 0; i < width * height; i++) {
		const unsigned char *pixel = src + (i / width) * bpr + (i % width) * (size_t)stored;
		unsigned char r = components == 1 ? pixel[map[0]] : pixel[map[2]];
		unsigned char g = components == 1 ? pixel[map[0]] : pixel[map[1]];
		unsigned char b = components == 1 ? pixel[map[0]] : pixel[map[0]];

		rgb[i * 3] = r;
		rgb[i * 3 + 1] = g;
		rgb[i * 3 + 2] = b;
	}
	CFRelease(data);

	/* **THE MATRIX IS IN SAMPLE COORDINATES, NOT THE UNIT SQUARE.** PostScript's ImageMatrix maps the
	 * sample grid (0..Width, 0..Height) into user space, unlike a PDF image XObject's unit square. The CTM
	 * supplies the target rectangle and the matrix maps the samples into it, with the y axis flipped because
	 * the first sample row is the top one. */
	fn_buf_put(&w->body, "gsave\n", 6);
	fn_num(&w->body, rect.origin.x);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.origin.y);
	fn_buf_text(&w->body, " translate\n");
	fn_num(&w->body, rect.size.width / (double)width);
	fn_buf_put(&w->body, " ", 1);
	fn_num(&w->body, rect.size.height / (double)height);
	fn_buf_text(&w->body, " scale\n");
	fn_buf_printf(&w->body, "%zu %zu 8 [", width, height);
	fn_buf_text(&w->body, "1 0 0 -1 0 ");
	fn_num(&w->body, (double)height);
	fn_buf_text(&w->body, "] ");
	fn_hex_bytes(&w->body, rgb, width * height * 3);
	fn_buf_text(&w->body, " false 3 colorimage\n");
	fn_buf_text(&w->body, "grestore\n");
	free(rgb);
}

/* ------------------------------------------------------------------------- */
/* gradients and shadings                                                    */
/* ------------------------------------------------------------------------- */

/* **A GRADIENT AS A STITCHING FUNCTION, NOT A SAMPLED ONE.** A type 2 segment between two stops is the
 * gradient's own interpolation, and PostScript Level 3's type 3 function stitches them at the stop locations.
 * Flat segments at the two ends carry the clamp outside the first and last stop, which is what CGGradient's
 * stop table means. This is exact at every stop and needs no sample grid whose layout could be misread. */
static void fn_gradient_function(eps_buf *out, CGGradientRef gradient)
{
	CGFloat locations[EPS_GRADIENT_MAX];
	CGFloat colours[EPS_GRADIENT_MAX][3];
	size_t count = cg_gradient_stop_count(gradient);
	size_t kept = 0;
	size_t i;
	size_t segments = 0;
	bool has_alpha = false;

	if (count > EPS_GRADIENT_MAX) {
		count = EPS_GRADIENT_MAX;
	}
	for (i = 0; i < count; i++) {
		CGFloat location;
		CGFloat rgba[4];

		cg_gradient_stop(gradient, i, &location, rgba);
		if (rgba[3] < 0.9999) {
			has_alpha = true;
		}
		if (kept > 0 && location <= locations[kept - 1]) {
			continue;
		}
		locations[kept] = location;
		colours[kept][0] = rgba[0];
		colours[kept][1] = rgba[1];
		colours[kept][2] = rgba[2];
		kept++;
	}
	if (kept < 2 || has_alpha) {
		fn_buf_text(out, "<< /FunctionType 2 /Domain [0 1] /C0 [");
		fn_num(out, colours[0][0]);
		fn_buf_put(out, " ", 1);
		fn_num(out, colours[0][1]);
		fn_buf_put(out, " ", 1);
		fn_num(out, colours[0][2]);
		fn_buf_text(out, "] /C1 [");
		fn_num(out, colours[0][0]);
		fn_buf_put(out, " ", 1);
		fn_num(out, colours[0][1]);
		fn_buf_put(out, " ", 1);
		fn_num(out, colours[0][2]);
		fn_buf_text(out, "] /N 1 >>");
		return;
	}
	/* COUNT THE SEGMENTS FIRST: a flat head when the first stop is past 0, one per gap, and a flat tail. */
	if (locations[0] > 0.0) {
		segments++;
	}
	segments += kept - 1;
	if (locations[kept - 1] < 1.0) {
		segments++;
	}
	fn_buf_text(out, "<< /FunctionType 3 /Domain [0 1] /Functions [");
	if (locations[0] > 0.0) {
		fn_buf_printf(out, "<< /FunctionType 2 /Domain [0 1] /C0 [%.4f %.4f %.4f] /C1 [%.4f %.4f %.4f] /N 1 >>",
			      (double)colours[0][0], (double)colours[0][1], (double)colours[0][2],
			      (double)colours[0][0], (double)colours[0][1], (double)colours[0][2]);
	}
	for (i = 0; i + 1 < kept; i++) {
		fn_buf_printf(out, "<< /FunctionType 2 /Domain [0 1] /C0 [%.4f %.4f %.4f] /C1 [%.4f %.4f %.4f] /N 1 >>",
			      (double)colours[i][0], (double)colours[i][1], (double)colours[i][2],
			      (double)colours[i + 1][0], (double)colours[i + 1][1], (double)colours[i + 1][2]);
	}
	if (locations[kept - 1] < 1.0) {
		fn_buf_printf(out, "<< /FunctionType 2 /Domain [0 1] /C0 [%.4f %.4f %.4f] /C1 [%.4f %.4f %.4f] /N 1 >>",
			      (double)colours[kept - 1][0], (double)colours[kept - 1][1], (double)colours[kept - 1][2],
			      (double)colours[kept - 1][0], (double)colours[kept - 1][1], (double)colours[kept - 1][2]);
	}
	fn_buf_text(out, "] /Bounds [");
	{
		size_t written = 0;

		if (locations[0] > 0.0) {
			fn_num(out, locations[0]);
			written++;
		}
		for (i = 1; i + 1 < kept; i++) {
			if (written++ != 0) {
				fn_buf_put(out, " ", 1);
			}
			fn_num(out, locations[i]);
		}
		if (locations[kept - 1] < 1.0) {
			if (written++ != 0) {
				fn_buf_put(out, " ", 1);
			}
			fn_num(out, locations[kept - 1]);
		}
	}
	fn_buf_text(out, "] /Encode [");
	for (i = 0; i < segments; i++) {
		if (i != 0) {
			fn_buf_put(out, " ", 1);
		}
		fn_buf_text(out, "0 1");
	}
	fn_buf_text(out, "] >>");
}

/* **A SAMPLED FUNCTION AS A STITCHING OF TYPE 2 SEGMENTS.** Every segment interpolates between two evaluated
 * colours, so the result is linear at 64 samples and needs only the function type every Level 3 interpreter
 * implements. A type 0 sample grid is avoided deliberately: its bit layout is easy to get subtly wrong and the
 * failure mode is a ramp that silently reads one sample. */
static void fn_sampled_function(eps_buf *out, const CGFloat *samples, size_t count, size_t components)
{
	size_t i;
	size_t j;

	if (count < 2) {
		fn_buf_text(out, "<< /FunctionType 2 /Domain [0 1] /C0 [");
		for (j = 0; j < components; j++) {
			if (j != 0) {
				fn_buf_put(out, " ", 1);
			}
			fn_num(out, count == 1 ? samples[j] : 0.0);
		}
		fn_buf_text(out, "] /C1 [");
		for (j = 0; j < components; j++) {
			if (j != 0) {
				fn_buf_put(out, " ", 1);
			}
			fn_num(out, count == 1 ? samples[j] : 0.0);
		}
		fn_buf_text(out, "] /N 1 >>");
		return;
	}
	fn_buf_text(out, "<< /FunctionType 3 /Domain [0 1] /Functions [");
	for (i = 0; i + 1 < count; i++) {
		fn_buf_text(out, "<< /FunctionType 2 /Domain [0 1] /C0 [");
		for (j = 0; j < components; j++) {
			if (j != 0) {
				fn_buf_put(out, " ", 1);
			}
			fn_num(out, samples[i * components + j]);
		}
		fn_buf_text(out, "] /C1 [");
		for (j = 0; j < components; j++) {
			if (j != 0) {
				fn_buf_put(out, " ", 1);
			}
			fn_num(out, samples[(i + 1) * components + j]);
		}
		fn_buf_text(out, "] /N 1 >>");
	}
	fn_buf_text(out, "] /Bounds [");
	for (i = 1; i + 1 < count; i++) {
		if (i != 1) {
			fn_buf_put(out, " ", 1);
		}
		fn_num(out, (double)i / (double)(count - 1));
	}
	fn_buf_text(out, "] /Encode [");
	for (i = 0; i + 1 < count; i++) {
		if (i != 0) {
			fn_buf_put(out, " ", 1);
		}
		fn_buf_text(out, "0 1");
	}
	fn_buf_text(out, "] >>");
}

static bool fn_shading_colour_space(CGColorSpaceRef space, const char **name, size_t *components)
{
	CGColorSpaceModel model;

	if (space == NULL) {
		return false;
	}
	model = CGColorSpaceGetModel(space);
	switch (model) {
	case kCGColorSpaceModelMonochrome:
		*name = "DeviceGray";
		*components = 1;
		return true;
	case kCGColorSpaceModelRGB:
		*name = "DeviceRGB";
		*components = 3;
		return true;
	case kCGColorSpaceModelCMYK:
		*name = "DeviceCMYK";
		*components = 4;
		return true;
	default:
		return false;
	}
}

static void fn_shading_dict(eps_buf *out, int type, const CGFloat *coords, size_t coord_count,
			    const char *space, const eps_buf *function, bool extend_start, bool extend_end)
{
	size_t i;

	fn_buf_printf(out, "<< /ShadingType %d /ColorSpace /%s /Coords [", type, space);
	for (i = 0; i < coord_count; i++) {
		if (i != 0) {
			fn_buf_put(out, " ", 1);
		}
		fn_num(out, coords[i]);
	}
	fn_buf_text(out, "] /Function ");
	fn_buf_put(out, function->data, function->len);
	fn_buf_printf(out, " /Extend [%s %s] >>", extend_start ? "true" : "false",
		      extend_end ? "true" : "false");
}

bool cg_eps_writer_gradient(struct cg_eps_writer *w, CGGradientRef gradient, bool radial, CGPoint start0,
			    CGFloat radius0, CGPoint start1, CGFloat radius1,
			    CGGradientDrawingOptions options, const cg_pdf_paint_state *paint)
{
	eps_buf dict = { 0 };
	CGFloat coords[6];
	size_t coord_count;
	bool extend_start;
	bool extend_end;

	if (w == NULL || gradient == NULL || paint == NULL || !w->page_open) {
		return false;
	}
	if (paint->alpha < 0.9999 || paint->blend != kCGBlendModeNormal) {
		fn_refuse(w, "a gradient with context alpha or a blend mode (PostScript has none)");
		return false;
	}
	if (paint->shadow_active) {
		fn_refuse(w, "a shadow on a gradient (there is no path to silhouette)");
		return false;
	}
	{
		size_t stop;
		size_t stops = cg_gradient_stop_count(gradient);

		for (stop = 0; stop < stops; stop++) {
			CGFloat location;
			CGFloat rgba[4];

			cg_gradient_stop(gradient, stop, &location, rgba);
			if (rgba[3] < 0.9999) {
				fn_refuse(w, "a gradient whose stops carry alpha (PostScript has no alpha)");
				return false;
			}
		}
	}
	extend_start = (options & kCGGradientDrawsBeforeStartLocation) != 0;
	extend_end = (options & kCGGradientDrawsAfterEndLocation) != 0;
	if (radial) {
		coords[0] = start0.x;
		coords[1] = start0.y;
		coords[2] = radius0;
		coords[3] = start1.x;
		coords[4] = start1.y;
		coords[5] = radius1;
		coord_count = 6;
	} else {
		coords[0] = start0.x;
		coords[1] = start0.y;
		coords[2] = start1.x;
		coords[3] = start1.y;
		coord_count = 4;
	}
	fn_buf_printf(&dict, "<< /ShadingType %d /ColorSpace /DeviceRGB /Coords [", radial ? 3 : 2);
	{
		size_t i;

		for (i = 0; i < coord_count; i++) {
			if (i != 0) {
				fn_buf_put(&dict, " ", 1);
			}
			fn_num(&dict, coords[i]);
		}
	}
	fn_buf_text(&dict, "] /Function ");
	fn_gradient_function(&dict, gradient);
	fn_buf_printf(&dict, " /Extend [%s %s] >>", extend_start ? "true" : "false",
		      extend_end ? "true" : "false");
	fn_buf_text(&w->body, "gsave\n");
	fn_buf_put(&w->body, dict.data, dict.len);
	fn_buf_text(&w->body, " shfill\ngrestore\n");
	fn_buf_free(&dict);
	return true;
}


bool cg_eps_writer_shading(struct cg_eps_writer *w, CGShadingRef shading, const cg_pdf_paint_state *paint)
{
	CGColorSpaceRef space;
	CGFunctionRef function;
	const char *space_name;
	size_t components;
	size_t range;
	CGFloat *samples;
	size_t i;
	size_t j;
	CGPoint start;
	CGPoint end;
	CGFloat start_radius;
	CGFloat end_radius;
	bool extend_start;
	bool extend_end;
	bool radial;
	eps_buf dict = { 0 };
	CGFloat coords[6];
	size_t coord_count;

	if (w == NULL || shading == NULL || paint == NULL || !w->page_open) {
		return false;
	}
	if (paint->alpha < 0.9999 || paint->blend != kCGBlendModeNormal) {
		fn_refuse(w, "a shading with context alpha or a blend mode (PostScript has none)");
		return false;
	}
	if (paint->shadow_active) {
		fn_refuse(w, "a shadow on a shading (there is no path to silhouette)");
		return false;
	}
	space = cg_shading_color_space(shading);
	function = cg_shading_function(shading);
	if (!fn_shading_colour_space(space, &space_name, &components) || function == NULL) {
		fn_refuse(w, "a shading outside DeviceGray, DeviceRGB and DeviceCMYK");
		return false;
	}
	range = cg_function_range_dimension(function);
	if (range != components) {
		fn_refuse(w, "a shading whose function range does not match its colour space");
		return false;
	}
	samples = calloc(64 * components, sizeof *samples);
	if (samples == NULL) {
		return false;
	}
	for (i = 0; i < 64; i++) {
		CGFloat t = (CGFloat)i / 63.0;
		CGFloat output[4] = { 0, 0, 0, 0 };

		cg_function_evaluate(function, &t, output);
		for (j = 0; j < components; j++) {
			CGFloat value = output[j];

			if (value < 0.0) {
				value = 0.0;
			}
			if (value > 1.0) {
				value = 1.0;
			}
			samples[i * components + j] = value;
		}
	}
	cg_shading_geometry(shading, &start, &end, &start_radius, &end_radius);
	cg_shading_extend(shading, &extend_start, &extend_end);
	radial = cg_shading_is_radial(shading);
	if (radial) {
		coords[0] = start.x;
		coords[1] = start.y;
		coords[2] = start_radius;
		coords[3] = end.x;
		coords[4] = end.y;
		coords[5] = end_radius;
		coord_count = 6;
	} else {
		coords[0] = start.x;
		coords[1] = start.y;
		coords[2] = end.x;
		coords[3] = end.y;
		coord_count = 4;
	}
	{
		eps_buf function_dict = { 0 };

		fn_sampled_function(&function_dict, samples, 64, components);
		fn_shading_dict(&dict, radial ? 3 : 2, coords, coord_count, space_name, &function_dict,
				extend_start, extend_end);
		fn_buf_free(&function_dict);
	}
	fn_buf_put(&w->body, "gsave\n", 6);
	fn_buf_put(&w->body, dict.data, dict.len);
	fn_buf_put(&w->body, " shfill\ngrestore\n", 17);
	fn_buf_free(&dict);
	free(samples);
	return true;
}

/* ------------------------------------------------------------------------- */
/* refusals with a shape of their own                                        */
/* ------------------------------------------------------------------------- */

bool cg_eps_writer_begin_pattern(struct cg_eps_writer *w, CGPatternRef pattern)
{
	(void)pattern;
	if (w != NULL && !w->warned_pattern) {
		fn_refuse(w, "a tiling pattern (the EPS writer does not record tiles yet)");
		w->warned_pattern = true;
	}
	return false;
}

void cg_eps_writer_end_pattern(struct cg_eps_writer *w, CGPatternRef pattern)
{
	(void)w;
	(void)pattern;
}

bool cg_eps_writer_begin_group(struct cg_eps_writer *w, CGRect bbox, CGFloat alpha, CGBlendMode blend)
{
	(void)bbox;
	if (w == NULL) {
		return false;
	}
	if (alpha < 0.9999 || blend != kCGBlendModeNormal) {
		if (!w->warned_alpha) {
			fn_refuse(w, "an isolated transparency group (PostScript has no alpha or groups)");
			w->warned_alpha = true;
		}
	}
	/* FALSE IS "FLATTEN": the caller draws the layer inline, which is exact only for an opaque Normal group. */
	return false;
}

bool cg_eps_writer_in_group(struct cg_eps_writer *w)
{
	(void)w;
	return false;
}

void cg_eps_writer_end_group(struct cg_eps_writer *w)
{
	(void)w;
}

bool cg_eps_writer_text_glyph(struct cg_eps_writer *w, CGFontRef font, CGGlyph glyph, CGPoint pen,
			      CGFloat font_size, CGAffineTransform text_matrix, int mode,
			      const cg_pdf_paint_state *paint)
{
	(void)w;
	(void)font;
	(void)glyph;
	(void)pen;
	(void)font_size;
	(void)text_matrix;
	(void)mode;
	(void)paint;
	/* NO FONT PROGRAM IS EMBEDDED IN THIS LANDING: the caller's outline path is the exact text. */
	return false;
}

/* ------------------------------------------------------------------------- */
/* document life                                                             */
/* ------------------------------------------------------------------------- */

struct cg_eps_writer *cg_eps_writer_create(CGDataConsumerRef consumer, const CGRect *bounds,
					   CFDictionaryRef auxiliaryInfo)
{
	struct cg_eps_writer *w;
	CGRect box;

	if (consumer == NULL) {
		return NULL;
	}
	w = calloc(1, sizeof *w);
	if (w == NULL) {
		return NULL;
	}
	w->consumer = CGDataConsumerRetain(consumer);
	w->auxiliary = auxiliaryInfo != NULL ? (CFDictionaryRef)CFRetain(auxiliaryInfo) : NULL;
	w->bbox = CGRectMake(0, 0, 612, 792);
	if (!cg_pdf_writer_box_from_dict(auxiliaryInfo, kCGPDFContextMediaBox, &box) && bounds != NULL) {
		box = *bounds;
	}
	if (box.size.width > 0 && box.size.height > 0) {
		w->bbox = box;
	}
	return w;
}

void cg_eps_writer_free(struct cg_eps_writer *w)
{
	if (w == NULL) {
		return;
	}
	fn_buf_free(&w->body);
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

bool cg_eps_writer_begin_page(struct cg_eps_writer *w, const CGRect *mediaBox)
{
	if (w == NULL || w->closed) {
		return false;
	}
	if (w->page_open) {
		fn_refuse(w, "a second page (an EPS file has exactly one)");
		return false;
	}
	if (w->page_done) {
		fn_refuse(w, "a second page (an EPS file has exactly one)");
		return false;
	}
	w->body.len = 0;
	if (w->body.data != NULL) {
		w->body.data[0] = '\0';
	}
	if (mediaBox != NULL && mediaBox->size.width > 0 && mediaBox->size.height > 0) {
		w->bbox = *mediaBox;
	}
	w->page_open = true;
	w->page_done = true;
	return true;
}

void cg_eps_writer_set_page_box(struct cg_eps_writer *w, int which, CGRect box)
{
	if (w == NULL || box.size.width <= 0 || box.size.height <= 0) {
		return;
	}
	if (which == CG_PDF_BOX_MEDIA) {
		w->bbox = box;
	} else if (!w->warned_boxes) {
		fn_refuse(w, "a crop, bleed, trim or art box (EPS has one bounding box)");
		w->warned_boxes = true;
	}
}

void cg_eps_writer_end_page(struct cg_eps_writer *w)
{
	if (w == NULL) {
		return;
	}
	w->page_open = false;
}

void cg_eps_writer_set_metadata(struct cg_eps_writer *w, CFDataRef metadata)
{
	if (w == NULL || metadata == NULL) {
		return;
	}
	if (w->metadata != NULL) {
		CFRelease(w->metadata);
	}
	w->metadata = (CFDataRef)CFRetain(metadata);
}

size_t cg_eps_writer_page_count(struct cg_eps_writer *w)
{
	return w != NULL && w->page_done ? 1 : 0;
}

static bool fn_output(struct cg_eps_writer *w, const void *data, size_t len)
{
	if (len == 0) {
		return true;
	}
	if (CGDataConsumerPutBytes(w->consumer, data, len) != len) {
		w->failed = true;
		return false;
	}
	return true;
}

static bool fn_output_text(struct cg_eps_writer *w, const char *text)
{
	return fn_output(w, text, strlen(text));
}

static void fn_header(struct cg_eps_writer *w, eps_buf *out)
{
	char date[64] = "";
	time_t now = time(NULL);
	struct tm local;

	if (localtime_r(&now, &local) != NULL) {
		strftime(date, sizeof date, "D:%Y%m%d%H%M%S", &local);
	}
	fn_buf_text(out, "%!PS-Adobe-3.0 EPSF-3.0\n");
	fn_buf_put(out, "%%Creator: ", 11);
	if (w->auxiliary != NULL) {
		CFStringRef creator = CFDictionaryGetValue(w->auxiliary, kCGPDFContextCreator);

		if (creator != NULL) {
			fn_comment_text(out, creator);
		} else {
			fn_buf_put(out, "Agora", 5);
		}
	} else {
		fn_buf_put(out, "Agora", 5);
	}
	fn_buf_put(out, "\n", 1);
	if (w->auxiliary != NULL) {
		CFStringRef title = CFDictionaryGetValue(w->auxiliary, kCGPDFContextTitle);

		if (title != NULL) {
			fn_buf_put(out, "%%Title: ", 9);
			fn_comment_text(out, title);
			fn_buf_put(out, "\n", 1);
		}
	}
	if (date[0] != '\0') {
		fn_buf_printf(out, "%%%%CreationDate: %s\n", date);
	}
	fn_buf_printf(out, "%%%%BoundingBox: 0 0 %d %d\n", (int)ceil((double)w->bbox.size.width),
		      (int)ceil((double)w->bbox.size.height));
	fn_buf_printf(out, "%%%%HiResBoundingBox: 0 0 %.4f %.4f\n", (double)w->bbox.size.width,
		      (double)w->bbox.size.height);
	fn_buf_put(out, "%%Pages: 1\n", 11);
	fn_buf_text(out, "%%DocumentData: Clean7Bit\n");
	fn_buf_put(out, "%%LanguageLevel: 3\n", 19);
	fn_buf_put(out, "%%EndComments\n", 14);
	fn_buf_text(out, "%%BeginProlog\n%%EndProlog\n");
}

bool cg_eps_writer_close(struct cg_eps_writer *w)
{
	eps_buf header = { 0 };
	eps_buf trailer = { 0 };
	bool ok = true;

	if (w == NULL || w->closed) {
		fn_refuse(w, "a second close (the file is already written)");
		return false;
	}
	if (w->page_open) {
		w->page_open = false;
	}
	if (!w->page_done) {
		fn_refuse(w, "an EPS file with no page (call CGEPSContextBeginPage)");
		return false;
	}
	w->closed = true;
	fn_header(w, &header);
	fn_buf_text(&trailer, "%%Trailer\n%%EOF\n");

	ok = ok && fn_output(w, header.data, header.len);
	ok = ok && fn_output_text(w, "%%Page: 1 1\n");
	ok = ok && fn_output_text(w, "gsave\n");
	{
		eps_buf shift = { 0 };

		fn_num(&shift, -w->bbox.origin.x);
		fn_buf_put(&shift, " ", 1);
		fn_num(&shift, -w->bbox.origin.y);
		fn_buf_put(&shift, " translate\n", 11);
		ok = ok && fn_output(w, shift.data, shift.len);
		fn_buf_free(&shift);
	}
	{
		eps_buf clip = { 0 };

		fn_buf_text(&clip, "0 0 moveto ");
		fn_num(&clip, (double)w->bbox.size.width);
		fn_buf_text(&clip, " 0 lineto ");
		fn_num(&clip, (double)w->bbox.size.width);
		fn_buf_put(&clip, " ", 1);
		fn_num(&clip, (double)w->bbox.size.height);
		fn_buf_text(&clip, " lineto 0 ");
		fn_num(&clip, (double)w->bbox.size.height);
		fn_buf_text(&clip, " lineto closepath clip newpath\n");
		ok = ok && fn_output(w, clip.data, clip.len);
		fn_buf_free(&clip);
	}
	ok = ok && fn_output(w, w->body.data, w->body.len);
	ok = ok && fn_output_text(w, "grestore\nshowpage\n");
	ok = ok && fn_output(w, trailer.data, trailer.len);
	fn_buf_free(&header);
	fn_buf_free(&trailer);
	return ok && !w->failed;
}
