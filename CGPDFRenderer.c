/*
 * CGPDFRenderer.c — content streams become drawing: state, paths, colour, text, images, forms and shadings.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **THE OPERATOR TABLE IS THE DESIGN.** The scanner already tokenises a content stream and holds its operands
 * (`CGPDFScanner.c`); this file installs a callback for each operator that draws, and every callback pops what
 * the specification says the operator takes. Nothing here re-reads the stream.
 *
 * **PDF AND CORE GRAPHICS HAVE DIFFERENT STATE MODELS, SO THIS FILE KEEPS BOTH.** `q`/`Q` save the context's
 * graphics state AND a private `fn_state` (colours, line state, text state, font) — the first holds the CTM,
 * clip, blend and alpha that Core Graphics owns, the second holds what PDF defines beyond it. Colour is
 * converted to RGB once, in `fn_set_components`, because this library's context takes light values and because
 * a PDF's /Indexed, /Separation and /ICCBased spaces are not spaces the context can be told about.
 *
 * **TEXT IS DRAWN AS OUTLINES, NOT THROUGH THE CONTEXT'S TEXT DOORS.** `cg_font_glyph_outline` gives the glyph's
 * geometry in user space; filling that path with the current fill colour is exactly what a PDF text-showing
 * operator means, and it makes the text matrix, horizontal scaling, rise and the render modes (fill, stroke,
 * clip) one path operation each. A PDF's glyph positioning is not Core Text's, so borrowing the text stack here
 * would be a second layout engine pretending to be the first.
 *
 * **THE PAGE IS DRAWN IN ITS OWN USER SPACE.** No page rotation is applied: a caller that wants a page fitted to
 * a rectangle concatenates `CGPDFPageGetDrawingTransform` first, which is the pair Apple's samples use and the
 * contract `CGPDFPage.h` states. The y axis is the content stream's — up — which is this library's bitmap
 * context's own default, so no flip is introduced for the page.
 *
 * WHAT IS REFUSED BY NAME, ONCE PER PAGE: Type 3 fonts and named CMaps (in CGPDFFont.c), inline images (the
 * scanner skips their bytes), patterns, soft masks, mesh shadings, PostScript calculator functions, and the
 * JPX/JBIG2/CCITT image codecs. Each prints a `CG-REFUSE` line saying what was not drawn; nothing is silently
 * approximated except the two colour models (`CalRGB`, `Lab`) whose formulas are standard but whose exact
 * white-point adaptation is not, and the stroked text-clip modes (6 and 7), which clip with the filled outline.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGFont_internal.h>
#include <CoreGraphics/CGContext_internal.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGColorSpace_internal.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFContentStream.h>
#include <CoreGraphics/CGPDFFont.h>
#include <CoreGraphics/CGPDFFunction.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGPDFScanner.h>
#include <CoreGraphics/CGPDFOperatorTable.h>

#include <lcms2.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* state                                                                     */
/* ------------------------------------------------------------------------- */

/* DEVICE SPACES ARE A KIND, NOT AN OBJECT: a `g` operator names no dictionary, and an object that does not exist
 * cannot be stored. The four kinds are the four an operator can select, and KIND_OBJECT is `/cs` with a real
 * space from the resources. */
enum {
	FN_SPACE_GRAY = 0,
	FN_SPACE_RGB,
	FN_SPACE_CMYK,
	FN_SPACE_OBJECT
};

typedef struct fn_state {
	int fill_kind;
	int stroke_kind;
	cg_pdf_obj *fill_space;
	cg_pdf_obj *stroke_space;
	CGFloat fill_rgb[3];
	CGFloat stroke_rgb[3];
	CGFloat fill_alpha;
	CGFloat stroke_alpha;

	CGAffineTransform tm;
	CGAffineTransform tlm;
	cg_pdf_font *font;
	double font_size;
	double char_spacing;
	double word_spacing;
	double hscale;
	double leading;
	double rise;
	int render_mode;

	CGFloat line_width;
	CGFloat miter_limit;
	CGLineCap cap;
	CGLineJoin join;
	CGFloat dash[16];
	size_t dash_count;
	CGFloat dash_phase;

	/* A TILING PATTERN AS THE FILL OR STROKE PAINT, when `scn`/`SCN` selected one. The SPACE flag is separate
	 * because `/Pattern` is a built-in colour space, not a resource the page names. */
	bool fill_pattern_space;
	bool stroke_pattern_space;
	cg_pdf_obj *fill_pattern_obj;
	cg_pdf_obj *stroke_pattern_obj;
} fn_state;

struct fn_icc_entry {
	cg_pdf_obj *stream;
	cmsHPROFILE profile;
	cmsHPROFILE srgb;
	cmsHTRANSFORM transform;
	int components;
};

struct fn_font_entry {
	cg_pdf_obj *dict;
	cg_pdf_font *font;
};

typedef struct fn_render {
	CGContextRef ctx;
	cg_pdf_doc *doc;
	CGPDFContentStreamRef content;

	fn_state state;
	fn_state *stack;
	size_t depth;
	size_t capacity;

	cg_pdf_obj *page_resources;

	bool pending_clip;
	bool pending_clip_eo;

	struct fn_icc_entry *icc;
	size_t icc_count;
	size_t icc_capacity;

	struct fn_font_entry *fonts;
	size_t font_count;
	size_t font_capacity;

	/* THE PATTERNS RENDERED FOR THIS PAGE, keyed by their document object. A pattern's CGPattern owns its rendered
	 * tile image and the buffer behind it; releasing the pattern releases both. */
	struct fn_pdf_pattern {
		cg_pdf_obj *object;
		CGPatternRef pattern;
	} *patterns;
	size_t pattern_count;
	size_t pattern_capacity;
	int pattern_depth;

	int form_depth;
	bool warned_inline;
	bool warned_pattern;
	bool warned_smask;
	bool warned_mesh;
	bool warned_jpx;
} fn_render;

/* ------------------------------------------------------------------------- */
/* colour                                                                    */
/* ------------------------------------------------------------------------- */

static void fn_xyz_to_srgb(double x, double y, double z, CGFloat rgb[3])
{
	double r =  3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
	double g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
	double b =  0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

	r = r <= 0.0031308 ? 12.92 * r : 1.055 * pow(r, 1.0 / 2.4) - 0.055;
	g = g <= 0.0031308 ? 12.92 * g : 1.055 * pow(g, 1.0 / 2.4) - 0.055;
	b = b <= 0.0031308 ? 12.92 * b : 1.055 * pow(b, 1.0 / 2.4) - 0.055;
	rgb[0] = (CGFloat)(r < 0 ? 0 : (r > 1 ? 1 : r));
	rgb[1] = (CGFloat)(g < 0 ? 0 : (g > 1 ? 1 : g));
	rgb[2] = (CGFloat)(b < 0 ? 0 : (b > 1 ? 1 : b));
}

static double fn_component(cg_pdf_doc *doc, cg_pdf_obj *array, size_t index, double fallback)
{
	cg_pdf_obj *item;
	double value;

	if (array == NULL || index >= array->u.array.count) {
		return fallback;
	}
	item = array->u.array.items[index];
	return cg_pdf_number(doc, item, &value) ? value : fallback;
}

/* THE COMPONENT COUNT OF A PDF COLOUR SPACE, which is what a `sc`/`scn` operand count is checked against. */
static size_t fn_space_components(cg_pdf_doc *doc, cg_pdf_obj *space)
{
	space = cg_pdf_resolve(doc, space);
	if (space == NULL) {
		return 0;
	}
	if (space->type == kCGPDFObjectTypeName) {
		const char *name = (const char *)space->u.name.bytes;

		if (strcmp(name, "DeviceGray") == 0 || strcmp(name, "G") == 0) return 1;
		if (strcmp(name, "DeviceRGB") == 0 || strcmp(name, "RGB") == 0) return 3;
		if (strcmp(name, "DeviceCMYK") == 0 || strcmp(name, "CMYK") == 0) return 4;
		if (strcmp(name, "Pattern") == 0) return 0;
		return 0;
	}
	if (space->type == kCGPDFObjectTypeArray && space->u.array.count >= 2) {
		cg_pdf_obj *family = cg_pdf_resolve(doc, space->u.array.items[0]);
		const char *name = family != NULL && family->type == kCGPDFObjectTypeName ?
				   (const char *)family->u.name.bytes : "";

		if (strcmp(name, "ICCBased") == 0) {
			cg_pdf_obj *stream = cg_pdf_resolve(doc, space->u.array.items[1]);
			long long n = 0;

			if (stream != NULL && stream->type == kCGPDFObjectTypeStream) {
				cg_pdf_integer(doc, cg_pdf_dict_get(doc, stream->u.stream.dict, "N"), &n);
			}
			return n > 0 ? (size_t)n : 0;
		}
		if (strcmp(name, "Indexed") == 0) return 1;
		if (strcmp(name, "CalGray") == 0) return 1;
		if (strcmp(name, "CalRGB") == 0) return 3;
		if (strcmp(name, "Lab") == 0) return 3;
		if (strcmp(name, "Separation") == 0) return 1;
		if (strcmp(name, "DeviceN") == 0) {
			cg_pdf_obj *names = cg_pdf_resolve(doc, space->u.array.items[1]);

			return names != NULL && names->type == kCGPDFObjectTypeArray ? names->u.array.count : 0;
		}
		if (strcmp(name, "Pattern") == 0) return 0;
	}
	return 0;
}

/* A PATTERN COLOUR SPACE, in either spelling: the name `/Pattern` or the array `[/Pattern base]`. */
static bool fn_space_is_pattern(cg_pdf_doc *doc, cg_pdf_obj *space)
{
	cg_pdf_obj *family;

	space = cg_pdf_resolve(doc, space);
	if (space == NULL) {
		return false;
	}
	if (space->type == kCGPDFObjectTypeName) {
		return strcmp((const char *)space->u.name.bytes, "Pattern") == 0;
	}
	if (space->type == kCGPDFObjectTypeArray && space->u.array.count >= 1) {
		family = cg_pdf_resolve(doc, space->u.array.items[0]);

		return family != NULL && family->type == kCGPDFObjectTypeName &&
		       strcmp((const char *)family->u.name.bytes, "Pattern") == 0;
	}
	return false;
}

static bool fn_icc_convert(fn_render *r, cg_pdf_obj *stream, const double *in, CGFloat rgb[3])
{
	struct fn_icc_entry *entry = NULL;
	size_t i;

	for (i = 0; i < r->icc_count; i++) {
		if (r->icc[i].stream == stream) {
			entry = &r->icc[i];
			break;
		}
	}
	if (entry == NULL) {
		CFDataRef data;
		long long n = 0;
		cmsHPROFILE profile;
		cmsHPROFILE srgb;
		cmsHTRANSFORM transform;
		int components;

		if (stream == NULL || stream->type != kCGPDFObjectTypeStream) {
			return false;
		}
		cg_pdf_integer(r->doc, cg_pdf_dict_get(r->doc, stream->u.stream.dict, "N"), &n);
		components = (int)n;
		if (components != 1 && components != 3 && components != 4) {
			fprintf(stderr, "CG-REFUSE: an ICCBased colour space declares %lld components, and this "
					"reader converts 1, 3 and 4\n", n);
			return false;
		}
		data = cg_pdf_stream_data(r->doc, stream);
		if (data == NULL) {
			return false;
		}
		profile = cmsOpenProfileFromMem(CFDataGetBytePtr(data), (cmsUInt32Number)CFDataGetLength(data));
		CFRelease(data);
		if (profile == NULL) {
			fprintf(stderr, "CG-REFUSE: an ICC profile would not open\n");
			return false;
		}
		srgb = cmsCreate_sRGBProfile();
		transform = cmsCreateTransform(profile,
					       components == 1 ? TYPE_GRAY_DBL :
					       components == 3 ? TYPE_RGB_DBL : TYPE_CMYK_DBL,
					       srgb, TYPE_RGB_DBL, INTENT_PERCEPTUAL, 0);
		if (transform == NULL) {
			cmsCloseProfile(profile);
			cmsCloseProfile(srgb);
			fprintf(stderr, "CG-REFUSE: an ICC transform to sRGB could not be made\n");
			return false;
		}
		if (r->icc_count == r->icc_capacity) {
			size_t cap = r->icc_capacity ? r->icc_capacity * 2 : 8;
			struct fn_icc_entry *grown = realloc(r->icc, cap * sizeof *grown);

			if (grown == NULL) {
				cmsDeleteTransform(transform);
				cmsCloseProfile(profile);
				cmsCloseProfile(srgb);
				return false;
			}
			r->icc = grown;
			r->icc_capacity = cap;
		}
		entry = &r->icc[r->icc_count++];
		entry->stream = stream;
		entry->profile = profile;
		entry->srgb = srgb;
		entry->transform = transform;
		entry->components = components;
	}
	{
		double input[4] = { 0, 0, 0, 0 };
		double output[3] = { 0, 0, 0 };

		for (i = 0; i < (size_t)entry->components; i++) {
			input[i] = in[i];
		}
		cmsDoTransform(entry->transform, input, output, 1);
		rgb[0] = (CGFloat)output[0];
		rgb[1] = (CGFloat)output[1];
		rgb[2] = (CGFloat)output[2];
	}
	return true;
}

