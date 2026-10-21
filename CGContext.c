/*
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 */
/*
 * CGContext.c — the context, and the polygon→trapezoid fill that C2's drawing is.
 *
 * THE PIPELINE, IN ONE PLACE, because every line below is a step of it:
 *
 *   path (USER space)
 *     → CTM                → device-space edges               [doubles]
 *     → clip to the surface → band sweep at every vertex y    [16.16 fixed]
 *     → TRAPEZOIDS          → pixman_composite_trapezoids     [8-bit or 1-bit mask]
 *     → the surface's bytes
 *
 * WHY PIXMAN'S TRAPEZOID ENGINE AND NOT A FIRST-PARTY RASTERIZER: this tree already
 * rasterizes this way — userland/xfb/fb/fbtrap.c feeds `pixman_composite_trapezoids`
 * exactly this — so the technique is the house stack's, not a new one. And it is the
 * RIGHT one, because a trapezoid is *exact area coverage*: pixman accumulates the
 * area of the trapezoid against each pixel, which is what antialiasing IS, instead of
 * approximating it. The parked toolkit's own instrument measured what the alternative
 * cost (rounded masked shapes at ~3.75 µs/px, with a mask cache hitting 17 times in
 * 103) — that measurement is the argument for this design, and it is a measurement
 * from this project rather than a preference.
 *
 * WHAT THIS FILE DOES NOT DO: it never decides whether the *shape* is right. It
 * decides coverage. Shadows and text are absent (see CGContext.h); gradients, which
 * were on that list from C2 until C6.1, are here now — and they arrived WITHOUT
 * changing this file's geometry at all, because a gradient is a different SOURCE
 * composited through the same trapezoids (see CGPaint_internal.h). That is the shape
 * of the extension: C2's pipeline was right, and what was missing was the colour.
 * The one input the fill REFUSES is... nothing, any more: a path whose edges cross
 * used to be refused because inside a band the sweep assumed the x-order of the
 * active edges was fixed, and the sweep now ends its bands at every crossing as
 * well as at every vertex.
 *
 * THE WINDING NUMBER'S SIGN IS NEVER EXAMINED, only whether it is zero, which is why
 * the default bitmap CTM's y-flip (a mirror, determinant -1) needs no special case:
 * mirroring reverses every winding number and leaves "non-zero" alone.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFont_internal.h>
#include <CoreGraphics/CGColorSpace_internal.h>	/* cg_color_space_is_device_cmyk, cg_device_cmyk_to_rgb */
#include <CoreGraphics/CGContext_internal.h>
#include <CoreGraphics/CGGradient_internal.h>
#include <CoreGraphics/CGPath.h>
#include <CoreGraphics/CGPath_internal.h>
#include <CoreGraphics/CGPaint_internal.h>
#include <CoreGraphics/CGPattern_internal.h>
#include <CoreGraphics/CGShading_internal.h>
#include <CoreGraphics/CGPDFWriter_internal.h>

/* THE RENAME TABLE FOR THE THREE PATH OPERATIONS THAT ARE OURS NOW (2026-10-05) — the public
 * declarations went with the era decision and the code lives under the names in CGPath_internal.h;
 * see CGPath.c for the argument in full. THIS FILE IS WHERE THEY ARE MOSTLY USED: `CGContextClip`,
 * `CGContextStrokePath` and `CGContextSetLineDash` are the 10.0-era verbs built on them, so the
 * table is what keeps a removed NAME from being a removed CAPABILITY. */
#define CGPathCreateCopyByFlattening cg_path_create_flattened_copy
#define CGPathCreateCopyByStrokingPath cg_path_create_stroked_copy
#define CGPathCreateCopyByDashingPath cg_path_create_dashed_copy

#include <math.h>
#include <CoreGraphics/CGImage_internal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pixman-1/pixman.h>

/* ------------------------------------------------------------------------- */
/* the graphics state                                                        */
/* ------------------------------------------------------------------------- */

/* The most dash entries the graphics state will hold. A pattern is a handful of numbers in
 * every real caller, and a BOUNDED ARRAY in the state is what makes save and restore honest:
 * the state is COPIED, so a pointer would be shared between a saved state and its successor
 * and freed by whoever let go of it first. A pattern longer than this is REFUSED rather than
 * truncated, so a caller never gets dashes they did not ask for. */
#define CG_DASH_STATE_MAX 16

typedef struct cg_state {
	CGAffineTransform ctm;
	pixman_region32_t clip;   /* DEVICE space */
	CGFloat rgba[4];          /* not premultiplied */
	/* THE FILL AND STROKE COLOUR SPACES, AS THE SHAPE OF THEIR COMPONENTS RATHER THAN AS OBJECTS:
	 * the component doors need exactly two facts about the current space — HOW MANY components there
	 * are and WHAT THEY MEAN — and a `CGColorSpaceModel` answers both. STORING THE MODEL RATHER THAN
	 * THE SPACE ITSELF is what keeps a retained object out of the graphics state, and therefore keeps
	 * CGContextSaveGState/RestoreGState and the context's release from needing anything they do not
	 * already do: this struct is copied and cleared like every other field. */
	int fill_model;
	int stroke_model;
	CGFloat alpha;
	CGBlendMode blend;
	int antialias;
	/* THE TEXT STATE: four values plus a font. The font is retained and released at the same
	 * five sites the pattern paint is; the matrix defaults to the IDENTITY because a zeroed one
	 * is singular and an untouched context would draw nothing at all. */
	CGFontRef font;
	CGFloat font_size;
	CGAffineTransform text_matrix;
	CGPoint text_position;
	CGFloat character_spacing;
	CGTextDrawingMode text_mode;
	/* THE ENCODING THE BYTE DOORS READ A BYTE WITH, set only by `CGContextSelectFont` — because that is
 	 * the door Apple gives it, and the font-specific value is what `CGContextSetFont` leaves. */
	CGTextEncoding text_encoding;
	/* THE GRAPHICS-STATE HALF OF THE SUBPIXEL PAIR. See the context's `allows_` flag above for why
	 * this one is here and that one is not. */
	int should_subpixel_position_fonts;
	/* AND THE SAME ASYMMETRY FOR SMOOTHING: its `Allows` is the context's and its `Should` is the state's. */
	int should_smooth_fonts;
	/* THE SHADOW, WHICH IS A GRAPHICS-STATE PARAMETER — so Save/Restore carry it for free, the struct
	 * being copied — and whose OFF STATE IS A FULLY TRANSPARENT COLOUR rather than a flag, which is
	 * Apple's own definition: "if `color` is NULL, it's equivalent to specifying a fully transparent
	 * color". An offset, a radius and a colour are all there is. */
	CGSize shadow_offset;
	CGFloat shadow_blur;
	CGFloat shadow_rgba[4];
	/* THE LINE STATE LIVES IN THE GRAPHICS STATE, which is why `CGContextSaveGState` and
	 * `CGContextRestoreGState` needed NO change to carry it: they copy this struct, so
	 * the width, the caps, the joins and the stroke colour are saved and restored with
	 * everything else — and the probe checks exactly that rather than assuming it. */
	CGFloat stroke_rgba[4];   /* not premultiplied */
	CGFloat line_width;
	CGLineCap line_cap;
	CGLineJoin line_join;
	CGFloat miter_limit;
	/* THE PATH FLATNESS, A GRAPHICS-STATE PARAMETER LIKE THE FOUR ABOVE IT and for the same reason:
	 * save and restore copy this struct, so it is carried for free. It is a DEVICE-PIXEL tolerance (see
	 * CGContext.h), and it replaces the fixed 0.1 the fill's flattener used — `cg_traps_for_path` reads
	 * it. The default is stated in `cg_state_init_full` rather than left to the calloc that would make
	 * it 0, which would mean "subdivide until the recursion bottoms out". */
	CGFloat flatness;
	/* AND SO DOES THE DASH PATTERN, as an array for the reason above. THE CONTEXT IS
	 * calloc'd, which is what makes a zero `dash_count` mean SOLID: there is no initializer
	 * to keep in step with this struct, and a state that was never set cannot be garbage. */
	CGFloat dash[CG_DASH_STATE_MAX];
	int dash_count;
	CGFloat dash_phase;
	/* HOW `CGContextDrawImage` SAMPLES WHEN IT SCALES. See CGContext.h for why `kCGInterpolationNone`
	 * is 0 and why that ordering is a decision rather than a habit. */
	CGInterpolationQuality interpolation;
	/* THE CLIP'S MASK HALF, OR NULL. The region above is the RECTANGULAR part of the clip and this is
	 * the rest of it: an 8-bit coverage image, built when a clip path has a slanted or curved edge. It is
	 * IMMUTABLE once built — a further clip multiplies into a NEW image — so save and restore may share it
	 * by refcount, the way the pattern is handled. IT IS REFCOUNTED, so the four lifetime sites below each
	 * know about it; a state that copied the field without retaining would be a double release.
	 *
	 * AND IT CARRIES ITS OWN ORIGIN NOW (§59). The image used to be the WHOLE SURFACE with device
	 * coordinates equal to its own indices, which made every reader's arithmetic free and made a CURVED
	 * PATH CLIP allocate the picture: §56 measured 0.5 ms for a 60x40 clip at 1280x800 and 5.1 ms for one
	 * covering it, with the SURFACE as the term that does not move with the shape. THE BOX IS WHERE THE
	 * MASK IS VALID, and a device pixel outside it reads as coverage 0 — which is what a path clip IS
	 * outside its path, and is why boxing that one changes no pixel. IT IS NOT WHY EVERY MASK CAN BE
	 * BOXED: `CGContextClipToMask`'s mask holds 1 outside the rectangle it was given, so its box is the
	 * surface and always will be. A mask is only ever 0 outside its box; the box says where it is defined,
	 * not what it holds. */
	pixman_image_t *clip_mask;
	int clip_mask_x;
	int clip_mask_y;
	/* THE PATTERN PAINT, AND IT IS THE ONE THING IN THIS STATE THAT IS NOT A VALUE. Everything above
	 * copies by assignment, which is why it is all numbers and bounded arrays; a pattern is an object,
	 * so the copy is not free and the rules are stated where they are needed: `CGContextSaveGState`
	 * RETAINS into the saved slot, `CGContextRestoreGState` RELEASES the state it is replacing, and
	 * `CGContextRelease` releases every slot it still holds. A struct copy that did none of those
	 * would have two states releasing one pattern. */
	CGPatternRef fill_pattern;
	CGPatternRef stroke_pattern;
	CGFloat fill_pattern_alpha;
	CGFloat stroke_pattern_alpha;
	CGSize pattern_phase;
} cg_state;

struct CGContext {
	int refcount;
	int is_bitmap;
	/* NON-NULL FOR A PDF CONTEXT: the recorder this context's drawing calls are said to instead of being
	 * rasterized. A PDF context still has a 1x1 pixman surface so the dozen doors that ask `c->data` keep
	 * their ordinary shape; every painting door checks this pointer before touching it. */
	struct cg_pdf_writer *writer;
	/* THE CALLER'S RELEASE CALLBACK AND ITS info FIELD, or NULL. Apple's header says the callback is
	 * called when the context is freed, with releaseInfo and data as arguments -- and THE DATA IS STILL
	 * THE CONTEXT'S TO FREE IF IT ALLOCATED IT, because the callback and the free are two promises. */
	CGBitmapContextReleaseDataCallback release_data;
	void *release_info;

	pixman_image_t *image;
	unsigned char *data;
	int owns_data;
	int width;
	int height;
	int stride;

	CGColorSpaceRef space;
	uint32_t bitmap_info;

	int allows_antialiasing;
	/* THE TWO "ALLOWS" FONT FLAGS LIVE ON THE CONTEXT, NOT IN THE GRAPHICS STATE — Apple's own
	 * comment on those three setters says "this parameter is not part of the graphics state" — while
	 * their `Should` twins ARE in the state below. THAT ASYMMETRY IS THE INTERFACE, and the probe
	 * checks it by saving, turning the state flag off, drawing, and restoring. */
	int allows_subpixel_positioning;
	/* AND SMOOTHING'S HALF OF THE SAME PAIR — sub-pixel ANTIALIASING, three samples to the pixel, which is a
	 * different rendering path from sub-pixel POSITIONING and governed by its own two flags. */
	int allows_font_smoothing;
	cg_state state;
	cg_state *stack;
	int depth;
	/* THE OPEN TRANSPARENCY LAYERS, as a chain rather than a fixed array because Apple's doors nest as
	 * deep as a caller likes. Each entry holds THE SURFACE IT REPLACED — data, stride, pixman image and
	 * who owned the bytes — plus THE OUTER ALPHA AND BLEND, which are read when the layer BEGINS because
	 * the group's own state has them set to 1 and normal. */
	struct cg_group *groups;
	int stack_cap;

	struct CGPath *path;
};

/* THE RECORDER'S VIEW OF THE GRAPHICS STATE, taken at the moment something is painted. The writer emits it
 * with the operator rather than mirroring every setter, which is what keeps its content stream in step with
 * `q`/`Q` without a second state machine. */
static void fn_pdf_paint_state(CGContextRef c, cg_pdf_paint_state *ps)
{
	size_t i;

	memset(ps, 0, sizeof *ps);
	for (i = 0; i < 4; i++) {
		ps->fill[i] = c->state.rgba[i];
		ps->stroke[i] = c->state.stroke_rgba[i];
	}
	ps->alpha = c->state.alpha;
	ps->blend = c->state.blend;
	ps->line_width = c->state.line_width;
	ps->miter_limit = c->state.miter_limit;
	ps->cap = (int)c->state.line_cap;
	ps->join = (int)c->state.line_join;
	ps->dash_count = (size_t)c->state.dash_count;
	for (i = 0; i < ps->dash_count && i < 16; i++) {
		ps->dash[i] = c->state.dash[i];
	}
	ps->dash_phase = c->state.dash_phase;
}

/* FORWARD, BECAUSE THE COLOUR SETTERS COME FIRST IN THIS FILE AND THE PATTERN THEY MUST CLEAR IS
 * DEFINED WITH THE REST OF C6.3'S PAINT CODE, well below them. The rule it implements — a later paint
 * replaces an earlier one of the other kind — is why the four component colour setters need it at
 * all; see `CGContextSetGrayFillColor`. */
static void cg_clear_pattern(CGPatternRef *slot);

/* ------------------------------------------------------------------------- */
/* small arithmetic                                                          */
/* ------------------------------------------------------------------------- */

/* THE CLAMP MOVED TO CGPaint.c WITH THE PREMULTIPLY in C6.1: the only caller here was the colour
 * packer, and it now lives beside it, so this file no longer clamps anything. */

/* 16.16 FIXED POINT IS PIXMAN'S OWN UNIT and the conversion is ours: pixman.h may
 * declare a `pixman_double_to_fixed`, but this file does not depend on which rounding
 * a library version chose for a value smaller than 1/65536 of a pixel — no assertion
 * anywhere in this tree can see that difference, and a helper that says so is better
 * than an implicit dependency that says nothing. */
static pixman_fixed_t cg_fixed(double v)
{
	if (v <= -32768.0) {
		return (pixman_fixed_t)(-32768 * 65536);
	}
	if (v >= 32767.0) {
		return (pixman_fixed_t)(32767 * 65536);
	}
	return (pixman_fixed_t)(v * 65536.0 + (v >= 0.0 ? 0.5 : -0.5));
}

/* THE PREMULTIPLY MOVED TO CGPaint.c IN C6.1, and the reason is C6's own: a gradient, a shading and
 * a pattern each need to pack a SAMPLED colour into a pixel word, and so does a solid fill. Keeping
 * this here would have meant either a second copy of the arithmetic for the paints or an exported
 * symbol anyway — so it is one function in one place (CGPaint_internal.h declares it), and this
 * comment is what is left of the version that lived here. See CGPaint.c for the format note. */

/* THE ONE BLEND MODE THAT IS NOT A PIXMAN OPERATOR. Plus-darker has no pixman counterpart (CGContext.h
 * says why), so `cg_op` answers with this sentinel and every composite that can carry a blend checks
 * for it and takes the hand-written path rather than handing it to pixman. IT IS A `pixman_op_t` SO IT
 * FLOWS THROUGH THE SAME `op` PARAMETER every paint already threads — colour, pattern, text, gradient
 * and shading alike — and it is a value pixman never defines, so a mistake that let it reach pixman
 * would be a loud one instead of a silent mispaint. */
#define CG_OP_PLUS_DARKER ((pixman_op_t)0x7fffffff)

static pixman_op_t cg_op(CGBlendMode mode)
{
	switch (mode) {
	case kCGBlendModeMultiply: return PIXMAN_OP_MULTIPLY;
	case kCGBlendModeScreen: return PIXMAN_OP_SCREEN;
	case kCGBlendModeOverlay: return PIXMAN_OP_OVERLAY;
	case kCGBlendModeDarken: return PIXMAN_OP_DARKEN;
	case kCGBlendModeLighten: return PIXMAN_OP_LIGHTEN;
	case kCGBlendModeColorDodge: return PIXMAN_OP_COLOR_DODGE;
	case kCGBlendModeColorBurn: return PIXMAN_OP_COLOR_BURN;
	case kCGBlendModeSoftLight: return PIXMAN_OP_SOFT_LIGHT;
	case kCGBlendModeHardLight: return PIXMAN_OP_HARD_LIGHT;
	case kCGBlendModeDifference: return PIXMAN_OP_DIFFERENCE;
	case kCGBlendModeExclusion: return PIXMAN_OP_EXCLUSION;
	case kCGBlendModeHue: return PIXMAN_OP_HSL_HUE;
	case kCGBlendModeSaturation: return PIXMAN_OP_HSL_SATURATION;
	case kCGBlendModeColor: return PIXMAN_OP_HSL_COLOR;
	case kCGBlendModeLuminosity: return PIXMAN_OP_HSL_LUMINOSITY;
	case kCGBlendModeClear: return PIXMAN_OP_CLEAR;
	case kCGBlendModeCopy: return PIXMAN_OP_SRC;
	case kCGBlendModeSourceIn: return PIXMAN_OP_IN;
	case kCGBlendModeSourceOut: return PIXMAN_OP_OUT;
	case kCGBlendModeSourceAtop: return PIXMAN_OP_ATOP;
	/* THE REVERSE NAMES ARE PIXMAN'S SPELLING OF THE DESTINATION-SIDE OPERATORS, and the
	 * mapping is semantic rather than a spelling match: pixman's `OVER` means
	 * `src OVER dst`, so the PDF — and CG — operator that composites the SOURCE UNDER
	 * the destination, `kCGBlendModeDestinationOver`, is `OVER_REVERSE`. The same shift
	 * applies to the other three `Destination*` cases. */
	case kCGBlendModeDestinationOver: return PIXMAN_OP_OVER_REVERSE;
	case kCGBlendModeDestinationIn: return PIXMAN_OP_IN_REVERSE;
	case kCGBlendModeDestinationOut: return PIXMAN_OP_OUT_REVERSE;
	case kCGBlendModeDestinationAtop: return PIXMAN_OP_ATOP_REVERSE;
	case kCGBlendModeXOR: return PIXMAN_OP_XOR;
	case kCGBlendModePlusLighter: return PIXMAN_OP_ADD;
	case kCGBlendModePlusDarker: return CG_OP_PLUS_DARKER;
	case kCGBlendModeNormal:
	default: return PIXMAN_OP_OVER;
	}
}

/* ------------------------------------------------------------------------- */
/* the bitmap context                                                        */
/* ------------------------------------------------------------------------- */

static void cg_state_init_full(cg_state *st, int width, int height)
{
	st->ctm = CGAffineTransformMake(1.0, 0.0, 0.0, -1.0, 0.0, (CGFloat)height);
	pixman_region32_init_rect(&st->clip, 0, 0, (unsigned int)width, (unsigned int)height);
	st->rgba[0] = 0.0;
	st->rgba[1] = 0.0;
	st->rgba[2] = 0.0;
	st->rgba[3] = 1.0;
	st->alpha = 1.0;
	/* DEVICE RGB IS THE DEFAULT COLOUR SPACE for both, which is what the component doors assume when
	 * nobody has set one, and what `CGContextSetRGBFillColor` has always written. STATED HERE for the
	 * reason the dash pattern's zero is: a default that depends on calloc is a default nobody chose. */
	/* A FULLY TRANSPARENT SHADOW: no shadow until somebody sets one, stated here rather than left to the
	 * calloc that happens to zero it. */
	st->shadow_offset = CGSizeMake(0.0, 0.0);
	st->shadow_blur = 0.0;
	st->shadow_rgba[0] = 0.0;
	st->shadow_rgba[1] = 0.0;
	st->shadow_rgba[2] = 0.0;
	st->shadow_rgba[3] = 0.0;
	st->fill_model = kCGColorSpaceModelRGB;
	st->stroke_model = kCGColorSpaceModelRGB;
	st->blend = kCGBlendModeNormal;
	st->antialias = 1;
	/* STATED RATHER THAN LEFT TO THE calloc THAT HAPPENS TO ZERO IT, for the reason the pattern
	 * fields are stated too: a default that depends on an enumerator's position is a default nobody
	 * chose, and this one is the library's PRE-EXISTING behaviour held on purpose (see CGContext.h). */
	st->interpolation = kCGInterpolationNone;
	/* AND SMOOTHING IS WANTED BY DEFAULT, pairing with the context's `allows_font_smoothing`. The two are read
	 * together in `cg_font_smoothing`, whose third term is the destination's own opacity. */
	st->should_smooth_fonts = 1;
	st->clip_mask = NULL;
	st->clip_mask_x = 0;
	st->clip_mask_y = 0;
	/* THE PATH FLATNESS'S DEFAULT, AND IT IS THE VALUE THE FILL'S FLATTENER ALREADY USED: 0.1 device
	 * pixels, `cg_traps_for_path`'s fixed number before this parameter existed. So a caller who never
	 * calls `CGContextSetFlatness` gets exactly the pixels it has always got, and the parameter is a door
	 * onto a number that was already load-bearing rather than a new knob with a new default. 0.0 WOULD BE
	 * A DIFFERENT MEANING — "subdivide forever" — which is why it is stated here and not left to calloc. */
	st->flatness = 0.1;
	/* APPLE'S DOCUMENTED DEFAULTS for the line state: width 1, butt caps, miter joins,
	 * miter limit 10. A zero default for the width would be a stroke that draws nothing
	 * — the failure a caller who sets everything explicitly never sees and everyone else
	 * does — and the stroke colour defaults to black, like the fill. */
	st->stroke_rgba[0] = 0.0;
	st->stroke_rgba[1] = 0.0;
	st->stroke_rgba[2] = 0.0;
	st->stroke_rgba[3] = 1.0;
	st->line_width = 1.0;
	st->line_cap = kCGLineCapButt;
	st->line_join = kCGLineJoinMiter;
	st->miter_limit = 10.0;
	/* NO PATTERN IS SET, and this is stated rather than left to the calloc that happens to zero the
	 * context: a paint with no pattern is a colour, and a garbage pointer here would be a fill that
	 * sampled a cell nobody drew. */
	st->fill_pattern = NULL;
	st->stroke_pattern = NULL;
	st->fill_pattern_alpha = 1.0;
	st->stroke_pattern_alpha = 1.0;
	st->font = NULL;
	st->font_size = 0.0;
	st->text_matrix = CGAffineTransformIdentity;
	st->text_position = CGPointMake(0.0, 0.0);
	st->character_spacing = 0.0;
	st->text_mode = kCGTextFill;
	st->text_encoding = kCGEncodingFontSpecific;
	st->should_subpixel_position_fonts = 1;
	st->pattern_phase = CGSizeMake(0.0, 0.0);
}

/* THE OLD DOOR, WHICH IS NOW THE NEW ONE WITH NO CALLBACK: one road, one set of validations. */
CGContextRef CGBitmapContextCreate(void *data, size_t width, size_t height, size_t bits_per_component,
				   size_t bytes_per_row, CGColorSpaceRef space, uint32_t bitmap_info)
{
	return CGBitmapContextCreateWithData(data, width, height, bits_per_component, bytes_per_row, space,
					     bitmap_info, NULL, NULL);
}

/* THE TWO BYTE ORDERS A CONTEXT CAN SPEAK, AND WHY EXACTLY THOSE TWO.
 *
 * `kCGBitmapByteOrder32Big` means the 32-bit word is stored big-endian and the component names read
 * most-significant first, so `PremultipliedLast | 32Big` is R, G, B, A in memory; `PremultipliedFirst |
 * 32Little` is B, G, R, A. THOSE ARE NOT ARBITRARY: they are the two conventions this tree already had,
 * one on each side of a seam that used to disagree — NSBitmapImageRep builds its CGImage declaring its
 * bytes as R, G, B, A, while every canvas CoreGraphics made was B, G, R, A. A context that could speak
 * only one of them forced the other side to be wrong, and NSImage's focus pair is where that hurt.
 *
 * THE OTHER TWO COMBINATIONS HAVE NO PIXMAN FORMAT HERE and stay refused by name: a word stored
 * little-endian with the alpha last is A, B, G, R in memory, and pixman has no such order. REFUSING IS THE
 * POINT — a context whose bytes mean something other than what the caller asked for would corrupt whatever
 * it touched, which is this file's oldest rule. */
static void cg_channel_slots(uint32_t bitmap_info, int slot[3]);

static pixman_format_code_t cg_context_format(uint32_t bitmap_info)
{
	uint32_t alpha = bitmap_info & kCGBitmapAlphaInfoMask;
	uint32_t order = bitmap_info & kCGBitmapByteOrderMask;

	if (alpha == kCGImageAlphaPremultipliedFirst && order == kCGBitmapByteOrder32Little) {
		return PIXMAN_a8r8g8b8;		/* memory B, G, R, A */
	}
	if (alpha == kCGImageAlphaPremultipliedLast && order == kCGBitmapByteOrder32Big) {
		return PIXMAN_a8b8g8r8;		/* memory R, G, B, A */
	}
	/* **AND THE TWO OPAQUE FORMATS, WHICH ARE WHAT SUB-PIXEL ANTIALIASING NEEDS ITS DESTINATION TO BE.** They
	 * carry no alpha channel at all, which is exactly the property `cg_font_smoothing` asks for, and pixman's
	 * `x` formats are the same words with the fourth byte NOT WRITTEN — so a context made this way keeps its
	 * three colour channels and never grows a transparency it did not have. */
	if (alpha == kCGImageAlphaNoneSkipFirst && order == kCGBitmapByteOrder32Little) {
		return PIXMAN_x8r8g8b8;		/* memory B, G, R, X */
	}
	if (alpha == kCGImageAlphaNoneSkipLast && order == kCGBitmapByteOrder32Big) {
		return PIXMAN_x8b8g8r8;		/* memory R, G, B, X */
	}
	return (pixman_format_code_t)0;
}

/* **WHERE RED, GREEN AND BLUE SIT IN A DESTINATION'S MEMORY, DECIDED IN ONE PLACE AND ASKED EVERYWHERE.**
 * The two orders this library answers are `kCGBitmapByteOrder32Little` with a component FIRST — memory
 * B, G, R, X/A — and `kCGBitmapByteOrder32Big` with one LAST — memory R, G, B, X/A. THE POSITION OF THE ALPHA
 * (OR OF THE SKIPPED COMPONENT) IS WHAT DECIDES THE COLOUR ORDER, and the OPAQUE spelling of a layout keeps
 * that layout's order: `NoneSkipFirst` is B, G, R as surely as `PremultipliedFirst` is.
 *
 * **AND THAT IS A CORRECTION, WITH A MEASUREMENT BEHIND IT.** This decision used to be spelled out at each
 * writer as "`PremultipliedLast` means R, G, B, everything else means B, G, R" — which was true while those were
 * the only two formats a context could be made in. The moment the OPAQUE spelling of each was answered, two of
 * those writers were wrong about `NoneSkipLast` (R, G, B, called B, G, R) and the third asked pixman instead —
 * `dest == PIXMAN_a8r8g8b8`, which the window's `NoneSkipFirst` surface is NOT, so MEASURED the window chrome's
 * close button came out (86, 80, 242): the theme's `#F25056` with red and blue swapped. */
static void cg_channel_slots(uint32_t bitmap_info, int slot[3])
{
	uint32_t alpha = bitmap_info & kCGBitmapAlphaInfoMask;
	int last = (alpha == kCGImageAlphaPremultipliedLast || alpha == kCGImageAlphaLast
		    || alpha == kCGImageAlphaNoneSkipLast);

	slot[0] = last ? 0 : 2;		/* red */
	slot[1] = 1;			/* green */
	slot[2] = last ? 2 : 0;		/* blue */
}

CGContextRef CGBitmapContextCreateWithData(void *data, size_t width, size_t height,
				   size_t bits_per_component, size_t bytes_per_row,
				   CGColorSpaceRef space, uint32_t bitmap_info,
				   CGBitmapContextReleaseDataCallback releaseCallback, void *releaseInfo)
{
	CGContextRef c;
	size_t stride;
	uint32_t alpha;
	uint32_t order;

	/* REFUSALS BY NAME, WITH NULL, AND NULL IS APPLE'S OWN ANSWER for a bad
	 * `CGBitmapContextCreate` — a real failure channel, so this is a refusal rather
	 * than a diagnostic. The unsupported shapes are not approximated: a context whose
	 * bytes mean something other than what the caller asked for would corrupt
	 * whatever it touched. */
	alpha = bitmap_info & kCGBitmapAlphaInfoMask;
	order = bitmap_info & kCGBitmapByteOrderMask;
	if (width == 0 || height == 0 || bits_per_component != 8) {
		fprintf(stderr, "CG-REFUSE: CGBitmapContextCreate needs 8 bits per component "
				"and a non-empty size\n");
		return NULL;
	}
	if (cg_context_format(bitmap_info) == 0) {
		fprintf(stderr, "CG-REFUSE: CGBitmapContextCreate supports four 8-bit orders, each "
				"in an alpha and an OPAQUE spelling: kCGImageAlphaPremultipliedFirst | "
				"kCGBitmapByteOrder32Little (memory B,G,R,A, every canvas this tree made "
				"before) and kCGImageAlphaNoneSkipFirst with it; and "
				"kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big (memory R,G,B,A, "
				"which is what an NSBitmapImageRep's own CGImage declares) and "
				"kCGImageAlphaNoneSkipLast with it. The opaque pair is what FONT SMOOTHING "
				"needs a destination to be, and the remaining orders have no pixman format "
				"here\n");
		(void)alpha;
		(void)order;
		return NULL;
	}
	if (space != NULL && CGColorSpaceGetModel(space) != kCGColorSpaceModelRGB) {
		fprintf(stderr, "CG-REFUSE: CGBitmapContextCreate supports a device RGB space\n");
		return NULL;
	}
	stride = bytes_per_row ? bytes_per_row : width * 4;
	if (stride < width * 4) {
		fprintf(stderr, "CG-REFUSE: CGBitmapContextCreate given a row shorter than "
				"its width\n");
		return NULL;
	}

	c = calloc(1, sizeof(struct CGContext));
	if (c == NULL) {
		return NULL;
	}
	c->owns_data = (data == NULL);
	if (c->owns_data) {
		data = calloc(1, stride * height);
		if (data == NULL) {
			free(c);
			return NULL;
		}
	}
	c->data = data;
	c->stride = (int)stride;
	c->width = (int)width;
	c->height = (int)height;
	c->is_bitmap = 1;
	c->release_data = releaseCallback;
	c->release_info = releaseInfo;
	c->space = CGColorSpaceRetain(space);
	c->bitmap_info = bitmap_info;
	c->allows_antialiasing = 1;
	/* APPLE'S RULE FOR SUBPIXEL POSITIONING IS AN AND OF FOUR THINGS, and all four exist here: the
	 * two flags the header names (the context's and the state's) and the two antialiasing settings
	 * its comment adds ("fonts will be antialiased when drawn"). Written out rather than
	 * abbreviated, because each term is a door somebody can close. */
	c->allows_subpixel_positioning = 1;
	/* **AND SMOOTHING IS ALLOWED BY DEFAULT, WHICH IS WHAT MAKES THE DESKTOP'S TEXT LOOK LIKE THE MAC'S.** This
	 * tier's AppKit never calls these doors, so a default of "off" would mean a renderer that can and never
	 * does; the destination has the last word in `cg_font_smoothing` below, where a context with an alpha
	 * channel never gets colour coverage. */
	c->allows_font_smoothing = 1;
	c->refcount = 1;
	cg_state_init_full(&c->state, (int)width, (int)height);
	c->path = (struct CGPath *)CGPathCreateMutable();
	c->image = pixman_image_create_bits(cg_context_format(c->bitmap_info), c->width, c->height,
					    (uint32_t *)c->data, c->stride);
	if (c->image == NULL) {
		CGContextRelease(c);
		return NULL;
	}
	return c;
}

static void fn_release_snapshot(void *info, const void *data, size_t size);

