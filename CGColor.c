/*
 * CGColor — the value type, and the two DEVICE spaces it can read.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 */
#include <CoreGraphics/CGColor.h>
#include <CoreGraphics/CGColorSpace_internal.h>

#include <lcms2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* THE MOST COMPONENTS A COLOUR CAN HOLD. RGB needs three and grayscale one; CMYK needs four.
 * The array is fixed rather than allocated because a colour is a small value that is created
 * and thrown away by the thousands while drawing, and an allocation per colour would be a
 * price paid on every one of them for a struct that fits in a cache line. The bound is kept
 * honest by `cg_color_space_components` refusing any model it does not have a count for. */
#define CG_COLOR_MAX_COMPONENTS 8

/* **A COLOUR IS A REAL OBJECTIVE-C OBJECT SINCE `dec-c21da7fdccafc70b`** (user, 2026-10-09), the same
 * decision that made every Core Text object one (`dec-35c114bf75c7c2d7`) — and it was a SMALL unit here for
 * a reason worth recording: this layout is declared in THIS FILE ONLY (measured: no other file under
 * `CoreGraphics/` reads a colour's fields, and no field-by-name access appears anywhere else in the
 * library), so the conversion is one file's private storage rather than a refactor. **AND THIS FILE IS
 * `CGColor.m` NOW, NOT `CGColor.c`**, because a C file cannot see an Objective-C ivar — the rule that also
 * merged `CTFont.c` into `CTFont.m`. Every build list here is a wildcard (`*.c` and `*.m`), so the rename
 * needed no makefile edit.
 *
 * **IT SUBCLASSES `NSObject`, FOR THE MEASURED REASON CT-M1e-a RECORDED**: this library already links
 * Foundation (its Objective-C halves have since the colour-space names), and a hand-written root class owes
 * the runtime about fifteen methods NSObject already has. The C API is untouched: `CGColorRef` points at
 * this class and the count doors below are thin wrappers.
 *
 * **AND ONE THING A SCRIPT'S OWN GUARD CAUGHT, WHICH IS WHY THE GUARD WAS THERE**: a colour can be made
 * WITH A PATTERN as well as from components, so there are TWO places a colour is allocated — the first
 * version of this conversion assumed one, and its guard refused to write anything until both were handled.
 * The count is no longer a field: it is the runtime's. */
#include <CoreFoundation/CoreFoundation.h>

/* **THE COLOUR IS A CORE FOUNDATION TYPE**, not an Objective-C class: `CGColorRef` is a `CFTypeRef`,
 * `CGColorGetTypeID()` is the identity Apple's header declares, and the storage is a `CFRuntimeBase`
 * followed by the fields below. The Fiwix tree made this an `NSObject` subclass because its Core
 * Graphics already linked Foundation; Agora's does not — Core Graphics depends on Core Foundation
 * (PROVENANCE.md) — so the colour is registered with Core Foundation's runtime. That is also what
 * makes a `CGColorRef` bridge to an `NSColor`.
 */
struct __CGColor {
	CFRuntimeBase base;
	CGColorSpaceRef space;                    /* RETAINED, so the colour owns it */
	int ncomp;                                /* the SPACE's components, alpha NOT counted */
	CGFloat comp[CG_COLOR_MAX_COMPONENTS];    /* the components, ALPHA LAST */
	/* THE PATTERN, FOR A COLOUR THAT HAS ONE (C6.3). A pattern colour's components are its ALPHA and
	 * nothing else — `ncomp` is 1 and `comp[0]` is that alpha — because the colours come from the
	 * pattern's cell. The pointer is RETAINED here, which is what lets a caller release their own
	 * reference the moment the colour is made. */
	CGPatternRef pattern;
};

/* THE SPACE AND THE PATTERN ARE RELEASED WITH THE COLOUR — `finalize` is the only place that happens. */
static void cg_color_finalize(CFTypeRef cf)
{
	struct __CGColor *color = (struct __CGColor *)cf;

	if (color->pattern != NULL) {
		CGPatternRelease(color->pattern);
	}
	if (color->space != NULL) {
		CGColorSpaceRelease(color->space);
	}
}

static const CFRuntimeClass cg_color_class = {
	0,                  /* version */
	"CGColor",          /* className */
	NULL,               /* init */
	NULL,               /* copy */
	cg_color_finalize,  /* finalize */
	NULL,               /* equal */
	NULL,               /* hash */
	NULL,               /* copyFormattingDesc */
	NULL,               /* copyDebugDesc */
	NULL,               /* reclaim */
	NULL,               /* refcount */
	0                   /* requiredAlignment */
};