/* ONE COLOUR SPACE'S COMPONENTS TO LIGHT. `in` holds `n` values in the space's own units — normalised for the
 * device and ICC spaces, the raw index for /Indexed, the tint for /Separation. */
static bool fn_convert_color(fn_render *r, cg_pdf_obj *space, const double *in, size_t n, CGFloat rgb[3])
{
	cg_pdf_doc *doc = r->doc;

	space = cg_pdf_resolve(doc, space);
	if (space == NULL || n == 0) {
		return false;
	}
	if (space->type == kCGPDFObjectTypeName) {
		const char *name = (const char *)space->u.name.bytes;

		if (strcmp(name, "DeviceGray") == 0) {
			rgb[0] = rgb[1] = rgb[2] = (CGFloat)in[0];
			return true;
		}
		if (strcmp(name, "DeviceRGB") == 0) {
			rgb[0] = (CGFloat)in[0];
			rgb[1] = (CGFloat)in[1];
			rgb[2] = (CGFloat)in[2];
			return true;
		}
		if (strcmp(name, "DeviceCMYK") == 0) {
			CGFloat cmyk[5] = { (CGFloat)in[0], (CGFloat)in[1], (CGFloat)in[2], (CGFloat)in[3], 1.0 };
			CGFloat out[4];

			cg_device_cmyk_to_rgb(cmyk, out);
			rgb[0] = out[0];
			rgb[1] = out[1];
			rgb[2] = out[2];
			return true;
		}
		return false;
	}
	if (space->type == kCGPDFObjectTypeArray && space->u.array.count >= 2) {
		cg_pdf_obj *family = cg_pdf_resolve(doc, space->u.array.items[0]);
		const char *name = family != NULL && family->type == kCGPDFObjectTypeName ?
				   (const char *)family->u.name.bytes : "";

		if (strcmp(name, "ICCBased") == 0) {
			return fn_icc_convert(r, cg_pdf_resolve(doc, space->u.array.items[1]), in, rgb);
		}
		if (strcmp(name, "Indexed") == 0 && space->u.array.count >= 4) {
			cg_pdf_obj *base = cg_pdf_resolve(doc, space->u.array.items[1]);
			double hival_d = 0;
			cg_pdf_obj *lookup;
			size_t components = fn_space_components(doc, base);
			long long index = (long long)floor(in[0] + 0.5);
			const unsigned char *bytes;
			size_t length;
			double base_in[4] = { 0, 0, 0, 0 };
			size_t i;

			if (!cg_pdf_number(doc, space->u.array.items[2], &hival_d)) {
				return false;
			}
			if (index < 0) index = 0;
			if (index > (long long)hival_d) index = (long long)hival_d;
			lookup = cg_pdf_resolve(doc, space->u.array.items[3]);
			if (lookup == NULL || lookup->type != kCGPDFObjectTypeString || components == 0) {
				return false;
			}
			bytes = lookup->u.string.bytes;
			length = lookup->u.string.len;
			if ((size_t)index * components + components > length) {
				return false;
			}
			for (i = 0; i < components && i < 4; i++) {
				base_in[i] = (double)bytes[index * (long long)components + (long long)i] / 255.0;
			}
			return fn_convert_color(r, base, base_in, components, rgb);
		}
		if (strcmp(name, "Separation") == 0 && space->u.array.count >= 4) {
			cg_pdf_obj *alternate = cg_pdf_resolve(doc, space->u.array.items[2]);
			cg_pdf_function *function =
				cg_pdf_function_create(doc, space->u.array.items[3]);
			double out[8] = { 0 };
			bool ok;

			if (function == NULL) {
				return false;
			}
			ok = cg_pdf_function_eval(function, in, out);
			{
				size_t count = cg_pdf_function_output_count(function);

				ok = ok && fn_convert_color(r, alternate, out, count, rgb);
			}
			cg_pdf_function_free(function);
			return ok;
		}
		if (strcmp(name, "DeviceN") == 0 && space->u.array.count >= 4) {
			cg_pdf_obj *alternate = cg_pdf_resolve(doc, space->u.array.items[2]);
			cg_pdf_function *function =
				cg_pdf_function_create(doc, space->u.array.items[3]);
			double out[8] = { 0 };
			bool ok;

			if (function == NULL) {
				return false;
			}
			ok = cg_pdf_function_eval(function, in, out);
			{
				size_t count = cg_pdf_function_output_count(function);

				ok = ok && fn_convert_color(r, alternate, out, count, rgb);
			}
			cg_pdf_function_free(function);
			return ok;
		}
		if (strcmp(name, "CalGray") == 0) {
			double gamma = fn_component(doc, cg_pdf_dict_get(doc, space->u.array.items[1], "Gamma"), 0, 1.0);
			double y = pow(in[0] < 0 ? 0 : in[0], gamma);
			CGFloat out[3];

			fn_xyz_to_srgb(y, y, y, out);
			rgb[0] = out[0];
			rgb[1] = out[1];
			rgb[2] = out[2];
			return true;
		}
		if (strcmp(name, "CalRGB") == 0) {
			cg_pdf_obj *dict = cg_pdf_resolve(doc, space->u.array.items[1]);
			cg_pdf_obj *gamma = cg_pdf_dict_get(doc, dict, "Gamma");
			cg_pdf_obj *matrix = cg_pdf_dict_get(doc, dict, "Matrix");
			double g[3] = { 1, 1, 1 };
			double m[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
			double lin[3];
			double x;
			double y;
			double z;
			size_t i;

			for (i = 0; i < 3; i++) {
				g[i] = fn_component(doc, gamma, i, 1.0);
				lin[i] = pow(in[i] < 0 ? 0 : in[i], g[i]);
			}
			for (i = 0; i < 9; i++) {
				m[i] = fn_component(doc, matrix, i, (i % 4 == 0) ? 1.0 : 0.0);
			}
			x = m[0] * lin[0] + m[1] * lin[1] + m[2] * lin[2];
			y = m[3] * lin[0] + m[4] * lin[1] + m[5] * lin[2];
			z = m[6] * lin[0] + m[7] * lin[1] + m[8] * lin[2];
			fn_xyz_to_srgb(x, y, z, rgb);
			return true;
		}
		if (strcmp(name, "Lab") == 0) {
			cg_pdf_obj *dict = cg_pdf_resolve(doc, space->u.array.items[1]);
			cg_pdf_obj *white = cg_pdf_dict_get(doc, dict, "WhitePoint");
			double wx = fn_component(doc, white, 0, 0.9642);
			double wy = fn_component(doc, white, 1, 1.0);
			double wz = fn_component(doc, white, 2, 0.8249);
			double l = in[0] < 0 ? 0 : (in[0] > 100 ? 100 : in[0]);
			double a = in[1];
			double b = in[2];
			double fy = (l + 16.0) / 116.0;
			double fx = fy + a / 500.0;
			double fz = fy - b / 200.0;
			double x = wx * (fx * fx * fx > 0.008856 ? fx * fx * fx : (116.0 * fx - 16.0) / 903.3);
			double y = wy * (l > 8 ? pow((l + 16.0) / 116.0, 3) : l / 903.3);
			double z = wz * (fz * fz * fz > 0.008856 ? fz * fz * fz : (116.0 * fz - 16.0) / 903.3);

			fn_xyz_to_srgb(x, y, z, rgb);
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* applying state to the context                                             */
/* ------------------------------------------------------------------------- */

static void fn_apply_fill(fn_render *r)
{
	CGContextSetRGBFillColor(r->ctx, r->state.fill_rgb[0], r->state.fill_rgb[1], r->state.fill_rgb[2],
				 r->state.fill_alpha);
}

static void fn_apply_stroke(fn_render *r)
{
	CGContextSetRGBStrokeColor(r->ctx, r->state.stroke_rgb[0], r->state.stroke_rgb[1],
				   r->state.stroke_rgb[2], r->state.stroke_alpha);
}

static void fn_apply_line(fn_render *r)
{
	CGContextSetLineWidth(r->ctx, r->state.line_width);
	CGContextSetLineCap(r->ctx, r->state.cap);
	CGContextSetLineJoin(r->ctx, r->state.join);
	CGContextSetMiterLimit(r->ctx, r->state.miter_limit);
	CGContextSetLineDash(r->ctx, r->state.dash_phase, r->state.dash_count ? r->state.dash : NULL,
			     r->state.dash_count);
}

static void fn_set_components(fn_render *r, bool fill, const double *components, size_t count)
{
	int kind = fill ? r->state.fill_kind : r->state.stroke_kind;
	CGFloat rgb[3] = { 0, 0, 0 };

	/* A COLOUR REPLACES A PATTERN, which is the same rule `CGContextSetRGBFillColor` follows. */
	if (fill) {
		r->state.fill_pattern_obj = NULL;
		r->state.fill_pattern_space = false;
	} else {
		r->state.stroke_pattern_obj = NULL;
		r->state.stroke_pattern_space = false;
	}

	switch (kind) {
	case FN_SPACE_GRAY:
		rgb[0] = rgb[1] = rgb[2] = count > 0 ? (CGFloat)components[0] : 0;
		break;
	case FN_SPACE_RGB:
		if (count >= 3) {
			rgb[0] = (CGFloat)components[0];
			rgb[1] = (CGFloat)components[1];
			rgb[2] = (CGFloat)components[2];
		}
		break;
	case FN_SPACE_CMYK:
		if (count >= 4) {
			CGFloat cmyk[5] = { (CGFloat)components[0], (CGFloat)components[1], (CGFloat)components[2],
					    (CGFloat)components[3], 1.0 };
			CGFloat out[4];

			cg_device_cmyk_to_rgb(cmyk, out);
			rgb[0] = out[0];
			rgb[1] = out[1];
			rgb[2] = out[2];
		}
		break;
	case FN_SPACE_OBJECT: {
		cg_pdf_obj *space = fill ? r->state.fill_space : r->state.stroke_space;

		if (!fn_convert_color(r, space, components, count, rgb)) {
			fprintf(stderr, "CG-REFUSE: a colour in a space this renderer cannot convert was not set\n");
			return;
		}
		break;
	}
	}
	if (fill) {
		r->state.fill_rgb[0] = rgb[0];
		r->state.fill_rgb[1] = rgb[1];
		r->state.fill_rgb[2] = rgb[2];
		fn_apply_fill(r);
	} else {
		r->state.stroke_rgb[0] = rgb[0];
		r->state.stroke_rgb[1] = rgb[1];
		r->state.stroke_rgb[2] = rgb[2];
		fn_apply_stroke(r);
	}
}

static void fn_save(fn_render *r)
{
	if (r->depth == r->capacity) {
		size_t cap = r->capacity ? r->capacity * 2 : 16;
		fn_state *grown = realloc(r->stack, cap * sizeof *grown);

		if (grown == NULL) {
			return;
		}
		r->stack = grown;
		r->capacity = cap;
	}
	CGContextSaveGState(r->ctx);
	r->stack[r->depth++] = r->state;
}

static void fn_restore(fn_render *r)
{
	/* A `Q` WITH NO `q` IS A DAMAGED STREAM, and restoring anyway would pop a state the context never
	 * pushed — which is a crash in Core Graphics, not a wrong colour. */
	if (r->depth == 0) {
		fprintf(stderr, "CG-REFUSE: a content stream has a `Q` with no matching `q`\n");
		return;
	}
	CGContextRestoreGState(r->ctx);
	r->state = r->stack[--r->depth];
}

/* ------------------------------------------------------------------------- */
/* fonts                                                                     */
/* ------------------------------------------------------------------------- */

static cg_pdf_font *fn_font_for(fn_render *r, cg_pdf_obj *dict)
{
	cg_pdf_obj *resolved = cg_pdf_resolve(r->doc, dict);
	size_t i;

	if (resolved == NULL) {
		return NULL;
	}
	for (i = 0; i < r->font_count; i++) {
		if (r->fonts[i].dict == resolved) {
			return r->fonts[i].font;
		}
	}
	{
		cg_pdf_font *font = cg_pdf_font_create(r->doc, resolved);

		if (font == NULL) {
			return NULL;
		}
		if (r->font_count == r->font_capacity) {
			size_t cap = r->font_capacity ? r->font_capacity * 2 : 8;
			struct fn_font_entry *grown = realloc(r->fonts, cap * sizeof *grown);

			if (grown == NULL) {
				cg_pdf_font_free(font);
				return NULL;
			}
			r->fonts = grown;
			r->font_capacity = cap;
		}
		r->fonts[r->font_count].dict = resolved;
		r->fonts[r->font_count].font = font;
		r->font_count++;
		return font;
	}
}

/* ------------------------------------------------------------------------- */
/* painting, with the pending clip applied first                             */
/* ------------------------------------------------------------------------- */

/* **`W` MODIFIES THE CLIP AT THE NEXT PAINTING OPERATION, WHICHEVER IT IS.** A path-filling operator paints the
 * path it clipped; an image (`Do`), a shading (`sh`) or a text show has no path to paint, so the clip is applied
 * and the path discarded — which is the `re W Do` and `re W sh` spellings a document writes. */
static void fn_flush_clip(fn_render *r)
{
	if (!r->pending_clip) {
		return;
	}
	if (r->pending_clip_eo) {
		CGContextEOClip(r->ctx);
	} else {
		CGContextClip(r->ctx);
	}
	CGContextBeginPath(r->ctx);
	r->pending_clip = false;
	r->pending_clip_eo = false;
}

static void fn_run_content(fn_render *r, cg_pdf_obj *stream, cg_pdf_obj *resources);

/* THE DEFAULT GRAPHICS STATE A CONTENT STREAM BEGINS IN: black fill and stroke, opaque, unit width, no transform,
 * identity text matrices. The page renderer and a pattern tile both start here, so the two cannot disagree. */
static void fn_state_default(fn_state *st)
{
	memset(st, 0, sizeof *st);
	st->fill_kind = FN_SPACE_GRAY;
	st->stroke_kind = FN_SPACE_GRAY;
	st->fill_rgb[0] = st->fill_rgb[1] = st->fill_rgb[2] = 0;
	st->stroke_rgb[0] = st->stroke_rgb[1] = st->stroke_rgb[2] = 0;
	st->fill_alpha = 1;
	st->stroke_alpha = 1;
	st->tm = CGAffineTransformIdentity;
	st->tlm = CGAffineTransformIdentity;
	st->hscale = 1.0;
	st->line_width = 1.0;
	st->miter_limit = 10.0;
	st->cap = kCGLineCapButt;
	st->join = kCGLineJoinMiter;
}

struct fn_pattern_info {
	CGImageRef image;
	unsigned char *buffer;
	CGRect bbox;
};

static void fn_pattern_draw(void *info, CGContextRef cg)
{
	struct fn_pattern_info *pattern = info;

	CGContextDrawImage(cg, pattern->bbox, pattern->image);
}

static void fn_pattern_release(void *info)
{
	struct fn_pattern_info *pattern = info;

	if (pattern == NULL) {
		return;
	}
	if (pattern->image != NULL) {
		CGImageRelease(pattern->image);
	}
	free(pattern->buffer);
	free(pattern);
}

/* **A PDF TILING PATTERN BECOMES A CGPattern OVER A RENDERED TILE.** The tile's content stream is run into a
 * bitmap just large enough for its /BBox, and the CGPattern's callback paints that bitmap into the cell; the
 * matrix, steps and bounds are the dictionary's. CG's own pattern sampler then does the tiling, so a pattern is
 * drawn by the same machinery a caller's CGPattern is. PaintType 2 (uncoloured) is refused: its cell would have
 * to be drawn in the fill colour at paint time, and the content stream does not carry that. */
static CGPatternRef fn_pattern_for(fn_render *r, cg_pdf_obj *object)
{
	size_t i;
	cg_pdf_obj *dict;
	cg_pdf_obj *resources;
	CGRect bbox;
	bool ok = false;
	CGAffineTransform matrix = CGAffineTransformIdentity;
	cg_pdf_obj *matrix_obj;
	double x_step = 0;
	double y_step = 0;
	long long paint_type = 1;
	size_t width;
	size_t height;
	unsigned char *buffer;
	CGContextRef tile;
	CGColorSpaceRef space;
	CGImageRef image;
	struct fn_pattern_info *info;
	CGPatternCallbacks callbacks = { 0, fn_pattern_draw, fn_pattern_release };
	CGPatternRef pattern;
	fn_state saved_state;
	CGContextRef saved_ctx;
	CGPDFContentStreamRef saved_content;
	cg_pdf_obj *saved_resources;
	size_t saved_depth;
	bool saved_pending;
	bool saved_pending_eo;

	for (i = 0; i < r->pattern_count; i++) {
		if (r->patterns[i].object == object) {
			return r->patterns[i].pattern;
		}
	}
	if (r->pattern_depth >= 8) {
		fprintf(stderr, "CG-REFUSE: patterns nested more than 8 deep\n");
		return NULL;
	}
	if (object == NULL || object->type != kCGPDFObjectTypeStream) {
		return NULL;
	}
	dict = object->u.stream.dict;
	bbox = cg_pdf_rect_from_object(r->doc, cg_pdf_dict_get(r->doc, dict, "BBox"), &ok);
	if (!ok || bbox.size.width <= 0 || bbox.size.height <= 0) {
		fprintf(stderr, "CG-REFUSE: a tiling pattern has no usable /BBox\n");
		return NULL;
	}
	matrix_obj = cg_pdf_dict_get(r->doc, dict, "Matrix");
	if (matrix_obj != NULL && matrix_obj->type == kCGPDFObjectTypeArray && matrix_obj->u.array.count >= 6) {
		double m[6];
		size_t k;

		for (k = 0; k < 6; k++) {
			cg_pdf_number(r->doc, matrix_obj->u.array.items[k], &m[k]);
		}
		matrix = CGAffineTransformMake((CGFloat)m[0], (CGFloat)m[1], (CGFloat)m[2], (CGFloat)m[3],
					       (CGFloat)m[4], (CGFloat)m[5]);
	}
	cg_pdf_number(r->doc, cg_pdf_dict_get(r->doc, dict, "XStep"), &x_step);
	cg_pdf_number(r->doc, cg_pdf_dict_get(r->doc, dict, "YStep"), &y_step);
	if (x_step <= 0 || y_step <= 0) {
		fprintf(stderr, "CG-REFUSE: a tiling pattern has no positive /XStep and /YStep\n");
		return NULL;
	}
	cg_pdf_integer(r->doc, cg_pdf_dict_get(r->doc, dict, "PaintType"), &paint_type);
	if (paint_type != 1) {
		fprintf(stderr, "CG-REFUSE: an uncoloured tiling pattern (PaintType 2) is not painted by this "
				"renderer\n");
		return NULL;
	}
	width = (size_t)ceil(bbox.size.width);
	height = (size_t)ceil(bbox.size.height);
	if (width == 0 || height == 0 || width > 2048 || height > 2048) {
		fprintf(stderr, "CG-REFUSE: a tiling pattern's cell is %zux%zu, which is outside this renderer's "
				"1..2048 range\n", width, height);
		return NULL;
	}
	buffer = calloc(height, width * 4);
	if (buffer == NULL) {
		return NULL;
	}
	space = CGColorSpaceCreateDeviceRGB();
	tile = CGBitmapContextCreate(buffer, width, height, 8, width * 4, space,
				     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(space);
	if (tile == NULL) {
		free(buffer);
		return NULL;
	}
	CGContextClearRect(tile, CGRectMake(0, 0, (CGFloat)width, (CGFloat)height));
	CGContextTranslateCTM(tile, -bbox.origin.x, -bbox.origin.y);
	CGContextClipToRect(tile, CGRectMake(0, 0, (CGFloat)width, (CGFloat)height));

	saved_ctx = r->ctx;
	saved_state = r->state;
	saved_content = r->content;
	saved_resources = r->page_resources;
	saved_depth = r->depth;
	saved_pending = r->pending_clip;
	saved_pending_eo = r->pending_clip_eo;

	r->ctx = tile;
	fn_state_default(&r->state);
	r->depth = 0;
	r->pending_clip = false;
	r->pending_clip_eo = false;
	r->pattern_depth++;
	resources = cg_pdf_dict_get(r->doc, dict, "Resources");
	fn_run_content(r, object, resources);
	r->pattern_depth--;
	r->ctx = saved_ctx;
	r->state = saved_state;
	r->content = saved_content;
	r->page_resources = saved_resources;
	r->depth = saved_depth;
	r->pending_clip = saved_pending;
	r->pending_clip_eo = saved_pending_eo;


	image = CGBitmapContextCreateImage(tile);
	CGContextRelease(tile);
	if (image == NULL) {
		free(buffer);
		return NULL;
	}
	info = calloc(1, sizeof *info);
	if (info == NULL) {
		CGImageRelease(image);
		free(buffer);
		return NULL;
	}
	info->image = image;
	info->buffer = buffer;
	info->bbox = bbox;
	pattern = CGPatternCreate(info, bbox, matrix, (CGFloat)x_step, (CGFloat)y_step,
				  kCGPatternTilingNoDistortion, true, &callbacks);
	if (pattern == NULL) {
		fn_pattern_release(info);
		return NULL;
	}
	if (r->pattern_count == r->pattern_capacity) {
		size_t cap = r->pattern_capacity ? r->pattern_capacity * 2 : 8;
		struct fn_pdf_pattern *grown = realloc(r->patterns, cap * sizeof *grown);

		if (grown == NULL) {
			CGPatternRelease(pattern);
			return NULL;
		}
		r->patterns = grown;
		r->pattern_capacity = cap;
	}
	r->patterns[r->pattern_count].object = object;
	r->patterns[r->pattern_count].pattern = pattern;
	r->pattern_count++;
	return pattern;
}

static bool fn_apply_pattern(fn_render *r, cg_pdf_obj *object, bool fill)
{
	CGPatternRef pattern = fn_pattern_for(r, object);
	const CGFloat components[1] = { 1.0 };

	if (pattern == NULL) {
		return false;
	}
	if (fill) {
		CGContextSetFillPattern(r->ctx, pattern, components);
	} else {
		CGContextSetStrokePattern(r->ctx, pattern, components);
	}
	return true;
}

static void fn_paint(fn_render *r, int mode, bool close)
{
	if (close) {
		CGContextClosePath(r->ctx);
	}
	if (r->pending_clip) {
		CGPathRef path = CGContextCopyPath(r->ctx);

		if (r->pending_clip_eo) {
			CGContextEOClip(r->ctx);
		} else {
			CGContextClip(r->ctx);
		}
		if (path != NULL) {
			CGContextAddPath(r->ctx, path);
			CGPathRelease(path);
		}
		r->pending_clip = false;
		r->pending_clip_eo = false;
	}
	{
		bool fill = mode == kCGPathFill || mode == kCGPathFillStroke || mode == kCGPathEOFill ||
			    mode == kCGPathEOFillStroke;
		bool stroke = mode == kCGPathStroke || mode == kCGPathFillStroke || mode == kCGPathEOFillStroke;

		if ((fill && r->state.fill_pattern_obj != NULL &&
		     !fn_apply_pattern(r, r->state.fill_pattern_obj, true)) ||
		    (stroke && r->state.stroke_pattern_obj != NULL &&
		     !fn_apply_pattern(r, r->state.stroke_pattern_obj, false))) {
			CGContextBeginPath(r->ctx);
			return;
		}
	}
	if (mode == kCGPathFill || mode == kCGPathFillStroke || mode == kCGPathEOFill ||
	    mode == kCGPathEOFillStroke) {
		CGContextDrawPath(r->ctx, (CGPathDrawingMode)mode);
	} else if (mode == kCGPathStroke) {
		CGContextStrokePath(r->ctx);
	} else {
		CGContextBeginPath(r->ctx);
	}
}

/* ------------------------------------------------------------------------- */
/* text                                                                      */
/* ------------------------------------------------------------------------- */

typedef struct fn_outline_sink_ctx {
	CGContextRef ctx;
} fn_outline_sink_ctx;

static void fn_outline_move(void *info, CGFloat x, CGFloat y)
{
	fn_outline_sink_ctx *ctx = info;

	CGContextMoveToPoint(ctx->ctx, x, y);
}

static void fn_outline_line(void *info, CGFloat x, CGFloat y)
{
	fn_outline_sink_ctx *ctx = info;

	CGContextAddLineToPoint(ctx->ctx, x, y);
}

static void fn_outline_conic(void *info, CGFloat cx, CGFloat cy, CGFloat x, CGFloat y)
{
	fn_outline_sink_ctx *ctx = info;

	CGContextAddQuadCurveToPoint(ctx->ctx, cx, cy, x, y);
}

static void fn_outline_cubic(void *info, CGFloat c1x, CGFloat c1y, CGFloat c2x, CGFloat c2y, CGFloat x, CGFloat y)
{
	fn_outline_sink_ctx *ctx = info;

	CGContextAddCurveToPoint(ctx->ctx, c1x, c1y, c2x, c2y, x, y);
}

static void fn_outline_close(void *info)
{
	fn_outline_sink_ctx *ctx = info;

	CGContextClosePath(ctx->ctx);
}

static const cg_outline_sink fn_sink = {
	fn_outline_move, fn_outline_line, fn_outline_conic, fn_outline_cubic, fn_outline_close
};

static void fn_show_one(fn_render *r, const unsigned char *bytes, size_t length)
{
	cg_pdf_font *font = r->state.font;
	int code_bytes;
	size_t at = 0;
	bool drew = false;
	fn_outline_sink_ctx sink_ctx = { r->ctx };

	if (font == NULL) {
		/* TEXT WITH NO FONT IS NOT DRAWN, and the reason is the Tf operator's absence, not this string's. */
		return;
	}
	code_bytes = cg_pdf_font_code_bytes(font);
	fn_flush_clip(r);
	CGContextBeginPath(r->ctx);
	while (at + (size_t)code_bytes <= length) {
		unsigned code = bytes[at];

		if (code_bytes == 2) {
			code = ((unsigned)bytes[at] << 8) | bytes[at + 1];
		}
		at += (size_t)code_bytes;
		{
			CGGlyph glyph = cg_pdf_font_glyph(font, code);
			double width = cg_pdf_font_width(font, code);
			double advance = (width / 1000.0 * r->state.font_size + r->state.char_spacing +
					  ((code_bytes == 1 && code == 32) ? r->state.word_spacing : 0.0)) *
					 r->state.hscale;
			CGAffineTransform glyph_matrix;

			if (r->state.render_mode != 3 && r->state.render_mode != 7 && glyph != 0) {
				glyph_matrix = CGAffineTransformConcat(
					CGAffineTransformConcat(
						CGAffineTransformMakeScale(r->state.hscale, 1.0),
						CGAffineTransformMakeTranslation(0.0, r->state.rise)),
					r->state.tm);
				/* THE PATH IS FILLED WITH THE CONTEXT'S CURRENT PATH RULES, so a glyph's contours need no
				 * winding fix-up: FreeType's outlines are already in the face's own direction. */
				if (cg_font_glyph_outline(cg_pdf_font_cgfont(font), glyph,
							  (CGFloat)r->state.font_size, glyph_matrix, &fn_sink,
							  &sink_ctx)) {
					drew = true;
				}
			}
			/* EVEN AN INVISIBLE GLYPH ADVANCES THE PEN, which is why the mode test is only around the path:
			 * `3 Tr` is how a document positions a searchable text layer. */
			r->state.tm = CGAffineTransformConcat(
				CGAffineTransformMakeTranslation((CGFloat)advance, 0.0), r->state.tm);
		}
	}
	if (!drew) {
		CGContextBeginPath(r->ctx);
		return;
	}
	switch (r->state.render_mode) {
	case 0:
		CGContextFillPath(r->ctx);
		break;
	case 1:
		CGContextStrokePath(r->ctx);
		break;
	case 2:
		CGContextDrawPath(r->ctx, kCGPathFillStroke);
		break;
	case 4:
	case 5:
	case 6:
	case 7:
		/* 6 AND 7 ASK FOR A CLIP FROM THE STROKED OUTLINE; this renderer clips with the filled one and
		 * states the difference here rather than drawing a clip the document did not ask for. */
		if (r->state.render_mode == 5 || r->state.render_mode == 7) {
			CGContextEOClip(r->ctx);
		} else {
			CGContextClip(r->ctx);
		}
		break;
	default:
		CGContextBeginPath(r->ctx);
		break;
	}
}

static void fn_show_string(fn_render *r, cg_pdf_obj *string)
{
	if (string == NULL || string->type != kCGPDFObjectTypeString) {
		return;
	}
	fn_show_one(r, string->u.string.bytes, string->u.string.len);
}

/* ------------------------------------------------------------------------- */
/* images                                                                    */
/* ------------------------------------------------------------------------- */

static bool fn_filter_has(cg_pdf_doc *doc, cg_pdf_obj *dict, const char *name)
{
	cg_pdf_obj *filter = cg_pdf_dict_get(doc, dict, "Filter");

	filter = cg_pdf_resolve(doc, filter);
	if (filter == NULL) {
		filter = cg_pdf_resolve(doc, cg_pdf_dict_get(doc, dict, "F"));
	}
	if (filter == NULL) {
		return false;
	}
	if (filter->type == kCGPDFObjectTypeName) {
		return strcmp((const char *)filter->u.name.bytes, name) == 0;
	}
	if (filter->type == kCGPDFObjectTypeArray) {
		size_t i;

		for (i = 0; i < filter->u.array.count; i++) {
			cg_pdf_obj *item = cg_pdf_resolve(doc, filter->u.array.items[i]);

			if (item != NULL && item->type == kCGPDFObjectTypeName &&
			    strcmp((const char *)item->u.name.bytes, name) == 0) {
				return true;
			}
		}
	}
	return false;
}

static unsigned fn_bits(const unsigned char *row, size_t bit, int bpc)
{
	unsigned value = 0;
	int i;

	for (i = 0; i < bpc; i++) {
		size_t at = bit + (size_t)i;

		value = (value << 1) | ((row[at >> 3] >> (7 - (at & 7))) & 1u);
	}
	return value;
}

static void fn_draw_image(fn_render *r, cg_pdf_obj *stream)
{
	cg_pdf_doc *doc = r->doc;
	cg_pdf_obj *dict = stream->u.stream.dict;
	long long width = 0;
	long long height = 0;
	long long bpc = 8;
	bool mask = false;
	bool inverted = false;
	cg_pdf_obj *space;
	cg_pdf_obj *decode;
	CFDataRef data;
	CGDataProviderRef provider;
	CGImageRef image = NULL;

	cg_pdf_integer(doc, cg_pdf_dict_get(doc, dict, "Width"), &width);
	cg_pdf_integer(doc, cg_pdf_dict_get(doc, dict, "Height"), &height);
	if (width <= 0 || height <= 0 || width > 65536 || height > 65536) {
		fprintf(stderr, "CG-REFUSE: an image XObject has no usable /Width and /Height\n");
		return;
	}
	cg_pdf_integer(doc, cg_pdf_dict_get(doc, dict, "BitsPerComponent"), &bpc);
	{
		CGPDFBoolean value = 0;

		if (CGPDFDictionaryGetBoolean((CGPDFDictionaryRef)dict, "ImageMask", &value)) {
			mask = value != 0;
		}
	}
	decode = cg_pdf_dict_get(doc, dict, "Decode");
	if (mask) {
		/* PDF PAINTS WHERE THE SAMPLE IS 0 by default; Core Graphics' mask is opaque where the sample is 0,
		 * so the default decode is inverted for the mask door. */
		inverted = true;
		if (decode != NULL && decode->type == kCGPDFObjectTypeArray && decode->u.array.count >= 2) {
			double d0 = 0;
			double d1 = 1;

			cg_pdf_number(doc, decode->u.array.items[0], &d0);
			cg_pdf_number(doc, decode->u.array.items[1], &d1);
			inverted = d0 < d1;
		}
		data = cg_pdf_stream_data(doc, stream);
		if (data == NULL) {
			return;
		}
		provider = CGDataProviderCreateWithCFData(data);
		CFRelease(data);
		if (provider == NULL) {
			return;
		}
		{
			CGFloat decode_mask[2] = { inverted ? 1.0 : 0.0, inverted ? 0.0 : 1.0 };
			size_t stride = ((size_t)width + 7) / 8;

			image = CGImageMaskCreate((size_t)width, (size_t)height, 1, 1, stride, provider, decode_mask,
						  false);
		}
		CGDataProviderRelease(provider);
		if (image == NULL) {
			return;
		}
		/* NO FLIP: `CGContextDrawImage`/`ClipToMask` put an image's FIRST ROW at the TOP of the rectangle,
		 * which is PDF's own orientation for an image XObject. */
		CGContextSaveGState(r->ctx);
		CGContextClipToMask(r->ctx, CGRectMake(0, 0, 1, 1), image);
		CGContextFillRect(r->ctx, CGRectMake(0, 0, 1, 1));
		CGContextRestoreGState(r->ctx);
		CGImageRelease(image);
		return;
	}

	if (fn_filter_has(doc, dict, "DCTDecode") || fn_filter_has(doc, dict, "DCT")) {
		data = cg_pdf_stream_data(doc, stream);
		if (data == NULL) {
			return;
		}
		provider = CGDataProviderCreateWithCFData(data);
		CFRelease(data);
		if (provider == NULL) {
			return;
		}
		image = CGImageCreateWithJPEGDataProvider(provider, NULL, true, kCGRenderingIntentDefault);
		CGDataProviderRelease(provider);
		if (image == NULL) {
			fprintf(stderr, "CG-REFUSE: a DCTDecode image did not decode as JPEG\n");
			return;
		}
	} else {
		cg_pdf_obj **filter_names = NULL;
		size_t filter_count = 0;
		bool unsupported = false;
		size_t i;

		cg_pdf_obj *filter = cg_pdf_resolve(doc, cg_pdf_dict_get(doc, dict, "Filter"));
		if (filter == NULL) {
			filter = cg_pdf_resolve(doc, cg_pdf_dict_get(doc, dict, "F"));
		}
		if (filter != NULL && filter->type == kCGPDFObjectTypeName) {
			filter_names = cg_pdf_alloc(doc, sizeof *filter_names);
			if (filter_names != NULL) {
				filter_names[0] = filter;
				filter_count = 1;
			}
		} else if (filter != NULL && filter->type == kCGPDFObjectTypeArray) {
			filter_names = filter->u.array.items;
			filter_count = filter->u.array.count;
		}
		for (i = 0; i < filter_count; i++) {
			cg_pdf_obj *name = cg_pdf_resolve(doc, filter_names[i]);

			if (name != NULL && name->type == kCGPDFObjectTypeName &&
			    (strcmp((const char *)name->u.name.bytes, "JPXDecode") == 0 ||
			     strcmp((const char *)name->u.name.bytes, "JBIG2Decode") == 0 ||
			     strcmp((const char *)name->u.name.bytes, "CCITTFaxDecode") == 0)) {
				unsupported = true;
			}
		}
		if (unsupported) {
			fprintf(stderr, "CG-REFUSE: an image XObject uses JPX, JBIG2 or CCITT compression, which this "
					"renderer does not decode\n");
			return;
		}
		data = cg_pdf_stream_data(doc, stream);
		if (data == NULL) {
			return;
		}
		space = cg_pdf_dict_get(doc, dict, "ColorSpace");
		if (space == NULL) {
			CFRelease(data);
			fprintf(stderr, "CG-REFUSE: an image XObject has no /ColorSpace\n");
			return;
		}
		{
			cg_pdf_obj *space_resolved = cg_pdf_resolve(doc, space);
			size_t components = fn_space_components(doc, space_resolved);
			size_t stride = ((size_t)width * components * (size_t)bpc + 7) / 8;
			const unsigned char *src = CFDataGetBytePtr(data);
			size_t src_len = (size_t)CFDataGetLength(data);
			unsigned char *rgb = calloc((size_t)width * (size_t)height, 4);
			size_t x;
			size_t y;
			size_t c;
			bool indexed = false;
			bool has_smask = false;
			double hival = 0;
			double dmin[4] = { 0, 0, 0, 0 };
			double dmax[4] = { 1, 1, 1, 1 };
			unsigned max_sample;

			if (components == 0 || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8) ||
			    (components > 1 && bpc != 8) || rgb == NULL ||
			    stride * (size_t)height > src_len) {
				fprintf(stderr, "CG-REFUSE: an image XObject's samples are not a layout this renderer "
						"reads (%zu components, %lld bits)\n", components, bpc);
				free(rgb);
				CFRelease(data);
				return;
			}
			if (space_resolved->type == kCGPDFObjectTypeArray &&
			    space_resolved->u.array.count >= 3) {
				cg_pdf_obj *family = cg_pdf_resolve(doc, space_resolved->u.array.items[0]);

				if (family != NULL && family->type == kCGPDFObjectTypeName &&
				    strcmp((const char *)family->u.name.bytes, "Indexed") == 0) {
					indexed = true;
					cg_pdf_number(doc, space_resolved->u.array.items[2], &hival);
				}
			}
			if (indexed) {
				dmax[0] = hival;
			} else if (decode != NULL && decode->type == kCGPDFObjectTypeArray &&
				   decode->u.array.count >= 2) {
				for (c = 0; c < components && c * 2 + 1 < decode->u.array.count; c++) {
					cg_pdf_number(doc, decode->u.array.items[c * 2], &dmin[c]);
					cg_pdf_number(doc, decode->u.array.items[c * 2 + 1], &dmax[c]);
				}
			}
			max_sample = bpc >= 32 ? 0xFFFFFFFFu : ((1u << bpc) - 1);
			for (y = 0; y < (size_t)height; y++) {
				const unsigned char *row = src + y * stride;

				for (x = 0; x < (size_t)width; x++) {
					double values[4] = { 0, 0, 0, 0 };
					CGFloat rgb_out[3] = { 0, 0, 0 };

					for (c = 0; c < components; c++) {
						unsigned sample = fn_bits(row, (x * components + c) * (size_t)bpc, (int)bpc);
						double normalised = max_sample ? (double)sample / (double)max_sample : 0.0;

						if (indexed) {
							values[0] = dmin[0] + normalised * (dmax[0] - dmin[0]);
						} else {
							values[c] = dmin[c] + normalised * (dmax[c] - dmin[c]);
						}
					}
					if (!fn_convert_color(r, space_resolved, values, components, rgb_out)) {
						fprintf(stderr, "CG-REFUSE: an image XObject's colour space could not be "
								"converted, so the image was not drawn\n");
						free(rgb);
						CFRelease(data);
						return;
					}
					{
						unsigned char *pixel = rgb + (y * (size_t)width + x) * 4;
						int to8 = (int)(rgb_out[0] * 255.0 + 0.5);
						int to8g = (int)(rgb_out[1] * 255.0 + 0.5);
						int to8b = (int)(rgb_out[2] * 255.0 + 0.5);

						pixel[0] = (unsigned char)(to8 < 0 ? 0 : (to8 > 255 ? 255 : to8));
						pixel[1] = (unsigned char)(to8g < 0 ? 0 : (to8g > 255 ? 255 : to8g));
						pixel[2] = (unsigned char)(to8b < 0 ? 0 : (to8b > 255 ? 255 : to8b));
						pixel[3] = 255;
					}
				}
			}
			CFRelease(data);
			/* **THE SOFT MASK, WHICH IS THE OTHER STREAM THIS IMAGE NAMES.** `/SMask` points at a
			 * DeviceGray image of the same nominal size; its samples ARE the alpha, 0 transparent and 1
			 * opaque (with /Decode applied). Without this, an image whose transparency the writer emitted
			 * as an /SMask came back opaque — MEASURED by the round trip in tests/pdf_write.c. */
			{
				cg_pdf_obj *smask = cg_pdf_resolve(doc, cg_pdf_dict_get(doc, dict, "SMask"));

				if (smask != NULL && smask->type == kCGPDFObjectTypeStream) {
					cg_pdf_obj *mdict = smask->u.stream.dict;
					long long mw = 0;
					long long mh = 0;
					long long mbpc = 8;
					CFDataRef mdata;

					cg_pdf_integer(doc, cg_pdf_dict_get(doc, mdict, "Width"), &mw);
					cg_pdf_integer(doc, cg_pdf_dict_get(doc, mdict, "Height"), &mh);
					cg_pdf_integer(doc, cg_pdf_dict_get(doc, mdict, "BitsPerComponent"), &mbpc);
					if (mw > 0 && mh > 0 && (mbpc == 1 || mbpc == 2 || mbpc == 4 || mbpc == 8) &&
					    (mdata = cg_pdf_stream_data(doc, smask)) != NULL) {
						size_t mstride = ((size_t)mw * (size_t)mbpc + 7) / 8;

						if (mstride * (size_t)mh <= (size_t)CFDataGetLength(mdata)) {
							const unsigned char *msrc = CFDataGetBytePtr(mdata);
							unsigned mmax = (1u << mbpc) - 1;
							double md0 = 0;
							double md1 = 1;
							cg_pdf_obj *mdecode = cg_pdf_dict_get(doc, mdict, "Decode");

							if (mdecode != NULL && mdecode->type == kCGPDFObjectTypeArray &&
							    mdecode->u.array.count >= 2) {
								cg_pdf_number(doc, mdecode->u.array.items[0], &md0);
								cg_pdf_number(doc, mdecode->u.array.items[1], &md1);
							}
							for (y = 0; y < (size_t)height; y++) {
								size_t my = y * (size_t)mh / (size_t)height;

								for (x = 0; x < (size_t)width; x++) {
									size_t mx = x * (size_t)mw / (size_t)width;
									unsigned sample = fn_bits(msrc + my * mstride,
												  mx * (size_t)mbpc, (int)mbpc);
									double value = mmax ? (double)sample / (double)mmax : 0.0;
									double a = md0 + value * (md1 - md0);

									if (a < 0) a = 0;
									if (a > 1) a = 1;
									rgb[(y * (size_t)width + x) * 4 + 3] =
										(unsigned char)(a * 255.0 + 0.5);
								}
							}
							has_smask = true;
						}
						CFRelease(mdata);
					}
				}
			}
			{
				/* THE PROVIDER MUST OUTLIVE THE IMAGE, and `CreateWithData` does not copy: the converted
				 * pixels go into a CFData that the provider retains, and the working buffer is free to go. */
				CFDataRef pixels = CFDataCreate(kCFAllocatorDefault, rgb,
								(CFIndex)((size_t)width * (size_t)height * 4));

				free(rgb);
				if (pixels == NULL) {
					return;
				}
				provider = CGDataProviderCreateWithCFData(pixels);
				CFRelease(pixels);
			}
			if (provider == NULL) {
				return;
			}
			{
				CGColorSpaceRef device_rgb = CGColorSpaceCreateDeviceRGB();

				image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)width * 4,
						      device_rgb,
						      (has_smask ? kCGImageAlphaLast : kCGImageAlphaNoneSkipLast) |
							      kCGBitmapByteOrder32Big,
						      provider, NULL, true, kCGRenderingIntentDefault);
				CGColorSpaceRelease(device_rgb);
			}
			CGDataProviderRelease(provider);
			if (image == NULL) {
				return;
			}
		}
	}

	CGContextSaveGState(r->ctx);
	CGContextDrawImage(r->ctx, CGRectMake(0, 0, 1, 1), image);
	CGContextRestoreGState(r->ctx);
	CGImageRelease(image);
}

/* ------------------------------------------------------------------------- */
/* shadings                                                                  */
/* ------------------------------------------------------------------------- */

static void fn_draw_shading(fn_render *r, cg_pdf_obj *shading)
{
	cg_pdf_doc *doc = r->doc;
	long long type = 0;
	cg_pdf_obj *function;
	cg_pdf_obj *space;
	cg_pdf_obj *coords;
	cg_pdf_obj *extend;
	double xy[6] = { 0, 0, 0, 0, 0, 0 };
	CGGradientRef gradient;
	CFMutableArrayRef colors;
	CGColorSpaceRef rgb_space;
	size_t samples = 64;
	size_t i;
	int options = 0;

	cg_pdf_integer(doc, cg_pdf_dict_get(doc, shading, "ShadingType"), &type);
	if (type != 2 && type != 3) {
		if (type >= 4 && !r->warned_mesh) {
			fprintf(stderr, "CG-REFUSE: a mesh shading (type %lld) is not drawn; this renderer draws axial "
					"and radial shadings only\n", type);
			r->warned_mesh = true;
		}
		return;
	}
	fn_flush_clip(r);
	coords = cg_pdf_dict_get(doc, shading, "Coords");
	space = cg_pdf_dict_get(doc, shading, "ColorSpace");
	function = cg_pdf_dict_get(doc, shading, "Function");
	if (coords == NULL || coords->type != kCGPDFObjectTypeArray || space == NULL || function == NULL) {
		fprintf(stderr, "CG-REFUSE: a shading lacks /Coords, /ColorSpace or /Function\n");
		return;
	}
	for (i = 0; i < coords->u.array.count && i < 6; i++) {
		cg_pdf_number(doc, coords->u.array.items[i], &xy[i]);
	}
	extend = cg_pdf_dict_get(doc, shading, "Extend");
	if (extend != NULL && extend->type == kCGPDFObjectTypeArray) {
		CGPDFBoolean value = 0;

		if (extend->u.array.count >= 1 &&
		    CGPDFArrayGetBoolean((CGPDFArrayRef)extend, 0, &value) && value) {
			options |= kCGGradientDrawsBeforeStartLocation;
		}
		if (extend->u.array.count >= 2 &&
		    CGPDFArrayGetBoolean((CGPDFArrayRef)extend, 1, &value) && value) {
			options |= kCGGradientDrawsAfterEndLocation;
		}
	}
	colors = CFArrayCreateMutable(kCFAllocatorDefault, (CFIndex)samples, &kCFTypeArrayCallBacks);
	rgb_space = CGColorSpaceCreateDeviceRGB();
	if (colors == NULL || rgb_space == NULL) {
		if (colors != NULL) CFRelease(colors);
		if (rgb_space != NULL) CGColorSpaceRelease(rgb_space);
		return;
	}
	{
		/* THE RAMP'S FUNCTION IS PARSED ONCE AND EVALUATED PER STOP; a shading's function is one-dimensional,
		 * and a multi-input one here would be a colour space rather than a ramp. */
		cg_pdf_function *ramp = cg_pdf_function_create(doc, function);

		if (ramp == NULL) {
			CFRelease(colors);
			CGColorSpaceRelease(rgb_space);
			return;
		}
		for (i = 0; i < samples; i++) {
			double t = (double)i / (double)(samples - 1);
			double input[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
			double output[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
			CGFloat rgb[3] = { 0, 0, 0 };
			CGColorRef color;
			CGFloat components[4];
			size_t inputs = cg_pdf_function_input_count(ramp);
			size_t k;

			for (k = 0; k < inputs && k < 8; k++) {
				input[k] = t;
			}
			if (!cg_pdf_function_eval(ramp, input, output) ||
			    !fn_convert_color(r, space, output, cg_pdf_function_output_count(ramp), rgb)) {
				cg_pdf_function_free(ramp);
				CFRelease(colors);
				CGColorSpaceRelease(rgb_space);
				return;
			}
			components[0] = rgb[0];
			components[1] = rgb[1];
			components[2] = rgb[2];
			components[3] = 1.0;
			color = CGColorCreate(rgb_space, components);
			if (color != NULL) {
				CFArrayAppendValue(colors, color);
				CGColorRelease(color);
			}
		}
		cg_pdf_function_free(ramp);
	}
	gradient = CGGradientCreateWithColors(rgb_space, colors, NULL);
	CFRelease(colors);
	CGColorSpaceRelease(rgb_space);
	if (gradient == NULL) {
		return;
	}
	if (type == 2) {
		CGContextDrawLinearGradient(r->ctx, gradient, CGPointMake(xy[0], xy[1]),
					    CGPointMake(xy[2], xy[3]), (CGGradientDrawingOptions)options);
	} else {
		CGContextDrawRadialGradient(r->ctx, gradient, CGPointMake(xy[0], xy[1]), (CGFloat)xy[2],
					    CGPointMake(xy[3], xy[4]), (CGFloat)xy[5],
					    (CGGradientDrawingOptions)options);
	}
	CGGradientRelease(gradient);
}

/* ------------------------------------------------------------------------- */
/* forms, ExtGState                                                          */
/* ------------------------------------------------------------------------- */

static void fn_run_content(fn_render *r, cg_pdf_obj *stream, cg_pdf_obj *resources);

static void fn_do_xobject(fn_render *r, const char *name)
{
	cg_pdf_obj *object = cg_pdf_resolve(r->doc,
					    (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "XObject", name));
	cg_pdf_obj *dict;
	cg_pdf_obj *subtype;

	if (object == NULL || object->type != kCGPDFObjectTypeStream) {
		return;
	}
	fn_flush_clip(r);
	dict = object->u.stream.dict;
	subtype = cg_pdf_dict_get(r->doc, dict, "Subtype");
	if (cg_pdf_name_is(r->doc, subtype, "Image")) {
		fn_draw_image(r, object);
		return;
	}
	if (cg_pdf_name_is(r->doc, subtype, "Form")) {
		cg_pdf_obj *form_resources = cg_pdf_dict_get(r->doc, dict, "Resources");
		cg_pdf_obj *bbox = cg_pdf_dict_get(r->doc, dict, "BBox");
		cg_pdf_obj *form_matrix = cg_pdf_dict_get(r->doc, dict, "Matrix");
		CGAffineTransform matrix = CGAffineTransformIdentity;
		bool ok = false;
		CGRect box;

		if (r->form_depth >= 16) {
			fprintf(stderr, "CG-REFUSE: form XObjects nested more than 16 deep — this is a cycle\n");
			return;
		}
		if (form_matrix != NULL && form_matrix->type == kCGPDFObjectTypeArray &&
		    form_matrix->u.array.count >= 6) {
			double m[6];
			size_t i;

			for (i = 0; i < 6; i++) {
				cg_pdf_number(r->doc, form_matrix->u.array.items[i], &m[i]);
			}
			matrix = CGAffineTransformMake((CGFloat)m[0], (CGFloat)m[1], (CGFloat)m[2], (CGFloat)m[3],
						       (CGFloat)m[4], (CGFloat)m[5]);
		}
		box = cg_pdf_rect_from_object(r->doc, bbox, &ok);
		CGContextSaveGState(r->ctx);
		CGContextConcatCTM(r->ctx, matrix);
		if (ok) {
			CGContextClipToRect(r->ctx, box);
		}
		r->form_depth++;
		fn_run_content(r, object, form_resources != NULL ? form_resources : r->page_resources);
		r->form_depth--;
		CGContextRestoreGState(r->ctx);
		return;
	}
	fprintf(stderr, "CG-REFUSE: an XObject of subtype `%s` is not drawn\n",
		subtype != NULL && subtype->type == kCGPDFObjectTypeName ?
		(const char *)subtype->u.name.bytes : "(none)");
}

static void fn_apply_extgstate(fn_render *r, cg_pdf_obj *dict)
{
	cg_pdf_obj *value;
	double number;
	long long integer;

	dict = cg_pdf_resolve(r->doc, dict);
	if (dict == NULL || dict->type != kCGPDFObjectTypeDictionary) {
		return;
	}
	if (cg_pdf_number(r->doc, cg_pdf_dict_get(r->doc, dict, "LW"), &number)) {
		r->state.line_width = (CGFloat)number;
	}
	if (cg_pdf_integer(r->doc, cg_pdf_dict_get(r->doc, dict, "LC"), &integer)) {
		r->state.cap = (CGLineCap)(integer < 0 ? 0 : (integer > 2 ? 2 : integer));
	}
	if (cg_pdf_integer(r->doc, cg_pdf_dict_get(r->doc, dict, "LJ"), &integer)) {
		r->state.join = (CGLineJoin)(integer < 0 ? 0 : (integer > 2 ? 2 : integer));
	}
	if (cg_pdf_number(r->doc, cg_pdf_dict_get(r->doc, dict, "ML"), &number)) {
		r->state.miter_limit = (CGFloat)number;
	}
	if (cg_pdf_number(r->doc, cg_pdf_dict_get(r->doc, dict, "ca"), &number)) {
		r->state.fill_alpha = (CGFloat)(number < 0 ? 0 : (number > 1 ? 1 : number));
		fn_apply_fill(r);
	}
	if (cg_pdf_number(r->doc, cg_pdf_dict_get(r->doc, dict, "CA"), &number)) {
		r->state.stroke_alpha = (CGFloat)(number < 0 ? 0 : (number > 1 ? 1 : number));
		fn_apply_stroke(r);
	}
	value = cg_pdf_dict_get(r->doc, dict, "BM");
	if (value != NULL && value->type == kCGPDFObjectTypeName) {
		const char *name = (const char *)value->u.name.bytes;
		CGBlendMode mode = kCGBlendModeNormal;

		if (strcmp(name, "Multiply") == 0) mode = kCGBlendModeMultiply;
		else if (strcmp(name, "Screen") == 0) mode = kCGBlendModeScreen;
		else if (strcmp(name, "Overlay") == 0) mode = kCGBlendModeOverlay;
		else if (strcmp(name, "Darken") == 0) mode = kCGBlendModeDarken;
		else if (strcmp(name, "Lighten") == 0) mode = kCGBlendModeLighten;
		else if (strcmp(name, "ColorDodge") == 0) mode = kCGBlendModeColorDodge;
		else if (strcmp(name, "ColorBurn") == 0) mode = kCGBlendModeColorBurn;
		else if (strcmp(name, "HardLight") == 0) mode = kCGBlendModeHardLight;
		else if (strcmp(name, "SoftLight") == 0) mode = kCGBlendModeSoftLight;
		else if (strcmp(name, "Difference") == 0) mode = kCGBlendModeDifference;
		else if (strcmp(name, "Exclusion") == 0) mode = kCGBlendModeExclusion;
		else if (strcmp(name, "Hue") == 0) mode = kCGBlendModeHue;
		else if (strcmp(name, "Saturation") == 0) mode = kCGBlendModeSaturation;
		else if (strcmp(name, "Color") == 0) mode = kCGBlendModeColor;
		else if (strcmp(name, "Luminosity") == 0) mode = kCGBlendModeLuminosity;
		CGContextSetBlendMode(r->ctx, mode);
	}
	value = cg_pdf_dict_get(r->doc, dict, "D");
	if (value != NULL && value->type == kCGPDFObjectTypeArray && value->u.array.count >= 2) {
		cg_pdf_obj *array = cg_pdf_resolve(r->doc, value->u.array.items[0]);
		double phase = 0;

		cg_pdf_number(r->doc, value->u.array.items[1], &phase);
		r->state.dash_count = 0;
		if (array != NULL && array->type == kCGPDFObjectTypeArray) {
			size_t i;

			for (i = 0; i < array->u.array.count && i < 16; i++) {
				double d;

				if (cg_pdf_number(r->doc, array->u.array.items[i], &d) && d >= 0) {
					r->state.dash[r->state.dash_count++] = (CGFloat)d;
				}
			}
		}
		r->state.dash_phase = (CGFloat)phase;
	}
	value = cg_pdf_dict_get(r->doc, dict, "Font");
	if (value != NULL && value->type == kCGPDFObjectTypeArray && value->u.array.count >= 2) {
		cg_pdf_font *font = fn_font_for(r, value->u.array.items[0]);
		double size = 0;

		if (font != NULL && cg_pdf_number(r->doc, value->u.array.items[1], &size)) {
			r->state.font = font;
			r->state.font_size = size;
		}
	}
	if (cg_pdf_dict_get(r->doc, dict, "SMask") != NULL && !r->warned_smask) {
		fprintf(stderr, "CG-REFUSE: a soft mask (/SMask) in an ExtGState is not applied; the object is "
				"drawn without it\n");
		r->warned_smask = true;
	}
	fn_apply_line(r);
}

/* ------------------------------------------------------------------------- */
/* the operator callbacks                                                    */
/* ------------------------------------------------------------------------- */

static double fn_pop_number(fn_render *r, CGPDFScannerRef scanner)
{
	CGPDFReal value = 0;

	if (!CGPDFScannerPopNumber(scanner, &value)) {
		fprintf(stderr, "CG-REFUSE: an operator expected a number and did not find one\n");
	}
	return (double)value;
}

static void fn_op_q(CGPDFScannerRef s, void *info) { fn_save(info); (void)s; }
static void fn_op_Q(CGPDFScannerRef s, void *info) { fn_restore(info); (void)s; }

static void fn_op_cm(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double f = fn_pop_number(r, s);
	double e = fn_pop_number(r, s);
	double d = fn_pop_number(r, s);
	double c = fn_pop_number(r, s);
	double b = fn_pop_number(r, s);
	double a = fn_pop_number(r, s);

	CGContextConcatCTM(r->ctx, CGAffineTransformMake((CGFloat)a, (CGFloat)b, (CGFloat)c, (CGFloat)d,
							(CGFloat)e, (CGFloat)f));
}

static void fn_op_w(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->state.line_width = (CGFloat)fn_pop_number(r, s);
	fn_apply_line(r);
}

static void fn_op_J(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFInteger value = 0;

	if (CGPDFScannerPopInteger(s, &value)) {
		r->state.cap = (CGLineCap)(value < 0 ? 0 : (value > 2 ? 2 : value));
		fn_apply_line(r);
	}
}

static void fn_op_j(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFInteger value = 0;

	if (CGPDFScannerPopInteger(s, &value)) {
		r->state.join = (CGLineJoin)(value < 0 ? 0 : (value > 2 ? 2 : value));
		fn_apply_line(r);
	}
}

static void fn_op_M(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->state.miter_limit = (CGFloat)fn_pop_number(r, s);
	fn_apply_line(r);
}

static void fn_op_d(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFArrayRef array = NULL;
	CGPDFReal phase = 0;

	if (!CGPDFScannerPopNumber(s, &phase) || !CGPDFScannerPopArray(s, &array)) {
		return;
	}
	r->state.dash_count = 0;
	{
		size_t i;
		size_t count = CGPDFArrayGetCount(array);

		for (i = 0; i < count && i < 16; i++) {
			CGPDFReal d = 0;

			if (CGPDFArrayGetNumber(array, i, &d) && d >= 0) {
				r->state.dash[r->state.dash_count++] = (CGFloat)d;
			}
		}
	}
	r->state.dash_phase = (CGFloat)phase;
	fn_apply_line(r);
}

static void fn_op_gs(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	const char *name = NULL;

	if (CGPDFScannerPopName(s, &name)) {
		fn_apply_extgstate(r, (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "ExtGState", name));
	}
}

static void fn_op_m(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double y = fn_pop_number(r, s);
	double x = fn_pop_number(r, s);

	CGContextMoveToPoint(r->ctx, (CGFloat)x, (CGFloat)y);
}

static void fn_op_l(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double y = fn_pop_number(r, s);
	double x = fn_pop_number(r, s);

	CGContextAddLineToPoint(r->ctx, (CGFloat)x, (CGFloat)y);
}

static void fn_op_c(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double y3 = fn_pop_number(r, s);
	double x3 = fn_pop_number(r, s);
	double y2 = fn_pop_number(r, s);
	double x2 = fn_pop_number(r, s);
	double y1 = fn_pop_number(r, s);
	double x1 = fn_pop_number(r, s);

	CGContextAddCurveToPoint(r->ctx, (CGFloat)x1, (CGFloat)y1, (CGFloat)x2, (CGFloat)y2, (CGFloat)x3,
				 (CGFloat)y3);
}

static void fn_op_v(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double y3 = fn_pop_number(r, s);
	double x3 = fn_pop_number(r, s);
	double y2 = fn_pop_number(r, s);
	double x2 = fn_pop_number(r, s);
	CGPoint current = CGContextGetPathCurrentPoint(r->ctx);

	CGContextAddCurveToPoint(r->ctx, current.x, current.y, (CGFloat)x2, (CGFloat)y2, (CGFloat)x3,
				 (CGFloat)y3);
}

static void fn_op_y(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double y3 = fn_pop_number(r, s);
	double x3 = fn_pop_number(r, s);
	double y1 = fn_pop_number(r, s);
	double x1 = fn_pop_number(r, s);

	CGContextAddCurveToPoint(r->ctx, (CGFloat)x1, (CGFloat)y1, (CGFloat)x3, (CGFloat)y3, (CGFloat)x3,
				 (CGFloat)y3);
}

static void fn_op_h(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	CGContextClosePath(r->ctx);
	(void)s;
}

static void fn_op_re(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double h = fn_pop_number(r, s);
	double w = fn_pop_number(r, s);
	double y = fn_pop_number(r, s);
	double x = fn_pop_number(r, s);

	CGContextAddRect(r->ctx, CGRectMake((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h));
}

static void fn_op_S(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathStroke, false); (void)s; }
static void fn_op_s(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathStroke, true); (void)s; }
static void fn_op_f(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathFill, false); (void)s; }
static void fn_op_fstar(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathEOFill, false); (void)s; }
static void fn_op_B(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathFillStroke, false); (void)s; }
static void fn_op_Bstar(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathEOFillStroke, false); (void)s; }
static void fn_op_b(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathFillStroke, true); (void)s; }
static void fn_op_bstar(CGPDFScannerRef s, void *info) { fn_paint(info, kCGPathEOFillStroke, true); (void)s; }
static void fn_op_n(CGPDFScannerRef s, void *info) { fn_paint(info, -1, false); (void)s; }