CGImageRef CGBitmapContextCreateImage(CGContextRef c)
{
	CGDataProviderRef provider;
	CGImageRef image;
	void *snapshot;
	size_t size;

	if (c == NULL || !c->is_bitmap || c->data == NULL || c->width <= 0 || c->height <= 0) {
		return NULL;
	}
	size = (size_t)c->stride * (size_t)c->height;
	/* A COPY, BECAUSE THAT IS WHAT THE DOOR PROMISES: "subsequent changes to context will not affect the
	 * contents of the returned image". The provider owns the copy and frees it. */
	snapshot = malloc(size);
	if (snapshot == NULL) {
		return NULL;
	}
	memcpy(snapshot, c->data, size);
	provider = CGDataProviderCreateWithData(snapshot, snapshot, size, fn_release_snapshot);
	if (provider == NULL) {
		free(snapshot);
		return NULL;
	}
	/* THE CONTEXT'S OWN CHART AND SPACE, because the image is a picture of THIS surface: the chart is the
	 * one `CGImageCreate` reads, which is why the context's `bitmap_info` is what it is. */
	image = CGImageCreate((size_t)c->width, (size_t)c->height, 8, 32, (size_t)c->stride, c->space,
			      c->bitmap_info, provider, NULL, false, kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	return image;
}

/* THE SNAPSHOT'S OWNER: the provider hands the copy back to nobody, so this frees it. */
static void fn_release_snapshot(void *info, const void *data, size_t size)
{
	(void)info;
	(void)size;
	free((void *)data);
}

void *CGBitmapContextGetData(CGContextRef c)
{
	return (c != NULL && c->is_bitmap) ? c->data : NULL;
}

size_t CGBitmapContextGetWidth(CGContextRef c)
{
	return (c != NULL && c->is_bitmap) ? (size_t)c->width : 0;
}

size_t CGBitmapContextGetHeight(CGContextRef c)
{
	return (c != NULL && c->is_bitmap) ? (size_t)c->height : 0;
}

size_t CGBitmapContextGetBytesPerRow(CGContextRef c)
{
	return (c != NULL && c->is_bitmap) ? (size_t)c->stride : 0;
}

size_t CGBitmapContextGetBitsPerComponent(CGContextRef c)
{
	return (c != NULL && c->is_bitmap) ? 8 : 0;
}

size_t CGBitmapContextGetBitsPerPixel(CGContextRef c)
{
	return (c != NULL && c->is_bitmap) ? 32 : 0;
}

CGImageAlphaInfo CGBitmapContextGetAlphaInfo(CGContextRef c)
{
	return (c != NULL) ? (CGImageAlphaInfo)(c->bitmap_info & 0x1Fu)
			   : kCGImageAlphaNoneSkipLast;
}

uint32_t CGBitmapContextGetBitmapInfo(CGContextRef c)
{
	return (c != NULL) ? c->bitmap_info : 0;
}

CGColorSpaceRef CGBitmapContextGetColorSpace(CGContextRef c)
{
	return (c != NULL) ? c->space : NULL;
}

/* ------------------------------------------------------------------------- */
/* lifetime                                                                  */
/* ------------------------------------------------------------------------- */

CGContextRef CGContextRetain(CGContextRef c)
{
	if (c != NULL) {
		c->refcount++;
	}
	return c;
}

struct cg_group {
	struct cg_group *next;
	unsigned char *saved_data;
	int saved_stride;
	pixman_image_t *saved_image;
	int saved_owns_data;
	unsigned char *data;		/* the group's own surface: OURS */
	pixman_image_t *image;
	CGFloat alpha;			/* the OUTER alpha, applied once at the end */
	CGBlendMode blend;		/* and the outer blend, which must be normal (see CGContext.h) */
};

/* THE WAY BACK, WITH OR WITHOUT A COMPOSITE: `composite` is what `CGContextEndTransparencyLayer` asks
 * for, and the release path asks for the other, because a context freed with a layer still open has to
 * let go of the group's surface and put the outer one back before the final free looks at it. */
static void cg_group_pop(CGContextRef c, int composite);

void CGContextRelease(CGContextRef c)
{
	int i;

	if (c == NULL || c->refcount == 0) {
		return;
	}
	if (--c->refcount > 0) {
		return;
	}
	for (i = 0; i < c->depth; i++) {
		/* A CONTEXT'S DEATH IS THE OTHER PLACE A PATTERN'S `releaseInfo` CAN FIRE, so every saved
		 * slot is walked as well as the live state: a caller who saved a state, set a pattern and
		 * released the context without restoring would otherwise leak the pattern AND never see
		 * `releaseInfo` called. */
		CGPatternRelease(c->stack[i].fill_pattern);
		CGPatternRelease(c->stack[i].stroke_pattern);
		CGFontRelease(c->stack[i].font);
		if (c->stack[i].clip_mask != NULL) {
			pixman_image_unref(c->stack[i].clip_mask);
		}
		pixman_region32_fini(&c->stack[i].clip);
	}
	free(c->stack);
	CGPatternRelease(c->state.fill_pattern);
	CGPatternRelease(c->state.stroke_pattern);
	CGFontRelease(c->state.font);
	if (c->state.clip_mask != NULL) {
		pixman_image_unref(c->state.clip_mask);
	}
	pixman_region32_fini(&c->state.clip);
	if (c->image != NULL) {
		pixman_image_unref(c->image);
	}
	if (c->path != NULL) {
		CGPathRelease((CGPathRef)c->path);
	}
	CGColorSpaceRelease(c->space);
	/* THE CALLER'S CALLBACK, LAST OF ALL AND WITH THE DATA STILL VALID: a block freed before the callback
	 * that was told about it would be a callback nobody could use. */
	if (c->release_data != NULL) {
		c->release_data(c->release_info, c->data);
	}
	/* AN UNCLOSED TRANSPARENCY LAYER IS NOT A REFUSAL AT RELEASE — the context is going away either way —
	 * but its surfaces must be freed and the OUTER surface put back first, or this last free would act on
	 * a group's buffer while the group's own record still pointed at the real one. */
	while (c->groups != NULL) {
		cg_group_pop(c, 0);
	}
	if (c->owns_data) {
		free(c->data);
	}
	if (c->writer != NULL) {
		cg_pdf_writer_free(c->writer);
		c->writer = NULL;
	}
	free(c);
}

/* ------------------------------------------------------------------------- */
/* a context with no surface                                                 */
/* ------------------------------------------------------------------------- */

/* **THE PDF CONTEXT IS A `CGContext` WHOSE SURFACE IS ONE PIXEL AND NEVER RECEIVED.** The surface exists because
 * `cg_text_can_draw` and `CGContextDrawImage` ask `c->data` whether there is anywhere to draw, and a context that
 * answered "no" would refuse the text a PDF context is FOR. Every painting door checks `c->writer` first and
 * records; the 1x1 image is the fallback that turns a missed hook into a discarded pixel rather than a crash. */
CGContextRef cg_context_create_pdf(struct cg_pdf_writer *writer)
{
	CGContextRef c;

	if (writer == NULL) {
		return NULL;
	}
	c = calloc(1, sizeof *c);
	if (c == NULL) {
		return NULL;
	}
	c->refcount = 1;
	c->is_bitmap = 0;
	c->writer = writer;
	c->width = 1;
	c->height = 1;
	c->stride = 4;
	cg_state_init_full(&c->state, 1, 1);
	c->state.ctm = CGAffineTransformIdentity;
	c->path = CGPathCreateMutable();
	c->allows_antialiasing = 1;
	c->allows_subpixel_positioning = 1;
	c->allows_font_smoothing = 1;
	c->state.should_subpixel_position_fonts = 1;
	c->state.should_smooth_fonts = 1;
	c->owns_data = 1;
	c->data = calloc(1, 4);
	if (c->path == NULL || c->data == NULL) {
		if (c->path != NULL) {
			CGPathRelease((CGPathRef)c->path);
		}
		free(c->data);
		free(c);
		return NULL;
	}
	c->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, (uint32_t *)c->data, 4);
	if (c->image == NULL) {
		CGPathRelease((CGPathRef)c->path);
		free(c->data);
		free(c);
		return NULL;
	}
	return c;
}

struct cg_pdf_writer *cg_context_pdf_writer(CGContextRef c)
{
	return c != NULL ? c->writer : NULL;
}

void CGContextFlush(CGContextRef c)
{
	/* NOTHING TO DO, AND THAT IS THE TRUTH rather than a stub: a bitmap context's
	 * drawing goes into `CGBitmapContextGetData` as it is performed, so at the moment
	 * this returns the bytes are current. It validates its argument so a NULL is not
	 * silently tolerated. */
	(void)c;
}

void CGContextSynchronize(CGContextRef c)
{
	(void)c;
}

/* ------------------------------------------------------------------------- */
/* the graphics state                                                        */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Transparency layers                                                         */
/* ------------------------------------------------------------------------- */

static void cg_group_pop(CGContextRef c, int composite)
{
	struct cg_group *g = c->groups;
	CGFloat outer_alpha;
	CGBlendMode outer_blend;

	if (g == NULL) {
		return;
	}
	outer_alpha = g->alpha;
	outer_blend = g->blend;
	c->groups = g->next;
	/* THE COMPOSITE HAPPENS OVER THE WHOLE SURFACE AND IS BOUNDED BY THE CLIP, which the paint path below
	 * already relies on: `pixman_image_set_clip_region32` is what makes "this operation respects the
	 * clipping region" true, for the group and for every shape drawn inside it. THE OUTER ALPHA IS A 1x1
	 * MASK SOURCE, the same shape a solid colour's composite uses. */
	if (composite) {
		pixman_image_t *mask;
		unsigned char level = (unsigned char)(outer_alpha <= 0.0 ? 0
						      : (outer_alpha >= 1.0 ? 255
							 : outer_alpha * 255.0 + 0.5));

		mask = pixman_image_create_bits(PIXMAN_a8, 1, 1, (uint32_t *)&level, 4);
		if (mask != NULL) {
			/* THE DESTINATION IS THE SURFACE THIS GROUP REPLACED AND NOT `c->image`: at this moment
			 * `c->image` IS the group's own, so compositing into it would put the group onto itself
			 * and leave the real destination untouched — which is exactly what the probe caught, as
			 * every readout of zero. The CLIP is still the group's, and that is right: a layer bounded
			 * by a rectangle is bounded at both ends by that rectangle. */
			pixman_image_set_repeat(mask, PIXMAN_REPEAT_NORMAL);
			pixman_image_set_clip_region32(g->saved_image, &c->state.clip);
			pixman_image_composite32(cg_op(outer_blend), g->image, mask, g->saved_image, 0, 0, 0, 0,
						 0, 0, c->width, c->height);
			pixman_image_unref(mask);
		}
	}
	pixman_image_unref(g->image);
	free(g->data);
	c->data = g->saved_data;
	c->stride = g->saved_stride;
	c->image = g->saved_image;
	c->owns_data = g->saved_owns_data;
	/* AND THE TWO STATE EXCEPTIONS ARE UNDONE, WHICH IS THE OTHER HALF OF APPLE'S SENTENCE: "after a call to
	 * this function, all of the parameters in the graphics state remain unchanged" — so the alpha the caller
	 * had before the layer is the alpha they have after it. The probe caught this as the layer's own alpha of
	 * 1 leaking out of the layer and attenuating nothing. */
	c->state.alpha = g->alpha;
	c->state.blend = g->blend;
	free(g);
}

static void cg_group_begin(CGContextRef c, CFDictionaryRef auxiliaryInfo)
{
	struct cg_group *g;
	size_t stride;

	(void)auxiliaryInfo;	/* Apple's reserved parameter, as on CGLayer */
	if (c == NULL || c->data == NULL) {
		return;
	}
	if (c->state.blend != kCGBlendModeNormal) {
		fprintf(stderr, "CG-REFUSE: a transparency layer under a non-normal blend mode has no answer in "
				"the header: the group is composited ONCE, so an operator like Multiply would reach "
				"every pixel of the clip rather than the ones the layer drew\n");
		return;
	}
	stride = (size_t)c->stride;
	g = calloc(1, sizeof(struct cg_group));
	if (g == NULL) {
		return;
	}
	/* "A FULLY TRANSPARENT BACKDROP", which calloc gives: premultiplied zero is transparent whatever the
	 * colour would have been. */
	g->data = calloc(1, stride * (size_t)c->height);
	if (g->data == NULL) {
		free(g);
		return;
	}
	g->image = pixman_image_create_bits(cg_context_format(c->bitmap_info), c->width, c->height, (uint32_t *)g->data,
					    (int)stride);
	if (g->image == NULL) {
		free(g->data);
		free(g);
		return;
	}
	g->saved_data = c->data;
	g->saved_stride = c->stride;
	g->saved_image = c->image;
	g->saved_owns_data = c->owns_data;
	g->alpha = c->state.alpha;
	g->blend = c->state.blend;
	g->next = c->groups;
	c->groups = g;
	/* AND THE THREE EXCEPTIONS, WHICH ARE THE POINT OF THE DOOR: inside the layer the alpha is 1, the
	 * shadow is off (this library has none to turn off) and the blend mode is normal — so that the OUTER
	 * alpha, applied once at the end, is not applied again to everything drawn in between. */
	c->data = g->data;
	c->stride = (int)stride;
	c->image = g->image;
	c->owns_data = 0;	/* the group's record owns it, and frees it on the way out */
	c->state.alpha = 1.0;
	c->state.blend = kCGBlendModeNormal;
}

void CGContextBeginTransparencyLayer(CGContextRef c, CFDictionaryRef auxiliaryInfo)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "transparency layers");
		return;
	}
	cg_group_begin(c, auxiliaryInfo);
}

void CGContextBeginTransparencyLayerWithRect(CGContextRef c, CGRect rect, CFDictionaryRef auxiliaryInfo)
{
	/* "IDENTICAL EXCEPT THAT THE CONTENT WILL BE BOUNDED BY `rect`" — and the CLIP is what bounds both
	 * halves, since the group's drawing and the final composite both respect it. `CGContextClipToRect` is
	 * the door that intersects the clip with a rectangle, and using it means a rotated CTM refuses here
	 * exactly as it does there rather than being approximated. */
	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "transparency layers");
		return;
	}
	cg_group_begin(c, auxiliaryInfo);
	if (c->groups != NULL) {
		CGContextClipToRect(c, rect);
	}
}

void CGContextEndTransparencyLayer(CGContextRef c)
{
	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		/* THE BEGIN WAS REFUSED TOO, so there is no group to pop and nothing the caller can do but
		 * continue; saying so once is the honest answer. */
		cg_pdf_writer_refuse(c->writer, "transparency layers");
		return;
	}
	if (c->groups == NULL) {
		fprintf(stderr, "CG-REFUSE: CGContextEndTransparencyLayer without a matching begin has no layer "
				"to end\n");
		return;
	}
	cg_group_pop(c, 1);
}

void CGContextSaveGState(CGContextRef c)
{
	cg_state *slot;

	if (c == NULL) {
		return;
	}
	if (c->depth == c->stack_cap) {
		int want = c->stack_cap ? c->stack_cap * 2 : 8;
		cg_state *grown = realloc(c->stack, (size_t)want * sizeof(cg_state));

		if (grown == NULL) {
			return;
		}
		c->stack = grown;
		c->stack_cap = want;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_save(c->writer);
	}
	slot = &c->stack[c->depth];
	*slot = c->state;
	/* THE PATTERNS ARE RETAINED INTO THE SAVED SLOT — see cg_state's note. The assignment above copied
	 * two pointers that the CURRENT state owns; the saved state needs its own claim on them, or the
	 * first of the two to be replaced or released would take the other's pattern away. */
	CGPatternRetain(slot->fill_pattern);
	CGPatternRetain(slot->stroke_pattern);
	CGFontRetain(slot->font);
	/* SHARED BY REFCOUNT, which is right for an image that clips are only ever ADDED to: a restore can
	 * hand the same mask back without copying it. */
	if (slot->clip_mask != NULL) {
		pixman_image_ref(slot->clip_mask);
	}
	/* THE CLIP IS DEEP-COPIED, and it must be: it is a region with its own
	 * allocation, so a struct copy would leave two states sharing one buffer and a
	 * later `ClipToRect` in the restored state would edit the saved one. */
	pixman_region32_init(&slot->clip);
	pixman_region32_copy(&slot->clip, &c->state.clip);
	c->depth++;
}

void CGContextRestoreGState(CGContextRef c)
{
	if (c == NULL || c->depth == 0) {
		/* Apple's contract is that an unbalanced restore is a caller error; the
		 * cheapest honest reading of it here is to keep drawing with the state there
		 * is rather than to read off the end of the stack. */
		return;
	}
	c->depth--;
	if (c->writer != NULL) {
		cg_pdf_writer_restore(c->writer);
	}
	pixman_region32_fini(&c->state.clip);
	/* THE STATE BEING REPLACED LETS GO OF ITS PATTERNS FIRST, and the saved state's claim is what
	 * survives — which is the pair to the retain `CGContextSaveGState` took. The order matters only
	 * in that the release must not be of the very pointers about to be installed. */
	CGPatternRelease(c->state.fill_pattern);
	CGPatternRelease(c->state.stroke_pattern);
	CGFontRelease(c->state.font);
	if (c->state.clip_mask != NULL) {
		pixman_image_unref(c->state.clip_mask);
	}
	c->state = c->stack[c->depth];
}

CGAffineTransform CGContextGetCTM(CGContextRef c)
{
	return (c != NULL) ? c->state.ctm : CGAffineTransformIdentity;
}

CGAffineTransform CGContextGetUserSpaceToDeviceSpaceTransform(CGContextRef c)
{
	return CGContextGetCTM(c);
}

/*
 * THE FOUR CTM DOORS, AND THE ONE RELATION THAT DECIDES ALL OF THEM.
 *
 * Apple's page for `concatenate(_:)` defines this family in one sentence: "the resulting CTM in the
 * context is `CTMnew = transform * CTMcontext`", and `scaleBy(x:y:)`, `translateBy(x:y:)` and
 * `rotate(by:)` are that operation with the matching `CGAffineTransformMake*`. With this library's
 * `CGAffineTransformConcat(t1, t2) = t1 * t2` (CGAffineTransform.c — the row-vector product Apple's own
 * header writes down), that formula is `Concat(transform, ctm)`: THE NEW TRANSFORM NAMED FIRST, the one
 * applied to the USER space.
 *
 * AND `CGContextScaleCTM` IS THEREFORE `CGAffineTransformScale`, exactly as it reads. The two families
 * agree because the primitive and the door are the same statement: `Scale(ctm, sx, sy)` is
 * `t' = [ sx 0 0 sy 0 0 ] * ctm`, which is `Concat(MakeScale(sx, sy), ctm)`.
 *
 * WHAT THIS COST, TWICE (measured 2026-10-07). The three convenience doors once called
 * `CGAffineTransformScale`/`Translate`/`Rotate` while THOSE primitives had their operands reversed, so
 * the doors composed the transform on the side that acts on DEVICE coordinates: on a 32x32 surface a
 * filled rect at user `(0,0,8,8)` came out DOUBLED through `CGContextConcatCTM(c, MakeScale(2, 2))` and
 * NOWHERE AT ALL through `CGContextScaleCTM(c, 2, 2)`; the flip idiom every top-left-origin caller uses
 * drew nothing. That was fixed by routing the three doors through `ConcatCTM` (§46). THEN THE PRIMITIVES
 * THEMSELVES WERE FIXED to Apple's formulas (§47), which made the natural spelling correct again — this
 * file is the spelling a reader expects, and the relation above is what keeps it honest. `coregraphics_ctm`
 * pins all four doors and `coregraphics_geometry` pins the primitives, so either drifting alone is caught.
 */
void CGContextConcatCTM(CGContextRef c, CGAffineTransform t)
{
	if (c != NULL) {
		if (c->writer != NULL) {
			cg_pdf_writer_ctm(c->writer, t);
		}
		c->state.ctm = CGAffineTransformConcat(t, c->state.ctm);
	}
}

void CGContextTranslateCTM(CGContextRef c, CGFloat tx, CGFloat ty)
{
	if (c != NULL) {
		if (c->writer != NULL) {
			cg_pdf_writer_ctm(c->writer, CGAffineTransformMakeTranslation(tx, ty));
		}
		c->state.ctm = CGAffineTransformTranslate(c->state.ctm, tx, ty);
	}
}

void CGContextScaleCTM(CGContextRef c, CGFloat sx, CGFloat sy)
{
	if (c != NULL) {
		if (c->writer != NULL) {
			cg_pdf_writer_ctm(c->writer, CGAffineTransformMakeScale(sx, sy));
		}
		c->state.ctm = CGAffineTransformScale(c->state.ctm, sx, sy);
	}
}

void CGContextRotateCTM(CGContextRef c, CGFloat angle)
{
	if (c != NULL) {
		if (c->writer != NULL) {
			cg_pdf_writer_ctm(c->writer, CGAffineTransformMakeRotation(angle));
		}
		c->state.ctm = CGAffineTransformRotate(c->state.ctm, angle);
	}
}

CGPoint CGContextConvertPointToDeviceSpace(CGContextRef c, CGPoint p)
{
	if (c == NULL) {
		return p;
	}
	return CGPointApplyAffineTransform(p, c->state.ctm);
}

CGPoint CGContextConvertPointToUserSpace(CGContextRef c, CGPoint p)
{
	CGAffineTransform inv;

	if (c == NULL) {
		return p;
	}
	/* A SINGULAR CTM HAS NO USER SPACE TO GO BACK TO, and rather than divide by zero
	 * the point is returned unchanged: the caller has already lost the space by
	 * collapsing it, and a garbage answer would be indistinguishable from a real one. */
	inv = CGAffineTransformInvert(c->state.ctm);
	if (CGAffineTransformEqualToTransform(inv, c->state.ctm) &&
	    !CGAffineTransformIsIdentity(c->state.ctm) &&
	    c->state.ctm.a * c->state.ctm.d - c->state.ctm.b * c->state.ctm.c == 0.0) {
		return p;
	}
	return CGPointApplyAffineTransform(p, inv);
}

CGSize CGContextConvertSizeToDeviceSpace(CGContextRef c, CGSize s)
{
	if (c == NULL) {
		return s;
	}
	return CGSizeApplyAffineTransform(s, c->state.ctm);
}

CGSize CGContextConvertSizeToUserSpace(CGContextRef c, CGSize s)
{
	if (c == NULL) {
		return s;
	}
	return CGSizeApplyAffineTransform(s, CGAffineTransformInvert(c->state.ctm));
}

CGRect CGContextConvertRectToDeviceSpace(CGContextRef c, CGRect r)
{
	if (c == NULL) {
		return r;
	}
	/* THE BOUNDING BOX OF THE TRANSFORMED RECTANGLE, which is what a rotation makes
	 * the only possible answer for a rectangle-shaped result. */
	return CGRectApplyAffineTransform(r, c->state.ctm);
}

CGRect CGContextConvertRectToUserSpace(CGContextRef c, CGRect r)
{
	if (c == NULL) {
		return r;
	}
	return CGRectApplyAffineTransform(r, CGAffineTransformInvert(c->state.ctm));
}

/* ------------------------------------------------------------------------- */
/* the clip                                                                  */
/* ------------------------------------------------------------------------- */

static void cg_clip_to_path(CGContextRef c, CGPathRef path, int even_odd);

static void cg_device_bounds(CGContextRef c, CGRect rect, int *left, int *top, int *right, int *bottom);
static double cg_image_mask_coverage(CGImageRef mask, const CGFloat *decode, double u,
				     double v);
static int cg_device_to_rect(const CGAffineTransform *inverse, CGRect rect, int x, int y, double *u,
			     double *v);

/* INSTALL A NEW CLIP MASK, MULTIPLYING THE OLD ONE IN, AND RECORD WHERE THE NEW ONE LIVES — the one
 * place both clip builders meet, so the two cannot drift apart on the origin arithmetic.
 *
 * `IN` IS THE OPERATOR THAT INTERSECTS TWO MASKS: out = src x dst.a, so compositing the OLD mask onto
 * the new leaves new x old. MEASURED against two alpha-only images (cover 255 -> 32 at an edge) after I
 * first assumed it would not and wrote a hand loop that DOUBLE-applied the clip.
 *
 * AND THE OFFSET IS THE WHOLE POINT NOW THAT A BOX IS INVOLVED: the new mask's device pixel `(mx, my)`
 * is the old mask's `(mx - old_x, my - old_y)`, so that is what the source is read at. Where the old box
 * does not reach, the source is transparent, `IN` writes zero, and that is the old clip being EMPTY
 * there — which is why the two boxes are not intersected first: the composite already does it. */
static void cg_clip_mask_install(CGContextRef c, pixman_image_t *mask, int mx, int my, int mw, int mh)
{
	if (mask == NULL) {
		return;
	}
	if (c->state.clip_mask != NULL) {
		pixman_image_composite32(PIXMAN_OP_IN, c->state.clip_mask, NULL, mask,
					 mx - c->state.clip_mask_x, my - c->state.clip_mask_y, 0, 0, 0, 0,
					 mw, mh);
		pixman_image_unref(c->state.clip_mask);
	}
	c->state.clip_mask = mask;
	c->state.clip_mask_x = mx;
	c->state.clip_mask_y = my;
}

void CGContextClipToMask(CGContextRef c, CGRect rect, CGImageRef mask)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "a clip mask (CGContextClipToMask)");
		return;
	}
	pixman_image_t *cover;
	unsigned char *bytes;
	CGAffineTransform inverse;
	int left, top, right, bottom;
	int x, y;

	if (c == NULL || mask == NULL || c->data == NULL) {
		return;
	}
	if (rect.size.width <= 0.0 || rect.size.height <= 0.0) {
		return;
	}
	if (!CGImageIsMask(mask)) {
		/* THE THREE RULES FOR AN IMAGE, IN APPLE'S WORDS: "it must be in the DeviceGray color space, may
		 * not have alpha, and may not be masked by an image mask or masking color." A colored mask would
		 * have to be reduced to one number per pixel and the header does not say how. */
		int colors = 0;

		if (CGImageGetColorSpace(mask) == NULL
		    || CGColorSpaceGetModel(CGImageGetColorSpace(mask)) != kCGColorSpaceModelMonochrome) {
			fprintf(stderr, "CG-REFUSE: CGContextClipToMask takes an image mask or a DeviceGray "
					"image, and this mask is neither\n");
			return;
		}
		if (CGImageGetAlphaInfo(mask) != kCGImageAlphaNone) {
			fprintf(stderr, "CG-REFUSE: CGContextClipToMask needs a mask image with no alpha "
					"channel: the mask's samples ARE the alpha\n");
			return;
		}
		if (cg_image_mask(mask) != NULL || cg_image_masking_colors(mask, &colors) != NULL) {
			fprintf(stderr, "CG-REFUSE: CGContextClipToMask needs a mask image that is not itself "
					"masked\n");
			return;
		}
	}
	if (CGImageGetBitsPerPixel(mask) != 8) {
		fprintf(stderr, "CG-REFUSE: CGContextClipToMask samples an 8-bit mask; this one has %lu bits "
				"per pixel\n", (unsigned long)CGImageGetBitsPerPixel(mask));
		return;
	}
	/* THE CLIP MASK COVERS THE WHOLE SURFACE AND STARTS AT "KEEP EVERYTHING": a pixel outside `rect` is not
	 * in the mask, and multiplying the clipping area by 1 leaves it as it was. ONLY THE RECTANGLE'S OWN
	 * PIXELS ARE SAMPLED, and it is the RECTANGLE that bounds them rather than the parallelogram — a
	 * bounding box under a rotation has corners outside the shape, and those must stay at 1. */
	bytes = calloc(1, (size_t)c->width * (size_t)c->height);
	if (bytes == NULL) {
		return;
	}
	memset(bytes, 255, (size_t)c->width * (size_t)c->height);
	cover = pixman_image_create_bits(PIXMAN_a8, c->width, c->height, (uint32_t *)bytes, c->width);
	if (cover == NULL) {
		free(bytes);
		return;
	}
	cg_device_bounds(c, rect, &left, &top, &right, &bottom);
	inverse = CGAffineTransformInvert(c->state.ctm);
	for (y = top; y < bottom; y++) {
		for (x = left; x < right; x++) {
			double u, v;

			if (!cg_device_to_rect(&inverse, rect, x, y, &u, &v)) {
				continue;
			}
			/* THE COVERAGE RULE IS `CGImageCreateWithMask`'S AND THE SAMPLER'S: an image mask is an
			 * INVERSE alpha and an image is the alpha itself, both of them this one function. */
			bytes[(size_t)y * (size_t)c->width + (size_t)x] =
				(unsigned char)(0.5 + 255.0 * cg_image_mask_coverage(mask, CGImageGetDecode(mask),
										     u, v));
		}
	}
	/* "INTERSECTED WITH THE CURRENT CLIPPING AREA" — and the two masks intersect with `IN`, which is the
	 * operator the path clip already uses for exactly this. */
	/* THIS MASK IS THE SURFACE AND STAYS THE SURFACE: outside `rect` it holds 1 — "keep everything" —
	 * so there is no box it could be narrowed to. The shared install still MULTIPLIES the previous clip
	 * into it, at the offset between the two origins. */
	cg_clip_mask_install(c, cover, 0, 0, c->width, c->height);
}

void CGContextClipToRects(CGContextRef c, const CGRect *rects, size_t count)
{
	CGMutablePathRef scratch;
	size_t i;

	if (c == NULL) {
		return;
	}
	if (rects == NULL || count == 0) {
		/* "This function resets the context's path to the empty path" — WITH NO RECTANGLES THERE IS NO
		 * CLIP TO ADD, and the reset still happens, which is the half of Apple's sentence a caller who
		 * passes an empty array is relying on. */
		CGContextBeginPath(c);
		return;
	}
	if (c->writer != NULL) {
		for (i = 0; i < count; i++) {
			cg_pdf_writer_clip_rect(c->writer, rects[i]);
		}
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		CGPathAddRect(scratch, NULL, rects[i]);
	}
	/* ONE PATH, SO AN OVERLAP MEANS "INSIDE EITHER" — see CGContext.h. `cg_clip_to_path` reads the path and
	 * does not take it (it is the same function `CGContextClip` calls with the context's own path), so the
	 * scratch is this function's to release, and the clip resets the current path itself. */
	cg_clip_to_path(c, (CGPathRef)scratch, 0);
	CGPathRelease((CGPathRef)scratch);
}

void CGContextClipToRect(CGContextRef c, CGRect rect)
{
	CGRect dev;

	if (c == NULL) {
		return;
	}
	/* THE ONE CASE THIS REFUSES, and the reason it is a refusal rather than a
	 * bounding box: under a CTM with rotation or skew the transformed rectangle is a
	 * parallelogram, and an axis-aligned region cannot hold one. Clipping to its
	 * bounding box would keep pixels OUTSIDE the rect the caller asked to keep — a
	 * silently wrong answer — so this refuses by name and leaves the clip alone. */
	if (c->state.ctm.b != 0.0 || c->state.ctm.c != 0.0) {
		fprintf(stderr, "CG-REFUSE: CGContextClipToRect under a rotated or skewed CTM "
				"(a device-space clip region is axis-aligned; the bounding box "
				"would clip away pixels inside the rect)\n");
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_clip_rect(c->writer, rect);
		return;
	}
	dev = CGRectApplyAffineTransform(rect, c->state.ctm);
	dev = CGRectIntegral(CGRectStandardize(dev));
	pixman_region32_intersect_rect(&c->state.clip, &c->state.clip,
				       (int)dev.origin.x, (int)dev.origin.y,
				       (unsigned int)(dev.size.width < 0 ? 0 : dev.size.width),
				       (unsigned int)(dev.size.height < 0 ? 0 : dev.size.height));
}

void cg_context_reset_clip(CGContextRef c)
{
	if (c == NULL) {
		return;
	}
	/* RESET MEANS THE WHOLE SURFACE, which is the region AND the mask. */
	if (c->state.clip_mask != NULL) {
		pixman_image_unref(c->state.clip_mask);
		c->state.clip_mask = NULL;
		c->state.clip_mask_x = 0;
		c->state.clip_mask_y = 0;
	}
	pixman_region32_fini(&c->state.clip);
	pixman_region32_init_rect(&c->state.clip, 0, 0, (unsigned int)c->width,
				  (unsigned int)c->height);
}

CGRect CGContextGetClipBoundingBox(CGContextRef c)
{
	pixman_box32_t *box;
	CGRect dev;
	CGAffineTransform inv;

	if (c == NULL) {
		return CGRectNull;
	}
	box = pixman_region32_extents(&c->state.clip);
	dev = CGRectMake((CGFloat)box->x1, (CGFloat)box->y1,
			 (CGFloat)(box->x2 - box->x1), (CGFloat)(box->y2 - box->y1));
	/* DEVICE SPACE IN, USER SPACE OUT: the clip is stored in device space because
	 * that is where pixman applies it, and a caller asking for the box is asking
	 * about the space it draws in. */
	inv = CGAffineTransformInvert(c->state.ctm);
	return CGRectApplyAffineTransform(dev, inv);
}

/* ------------------------------------------------------------------------- */
/* fill colour, alpha, blend, antialiasing                                   */
/* ------------------------------------------------------------------------- */