CFTypeID CGColorGetTypeID(void)
{
	static CFTypeID type_id = 0;

	if (type_id == 0) {
		type_id = _CFRuntimeRegisterClass(&cg_color_class);
	}
	return type_id;
}

/* THE ALLOCATION, IN ONE PLACE: `_CFRuntimeCreateInstance` gives the `CFRuntimeBase` a reference count
 * of 1 and zeroes the extra bytes, so a create door only has to fill the fields. */
static struct __CGColor *cg_color_alloc(void)
{
	return (struct __CGColor *)_CFRuntimeCreateInstance(kCFAllocatorDefault, CGColorGetTypeID(),
			(CFIndex)(sizeof(struct __CGColor) - sizeof(CFRuntimeBase)), NULL);
}

/* THE ONE PLACE THAT DECIDES WHICH SPACES THIS LIBRARY CAN READ, so that `CGColorCreate` and
 * the context's colour setters cannot disagree about it. -1 means "not a space this tree can
 * interpret", and every caller refuses on it. */
static int cg_color_space_components(CGColorSpaceRef space)
{
	if (space == NULL) {
		return -1;
	}
	switch (CGColorSpaceGetModel(space)) {
	case kCGColorSpaceModelMonochrome:
		return 1;
	case kCGColorSpaceModelRGB:
		return 3;
	case kCGColorSpaceModelCMYK:
		return 4;
	case kCGColorSpaceModelLab:
		return 3;   /* L*, a*, b* */
	/* !! `case kCGColorSpaceModelXYZ: return 3;` STOOD HERE (2026-10-05): the case went with the
	 * enum member, so an XYZ colour cannot be created any more and the default arm answers it —
	 * which is the right answer rather than a lost one, because X, Y and Z are not device numbers
	 * and nothing here converts them. */
	default:
		/* A SPACE WHOSE NUMBERS THIS TREE CANNOT INTERPRET IS NOT GUESSED AT. A space this
		 * code has no model for would have its components read as if they were a device
		 * model, which draws something, and something wrong. */
		return -1;
	}
}

CGColorRef CGColorCreate(CGColorSpaceRef space, const CGFloat components[])
{
	CGColorRef color;
	int ncomp;
	int i;

	ncomp = cg_color_space_components(space);
	if (ncomp < 0 || components == NULL) {
		fprintf(stderr, "CG-REFUSE: CGColorCreate needs a color space this tree can read "
				"(grayscale, RGB or CMYK) and a non-NULL component array\n");
		return NULL;
	}
	color = cg_color_alloc();
	if (color == NULL) {
		return NULL;
	}
	color->space = CGColorSpaceRetain(space);
	color->ncomp = ncomp;
	/* ALPHA LAST means ncomp + 1 values, so the loop is `<=` and not `<`. */
	for (i = 0; i <= ncomp; i++) {
		color->comp[i] = components[i];
	}
	return color;
}

CGColorRef CGColorCreateGenericGray(CGFloat gray, CGFloat alpha)
{
	CGColorSpaceRef space;
	CGColorRef color;
	CGFloat comp[2];

	space = CGColorSpaceCreateDeviceGray();
	if (space == NULL) {
		return NULL;
	}
	comp[0] = gray;
	comp[1] = alpha;
	color = CGColorCreate(space, comp);
	CGColorSpaceRelease(space);
	return color;
}

CGColorRef CGColorCreateGenericRGB(CGFloat red, CGFloat green, CGFloat blue, CGFloat alpha)
{
	CGColorSpaceRef space;
	CGColorRef color;
	CGFloat comp[4];

	space = CGColorSpaceCreateDeviceRGB();
	if (space == NULL) {
		return NULL;
	}
	comp[0] = red;
	comp[1] = green;
	comp[2] = blue;
	comp[3] = alpha;
	color = CGColorCreate(space, comp);
	CGColorSpaceRelease(space);
	return color;
}

/* THE FOURTH DEVICE-SPACE CREATOR, and the only one whose numbers this library has to interpret before it
 * can draw them: `cg_device_cmyk_to_rgb` holds the rule and the record of whose decision it was. FIVE
 * VALUES — four inks and the alpha LAST, like the two creators above — because `CGColorCreate` reads
 * `components[0..ncomp]` and the device CMYK space's ncomp is 4. */