static void fn_op_W(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->pending_clip = true;
	r->pending_clip_eo = false;
	(void)s;
}

static void fn_op_Wstar(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->pending_clip = true;
	r->pending_clip_eo = true;
	(void)s;
}

/* THE `/cs` OPERATOR'S SPACE IS AN OBJECT; the shorthand operators set a DEVICE KIND. Both end in the same two
 * fields, so a later `scn` knows how many operands to expect and how to convert them. */
static void fn_set_space(fn_render *r, bool fill, cg_pdf_obj *space)
{
	size_t components;

	if (fn_space_is_pattern(r->doc, space)) {
		if (fill) {
			r->state.fill_kind = FN_SPACE_OBJECT;
			r->state.fill_space = space;
			r->state.fill_pattern_space = true;
			r->state.fill_pattern_obj = NULL;
		} else {
			r->state.stroke_kind = FN_SPACE_OBJECT;
			r->state.stroke_space = space;
			r->state.stroke_pattern_space = true;
			r->state.stroke_pattern_obj = NULL;
		}
		return;
	}
	if (fill) {
		r->state.fill_pattern_space = false;
	} else {
		r->state.stroke_pattern_space = false;
	}
	components = fn_space_components(r->doc, space);

	if (components == 0 && !cg_pdf_name_is(r->doc, space, "Pattern")) {
		fprintf(stderr, "CG-REFUSE: a colour space with no readable component count was not set\n");
		return;
	}
	if (fill) {
		r->state.fill_kind = FN_SPACE_OBJECT;
		r->state.fill_space = space;
	} else {
		r->state.stroke_kind = FN_SPACE_OBJECT;
		r->state.stroke_space = space;
	}
	{
		double zero[4] = { 0, 0, 0, 0 };

		fn_set_components(r, fill, zero, components);
	}
}