void CGContextSetGrayFillColor(CGContextRef c, CGFloat gray, CGFloat alpha)
{
	if (c == NULL) {
		return;
	}
	/* A COMPONENT COLOUR CLEARS A PATTERN JUST AS A `CGColor` ONE DOES, and this was the ONE hole in
	 * the "the later paint wins" rule until the pattern probe's save/restore check found it: a caller
	 * who set a pattern and then a component colour got the pattern, because this setter writes the
	 * numbers the COLOUR path reads and the fill never asks for them while a pattern is set. Four
	 * setters had it; all four clear it now, and the check that caught it is the one that fills after
	 * a `CGContextSetRGBFillColor` and expects the colour. */
	cg_clear_pattern(&c->state.fill_pattern);
	c->state.rgba[0] = gray;
	c->state.rgba[1] = gray;
	c->state.rgba[2] = gray;
	c->state.rgba[3] = alpha;
}

void CGContextSetRGBFillColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha)
{
	if (c == NULL) {
		return;
	}
	cg_clear_pattern(&c->state.fill_pattern);   /* see CGContextSetGrayFillColor */
	c->state.rgba[0] = red;
	c->state.rgba[1] = green;
	c->state.rgba[2] = blue;
	c->state.rgba[3] = alpha;
}

/* THE CMYK DOOR, WHICH IS THE ONE PLACE INK VALUES ARE SET DIRECTLY. The four inks become device RGB
 * through `cg_device_cmyk_to_rgb` — this library's own stated rule, with the reason and the record of whose
 * decision it was, in CGColor.h — and the pattern is cleared like every other component colour, for the
 * reason CGContextSetGrayFillColor records. THE COLOUR SPACE IS NOT TOUCHED: this tree's three sibling
 * doors do not set it either, so `CGContextGetFillColorSpace` keeps reporting the space the caller last
 * asked for, and the components door keeps reading its own model. */
void CGContextSetCMYKFillColor(CGContextRef c, CGFloat cyan, CGFloat magenta, CGFloat yellow,
			       CGFloat black, CGFloat alpha)
{
	const CGFloat cmyk[5] = { cyan, magenta, yellow, black, alpha };
	CGFloat rgb[4];

	if (c == NULL) {
		return;
	}
	cg_clear_pattern(&c->state.fill_pattern);
	cg_device_cmyk_to_rgb(cmyk, rgb);
	c->state.rgba[0] = rgb[0];
	c->state.rgba[1] = rgb[1];
	c->state.rgba[2] = rgb[2];
	c->state.rgba[3] = rgb[3];
}

/* THE COLOUR CONVERSION IS DEFINED WITH THE COLOUR DOORS BELOW, and a static function cannot be used
 * before its declaration: the same lesson CGLayer, the clip door and the mask clip each paid for, so
 * this one is declared at the same moment the call is written. */
static int cg_color_to_rgba(CGColorRef color, CGFloat rgba[4]);

void CGContextSetShadowWithColor(CGContextRef c, CGSize offset, CGFloat blur, CGColorRef color)
{
	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "shadows");
		return;
	}
	if (blur < 0.0) {
		fprintf(stderr, "CG-REFUSE: CGContextSetShadowWithColor takes a NON-NEGATIVE blur, and was given "
				"%g\n", (double)blur);
		return;
	}
	c->state.shadow_offset = offset;
	c->state.shadow_blur = blur;
	if (color == NULL) {
		/* APPLE: "if `color` is NULL, it's equivalent to specifying a fully transparent color" — which is
		 * this library's off state, so the two spellings of OFF are one state. */
		c->state.shadow_rgba[0] = 0.0;
		c->state.shadow_rgba[1] = 0.0;
		c->state.shadow_rgba[2] = 0.0;
		c->state.shadow_rgba[3] = 0.0;
		return;
	}
	if (!cg_color_to_rgba(color, c->state.shadow_rgba)) {
		return;	/* the conversion says why; the previous shadow state stays, as every refusal here does */
	}}

void CGContextSetShadow(CGContextRef c, CGSize offset, CGFloat blur)
{
	/* APPLE'S OWN DEFINITION OF THIS DOOR: "equivalent to calling CGContextSetShadowWithColor(context, offset,
	 * blur, color) where color is black with 1/3 alpha (i.e., RGBA = {0, 0, 0, 1.0/3.0}) in the DeviceRGB color
	 * space." IMPLEMENTED AS EXACTLY THAT CALL, so the equivalence cannot drift. */
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	const CGFloat black[4] = { 0.0, 0.0, 0.0, 1.0 / 3.0 };
	CGColorRef color;

	if (c == NULL || space == NULL) {
		CGColorSpaceRelease(space);
		return;
	}
	color = CGColorCreate(space, black);
	if (color == NULL) {
		CGColorSpaceRelease(space);
		return;
	}
	CGContextSetShadowWithColor(c, offset, blur, color);
	CGColorRelease(color);
	CGColorSpaceRelease(space);
}

void CGContextSetAlpha(CGContextRef c, CGFloat alpha)
{
	if (c != NULL) {
		c->state.alpha = alpha;
	}
}

void CGContextSetBlendMode(CGContextRef c, CGBlendMode mode)
{
	if (c != NULL) {
		c->state.blend = mode;
	}
}

void CGContextSetAllowsAntialiasing(CGContextRef c, int allows)
{
	if (c != NULL) {
		c->allows_antialiasing = allows ? 1 : 0;
	}
}

void CGContextSetShouldAntialias(CGContextRef c, int antialias)
{
	if (c != NULL) {
		c->state.antialias = antialias ? 1 : 0;
	}
}

/* ------------------------------------------------------------------------- */
/* the path                                                                  */
/* ------------------------------------------------------------------------- */

void CGContextBeginPath(CGContextRef c)
{
	if (c == NULL) {
		return;
	}
	CGPathRelease((CGPathRef)c->path);
	c->path = (struct CGPath *)CGPathCreateMutable();
}

void CGContextMoveToPoint(CGContextRef c, CGFloat x, CGFloat y)
{
	if (c != NULL) {
		CGPathMoveToPoint((CGMutablePathRef)c->path, NULL, x, y);
	}
}

void CGContextAddLineToPoint(CGContextRef c, CGFloat x, CGFloat y)
{
	if (c != NULL) {
		CGPathAddLineToPoint((CGMutablePathRef)c->path, NULL, x, y);
	}
}

void CGContextAddRect(CGContextRef c, CGRect rect)
{
	if (c != NULL) {
		CGPathAddRect((CGMutablePathRef)c->path, NULL, rect);
	}
}

void CGContextClosePath(CGContextRef c)
{
	if (c != NULL) {
		CGPathCloseSubpath((CGMutablePathRef)c->path);
	}
}

/* A PATH IS APPENDED BY WALKING IT, which makes this function a client of the public
 * CGPath API rather than a friend of its internals — and that is the reason the
 * element stream exists. */
typedef struct {
	struct CGPath *into;
} cg_copy_ctx;

static void cg_copy_element(void *info, const CGPathElement *element)
{
	cg_copy_ctx *cc = info;

	switch (element->type) {
	case kCGPathElementMoveToPoint:
		CGPathMoveToPoint((CGMutablePathRef)cc->into, NULL,
				  element->points[0].x, element->points[0].y);
		break;
	case kCGPathElementAddLineToPoint:
		CGPathAddLineToPoint((CGMutablePathRef)cc->into, NULL,
				     element->points[0].x, element->points[0].y);
		break;
	/* THE CURVE CASES ARE NOT DECORATION: WITHOUT THEM `CGContextAddPath` SILENTLY DROPS
	 * CURVES, and a caller's curve path becomes whatever lines survived — here, a lone
	 * move, which fills nothing at all. That is the failure this tree calls the worst kind,
	 * and it was LATENT until curves existed: the C2 path model had no curve element to
	 * lose, so `default: break` was correct and invisible. The curve probe found it by
	 * filling a quarter circle and reading ZERO of the 12818 coverage units its area is. */
	case kCGPathElementAddQuadCurveToPoint:
		CGPathAddQuadCurveToPoint((CGMutablePathRef)cc->into, NULL,
					  element->points[0].x, element->points[0].y,
					  element->points[1].x, element->points[1].y);
		break;
	case kCGPathElementAddCurveToPoint:
		CGPathAddCurveToPoint((CGMutablePathRef)cc->into, NULL,
				      element->points[0].x, element->points[0].y,
				      element->points[1].x, element->points[1].y,
				      element->points[2].x, element->points[2].y);
		break;
	case kCGPathElementCloseSubpath:
		CGPathCloseSubpath((CGMutablePathRef)cc->into);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* the context's own constructors, which are the path's                      */
/* ------------------------------------------------------------------------- */

/* EVERY FUNCTION BELOW IS A PASSTHROUGH, and that is the point: the geometry lives in
 * CGPath.c and the context owns a path, so a constructor here is a call and nothing else.
 * What it buys is the spelling a caller uses — `CGContextAddArc` instead of building a
 * CGPath, adding it and releasing it — and the risk it carries is a passthrough wired to the
 * WRONG path function, which no type checker can see and which the arc probe therefore
 * checks through the pixels. */

void CGContextAddLines(CGContextRef c, const CGPoint *points, size_t count)
{
	size_t i;

	if (c == NULL || points == NULL || count < 2) {
		return;
	}
	/* A POLYLINE: one subpath through every point. `count` is the number of POINTS, which is
	 * Apple's reading of the parameter and the one that stops a caller handing over twice as
	 * many as their array holds. */
	CGContextMoveToPoint(c, points[0].x, points[0].y);
	for (i = 1; i < count; i++) {
		CGContextAddLineToPoint(c, points[i].x, points[i].y);
	}
}

void CGContextAddRects(CGContextRef c, const CGRect *rects, size_t count)
{
	size_t i;

	if (c == NULL || rects == NULL) {
		return;
	}
	/* EACH RECTANGLE IS ITS OWN CLOSED SUBPATH — `CGPathAddRect` closes — so a fill of
	 * several of them fills each one, and a NON-ZERO fill of two overlapping ones is still
	 * their union only if they wind the same way, which they do. */
	for (i = 0; i < count; i++) {
		CGContextAddRect(c, rects[i]);
	}
}

void CGContextAddQuadCurveToPoint(CGContextRef c, CGFloat cpx, CGFloat cpy, CGFloat x, CGFloat y)
{
	if (c != NULL) {
		CGPathAddQuadCurveToPoint((CGMutablePathRef)c->path, NULL, cpx, cpy, x, y);
	}
}

void CGContextAddCurveToPoint(CGContextRef c, CGFloat cp1x, CGFloat cp1y, CGFloat cp2x,
			      CGFloat cp2y, CGFloat x, CGFloat y)
{
	if (c != NULL) {
		CGPathAddCurveToPoint((CGMutablePathRef)c->path, NULL, cp1x, cp1y, cp2x, cp2y, x, y);
	}
}

void CGContextAddArc(CGContextRef c, CGFloat x, CGFloat y, CGFloat radius, CGFloat startAngle,
		     CGFloat endAngle, bool clockwise)
{
	if (c != NULL) {
		CGPathAddArc((CGMutablePathRef)c->path, NULL, x, y, radius, startAngle, endAngle,
			     clockwise);
	}
}

void CGContextAddEllipseInRect(CGContextRef c, CGRect rect)
{
	if (c != NULL) {
		CGPathAddEllipseInRect((CGMutablePathRef)c->path, NULL, rect);
	}
}

void CGContextAddRoundedRect(CGContextRef c, CGRect rect, CGFloat cornerWidth, CGFloat cornerHeight)
{
	if (c != NULL) {
		CGPathAddRoundedRect((CGMutablePathRef)c->path, NULL, rect, cornerWidth, cornerHeight);
	}
}

CGPoint CGContextGetPathCurrentPoint(CGContextRef c)
{
	if (c == NULL) {
		return CGPointZero;
	}
	return CGPathGetCurrentPoint((CGPathRef)c->path);
}

void CGContextAddPath(CGContextRef c, CGPathRef path)
{
	cg_copy_ctx cc;

	if (c == NULL || path == NULL) {
		return;
	}
	cc.into = c->path;
	CGPathApply(path, &cc, cg_copy_element);
}

int CGContextIsPathEmpty(CGContextRef c)
{
	return (c == NULL) ? 1 : CGPathIsEmpty((CGPathRef)c->path);
}

CGRect CGContextGetPathBoundingBox(CGContextRef c)
{
	if (c == NULL) {
		return CGRectNull;
	}
	return CGPathGetPathBoundingBox((CGPathRef)c->path);
}

/* ------------------------------------------------------------------------- */
/* the rasterizer                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
	double x0, y0, x1, y1;
	int dir;             /* +1 going down the device grid, -1 going up */
} cg_edge;

typedef struct {
	CGAffineTransform ctm;
	double width, height;
	cg_edge *edges;
	int count;
	int cap;
	/* THE CURRENT SUBPATH, IN DEVICE SPACE, BEFORE CLIPPING — and this field exists
	 * because an EDGE AT A TIME CANNOT BE CLIPPED CORRECTLY. See cg_close_subpath: the
	 * bug it fixes was a fill that encloses the surface painting nothing. */
	double *pts;   /* x0, y0, x1, y1, … */
	int npts;
	int pts_cap;
} cg_flatten;

static int cg_edge_reserve(cg_flatten *fl)
{
	if (fl->count == fl->cap) {
		int want = fl->cap ? fl->cap * 2 : 32;
		cg_edge *grown = realloc(fl->edges, (size_t)want * sizeof(cg_edge));

		if (grown == NULL) {
			return 0;
		}
		fl->edges = grown;
		fl->cap = want;
	}
	return 1;
}

static int cg_push_vertex(cg_flatten *fl, double x, double y)
{
	if (fl->npts == fl->pts_cap) {
		int want = fl->pts_cap ? fl->pts_cap * 2 : 32;
		double *grown = realloc(fl->pts, (size_t)want * 2 * sizeof(double));

		if (grown == NULL) {
			return 0;
		}
		fl->pts = grown;
		fl->pts_cap = want;
	}
	fl->pts[fl->npts * 2] = x;
	fl->pts[fl->npts * 2 + 1] = y;
	fl->npts++;
	return 1;
}

/* ONE EDGE OF THE CLIPPED OUTLINE. A HORIZONTAL EDGE IS NEVER ACTIVE in a band with
 * height > 0, so it is dropped here and the sweep never has to ask. */
static int cg_emit_edge(cg_flatten *fl, double x0, double y0, double x1, double y1)
{
	if (y0 == y1) {
		return 1;
	}
	if (!cg_edge_reserve(fl)) {
		return 0;
	}
	fl->edges[fl->count].x0 = x0;
	fl->edges[fl->count].y0 = y0;
	fl->edges[fl->count].x1 = x1;
	fl->edges[fl->count].y1 = y1;
	/* THE SIGN IS THE WINDING CONTRIBUTION, and Sutherland–Hodgman PRESERVES the
	 * outline's orientation, so the signs stay consistent through the four passes. */
	fl->edges[fl->count].dir = (y1 > y0) ? 1 : -1;
	fl->count++;
	return 1;
}

/* ONE SIDE OF SUTHERLAND–HODGMAN, WITH A CAPACITY RATHER THAN A PROMISE. The input is
 * treated as a CLOSED loop — the last vertex joins the first — which is what makes the
 * output a closed outline: the segments this pass adds are the ones ALONG the clipping
 * boundary. Clipping an n-gon against one half-plane yields at most n + 1 vertices, so
 * the capacity only has to be generous; it is checked anyway, because a silent write
 * past the end of a buffer is the failure mode that costs a week. */
static int cg_clip_side(const double *in, int nin, double *out, int nout_cap, int *nout,
			int axis, double bound, int keep_ge)
{
	int i;

	*nout = 0;
	for (i = 0; i < nin; i++) {
		int j = (i + 1) % nin;
		double ax = in[i * 2];
		double ay = in[i * 2 + 1];
		double bx = in[j * 2];
		double by = in[j * 2 + 1];
		double ca = axis ? ay : ax;
		double cb = axis ? by : bx;
		int ina = keep_ge ? (ca >= bound) : (ca <= bound);
		int inb = keep_ge ? (cb >= bound) : (cb <= bound);

		if (*nout + 2 > nout_cap) {
			return 0;
		}
		if (ina) {
			out[*nout * 2] = ax;
			out[*nout * 2 + 1] = ay;
			(*nout)++;
		}
		if (ina != inb && cb != ca) {
			double t = (bound - ca) / (cb - ca);

			out[*nout * 2] = ax + t * (bx - ax);
			out[*nout * 2 + 1] = ay + t * (by - ay);
			(*nout)++;
		}
	}
	return 1;
}

/*
 * CLOSE THE CURRENT SUBPATH: clip its OUTLINE to the surface and emit the clipped
 * edges. Every subpath is implicitly closed for a fill, which is why this runs for an
 * explicitly closed subpath as well — and why a subpath of fewer than three vertices
 * contributes nothing (a lone move, or a move and a line, has no area: its loop is the
 * same edge in both directions, which is a winding of zero).
 *
 * THIS REPLACED AN EDGE-AT-A-TIME CLIPPER, AND THE MEASUREMENT THAT KILLED IT IS: **A
 * FILL THAT ENCLOSES THE SURFACE PAINTED NOTHING.** Every edge of such a polygon lies
 * OUTSIDE the surface, so clipping each edge to the surface kept NONE of them and the
 * sweep was left with no edges to pair — while every polygon whose boundary crosses the
 * surface was correct. Clipping the OUTLINE answers both cases at once: a polygon that
 * contains the surface clips down to the SURFACE ITSELF — four edges enclosing every
 * pixel — and a polygon that crosses it clips to exactly the intersection.
 *
 * Found by a probe check that failed for the "wrong" reason; it is now the check that
 * pins this behaviour, in userland/tests/coregraphics_context.c.
 */
static void cg_close_subpath(cg_flatten *fl)
{
	int n = fl->npts;
	int cap = 2 * n + 8;
	double *a;
	double *b;
	int na = n;
	int nb = 0;
	int i;

	if (n < 3) {
		fl->npts = 0;
		return;
	}
	/* **A SUBPATH THAT DOES NOT TOUCH THE SURFACE CONTRIBUTES NOTHING, AND SAYS NOTHING.** Clipping its
	 * outline to the surface then leaves no vertices, which is correct — the shape is off the picture — and the
	 * refusal below is for the OTHER case: an outline that DOES cross the surface and still clips away to
	 * nothing, which is a bug in the clipper. A STROKE hits the legitimate case constantly, because its outline
	 * is deliberately wider than the path it strokes: MEASURED, the one-pixel border around a menu panel put a
	 * four-point sliver just past the right edge of a 164-wide surface, and every frame printed a refusal about a
	 * subpath that had correctly contributed nothing. The box test is the cheap way to tell the two apart, and it
	 * runs before either buffer is allocated. */
	{
		double minx = fl->pts[0], maxx = fl->pts[0];
		double miny = fl->pts[1], maxy = fl->pts[1];
		int k;

		for (k = 1; k < n; k++) {
			double px = fl->pts[k * 2];
			double py = fl->pts[k * 2 + 1];

			if (px < minx) minx = px;
			if (px > maxx) maxx = px;
			if (py < miny) miny = py;
			if (py > maxy) maxy = py;
		}
		if (maxx <= 0.0 || minx >= fl->width || maxy <= 0.0 || miny >= fl->height) {
			fl->npts = 0;
			return;
		}
	}
	a = malloc((size_t)cap * 2 * sizeof(double));
	b = malloc((size_t)cap * 2 * sizeof(double));
	if (a == NULL || b == NULL) {
		free(a);
		free(b);
		fl->npts = 0;
		return;
	}
	for (i = 0; i < n * 2; i++) {
		a[i] = fl->pts[i];
	}
	fl->npts = 0;

	/* FOUR PASSES, ONE PER SIDE OF THE SURFACE, alternating buffers; the result ends in
	 * `a` and its length in `na`. */
	if (!cg_clip_side(a, na, b, cap, &nb, 0, 0.0, 1)) {
		nb = 0;
	}
	if (nb >= 3) {
		if (!cg_clip_side(b, nb, a, cap, &na, 0, fl->width, 0)) {
			na = 0;
		}
	} else {
		na = 0;
	}
	if (na >= 3) {
		if (!cg_clip_side(a, na, b, cap, &nb, 1, 0.0, 1)) {
			nb = 0;
		}
	} else {
		nb = 0;
	}
	if (nb >= 3) {
		if (!cg_clip_side(b, nb, a, cap, &na, 1, fl->height, 0)) {
			na = 0;
		}
	} else {
		na = 0;
	}
	if (na < 3) {
		fprintf(stderr, "CG-REFUSE: a subpath's clipped outline did not survive the "
				"surface clip\n");
		free(a);
		free(b);
		return;
	}
	for (i = 0; i < na; i++) {
		int j = (i + 1) % na;

		cg_emit_edge(fl, a[i * 2], a[i * 2 + 1], a[j * 2], a[j * 2 + 1]);
	}
	free(a);
	free(b);
}

static void cg_flatten_element(void *info, const CGPathElement *element)
{
	cg_flatten *fl = info;
	CGPoint p;

	switch (element->type) {
	case kCGPathElementMoveToPoint:
		cg_close_subpath(fl);
		p = CGPointApplyAffineTransform(element->points[0], fl->ctm);
		cg_push_vertex(fl, p.x, p.y);
		break;
	case kCGPathElementAddLineToPoint:
		p = CGPointApplyAffineTransform(element->points[0], fl->ctm);
		/* A LINE WITH NO PRECEDING MOVE STARTS AT THE ORIGIN, which is what
		 * `CGPathGetCurrentPoint` states the current point of an empty path is. */
		if (fl->npts == 0) {
			cg_push_vertex(fl, 0.0, 0.0);
		}
		cg_push_vertex(fl, p.x, p.y);
		break;
	case kCGPathElementCloseSubpath:
		/* CLOSING RETURNS THE CURRENT POINT TO THE SUBPATH'S START, so a line AFTER the
		 * close continues from there: save the start, clip the outline, and re-seed the
		 * subpath with that single vertex — which emits nothing on its own. */
		if (fl->npts > 0) {
			double sx = fl->pts[0];
			double sy = fl->pts[1];

			cg_close_subpath(fl);
			cg_push_vertex(fl, sx, sy);
		}
		break;
	default:
		break;
	}
}

/* DO TWO EDGES PROPERLY CROSS? Only the INTERIOR counts: edges that meet at a shared
 * vertex are how polygons are built, and flagging those would refuse every path. This
 * is the test behind the one refusal in this file — see the caller. */
static int cg_proper_cross(const cg_edge *a, const cg_edge *b)
{
	double d1, d2, d3, d4;

	d1 = (b->x1 - b->x0) * (a->y0 - b->y0) - (b->y1 - b->y0) * (a->x0 - b->x0);
	d2 = (b->x1 - b->x0) * (a->y1 - b->y0) - (b->y1 - b->y0) * (a->x1 - b->x0);
	d3 = (a->x1 - a->x0) * (b->y0 - a->y0) - (a->y1 - a->y0) * (b->x0 - a->x0);
	d4 = (a->x1 - a->x0) * (b->y1 - a->y0) - (a->y1 - a->y0) * (b->x1 - a->x0);
	return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
	       ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

static double cg_edge_x_at(const cg_edge *e, double y)
{
	double t = (y - e->y0) / (e->y1 - e->y0);

	return e->x0 + t * (e->x1 - e->x0);
}

/* WHERE TWO EDGES CROSS, IN y. `cg_proper_cross` answers "do they"; this answers "where",
 * which is the question a sweep needs: a band across which the active edges change their
 * x-ORDER has to end at the y where that happens, or the pairing the winding walk makes is
 * the pairing of a different band. The linear form is f(t) = f(0) + t·(f(1) − f(0)) with the
 * SAME two cross products `cg_proper_cross` computes, so the root is f(0)/(f(0) − f(1)) and
 * there is no second intersection test that could disagree with the first. */
static int cg_cross_y(const cg_edge *a, const cg_edge *b, double *y)
{
	double f0 = (b->x1 - b->x0) * (a->y0 - b->y0) - (b->y1 - b->y0) * (a->x0 - b->x0);
	double f1 = (b->x1 - b->x0) * (a->y1 - b->y0) - (b->y1 - b->y0) * (a->x1 - b->x0);
	double den = f0 - f1;

	if (den == 0.0) {
		return 0;
	}
	*y = a->y0 + (f0 / den) * (a->y1 - a->y0);
	return 1;
}

/* THE BAND BOUNDARY LIST GROWS, because crossings are O(n²) in the edges while the vertex
 * list is only O(n). A stroked ARC's outline is 68 edges and its pieces cross in dozens of
 * places; a fixed array sized from the vertices would either overflow or refuse a shape
 * that is perfectly legitimate — and it is the shape a curve is. */
static int cg_ys_add(double **ys, int *nys, int *cap, double y)
{
	if (*nys == *cap) {
		int want = *cap ? *cap * 2 : 16;
		double *grown = realloc(*ys, (size_t)want * sizeof(double));

		if (grown == NULL) {
			return 0;
		}
		*ys = grown;
		*cap = want;
	}
	(*ys)[(*nys)++] = y;
	return 1;
}

typedef struct {
	pixman_trapezoid_t *traps;
	int count;
	int cap;
} cg_traps;

static int cg_trap_reserve(cg_traps *tr)
{
	if (tr->count == tr->cap) {
		int want = tr->cap ? tr->cap * 2 : 64;
		pixman_trapezoid_t *grown = realloc(tr->traps, (size_t)want * sizeof(pixman_trapezoid_t));

		if (grown == NULL) {
			return 0;
		}
		tr->traps = grown;
		tr->cap = want;
	}
	return 1;
}

/* TWO COMPARATORS FOR `cg_sweep`'S SORTS. THE SECOND ONE'S TIE-BREAK IS LOAD-BEARING: the
 * active-edge order decides which edge a winding walk pairs with which, and the insertion sort it
 * replaces kept ties in SCAN ORDER — so the tie-break is the INDEX, and with it the order is the
 * same one that stable sort produced, byte for byte. The first needs no tie-break: the bands are
 * consecutive pairs of the y table and a zero-height band is skipped, so the order among equal
 * values never reaches a trapezoid. */
static int cg_double_cmp(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;

	return (x < y) ? -1 : (x > y) ? 1 : 0;
}

typedef struct {
	double x;
	int idx;
} cg_active;

/* AN EDGE'S OWN Y EXTENT PLUS ITS INDEX, so the sweep can order the edges by where they START
 * without a comparator that needs the edge array — `qsort`'s comparison function takes no context.
 * `idx` is the index into `fl->edges`, and the active table stores the POSITION in `erefs`. */
typedef struct {
	double lo;
	double hi;
	int idx;
} cg_eref;

static int cg_eref_cmp(const void *a, const void *b)
{
	const cg_eref *p = a;
	const cg_eref *q = b;

	if (p->lo < q->lo) {
		return -1;
	}
	if (p->lo > q->lo) {
		return 1;
	}
	return (p->idx < q->idx) ? -1 : (p->idx > q->idx) ? 1 : 0;
}

static int cg_active_cmp(const void *a, const void *b)
{
	const cg_active *p = a;
	const cg_active *q = b;

	if (p->x < q->x) {
		return -1;
	}
	if (p->x > q->x) {
		return 1;
	}
	return (p->idx < q->idx) ? -1 : (p->idx > q->idx) ? 1 : 0;
}

/* The sweep: band by band between the edges' own y values, pair the active edges by
 * the fill rule, and emit one trapezoid per inside interval. */
static void cg_sweep(cg_flatten *fl, cg_traps *tr, int even_odd)
{
	double *ys;
	cg_eref *erefs;
	cg_active *act;
	int *active;
	int *xactive;
	int nys = 0;
	int na = 0;
	int cursor = 0;
	int i, j;

	if (fl->count < 2) {
		return;
	}
	ys = malloc((size_t)(fl->count * 2) * sizeof(double));
	if (ys == NULL) {
		return;
	}
	for (i = 0; i < fl->count; i++) {
		double y0 = fl->edges[i].y0;
		double y1 = fl->edges[i].y1;

		if (y0 > y1) {
			double tmp = y0;
			y0 = y1;
			y1 = tmp;
		}
		if (y0 < 0.0) {
			y0 = 0.0;
		}
		if (y1 > fl->height) {
			y1 = fl->height;
		}
		if (y0 < y1) {
			ys[nys++] = y0;
			ys[nys++] = y1;
		}
	}
	/* AND EVERY CROSSING, NOT ONLY EVERY VERTEX. A band has to END where two active edges
	 * change their x-order, because that is the one place the sweep's assumption — that the
	 * order holds across the band — stops being true. THIS IS WHAT MAKES A SELF-INTERSECTING
	 * FILL, AND A STROKE, COMPUTABLE AT ALL: a stroked polyline is a set of overlapping
	 * quadrilaterals BY DESIGN, and on a CURVE neighbouring pieces genuinely cross, while on
	 * a straight one they only ever touch at a shared vertex. MEASURED: stroking a quarter
	 * circle painted NOTHING — coverage 0 with three refusal lines on stderr — because those
	 * crossings reached the fill's refusal instead of reaching this loop.
	 *
	 * AND IT IS AN EVENT SWEEP NOW, NOT AN ALL-PAIRS SCAN. The scan was 11,517,600 iterations for ONE
	 * pathological stroke; §53's y-overlap test cut the WORK to the 243,077 pairs that can actually
	 * meet (2.1%), but the other 11.3M iterations were still walked, and MEASURED (2026-10-07, §55)
	 * that walk was 85 ms of the stroke's 139. The edges are put in TOP ORDER first, so the scan
	 * becomes a sweep: an edge is tested against THE EDGES STILL LIVE when it starts, and the live set
	 * is pruned as the sweep passes each edge's bottom. EVERY PAIR IS TESTED EXACTLY ONCE — when the
	 * later of the two starts — and a pair whose y ranges overlap IS tested, because the earlier one
	 * is still live at that moment. §53's per-pair y test is therefore gone: the sweep is that test,
	 * applied once per edge instead of once per pair.
	 *
	 * AND THE PAIR IS STILL PRESENTED TO `cg_proper_cross`/`cg_cross_y` IN INDEX ORDER, which is not
	 * tidiness: those functions are floating point, the old scan always passed `(i, j)` with `i < j`,
	 * and a crossing y differing in the last bit would move a band boundary. The sweep finds the same
	 * pairs and asks about them in the same order.
	 *
	 * THE CAPACITY IS WHATEVER THE VERTEX PASS FILLED: it adds at most two entries per edge, which is
	 * exactly what was allocated for it, so the crossings are the first entries that can need more room
	 * and `cg_ys_add` grows the array when they do. A fixed size taken from the vertices would overflow
	 * or refuse a shape that is perfectly legitimate — and a curve is one. */
	erefs = malloc((size_t)fl->count * sizeof(cg_eref));
	xactive = malloc((size_t)fl->count * sizeof(int));
	if (erefs == NULL || xactive == NULL) {
		free(erefs);
		free(xactive);
		free(ys);
		return;
	}
	for (j = 0; j < fl->count; j++) {
		const cg_edge *e = &fl->edges[j];

		erefs[j].lo = e->y0 < e->y1 ? e->y0 : e->y1;
		erefs[j].hi = e->y0 < e->y1 ? e->y1 : e->y0;
		erefs[j].idx = j;
	}
	qsort(erefs, (size_t)fl->count, sizeof(cg_eref), cg_eref_cmp);
	{
		int ys_cap = fl->count * 2;
		int na_x = 0;

		for (i = 0; i < fl->count; i++) {
			double lo_i = erefs[i].lo;
			int out = 0;

			/* DROP WHAT CAN NO LONGER MEET ANYTHING THAT STARTS FROM HERE ON. */
			for (j = 0; j < na_x; j++) {
				if (erefs[xactive[j]].hi >= lo_i) {
					xactive[out++] = xactive[j];
				}
			}
			na_x = out;
			/* AND TEST THIS EDGE AGAINST THE LIVE SET, IN INDEX ORDER. */
			for (j = 0; j < na_x; j++) {
				int ia = erefs[xactive[j]].idx;
				int ib = erefs[i].idx;
				int pa = ia < ib ? ia : ib;
				int pb = ia < ib ? ib : ia;
				double cy;

				if (cg_proper_cross(&fl->edges[pa], &fl->edges[pb]) &&
				    cg_cross_y(&fl->edges[pa], &fl->edges[pb], &cy) && cy > 0.0 &&
				    cy < fl->height) {
					if (!cg_ys_add(&ys, &nys, &ys_cap, cy)) {
						free(erefs);
						free(xactive);
						free(ys);
						return;
					}
				}
			}
			xactive[na_x++] = i;
		}
	}
	/* SORTED WITH `qsort`, AND THE INSERTION SORT THAT STOOD HERE WAS THE FILL'S LARGEST COST.
	 * MEASURED (2026-10-07, §53) on ONE stroked 600-gon — the shape a stroke really is, four
	 * overlapping pieces per segment: 4800 edges and **32677** y values, of which the insertion
	 * sort was **750 ms** of the 1460 the fill took. It is quadratic in the BAND COUNT, and
	 * crossings make the band count large on purpose. `qsort` is O(n log n) on the same values
	 * and IS EXACT: the bands are consecutive PAIRS of this table and a zero-height band is
	 * skipped below, so the order among equal values never reaches a trapezoid. (The comment
	 * that stood here said an active-edge table is "what a pathological path would want" — still
	 * true, still owed; this is the part that needed no new structure.) */
	qsort(ys, (size_t)nys, sizeof(double), cg_double_cmp);

	/* THE ACTIVE-EDGE TABLE, WHICH IS WHAT THE COMMENT OVER THE INSERTION SORT SAID THIS WANTED.
	 * The band loop used to REDISCOVER the active edges for every band — a scan of all `fl->count`
	 * edges — and MEASURED (2026-10-07, §54) that scan is the last quadratic here: 485 ms of a 567 ms
	 * stroke, with 32677 bands over 4800 edges. It is a table advanced by a CURSOR now: the edges are
	 * pre-sorted by their top, ADDED once when the sweep reaches them, and DROPPED by one compacting
	 * pass when the band passes their bottom.
	 *
	 * AND IT IS EXACT, WHICH IS WHY IT IS DONE THIS WAY: the scan kept an edge for a band iff
	 * `lo <= yt && hi >= yb && hi != lo`, and so does this — the cursor adds exactly the edges with
	 * `lo <= yt` and the compaction keeps exactly those with `hi >= yb`, so the SET is the same one,
	 * band for band. THE WINDING WALK BELOW IS UNTOUCHED, which is what makes this safe to do in a
	 * rasteriser this subtle: only the GATHERING moved. (The per-band `act` was also malloc'd and
	 * freed on every band — 32677 times — and is allocated once now.) */
	act = malloc((size_t)fl->count * sizeof(cg_active));
	active = malloc((size_t)fl->count * sizeof(int));
	if (act == NULL || active == NULL) {
		free(act);
		free(active);
		free(erefs);
		free(xactive);
		free(ys);
		return;
	}

	for (i = 0; i + 1 < nys; i++) {
		double yt = ys[i];
		double yb = ys[i + 1];
		int n;
		int w = 0;
		int left = -1;

		if (yb <= yt) {
			continue;
		}
		/* ADD WHAT STARTED BY THIS BAND'S TOP, THEN DROP WHAT ENDED BY ITS BOTTOM. */
		while (cursor < fl->count && erefs[cursor].lo <= yt) {
			if (erefs[cursor].hi != erefs[cursor].lo) {
				active[na++] = cursor;
			}
			cursor++;
		}
		{
			int out = 0;

			for (j = 0; j < na; j++) {
				if (erefs[active[j]].hi >= yb) {
					active[out++] = active[j];
				}
			}
			na = out;
		}
		/* THE X IS TAKEN AT THE BAND'S MIDDLE, NOT ITS TOP — the second half of the crossing fix.
		 * When a band's top IS a crossing, the two edges that meet there are TIED, and a tie resolved
		 * at the top can come out either way, which would pair the wrong edges for the whole band. The
		 * middle is unambiguous wherever the top is not. **AND IT IS FOR THE ORDER ONLY**, which is what
		 * the sort below and the winding walk read: the trapezoid's own x values are taken at the band's
		 * top and bottom, where the comment on `xa_top` says what reading them from here costs. */
		n = na;
		for (j = 0; j < n; j++) {
			act[j].idx = erefs[active[j]].idx;
			act[j].x = cg_edge_x_at(&fl->edges[act[j].idx], (yt + yb) / 2.0);
		}
		/* THE PER-BAND SORT IS ALSO `qsort`, AND ITS TIE-BREAK IS `cg_active_cmp`'s INDEX — see the
		 * note on the comparators: the insertion sort this replaces kept ties in SCAN ORDER, and the
		 * winding walk below depends on that order, so the comparator EARNS its tie-break rather than
		 * merely stabilising a sort. */
		qsort(act, (size_t)n, sizeof(cg_active), cg_active_cmp);
		for (j = 0; j < n; j++) {
			const cg_edge *e = &fl->edges[act[j].idx];

			if (even_odd) {
				w ^= 1;
			} else {
				w += e->dir;
			}
			if (w != 0 && left < 0) {
				left = j;
			} else if (w == 0 && left >= 0) {
				const cg_edge *ea = &fl->edges[act[left].idx];
				const cg_edge *eb = &fl->edges[act[j].idx];
				pixman_trapezoid_t t;
				/* **THE TRAPEZOID'S OWN X VALUES ARE TAKEN AT ITS OWN TOP AND BOTTOM, AND THE TOP ONE IS NOT
				 * `act[].x`.** That array holds each edge's x at the band's MIDDLE, which exists for the ORDER —
				 * a tie at a band's top is unresolvable, and the middle is not — and it is not the trapezoid's
				 * shape. Reading the top x out of it made every shape's top edge as wide as its middle: MEASURED,
				 * a triangle (12,20),(4,4),(20,4) came out 8 points wide across its top row and 16 across its base,
				 * with the apex MISSING, and the mirrored triangle came out wrong the other way. A rectangle was
				 * unaffected — its edges are vertical, so its x is the same at every y — which is why this
				 * survived every case that had been written down. */
				double xa_top = cg_edge_x_at(ea, yt);
				double xb_top = cg_edge_x_at(eb, yt);
				double xa_bot = cg_edge_x_at(ea, yb);
				double xb_bot = cg_edge_x_at(eb, yb);

				/* LEFT MUST BE TO THE LEFT, AND THE WINDING WALK CANNOT PROMISE IT. When
				 * two edges are TIED at the top of a band — a triangle's apex, which is
				 * exactly where a fill's first band begins — the sort has nothing to break
				 * the tie with, so the pair the WINDING found can be geometrically
				 * reversed, and a trapezoid whose left edge sits right of its right edge
				 * has no coverage. MEASURED: a triangle drew NOTHING until this
				 * normalization was added, while every rectangle — whose edges are never
				 * tied — was correct. The fallback comparison is the band's BOTTOM, where
				 * the geometry itself breaks the tie. */
				if (xa_top > xb_top || (xa_top == xb_top && xa_bot > xb_bot)) {
					double tmp;

					tmp = xa_top;
					xa_top = xb_top;
					xb_top = tmp;
					tmp = xa_bot;
					xa_bot = xb_bot;
					xb_bot = tmp;
				}
				t.top = cg_fixed(yt);
				t.bottom = cg_fixed(yb);
				t.left.p1.x = cg_fixed(xa_top);
				t.left.p1.y = cg_fixed(yt);
				t.left.p2.x = cg_fixed(xa_bot);
				t.left.p2.y = cg_fixed(yb);
				t.right.p1.x = cg_fixed(xb_top);
				t.right.p1.y = cg_fixed(yt);
				t.right.p2.x = cg_fixed(xb_bot);
				t.right.p2.y = cg_fixed(yb);
				if (pixman_trapezoid_valid(&t) && cg_trap_reserve(tr)) {
					tr->traps[tr->count++] = t;
				}
				left = -1;
			}
		}
	}
	free(erefs);
	free(act);
	free(active);
	free(xactive);
	free(ys);
}

/* ------------------------------------------------------------------------- */
/* filling                                                                   */
/* ------------------------------------------------------------------------- */

/* THE TRAPEZOIDS A PATH MAKES, AND NOTHING ELSE. Flattening, the sweep, and the mask format are the
 * geometry; the COLOUR is not. C6.1 split this out of the fill because a fill's source stopped being
 * a colour: a gradient, a shading and a pattern are all SAMPLED sources, and they must take the same
 * road as a constant one — one place that answers "where does this path cover", so a sampled fill
 * and a solid fill cannot come to different conclusions about the shape they are filling.
 *
 * A PATH THAT CROSSES ITSELF IS NO LONGER REFUSED, and the reason is one function below: `cg_sweep`
 * now ends its bands at every edge-edge CROSSING as well as at every vertex, so the x-order of the
 * active edges holds inside each band and a path that crosses itself — or a stroke, whose overlaps
 * ARE the design — computes like any other. The refusal was honest for its day ("the fill would be
 * wrong in a way that looks deliberate") and the checks that asserted it now assert the fills it was
 * refusing. */
static void cg_traps_for_path(CGContextRef c, CGPathRef path, int even_odd, cg_traps *tr)
{
	cg_flatten fl;

	memset(&fl, 0, sizeof(fl));
	memset(tr, 0, sizeof(*tr));
	fl.ctm = c->state.ctm;
	fl.width = (double)c->width;
	fl.height = (double)c->height;
	/* THE FILL DRAWS WHAT THE FLATTENER SAYS: a curve becomes lines HERE, once, for every
	 * fill — rather than in cg_flatten_element just below, which would then need its own
	 * subdivision and would be a second answer to where the curve is.
	 *
	 * THE TOLERANCE IS IN DEVICE SPACE, because that is the space the pixels are in: a
	 * user-space tolerance would draw a zoomed curve visibly faceted. The scale below is an
	 * UPPER BOUND on the CTM's — four numbers added instead of a square root, which is why
	 * `math.h` was not in this file until the INTERPOLATING SAMPLER below needed a floor — and an upper bound is the SAFE direction: it
	 * subdivides more finely than the device grid can show, never less.
	 *
	 * AND THE NUMBER IS NO LONGER FIXED HERE: it is the context's flatness, `CGContextSetFlatness`'s
	 * value, whose default 0.1 is the constant this line used to carry. The doubling duty is the
	 * division — the caller's number is in DEVICE PIXELS (Apple's words, CGContext.h) and the path handed
	 * to the flattener is in USER space — which is exactly why the parameter belongs here and not at the
	 * flattener, where the space would have to be guessed. */
	{
		double a = c->state.ctm.a, b = c->state.ctm.b, cc = c->state.ctm.c, d = c->state.ctm.d;
		double scale = (a < 0 ? -a : a) + (b < 0 ? -b : b) + (cc < 0 ? -cc : cc) +
			       (d < 0 ? -d : d);
		double flatness = c->state.flatness > 0.0 ? c->state.flatness : 0.1;
		CGPathRef flat;

		if (scale < 1e-6) {
			scale = 1.0;
		}
		flat = CGPathCreateCopyByFlattening(path, flatness / scale);
		if (flat != NULL) {
			CGPathApply(flat, &fl, cg_flatten_element);
			CGPathRelease(flat);
		}
	}
	cg_close_subpath(&fl);
	cg_sweep(&fl, tr, even_odd);
	free(fl.edges);
}

/* THE MASK FORMAT IS THE ANTIALIASING SWITCH, not a rounding step: pixman rasterizes the same
 * trapezoids into an 8-bit mask when coverage is wanted and a 1-BIT mask when it is not, so "no
 * antialiasing" is exact rather than approximated. Asked once, so the choice cannot drift between
 * two copies of the same composite. */
/* !! `cg_white_source` STOOD HERE AND WAS REMOVED (2026-10-07, §52): a solid 1x1 WHITE source, built to
 * stamp a coverage mask out of trapezoids through `pixman_composite_trapezoids`. THE BOX CHANGE MADE IT
 * DEAD — `cg_coverage_from_traps_boxed` rasterises the trapezoids directly, because that function cannot
 * offset them into a smaller destination — and THE LESSON IT CARRIED IS KEPT HERE, because it cost a
 * debugging round and is about `pixman_image_create_bits` rather than about this function: ***it does NOT
 * copy the data it is given, it keeps the POINTER***, so a source built over a LOCAL `pixel` points every
 * image at a stack slot that dies on return and the mask comes out ALL ZERO — a clip that paints nothing,
 * which is worse than a refusal because it is silent. MEASURED both ways at the time: the same calls in a
 * scope that outlived the composite gave correct edge coverage (32/96/159/223), the local one gave 0. */

/* A COVERAGE MASK OVER A BOX, STAMPED FROM TRAPEZOIDS. a8 ALWAYS, even when the fill's own mask format
 * would be a1, because coverage is the point of a mask and a 1-bit clip is a different feature.
 * `pixman_image_create_bits` ZEROES the image, so anywhere the path does not cover is coverage 0 rather
 * than uninitialised.
 *
 * WHY IT TAKES A BOX, AND IT IS MEASURED: this used to be the full surface for EVERY fill, which is the
 * §41 defect class in the fill path — a mask the size of the picture for a shape of any size. MEASURED
 * (2026-10-07, §52): a 2x2 fill under a CLIP MASK costs 5.64 ms at 1280x800 and an 800x500 fill under
 * the same clip costs 4.86 ms, so the SHAPE is irrelevant and the SURFACE is the cost; the same 2x2 fill
 * under a rectilinear clip, where no mask is built, is 0.009 ms. The traps are in DEVICE coordinates and
 * the mask's origin is the box's corner, so `pixman_composite_trapezoids` is given the NEGATIVE corner
 * as its destination offset and everything outside the box is zero by construction. */
static pixman_image_t *cg_coverage_from_traps_boxed(CGContextRef c, cg_traps *tr, int bx, int by,
						    int bw, int bh)
{
	pixman_image_t *mask;
	int i;

	(void)c;
	if (bw <= 0 || bh <= 0) {
		return NULL;
	}
	mask = pixman_image_create_bits(PIXMAN_a8, bw, bh, NULL, 0);
	if (mask == NULL) {
		return NULL;
	}
	/* THE RASTERISER DIRECTLY, AND NOT `pixman_composite_trapezoids`. That function CANNOT stamp an
	 * offset set of traps into a smaller destination: its general path calls `get_trap_extents(op, dst,
	 * …)` and clips the traps against the DESTINATION'S bounds using the traps' OWN coordinates, so a
	 * box-sized mask rejects every trap that lies outside the box — MEASURED as a fill that painted
	 * NOTHING (the blend probe's first partial-coverage plus-darker fill read 0.0 everywhere, and its
	 * `R == A` check passed VACUOUSLY at 0 == 0). `pixman_rasterize_trapezoid` is the same rasteriser
	 * without that clip: it ADDS `x_off`/`y_off` to the trap (`t = trap->top + y_off_fixed` in
	 * pixman-trap.c), which is exactly the box origin this needs, and it clips to the mask it is given.
	 * The coverage it writes is the coverage the composite path would have written — the two use one
	 * rasteriser — which is why the full-surface form below can delegate here and keep the clip's own
	 * probe green. */
	for (i = 0; i < tr->count; i++) {
		if (pixman_trapezoid_valid(&tr->traps[i])) {
			pixman_rasterize_trapezoid(mask, &tr->traps[i], -bx, -by);
		}
	}
	return mask;
}

/* THE WHOLE-SURFACE FORM, for the two callers whose mask IS the surface by nature: a CLIP is addressed in
 * device coordinates over the picture, and the shadow's landing box is already its own bound. */
static pixman_image_t *cg_coverage_from_traps(CGContextRef c, cg_traps *tr)
{
	return cg_coverage_from_traps_boxed(c, tr, 0, 0, c->width, c->height);
}

/* THE CLIP MASK'S COVERAGE AT ONE DEVICE PIXEL, 0..255, or 255 when there is no mask. The image covers
 * the whole surface at the origin, so the index is the pixel itself. */
static unsigned int cg_clip_coverage(CGContextRef c, int x, int y)
{
	const unsigned char *d;
	int stride;

	int mx, my;

	if (c->state.clip_mask == NULL) {
		return 255u;
	}
	/* THE MASK'S BOX, NOT THE PICTURE'S: outside it the coverage is ZERO, and a device pixel outside the
	 * picture is outside because the box is clamped to the picture when it is built. The two tests are
	 * one test now. */
	mx = x - c->state.clip_mask_x;
	my = y - c->state.clip_mask_y;
	if (mx < 0 || my < 0 || mx >= pixman_image_get_width(c->state.clip_mask) ||
	    my >= pixman_image_get_height(c->state.clip_mask)) {
		return 0u;
	}
	d = (const unsigned char *)pixman_image_get_data(c->state.clip_mask);
	stride = pixman_image_get_stride(c->state.clip_mask);
	return (unsigned int)d[(size_t)my * (size_t)stride + (size_t)mx];
}

/* A 16.16 FIXED VALUE AS A DEVICE COORDINATE, ROUNDED OUTWARD — the direction `CGRectIntegral` also
 * takes in `CGContextClipToRect` above, and the safe one for a CLIP: a region rounded outward keeps at
 * least the pixels the caller asked for, where rounding inward would silently clip some of them. */
static int cg_floor_fixed(pixman_fixed_t v)
{
	return (int)floor((double)v / 65536.0);
}

static int cg_ceil_fixed(pixman_fixed_t v)
{
	return (int)ceil((double)v / 65536.0);
}

/* THE PATH CLIP. See CGContext.h for the boundary this draws: a rectilinear path is exact and
 * anything else is refused, because a clip is a REGION here and a mask is the other half. */
/* THE TRAPS' DEVICE BOX. Declared here because the CLIP builds a box-sized mask long before the fill
 * asks for one, and defined with the fill path below (§59). */
static void cg_traps_box(cg_traps *tr, int *bx, int *by, int *bw, int *bh);

static void cg_clip_to_path(CGContextRef c, CGPathRef path, int even_odd)
{
	cg_traps tr;
	pixman_region32_t path_region;
	pixman_region32_t out;
	int i;

	if (c == NULL) {
		return;
	}
	/* THE SAME REFUSAL `CGContextClipToRect` MAKES, AND FOR THE SAME REASON: under a rotation the
	 * device-space trapezoids are the rotated shape, and the region that would hold them is not the
	 * region the caller asked to keep. */
	if (c->state.ctm.b != 0.0 || c->state.ctm.c != 0.0) {
		fprintf(stderr, "CG-REFUSE: CGContextClip under a rotated or skewed CTM (the clip is a "
				"device-space region of rectangles, and a rotated path is not one)\n");
		return;
	}
	cg_traps_for_path(c, path, even_odd, &tr);
	pixman_region32_init(&path_region);
	for (i = 0; i < tr.count; i++) {
		const pixman_trapezoid_t *t = &tr.traps[i];

		/* A SLANTED SIDE MEANS THE PATH IS NOT A RECTANGLE SET, and that is where the MASK half
		 * begins: the whole path becomes an 8-bit coverage image and the clip carries BOTH halves. The
		 * test is EXACT rather than a tolerance — for every trapezoid of a rectilinear path the two
		 * points of each side share an x — so a curve flattened into many small slanted pieces takes
		 * the mask path, which is what it needs. */
		if (t->left.p1.x != t->left.p2.x || t->right.p1.x != t->right.p2.x) {
			/* THE MASK IS THE PATH'S OWN BOX, CLAMPED TO THE PICTURE, AND NOT THE PICTURE. §56
			 * measured this build at 0.5 ms for a 60x40 clip at 1280x800 against 0.4 MICROSECONDS
			 * for the rectilinear clip that needs no mask at all, and the term that did not move
			 * with the shape was the surface. Outside the box the coverage is 0, which is what a path
			 * clip IS outside its path, so no pixel changes — and a box that clamps away to nothing
			 * (a path wholly off the picture) becomes the 1x1 ZERO mask that says the clip is EMPTY,
			 * rather than a NULL this file reads as "no clip". */
			int bx_, by_, bw_, bh_;
			int mbx, mby, mbw, mbh;
			pixman_image_t *mask;

			cg_traps_box(&tr, &bx_, &by_, &bw_, &bh_);
			mbx = bx_ < 0 ? 0 : bx_;
			mby = by_ < 0 ? 0 : by_;
			if (bx_ + bw_ > c->width) {
				bw_ = c->width - bx_;
			}
			if (by_ + bh_ > c->height) {
				bh_ = c->height - by_;
			}
			mbw = bx_ + bw_ - mbx;
			mbh = by_ + bh_ - mby;
			if (mbw <= 0 || mbh <= 0) {
				mbx = 0;
				mby = 0;
				mbw = 1;
				mbh = 1;
			}
			mask = cg_coverage_from_traps_boxed(c, &tr, mbx, mby, mbw, mbh);

			pixman_region32_fini(&path_region);
			free(tr.traps);
			if (mask == NULL) {
				return;
			}
			cg_clip_mask_install(c, mask, mbx, mby, mbw, mbh);
			CGContextBeginPath(c);
			return;
		}
		{
			int x1 = cg_floor_fixed(t->left.p1.x);
			int y1 = cg_floor_fixed(t->top);
			int x2 = cg_ceil_fixed(t->right.p1.x);
			int y2 = cg_ceil_fixed(t->bottom);

			if (x2 > x1 && y2 > y1) {
				pixman_region32_union_rect(&path_region, &path_region, x1, y1,
							   (unsigned int)(x2 - x1),
							   (unsigned int)(y2 - y1));
			}
		}
	}
	free(tr.traps);
	/* INTERSECTED THROUGH A TEMPORARY, because `pixman_region32_intersect` takes a destination of its
	 * own rather than one of its sources. */
	pixman_region32_init(&out);
	pixman_region32_intersect(&out, &c->state.clip, &path_region);
	pixman_region32_fini(&c->state.clip);
	pixman_region32_init(&c->state.clip);
	pixman_region32_copy(&c->state.clip, &out);
	pixman_region32_fini(&out);
	pixman_region32_fini(&path_region);
	/* A CLIP CONSUMES THE CURRENT PATH, as Apple's does — the same rule the fills follow. */
	CGContextBeginPath(c);
}

/* AND THE CURRENT-PATH FORM, which is what the public doors call. Text needs the PATH form: a glyph
 * clipped out of a text run must not disturb whatever path the caller was building. */
static void cg_clip_to_current_path(CGContextRef c, int even_odd)
{
	cg_clip_to_path(c, c == NULL ? NULL : (CGPathRef)c->path, even_odd);
}

void CGContextClip(CGContextRef c)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_clip_path(c->writer, (CGPathRef)c->path, false);
		CGContextBeginPath(c);
		return;
	}
	cg_clip_to_current_path(c, 0);
}