CGColorRef CGColorCreateGenericCMYK(CGFloat cyan, CGFloat magenta, CGFloat yellow, CGFloat black,
				    CGFloat alpha)
{
	CGColorSpaceRef space;
	CGColorRef color;
	CGFloat comp[5];

	space = CGColorSpaceCreateDeviceCMYK();
	if (space == NULL) {
		return NULL;
	}
	comp[0] = cyan;
	comp[1] = magenta;
	comp[2] = yellow;
	comp[3] = black;
	comp[4] = alpha;
	color = CGColorCreate(space, comp);
	CGColorSpaceRelease(space);
	return color;
}

CGColorRef CGColorCreateCopy(CGColorRef color)
{
	if (color == NULL) {
		return NULL;
	}
	/* A PATTERN COLOUR IS COPIED THROUGH ITS OWN DOOR, because its components are not a colour in its
	 * space: `CGColorCreate` refuses a pattern space (its numbers mean nothing without a pattern), so
	 * a copy that went that way would return NULL for every pattern colour while looking right. */
	if (color->pattern != NULL) {
		return CGColorCreateWithPattern(color->space, color->pattern, color->comp);
	}
	/* OTHERWISE A COPY KEEPS THE SAME SPACE OBJECT, not a duplicate of it: a colour space here is an
	 * immutable description of how components are to be read, so two colours sharing one is what it
	 * is for. */
	return CGColorCreate(color->space, color->comp);
}

/* THE PATTERN COLOUR — a colour whose paint is a DRAWING. See CGColor.h for the contract; what is
 * worth saying here is the COMPONENT COUNT: `ncomp` is set to ZERO, so this colour has ONE component
 * counting the alpha, which is what Apple's own documentation gives for a coloured pattern colour
 * ("the array has one element, the alpha value"). The alternative encoding — one space component plus
 * an alpha — would make `CGColorGetNumberOfComponents` answer 2 and disagree with the documentation
 * this library is a duplication of. */
CGColorRef CGColorCreateWithPattern(CGColorSpaceRef space, CGPatternRef pattern,
				    const CGFloat *components)
{
	CGColorRef color;

	if (space == NULL || CGColorSpaceGetModel(space) != kCGColorSpaceModelPattern) {
		fprintf(stderr, "CG-REFUSE: CGColorCreateWithPattern needs a pattern colour space "
				"(CGColorSpaceCreatePattern); a colour's other spaces have numbers, and "
				"this one has a pattern\n");
		return NULL;
	}
	if (pattern == NULL) {
		fprintf(stderr, "CG-REFUSE: CGColorCreateWithPattern needs a pattern\n");
		return NULL;
	}
	if (components == NULL) {
		fprintf(stderr, "CG-REFUSE: CGColorCreateWithPattern needs the pattern's alpha; a NULL "
				"array is refused rather than read as an opaque pattern\n");
		return NULL;
	}
	color = cg_color_alloc();
	if (color == NULL) {
		return NULL;
	}
	color->space = CGColorSpaceRetain(space);
	color->pattern = CGPatternRetain(pattern);
	color->ncomp = 0;
	color->comp[0] = components[0];
	return color;
}

CGPatternRef CGColorGetPattern(CGColorRef color)
{
	return color == NULL ? NULL : color->pattern;
}

CGColorRef CGColorCreateCopyWithAlpha(CGColorRef color, CGFloat alpha)
{
	CGColorRef copy;

	copy = CGColorCreateCopy(color);
	if (copy == NULL) {
		return NULL;
	}
	copy->comp[copy->ncomp] = alpha;
	return copy;
}

CGColorRef CGColorRetain(CGColorRef color)
{
	return (CGColorRef)CFRetain(color);
}

/* AND THE RELEASE IS WHERE THE SPACE AND THE PATTERN GO — through `-dealloc`, the one place that lifetime
 * ends now. */
void CGColorRelease(CGColorRef color)
{
	CFRelease(color);
}

CGColorSpaceRef CGColorGetColorSpace(CGColorRef color)
{
	return color == NULL ? NULL : color->space;
}

const CGFloat *CGColorGetComponents(CGColorRef color)
{
	return color == NULL ? NULL : color->comp;
}