static void fn_op_CS(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	const char *name = NULL;

	if (CGPDFScannerPopName(s, &name)) {
		if (strcmp(name, "Pattern") == 0) {
			r->state.stroke_kind = FN_SPACE_OBJECT;
			r->state.stroke_space = NULL;
			r->state.stroke_pattern_space = true;
			r->state.stroke_pattern_obj = NULL;
		} else {
			fn_set_space(r, false,
				     (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "ColorSpace", name));
		}
	}
}

static void fn_op_cs(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	const char *name = NULL;

	if (CGPDFScannerPopName(s, &name)) {
		if (strcmp(name, "Pattern") == 0) {
			r->state.fill_kind = FN_SPACE_OBJECT;
			r->state.fill_space = NULL;
			r->state.fill_pattern_space = true;
			r->state.fill_pattern_obj = NULL;
		} else {
			fn_set_space(r, true,
				     (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "ColorSpace", name));
		}
	}
}

static void fn_pop_components(fn_render *r, CGPDFScannerRef s, bool fill)
{
	size_t count = fill ? fn_space_components(r->doc, r->state.fill_space) :
			      fn_space_components(r->doc, r->state.stroke_space);
	double values[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	cg_pdf_obj *space = fill ? r->state.fill_space : r->state.stroke_space;
	size_t i;

	if (fill ? r->state.fill_pattern_space : r->state.stroke_pattern_space) {
		const char *name = NULL;
		cg_pdf_obj *pattern;

		if (!CGPDFScannerPopName(s, &name)) {
			return;
		}
		pattern = cg_pdf_resolve(r->doc,
					 (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "Pattern", name));
		if (pattern == NULL || pattern->type != kCGPDFObjectTypeStream) {
			if (!r->warned_pattern) {
				fprintf(stderr, "CG-REFUSE: a pattern colour names `%s`, which the page's /Pattern "
						"resources do not define\n", name);
				r->warned_pattern = true;
			}
			return;
		}
		if (fill) {
			r->state.fill_pattern_obj = pattern;
		} else {
			r->state.stroke_pattern_obj = pattern;
		}
		return;
	}
	if (count == 0 || count > 8) {
		return;
	}
	for (i = count; i > 0; i--) {
		values[i - 1] = fn_pop_number(r, s);
	}
	fn_set_components(r, fill, values, count);
}

static void fn_op_SC(CGPDFScannerRef s, void *info) { fn_pop_components(info, s, false); }
static void fn_op_sc(CGPDFScannerRef s, void *info) { fn_pop_components(info, s, true); }
static void fn_op_SCN(CGPDFScannerRef s, void *info) { fn_pop_components(info, s, false); }
static void fn_op_scn(CGPDFScannerRef s, void *info) { fn_pop_components(info, s, true); }

static void fn_set_device(fn_render *r, bool fill, int kind, const double *values)
{
	if (fill) {
		r->state.fill_kind = kind;
	} else {
		r->state.stroke_kind = kind;
	}
	fn_set_components(r, fill, values, kind == FN_SPACE_GRAY ? 1 : (kind == FN_SPACE_RGB ? 3 : 4));
}

static void fn_op_G(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	fn_set_device(r, false, FN_SPACE_GRAY, (double[]){ fn_pop_number(r, s) });
}

static void fn_op_g(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	fn_set_device(r, true, FN_SPACE_GRAY, (double[]){ fn_pop_number(r, s) });
}

static void fn_op_RG(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double b = fn_pop_number(r, s);
	double g = fn_pop_number(r, s);
	double red = fn_pop_number(r, s);

	fn_set_device(r, false, FN_SPACE_RGB, (double[]){ red, g, b });
}

static void fn_op_rg(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double b = fn_pop_number(r, s);
	double g = fn_pop_number(r, s);
	double red = fn_pop_number(r, s);

	fn_set_device(r, true, FN_SPACE_RGB, (double[]){ red, g, b });
}

static void fn_op_K(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double k = fn_pop_number(r, s);
	double y = fn_pop_number(r, s);
	double m = fn_pop_number(r, s);
	double c = fn_pop_number(r, s);

	fn_set_device(r, false, FN_SPACE_CMYK, (double[]){ c, m, y, k });
}

static void fn_op_k(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double k = fn_pop_number(r, s);
	double y = fn_pop_number(r, s);
	double m = fn_pop_number(r, s);
	double c = fn_pop_number(r, s);

	fn_set_device(r, true, FN_SPACE_CMYK, (double[]){ c, m, y, k });
}

static void fn_op_BT(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->state.tm = CGAffineTransformIdentity;
	r->state.tlm = CGAffineTransformIdentity;
	(void)s;
}

static void fn_op_ET(CGPDFScannerRef s, void *info) { (void)s; (void)info; }

static void fn_op_Tf(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFReal size = 0;
	const char *name = NULL;

	if (!CGPDFScannerPopNumber(s, &size) || !CGPDFScannerPopName(s, &name)) {
		return;
	}
	r->state.font = fn_font_for(r, (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "Font", name));
	r->state.font_size = (double)size;
}

static void fn_op_Td(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double ty = fn_pop_number(r, s);
	double tx = fn_pop_number(r, s);

	r->state.tlm = CGAffineTransformConcat(CGAffineTransformMakeTranslation((CGFloat)tx, (CGFloat)ty),
					      r->state.tlm);
	r->state.tm = r->state.tlm;
}

static void fn_op_TD(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double ty = fn_pop_number(r, s);
	double tx = fn_pop_number(r, s);

	r->state.leading = -ty;
	r->state.tlm = CGAffineTransformConcat(CGAffineTransformMakeTranslation((CGFloat)tx, (CGFloat)ty),
					      r->state.tlm);
	r->state.tm = r->state.tlm;
}

static void fn_op_Tm(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	double f = fn_pop_number(r, s);
	double e = fn_pop_number(r, s);
	double d = fn_pop_number(r, s);
	double c = fn_pop_number(r, s);
	double b = fn_pop_number(r, s);
	double a = fn_pop_number(r, s);

	r->state.tm = CGAffineTransformMake((CGFloat)a, (CGFloat)b, (CGFloat)c, (CGFloat)d, (CGFloat)e,
					    (CGFloat)f);
	r->state.tlm = r->state.tm;
}

static void fn_op_Tstar(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->state.tlm = CGAffineTransformConcat(
		CGAffineTransformMakeTranslation(0, (CGFloat)-r->state.leading), r->state.tlm);
	r->state.tm = r->state.tlm;
	(void)s;
}

static void fn_op_Tc(CGPDFScannerRef s, void *info) { ((fn_render *)info)->state.char_spacing = fn_pop_number(info, s); }
static void fn_op_Tw(CGPDFScannerRef s, void *info) { ((fn_render *)info)->state.word_spacing = fn_pop_number(info, s); }
static void fn_op_TL(CGPDFScannerRef s, void *info) { ((fn_render *)info)->state.leading = fn_pop_number(info, s); }
static void fn_op_Ts(CGPDFScannerRef s, void *info) { ((fn_render *)info)->state.rise = fn_pop_number(info, s); }

static void fn_op_Tz(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	r->state.hscale = fn_pop_number(r, s) / 100.0;
}

static void fn_op_Tr(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFInteger value = 0;

	if (CGPDFScannerPopInteger(s, &value)) {
		r->state.render_mode = (int)value;
	}
}

static void fn_op_Tj(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFStringRef string = NULL;

	if (CGPDFScannerPopString(s, &string)) {
		fn_show_string(r, (cg_pdf_obj *)string);
	}
}

static void fn_op_TJ(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFArrayRef array = NULL;

	if (!CGPDFScannerPopArray(s, &array)) {
		return;
	}
	{
		size_t i;
		size_t count = CGPDFArrayGetCount(array);

		for (i = 0; i < count; i++) {
			cg_pdf_obj *item = NULL;

			if (!CGPDFArrayGetObject(array, i, (CGPDFObjectRef *)&item) || item == NULL) {
				continue;
			}
			item = cg_pdf_resolve(r->doc, item);
			if (item == NULL) {
				continue;
			}
			if (item->type == kCGPDFObjectTypeString) {
				fn_show_string(r, item);
			} else if (item->type == kCGPDFObjectTypeInteger || item->type == kCGPDFObjectTypeReal) {
				double adjustment = item->type == kCGPDFObjectTypeInteger ?
						    (double)item->u.integer : item->u.real;
				double tx = -adjustment / 1000.0 * r->state.font_size * r->state.hscale;

				r->state.tm = CGAffineTransformConcat(
					CGAffineTransformMakeTranslation((CGFloat)tx, 0), r->state.tm);
			}
		}
	}
}

static void fn_op_quote(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFStringRef string = NULL;

	fn_op_Tstar(s, info);
	if (CGPDFScannerPopString(s, &string)) {
		fn_show_string(r, (cg_pdf_obj *)string);
	}
}

static void fn_op_dquote(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	CGPDFStringRef string = NULL;
	CGPDFReal ac = 0;
	CGPDFReal aw = 0;

	if (!CGPDFScannerPopString(s, &string) || !CGPDFScannerPopNumber(s, &ac) ||
	    !CGPDFScannerPopNumber(s, &aw)) {
		return;
	}
	r->state.word_spacing = (double)aw;
	r->state.char_spacing = (double)ac;
	fn_op_Tstar(s, info);
	fn_show_string(r, (cg_pdf_obj *)string);
}

static void fn_op_Do(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	const char *name = NULL;

	if (CGPDFScannerPopName(s, &name)) {
		fn_do_xobject(r, name);
	}
}

static void fn_op_sh(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;
	const char *name = NULL;

	if (CGPDFScannerPopName(s, &name)) {
		fn_draw_shading(r, (cg_pdf_obj *)CGPDFContentStreamGetResource(r->content, "Shading", name));
	}
}

static void fn_op_BI(CGPDFScannerRef s, void *info)
{
	fn_render *r = info;

	if (!r->warned_inline) {
		fprintf(stderr, "CG-REFUSE: an inline image (BI ... ID ... EI) is skipped by the content-stream "
				"scanner and is not drawn by this renderer\n");
		r->warned_inline = true;
	}
	(void)s;
}

/* ------------------------------------------------------------------------- */
/* running a content stream                                                  */
/* ------------------------------------------------------------------------- */

static void fn_register(CGPDFOperatorTableRef table, const char *name, CGPDFOperatorCallback callback)
{
	CGPDFOperatorTableSetCallback(table, name, callback);
}

static CGPDFOperatorTableRef fn_operator_table(void)
{
	CGPDFOperatorTableRef table = CGPDFOperatorTableCreate();

	if (table == NULL) {
		return NULL;
	}
	fn_register(table, "q", fn_op_q);
	fn_register(table, "Q", fn_op_Q);
	fn_register(table, "cm", fn_op_cm);
	fn_register(table, "w", fn_op_w);
	fn_register(table, "J", fn_op_J);
	fn_register(table, "j", fn_op_j);
	fn_register(table, "M", fn_op_M);
	fn_register(table, "d", fn_op_d);
	fn_register(table, "gs", fn_op_gs);
	fn_register(table, "m", fn_op_m);
	fn_register(table, "l", fn_op_l);
	fn_register(table, "c", fn_op_c);
	fn_register(table, "v", fn_op_v);
	fn_register(table, "y", fn_op_y);
	fn_register(table, "h", fn_op_h);
	fn_register(table, "re", fn_op_re);
	fn_register(table, "S", fn_op_S);
	fn_register(table, "s", fn_op_s);
	fn_register(table, "f", fn_op_f);
	fn_register(table, "F", fn_op_f);
	fn_register(table, "f*", fn_op_fstar);
	fn_register(table, "B", fn_op_B);
	fn_register(table, "B*", fn_op_Bstar);
	fn_register(table, "b", fn_op_b);
	fn_register(table, "b*", fn_op_bstar);
	fn_register(table, "n", fn_op_n);
	fn_register(table, "W", fn_op_W);
	fn_register(table, "W*", fn_op_Wstar);
	fn_register(table, "CS", fn_op_CS);
	fn_register(table, "cs", fn_op_cs);
	fn_register(table, "SC", fn_op_SC);
	fn_register(table, "sc", fn_op_sc);
	fn_register(table, "SCN", fn_op_SCN);
	fn_register(table, "scn", fn_op_scn);
	fn_register(table, "G", fn_op_G);
	fn_register(table, "g", fn_op_g);
	fn_register(table, "RG", fn_op_RG);
	fn_register(table, "rg", fn_op_rg);
	fn_register(table, "K", fn_op_K);
	fn_register(table, "k", fn_op_k);
	fn_register(table, "BT", fn_op_BT);
	fn_register(table, "ET", fn_op_ET);
	fn_register(table, "Tf", fn_op_Tf);
	fn_register(table, "Td", fn_op_Td);
	fn_register(table, "TD", fn_op_TD);
	fn_register(table, "Tm", fn_op_Tm);
	fn_register(table, "T*", fn_op_Tstar);
	fn_register(table, "Tc", fn_op_Tc);
	fn_register(table, "Tw", fn_op_Tw);
	fn_register(table, "Tz", fn_op_Tz);
	fn_register(table, "TL", fn_op_TL);
	fn_register(table, "Ts", fn_op_Ts);
	fn_register(table, "Tr", fn_op_Tr);
	fn_register(table, "Tj", fn_op_Tj);
	fn_register(table, "TJ", fn_op_TJ);
	fn_register(table, "'", fn_op_quote);
	fn_register(table, "\"", fn_op_dquote);
	fn_register(table, "Do", fn_op_Do);
	fn_register(table, "sh", fn_op_sh);
	fn_register(table, "BI", fn_op_BI);
	return table;
}

static void fn_run_content(fn_render *r, cg_pdf_obj *stream, cg_pdf_obj *resources)
{
	CGPDFContentStreamRef content;
	CGPDFScannerRef scanner;
	CGPDFOperatorTableRef table;
	CGPDFContentStreamRef previous = r->content;
	cg_pdf_obj *previous_resources = r->page_resources;
	bool scanned;

	content = CGPDFContentStreamCreateWithStream((CGPDFStreamRef)stream, (CGPDFDictionaryRef)resources,
						     previous);
	if (content == NULL) {
		return;
	}
	table = fn_operator_table();
	if (table == NULL) {
		CGPDFContentStreamRelease(content);
		return;
	}
	scanner = CGPDFScannerCreate(content, table, r);
	if (scanner != NULL) {
		r->content = content;
		if (resources != NULL) {
			r->page_resources = resources;
		}
		scanned = CGPDFScannerScan(scanner);
		(void)scanned;
		r->content = previous;
		r->page_resources = previous_resources;
		CGPDFScannerRelease(scanner);
	}
	CGPDFOperatorTableRelease(table);
	CGPDFContentStreamRelease(content);
}

/* ------------------------------------------------------------------------- */
/* the public door                                                           */
/* ------------------------------------------------------------------------- */

void CGContextDrawPDFPage(CGContextRef c, CGPDFPageRef page)
{
	cg_pdf_doc *doc;
	CGPDFContentStreamRef content;
	CGPDFScannerRef scanner;
	CGPDFOperatorTableRef table;
	fn_render r;
	size_t i;

	if (c == NULL || page == NULL) {
		return;
	}
	doc = cg_pdf_page_doc(page);
	if (doc == NULL) {
		return;
	}
	if (doc->encrypted) {
		fprintf(stderr, "CG-REFUSE: CGContextDrawPDFPage on an encrypted document — this reader has no "
				"decryption\n");
		return;
	}
	content = CGPDFContentStreamCreateWithPage(page);
	if (content == NULL) {
		return;
	}
	memset(&r, 0, sizeof r);
	r.ctx = c;
	r.doc = doc;
	r.content = content;
	r.page_resources = doc->pages[cg_pdf_page_index(page)].resources;
	fn_state_default(&r.state);
	fn_apply_fill(&r);
	fn_apply_stroke(&r);
	fn_apply_line(&r);

	table = fn_operator_table();
	if (table != NULL) {
		scanner = CGPDFScannerCreate(content, table, &r);
		if (scanner != NULL) {
			CGPDFScannerScan(scanner);
			CGPDFScannerRelease(scanner);
		}
		CGPDFOperatorTableRelease(table);
	}
	for (i = 0; i < r.pattern_count; i++) {
		CGPatternRelease(r.patterns[i].pattern);
	}
	free(r.patterns);
	for (i = 0; i < r.font_count; i++) {
		cg_pdf_font_free(r.fonts[i].font);
	}
	for (i = 0; i < r.icc_count; i++) {
		if (r.icc[i].transform != NULL) {
			cmsDeleteTransform(r.icc[i].transform);
		}
		if (r.icc[i].profile != NULL) {
			cmsCloseProfile(r.icc[i].profile);
		}
		if (r.icc[i].srgb != NULL) {
			cmsCloseProfile(r.icc[i].srgb);
		}
	}
	free(r.icc);
	free(r.fonts);
	free(r.stack);
	CGPDFContentStreamRelease(content);
}