void CGContextEOClip(CGContextRef c)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_clip_path(c->writer, (CGPathRef)c->path, true);
		CGContextBeginPath(c);
		return;
	}
	cg_clip_to_current_path(c, 1);
}

static pixman_format_code_t cg_mask_format(CGContextRef c)
{
	return (c->allows_antialiasing && c->state.antialias) ? PIXMAN_a8 : PIXMAN_a1;
}

/* THE COMPOSITE, AND THE ONLY ONE A PATH HAS: any source, at any offset, through this path's
 * coverage. `x_src`/`y_src` sample the source; the trapezoids are already in surface coordinates, so
 * the destination offsets are zero. */
/* ------------------------------------------------------------------------- */
/* the shadow: a real blur over the shape's own coverage                        */
/* ------------------------------------------------------------------------- */

/* A SEPARABLE BOX BLUR, THREE PASSES — the standard approximation of a Gaussian, and the reason it is three is
 * that one box is a visible square and three are not. HORIZONTAL THEN VERTICAL, each pass a sliding sum so the
 * cost does not grow with the radius, with the edge clamped to the border pixel (a shadow's coverage fades at
 * the edge of the surface, and reading past it would wrap the far side in).
 *
 * AND IT SWEEPS ONLY THE BOX IT IS GIVEN, which is the whole reason this function takes one. The first version
 * swept the ENTIRE SURFACE six times per shadow, so the shadow of a 130x70 panel cost 6 x 2.07M byte operations
 * at 1080p: MEASURED at 98 ms of a 194 ms drawing, half of it, for a blur whose own support is the shape's
 * extent grown by the radius. THE SEED IS THE WINDOW CENTRED ON THE BOX'S FIRST INDEX rather than on zero, and
 * the run then advances exactly as it did — so the arithmetic per pixel is unchanged, and so are the pixels.
 *
 * THE EDGE CLAMP IS STILL THE SURFACE'S AND NOT THE BOX'S: a box blur near the surface edge must read the edge
 * pixel, and clamping to the box would invent a different picture at the border. */
static void cg_box_pass(unsigned char *dst, const unsigned char *src, int w, int h, int radius,
			int vertical, int bx0, int by0, int bx1, int by1)
{
	int step = vertical ? w : 1;
	int major = vertical ? h : w;		/* the axis swept */
	int lo = vertical ? by0 : bx0;		/* ... over the box's span on that axis */
	int hi = vertical ? by1 : bx1;
	int first = vertical ? bx0 : by0;	/* one line per column, or per row */
	int last = vertical ? bx1 : by1;
	int count = 2 * radius + 1;
	int outer, line, i;

	for (line = first; line < last; line++) {
		int sum = 0;
		unsigned char *d = vertical ? dst + line : dst + (size_t)line * (size_t)w;
		const unsigned char *s = vertical ? src + line : src + (size_t)line * (size_t)w;
		/* THE RUNNING SUM, seeded with the window centred on the pixel this pass writes FIRST — the
		 * box's own first index — with the edge pixel repeated where the window runs off the surface. */
		for (i = lo - radius; i <= lo + radius; i++) {
			int k = i < 0 ? 0 : (i >= major ? major - 1 : i);

			sum += s[(size_t)k * (size_t)step];
		}
		for (outer = lo; outer < hi; outer++) {
			int out = outer - radius;
			int in = outer + radius + 1;
			int ko = out < 0 ? 0 : (out >= major ? major - 1 : out);
			int ki = in < 0 ? 0 : (in >= major ? major - 1 : in);

			d[(size_t)outer * (size_t)step] = (unsigned char)(sum / count);
			sum += s[(size_t)ki * (size_t)step] - s[(size_t)ko * (size_t)step];
		}
	}
}

/* THE COVERAGE OF A SHAPE, BLURRED. The radius arrives in DEVICE pixels, already converted by the caller, and a
 * radius of zero means the shape's own coverage untouched — which is what a hard shadow is. */
static pixman_image_t *cg_blurred_coverage(CGContextRef c, pixman_image_t *cover, int radius,
					   int bx0, int by0, int bx1, int by1)
{
	unsigned char *a;
	unsigned char *b;
	const unsigned char *in;
	int in_stride;
	int w = c->width;
	int h = c->height;
	int y;
	int pass;

	if (cover == NULL || radius <= 0) {
		return cover;
	}
	/* THE WORKING BUFFERS ARE DENSE — one row of `w` bytes after another — AND THE COVER'S OWN STRIDE IS
	 * NOT ASSUMED TO BE THAT. `cg_box_pass` walks rows `w` apart, and the coverage comes from a pixman
	 * image whose stride pixman chose. The rows are copied in and out one at a time for that reason,
	 * which is also what keeps a surface whose width is not a multiple of four from being a special
	 * case: a coverage image over a 15-wide surface has a 16-byte stride, and reading it as `w` apart
	 * would shear every row. ZEROED RATHER THAN MERELY ALLOCATED, BECAUSE THE PASSES READ OUTSIDE THE
	 * BOX: a pass centred on the box's own edge reads `radius` pixels beyond it, and what it must find
	 * there is the coverage's real value — which is zero, since the box already contains every pixel the
	 * shape touches. A malloc'd buffer would hand it whatever happened to be there. */
	a = calloc((size_t)w * (size_t)h, 1);
	b = calloc((size_t)w * (size_t)h, 1);
	if (a == NULL || b == NULL) {
		free(a);
		free(b);
		return cover;	/* a blur that cannot be allocated leaves the hard shadow rather than nothing */
	}
	in_stride = pixman_image_get_stride(cover);
	in = (const unsigned char *)pixman_image_get_data(cover);
	for (y = by0; y < by1; y++) {
		memcpy(a + (size_t)y * (size_t)w, in + (size_t)y * (size_t)in_stride, (size_t)w);
	}
	/* THE PASS RADII SUM TO THE CALLER'S `blur`, one box blur per third of it: three is the count that
	 * keeps a single box's square profile from showing, and a blur smaller than three units gets fewer,
	 * smaller boxes rather than three clamped-up ones that would reach further than the caller asked
	 * for.
	 *
	 * EVERY PASS SWEEPS THE SAME BOX, AND THAT IS EXACT RATHER THAN APPROXIMATE. A pass sweeping box B
	 * reads its input over B grown by its own radius; the part of that lying outside B is read from the
	 * zeroed buffer, and the true value there is zero as well — because a pass's support is the shape
	 * grown by the radii BEFORE it, which is inside B = the shape grown by `radius`. So no pass needs a
	 * value from outside B, and no value inside B is computed from a stale one. */
	for (pass = 0; pass < 3; pass++) {
		int r = radius / 3 + (pass < radius % 3 ? 1 : 0);

		if (r < 1) {
			continue;
		}
		cg_box_pass(b, a, w, h, r, 0, bx0, by0, bx1, by1);
		cg_box_pass(a, b, w, h, r, 1, bx0, by0, bx1, by1);
	}
	{
		pixman_image_t *out;
		unsigned char *out_data;
		int out_stride;

		out = pixman_image_create_bits(PIXMAN_a8, w, h, NULL, 0);
		if (out == NULL) {
			free(a);
			free(b);
			return cover;
		}
		out_stride = pixman_image_get_stride(out);
		out_data = (unsigned char *)pixman_image_get_data(out);
		/* ZEROED, THEN THE BOX IS COPIED IN: the compositor samples this mask only over the box, but a
		 * mask is uninitialized memory everywhere else, and a value that is never supposed to be read is
		 * still a value that gets read if this is ever wrong. */
		memset(out_data, 0, (size_t)out_stride * (size_t)h);
		for (y = by0; y < by1; y++) {
			memcpy(out_data + (size_t)y * (size_t)out_stride, a + (size_t)y * (size_t)w,
			       (size_t)w);
		}
		free(a);
		free(b);
		pixman_image_unref(cover);
		return out;
	}
}

/* THE SHADOW ITSELF: the shape's coverage, blurred, painted in the shadow colour, offset — and composited
 * BEFORE the shape so that it is behind it. THE OFFSET IS BASE-SPACE AND BECOMES A DEVICE OFFSET THROUGH THE
 * CTM's LINEAR PART, which is the same transformation every other coordinate takes. */
static void cg_paint_shadow(CGContextRef c, CGPathRef path, int even_odd)
{
	cg_traps tr;
	pixman_image_t *cover;
	pixman_image_t *src;
	uint32_t pixel;
	double scale;
	int radius;
	int dx, dy;
	int sx0, sy0, sx1, sy1;		/* the shape's own device box, read off its traps */
	int bx0, by0, bx1, by1;		/* ... grown by the blur radius: what the shadow can reach */
	int lx0, ly0, lx1, ly1;		/* ... moved by the offset: where it lands on the surface */

	if (c->state.shadow_rgba[3] <= 0.0) {
		return;	/* a fully transparent shadow colour is Apple's way of saying OFF */
	}
	scale = fabs((double)c->state.ctm.a * (double)c->state.ctm.d
		     - (double)c->state.ctm.b * (double)c->state.ctm.c);
	scale = sqrt(scale);
	radius = (int)((double)c->state.shadow_blur * scale + 0.5);
	dx = (int)floor((double)c->state.ctm.a * (double)c->state.shadow_offset.width
			+ (double)c->state.ctm.c * (double)c->state.shadow_offset.height);
	dy = (int)floor((double)c->state.ctm.b * (double)c->state.shadow_offset.width
			+ (double)c->state.ctm.d * (double)c->state.shadow_offset.height);

	cg_traps_for_path(c, path, even_odd, &tr);
	if (tr.count == 0) {
		free(tr.traps);
		return;
	}
	/* THE SHAPE'S OWN DEVICE BOX, READ OFF THE TRAPS THE WAY THE CLIP READS THEM — and read HERE, while
	 * the traps are still alive, because everything below is bounded to it. */
	sx0 = cg_floor_fixed(tr.traps[0].left.p1.x);
	sy0 = cg_floor_fixed(tr.traps[0].top);
	sx1 = cg_ceil_fixed(tr.traps[0].right.p1.x);
	sy1 = cg_ceil_fixed(tr.traps[0].bottom);
	{
		int t;

		for (t = 1; t < tr.count; t++) {
			int x0 = cg_floor_fixed(tr.traps[t].left.p1.x);
			int y0 = cg_floor_fixed(tr.traps[t].top);
			int x1 = cg_ceil_fixed(tr.traps[t].right.p1.x);
			int y1 = cg_ceil_fixed(tr.traps[t].bottom);

			if (x0 < sx0) sx0 = x0;
			if (y0 < sy0) sy0 = y0;
			if (x1 > sx1) sx1 = x1;
			if (y1 > sy1) sy1 = y1;
		}
	}
	cover = cg_coverage_from_traps(c, &tr);
	free(tr.traps);
	if (cover == NULL) {
		return;
	}
	/* THE BOX THE SHADOW CAN REACH, AND EVERY STEP FROM HERE IS BOUNDED TO IT. The coverage is zero
	 * outside the shape's box, a blur of reach `radius` can only carry it `radius` further, and the
	 * offset then moves the result — so the blur sweeps `b` (the shape grown by the radius, clamped to
	 * the surface) and the composite writes `l` (that same box, moved). WHY IT MATTERS, MEASURED: with
	 * neither bound, the blur of ONE 130x70 panel's shadow swept the entire 1920x1080 surface six times
	 * and cost 98 ms of a 194 ms drawing — half of it, for a blur whose own support is 166x106. */
	bx0 = sx0 - radius; if (bx0 < 0) bx0 = 0;
	by0 = sy0 - radius; if (by0 < 0) by0 = 0;
	bx1 = sx1 + radius; if (bx1 > c->width) bx1 = c->width;
	by1 = sy1 + radius; if (by1 > c->height) by1 = c->height;
	if (bx1 <= bx0 || by1 <= by0) {
		pixman_image_unref(cover);	/* the shape is not on the surface, so there is no shadow */
		return;
	}
	lx0 = bx0 + dx; if (lx0 < 0) lx0 = 0;
	ly0 = by0 + dy; if (ly0 < 0) ly0 = 0;
	lx1 = bx1 + dx; if (lx1 > c->width) lx1 = c->width;
	ly1 = by1 + dy; if (ly1 > c->height) ly1 = c->height;
	if (lx1 <= lx0 || ly1 <= ly0) {
		pixman_image_unref(cover);	/* the offset carried the whole shadow off the surface */
		return;
	}
	/* THE COVERAGE IS BLURRED FIRST, so the shape's own edge is inside the blur rather than at the edge
	 * of a surface something padded around it. */
	cover = cg_blurred_coverage(c, cover, radius, bx0, by0, bx1, by1);
	pixel = cg_premultiplied_pixel(c->state.shadow_rgba[0], c->state.shadow_rgba[1],
				       c->state.shadow_rgba[2], c->state.shadow_rgba[3]);
	src = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, &pixel, 4);
	if (src != NULL) {
		pixman_image_set_repeat(src, PIXMAN_REPEAT_NORMAL);
		pixman_image_set_clip_region32(c->image, &c->state.clip);
		/* THE OFFSET IS THE MASK'S OWN ORIGIN. pixman samples a mask at `mask_x + x` for destination
		 * pixel x, so a `mask_x` of `-dx` covers that pixel with the shape's coverage at `x - dx` —
		 * the shadow moved right by dx, with whatever lands past the surface's edge simply not
		 * sampled. MEASURED, BECAUSE THE SPELLINGS ARE NOT EQUIVALENT: offsetting the DESTINATION
		 * instead (`dest_x = dx`) puts the shadow in the same place, but it moves the composite's
		 * region with it, and so changes which pixels pixman clips.
		 *
		 * A PADDED MASK WAS TRIED FIRST AND DID NOT WORK AT ALL — for two separate reasons. It cost a
		 * whole surface of allocation and copying per fill, and its row stride was
		 * `c->width + |offset|`, which is not generally a multiple of four: `pixman_image_create_bits`
		 * REFUSES such a stride by returning NULL, so the shadow silently never appeared. Both are
		 * gone, and the coverage is composited where it is, with the offset carried by the mask
		 * coordinates instead.
		 *
		 * AND THE REGION IS `l`, THE BOX THE SHADOW LANDS IN: outside it the mask is zero by
		 * construction, so compositing it would be work with no answer. The mask coordinates follow
		 * the region — `l` minus the offset is the mask's own corner — which is the rule above said
		 * in terms of the box instead of the surface's corner. */
		pixman_image_composite32(PIXMAN_OP_OVER, src, cover, c->image, 0, 0, lx0 - dx, ly0 - dy, lx0,
					 ly0, lx1 - lx0, ly1 - ly0);
		pixman_image_unref(src);
	}
	pixman_image_unref(cover);
}