size_t CGColorGetNumberOfComponents(CGColorRef color)
{
	return color == NULL ? 0 : (size_t)(color->ncomp + 1);
}

CGFloat CGColorGetAlpha(CGColorRef color)
{
	return color == NULL ? 0.0 : color->comp[color->ncomp];
}

bool CGColorEqualToColor(CGColorRef color1, CGColorRef color2)
{
	int i;

	if (color1 == NULL || color2 == NULL) {
		return false;
	}
	/* THE SPACE'S MODEL AND NOT THE SPACE OBJECT. Two colours made from two separate device-RGB
	 * spaces are the same colour, because a device space IS a statement that the numbers are
	 * the ones to blend - there is nothing else to them. When a colour carries an ICC profile
	 * the profile will be part of the comparison and this line will have to say so. */
	if (CGColorSpaceGetModel(color1->space) != CGColorSpaceGetModel(color2->space) ||
	    color1->ncomp != color2->ncomp) {
		return false;
	}
	/* TWO PATTERN COLOURS ARE EQUAL WHEN THEY ARE THE SAME PATTERN AT THE SAME ALPHA, and the
	 * components alone cannot say so: two pattern colours made from two different patterns both have
	 * one component, so comparing just that would call a polka dot equal to a checkerboard. */
	if (color1->pattern != color2->pattern) {
		return false;
	}
	for (i = 0; i <= color1->ncomp; i++) {
		if (color1->comp[i] != color2->comp[i]) {
			return false;
		}
	}
	return true;
}

/* ---------------------------------------------------------------------------------------
 * THE CONVERSION. Every line below is the engine's (lcms2), and everything above it in this
 * file treats a colour as numbers in a named space; this is the one place where the numbers
 * are interpreted.
 * ------------------------------------------------------------------------------------- */

/* !! THE COLOUR CONVERSION STOOD HERE AND WAS REMOVED (2026-10-05): the function
 * `CGColorCreateCopyByMatchingToColorSpace` and the THREE ENGINE HELPERS that existed for it and
 * nothing else — `cg_engine_intent`, `cg_engine_format` and `cg_engine_profile_for`, which mapped
 * this library's rendering intents and space models onto the engine's and built the profile a
 * transform needs. All three were `static`, so `nm` cannot be asked about them; what is asked
 * about is the function, and it is gone from the built library.
 *
 * IT IS macOS 10.11 — THE VERSION THAT ADDED COLOUR CONVERSION TO THIS API — AGAINST A 10.6-ERA
 * SURFACE, WHICH HAS NONE. So this is not a name dropped for a name's sake: after this, NOTHING
 * in this library or the AppKit re-expresses a colour in another space. The engine (lcms2) stays
 * linked and still parses ICC profiles, and that is the whole of its remaining job.
 *
 * WHAT HAPPENS AT EACH DOOR THAT USED TO CONVERT, all of them refusing BY NAME rather than
 * approximating, so that one colour is either paintable everywhere or refused everywhere:
 *
 *   * `cg_color_to_rgba` (CGContext.c) — the context's colour setters. It already refused a colour
 *     it could not read; now the refusal is every model that is not RGB or gray.
 *   * `cg_paint_device_rgb_from_color` (CGPaint.c) — the paints, so a Lab or ICC fill, gradient
 *     stop or pattern colour paints nothing rather than painting the wrong thing.
 *   * `fn_components_in` (NSColor.m) — the AppKit's component getters, which answer 0 and let
 *     their callers raise NSInternalInconsistencyException.
 *
 * THE CHANNEL REORDERING INSIDE A FAMILY STAYS — one gray value into the three RGB channels and
 * back — because that is not a colour-space transform and because `CGContextSetGrayFillColor` and
 * `-[NSColor whiteComponent]` are 10.0-era doors built on it.
 *
 * THE MEASUREMENTS THE CONVERSION ESTABLISHED ARE RECORDED IN HISTORY WITH IT, and they are the
 * reason this is a real loss rather than a tidy-up: the ICC PCS white point landing EXACTLY white
 * in device RGB; BT.2020's linear segment decoding 0.02 to 0.02/4.5; DCI-P3 and Display P3
 * converting one grey differently; a neutral Lab colour staying neutral to within two percent
 * across D50→D65 (the first version of that check demanded exact equality and could only ever
 * have passed by luck); and the sRGB curve's toe, which told a piecewise space from a power one.
 */