/* A 0..1 COLOUR COMPONENT AS A STORED BYTE, ROUNDED AND SATURATED. Here rather than reused from
 * CGPaint.c's `cg_clamp01` because that one is static to its file and this needs the byte, not the
 * clamp. */
static unsigned char cg_blend_byte(double v)
{
	if (v <= 0.0) {
		return 0;
	}
	if (v >= 1.0) {
		return 255;
	}
	return (unsigned char)(v * 255.0 + 0.5);
}

/*
 * PLUS-DARKER, THE ONE BLEND MODE WITH NO PIXMAN OPERATOR (CGContext.h), COMPOSITED BY HAND.
 *
 * THE ARITHMETIC IS THE PDF BLEND FORMULA ON PREMULTIPLIED COMPONENTS, and it is written once, here,
 * because every paint that can blend plus-darker routes through it. For a source `(Cs, As)` over a
 * backdrop `(Cd, Ad)`, with the blend function `B(s, d) = max(0, s + d - 1)`:
 *
 *     Cr = (1 - As)·Cd + (1 - Ad)·Cs + As·Ad·B(Cs/As, Cd/Ad)
 *     Ar = As + Ad - As·Ad
 *
 * AND THE PREMULTIPLICATIONS CANCEL OUT OF THE BLEND TERM, WHICH IS WHY NOTHING IS EVER DIVIDED:
 * `As·Ad·max(0, Cs/As + Cd/Ad - 1)` is `max(0, Ad·Cs + As·Cd - As·Ad)` (scaling by a non-negative
 * factor is exact inside a max), so the loop works entirely in the stored premultiplied channels and
 * has no divide-by-zero case at a transparent pixel. `coregraphics_blend` checks the two identities
 * this falls out of: both operands opaque gives `max(0, Cs + Cd - 1)`, and a transparent backdrop
 * gives `Cs` back unchanged.
 *
 * THE MASK IS THE PAINT'S OWN COVERAGE (a path's, the clip's, or a glyph's), an A8 image; a NULL
 * mask is full coverage. The source may be a 1x1 colour that repeats or a raster over the clip's
 * box, so its index is CLAMPED — the loop's one departure from pixman's repeat rules, and an
 * invisible one, because every caller either aligns the source with the box or hands a 1x1, where
 * clamping and repeating agree. A solid-fill source has no readable bits and is refused by name; the
 * one caller that had one (the glyph fill) hands a 1x1 bits image instead.
 *
 * THE CLIP REGION IS TESTED PER PIXEL, because this loop writes `c->data` directly and so cannot
 * lean on `pixman_image_set_clip_region32` on the destination the way the pixman composites do. It
 * is the same test `CGContextDrawImage`'s own hand loop makes, for the same reason.
 */
static void cg_composite_plus_darker(CGContextRef c, pixman_image_t *src, pixman_image_t *mask,
				     int sx, int sy, int mx, int my, int dx, int dy, int w, int h)
{
	const unsigned char *sbits = (const unsigned char *)pixman_image_get_data(src);
	const unsigned char *mbits = mask != NULL
				     ? (const unsigned char *)pixman_image_get_data(mask) : NULL;
	int sw = pixman_image_get_width(src);
	int sh = pixman_image_get_height(src);
	int sstride = pixman_image_get_stride(src);
	int mstride = mask != NULL ? pixman_image_get_stride(mask) : 0;
	/* AND WHERE RED SITS IN THE DESTINATION, which is the CONTEXT's order and not the source's: the
	 * sources this path is handed are pixman images this tree made as a8r8g8b8 (B,G,R,A) and stay that
	 * way. The destination is the caller's buffer, and `cg_channel_slots` is where its order is decided. */
	int slots[3];
	int ro;
	int x;
	int y;

	cg_channel_slots(c->bitmap_info, slots);
	ro = slots[0];

	if (sbits == NULL) {
		fprintf(stderr, "CG-REFUSE: plus-darker needs a readable source and this one is a solid "
				"fill: give it a 1x1 bits image\n");
		return;
	}
	for (y = dy; y < dy + h; y++) {
		for (x = dx; x < dx + w; x++) {
			unsigned char *d;
			const unsigned char *sp;
			double sa, da, ao, cover = 1.0;
			double sc[3], dc[3];
			int sxi, syi;
			int k;

			if (x < 0 || y < 0 || x >= c->width || y >= c->height) {
				continue;
			}
			if (!pixman_region32_contains_point(&c->state.clip, x, y, NULL)) {
				continue;
			}
			sxi = sx + (x - dx);
			syi = sy + (y - dy);
			if (sw <= 1) {
				sxi = 0;
			} else if (sxi < 0) {
				sxi = 0;
			} else if (sxi >= sw) {
				sxi = sw - 1;
			}
			if (sh <= 1) {
				syi = 0;
			} else if (syi < 0) {
				syi = 0;
			} else if (syi >= sh) {
				syi = sh - 1;
			}
			sp = sbits + (size_t)syi * (size_t)sstride + (size_t)sxi * 4u;
			if (mbits != NULL) {
				cover = (double)mbits[(size_t)(my + (y - dy)) * (size_t)mstride
						      + (size_t)(mx + (x - dx))] / 255.0;
			}
			d = c->data + (size_t)y * (size_t)c->stride + (size_t)x * 4u;
			/* THE MEMORY ORDER IS B, G, R, A — the one chart this library draws in (CGPaint.c).
			 *
			 * AND THE COVERAGE MULTIPLIES THE WHOLE PREMULTIPLIED SOURCE, NOT JUST ITS ALPHA. A mask
			 * is a coverage multiplier on the source PIXEL, so the masked source is `(Cs*M, As*M)`;
			 * scaling only `As` leaves `Cs` at full strength, which makes every ANTIALIASED EDGE of a
			 * plus-darker shape come out too bright — a half-covered red pixel at alpha 0.5 would carry
			 * full red and unpremultiply to a saturated red. THAT WAS THE BUG THIS LOOP SHIPPED WITH
			 * (§44), and the first version of the probe could not see it because its check took only
			 * full-coverage pixels; §51 adds the partial-coverage one. */
			sa = (double)sp[3] / 255.0 * cover;
			sc[0] = (double)sp[2] / 255.0 * cover;
			sc[1] = (double)sp[1] / 255.0 * cover;
			sc[2] = (double)sp[0] / 255.0 * cover;
			da = (double)d[3] / 255.0;
			dc[0] = (double)d[ro] / 255.0;
			dc[1] = (double)d[1] / 255.0;
			dc[2] = (double)d[2 - ro] / 255.0;
			for (k = 0; k < 3; k++) {
				/* RED, GREEN AND BLUE LIVE AT THE CONTEXT'S OWN OFFSETS, which are 2, 1, 0 for a
				 * B,G,R,A canvas and 0, 1, 2 for an R,G,B,A one. */
				const int slot[3] = { ro, 1, 2 - ro };
				double inner = da * sc[k] + sa * dc[k] - sa * da;

				if (inner < 0.0) {
					inner = 0.0;
				}
				d[slot[k]] = cg_blend_byte((1.0 - sa) * dc[k] + (1.0 - da) * sc[k]
							   + inner);
			}
			ao = sa + da - sa * da;
			d[3] = cg_blend_byte(ao);
		}
	}
}

/* THE ONE COMPOSITE EVERY BLEND-CARRYING PAINT GOES THROUGH. For every operator but plus-darker it
 * IS `pixman_image_composite32`, unchanged; plus-darker takes the hand path above. It exists so the
 * sentinel never reaches pixman through a path nobody remembered to guard: a new caller that wants
 * a blend mode should call this rather than `pixman_image_composite32` directly. */
static void cg_composite(CGContextRef c, pixman_op_t op, pixman_image_t *src, pixman_image_t *mask,
			 int sx, int sy, int mx, int my, int dx, int dy, int w, int h)
{
	if (op != CG_OP_PLUS_DARKER) {
		pixman_image_composite32(op, src, mask, c->image, sx, sy, mx, my, dx, dy, w, h);
		return;
	}
	cg_composite_plus_darker(c, src, mask, sx, sy, mx, my, dx, dy, w, h);
}

/* THE BOX THE TRAPEZOIDS THEMSELVES OCCUPY, rounded OUTWARD so nothing the shape covers is left out.
 *
 * WHY IT EXISTS, AND IT IS MEASURED: a composite is bounded by the DESTINATION'S CLIP REGION, and for the
 * operators that are not source-over pixman sweeps that whole region however small the shape is. A 2x2
 * fill with `kCGBlendModeMultiply` costs **12.3 ms** over an unclipped 1280x800 surface and **0.049 ms**
 * under a 20x20 clip; a 1000x600 shape under that same clip is 0.129 ms — so the cost is the CLIP and not
 * the shape (measured 2026-10-07, §51). Source-over does not pay it: the same 2x2 fill is 0.009 ms.
 *
 * AND NARROWING TO THE SHAPE'S OWN BOX IS EXACT RATHER THAN AN APPROXIMATION: outside the trapezoids the
 * coverage is zero, and pixman's compositing model interpolates by the mask —
 * `out = mask*op(src,dst) + (1-mask)*dst` — so a zero mask leaves the destination untouched FOR EVERY
 * OPERATOR, which is what makes this safe for the ones that ignore the destination (`Copy`, `Clear`).
 * The picture is compared byte for byte after the change rather than argued about. */
static void cg_traps_box(cg_traps *tr, int *bx, int *by, int *bw, int *bh)
{
	pixman_fixed_t x1, y1, x2, y2;
	int i;

	*bx = *by = *bw = *bh = 0;
	if (tr->count <= 0) {
		return;
	}
	x1 = tr->traps[0].left.p1.x;
	x2 = x1;
	y1 = tr->traps[0].top;
	y2 = tr->traps[0].bottom;
	for (i = 0; i < tr->count; i++) {
		pixman_trapezoid_t *t = &tr->traps[i];
		pixman_fixed_t xs[4];
		int k;

		xs[0] = t->left.p1.x;
		xs[1] = t->left.p2.x;
		xs[2] = t->right.p1.x;
		xs[3] = t->right.p2.x;
		for (k = 0; k < 4; k++) {
			if (xs[k] < x1) {
				x1 = xs[k];
			}
			if (xs[k] > x2) {
				x2 = xs[k];
			}
		}
		if (t->top < y1) {
			y1 = t->top;
		}
		if (t->bottom > y2) {
			y2 = t->bottom;
		}
	}
	*bx = cg_floor_fixed(x1);
	*by = cg_floor_fixed(y1);
	*bw = cg_ceil_fixed(x2) - *bx;
	*bh = cg_ceil_fixed(y2) - *by;
}

static void cg_composite_traps(CGContextRef c, cg_traps *tr, pixman_op_t op, pixman_image_t *src,
			       int x_src, int y_src)
{
	pixman_region32_t bounded;
	int bx, by, bw, bh;

	if (tr->count == 0 || src == NULL) {
		return;
	}
	/* THE DESTINATION IS BOUNDED BY THE SHAPE AND NOT BY THE CLIP — see `cg_traps_box`. The caller's
	 * clip still bounds it (the two are INTERSECTED), so this narrows and never widens. */
	cg_traps_box(tr, &bx, &by, &bw, &bh);
	pixman_region32_init_rect(&bounded, bx, by, (unsigned int)(bw > 0 ? bw : 0),
				  (unsigned int)(bh > 0 ? bh : 0));
	pixman_region32_intersect(&bounded, &bounded, &c->state.clip);
	pixman_image_set_clip_region32(c->image, &bounded);
	if (op == CG_OP_PLUS_DARKER) {
		/* PLUS-DARKER CANNOT GO THROUGH pixman_composite_trapezoids (no operator — CGContext.h), so
		 * the coverage is materialised into the SAME A8 mask the clip branch below already builds and
		 * the hand composite reads it. THE CLIP IS FOLDED IN WHEN THERE IS ONE, so this path and the
		 * operator path see the same coverage; and it comes through the antialiased builder whether or
		 * not `CGContextSetShouldAntialias` is off, which is the property that branch already has. */
		pixman_image_t *cover = cg_coverage_from_traps_boxed(c, tr, bx, by, bw, bh);

		if (cover == NULL) {
			pixman_region32_fini(&bounded);
			return;
		}
		if (c->state.clip_mask != NULL) {
			/* THE COVER IS THE BOX'S IMAGE AND THE CLIP'S BOX IS ITS OWN, so the clip is read where
			 * BOTH boxes say the box's corner is: the offset between the two origins. Outside the
			 * clip's box the source is transparent, `IN` writes zero, and that is the clip being empty
			 * there. */
			pixman_image_composite32(PIXMAN_OP_IN, c->state.clip_mask, NULL, cover,
						 bx - c->state.clip_mask_x, by - c->state.clip_mask_y,
						 0, 0, 0, 0, bw, bh);
		}
		cg_composite(c, op, src, cover, x_src, y_src, 0, 0, bx, by, bw, bh);
		pixman_image_unref(cover);
		pixman_region32_fini(&bounded);
		return;
	}
	if (c->state.clip_mask == NULL) {
		pixman_composite_trapezoids(op, src, c->image, cg_mask_format(c), x_src, y_src, 0, 0,
					    tr->count, tr->traps);
		pixman_region32_fini(&bounded);
		return;
	}
	/* THE PATH'S COVERAGE TIMES THE CLIP'S, WHICH TAKES A TEMPORARY: pixman composites through ONE
	 * mask and here the mask is the PRODUCT of two — the fill's own coverage and the clip's — so the
	 * product is built and the source goes through it. `IN` multiplies the clip into the path's
	 * coverage, which is exactly the intersection a clip is. */
	{
		pixman_image_t *cover = cg_coverage_from_traps_boxed(c, tr, bx, by, bw, bh);

		if (cover == NULL) {
			pixman_region32_fini(&bounded);
			return;
		}
		pixman_image_composite32(PIXMAN_OP_IN, c->state.clip_mask, NULL, cover,
					 bx - c->state.clip_mask_x, by - c->state.clip_mask_y, 0, 0, 0, 0,
					 bw, bh);
		/* THE COVER IS THE BOX'S OWN IMAGE, so its mask coordinates are its origin and the DESTINATION
		 * carries the box's corner. */
		cg_composite(c, op, src, cover, x_src, y_src, 0, 0, bx, by, bw, bh);
		pixman_image_unref(cover);
	}
	pixman_region32_fini(&bounded);
}

/* A FILL WITH A SOURCE SOMEONE ELSE BUILT, which is what C6's sampled paints need: the caller has a
 * device-space image and an offset into it, and everything else is the shape. */
static void cg_fill_path_with_source(CGContextRef c, CGPathRef path, int even_odd, pixman_op_t op,
				     pixman_image_t *src, int x_src, int y_src)
{
	cg_traps tr;

	cg_traps_for_path(c, path, even_odd, &tr);
	cg_composite_traps(c, &tr, op, src, x_src, y_src);
	free(tr.traps);
}

/* AND THE CONSTANT-COLOUR FILL, WHICH IS THE SAME THING WITH A 1×1 SOURCE. This is the whole
 * difference between C2's fill and C6's: a colour is a paint whose image happens to be one pixel. */
static int cg_fill_path(CGContextRef c, CGPathRef path, int even_odd, pixman_op_t op,
			CGFloat r, CGFloat g, CGFloat b, CGFloat a)
{
	pixman_image_t *src;
	uint32_t pixel = cg_premultiplied_pixel(r, g, b, a);

	src = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, &pixel, 4);
	if (src == NULL) {
		return -1;
	}
	/* A 1×1 SOURCE MUST BE TOLD TO REPEAT, or only the FIRST destination pixel samples it: the
	 * composite reads the source at (x_src + x, y_src + y) for every pixel, so with
	 * PIXMAN_REPEAT_NONE every coordinate but (0,0) lands outside the image and reads
	 * transparent. MEASURED — C2's first probe run painted exactly one pixel, and the byte check
	 * at (0,0) passing while every other pixel stayed empty is what identified it. */
	pixman_image_set_repeat(src, PIXMAN_REPEAT_NORMAL);
	cg_fill_path_with_source(c, path, even_odd, op, src, 0, 0);
	pixman_image_unref(src);
	return 0;
}

/* ------------------------------------------------------------------------- */
/* C6.3: the pattern paint                                                   */
/* ------------------------------------------------------------------------- */

/* FORWARD, AND IT IS THE CLIP'S CORNER THE PATTERN PAINT NEEDS: `cg_paint_extents` is defined with
 * the rest of the paint code just below, and the dispatcher above it asks for the rectangle before
 * that definition has been read. Nothing here is recursive; the order is only about where the reader
 * finds the paint substrate. */
static int cg_paint_extents(CGContextRef c, int *px, int *py, int *pw, int *ph);

/* WHAT A PATTERN'S SAMPLER NEEDS TO KNOW, gathered per fill and thrown away with it: the pattern, the
 * cell rendered from it, and the phase the context had when the fill happened. */
typedef struct {
	CGPatternRef pattern;
	cg_pattern_cell *cell;
	CGSize phase;
} cg_pattern_paint;

static void cg_pattern_paint_eval(void *info, CGFloat x, CGFloat y, CGFloat rgba[4])
{
	cg_pattern_paint *pp = info;

	/* `x` AND `y` ARRIVE IN USER SPACE, which is what the paint substrate hands every sampler and
	 * what a pattern's phase and matrix are expressed against — so the sampler does the rest. */
	cg_pattern_sample(pp->pattern, pp->cell, pp->phase, x, y, rgba);
}

/*
 * THE ONE PLACE A PATH'S PAINT IS DECIDED: a colour or a pattern, and nothing else may be either.
 * Every fill and every stroke in this file comes through here, which is what keeps a pattern from
 * working for fills and silently not for strokes — the mistake a second dispatch inside the stroke
 * would eventually produce.
 *
 * A DEVIATION, AND IT IS A VISIBLE ONE, SO IT IS WRITTEN DOWN WHERE THE SAMPLING HAPPENS. The paint
 * substrate hands a sampler the point in the context's CURRENT user space — that is what every other
 * paint wants, and it is what makes a gradient follow the CTM. Apple anchors a pattern in the DEFAULT
 * user space instead, so that setting a CTM and then filling with a pattern leaves the tiles' size
 * alone while this library scales them with the CTM. With no CTM set — which is what a caller who
 * wants a fixed pattern does — the two spaces are the same up to the y flip and the pictures agree.
 * Closing the gap means the context tracking the default user space separately from the current one,
 * which is a change to the graphics state rather than to the pattern, and it is not in this slice.
 */
static void cg_paint_path(CGContextRef c, CGPathRef path, int even_odd, pixman_op_t op,
			  const CGFloat *rgba, CGPatternRef pattern, CGFloat alpha)
{
	pixman_image_t *src;
	cg_pattern_cell *cell;
	cg_pattern_paint info;
	int px;
	int py;
	int pw;
	int ph;

	if (c == NULL || path == NULL) {
		return;
	}
	/* THE SHADOW GOES DOWN FIRST, WHICH IS THE ONLY ORDER THAT WORKS: it is behind the shape, so drawing
	 * it afterwards would paint over what casts it. This is the one place every path-shaped paint passes
	 * through — fills, strokes, text and patterns — so a shadow arrives with all of them. */
	cg_paint_shadow(c, path, even_odd);
	if (pattern == NULL) {
		if (rgba == NULL) {
			return;
		}
		/* A COLOUR'S ALPHA IS ITS OWN TIMES THE CONTEXT'S, which is why the multiply is here and not
		 * in the caller: for a pattern there is no colour alpha, and the pattern's own alpha has
		 * already been folded into `alpha` by whoever set the paint. */
		cg_fill_path(c, path, even_odd, op, rgba[0], rgba[1], rgba[2], rgba[3] * alpha);
		return;
	}
	if (!cg_paint_extents(c, &px, &py, &pw, &ph)) {
		return;
	}
	/* THE CELL IS RENDERED PER FILL AND LET GO — see CGPattern_internal.h for why it is not cached. */
	cell = cg_pattern_render_cell(pattern);
	if (cell == NULL) {
		return;
	}
	info.pattern = pattern;
	info.cell = cell;
	info.phase = c->state.pattern_phase;
	src = cg_paint_image(px, py, pw, ph, c->state.ctm, alpha, cg_pattern_paint_eval, &info);
	cg_pattern_cell_free(cell);
	if (src == NULL) {
		return;
	}
	/* THE PAINT IMAGE'S ORIGIN IS THE CLIP'S CORNER, so the trapezoid composite samples it from
	 * there: `x_src`/`y_src` are the offsets a 1x1 colour never needed, and getting their sign wrong
	 * shifts the whole pattern by the clip's corner. */
	cg_fill_path_with_source(c, path, even_odd, op, src, -px, -py);
	pixman_image_unref(src);
}

/* CLEARING A PATTERN IS A RELEASE AND A NULL, in one place because three callers do it: setting a
 * colour, setting a new pattern, and nothing else. */
static void cg_clear_pattern(CGPatternRef *slot)
{
	CGPatternRelease(*slot);
	*slot = NULL;
}

/* THE DEVICE RECTANGLE A CLIP-ONLY PAINT COVERS: the clipping region's bounding box, intersected with
 * the surface. IT IS ASKED ONCE AND USED TWICE — by the paint builder, which sizes its image to it,
 * and by the composite, which paints exactly that many pixels — and that is the point of splitting
 * it out: a builder and a composite that each computed their own extent could disagree by a row and
 * leave a stripe of the surface unpainted, which is the kind of wrong picture that looks deliberate.
 */
static int cg_paint_extents(CGContextRef c, int *px, int *py, int *pw, int *ph)
{
	pixman_box32_t box = *pixman_region32_extents(&c->state.clip);
	int x = box.x1 < 0 ? 0 : box.x1;
	int y = box.y1 < 0 ? 0 : box.y1;
	int x2 = box.x2 > c->width ? c->width : box.x2;
	int y2 = box.y2 > c->height ? c->height : box.y2;

	*px = x;
	*py = y;
	*pw = x2 - x;
	*ph = y2 - y;
	return *pw > 0 && *ph > 0;
}

/* PAINTING THE CLIP, WITH NO PATH IN IT AT ALL. A gradient's draw verbs fill the current clipping
 * region and are indifferent to the current path (CGContext.h says why), so there is no mask to
 * build: the clip is already set on the destination and the composite is the whole operation. */
static void cg_paint_clip(CGContextRef c, pixman_image_t *src, pixman_op_t op)
{
	int px, py, pw, ph;

	if (c == NULL || src == NULL || !cg_paint_extents(c, &px, &py, &pw, &ph)) {
		return;
	}
	pixman_image_set_clip_region32(c->image, &c->state.clip);
	/* THE MASK SLOT IS FREE HERE, which makes this the one composite that needs no temporary: the mask is
	 * sampled at the same device coordinates the paint is drawn over, less ITS OWN ORIGIN. `cg_composite`
	 * is the same call for every operator but plus-darker, which it composites by hand. */
	cg_composite(c, op, src, c->state.clip_mask, 0, 0,
		     c->state.clip_mask != NULL ? px - c->state.clip_mask_x : 0,
		     c->state.clip_mask != NULL ? py - c->state.clip_mask_y : 0, px, py, pw, ph);
}

static void cg_fill_current_path(CGContextRef c, int even_odd)
{
	CGFloat a;

	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_paint_state ps;

		fn_pdf_paint_state(c, &ps);
		cg_pdf_writer_paint_path(c->writer, (CGPathRef)c->path,
					 even_odd ? CG_PDF_PAINT_EOFILL : CG_PDF_PAINT_FILL, &ps);
		CGContextBeginPath(c);
		return;
	}
	/* THE CONTEXT'S ALPHA MULTIPLIES THE PAINT'S, at draw time, which is Apple's contract for
	 * `CGContextSetAlpha` and the reason it is a separate field rather than a modification of the fill
	 * colour. WHICH PAINT IT IS — a colour or a pattern — is decided in one place, `cg_paint_path`. */
	a = c->state.alpha;
	if (c->state.fill_pattern != NULL) {
		cg_paint_path(c, (CGPathRef)c->path, even_odd, cg_op(c->state.blend), NULL,
			      c->state.fill_pattern, a * c->state.fill_pattern_alpha);
	} else {
		cg_paint_path(c, (CGPathRef)c->path, even_odd, cg_op(c->state.blend), c->state.rgba,
			      NULL, a);
	}
	/* A FILL CONSUMES THE PATH. */
	CGContextBeginPath(c);
}

void CGContextFillPath(CGContextRef c)
{
	cg_fill_current_path(c, 0);
}

void CGContextEOFillPath(CGContextRef c)
{
	cg_fill_current_path(c, 1);
}

static void fn_paint_scratch_path(CGContextRef c, CGPathRef scratch, int stroking);

void CGContextFillRect(CGContextRef c, CGRect rect)
{
	CGMutablePathRef scratch;

	if (c == NULL) {
		return;
	}
	/* A RECTANGLE GOES THROUGH THE SAME ROAD AS ANY OTHER PATH — no private fast
	 * path — because a rect with fractional coordinates needs the same area coverage
	 * every other shape gets, and two implementations of one geometry is how they
	 * come to disagree. The scratch path is local, so the context's current path is
	 * untouched: Apple's `FillRect` does not modify it, and the probe asserts that. */
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	CGPathAddRect(scratch, NULL, rect);
	/* ONE SPELLING OF "PAINT A SCRATCH PATH", shared with the convenience doors below: the scratch is built
	 * here and handed over, and fn_paint_scratch_path releases it. */
	fn_paint_scratch_path(c, (CGPathRef)scratch, 0);
}

void CGContextClearRect(CGContextRef c, CGRect rect)
{
	CGMutablePathRef scratch;

	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_clear_rect(c->writer, rect);
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	CGPathAddRect(scratch, NULL, rect);
	/* CLEAR MEANS TRANSPARENT BLACK, NOT "THE FILL COLOUR WITH ZERO ALPHA": premultiplied
	 * zero is transparent whatever the colour is, and REPLACE (`PIXMAN_OP_SRC`) is what
	 * makes it a clear rather than a blend onto what was there. */
	cg_fill_path(c, (CGPathRef)scratch, 0, PIXMAN_OP_SRC, 0.0, 0.0, 0.0, 0.0);
	CGPathRelease((CGPathRef)scratch);
}

/* ------------------------------------------------------------------------- */
/* stroking                                                                  */
/* ------------------------------------------------------------------------- */

/* DEFINED HERE, BESIDE THE CODE THAT READS IT, because the pattern is a field of the private
 * graphics state; its DECLARATION is in the header with the rest of the line state, which is
 * what a caller needs.
 *
 * A NULL ARRAY OR A ZERO COUNT IS THE CLEARING FORM rather than an error: that is how a caller
 * goes back to a solid line without a save and restore. */
void CGContextSetLineDash(CGContextRef c, CGFloat phase, const CGFloat *lengths, size_t count)
{
	size_t i;

	if (c == NULL) {
		return;
	}
	if (lengths == NULL || count == 0) {
		c->state.dash_count = 0;
		c->state.dash_phase = 0.0;
		return;
	}
	/* A PATTERN TOO LONG FOR THE STATE IS REFUSED AND THE STATE IS LEFT AS IT WAS: truncating
	 * it would draw dashes the caller did not ask for, which is worse than not changing them. */
	if (count > CG_DASH_STATE_MAX) {
		fprintf(stderr, "CG-REFUSE: a dash pattern of %d entries does not fit the graphics "
				"state's %d\n", (int)count, CG_DASH_STATE_MAX);
		return;
	}
	for (i = 0; i < count; i++) {
		c->state.dash[i] = lengths[i];
	}
	c->state.dash_count = (int)count;
	c->state.dash_phase = phase;
}

/* THE WHOLE OF THE CONTEXT'S STROKE IS THIS FUNCTION. The geometry comes from
 * `CGPathCreateCopyByStrokingPath` (CGPathStroke.c); what is left is to fill the outline
 * with the STROKE colour under the NON-ZERO rule. The rule is not a preference — the
 * stroked path is a set of overlapping oriented pieces, and an even-odd fill of it is not
 * the stroke (the plan's §9 records that deviation, and coregraphics_stroke.c asserts it).
 *
 * AND THE DASH GOES ON BEFORE THE STROKE, WHICH IS THE ORDER THE WHOLE DESIGN RESTS ON: the
 * dashes are PIECES OF THE PATH, so each one gets its own caps and joins. Dashing the stroked
 * OUTLINE instead would give one shape with gaps cut in it and the wrong ends.
 */
static void cg_stroke_path_with_width(CGContextRef c, CGPathRef path, CGFloat width)
{
	CGPathRef dashed = NULL;
	CGPathRef outline;
	CGFloat a;

	if (c == NULL || path == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_paint_state ps;

		fn_pdf_paint_state(c, &ps);
		ps.line_width = width;
		cg_pdf_writer_paint_path(c->writer, path, CG_PDF_PAINT_STROKE, &ps);
		return;
	}
	if (c->state.dash_count > 0) {
		dashed = CGPathCreateCopyByDashingPath(path, NULL, c->state.dash_phase,
						       c->state.dash, (size_t)c->state.dash_count);
		if (dashed != NULL) {
			path = dashed;
		}
	}
	outline = CGPathCreateCopyByStrokingPath(path, NULL, width, c->state.line_cap,
						 c->state.line_join, c->state.miter_limit);
	if (outline == NULL) {
		CGPathRelease(dashed);
		return;
	}
	/* THE CONTEXT'S ALPHA MULTIPLIES THE STROKE PAINT'S, exactly as it does for a fill, and the
	 * STROKE's pattern is the one that applies here rather than the fill's. */
	a = c->state.alpha;
	if (c->state.stroke_pattern != NULL) {
		cg_paint_path(c, outline, 0, cg_op(c->state.blend), NULL, c->state.stroke_pattern,
			      a * c->state.stroke_pattern_alpha);
	} else {
		cg_paint_path(c, outline, 0, cg_op(c->state.blend), c->state.stroke_rgba, NULL, a);
	}
	CGPathRelease(outline);
	CGPathRelease(dashed);
}

static void cg_stroke_current_path(CGContextRef c)
{
	if (c == NULL) {
		return;
	}
	cg_stroke_path_with_width(c, (CGPathRef)c->path, c->state.line_width);
	/* A STROKE CONSUMES THE PATH, exactly as a fill does. */
	CGContextBeginPath(c);
}

void CGContextStrokePath(CGContextRef c)
{
	cg_stroke_current_path(c);
}

static void cg_stroke_rect_with(CGContextRef c, CGRect rect, CGFloat width)
{
	CGMutablePathRef scratch;

	if (c == NULL) {
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	CGPathAddRect(scratch, NULL, rect);
	cg_stroke_path_with_width(c, (CGPathRef)scratch, width);
	CGPathRelease((CGPathRef)scratch);
}

void CGContextStrokeRect(CGContextRef c, CGRect rect)
{
	if (c != NULL) {
		cg_stroke_rect_with(c, rect, c->state.line_width);
	}
}

/* ------------------------------------------------------------------------- */
/* Five conveniences over machinery that already exists                        */
/* ------------------------------------------------------------------------- */

/* PAINT A SCRATCH PATH WITHOUT TOUCHING THE CALLER'S, which is the guarantee `CGContextFillRect` makes in its
 * own comment and the shape every convenience in this file follows. THE SCRATCH PATH IS HANDED OVER, not
 * released here: the fill and stroke doors CONSUME the current path (that is what they mean by emptying it), so
 * whatever ends up in `c->path` afterwards is what this function owns and releases — the door's leftover, not
 * the pointer that was passed in. */
static void fn_paint_scratch_path(CGContextRef c, CGPathRef scratch, int stroking)
{
	if (scratch == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_paint_state ps;

		fn_pdf_paint_state(c, &ps);
		if (stroking) {
			ps.line_width = c->state.line_width;
			cg_pdf_writer_paint_path(c->writer, scratch, CG_PDF_PAINT_STROKE, &ps);
		} else {
			cg_pdf_writer_paint_path(c->writer, scratch, CG_PDF_PAINT_FILL, &ps);
		}
		CGPathRelease(scratch);
		return;
	}
	/* THE HELPERS TAKE A PATH AND DO NOT CONSUME IT, so the caller's current path is untouched BY
	 * CONSTRUCTION and there is no swap to get wrong. AN EARLIER VERSION OF THIS FUNCTION SWAPPED `c->path`
	 * INSTEAD, and because a fill EMPTIES the path IN PLACE — the same pointer, its count set to zero — the
	 * pointer that came back was the caller's own, which was then released and put back: a use-after-free of
	 * the caller's path. THE PROBE IS WHAT FOUND IT, as everything after the first painted convenience in that
	 * probe painted nothing at all. */
	if (stroking) {
		cg_stroke_path_with_width(c, scratch, c->state.line_width);
	} else {
		CGFloat a = c->state.alpha;

		if (c->state.fill_pattern != NULL) {
			cg_paint_path(c, scratch, 0, cg_op(c->state.blend), NULL, c->state.fill_pattern,
				      a * c->state.fill_pattern_alpha);
		} else {
			cg_paint_path(c, scratch, 0, cg_op(c->state.blend), c->state.rgba, NULL, a);
		}
	}
	CGPathRelease(scratch);
}

CGPathRef CGContextCopyPath(CGContextRef c)
{
	return c == NULL ? NULL : CGPathCreateCopy((CGPathRef)c->path);
}

bool CGContextPathContainsPoint(CGContextRef c, CGPoint point, CGPathDrawingMode mode)
{
	if (c == NULL || c->path == NULL) {
		return false;
	}
	switch (mode) {
	case kCGPathFill:
		return CGPathContainsPoint((CGPathRef)c->path, NULL, point, false);
	case kCGPathEOFill:
		return CGPathContainsPoint((CGPathRef)c->path, NULL, point, true);
	case kCGPathStroke:
	case kCGPathFillStroke:
	case kCGPathEOFillStroke: {
		CGPathRef outline = CGPathCreateCopyByStrokingPath((CGPathRef)c->path, NULL,
								   c->state.line_width, c->state.line_cap,
								   c->state.line_join, c->state.miter_limit);
		bool hit = false;

		if (outline != NULL) {
			/* NON-ZERO, because that is what the stroker's result is FOR: it is a set of overlapping
			 * oriented pieces, and even-odd would punch holes where its own joins overlap. */
			hit = CGPathContainsPoint(outline, NULL, point, false);
			CGPathRelease(outline);
		}
		if (!hit && mode != kCGPathStroke) {
			/* Apple's sentence is "stroked OR filled", so the combined modes are the union. */
			hit = CGPathContainsPoint((CGPathRef)c->path, NULL, point,
						  mode == kCGPathEOFillStroke);
		}
		return hit;
	}
	default:
		return false;
	}
}

void CGContextAddArcToPoint(CGContextRef c, CGFloat x1, CGFloat y1, CGFloat x2, CGFloat y2, CGFloat radius)
{
	if (c != NULL) {
		CGPathAddArcToPoint((CGMutablePathRef)c->path, NULL, x1, y1, x2, y2, radius);
	}
}

void CGContextFillRects(CGContextRef c, const CGRect *rects, size_t count)
{
	CGMutablePathRef scratch;
	size_t i;

	if (c == NULL || rects == NULL || count == 0) {
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	for (i = 0; i < count; i++) {
		CGPathAddRect(scratch, NULL, rects[i]);
	}
	fn_paint_scratch_path(c, (CGPathRef)scratch, 0);
}

void CGContextFillEllipseInRect(CGContextRef c, CGRect rect)
{
	CGMutablePathRef scratch;

	if (c == NULL) {
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	CGPathAddEllipseInRect(scratch, NULL, rect);
	fn_paint_scratch_path(c, (CGPathRef)scratch, 0);
}

void CGContextStrokeEllipseInRect(CGContextRef c, CGRect rect)
{
	CGMutablePathRef scratch;

	if (c == NULL) {
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	CGPathAddEllipseInRect(scratch, NULL, rect);
	fn_paint_scratch_path(c, (CGPathRef)scratch, 1);
}

void CGContextStrokeRectWithWidth(CGContextRef c, CGRect rect, CGFloat width)
{
	/* THE WIDTH IS A PARAMETER, NOT A SETTING: the context's line width is untouched, so a
	 * caller can draw one thick frame without the next stroke inheriting it. */
	cg_stroke_rect_with(c, rect, width);
}

void CGContextStrokeLineSegments(CGContextRef c, const CGPoint *points, size_t count)
{
	CGMutablePathRef scratch;
	size_t i;

	if (c == NULL || points == NULL || count < 2) {
		return;
	}
	scratch = CGPathCreateMutable();
	if (scratch == NULL) {
		return;
	}
	/* PAIRS, AND AN ODD COORDINATE IS NOT A SEGMENT: it is dropped rather than paired with
	 * something invented, which would draw a line to the origin no caller asked for. */
	for (i = 0; i + 1 < count; i += 2) {
		CGPathMoveToPoint(scratch, NULL, points[i].x, points[i].y);
		CGPathAddLineToPoint(scratch, NULL, points[i + 1].x, points[i + 1].y);
	}
	cg_stroke_path_with_width(c, (CGPathRef)scratch, c->state.line_width);
	CGPathRelease((CGPathRef)scratch);
}

/* ------------------------------------------------------------------------- */
/* the tiled image, and the two page doors that refuse                          */
/* ------------------------------------------------------------------------- */

/* A TILE IS `CGContextDrawImage`, AND THE GRID IS APPLE'S SENTENCE: the tile at (i, j) goes at
 * `rect.origin + (i·rect.width, j·rect.height)`, so a shifted origin shifts the whole tiling and the CTM
 * transforms every tile exactly as it transforms one image. THE INDEX RANGE COMES FROM THE CLIP'S DEVICE
 * EXTENTS TAKEN BACK TO USER SPACE: the clip is what has to be filled, it is a device-space region, and the
 * corners of its bounding box through the inverse CTM give a user-space box that CONTAINS it — a box that
 * covers a little more than the clip costs a few tiles that draw nothing, and a box that covered less would
 * leave part of the clip untiled. */
void CGContextDrawTiledImage(CGContextRef c, CGRect rect, CGImageRef image)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "tiled images (CGContextDrawTiledImage)");
		return;
	}
	CGAffineTransform inverse;
	pixman_box32_t *ext;
	CGRect extents;
	CGPoint corner[4];
	double minx, maxx, miny, maxy;
	int i, j, i0, i1, j0, j1;

	if (c == NULL || image == NULL) {
		return;
	}
	if (rect.size.width <= 0.0 || rect.size.height <= 0.0) {
		return;
	}
	ext = pixman_region32_extents(&c->state.clip);
	extents = CGRectMake((CGFloat)ext->x1, (CGFloat)ext->y1, (CGFloat)(ext->x2 - ext->x1),
			     (CGFloat)(ext->y2 - ext->y1));
	inverse = CGAffineTransformInvert(c->state.ctm);
	corner[0] = CGPointMake(extents.origin.x, extents.origin.y);
	corner[1] = CGPointMake(extents.origin.x + extents.size.width, extents.origin.y);
	corner[2] = CGPointMake(extents.origin.x, extents.origin.y + extents.size.height);
	corner[3] = CGPointMake(extents.origin.x + extents.size.width,
				extents.origin.y + extents.size.height);
	for (i = 0; i < 4; i++) {
		corner[i] = CGPointApplyAffineTransform(corner[i], inverse);
	}
	minx = maxx = corner[0].x;
	miny = maxy = corner[0].y;
	for (i = 1; i < 4; i++) {
		if (corner[i].x < minx) {
			minx = corner[i].x;
		}
		if (corner[i].x > maxx) {
			maxx = corner[i].x;
		}
		if (corner[i].y < miny) {
			miny = corner[i].y;
		}
		if (corner[i].y > maxy) {
			maxy = corner[i].y;
		}
	}
	i0 = (int)floor((minx - rect.origin.x) / rect.size.width);
	i1 = (int)ceil((maxx - rect.origin.x) / rect.size.width);
	j0 = (int)floor((miny - rect.origin.y) / rect.size.height);
	j1 = (int)ceil((maxy - rect.origin.y) / rect.size.height);
	for (j = j0; j <= j1; j++) {
		for (i = i0; i <= i1; i++) {
			CGRect tile = CGRectMake(rect.origin.x + (CGFloat)i * rect.size.width,
						 rect.origin.y + (CGFloat)j * rect.size.height,
						 rect.size.width, rect.size.height);

			CGContextDrawImage(c, tile, image);
		}
	}
}

void CGContextBeginPage(CGContextRef c, const CGRect *mediaBox)
{
	if (c != NULL && c->writer != NULL) {
		(void)cg_pdf_writer_begin_page(c->writer, mediaBox);
		return;
	}
	(void)c;
	(void)mediaBox;
	fprintf(stderr, "CG-REFUSE: a PAGE is a unit of a page-based context — a PDF or printing context — and "
			"the only contexts this library has are bitmaps; the 10.6 header says only \"Begin a new "
			"page.\" and inventing what that means here is not this door\'s to do\n");
}

void CGContextEndPage(CGContextRef c)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_end_page(c->writer);
		return;
	}
	(void)c;
	fprintf(stderr, "CG-REFUSE: CGContextEndPage needs a page-based context, and this library has none\n");
}

void CGContextDrawPath(CGContextRef c, CGPathDrawingMode mode)
{
	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_paint_state ps;
		int operation;

		switch (mode) {
		case kCGPathFill: operation = CG_PDF_PAINT_FILL; break;
		case kCGPathEOFill: operation = CG_PDF_PAINT_EOFILL; break;
		case kCGPathStroke: operation = CG_PDF_PAINT_STROKE; break;
		case kCGPathFillStroke: operation = CG_PDF_PAINT_FILLSTROKE; break;
		case kCGPathEOFillStroke: operation = CG_PDF_PAINT_EOFILLSTROKE; break;
		default: return;
		}
		fn_pdf_paint_state(c, &ps);
		cg_pdf_writer_paint_path(c->writer, (CGPathRef)c->path, operation, &ps);
		CGContextBeginPath(c);
		return;
	}
	switch (mode) {
	case kCGPathFill:
	case kCGPathEOFill:
		cg_fill_current_path(c, mode == kCGPathEOFill);
		return;
	case kCGPathStroke:
		cg_stroke_current_path(c);
		return;
	case kCGPathFillStroke:
	case kCGPathEOFillStroke:
		/* FILL FIRST, STROKE SECOND, FROM THE SAME PATH — and the path is consumed at the
		 * END rather than by the first of the two, which is exactly why this cannot simply
		 * call the two public functions in a row: `FillPath` would clear the path the
		 * stroke still needs. Both colours are in play, the fill's for the interior and the
		 * stroke's for the outline. */
		if (c->state.fill_pattern != NULL) {
			cg_paint_path(c, (CGPathRef)c->path, mode == kCGPathEOFillStroke,
				      cg_op(c->state.blend), NULL, c->state.fill_pattern,
				      c->state.alpha * c->state.fill_pattern_alpha);
		} else {
			cg_paint_path(c, (CGPathRef)c->path, mode == kCGPathEOFillStroke,
				      cg_op(c->state.blend), c->state.rgba, NULL, c->state.alpha);
		}
		cg_stroke_path_with_width(c, (CGPathRef)c->path, c->state.line_width);
		CGContextBeginPath(c);
		return;
	default:
		return;
	}
}

void CGContextReplacePathWithStrokedPath(CGContextRef c)
{
	CGPathRef outline;

	if (c == NULL) {
		return;
	}
	/* IN USER SPACE, WITH THE CURRENT LINE STATE, and no transform parameter: the stroke
	 * is built where the path is, so a later fill puts it wherever the CTM puts the
	 * geometry — which is what makes this the same stroke `StrokePath` would have drawn,
	 * only as a path instead of as pixels. */
	outline = CGPathCreateCopyByStrokingPath((CGPathRef)c->path, NULL, c->state.line_width,
						 c->state.line_cap, c->state.line_join,
						 c->state.miter_limit);
	if (outline == NULL) {
		return;
	}
	CGPathRelease((CGPathRef)c->path);
	c->path = (struct CGPath *)outline;
}

/* --- the line state and the stroke colour --------------------------------- */

void CGContextSetLineWidth(CGContextRef c, CGFloat width)
{
	if (c != NULL) {
		c->state.line_width = width;
	}
}

void CGContextSetLineCap(CGContextRef c, CGLineCap cap)
{
	if (c != NULL) {
		c->state.line_cap = cap;
	}
}

void CGContextSetLineJoin(CGContextRef c, CGLineJoin join)
{
	if (c != NULL) {
		c->state.line_join = join;
	}
}

void CGContextSetMiterLimit(CGContextRef c, CGFloat limit)
{
	if (c != NULL) {
		c->state.miter_limit = limit;
	}
}

/* THE FLATNESS DOOR. It STORES — the value is a graphics-state parameter, so save and restore carry it
 * for free — and the reading happens where the flattening does: `cg_traps_for_path` divides it by the
 * CTM's upper-bound scale to get a USER-space tolerance, because the path it hands the flattener is in
 * user space while the caller's number is in device pixels (CGContext.h).
 *
 * A NON-POSITIVE VALUE IS NOT STORED AS GIVEN: it means "unset" and becomes the default 0.1, which is
 * the SAME rule `CGPathCreateCopyByFlattening` applies to its flatness argument. Storing a 0 would make
 * every fill subdivide until the recursion bottomed out — a hang dressed as accuracy — and storing a
 * negative one would be a tolerance no distance can be under. */
void CGContextSetFlatness(CGContextRef c, CGFloat flatness)
{
	if (c == NULL) {
		return;
	}
	c->state.flatness = (flatness > 0.0) ? flatness : 0.1;
}

void CGContextSetGrayStrokeColor(CGContextRef c, CGFloat gray, CGFloat alpha)
{
	if (c == NULL) {
		return;
	}
	cg_clear_pattern(&c->state.stroke_pattern);   /* see CGContextSetGrayFillColor */
	c->state.stroke_rgba[0] = gray;
	c->state.stroke_rgba[1] = gray;
	c->state.stroke_rgba[2] = gray;
	c->state.stroke_rgba[3] = alpha;
}

void CGContextSetRGBStrokeColor(CGContextRef c, CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha)
{
	if (c == NULL) {
		return;
	}
	cg_clear_pattern(&c->state.stroke_pattern);   /* see CGContextSetGrayFillColor */
	c->state.stroke_rgba[0] = red;
	c->state.stroke_rgba[1] = green;
	c->state.stroke_rgba[2] = blue;
	c->state.stroke_rgba[3] = alpha;
}

/* THE STROKE HALF OF THE CMYK DOOR: the same mapping, the same refusal to touch the colour space, and the
 * STROKE pattern cleared in place of the fill's. See CGContextSetCMYKFillColor for the rule and its
 * record. */
void CGContextSetCMYKStrokeColor(CGContextRef c, CGFloat cyan, CGFloat magenta, CGFloat yellow,
				 CGFloat black, CGFloat alpha)
{
	const CGFloat cmyk[5] = { cyan, magenta, yellow, black, alpha };
	CGFloat rgb[4];

	if (c == NULL) {
		return;
	}
	cg_clear_pattern(&c->state.stroke_pattern);
	cg_device_cmyk_to_rgb(cmyk, rgb);
	c->state.stroke_rgba[0] = rgb[0];
	c->state.stroke_rgba[1] = rgb[1];
	c->state.stroke_rgba[2] = rgb[2];
	c->state.stroke_rgba[3] = rgb[3];
}

/* ---------------------------------------------------------------------------------------
 * THE COLOUR-TAKING FORMS, which is where a CGColor becomes the four numbers the rasterizer
 * actually blends with.
 *
 * THIS IS THE ONE PLACE THAT DECODES A COLOUR'S COMPONENTS, and it is a REFUSAL rather than a
 * best guess wherever the numbers would have to be interpreted rather than copied. A DEVICE
 * space is a statement that the numbers are already the ones to blend, so a grayscale colour
 * copies its single value into all three channels and an RGB one copies three; a CMYK colour
 * does not, because turning four ink values into three light values is a conversion with a
 * profile behind it, and that is the colour engine's job (lcms2, C4.2). Drawing the raw
 * numbers instead would paint something confident and wrong.
 *
 * IT RETURNS 0 AND SAYS WHY ON stderr, so the caller leaves the context exactly as it was: a
 * refused colour must not silently become black, and it must not silently become the PREVIOUS
 * colour either - the first is a lie and the second is a bug hunt.
 * ------------------------------------------------------------------------------------- */
static int cg_color_to_rgba(CGColorRef color, CGFloat rgba[4])
{
	const CGFloat *comp;
	int model;

	if (color == NULL) {
		return 0;   /* NULL is the clearing form's opposite: nothing asked, nothing done */
	}
	model = CGColorSpaceGetModel(CGColorGetColorSpace(color));
	/* DEVICE CMYK IS READ, AND A CMYK PROFILE IS NOT — WHICH IS WHY THIS TEST IS THE SPACE AND NOT THE
	 * MODEL. The user's decision of 2026-10-06 makes device CMYK drawable through this library's own
	 * device mapping (`cg_device_cmyk_to_rgb`, whose rule and record are in CGColor.h); an ICC profile
	 * whose DATA space is CMYK reports the same model and stays refused below, because its numbers
	 * belong to the profile and this library does not read profiles here. */
	if (cg_color_space_is_device_cmyk(CGColorGetColorSpace(color))) {
		CGFloat cmyk[5];
		const CGFloat *device = CGColorGetComponents(color);
		int i;

		for (i = 0; i < 5; i++) {
			cmyk[i] = device[i];
		}
		cg_device_cmyk_to_rgb(cmyk, rgba);
		return 1;
	}
	if (model != kCGColorSpaceModelMonochrome && model != kCGColorSpaceModelRGB) {
		/* !! A SPACE WHOSE NUMBERS HAVE TO BE INTERPRETED IS NOW REFUSED (2026-10-05), WHERE IT
		 * USED TO BE CONVERTED. The conversion was the library's own
		 * `CGColorCreateCopyByMatchingToColorSpace`, which is macOS 10.11 — the version that added
		 * colour conversion to this API — against a 10.6-era surface that has none. So a Lab or an
		 * ICC profile's colour CANNOT BE DRAWN here, and the honest answer is this refusal rather
		 * than the raw numbers: drawing profile numbers as if they were light is the "confident and
		 * wrong" this function's own comment warns about. (DEVICE CMYK LEFT THIS ARM ON
		 * 2026-10-06 — it is read above, by the stated device mapping, and a CMYK PROFILE is still
		 * refused here.) REFUSING MEANS THE CONTEXT KEEPS THE COLOUR IT HAD — it does not become
		 * black and it does not keep the previous one silently — which is the same rule for one
		 * case as for the class. */
		fprintf(stderr, "CG-REFUSE: this colour's space cannot be read as light values — this "
				"library converts no colour through a profile, so a Lab or ICC colour is "
				"refused rather than approximated\n");
		return 0;
	}
	comp = CGColorGetComponents(color);
	switch (model) {
	case kCGColorSpaceModelMonochrome:
		rgba[0] = comp[0];
		rgba[1] = comp[0];
		rgba[2] = comp[0];
		rgba[3] = comp[1];
		return 1;
	case kCGColorSpaceModelRGB:
		rgba[0] = comp[0];
		rgba[1] = comp[1];
		rgba[2] = comp[2];
		rgba[3] = comp[3];
		return 1;
	default:
		/* UNREACHABLE: the test above sends every other model to the conversion, which is
		 * where the refusals now live. Kept so that a model added later without a home here
		 * fails visibly in this function rather than silently somewhere else. */
		fprintf(stderr, "CG-REFUSE: a colour in this color space has no conversion\n");
		return 0;
	}
}

/* ------------------------------------------------------------------------- */
/* the component doors, and the colour spaces behind them                      */
/* ------------------------------------------------------------------------- */

/* THE COMPONENT DOORS ARE DEFINED BY THE CURRENT COLOUR SPACE AND NOT BY RGB, and Apple's header says what
 * that means precisely: "the number of elements in `components` must be one greater than the number of
 * components in the current fill color space (N color components + 1 alpha component). The current fill color
 * space must not be a pattern color space."
 *
 * SO THE SPACE IS WHAT MAKES A COMPONENT MEANINGFUL, AND THIS LIBRARY REMEMBERS IT AS ITS MODEL: how many
 * components there are and what they mean is all a `CGColorSpaceModel` carries, and storing the model rather
 * than the object means THE GRAPHICS STATE GAINS NO RETAINED POINTER — nothing to retain on save, nothing to
 * release on restore, nothing for the context's release to know about. A space this library cannot read as
 * light values (CMYK: what ink values mean depends on the press; Lab and ICC: no colour conversion here) is
 * REFUSED BY NAME rather than stored, because a stored space no colour could ever be set with is a setting
 * with no effect. */
static const char *fn_model_name(int model)
{
	switch (model) {
	case kCGColorSpaceModelMonochrome:
		return "grayscale";
	case kCGColorSpaceModelRGB:
		return "RGB";
	case kCGColorSpaceModelCMYK:
		return "CMYK";
	case kCGColorSpaceModelLab:
		return "Lab";
	case kCGColorSpaceModelIndexed:
		return "indexed";
	case kCGColorSpaceModelPattern:
		return "pattern";
	default:
		return "an unrecognised";
	}
}

/* APPLE'S SIDE EFFECT, WHICH IS PART OF THE DOOR AND NOT A DETAIL: "As a side-effect, set the fill color to a
 * default value appropriate for the color space." THE DEFAULT IS BLACK AND OPAQUE — every colour component
 * zero, alpha one — which is what this library already paints with before anybody sets a colour, so a caller
 * who changes the space and then fills gets black rather than the colour they were using a moment ago. */
static void fn_default_color(CGContextRef c, CGFloat *rgba, int model)
{
	/* CMYK JOINS THE TWO WHOSE DEFAULT IS THE SAME TRIPLE, AND FOR A REASON RATHER THAN BY CONVENIENCE:
	 * the default ink is (0, 0, 0, 1) — black ink, no chromatic ink — and `cg_device_cmyk_to_rgb` maps
	 * exactly that to (0, 0, 0), so the value written here is the space's own default rather than a
	 * substitution for it. */
	if (model == kCGColorSpaceModelMonochrome || model == kCGColorSpaceModelRGB
	    || model == kCGColorSpaceModelCMYK) {
		rgba[0] = 0.0;
		rgba[1] = 0.0;
		rgba[2] = 0.0;
		rgba[3] = 1.0;
	}
	(void)c;
}

void CGContextSetFillColorSpace(CGContextRef c, CGColorSpaceRef space)
{
	if (c == NULL || space == NULL) {
		return;
	}
	if (CGColorSpaceGetModel(space) != kCGColorSpaceModelMonochrome
	    && CGColorSpaceGetModel(space) != kCGColorSpaceModelRGB
	    && !cg_color_space_is_device_cmyk(space)) {
		fprintf(stderr, "CG-REFUSE: CGContextSetFillColorSpace takes a %s space, and this library reads "
				"light values only — a CMYK space has to be the DEVICE one, whose mapping is "
				"this library's own (see CGColor.h), and no colour is converted through a "
				"profile here\n", fn_model_name(CGColorSpaceGetModel(space)));
		return;
	}
	c->state.fill_model = CGColorSpaceGetModel(space);
	fn_default_color(c, c->state.rgba, c->state.fill_model);
}

void CGContextSetStrokeColorSpace(CGContextRef c, CGColorSpaceRef space)
{
	if (c == NULL || space == NULL) {
		return;
	}
	if (CGColorSpaceGetModel(space) != kCGColorSpaceModelMonochrome
	    && CGColorSpaceGetModel(space) != kCGColorSpaceModelRGB
	    && !cg_color_space_is_device_cmyk(space)) {
		fprintf(stderr, "CG-REFUSE: CGContextSetStrokeColorSpace takes a %s space, and this library reads "
				"light values only — a CMYK space has to be the DEVICE one, whose mapping is "
				"this library's own (see CGColor.h)\n", fn_model_name(CGColorSpaceGetModel(space)));
		return;
	}
	c->state.stroke_model = CGColorSpaceGetModel(space);
	fn_default_color(c, c->state.stroke_rgba, c->state.stroke_model);
}

/* A MONOCHROME SPACE HAS ONE COMPONENT AND IT FEEDS ALL THREE CHANNELS, which is the same reading
 * `CGContextSetGrayFillColor` has always used; a NULL array is refused rather than read. */
void CGContextSetFillColor(CGContextRef c, const CGFloat *components)
{
	if (c == NULL || components == NULL) {
		return;
	}
	if (c->state.fill_model == kCGColorSpaceModelMonochrome) {
		c->state.rgba[0] = components[0];
		c->state.rgba[1] = components[0];
		c->state.rgba[2] = components[0];
		c->state.rgba[3] = components[1];
		return;
	}
	/* CMYK IS FIVE VALUES WHERE THE OTHER TWO ARE TWO AND FOUR (four inks and the alpha LAST), and they
	 * become light through the same stated mapping every other CMYK door uses. THE MODEL IS WHAT DECIDES
	 * — this door reads the space the caller last declared with `CGContextSetFillColorSpace`, which is
	 * the flow Apple's own documentation describes for the component form. */
	if (c->state.fill_model == kCGColorSpaceModelCMYK) {
		CGFloat cmyk[5];
		CGFloat rgb[4];
		int i;

		for (i = 0; i < 5; i++) {
			cmyk[i] = components[i];
		}
		cg_device_cmyk_to_rgb(cmyk, rgb);
		c->state.rgba[0] = rgb[0];
		c->state.rgba[1] = rgb[1];
		c->state.rgba[2] = rgb[2];
		c->state.rgba[3] = rgb[3];
		return;
	}
	c->state.rgba[0] = components[0];
	c->state.rgba[1] = components[1];
	c->state.rgba[2] = components[2];
	c->state.rgba[3] = components[3];
}

void CGContextSetStrokeColor(CGContextRef c, const CGFloat *components)
{
	if (c == NULL || components == NULL) {
		return;
	}
	if (c->state.stroke_model == kCGColorSpaceModelMonochrome) {
		c->state.stroke_rgba[0] = components[0];
		c->state.stroke_rgba[1] = components[0];
		c->state.stroke_rgba[2] = components[0];
		c->state.stroke_rgba[3] = components[1];
		return;
	}
	/* THE STROKE HALF OF THE FIVE-VALUE CASE above, reading the STROKE model. */
	if (c->state.stroke_model == kCGColorSpaceModelCMYK) {
		CGFloat cmyk[5];
		CGFloat rgb[4];
		int i;

		for (i = 0; i < 5; i++) {
			cmyk[i] = components[i];
		}
		cg_device_cmyk_to_rgb(cmyk, rgb);
		c->state.stroke_rgba[0] = rgb[0];
		c->state.stroke_rgba[1] = rgb[1];
		c->state.stroke_rgba[2] = rgb[2];
		c->state.stroke_rgba[3] = rgb[3];
		return;
	}
	c->state.stroke_rgba[0] = components[0];
	c->state.stroke_rgba[1] = components[1];
	c->state.stroke_rgba[2] = components[2];
	c->state.stroke_rgba[3] = components[3];
}

void CGContextSetFillColorWithColor(CGContextRef c, CGColorRef color)
{
	CGFloat rgba[4];

	if (c == NULL) {
		return;
	}
	/* SEE `CGContextSetStrokeColorWithColor` FOR WHY THIS DOOR EXISTS. */
	if (CGColorGetPattern(color) != NULL) {
		cg_clear_pattern(&c->state.fill_pattern);
		c->state.fill_pattern = CGPatternRetain(CGColorGetPattern(color));
		c->state.fill_pattern_alpha = CGColorGetAlpha(color);
		return;
	}
	if (!cg_color_to_rgba(color, rgba)) {
		return;
	}
	cg_clear_pattern(&c->state.fill_pattern);
	c->state.rgba[0] = rgba[0];
	c->state.rgba[1] = rgba[1];
	c->state.rgba[2] = rgba[2];
	c->state.rgba[3] = rgba[3];
}

void CGContextSetStrokeColorWithColor(CGContextRef c, CGColorRef color)
{
	CGFloat rgba[4];

	if (c == NULL) {
		return;
	}
	/* A PATTERN COLOUR SETS THE STROKE PATTERN, which is the other door into the pattern state and the
	 * reason `CGColorGetPattern` exists: it is how a setter tells it was handed a pattern rather than
	 * numbers. The alpha comes from the colour, which for a pattern colour IS its one component. */
	if (CGColorGetPattern(color) != NULL) {
		cg_clear_pattern(&c->state.stroke_pattern);
		c->state.stroke_pattern = CGPatternRetain(CGColorGetPattern(color));
		c->state.stroke_pattern_alpha = CGColorGetAlpha(color);
		return;
	}
	if (!cg_color_to_rgba(color, rgba)) {
		return;
	}
	cg_clear_pattern(&c->state.stroke_pattern);
	c->state.stroke_rgba[0] = rgba[0];
	c->state.stroke_rgba[1] = rgba[1];
	c->state.stroke_rgba[2] = rgba[2];
	c->state.stroke_rgba[3] = rgba[3];
}

/* THE PATTERN SETTERS. A COLOUR AND A PATTERN ARE TWO ANSWERS TO ONE QUESTION, so setting either
 * CLEARS the other — the pair of halves is in the two `.WithColor` setters above and in the two
 * below, and the rule is that the LATER one wins rather than that they coexist. A NULL pattern is
 * REFUSED rather than read as "back to the colour": Apple has no such reading and neither does this
 * library, and a caller who wants the colour back sets a colour. */
void CGContextSetFillPattern(CGContextRef c, CGPatternRef pattern, const CGFloat *components)
{
	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "pattern fills");
		return;
	}
	if (pattern == NULL || components == NULL) {
		fprintf(stderr, "CG-REFUSE: CGContextSetFillPattern needs a pattern and its components; "
				"a NULL pair is refused rather than read as 'back to the fill "
				"colour'\n");
		return;
	}
	cg_clear_pattern(&c->state.fill_pattern);
	c->state.fill_pattern = CGPatternRetain(pattern);
	c->state.fill_pattern_alpha = components[0];
}

void CGContextSetStrokePattern(CGContextRef c, CGPatternRef pattern, const CGFloat *components)
{
	if (c == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "pattern strokes");
		return;
	}
	if (pattern == NULL || components == NULL) {
		fprintf(stderr, "CG-REFUSE: CGContextSetStrokePattern needs a pattern and its "
				"components\n");
		return;
	}
	cg_clear_pattern(&c->state.stroke_pattern);
	c->state.stroke_pattern = CGPatternRetain(pattern);
	c->state.stroke_pattern_alpha = components[0];
}

/* THE PHASE IS STATE AND NOT A DRAW PARAMETER, which is Apple's spelling and is why it can be set
 * once and left: it applies to every pattern fill and stroke until it is changed, and it is saved and
 * restored with the rest of the graphics state. */
void CGContextSetPatternPhase(CGContextRef c, CGSize phase)
{
	if (c != NULL) {
		c->state.pattern_phase = phase;
	}
}

/* THE TWO SEAMS AN IMAGE DRAWN INTO A CONTEXT NEEDS, INCLUDED HERE RATHER THAN AT THE TOP OF THE
 * FILE because the function below is their only user: the drawability question, asked where it was
 * ANSWERED (`CGImageCreate` refuses a chart this library cannot draw, so asking it again here would
 * be a second spelling of one rule), and the provider's bytes. */
#include <CoreGraphics/CGDataProvider_internal.h>

/* ---------------------------------------------------------------------------------------
 * DRAWING AN IMAGE, AND THE TWO THINGS ABOUT IT THAT ARE SEMANTICS RATHER THAN CODE.
 *
 * THE IMAGE'S FIRST ROW LANDS AT THE TOP OF THE RECT. Apple's contract puts the image's origin at
 * the rect's origin — and because an image's row 0 is its TOP row while this library's user space
 * has y increasing upward, that is a flip relative to user coordinates. It is what callers expect,
 * and the probe pins it with an image whose top and bottom rows differ.
 *
 * AND IT IS COMPOSITED SOURCE-OVER, PREMULTIPLIED, WITH THE CONTEXT'S ALPHA. That is what the
 * pinned format IS, so nothing is unpremultiplied anywhere. A NON-NORMAL BLEND MODE IS REFUSED
 * rather than ignored — the rule this library follows everywhere, since drawing an image with the
 * blend mode quietly dropped is a wrong answer that looks like a right one.
 *
 * SCALING IS NEAREST-NEIGHBOUR: the pinned format carries no filter state, and `shouldInterpolate`
 * is RECORDED on the image rather than honoured, which is why the getter for it exists and why
 * this comment says which of the two it is.
 * ------------------------------------------------------------------------------------- */
/* THE PREMULTIPLIED COLOUR OF ONE TEXEL, CLAMPED TO THE IMAGE — the sampler the interpolating path
 * below needs, and the one place that says what "premultiplied" means for it. IT IS APPLIED BEFORE
 * ANY WEIGHTING between texels, which is not tidiness: lerping STRAIGHT components drags the colour
 * of a fully transparent texel into a visible one, and that halo is what image scalers are known
 * for. A chart with no alpha channel is already "premultiplied by one", so nothing happens to it,
 * and a chart whose alpha is STRAIGHT is multiplied once, here, exactly as the nearest path does. */
/* THE MASK A PICTURE IS PAINTED THROUGH, AS COVERAGE IN 0..1, AND APPLE'S TWO RULES DIFFER BY AN INVERSION:
 * an IMAGE MASK's sample is an INVERSE alpha (S=1 paints nothing), while a gray PICTURE used as a mask is the
 * alpha itself (S=1 paints fully). Both are read here as ONE BYTE per pixel, which is what
 * CGImageCreateWithMask refuses any other depth for.
 *
 * IT IS SAMPLED BY NORMALIZED POSITION, the same coordinates the picture is sampled at, because the header
 * never says the two are the same size: a mask is stretched over the rectangle the picture is drawn into.
 * Outside 0..1 there is no mask to apply, and NO EFFECT is the honest answer rather than a refuse — the
 * picture's own bounds already ended the loop for those pixels. */
static double cg_image_mask_coverage(CGImageRef mask, const CGFloat *decode, double u, double v)
{
	const unsigned char *m;
	size_t size = 0;
	size_t w = CGImageGetWidth(mask);
	size_t h = CGImageGetHeight(mask);
	size_t row = CGImageGetBytesPerRow(mask);
	int sx, sy;
	double s;

	if (w == 0 || h == 0) {
		return 1.0;
	}
	m = (const unsigned char *)cg_dataprovider_bytes(CGImageGetDataProvider(mask), &size);
	if (m == NULL) {
		return 1.0;
	}
	/* **AND A MASK THAT IS SCALED IS SAMPLED, NOT PICKED.** Nearest-neighbour was the whole of this sampler, so a
	 * stencil drawn at a size it was not made for came out BLOCKY — MEASURED: a 16-pixel arrow drawn into a
	 * 24-point toolbar cell had hard, dithered edges, because every output pixel took the value of one input
	 * pixel. When the image asks to be interpolated (see `CGImageMaskCreate`), the four samples around the point
	 * are blended; a 1:1 draw lands exactly on texel centres and is unchanged, which is why this is safe for the
	 * sizes the table carries natively. */
	if (CGImageGetShouldInterpolate(mask)) {
		double fx = u * (double)w - 0.5;
		double fy = (1.0 - v) * (double)h - 0.5;
		int x0 = (int)floor(fx);
		int y0 = (int)floor(fy);
		double tx = fx - (double)x0;
		double ty = fy - (double)y0;
		double s00, s10, s01, s11;

		if (x0 < 0) { x0 = 0; tx = 0.0; }
		if (y0 < 0) { y0 = 0; ty = 0.0; }
		if (x0 > (int)w - 1) { x0 = (int)w - 1; tx = 0.0; }
		if (y0 > (int)h - 1) { y0 = (int)h - 1; ty = 0.0; }
#define FN_MASK_TEXEL(gx, gy) 		((double)m[(size_t)((gy) > (int)h - 1 ? (int)h - 1 : (gy)) * row + \
			   (size_t)((gx) > (int)w - 1 ? (int)w - 1 : (gx))])
		s00 = FN_MASK_TEXEL(x0, y0);
		s10 = FN_MASK_TEXEL(x0 + 1, y0);
		s01 = FN_MASK_TEXEL(x0, y0 + 1);
		s11 = FN_MASK_TEXEL(x0 + 1, y0 + 1);
#undef FN_MASK_TEXEL
		s = (s00 * (1.0 - tx) + s10 * tx) * (1.0 - ty) + (s01 * (1.0 - tx) + s11 * tx) * ty;
	} else {
		sx = (int)(u * (double)w);
		sy = (int)((1.0 - v) * (double)h);
		if (sx >= (int)w) {
			sx = (int)w - 1;
		}
		if (sy >= (int)h) {
			sy = (int)h - 1;
		}
		if (sx < 0 || sy < 0) {
			return 1.0;
		}
		s = (double)m[(size_t)sy * row + (size_t)sx];
	}
	/* THE MASK'S OWN DECODE ARRAY RUNS BEFORE THE MASK RULE, because it remaps the SAMPLE and the rule
	 * then says what that sample means. A mask has one component, so its array is one pair. */
	if (decode != NULL) {
		s = decode[0] + (s / 255.0) * (decode[1] - decode[0]);
	}
	return CGImageIsMask(mask) ? (1.0 - s / 255.0) : (s / 255.0);
}

/* THE DECODE ARRAY, APPLIED TO ONE PIXEL'S COLOR COMPONENTS. `comp` is in the COLOR SPACE's order (red,
 * green, blue) and the array holds 2N sample ranges for its N components, so the pair for component i is
 * elements 2i and 2i+1 — THE SAME SHAPE AND THE SAME ORDER as the masking colors above. A sample is remapped
 * LINEARLY: `min + (sample / 255) * (max - min)`, in place, and the ALPHA COMPONENT IS UNTOUCHED because N
 * counts the color space's components and a color space has no alpha. */
static void cg_image_apply_decode(const CGFloat *decode, int n, double comp[3])
{
	int i;

	for (i = 0; i < n; i++) {
		if (decode[2 * i] == 0.0 && decode[2 * i + 1] == 255.0) {
			continue;	/* the identity: leave the sample exactly as it was read */
		}
		comp[i] = decode[2 * i] + (comp[i] / 255.0) * (decode[2 * i + 1] - decode[2 * i]);
	}
}

/* IS THIS PIXEL MASKED OUT BY MASKING COLORS? Apple's rule: a sample whose components ALL fall inside their
 * ranges is not painted. `comp` is in the COLOR SPACE's order — red, green, blue for RGB, the single gray for
 * a one-component space — while the sampler's own variables are in the pixel's stored order, so the caller
 * does that mapping and this function does the test. */
static int cg_image_masked_out(const CGFloat *colors, int n, const double comp[3])
{
	int i;

	for (i = 0; i < n; i++) {
		if (!(comp[i] >= (double)colors[i * 2] && comp[i] <= (double)colors[i * 2 + 1])) {
			return 0;
		}
	}
	return 1;
}

static void cg_image_texel(const unsigned char *src, size_t row_bytes, size_t stored, size_t img_w,
			   size_t img_h, const int channels[4], int straight, int sx, int sy,
			   double out[4])
{
	const unsigned char *s;

	if (sx < 0) {
		sx = 0;
	}
	if (sy < 0) {
		sy = 0;
	}
	if (sx >= (int)img_w) {
		sx = (int)img_w - 1;
	}
	if (sy >= (int)img_h) {
		sy = (int)img_h - 1;
	}
	s = src + (size_t)sy * row_bytes + (size_t)sx * stored;
	out[3] = (channels[3] < 0 ? 255.0 : (double)s[channels[3]]) / 255.0;
	out[0] = (double)s[channels[0]] / 255.0;
	out[1] = (double)s[channels[1]] / 255.0;
	out[2] = (double)s[channels[2]] / 255.0;
	if (straight && channels[3] >= 0) {
		out[0] = out[0] * out[3];
		out[1] = out[1] * out[3];
		out[2] = out[2] * out[3];
	}
}

/* THE USER-SPACE RECTANGLE'S DEVICE BOUNDING BOX, AND WHERE A DEVICE PIXEL LANDS INSIDE IT. THESE WERE
 * INLINE IN THE SAMPLER BELOW, WITH A COMMENT SAYING THE TRANSFORM WAS DONE "by hand, because both numbers
 * are wanted rather than a helper's result" — WHICH WAS TRUE WHILE THERE WAS ONE CALLER. `CGContextClipToMask`
 * needs the same two steps, and a second copy of one geometry is how the two come to disagree, so the reason
 * for inlining is gone and the reason for a helper has taken its place. */
static void cg_device_bounds(CGContextRef c, CGRect rect, int *left, int *top, int *right, int *bottom)
{
	CGPoint corner[4];
	double minx, maxx, miny, maxy;
	int i;

	corner[0] = CGPointMake(rect.origin.x, rect.origin.y);
	corner[1] = CGPointMake(rect.origin.x + rect.size.width, rect.origin.y);
	corner[2] = CGPointMake(rect.origin.x, rect.origin.y + rect.size.height);
	corner[3] = CGPointMake(rect.origin.x + rect.size.width, rect.origin.y + rect.size.height);
	for (i = 0; i < 4; i++) {
		corner[i] = CGPointApplyAffineTransform(corner[i], c->state.ctm);
	}
	minx = maxx = corner[0].x;
	miny = maxy = corner[0].y;
	for (i = 1; i < 4; i++) {
		if (corner[i].x < minx) {
			minx = corner[i].x;
		}
		if (corner[i].x > maxx) {
			maxx = corner[i].x;
		}
		if (corner[i].y < miny) {
			miny = corner[i].y;
		}
		if (corner[i].y > maxy) {
			maxy = corner[i].y;
		}
	}
	/* **THE EDGES ARE ROUNDED OUTWARD, NOT TRUNCATED.** A rectangle whose right edge falls inside a pixel still
	 * covers that pixel PARTLY, and a sampler that stops at the truncated edge never asks the mask about it — so
	 * the uncovered column is left with whatever coverage it had, which for `CGContextClipToMask` is "keep
	 * everything". MEASURED: a template image drawn into a rectangle at x.6 painted a one-pixel vertical line
	 * down its right edge, at a third of the fill's strength, because the mask was never consulted there. Floor on
	 * the low edges and ceiling on the high ones is the rule that includes every pixel the rectangle touches. */
	*left = (int)floor(minx);
	*top = (int)floor(miny);
	*right = (int)ceil(maxx);
	*bottom = (int)ceil(maxy);
	if (*left < 0) {
		*left = 0;
	}
	if (*top < 0) {
		*top = 0;
	}
	if (*right > c->width) {
		*right = c->width;
	}
	if (*bottom > c->height) {
		*bottom = c->height;
	}
}

/* THE INVERSE CTM TAKES A DEVICE PIXEL BACK TO USER SPACE, which is the direction a sampler needs: the image
 * is a function of user coordinates, and each device pixel asks what belongs there. NORMALISED TO THE
 * RECTANGLE, so 0..1 spans it — and a pixel that lands OUTSIDE returns 0, which is what both callers want:
 * the sampler skips it, and the mask clip leaves the clipping area unchanged there. */
static int cg_device_to_rect(const CGAffineTransform *inverse, CGRect rect, int x, int y, double *u, double *v)
{
	CGPoint p = CGPointMake((double)x + 0.5, (double)y + 0.5);
	double uu, vv;

	p = CGPointApplyAffineTransform(p, *inverse);
	uu = (p.x - rect.origin.x) / rect.size.width;
	vv = (p.y - rect.origin.y) / rect.size.height;
	if (uu < 0.0 || uu >= 1.0 || vv < 0.0 || vv >= 1.0) {
		return 0;
	}
	*u = uu;
	*v = vv;
	return 1;
}

void CGContextDrawImage(CGContextRef c, CGRect rect, CGImageRef image)
{
	CGAffineTransform inverse;
	const unsigned char *src;
	int channels[4];
	int straight = 0;
	int stored = 0;
	size_t size = 0;
	size_t row_bytes;
	size_t img_w, img_h;
	double alpha;
	int copy;			/* the blend mode, read once: Copy replaces, Normal blends */
	int left, right, top, bottom;
	int x, y;
	/* **WHERE RED AND BLUE LIVE IN THE DESTINATION, WHICH THIS HAND LOOP HAS TO ASK FOR.** This sampler
	 * composites by hand rather than through pixman, so it is the ONE writer that has to know the surface's byte
	 * order — and it asks `cg_channel_slots`, which is the one place that says. It used to ask PIXMAN instead,
	 * comparing the destination's format to `a8r8g8b8`; the window's surface is `x8r8g8b8`, so the comparison
	 * failed and every image drawn onto a window had its red and blue swapped. */
	unsigned red_offset, blue_offset;
	CGImageRef mask_image;
	const CGFloat *mask_colors;
	int mask_count = 0;
	const CGFloat *decode;
	int decode_count = 0;
	const CGFloat *mask_decode;

	if (c == NULL || image == NULL || c->data == NULL) {
		return;
	}
	if (c->writer != NULL) {
		cg_pdf_paint_state ps;

		fn_pdf_paint_state(c, &ps);
		cg_pdf_writer_image(c->writer, rect, image, &ps);
		return;
	}
	if (!cg_image_is_drawable(image)) {
		fprintf(stderr, "CG-REFUSE: this image was not built in the format these contexts draw\n");
		return;
	}
	/* THE CHART IS READ ONCE, NOT PER PIXEL. `cg_image_layout` says where each of blue, green, red
	 * and alpha lives inside a pixel — or that a channel is absent, which is how a grey chart and a
	 * mask are handled without the loop below having a case for either — and whether the stored
	 * alpha is straight. Everything past this point works in RGBA and lets the map do the work, so
	 * THIS LOOP HAS NO FORMAT MATRIX IN IT AT ALL. */
	cg_image_layout(CGImageGetColorSpace(image), CGImageGetAlphaInfo(image),
			CGImageGetBitmapInfo(image) & ~0x1fu, channels, &stored, &straight);
	if (stored == 0) {
		fprintf(stderr, "CG-REFUSE: CGContextDrawImage cannot read this image's channel "
				"layout\n");
		return;
	}
	/* WHAT THE PICTURE IS PAINTED THROUGH, READ ONCE: either a mask or masking colors, never both —
	 * CGImageCreateWithMask refuses a picture that is already masked. */
	mask_image = cg_image_mask(image);
	mask_colors = cg_image_masking_colors(image, &mask_count);
	/* THE DECODE ARRAY IS READ ONCE TOO, for the picture and for its mask: both are applied per pixel
	 * and neither is looked up again inside the loop. */
	decode = CGImageGetDecode(image);
	decode_count = decode != NULL
		       ? CGColorSpaceGetNumberOfComponents(CGImageGetColorSpace(image)) : 0;
	mask_decode = mask_image != NULL ? CGImageGetDecode(mask_image) : NULL;
	/* TWO OPERATIONS, AND THE SECOND IS WHAT A BLIT IS. `Copy` means the image REPLACES what is under it,
	 * which is the operation an image draw actually asks for most of the time and the one whose arithmetic
	 * is TRIVIAL in this loop: the source-over formula minus the destination term. Every other operation
	 * needs the full blend matrix, which this sampler does not have — so they are refused BY NAME with the
	 * two it does have, rather than drawn as source-over and silently wrong. */
	if (c->state.blend != kCGBlendModeNormal && c->state.blend != kCGBlendModeCopy) {
		fprintf(stderr, "CG-REFUSE: CGContextDrawImage applies kCGBlendModeNormal and "
				"kCGBlendModeCopy; this sampler has no blend matrix for any other "
				"operation\n");
		return;
	}
	if (rect.size.width <= 0.0 || rect.size.height <= 0.0) {
		return;
	}
	src = (const unsigned char *)cg_dataprovider_bytes(CGImageGetDataProvider(image), &size);
	if (src == NULL) {
		return;
	}
	img_w = CGImageGetWidth(image);
	img_h = CGImageGetHeight(image);
	row_bytes = CGImageGetBytesPerRow(image);
	cg_device_bounds(c, rect, &left, &top, &right, &bottom);
	/* THE INVERSE CTM TAKES A DEVICE PIXEL BACK TO USER SPACE, which is the direction a sampler
	 * needs: the image is a function of user coordinates, and each device pixel asks what colour
	 * belongs there. Computed once rather than per pixel. */
	inverse = CGAffineTransformInvert(c->state.ctm);
	alpha = c->state.alpha;
	/* READ ONCE, LIKE EVERYTHING ELSE THIS LOOP LIFTS OUT OF ITS PER-PIXEL BODY. */
	copy = (c->state.blend == kCGBlendModeCopy);
	{
		int slot[3];

		cg_channel_slots(c->bitmap_info, slot);
		red_offset = (unsigned)slot[0];
		blue_offset = (unsigned)slot[2];
	}

	for (y = top; y < bottom; y++) {
		for (x = left; x < right; x++) {
			unsigned char *d;
			const unsigned char *s;
			double u, v;
			double sa;
			double sb, sg, sr, sv;
			int sx, sy;

			if (!pixman_region32_contains_point(&c->state.clip, x, y, NULL)) {
				continue;
			}
			if (!cg_device_to_rect(&inverse, rect, x, y, &u, &v)) {
				continue;
			}
			/* THE INTERPOLATING PATH IS TAKEN FIRST, SO THAT THE NEAREST SAMPLER BELOW STAYS
			 * BYTE-FOR-BYTE WHAT IT WAS: `kCGInterpolationNone` is the default and the C5 image
			 * probe pins its scaling, so the branch that changes behaviour is the new one. */
			if (c->state.interpolation != kCGInterpolationNone) {
				double mix[4];
				double c00[4];
				double c10[4];
				double c01[4];
				double c11[4];
				double fx, fy, tx, ty;
				int k, x0, y0;

				/* TEXEL CENTRES: the point u in [0,1) is scaled into texel space, where texel i's
				 * CENTRE sits at i + 0.5 — so the sample coordinate is u*width - 0.5 and the four
				 * neighbours are the floor and the floor plus one. */
				fx = u * (double)img_w - 0.5;
				fy = (1.0 - v) * (double)img_h - 0.5;
				x0 = (int)floor(fx);
				y0 = (int)floor(fy);
				tx = fx - (double)x0;
				ty = fy - (double)y0;
				cg_image_texel(src, row_bytes, (size_t)stored, img_w, img_h, channels, straight,
					       x0, y0, c00);
				cg_image_texel(src, row_bytes, (size_t)stored, img_w, img_h, channels, straight,
					       x0 + 1, y0, c10);
				cg_image_texel(src, row_bytes, (size_t)stored, img_w, img_h, channels, straight,
					       x0, y0 + 1, c01);
				cg_image_texel(src, row_bytes, (size_t)stored, img_w, img_h, channels, straight,
					       x0 + 1, y0 + 1, c11);
				for (k = 0; k < 4; k++) {
					mix[k] = c00[k] * (1.0 - tx) * (1.0 - ty)
					       + c10[k] * tx * (1.0 - ty)
					       + c01[k] * (1.0 - tx) * ty
					       + c11[k] * tx * ty;
				}
				/* THE SAME DECODE, OVER THIS PATH'S OWN VARIABLES: `mix` is in 0..1 and the array is in
				 * sample units, so it converts in and back out — and the ORDER is the sampler's (blue,
				 * green, red, alpha) while the array is the color space's (red, green, blue). */
				if (decode_count > 0) {
					double comp[3];

					comp[0] = mix[2] * 255.0;
					comp[1] = mix[1] * 255.0;
					comp[2] = mix[0] * 255.0;
					cg_image_apply_decode(decode, decode_count, comp);
					mix[2] = comp[0] / 255.0;
					mix[1] = (decode_count > 1 ? comp[1] : comp[0]) / 255.0;
					mix[0] = (decode_count > 1 ? comp[2] : comp[0]) / 255.0;
				}
				d = c->data + (size_t)y * (size_t)c->stride + (size_t)x * 4u;
				sa = mix[3] * alpha;
				/* THIS BLIT COMPOSITES BY HAND RATHER THAN THROUGH pixman, so it has to consult the
				 * clip's mask half itself. */
				if (c->state.clip_mask != NULL) {
					sa = sa * (double)cg_clip_coverage(c, x, y) / 255.0;
				}
				/* MASKING COLORS ARE TESTED AND THE PIXEL IS EITHER PAINTED OR NOT, while a MASK SCALES
				 * the alpha: those are Apple's two rules, and mixing them would paint a blend where the
				 * caller asked for nothing at all. `mix` is the sampler's order (blue, green, red,
				 * alpha) and the masking colors are in the SPACE's order (red, green, blue). */
				if (mask_count > 0) {
					double comp[3];

					comp[0] = mix[2] * 255.0;
					comp[1] = mix[1] * 255.0;
					comp[2] = mix[0] * 255.0;
					if (cg_image_masked_out(mask_colors, mask_count, comp)) {
						continue;
					}
				}
				if (mask_image != NULL) {
					sa = sa * cg_image_mask_coverage(mask_image, mask_decode, u, v);
				}
				if (copy) {
					/* THE SOURCE REPLACES THE DESTINATION, masks and all: no destination term. */
					d[red_offset] = (unsigned char)(mix[2] * 255.0 * sa);
					d[1] = (unsigned char)(mix[1] * 255.0 * sa);
					d[blue_offset] = (unsigned char)(mix[0] * 255.0 * sa);
					d[3] = (unsigned char)(255.0 * sa);
				} else {
					d[red_offset] = (unsigned char)(mix[2] * 255.0 * sa + (double)d[red_offset] * (1.0 - sa));
					d[1] = (unsigned char)(mix[1] * 255.0 * sa + (double)d[1] * (1.0 - sa));
					d[blue_offset] = (unsigned char)(mix[0] * 255.0 * sa + (double)d[blue_offset] * (1.0 - sa));
					d[3] = (unsigned char)((double)d[3] * (1.0 - sa) + 255.0 * sa);
				}
				continue;
			}
			/* THE FLIP LIVES IN THIS ONE LINE: v = 1 is the TOP of the rect, and row 0 of the
			 * image is its top row. */
			sx = (int)(u * (double)img_w);
			sy = (int)((1.0 - v) * (double)img_h);
			if (sx >= (int)img_w) {
				sx = (int)img_w - 1;
			}
			if (sy >= (int)img_h) {
				sy = (int)img_h - 1;
			}
			s = src + (size_t)sy * row_bytes + (size_t)sx * (size_t)stored;
			d = c->data + (size_t)y * (size_t)c->stride + (size_t)x * 4u;
			/* PREMULTIPLIED SOURCE-OVER, AND EVERY CHART REACHES IT THE SAME WAY: the channels
			 * come through the map, so grey reads one byte three times and a mask reads none,
			 * and a chart with no alpha channel reads 255 so the blend reduces to a copy.
			 * A chart whose alpha is STRAIGHT is premultiplied right here — once, before the
			 * composite — which is the same multiply CGImageCreateWithPNGDataProvider does, and
			 * is why the blend below needs no case for it. */
			sb = (double)s[channels[0]];
			sg = (double)s[channels[1]];
			sr = (double)s[channels[2]];
			sv = channels[3] < 0 ? 255.0 : (double)s[channels[3]];
			/* THE DECODE ARRAY RUNS HERE: before the premultiply below, because the array remaps THE
			 * CALLER'S SAMPLES and the premultiply is the first thing that treats them as colours. */
			if (decode_count > 0) {
				double comp[3];

				comp[0] = sr;
				comp[1] = sg;
				comp[2] = sb;
				cg_image_apply_decode(decode, decode_count, comp);
				/* A ONE-COMPONENT SPACE HAS ONE NUMBER, AND IT BECOMES ALL THREE CHANNELS. A gray chart
				 * reads one byte three times, so a decoded gray must be written three times too —
				 * writing back only red would TINT the image, which is exactly what the probe caught:
				 * blue and green stayed at the raw sample. */
				sr = comp[0];
				sg = decode_count > 1 ? comp[1] : comp[0];
				sb = decode_count > 1 ? comp[2] : comp[0];
			}
			if (straight && channels[3] >= 0) {
				sb = sb * sv / 255.0;
				sg = sg * sv / 255.0;
				sr = sr * sv / 255.0;
			}
			sa = (sv / 255.0) * alpha;
			if (c->state.clip_mask != NULL) {
				sa = sa * (double)cg_clip_coverage(c, x, y) / 255.0;
			}
			/* THE SAME TWO RULES AS THE INTERPOLATING PATH ABOVE, over this path's own variables:
			 * `sr`, `sg` and `sb` are red, green and blue, and a gray chart reads one byte into all
			 * three, which is why the single-component case needs no case of its own. */
			if (mask_count > 0) {
				double comp[3];

				comp[0] = sr;
				comp[1] = sg;
				comp[2] = sb;
				if (cg_image_masked_out(mask_colors, mask_count, comp)) {
					continue;
				}
			}
			if (mask_image != NULL) {
				sa = sa * cg_image_mask_coverage(mask_image, mask_decode, u, v);
			}
			if (copy) {
				d[red_offset] = (unsigned char)(sr * sa);
				d[1] = (unsigned char)(sg * sa);
				d[blue_offset] = (unsigned char)(sb * sa);
				d[3] = (unsigned char)(255.0 * sa);
			} else {
				d[red_offset] = (unsigned char)(sr * sa + (double)d[red_offset] * (1.0 - sa));
				d[1] = (unsigned char)(sg * sa + (double)d[1] * (1.0 - sa));
				d[blue_offset] = (unsigned char)(sb * sa + (double)d[blue_offset] * (1.0 - sa));
				d[3] = (unsigned char)((double)d[3] * (1.0 - sa) + 255.0 * sa);
			}
		}
	}
}

/* ------------------------------------------------------------------------- */
/* C6.1: the gradient draw verbs                                             */
/* ------------------------------------------------------------------------- */

/* THE VERBS DIFFER ONLY IN WHICH SAMPLER THEY HAND THE PAINT BUILDER, which is why there is one
 * struct, one draw function and TWO one-line evaluators rather than two copies of the same twenty
 * lines. The geometry the draw call carries — two points, or two circles — is what the samplers
 * (CGGradient.c) know how to read; the DRAWING is here, where the CTM, the clip and the alpha are.
 * A THIRD FIELD SET WENT WITH THE CONIC VERB (2026-10-05) and so did its `angle`, which nothing
 * else read. */
typedef struct {
	CGGradientRef gradient;
	CGPoint start;
	CGPoint end;
	CGFloat r0;
	CGFloat r1;
	CGGradientDrawingOptions options;
} cg_gradient_draw;

static void cg_linear_paint(void *info, CGFloat x, CGFloat y, CGFloat rgba[4])
{
	const cg_gradient_draw *gd = info;

	cg_gradient_linear_sample(gd->gradient, gd->start, gd->end, gd->options, x, y, rgba);
}

static void cg_radial_paint(void *info, CGFloat x, CGFloat y, CGFloat rgba[4])
{
	const cg_gradient_draw *gd = info;

	cg_gradient_radial_sample(gd->gradient, gd->start, gd->r0, gd->end, gd->r1, gd->options, x, y,
				  rgba);
}

/* !! `cg_conic_paint` STOOD HERE, AND ITS CALLER IS GONE (2026-10-05): `CGContextDrawConicGradient`
 * is macOS 14.0 — out of era by eight years — so the verb, this sampler, the parameter it computes
 * and the gradient sample behind it were all removed together. The other two verbs are untouched,
 * which is why `cg_draw_gradient` below still takes a sampler: LINEAR AND RADIAL ARE 10.5 AND THIS
 * ONE WAS THE ODD ONE OUT, not the shape the others are built on. */

/* THE ONE ROAD ONTO THE SURFACE, so the three verbs cannot differ in anything but the sampler. The
 * paint image is built over exactly the rectangle the composite will cover — `cg_paint_extents` is
 * asked once by each — and the user-to-device mapping inside `cg_paint_image` is the CTM, which is
 * what makes a rotated or scaled context do the right thing without any of this knowing about it. */
static void cg_draw_gradient(CGContextRef c, cg_gradient_draw *gd, cg_paint_eval_fn eval)
{
	pixman_image_t *src;
	int px;
	int py;
	int pw;
	int ph;

	if (!cg_paint_extents(c, &px, &py, &pw, &ph)) {
		return;
	}
	src = cg_paint_image(px, py, pw, ph, c->state.ctm, c->state.alpha, eval, gd);
	if (src == NULL) {
		return;
	}
	cg_paint_clip(c, src, cg_op(c->state.blend));
	pixman_image_unref(src);
}

void CGContextDrawLinearGradient(CGContextRef c, CGGradientRef gradient, CGPoint startPoint,
				 CGPoint endPoint, CGGradientDrawingOptions options)
{
	cg_gradient_draw gd;

	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "gradients (CGContextDrawLinearGradient)");
		return;
	}

	if (c == NULL) {
		return;
	}
	if (gradient == NULL) {
		fprintf(stderr, "CG-REFUSE: CGContextDrawLinearGradient needs a gradient; a NULL one "
				"would otherwise paint the clip with nothing and look like a clip that "
				"worked\n");
		return;
	}
	memset(&gd, 0, sizeof(gd));
	gd.gradient = gradient;
	gd.start = startPoint;
	gd.end = endPoint;
	gd.options = options;
	cg_draw_gradient(c, &gd, cg_linear_paint);
}

void CGContextDrawRadialGradient(CGContextRef c, CGGradientRef gradient, CGPoint startCenter,
				 CGFloat startRadius, CGPoint endCenter, CGFloat endRadius,
				 CGGradientDrawingOptions options)
{
	cg_gradient_draw gd;

	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "gradients (CGContextDrawRadialGradient)");
		return;
	}

	if (c == NULL) {
		return;
	}
	if (gradient == NULL) {
		fprintf(stderr, "CG-REFUSE: CGContextDrawRadialGradient needs a gradient\n");
		return;
	}
	memset(&gd, 0, sizeof(gd));
	gd.gradient = gradient;
	gd.start = startCenter;
	gd.end = endCenter;
	gd.r0 = startRadius;
	gd.r1 = endRadius;
	gd.options = options;
	cg_draw_gradient(c, &gd, cg_radial_paint);
}

/* !! `CGContextDrawConicGradient` STOOD HERE AND WAS REMOVED (2026-10-05): it is macOS 14.0, and
 * this duplication is a 10.6-era surface. WHAT THAT COSTS A CALLER IS A VERB AND NOT A COLOUR: the
 * conic ramp's only unique property was that it WRAPS, so no extension flag had anything to
 * describe — an angular ramp between two stops is `CGContextDrawLinearGradient` rotated, which is
 * how a caller of this era draws one. THE PARAMETER CODE WENT WITH IT (`cg_paint_conic_parameter`
 * in CGPaint.c and `cg_gradient_conic_sample` in CGGradient.c), because this verb was their only
 * caller. */

/* THE SHADING'S SAMPLER, AND IT NEEDS NO GEOMETRY FROM HERE: a shading was built axial or radial and
 * carries its own two points, so the context hands over a user-space point and nothing else. That is
 * why there is no `cg_shading_draw` struct beside `cg_gradient_draw` — there is nothing to fill in.
 */
static void cg_shading_paint(void *info, CGFloat x, CGFloat y, CGFloat rgba[4])
{
	cg_shading_sample((CGShadingRef)info, x, y, rgba);
}

void CGContextDrawShading(CGContextRef c, CGShadingRef shading)
{
	if (c != NULL && c->writer != NULL) {
		cg_pdf_writer_refuse(c->writer, "shadings (CGContextDrawShading)");
		return;
	}
	pixman_image_t *src;
	int px;
	int py;
	int pw;
	int ph;

	if (c == NULL) {
		return;
	}
	if (shading == NULL) {
		fprintf(stderr, "CG-REFUSE: CGContextDrawShading needs a shading; a NULL one would "
				"otherwise paint the clip with nothing and look like a clip that "
				"worked\n");
		return;
	}
	if (!cg_paint_extents(c, &px, &py, &pw, &ph)) {
		return;
	}
	/* THE SAME ROAD THE GRADIENTS TAKE, which is the point of the substrate C6.1 built: the shading is
	 * a different SOURCE and nothing else changes — the clip bounds it, the context's alpha multiplies
	 * it, and the blend mode applies, all without this function mentioning any of them. */
	src = cg_paint_image(px, py, pw, ph, c->state.ctm, c->state.alpha, cg_shading_paint, shading);
	if (src == NULL) {
		return;
	}
	cg_paint_clip(c, src, cg_op(c->state.blend));
	pixman_image_unref(src);
}

/* ------------------------------------------------------------------------- */
/* the interpolation quality                                                 */
/* ------------------------------------------------------------------------- */

/* HERE RATHER THAN WITH THE OTHER COLOUR SETTERS, because the one thing it controls is
 * `CGContextDrawImage`, which is in this file's tail. THE TWO LEVELS THIS LIBRARY CANNOT HONOUR ARE
 * REFUSED BY NAME AND NOT SILENTLY MAPPED ONTO THE TWO IT HAS: `kCGInterpolationLow` and
 * `kCGInterpolationHigh` ask for a third and a fourth filter, this file has exactly two samplers
 * (nearest and bilinear), and accepting them would give three different names one behaviour - the
 * same collapse `kCGBlendModePlusDarker` and `kCGPatternTilingConstantSpacing` are refused for. THE
 * STATE IS LEFT AS IT WAS on a refusal, which is the rule the dash pattern and the pattern setter
 * already follow. */
void CGContextSetInterpolationQuality(CGContextRef c, CGInterpolationQuality quality)
{
	if (c == NULL) {
		return;
	}
	switch (quality) {
	case kCGInterpolationNone:
	case kCGInterpolationDefault:
	case kCGInterpolationMedium:
		c->state.interpolation = quality;
		return;
	case kCGInterpolationLow:
	case kCGInterpolationHigh:
	default:
		fprintf(stderr, "CG-REFUSE: this library samples images NEAREST and BILINEAR only, so "
				"kCGInterpolationLow and kCGInterpolationHigh - a third and a fourth filter "
				"- are refused rather than mapped onto one of the two, and the quality is "
				"left as it was\n");
		return;
	}
}

/* `Default` READS BACK AS ITSELF rather than as the sampler it selects, because what the property
 * holds is WHAT THE CALLER ASKED FOR - the same arrangement NSGraphicsContext uses for the options
 * CoreGraphics has no getter for. A NULL context answers `None`, which is also the state a context
 * starts in. */
CGInterpolationQuality CGContextGetInterpolationQuality(CGContextRef c)
{
	return c == NULL ? kCGInterpolationNone : c->state.interpolation;
}

/* ------------------------------------------------------------------------- */
/* Text: the state, and one drawing door                                     */
/* ------------------------------------------------------------------------- */

void CGContextSetFont(CGContextRef c, CGFontRef font)
{
	if (c == NULL) {
		return;
	}
	CGFontRetain(font);		/* retain first, so SetFont(c, c->state.font) is safe */
	CGFontRelease(c->state.font);
	c->state.font = font;
}

/* THE SUBPIXEL PAIR, IN FULL: the context's gate and the state's, which the header says must BOTH be
 * true. The effective question is asked in one place (cg_subpixel_positioning below) so the drawing
 * path cannot get it half right. */
void CGContextSetAllowsFontSubpixelPositioning(CGContextRef c, bool allows)
{
	if (c != NULL) {
		c->allows_subpixel_positioning = allows ? 1 : 0;
	}
}

void CGContextSetShouldSubpixelPositionFonts(CGContextRef c, bool should)
{
	if (c != NULL) {
		c->state.should_subpixel_position_fonts = should ? 1 : 0;
	}
}

/* **SMOOTHING IS IMPLEMENTED, AND IT IS SUB-PIXEL ANTIALIASING: THREE SAMPLES TO THE PIXEL, FILTERED ALONG
 * THEIR ORDER.** The engine renders colour coverage (`cg_font_render_glyph`'s `smoothing`), the samples are
 * lined up with the destination's own sub-columns, and `cg_composite_glyph_smoothed` blends each channel with
 * its own coverage — the one paint in this file that cannot go through pixman, because a mask there is a single
 * coverage for all three channels.
 *
 * THE TWO DOORS RECORD, like the sub-pixel POSITIONING pair above and for the same reason: one is the context's
 * and one is the graphics state's, and the effective question is asked in one place (`cg_font_smoothing`) so
 * that the two cannot disagree.
 *
 * QUANTIZATION IS STILL REFUSED, AND FOR ITS OWN REASON: THE 10.6 HEADER NEVER SAYS WHAT IT QUANTIZES — it says
 * a context "quantizes subpixel positions" and stops — and picking a quantum (a third of a pixel is the usual
 * one) would be this library inventing a contract rather than duplicating one. */
void CGContextSetAllowsFontSmoothing(CGContextRef c, bool allows)
{
	if (c != NULL) {
		c->allows_font_smoothing = allows ? 1 : 0;
	}
}

void CGContextSetShouldSmoothFonts(CGContextRef c, bool should)
{
	if (c != NULL) {
		c->state.should_smooth_fonts = should ? 1 : 0;
	}
}

void CGContextSetAllowsFontSubpixelQuantization(CGContextRef c, bool allows)
{
	(void)c;
	(void)allows;
	fprintf(stderr, "CG-REFUSE: subpixel quantisation is not implemented: the 10.6 header says a "
			"context quantizes subpixel positions and never says to what, so this library either "
			"honours the fraction or rounds the pen\n");
}

void CGContextSetShouldSubpixelQuantizeFonts(CGContextRef c, bool should)
{
	(void)c;
	(void)should;
	fprintf(stderr, "CG-REFUSE: subpixel quantisation (the state's half) is refused for the reason "
			"above\n");
}

void CGContextSelectFont(CGContextRef c, const char *name, CGFloat size, CGTextEncoding textEncoding)
{
	CGFontRef font;

	if (c == NULL) {
		return;
	}
	font = cg_font_create_with_name(name);
	if (font == NULL) {
		fprintf(stderr, "CG-REFUSE: no font named \"%s\" in this system's font directories; the "
			"context keeps the font it had\n", name);
		return;
	}
	CGContextSetFont(c, font);	/* the context retains it */
	CGFontRelease(font);
	CGContextSetFontSize(c, size);
	c->state.text_encoding = textEncoding;
}

void CGContextSetFontSize(CGContextRef c, CGFloat size)
{
	if (c != NULL) {
		c->state.font_size = size;
	}
}

void CGContextSetTextMatrix(CGContextRef c, CGAffineTransform t)
{
	if (c != NULL) {
		c->state.text_matrix = t;
	}
}

CGAffineTransform CGContextGetTextMatrix(CGContextRef c)
{
	return c == NULL ? CGAffineTransformIdentity : c->state.text_matrix;
}

void CGContextSetTextPosition(CGContextRef c, CGFloat x, CGFloat y)
{
	if (c != NULL) {
		c->state.text_position = CGPointMake(x, y);
	}
}

CGPoint CGContextGetTextPosition(CGContextRef c)
{
	return c == NULL ? CGPointMake(0.0, 0.0) : c->state.text_position;
}

void CGContextSetCharacterSpacing(CGContextRef c, CGFloat spacing)
{
	if (c != NULL) {
		c->state.character_spacing = spacing;
	}
}

void CGContextSetTextDrawingMode(CGContextRef c, CGTextDrawingMode mode)
{
	if (c != NULL) {
		c->state.text_mode = mode;
	}
}

static double cg_text_clamp01(double v)
{
	if (v < 0.0) {
		return 0.0;
	}
	if (v > 1.0) {
		return 1.0;
	}
	return v;
}

static int cg_glyph_pen(double v)
{
	return (int)(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* THE ONE PLACE A GLYPH IS PUT ON A SURFACE, and every text door below is a loop over it: the pen is in
 * USER SPACE and the CTM places it, the TEXT MATRIX transforms the glyph itself (see the header). */
/* IS A FRACTIONAL PEN HONOURED? Apple's rule, with every term a door: the context's gate, the state's
 * gate, and that fonts are antialiased when drawn — the last two of which this context already
 * tracks. ONE PLACE ASKS IT, so the fraction and its rounding cannot disagree. */
static int cg_subpixel_positioning(CGContextRef c)
{
	return c->allows_subpixel_positioning && c->state.should_subpixel_position_fonts
	       && c->allows_antialiasing && c->state.antialias;
}

/* **WHETHER A GLYPH IS RASTERISED IN COLOUR: SUB-PIXEL COVERAGE, THREE SAMPLES TO THE PIXEL.** Four terms, and
 * the fourth is the one that makes it correct rather than merely switched on — THE DESTINATION MUST HAVE NO ALPHA
 * CHANNEL. A pixel's three samples attenuate its three channels INDEPENDENTLY and there is only ONE alpha to
 * write, so a smoothed glyph drawn into a canvas with transparency would either paint the average (which is grey
 * coverage by another route, and the whole point lost) or write colour into a layer that will be composited
 * somewhere else and fringe against whatever it lands on. Apple's rule is the same one: smoothing is for opaque
 * destinations. SO A WINDOW'S SURFACE — made with `kCGImageAlphaNoneSkipFirst`, which is what an opaque window
 * IS — gets it, and every offscreen image with an alpha channel does not, without a caller having to say so. */
static int cg_font_smoothing(CGContextRef c)
{
	uint32_t alpha = c->bitmap_info & kCGBitmapAlphaInfoMask;

	if (!c->allows_font_smoothing || !c->state.should_smooth_fonts) {
		return 0;
	}
	if (!c->allows_antialiasing || !c->state.antialias) {
		return 0;
	}
	return (alpha == kCGImageAlphaNone || alpha == kCGImageAlphaNoneSkipFirst
		|| alpha == kCGImageAlphaNoneSkipLast) ? 1 : 0;
}

/* **A SMOOTHED GLYPH, COMPOSITED BY HAND BECAUSE PIXMAN CANNOT DO IT.** Every other paint in this file goes
 * through `pixman_image_composite32`, and text cannot when it is smoothed: a mask there is `PIXMAN_a8`, ONE
 * coverage applying to all three channels, and the whole point of sub-pixel coverage is that the three DIFFER —
 * the destination has to be attenuated per channel, and no pixman operator says that. So the samples are walked
 * directly, which is what every implementation of this does.
 *
 * **AND `left` IS A WHOLE-PIXEL BEARING FOR BOTH RENDERINGS, SO THE SAMPLES LAND WHERE THEY SAY.** The engine
 * reports an LCD bitmap's bearing in PIXELS like a grey one's — MEASURED, and this is the correction a sweep of the
 * pen's phase made: treating it as thirds put the ink in a different sub-column for every phase, and the composited
 * glyph JITTERED by four tenths of a pixel as the pen moved, which is a wobble a reader sees as uneven spacing.
 * The samples of a pixel are its three sub-pixels, in order, so sample `3k + c` is channel `c` of pixel `left + k`
 * and no remainder is involved at all. */
static void cg_composite_glyph_smoothed(CGContextRef c, const unsigned char *bitmap, int nsamples,
					int rows, int left, int dx, int dy)
{
	int slot[3];
	double a = c->state.rgba[3] * c->state.alpha;
	double colour[3];
	const unsigned char *mbits = NULL;
	int mstride = 0, mw = 0, mh = 0;
	int row, sample;

	cg_channel_slots(c->bitmap_info, slot);
	colour[0] = c->state.rgba[0];
	colour[1] = c->state.rgba[1];
	colour[2] = c->state.rgba[2];
	if (c->state.clip_mask != NULL) {
		mbits = (const unsigned char *)pixman_image_get_data(c->state.clip_mask);
		mstride = pixman_image_get_stride(c->state.clip_mask);
		mw = pixman_image_get_width(c->state.clip_mask);
		mh = pixman_image_get_height(c->state.clip_mask);
	}
	for (row = 0; row < rows; row++) {
		const unsigned char *srow = bitmap + (size_t)row * (size_t)nsamples;
		int y = dy + row;

		if (y < 0 || y >= c->height) {
			continue;
		}
		for (sample = 0; sample < nsamples; sample++) {
			int column = left + sample / 3;		/* sample 3k+c is channel c of pixel left + k */
			int channel = sample % 3;
			int x = dx + column;
			unsigned char *d;
			double k, dst;

			if (x < 0 || x >= c->width) {
				continue;
			}
			if (!pixman_region32_contains_point(&c->state.clip, x, y, NULL)) {
				continue;
			}
			k = a * (double)srow[sample] / 255.0;
			if (mbits != NULL) {
				/* A MASK IS ONE COVERAGE PER PIXEL, so it scales all three of that pixel's samples alike — and
				 * a pixel outside the mask's own box reads as coverage 0, which is what a mask IS. */
				int mx = x - c->state.clip_mask_x;
				int my = y - c->state.clip_mask_y;

				if (mx < 0 || my < 0 || mx >= mw || my >= mh) {
					continue;
				}
				k *= (double)mbits[(size_t)my * (size_t)mstride + (size_t)mx] / 255.0;
			}
			if (k <= 0.0) {
				continue;
			}
			d = c->data + (size_t)y * (size_t)c->stride + (size_t)x * 4u;
			/* THE DESTINATION IS OPAQUE — that is the gate this path is entered under — so its bytes ARE its
			 * colours and the blend is the ordinary one: dst = colour*k + dst*(1 - k), per channel. */
			dst = (double)d[slot[channel]] / 255.0;
			d[slot[channel]] = cg_blend_byte(colour[channel] * k + dst * (1.0 - k));
		}
	}
}

static void cg_show_one_glyph_fill(CGContextRef c, CGGlyph glyph, CGPoint pen, pixman_op_t op)
{
	unsigned char *coverage = NULL;
	pixman_image_t *src, *cover;
	pixman_color_t solid;
	CGAffineTransform linear;
	CGPoint device;
	CGPoint fraction;
	double a;
	int w = 0, h = 0, left = 0, top = 0, dx, dy;
	double advance = 0.0;
	/* **SMOOTHING NEEDS THE ORDINARY `OVER` AND NOTHING ELSE**: the hand blend below knows one operator, and
	 * every other mode (plus-darker, and the rest of pixman's) goes through the grey coverage it has always
	 * used. That is a limit and it is stated rather than discovered. */
	int smoothing = cg_font_smoothing(c) && op == PIXMAN_OP_OVER;
	int samples = 1;

	/* THE PEN, PLUS THE TEXT MATRIX'S TRANSLATION: the matrix's SCALE and ROTATION are already in
	 * `linear` below (they rasterise the glyph), and its TRANSLATION is the one part that moves the
	 * glyph relative to the pen — which is what a caller shifting text within a line means by it. */
	pen.x += c->state.text_matrix.tx;
	pen.y += c->state.text_matrix.ty;
	device = CGPointApplyAffineTransform(pen, c->state.ctm);
	linear = CGAffineTransformConcat(c->state.text_matrix, c->state.ctm);
	linear.tx = 0.0;
	linear.ty = 0.0;
	/* THE FRACTION GOES TO THE ENGINE AND THE WHOLE PART PLACES THE BITMAP: the pen's whole device
	 * pixels position the mask (below, as before), and the leftover fraction is what the engine renders
	 * the outline with — so a half-pixel pen draws a half-pixel-shifted glyph rather than the same one
	 * twice. WITH POSITIONING OFF THE FRACTION IS ZERO, which is what rounding the pen IS. */
	fraction.x = cg_subpixel_positioning(c) ? device.x - (CGFloat)cg_glyph_pen(device.x) : 0.0;
	fraction.y = cg_subpixel_positioning(c) ? device.y - (CGFloat)cg_glyph_pen(device.y) : 0.0;
	if (!cg_font_render_glyph(c->state.font, glyph, c->state.font_size, linear, fraction, smoothing,
				  &coverage, &w, &h, &left, &top, &advance, &samples)) {
		return;
	}
	if (w <= 0 || h <= 0) {
		/* NO INK, WHICH IS NOT A FAILURE: a space advances and draws nothing. */
		free(coverage);
		return;
	}
	dx = cg_glyph_pen(device.x);
	dy = cg_glyph_pen(device.y) - top;
	/* **THE ANSWER SAYS WHAT WAS RENDERED, NOT WHAT WAS ASKED FOR**: a glyph the engine could not turn comes
	 * back grey, and it is blended as grey. */
	if (samples == 3) {
		cg_composite_glyph_smoothed(c, coverage, w, h, left, dx, dy);
		free(coverage);
		return;
	}
	dx += left;
	/* THE MASK IS THE GLYPH'S OWN BOX, WHICH IS THE WHOLE POINT OF IT: `w` AND `h` COME BACK FROM THE
	 * ENGINE, so both the mask and the composite can be the glyph's pixels rather than the surface's. The
	 * first version built a FULL-SURFACE mask and composited the WHOLE SURFACE per glyph — 2.07M pixels of
	 * calloc and 2.07M of composite for a 30x40 glyph — and MEASURED (2026-10-06) that as 31.1 ms for the
	 * demo's six text runs, 113 glyphs, against 0.0 ms for resolving the font six times. The work is now
	 * the glyph's own area, and the box lands EXACTLY where it did: the mask is placed at `dx`/`dy` by the
	 * destination coordinates instead of by the copy loop, and pixman clips whatever falls off the surface
	 * — which is what the per-pixel bounds test used to do. */
	cover = pixman_image_create_bits(PIXMAN_a8, w, h, NULL, 0);
	if (cover == NULL) {
		free(coverage);
		return;
	}
	{
		uint8_t *cd = (uint8_t *)pixman_image_get_data(cover);
		int cs = pixman_image_get_stride(cover);
		int row;

		for (row = 0; row < h; row++) {
			memcpy(cd + (size_t)row * (size_t)cs, coverage + (size_t)row * (size_t)w,
			       (size_t)w);
		}
	}
	free(coverage);
	a = c->state.rgba[3] * c->state.alpha;
	solid.red = (uint16_t)(cg_text_clamp01(c->state.rgba[0] * a) * 65535.0 + 0.5);
	solid.green = (uint16_t)(cg_text_clamp01(c->state.rgba[1] * a) * 65535.0 + 0.5);
	solid.blue = (uint16_t)(cg_text_clamp01(c->state.rgba[2] * a) * 65535.0 + 0.5);
	solid.alpha = (uint16_t)(cg_text_clamp01(a) * 65535.0 + 0.5);
	src = pixman_image_create_solid_fill(&solid);
	if (src == NULL) {
		pixman_image_unref(cover);
		return;
	}
	pixman_image_set_clip_region32(c->image, &c->state.clip);
	if (c->state.clip_mask != NULL) {
		/* THE GLYPH'S BOX READS THE CLIP AT `dx`/`dy` LESS THE CLIP'S OWN ORIGIN, while writing its
		 * own corner — the rule every masked composite in this file now follows. */
		pixman_image_composite32(PIXMAN_OP_IN, c->state.clip_mask, NULL, cover,
					 dx - c->state.clip_mask_x, dy - c->state.clip_mask_y, 0, 0, 0, 0,
					 w, h);
	}
	if (op == CG_OP_PLUS_DARKER) {
		/* PIXMAN'S SOLID-FILL SOURCE HAS NO READABLE BITS and the hand composite needs them, so for
		 * plus-darker the glyph's colour is handed over as the SAME 1x1 a8r8g8b8 image every other
		 * paint of a colour already uses (`cg_fill_path`), which the loop reads and repeats. */
		uint32_t pixel = cg_premultiplied_pixel(c->state.rgba[0], c->state.rgba[1],
							c->state.rgba[2], a);
		pixman_image_t *bits = pixman_image_create_bits(PIXMAN_a8r8g8b8, 1, 1, &pixel, 4);

		if (bits != NULL) {
			pixman_image_set_repeat(bits, PIXMAN_REPEAT_NORMAL);
			cg_composite(c, op, bits, cover, 0, 0, 0, 0, dx, dy, w, h);
			pixman_image_unref(bits);
		}
	} else {
		cg_composite(c, op, src, cover, 0, 0, 0, 0, dx, dy, w, h);
	}
	pixman_image_unref(src);
	pixman_image_unref(cover);
}

/* THE REFUSALS, IN ONE PLACE: every text door draws through `cg_show_one_glyph`, so the conditions are
 * asked once here rather than four times. */
static int cg_text_can_draw(CGContextRef c)
{
	if (c == NULL || c->data == NULL) {
		return 0;
	}
	if (c->state.font == NULL) {
		fprintf(stderr, "CG-REFUSE: no font is set on this context (CGContextSetFont)\n");
		return 0;
	}
	if (c->state.font_size <= 0.0) {
		fprintf(stderr, "CG-REFUSE: the font size is not positive (CGContextSetFontSize)\n");
		return 0;
	}
	return 1;
}

/* THE FONT'S OWN ADVANCE FOR ONE GLYPH, IN USER SPACE: font units scaled by the font size, because the
 * size IS in user-space units. Zero for a font with no units-per-em or a glyph the engine cannot measure
 * — a call that draws the glyph where it is and then does not move, rather than a refusal. */
static double cg_glyph_advance(CGContextRef c, CGGlyph glyph)
{
	int units = CGFontGetUnitsPerEm(c->state.font);
	int advance = 0;

	if (units <= 0 || !CGFontGetGlyphAdvances(c->state.font, &glyph, 1, &advance)) {
		return 0.0;
	}
	return (double)advance * c->state.font_size / (double)units;
}

/* ------------------------------------------------------------------------- */
/* Text as GEOMETRY: the modes a mask cannot draw                              */
/* ------------------------------------------------------------------------- */

/* THE SINK'S CALLBACKS APPEND TO A PATH, and they are the only place the pen is added: the seam hands out
 * points in user space RELATIVE TO THE PEN, already scaled by the em and transformed by the text matrix. */
struct cg_text_path_info {
	CGMutablePathRef path;
	CGPoint pen;
};

static void cg_text_move_to(void *info, CGFloat x, CGFloat y)
{
	struct cg_text_path_info *ti = info;

	CGPathMoveToPoint(ti->path, NULL, ti->pen.x + x, ti->pen.y + y);
}

static void cg_text_line_to(void *info, CGFloat x, CGFloat y)
{
	struct cg_text_path_info *ti = info;

	CGPathAddLineToPoint(ti->path, NULL, ti->pen.x + x, ti->pen.y + y);
}

static void cg_text_conic_to(void *info, CGFloat cx, CGFloat cy, CGFloat x, CGFloat y)
{
	struct cg_text_path_info *ti = info;

	CGPathAddQuadCurveToPoint(ti->path, NULL, ti->pen.x + cx, ti->pen.y + cy, ti->pen.x + x,
				  ti->pen.y + y);
}

static void cg_text_cubic_to(void *info, CGFloat c1x, CGFloat c1y, CGFloat c2x, CGFloat c2y, CGFloat x,
			     CGFloat y)
{
	struct cg_text_path_info *ti = info;

	CGPathAddCurveToPoint(ti->path, NULL, ti->pen.x + c1x, ti->pen.y + c1y, ti->pen.x + c2x,
			      ti->pen.y + c2y, ti->pen.x + x, ti->pen.y + y);
}

static void cg_text_close(void *info)
{
	struct cg_text_path_info *ti = info;

	CGPathCloseSubpath(ti->path);
}

/* THE GLYPH AS A PATH, IN USER SPACE AT THE PEN. NULL means the font refused; an EMPTY path means the glyph
 * has no outline (a space), which the modes below treat as nothing to draw rather than a failure. */
static CGMutablePathRef cg_text_path(CGContextRef c, CGGlyph glyph, CGPoint pen)
{
	static const cg_outline_sink sink = {
		cg_text_move_to, cg_text_line_to, cg_text_conic_to, cg_text_cubic_to, cg_text_close
	};
	CGMutablePathRef path = CGPathCreateMutable();
	struct cg_text_path_info info;

	if (path == NULL) {
		return NULL;
	}
	info.path = path;
	info.pen = pen;
	if (!cg_font_glyph_outline(c->state.font, glyph, c->state.font_size, c->state.text_matrix,
				   &sink, &info)) {
		CGPathRelease((CGPathRef)path);
		return NULL;
	}
	return path;
}

/* A TEXT FILL WITH THE CURRENT PAINT — the same two branches `cg_fill_current_path` takes, because a text
 * fill is a fill and a pattern fill of glyphs has to be the same thing as a pattern fill of anything else. */
static void cg_text_paint(CGContextRef c, CGPathRef path, pixman_op_t op)
{
	if (c->state.fill_pattern != NULL) {
		cg_paint_path(c, path, 0, op, NULL, c->state.fill_pattern,
			      c->state.alpha * c->state.fill_pattern_alpha);
	} else {
		cg_paint_path(c, path, 0, op, c->state.rgba, NULL, c->state.alpha);
	}
}

/* ONE GLYPH, IN WHICHEVER MODE THE CALLER ASKED FOR. The mask route is kept for `kCGTextFill` — it is the
 * one the rasteriser and the subpixel delta were built for — and every other mode is GEOMETRY through the
 * path, which is also what makes clipping a glyph expressible at all. */
static void cg_show_glyph_mode(CGContextRef c, CGGlyph glyph, CGPoint pen, pixman_op_t op)
{
	CGMutablePathRef path;
	int mode = (int)c->state.text_mode;

	if (c->writer != NULL) {
		cg_pdf_paint_state ps;

		if (mode == kCGTextInvisible) {
			return;
		}
		if (mode == kCGTextStrokeClip || mode == kCGTextFillStrokeClip) {
			fprintf(stderr, "CG-REFUSE: text drawing mode %d is not recorded by the PDF context\n", mode);
			return;
		}
		fn_pdf_paint_state(c, &ps);
		/* **TEXT FIRST, OUTLINES ONLY WHEN THE FACE CANNOT BE EMBEDDED.** A TrueType-programmed face becomes a
		 * Type0/CIDFontType2 font with Identity-H, so the page carries real text; a CFF or program-less face
		 * falls through to the outline path below. */
		if ((mode == kCGTextFill || mode == kCGTextStroke || mode == kCGTextFillStroke) && glyph != 0 &&
		    cg_pdf_writer_text_glyph(c->writer, c->state.font, glyph, pen, c->state.font_size,
					     c->state.text_matrix, mode, &ps)) {
			return;
		}
		path = cg_text_path(c, glyph, pen);
		if (path == NULL) {
			return;
		}
		switch (mode) {
		case kCGTextFill:
			cg_pdf_writer_paint_path(c->writer, (CGPathRef)path, CG_PDF_PAINT_FILL, &ps);
			break;
		case kCGTextStroke:
			cg_pdf_writer_paint_path(c->writer, (CGPathRef)path, CG_PDF_PAINT_STROKE, &ps);
			break;
		case kCGTextFillStroke:
			cg_pdf_writer_paint_path(c->writer, (CGPathRef)path, CG_PDF_PAINT_FILLSTROKE, &ps);
			break;
		case kCGTextClip:
			cg_pdf_writer_clip_path(c->writer, (CGPathRef)path, false);
			break;
		case kCGTextFillClip:
			cg_pdf_writer_paint_path(c->writer, (CGPathRef)path, CG_PDF_PAINT_FILL, &ps);
			cg_pdf_writer_clip_path(c->writer, (CGPathRef)path, false);
			break;
		default:
			break;
		}
		CGPathRelease((CGPathRef)path);
		return;
	}
	if (mode == kCGTextFill) {
		cg_show_one_glyph_fill(c, glyph, pen, op);
		return;
	}
	if (mode == kCGTextInvisible) {
		/* NO INK AND NO REFUSAL: the mode a caller MEASURES with, and the pen still advances. */
		return;
	}
	if (mode != kCGTextStroke && mode != kCGTextFillStroke && mode != kCGTextClip
	    && mode != kCGTextFillClip) {
		/* THE TWO REFUSED MODES, BY NAME: `kCGTextStrokeClip` and `kCGTextFillStrokeClip` both need a
		 * decision the 10.6 header does not make — whether the clip is the glyph's OUTLINE or the STROKED
		 * region around it — and inventing one would be this library writing a contract rather than
		 * duplicating one. (The STROKER exists, `cg_path_create_stroked_copy`, so the work is small; the
		 * missing part is the SEMANTICS.) */
		fprintf(stderr, "CG-REFUSE: text drawing mode %d is not implemented: whether its clip is the "
				"glyph's outline or the stroked region is a decision the header does not make\n",
			mode);
		return;
	}
	path = cg_text_path(c, glyph, pen);
	if (path == NULL) {
		return;
	}
	if (mode == kCGTextFill || mode == kCGTextFillStroke || mode == kCGTextFillClip) {
		cg_text_paint(c, (CGPathRef)path, op);
	}
	if (mode == kCGTextStroke || mode == kCGTextFillStroke) {
		/* FILL FIRST, THEN STROKE, which is what the mode's name says. */
		cg_stroke_path_with_width(c, (CGPathRef)path, c->state.line_width);
	}
	if (mode == kCGTextClip || mode == kCGTextFillClip) {
		cg_clip_to_path(c, (CGPathRef)path, 0);
	}
	CGPathRelease((CGPathRef)path);
}

void CGContextShowGlyphsAtPositions(CGContextRef c, const CGGlyph glyphs[], const CGPoint positions[],
				    size_t count)
{
	pixman_op_t op;
	size_t i;

	if (c == NULL || glyphs == NULL || positions == NULL || !cg_text_can_draw(c)) {
		return;
	}
	op = cg_op(c->state.blend);
	pixman_image_set_clip_region32(c->image, &c->state.clip);
	for (i = 0; i < count; i++) {
		cg_show_glyph_mode(c, glyphs[i], positions[i], op);
	}
}

void CGContextShowGlyphs(CGContextRef c, const CGGlyph glyphs[], size_t count)
{
	CGPoint pen;
	pixman_op_t op;
	size_t i;

	if (c == NULL || glyphs == NULL || !cg_text_can_draw(c)) {
		return;
	}
	op = cg_op(c->state.blend);
	pen = c->state.text_position;
	pixman_image_set_clip_region32(c->image, &c->state.clip);
	for (i = 0; i < count; i++) {
		cg_show_glyph_mode(c, glyphs[i], pen, op);
		pen.x += (CGFloat)(cg_glyph_advance(c, glyphs[i]) + (double)c->state.character_spacing);
	}
	/* THE PEN IS THE TEXT POSITION'S, SO IT MOVES: that is what makes the position state worth having,
	 * and the probe reads it back with `CGContextGetTextPosition`. */
	c->state.text_position = pen;
}

void CGContextShowGlyphsAtPoint(CGContextRef c, CGFloat x, CGFloat y, const CGGlyph glyphs[], size_t count)
{
	if (c == NULL) {
		return;
	}
	c->state.text_position = CGPointMake(x, y);
	CGContextShowGlyphs(c, glyphs, count);
}

/* THE BYTE PATH, WHICH IS THE GLYPH PATH WITH A MAPPING IN FRONT OF IT: each byte becomes a glyph
 * through the engine's character map and then the loop is the one above. A byte with no glyph is
 * SKIPPED (see the header) — that is a `continue`, not an advance. */
void CGContextShowText(CGContextRef c, const char *string, size_t length)
{
	CGPoint pen;
	pixman_op_t op;
	size_t i;

	if (c == NULL || string == NULL || !cg_text_can_draw(c)) {
		return;
	}
	op = cg_op(c->state.blend);
	pen = c->state.text_position;
	pixman_image_set_clip_region32(c->image, &c->state.clip);
	for (i = 0; i < length; i++) {
		CGGlyph g = cg_font_glyph_for_byte(c->state.font, (unsigned char)string[i],
						   c->state.text_encoding == kCGEncodingMacRoman);

		if (g == (CGGlyph)kCGFontIndexInvalid) {
			continue;
		}
		cg_show_glyph_mode(c, g, pen, op);
		pen.x += (CGFloat)(cg_glyph_advance(c, g) + (double)c->state.character_spacing);
	}
	c->state.text_position = pen;
}

void CGContextShowTextAtPoint(CGContextRef c, CGFloat x, CGFloat y, const char *string, size_t length)
{
	if (c == NULL) {
		return;
	}
	c->state.text_position = CGPointMake(x, y);
	CGContextShowText(c, string, length);
}

void CGContextShowGlyphsWithAdvances(CGContextRef c, const CGGlyph glyphs[], const CGSize advances[],
				     size_t count)
{
	CGPoint pen;
	pixman_op_t op;
	size_t i;

	if (c == NULL || glyphs == NULL || advances == NULL || !cg_text_can_draw(c)) {
		return;
	}
	op = cg_op(c->state.blend);
	pen = c->state.text_position;
	pixman_image_set_clip_region32(c->image, &c->state.clip);
	for (i = 0; i < count; i++) {
		cg_show_glyph_mode(c, glyphs[i], pen, op);
		pen.x += advances[i].width + c->state.character_spacing;
		pen.y += advances[i].height;
	}
	c->state.text_position = pen;
}
