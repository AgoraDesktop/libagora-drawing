/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * drawing — libagora-drawing's own test suite.
 *
 * WHY THIS EXISTS AT ALL: until it did, NOTHING IN THIS TREE HELD THIS LIBRARY. Every defect that turned
 * up in it was found by a throwaway probe in /tmp — the CFEqual segfault that made
 * CGColorSpaceCreateWithName crash on kCGColorSpaceAdobeRGB1998 among them — which means the verification
 * existed only as long as the probe file did, and nothing would notice the defect coming back. The
 * bridge's suite in libagora-base is a different thing: it tests the two libraries TOGETHER and knows
 * nothing about CoreGraphics' own surface.
 *
 * ONE CHILD PER CASE, COPIED FROM THAT SUITE BECAUSE THE REASON IS THE SAME: three different things can
 * end a case — a wrong answer (counted), an assertion, and a CRASH, which no handler can see. A crash in a
 * single-process run loses every case after it; a child costs one case and the tally survives. The counts
 * cannot cross the fork, so the child reports through its exit status and the parent counts CASES, while
 * the FAIL detail lines still come from the child on the same stderr.
 *
 * THE CASES ARE THE ONES THAT WERE ONLY EVER VERIFIED BY HAND, and each one names the thing it is
 * protecting rather than asserting a number for its own sake: the two promises CGTypeID.c makes are
 * stability and uniqueness, NOT particular ids — so a reshuffle of the table must not fail this suite,
 * while two classes sharing an id must.
 */
#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGColor.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGFont.h>
/* THE INTERNAL HEADER, FOR THE ONE DOOR THIS LIBRARY OWES CORE TEXT RATHER THAN ITS OWN CLIENTS: the
 * Unicode glyph step, whose assertions live here because the face does. */
#include <CoreGraphics/CGFont_internal.h>
#include <CoreGraphics/CGFunction.h>
#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGImageDestination.h>
#include <CoreGraphics/CGDataProvider_internal.h>
#include <CoreGraphics/CGLayer.h>
#include <CoreGraphics/CGPath.h>
#include <CoreGraphics/CGPattern.h>
#include <CoreGraphics/CGShading.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	g_checks++;
	if (ok) {
		return;
	}
	g_fail++;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* the colour spaces, by name                                                */
/* ------------------------------------------------------------------------ */

/* EVERY NAME THIS LIBRARY ANSWERS AND ONE IT DOES NOT. The seven were owed in four different ways until
 * recently — three built from values this library chose and records as a deviation, one from profile
 * bytes embedded in the library — so what is held here is that they REACH A PROFILE AT ALL, that the
 * component count follows the model, and that the name a space was made with comes back out of it.
 *
 * AND THE REFUSAL IS A CASE, NOT AN ABSENCE: a name this library cannot answer must return NULL and say
 * so, never quietly become a default. Drawing in the wrong colour space is a wrong answer, and a silent
 * one. */
static int case_named_spaces(void)
{
	int before = g_fail;

	/* AUTOMATIC, NOT static: the constants are `extern const CFStringRef` VARIABLES, whose values are not
	 * constant expressions, so a static aggregate cannot be initialised from them -- which is what the
	 * first version of this case got wrong. */
	struct { const char *name; CFStringRef constant; size_t components; } table[] = {
		{ "kCGColorSpaceSRGB",                kCGColorSpaceSRGB, 3 },
		{ "kCGColorSpaceAdobeRGB1998",        kCGColorSpaceAdobeRGB1998, 3 },
		{ "kCGColorSpaceGenericGrayGamma2_2", kCGColorSpaceGenericGrayGamma2_2, 1 },
		{ "kCGColorSpaceGenericGray",         kCGColorSpaceGenericGray, 1 },
		{ "kCGColorSpaceGenericRGB",          kCGColorSpaceGenericRGB, 3 },
		{ "kCGColorSpaceGenericRGBLinear",    kCGColorSpaceGenericRGBLinear, 3 },
		{ "kCGColorSpaceGenericCMYK",         kCGColorSpaceGenericCMYK, 4 },
	};
	size_t i;

	for (i = 0; i < sizeof table / sizeof table[0]; i++) {
		CGColorSpaceRef space = CGColorSpaceCreateWithName(table[i].constant);
		CFStringRef back;

		check(space != NULL, "%s was refused; it answers a profile", table[i].name);
		if (space == NULL) {
			continue;
		}
		check(CGColorSpaceGetNumberOfComponents(space) == table[i].components,
		      "%s answers %zu components, want %zu", table[i].name,
		      CGColorSpaceGetNumberOfComponents(space), table[i].components);
		back = CGColorSpaceCopyName(space);
		check(back != NULL && CFEqual(back, table[i].constant),
		      "%s does not round-trip through CGColorSpaceCopyName", table[i].name);
	}

	check(CGColorSpaceCreateWithName(CFSTR("kCGColorSpaceNoSuchName")) == NULL,
	      "a name this library does not answer was ACCEPTED rather than refused");

	/* THE DEVICE SPACES ARE SINGLETONS AND THE NAMED ONES ARE ONE OBJECT PER NAME, which is a contract
	 * CGColorSpace.h states out loud; a caller may hold one, compare it with the next call's answer, and
	 * release it. */
	check(CGColorSpaceCreateDeviceRGB() == CGColorSpaceCreateDeviceRGB(),
	      "the device RGB space is not one object");
	check(CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB) ==
	      CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB),
	      "a named space is not one object per name");
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the type-id table                                                        */
/* ------------------------------------------------------------------------ */

/* CGTypeID.c MAKES TWO PROMISES AND THIS IS BOTH OF THEM: the same class always answers the same id, and
 * two classes never share one. THE IDS THEMSELVES ARE NOT ASSERTED — measured they are 2..13 for the
 * types this library mints, with CGColor at 256 because it is a registered CORE FOUNDATION type rather
 * than one minted here — because those numbers are the table's business and reshuffling it must not fail
 * this suite. Uniqueness and stability must. */
static int case_type_ids(void)
{
	int before = g_fail;

	struct { const char *name; CGTypeID id; } t[13];
	int i, j, unique = 1, stable = 1, nonzero = 1;

	t[0].name = "CGColor";         t[0].id = CGColorGetTypeID();
	t[1].name = "CGColorSpace";    t[1].id = CGColorSpaceGetTypeID();
	t[2].name = "CGContext";       t[2].id = CGContextGetTypeID();
	t[3].name = "CGDataConsumer";  t[3].id = CGDataConsumerGetTypeID();
	t[4].name = "CGDataProvider";  t[4].id = CGDataProviderGetTypeID();
	t[5].name = "CGFont";          t[5].id = CGFontGetTypeID();
	t[6].name = "CGFunction";      t[6].id = CGFunctionGetTypeID();
	t[7].name = "CGGradient";      t[7].id = CGGradientGetTypeID();
	t[8].name = "CGImage";         t[8].id = CGImageGetTypeID();
	t[9].name = "CGLayer";         t[9].id = CGLayerGetTypeID();
	t[10].name = "CGPath";         t[10].id = CGPathGetTypeID();
	t[11].name = "CGPattern";      t[11].id = CGPatternGetTypeID();
	t[12].name = "CGShading";      t[12].id = CGShadingGetTypeID();

	for (i = 0; i < 13; i++) {
		if (t[i].id == 0) {
			nonzero = 0;
			fprintf(stderr, "FAIL: %s answers type id 0\n", t[i].name);
		}
		for (j = i + 1; j < 13; j++) {
			if (t[i].id == t[j].id) {
				unique = 0;
				fprintf(stderr, "FAIL: %s and %s share type id %lu\n",
					t[i].name, t[j].name, (unsigned long)t[i].id);
			}
		}
	}
	check(nonzero, "a type id of 0 identifies nothing");
	check(unique, "two classes share a type id; the table's second promise is broken");

	stable = (CGColorGetTypeID() == t[0].id) && (CGFontGetTypeID() == t[5].id) &&
		 (CGLayerGetTypeID() == t[9].id) && (CGColorSpaceGetTypeID() == t[1].id);
	check(stable, "a class answered a different type id on a second call");
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the data provider and consumer                                           */
/* ------------------------------------------------------------------------ */

/* A PROVIDER MADE FROM A CFDataRef HANDS THE SAME BYTES BACK. That is Apple's `CreateWithCFData` and its
 * `CopyData` pair, and the note in CGDataProvider.h claimed for a while that the accessor was absent — it
 * is not, and this case is what would have caught the note, since a header cannot. */
static int case_data_provider(void)
{
	int before = g_fail;

	static const char *text = "the bytes a provider must give back\n";
	CFDataRef source = CFDataCreate(NULL, (const UInt8 *)text, (CFIndex)strlen(text));
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(source);
	CFDataRef back;

	check(provider != NULL, "CGDataProviderCreateWithCFData refused a CFDataRef");
	if (provider == NULL) {
		return g_fail - before;
	}
	back = CGDataProviderCopyData(provider);
	check(back != NULL, "CGDataProviderCopyData answered nothing");
	check(back != NULL && CFEqual(source, back), "the provider gave back different bytes");
	check(back != NULL && CFDataGetLength(back) == (CFIndex)strlen(text),
	      "the provider gave back %ld bytes, want %zu", back ? (long)CFDataGetLength(back) : -1L,
	      strlen(text));
	return g_fail - before;
}

/* A CONSUMER OVER A CFMutableDataRef TAKES BYTES AND THE SINK ENDS UP HOLDING THEM. Both refusals the
 * door makes are cases as well: a gradient needs at least two stops to interpolate between, and a stop
 * that is not a colour would draw something, and something wrong. */
static int case_consumer_and_gradient(void)
{
	int before = g_fail;

	CFMutableDataRef sink = CFDataCreateMutable(NULL, 0);
	CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(sink);
	const char *payload = "written through the consumer";
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGColorRef first = CGColorCreateGenericRGB(1, 0, 0, 1);
	CGColorRef second = CGColorCreateGenericRGB(0, 0, 1, 1);
	const void *values[2];
	CFArrayRef ramp;
	CGGradientRef gradient;

	check(consumer != NULL, "CGDataConsumerCreateWithCFData refused a CFMutableDataRef");
	if (consumer != NULL) {
		check(CGDataConsumerPutBytes(consumer, payload, strlen(payload)) == strlen(payload),
		      "the consumer took a different number of bytes than it was given");
		check(CFDataGetLength(sink) == (CFIndex)strlen(payload) &&
		      memcmp(CFDataGetBytePtr(sink), payload, strlen(payload)) == 0,
		      "the bytes did not arrive in the sink");
	}

	check(rgb != NULL && first != NULL && second != NULL, "a colour or the device RGB space was refused");
	if (rgb == NULL || first == NULL || second == NULL) {
		return g_fail - before;
	}
	values[0] = first;
	values[1] = second;
	ramp = CFArrayCreate(NULL, values, 2, &kCFTypeArrayCallBacks);
	gradient = CGGradientCreateWithColors(rgb, ramp, NULL);
	check(gradient != NULL,
	      "a gradient was refused an array of CGColorRef -- the elements ARE the colours");

	/* ONE STOP IS REFUSED: there is nothing to interpolate between. */
	{
		const void *lonely[1];
		CFArrayRef one;

		lonely[0] = first;
		one = CFArrayCreate(NULL, lonely, 1, &kCFTypeArrayCallBacks);
		check(CGGradientCreateWithColors(rgb, one, NULL) == NULL, "a one-stop gradient was ACCEPTED");
	}

	/* AND A NULL ELEMENT IS REFUSED RATHER THAN CAST, which is the difference between a wrong gradient
	 * and no gradient. */
	{
		const void *hole[2];
		CFArrayRef broken;

		/* NO CALLBACKS HERE, DELIBERATELY, and this was measured the hard way: kCFTypeArrayCallBacks RETAINS
		 * every value, and Core Foundation HALTS -- SIGILL -- on a NULL one. An array that can hold a NULL
		 * element is made with NULL callbacks, and THAT is the array this door has to refuse. */
		hole[0] = first;
		hole[1] = NULL;
		broken = CFArrayCreate(NULL, hole, 2, NULL);
		check(CGGradientCreateWithColors(rgb, broken, NULL) == NULL,
		      "a gradient with a NULL stop was ACCEPTED");
	}
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the runner                                                               */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* the drawing core: a context that actually paints, a path, and a font      */
/* ------------------------------------------------------------------------ */

/* A FILL THAT REACHES THE PIXELS, WHICH IS THE ONE THING NO HEADER CAN PROMISE. The buffer is the
 * context's, the fill is opaque red, and the check reads the BYTES back. THE PIXEL ORDER IS THE
 * IMPLEMENTATION'S OWN AND ITS HEADERS STATE IT: CGBitmapContext.h and CGImage.h both say 8 bits per
 * component with kCGImageAlphaPremultipliedFirst and kCGBitmapByteOrder32Little, which puts a pixel in
 * memory as B, G, R, A. The assertion is written against that and not against a convenient one.
 *
 * AND THE TWO DOORS THAT REFUSE LIVE HERE TOO, because they need a context: neither has a getter, so what
 * can be held is that they are CALLABLE and that a text-drawing mode this library does not honour is
 * refused rather than acted on. What that catches is the likeliest regression -- a door that starts
 * crashing or halting instead of declining. */
static int case_bitmap_context_fill(void)
{
	int before = g_fail;
	const size_t width = 4, height = 3, bpr = width * 4;
	unsigned char *buffer = (unsigned char *)calloc(height, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	CGImageRef image;
	size_t i;
	int all_red = 1, mode;

	check(buffer != NULL && rgb != NULL, "the test could not allocate its buffer or space");
	if (buffer == NULL || rgb == NULL) {
		return g_fail - before;
	}
	ctx = CGBitmapContextCreate(buffer, width, height, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "CGBitmapContextCreate refused an 8-bit RGBA buffer");
	if (ctx == NULL) {
		return g_fail - before;
	}
	check(CGBitmapContextGetData(ctx) == buffer, "the context does not answer the caller's own buffer");
	check(CGBitmapContextGetBytesPerRow(ctx) == bpr, "the context answers %zu bytes per row, want %zu",
	      CGBitmapContextGetBytesPerRow(ctx), bpr);

	check(buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 0,
	      "the caller's buffer was not handed over as zeros");

	CGContextSetRGBFillColor(ctx, 1, 0, 0, 1);
	CGContextFillRect(ctx, CGRectMake(0, 0, (CGFloat)width, (CGFloat)height));
	for (i = 0; i < width * height; i++) {
		const unsigned char *px = buffer + i * 4;

		if (!(px[2] > 200 && px[1] < 60 && px[0] < 60 && px[3] > 200)) {
			all_red = 0;
			fprintf(stderr, "FAIL: pixel %zu is B=%u G=%u R=%u A=%u\n", i, px[0], px[1], px[2], px[3]);
			break;
		}
	}
	check(all_red, "an opaque red fill did not reach every pixel (B,G,R,A order)");

	image = CGBitmapContextCreateImage(ctx);
	check(image != NULL, "CGBitmapContextCreateImage answered nothing");
	check(image != NULL && CGImageGetWidth(image) == width && CGImageGetHeight(image) == height,
	      "the image is %zux%zu, want %zux%zu", image ? CGImageGetWidth(image) : 0,
	      image ? CGImageGetHeight(image) : 0, width, height);
	check(image != NULL && CGImageGetBitsPerPixel(image) == 32, "the image answers %zu bits per pixel",
	      image ? CGImageGetBitsPerPixel(image) : 0);

	/* THE REFUSALS, INTO WHICH EVERY VALUE IS FED RATHER THAN THE ONE A CALLER WOULD CHOOSE. */
	CGContextSetAllowsFontSubpixelQuantization(ctx, true);
	CGContextSetAllowsFontSubpixelQuantization(ctx, false);
	for (mode = 0; mode <= 7; mode++) {
		CGContextSetTextDrawingMode(ctx, (CGTextDrawingMode)mode);
	}
	return g_fail - before;
}

/* A PATH'S THREE QUESTIONS: is it empty, does it contain a point, and what is its box. The triangle is
 * (0,0), (4,0), (4,4) closed, so the box is 4x4 and a point at (3,1) is inside it while (-1,-1) is not --
 * and a rectangle added at (10,10) makes the box 12 wide, which is what a box is a UNION of. */
static int case_path(void)
{
	int before = g_fail;
	CGMutablePathRef path = CGPathCreateMutable();
	CGRect box;
	CGPoint inside = { 3.0, 1.0 };
	CGPoint outside = { -1.0, -1.0 };

	check(path != NULL, "CGPathCreateMutable answered nothing");
	if (path == NULL) {
		return g_fail - before;
	}
	check(CGPathIsEmpty(path), "a path just created says it is not empty");
	CGPathMoveToPoint(path, NULL, 0, 0);
	CGPathAddLineToPoint(path, NULL, 4, 0);
	CGPathAddLineToPoint(path, NULL, 4, 4);
	CGPathCloseSubpath(path);
	check(!CGPathIsEmpty(path), "a path with three segments says it is empty");
	check(CGPathContainsPoint(path, NULL, inside, false), "a point inside the triangle is reported outside");
	check(!CGPathContainsPoint(path, NULL, outside, false), "a point outside is reported inside");

	box = CGPathGetBoundingBox(path);
	check(box.origin.x == 0 && box.origin.y == 0 && box.size.width == 4 && box.size.height == 4,
	      "the triangle's box is (%g,%g) %gx%g, want (0,0) 4x4", (double)box.origin.x,
	      (double)box.origin.y, (double)box.size.width, (double)box.size.height);

	CGPathAddRect(path, NULL, CGRectMake(10, 10, 2, 2));
	box = CGPathGetBoundingBox(path);
	check(box.size.width == 12 && box.size.height == 12,
	      "after adding a rect at (10,10) the box is %gx%g, want 12x12", (double)box.size.width,
	      (double)box.size.height);
	return g_fail - before;
}


/* **A QUADRATIC'S BOXES AND ITS FILL, WHICH IS THE CASE `cg_quad_as_cubic` OWED.** The conversion that raises a
 * quad to a cubic read the START POINT from the element's fifth and sixth doubles — past the four it has — so the
 * control points came out of whatever memory followed. MEASURED before the fix: this arc's tight box was
 * `(0,0 53x21)`, it should be `(10,10 80x40)`, and FILLING IT PAINTED NOTHING. TrueType glyph outlines reach this
 * code through `CGContextAddQuadCurveToPoint`, so every such glyph was wrong; the case is written against the
 * generic curve door so it does not depend on a font.
 *
 * BOTH BOXES ARE ASKED FOR because they answer different questions: `CGPathGetBoundingBox` INCLUDES the control
 * point (height 80), while `CGPathGetPathBoundingBox` is the tight curve (height 40). A fix that clamped the
 * control into the element would pass the first and fail the second. */
static int case_quad_curve(void)
{
	int before = g_fail;
	const size_t width = 100, height = 100, bpr = width * 4;
	unsigned char *buffer = (unsigned char *)calloc(height, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	CGFloat cpx = 50, cpy = 90, x = 90, y = 10;
	CGRect loose;
	CGRect tight;
	CGPoint inside = { 50.0, 30.0 };
	CGPoint outside = { 50.0, 70.0 };

	check(buffer != NULL && rgb != NULL, "the quad case could not allocate");
	if (buffer == NULL || rgb == NULL) {
		free(buffer);
		CGColorSpaceRelease(rgb);
		return g_fail - before;
	}
	ctx = CGBitmapContextCreate(buffer, width, height, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	if (ctx == NULL) {
		check(0, "the quad case's context was refused");
		free(buffer);
		CGColorSpaceRelease(rgb);
		return g_fail - before;
	}
	CGContextBeginPath(ctx);
	CGContextMoveToPoint(ctx, 10, 10);
	CGContextAddQuadCurveToPoint(ctx, cpx, cpy, x, y);
	tight = CGContextGetPathBoundingBox(ctx);
	check(tight.origin.x == 10 && tight.origin.y == 10 && tight.size.width == 80 &&
	      tight.size.height == 40,
	      "the quad's tight box is (%g,%g) %gx%g, want (10,10) 80x40", (double)tight.origin.x,
	      (double)tight.origin.y, (double)tight.size.width, (double)tight.size.height);
	{
		CGPathRef copy = CGContextCopyPath(ctx);

		loose = copy != NULL ? CGPathGetBoundingBox(copy) : CGRectNull;
		if (copy != NULL) {
			CGPathRelease(copy);
		}
	}
	check(loose.origin.x == 10 && loose.origin.y == 10 && loose.size.width == 80 &&
	      loose.size.height == 80,
	      "the quad's control-inclusive box is (%g,%g) %gx%g, want (10,10) 80x80",
	      (double)loose.origin.x, (double)loose.origin.y, (double)loose.size.width,
	      (double)loose.size.height);

	CGContextSetRGBFillColor(ctx, 1, 0, 0, 1);
	CGContextFillPath(ctx);
	{
		/* the point (50,30) is under the arch; the point (50,70) is above it */
		const unsigned char *in = buffer + ((size_t)(height - 1 - (size_t)inside.y) * width +
						    (size_t)inside.x) * 4;
		const unsigned char *out = buffer + ((size_t)(height - 1 - (size_t)outside.y) * width +
						     (size_t)outside.x) * 4;

		check(in[3] > 200 && in[2] > 200 && in[0] < 60 && in[1] < 60,
		      "the quad did not fill under its arch (B=%u G=%u R=%u A=%u)", in[0], in[1], in[2], in[3]);
		check(out[3] < 20, "the quad filled above its arch (A=%u)", out[3]);
	}

	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
	free(buffer);
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the sweep, and the shapes a band has to be read at both ends of           */
/* ------------------------------------------------------------------------ */

/* A TRIANGLE FILLS AS A TRIANGLE, AND THIS CASE IS THE ONE THE SWEEP OWED.
 *
 * **THE SWEEP TOOK EACH TRAPEZOID'S TOP X OUT OF AN ARRAY THAT HOLDS THE BAND'S MIDDLE.** The x's it sorts by are
 * read at the middle of a band ON PURPOSE -- a tie at a band's top is unresolvable and the middle is not -- and
 * the trapezoid's own corners were then read back out of that same array, so EVERY shape's top edge came out as
 * wide as its middle. A triangle is the shape that shows it: MEASURED before the fix, the one below painted 8
 * points across its top row and 16 across its base, with the apex MISSING, and the mirrored triangle was wrong the
 * other way -- 8 across and no base at all. A RECTANGLE IS BLIND TO IT, because its edges are vertical and its x
 * is the same at every y, which is why every case that had been written down passed while triangles were broken.
 *
 * THE TWO CLAIMS ARE THE MIRROR AND THE TAPER, and neither is a pixel count for its own sake. The mirror is what
 * the defect broke first -- one shape read its top from the middle and the other read its bottom the same way, so
 * the two were wrong in opposite directions -- and the taper is what says the shape is a TRIANGLE rather than a
 * trapezium whose top edge happens to be short.
 */
#define TRI_SIDE 24
static unsigned char g_tri_buf[TRI_SIDE * TRI_SIDE * 4];

/* Fills one triangle on a white ground and answers how many points it painted, with the width of the widest row
 * and of the row at the FAR END from it. The buffer's rows count from the top and the vertices are given in user
 * space, whose origin is the bottom left, so the row index is converted -- the same flip `pixel_lit` records. */
static size_t tri_fill(double x0, double y0, double x1, double y1, double x2, double y2,
		       int *base_w, int *tip_w)
{
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef c;
	CGMutablePathRef p;
	size_t i, painted = 0;
	int y, widest = 0, widest_y = -1, first_y = -1, last_y = -1;

	memset(g_tri_buf, 0xff, sizeof g_tri_buf);
	c = CGBitmapContextCreate(g_tri_buf, TRI_SIDE, TRI_SIDE, 8, TRI_SIDE * 4, rgb,
				  kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(rgb);
	CGContextSetRGBFillColor(c, 0, 0, 0, 1);
	p = CGPathCreateMutable();
	CGPathMoveToPoint(p, NULL, x0, y0);
	CGPathAddLineToPoint(p, NULL, x1, y1);
	CGPathAddLineToPoint(p, NULL, x2, y2);
	CGPathCloseSubpath(p);
	CGContextAddPath(c, p);
	CGContextFillPath(c);

	for (i = 0; i < TRI_SIDE * TRI_SIDE; i++) {
		if (g_tri_buf[i * 4 + 2] < 128) {	/* B,G,R,A: R is byte 2 */
			painted++;
		}
	}
	for (y = 0; y < TRI_SIDE; y++) {
		size_t row = TRI_SIDE - 1 - y;
		int x, w = 0;

		for (x = 0; x < TRI_SIDE; x++) {
			if (g_tri_buf[(row * TRI_SIDE + x) * 4 + 2] < 128) {
				w++;
			}
		}
		if (w > 0) {
			if (first_y < 0) {
				first_y = y;
			}
			last_y = y;
			if (w > widest) {
				widest = w;
				widest_y = y;
			}
		}
	}
	if (base_w != NULL) {
		*base_w = widest;
	}
	if (tip_w != NULL) {
		/* the end AWAY from the widest row is the tip, whichever way round it is */
		int tip_y = (widest_y - first_y) > (last_y - widest_y) ? first_y : last_y;
		size_t row = TRI_SIDE - 1 - tip_y;
		int x, w = 0;

		for (x = 0; x < TRI_SIDE; x++) {
			if (g_tri_buf[(row * TRI_SIDE + x) * 4 + 2] < 128) {
				w++;
			}
		}
		*tip_w = w;
	}
	CGPathRelease(p);
	CGContextRelease(c);
	return painted;
}

static int case_triangle_fill(void)
{
	int before = g_fail;
	int up_base = 0, up_tip = 0, down_base = 0, down_tip = 0;
	size_t up, down;

	/* ONE TRIANGLE AND ITS MIRROR, reflected about y = 11.5: 16 points across the base and 16 tall. */
	up = tri_fill(12, 20, 4, 4, 20, 4, &up_base, &up_tip);
	down = tri_fill(12, 4, 4, 20, 20, 20, &down_base, &down_tip);

	check(up_base == 16 && down_base == 16,
	      "a triangle 16 points across its base fills %d and %d points across its widest row, want 16",
	      up_base, down_base);
	check(up_tip <= 4 && down_tip <= 4,
	      "and the end away from the base is a TIP, %d and %d points, not an edge as wide as the middle",
	      up_tip, down_tip);
	check(up == down,
	      "the triangle and its mirror paint the same pixels: %zu and %zu -- the defect made them differ",
	      up, down);
	return g_fail - before;
}

/* A FONT FROM BYTES, WHICH IS THE FORM THAT OPENS NO FILE BY NAME: a provider over a real file on this
 * machine, then the numbers a font must have. THE FILE IS CHECKED FOR FIRST AND ITS ABSENCE IS REPORTED
 * RATHER THAN COUNTED AS A PASS -- a case that quietly skips is a case that stops covering anything. */
static int case_font_from_provider(void)
{
	int before = g_fail;
	const char *path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
	CGDataProviderRef provider;
	CGFontRef font;
	CFStringRef name;

	if (access(path, R_OK) != 0) {
		printf("  (not run: %s is not on this machine, so CGFont's data path was NOT exercised)\n",
		       path);
		return g_fail - before;
	}
	provider = CGDataProviderCreateWithFilename(path);
	check(provider != NULL, "a provider could not be made from %s", path);
	if (provider == NULL) {
		return g_fail - before;
	}
	font = CGFontCreateWithDataProvider(provider);
	check(font != NULL, "CGFontCreateWithDataProvider refused a real TrueType face");
	if (font == NULL) {
		return g_fail - before;
	}
	check(CGFontGetNumberOfGlyphs(font) > 0, "the font answers 0 glyphs");
	check(CGFontGetUnitsPerEm(font) > 0, "the font answers %d units per em",
	      CGFontGetUnitsPerEm(font));
	name = CGFontCopyPostScriptName(font);
	check(name != NULL && CFStringGetLength(name) > 0, "the font has no PostScript name");
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the pixel paths: a gradient and a shading painted into a context, and     */
/* images decoded from real files                                            */
/* ------------------------------------------------------------------------ */

/* A GRADIENT DRAWN INTO A CONTEXT, WHICH IS THE FIRST THING HERE THAT PAINTS WITHOUT A RECT. The check is
 * on the TWO ENDS AND ON THEIR DIFFERENCE: a gradient that painted one colour everywhere would satisfy an
 * end-only check on one side, and "the ends differ" is what makes this a gradient rather than a fill. The
 * pixel order is the implementation's documented B, G, R, A. */
static int case_gradient_into_context(void)
{
	int before = g_fail;
	const size_t width = 16, height = 1, bpr = width * 4;
	unsigned char *buffer = (unsigned char *)calloc(height, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	CGColorRef red = CGColorCreateGenericRGB(1, 0, 0, 1);
	CGColorRef blue = CGColorCreateGenericRGB(0, 0, 1, 1);
	const void *values[2];
	CFArrayRef ramp;
	CGGradientRef gradient;
	const unsigned char *last;

	check(buffer != NULL && rgb != NULL && red != NULL && blue != NULL, "the test could not set up");
	if (buffer == NULL || rgb == NULL || red == NULL || blue == NULL) {
		return g_fail - before;
	}
	ctx = CGBitmapContextCreate(buffer, width, height, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "the context for the gradient was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	values[0] = red;
	values[1] = blue;
	ramp = CFArrayCreate(NULL, values, 2, &kCFTypeArrayCallBacks);
	gradient = CGGradientCreateWithColors(rgb, ramp, NULL);
	check(gradient != NULL, "the gradient for the draw was refused");
	if (gradient == NULL) {
		return g_fail - before;
	}
	CGContextDrawLinearGradient(ctx, gradient, CGPointMake(0, 0), CGPointMake((CGFloat)width, 0), 0);

	last = buffer + (width - 1) * 4;
	check(buffer[2] > 150 && buffer[0] < 100,
	      "the start of a red-to-blue gradient is not red-dominant (B=%u G=%u R=%u A=%u)",
	      buffer[0], buffer[1], buffer[2], buffer[3]);
	check(last[0] > 150 && last[2] < 100,
	      "the end of a red-to-blue gradient is not blue-dominant (B=%u G=%u R=%u A=%u)",
	      last[0], last[1], last[2], last[3]);
	check(memcmp(buffer, last, 4) != 0, "both ends of the gradient are the same pixel");
	return g_fail - before;
}

/* A SHADING, WHICH PAINTS THROUGH A CALLER'S FUNCTION RATHER THAN THROUGH STOPS: the function is asked
 * for a colour at each parameter along the axis, and here it returns red ramping up as blue ramps down --
 * the same picture as the gradient above, arriving by a different door, which is why the same two-end
 * check applies.
 *
 * THE FUNCTION PRODUCES THE SPACE'S COMPONENTS AND NO MORE, WHICH IS THE CONTRACT AND WAS MEASURED THE HARD
 * WAY. This case first asked for FOUR components -- RGBA -- against a THREE-component RGB space, and
 * CGShadingCreateAxial refused it, saying so precisely:
 *
 *   CG-REFUSE: the shading's colour space has 3 components and the function produces 4
 *
 * An alpha is not the function's business: the context's alpha state supplies it. The library was right
 * and the case was wrong, which is the second time in this suite that a failing case turned out to be a
 * mis-specified test rather than a defect. */
static void shading_evaluate(void *info, const CGFloat *in, CGFloat *out)
{
	(void)info;
	out[0] = in[0];
	out[1] = 0.0;
	out[2] = 1.0 - in[0];
}

static int case_axial_shading(void)
{
	int before = g_fail;
	const size_t width = 16, height = 1, bpr = width * 4;
	unsigned char *buffer = (unsigned char *)calloc(height, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	const CGFloat domain[2] = { 0.0, 1.0 };
	const CGFloat range[6] = { 0, 1, 0, 1, 0, 1 };	/* three components: the space's */
	CGFunctionCallbacks callbacks = { 0, shading_evaluate, NULL };
	CGFunctionRef function;
	CGShadingRef shading;
	const unsigned char *last;

	check(buffer != NULL && rgb != NULL, "the test could not set up");
	if (buffer == NULL || rgb == NULL) {
		return g_fail - before;
	}
	ctx = CGBitmapContextCreate(buffer, width, height, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "the context for the shading was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	function = CGFunctionCreate(NULL, 1, domain, 3, range, &callbacks);
	check(function != NULL, "CGFunctionCreate refused a one-in four-out function");
	if (function == NULL) {
		return g_fail - before;
	}
	shading = CGShadingCreateAxial(rgb, CGPointMake(0, 0), CGPointMake((CGFloat)width, 0), function,
				       true, true);
	check(shading != NULL, "CGShadingCreateAxial refused an axial shading");
	if (shading == NULL) {
		return g_fail - before;
	}
	CGContextDrawShading(ctx, shading);

	/* THE PARAMETER RUNS FROM 0 AT THE START TO 1 AT THE END, and the callback puts RED at in[0], so the
	 * axis is BLUE AT ITS START AND RED AT ITS END -- measured, and the first version of this case
	 * asserted the opposite because it was written from the name "red-to-blue" rather than from the
	 * callback beside it. The library was right for the third time in this suite; the case was wrong. */
	last = buffer + (width - 1) * 4;
	check(buffer[0] > 150 && buffer[2] < 100,
	      "the shading's START is not the callback's in=0 end, which is blue (B=%u R=%u)",
	      buffer[0], buffer[2]);
	check(last[2] > 150 && last[0] < 100,
	      "the shading's END is not the callback's in=1 end, which is red (B=%u R=%u)",
	      last[0], last[2]);
	check(memcmp(buffer, last, 4) != 0, "both ends of the shading are the same pixel");
	return g_fail - before;
}

/* IMAGES DECODED FROM REAL FILES AND THEN DRAWN, WHICH IS THE ONE PATH THAT READS A FILE FORMAT. The PNG
 * and JPEG files are chosen because they were MEASURED to decode here rather than because they are the
 * obvious ones: /usr/share/pixmaps/debian-logo.png is 48x48, python3's file.png is 16x16,
 * sane-logo.jpg is 128x150 and sane-umax-advanced.jpg is 291x235, all answering 32 bits per pixel.
 *
 * WHAT IS ASSERTED IS THE DECODE AND THAT DRAWING IT CHANGES PIXELS -- NOT WHAT THE PICTURES CONTAIN,
 * because the file is not this library's and its contents are not this suite's business. Drawing into a
 * 64x64 rect SCALES the image to that rect, which is why the whole buffer is covered rather than 16x16 of
 * it. A file that is not on the machine is REPORTED, per file, rather than quietly skipped. */
static int case_images_from_data(void)
{
	int before = g_fail;
	static const struct { const char *path; int is_png; } files[] = {
		{ "/usr/share/pixmaps/debian-logo.png", 1 },
		{ "/usr/share/doc/python3/_static/file.png", 1 },
		{ "/usr/share/doc/libsane-common/umax/sane-logo.jpg", 0 },
		{ "/usr/share/doc/libsane-common/umax/sane-umax-advanced.jpg", 0 },
	};
	size_t i;

	for (i = 0; i < sizeof files / sizeof files[0]; i++) {
		CGDataProviderRef provider;
		CGImageRef image;
		unsigned char *buffer;
		CGContextRef ctx;
		size_t j, nonzero = 0;

		if (access(files[i].path, R_OK) != 0) {
			printf("  (not run: %s is not on this machine)\n", files[i].path);
			continue;
		}
		provider = CGDataProviderCreateWithFilename(files[i].path);
		check(provider != NULL, "a provider could not be made from %s", files[i].path);
		if (provider == NULL) {
			continue;
		}
		image = files[i].is_png
			? CGImageCreateWithPNGDataProvider(provider, NULL, true, kCGRenderingIntentDefault)
			: CGImageCreateWithJPEGDataProvider(provider, NULL, true, kCGRenderingIntentDefault);
		check(image != NULL, "%s could not be decoded", files[i].path);
		if (image == NULL) {
			continue;
		}
		check(CGImageGetWidth(image) > 0 && CGImageGetHeight(image) > 0,
		      "%s decoded to a %zux%zu image", files[i].path, CGImageGetWidth(image),
		      CGImageGetHeight(image));
		check(CGImageGetBitsPerPixel(image) == 32, "%s decoded to %zu bits per pixel",
		      files[i].path, CGImageGetBitsPerPixel(image));

		buffer = (unsigned char *)calloc(64, 64 * 4);
		check(buffer != NULL, "the test could not allocate the draw buffer");
		if (buffer == NULL) {
			continue;
		}
		ctx = CGBitmapContextCreate(buffer, 64, 64, 8, 64 * 4, CGColorSpaceCreateDeviceRGB(),
					    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
		check(ctx != NULL, "the context for drawing %s was refused", files[i].path);
		if (ctx != NULL) {
			CGContextDrawImage(ctx, CGRectMake(0, 0, 64, 64), image);
			for (j = 0; j < 64 * 64 * 4; j++) {
				if (buffer[j] != 0) {
					nonzero++;
				}
			}
			check(nonzero > 0, "drawing %s changed no pixels at all", files[i].path);
		}
		free(buffer);
	}
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the radial shading, and the degenerate cases the header PROMISES about   */
/* ------------------------------------------------------------------------ */

/* A RADIAL RAMP IS CHECKED NEAR ITS CENTRE AGAINST A CORNER, which is what makes the check independent of
 * which way the rows run in memory: whatever the y flip, the pixel at (7,7) is near the centre and the
 * pixel at offset 0 is a corner. MEASURED, with the callback that puts red at in[0]:
 *
 *   r0=0, r1=8 (a point out to a disc)   near-centre B=232 R=23   corner B=0 R=255
 *   r0=8, r1=0 (the same circles swapped) near-centre B=23 R=232   corner B=255 R=0
 *
 * SO THE PARAMETER RUNS FROM THE START CIRCLE TO THE END CIRCLE, and the second half of this case is what
 * says so: swapping the radii swaps the picture, which a case that only looked at one of them could not
 * tell from a shading that ignored its arguments.
 *
 * BOTH ARE COINCIDENT-CENTRE SHADINGS -- the header's "the centres may coincide (concentric)" case -- and
 * both carry a radius of zero, which is its other one. */
static int case_radial_shading(void)
{
	int before = g_fail;
	const size_t side = 16, bpr = side * 4;
	unsigned char *buffer = (unsigned char *)calloc(side, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	const CGFloat domain[2] = { 0.0, 1.0 };
	const CGFloat range[6] = { 0, 1, 0, 1, 0, 1 };
	CGFunctionCallbacks callbacks = { 0, shading_evaluate, NULL };
	CGFunctionRef function;
	CGPoint c = { 8.0, 8.0 };
	CGShadingRef out, in;
	const unsigned char *near_centre = buffer + (7 * side + 7) * 4;
	const unsigned char *corner = buffer;

	check(buffer != NULL && rgb != NULL, "the test could not set up");
	if (buffer == NULL || rgb == NULL) {
		return g_fail - before;
	}
	ctx = CGBitmapContextCreate(buffer, side, side, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "the context for the radial shading was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	function = CGFunctionCreate(NULL, 1, domain, 3, range, &callbacks);
	check(function != NULL, "the radial shading's function was refused");
	if (function == NULL) {
		return g_fail - before;
	}

	out = CGShadingCreateRadial(rgb, c, 0.0, c, 8.0, function, true, true);
	check(out != NULL, "a shading from a point out to a disc was refused");
	if (out != NULL) {
		CGContextDrawShading(ctx, out);
		check(near_centre[0] > 150 && near_centre[2] < 100,
		      "the centre of a 0-to-8 ramp is not the callback's in=0 end (B=%u R=%u)",
		      near_centre[0], near_centre[2]);
		check(corner[2] > 150 && corner[0] < 100,
		      "the corner of a 0-to-8 ramp is not the callback's in=1 end (B=%u R=%u)",
		      corner[0], corner[2]);
		check(memcmp(near_centre, corner, 4) != 0, "the ramp painted one colour everywhere");
	}

	in = CGShadingCreateRadial(rgb, c, 8.0, c, 0.0, function, true, true);
	check(in != NULL, "a shading with the two circles swapped was refused");
	if (in != NULL) {
		memset(buffer, 0, side * bpr);
		CGContextDrawShading(ctx, in);
		check(near_centre[2] > 150 && near_centre[0] < 100,
		      "swapping the radii did not swap the ramp: the centre is still blue-dominant "
		      "(B=%u R=%u)", near_centre[0], near_centre[2]);
		check(corner[0] > 150 && corner[2] < 100,
		      "swapping the radii did not swap the ramp: the corner is still red-dominant "
		      "(B=%u R=%u)", corner[0], corner[2]);
	}
	return g_fail - before;
}

/* THE DEGENERATE CASES ARE PROMISED RATHER THAN DESCRIBED: "either may have a radius of zero (a point) and
 * the centres may coincide (concentric) -- the degenerate cases are answered rather than refused". So they
 * are CREATED here, which is the promise, and the fully degenerate one is also DRAWN, which is the part a
 * refusal could not survive. MEASURED: a point-to-a-point shading is created and paints NOTHING -- that is
 * recorded rather than asserted, because the header promises an answer and not a particular one.
 *
 * AND THE TWO REFUSALS THE DOOR DOES MAKE ARE CASES HERE, so that they are held by something other than the
 * prose that states them: a function whose range dimension is not the space's component count, and a NULL
 * function. The first is what this suite's own shading case tripped over before it was fixed. */
static int case_radial_degenerate_and_refusals(void)
{
	int before = g_fail;
	const size_t side = 16, bpr = side * 4;
	unsigned char *buffer = (unsigned char *)calloc(side, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	const CGFloat domain[2] = { 0.0, 1.0 };
	const CGFloat range3[6] = { 0, 1, 0, 1, 0, 1 };
	const CGFloat range4[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };
	CGFunctionCallbacks callbacks = { 0, shading_evaluate, NULL };
	CGFunctionRef three, four;
	CGShadingRef point, loose;
	CGPoint c = { 8.0, 8.0 };
	size_t i, painted = 0;

	check(buffer != NULL && rgb != NULL, "the test could not set up");
	if (buffer == NULL || rgb == NULL) {
		return g_fail - before;
	}
	ctx = CGBitmapContextCreate(buffer, side, side, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "the context was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	three = CGFunctionCreate(NULL, 1, domain, 3, range3, &callbacks);
	four = CGFunctionCreate(NULL, 1, domain, 4, range4, &callbacks);
	check(three != NULL && four != NULL, "the test's functions were refused");

	/* BOTH RADII ZERO, WHICH IS THE HEADER'S "A POINT" CASE AT BOTH ENDS AT ONCE. */
	point = CGShadingCreateRadial(rgb, c, 0.0, c, 0.0, three, true, true);
	check(point != NULL, "a shading from a point to a point was REFUSED; the header says the "
			     "degenerate cases are answered");
	if (point != NULL) {
		CGContextDrawShading(ctx, point);
		for (i = 0; i < side * side * 4; i++) {
			if (buffer[i] != 0) {
				painted++;
			}
		}
		/* THIS IS A BEHAVIOUR PINNED RATHER THAN A PROMISE KEPT. The header promises that a degenerate
		 * ramp is ANSWERED, not what colour it is, and MEASURED this one paints nothing at all -- so the
		 * assertion is that it still paints nothing, and a change here is a finding to look at rather than
		 * automatically a bug. An earlier version of this line read `check(painted == 0 || 1, ...)`, which
		 * cannot fail and was therefore decoration; it is worth naming because that is the failure mode
		 * this suite exists to avoid. */
		check(painted == 0,
		      "a point-to-point shading painted %zu bytes; measured, it paints nothing", painted);
	}

	/* TWO CIRCLES THAT DO NOT NEST, WHICH IS NEITHER DEGENERATE NOR REFUSED. */
	loose = CGShadingCreateRadial(rgb, CGPointMake(4, 4), 4.0, CGPointMake(12, 12), 2.0, three, true,
				      true);
	check(loose != NULL, "a shading between two circles that do not nest was refused");
	if (loose != NULL) {
		memset(buffer, 0, side * bpr);
		painted = 0;	/* AND THE COUNT IS RESET, or the check below passes on the FIRST shading's bytes */
		CGContextDrawShading(ctx, loose);
		for (i = 0; i < side * side * 4; i++) {
			if (buffer[i] != 0) {
				painted++;
			}
		}
		check(painted > 0, "a shading between two non-nesting circles painted nothing at all");
	}

	/* AND THE TWO REFUSALS. */
	check(CGShadingCreateRadial(rgb, c, 0.0, c, 8.0, four, true, true) == NULL,
	      "a function producing 4 components was ACCEPTED for a 3-component space");
	check(CGShadingCreateRadial(rgb, c, 0.0, c, 8.0, NULL, true, true) == NULL,
	      "a shading with a NULL function was ACCEPTED");
	return g_fail - before;
}

/* THE RADIAL GRADIENT AND THE RADIAL SHADING ARE THE SAME RAMP THROUGH TWO DOORS, AND THIS IS WHAT MAKES
 * THAT A CLAIM RATHER THAN A COMMENT. CGShading.h says the degenerate answers come from "CGPaint.c's
 * `cg_paint_radial_parameter`, shared with the radial gradient", so the two must agree about WHICH PIXELS
 * the ramp covers -- their colour mappings are each their own, but the parameter and its extent are not.
 *
 * MEASURED WITH THE EXTENTS OFF, WHERE THE SHAPE IS A SHAPE AND NOT THE WHOLE BUFFER:
 *
 *   the two masks differ at 0 of 1024 pixels      (and 208 are painted -- a disc of radius 8, not 1024)
 *   shading : centre B=232 R=23  mid-ring B=142 R=113  beyond the end radius B=0 R=0
 *   gradient: centre B=232 R=23  mid-ring B=142 R=113
 *
 * THAT IS WHY THE COMPARISON IS PIXEL FOR PIXEL AND THE COUNT IS ASSERTED TO BE NEITHER ZERO NOR THE
 * WHOLE BUFFER: a "0 of 1024 differ" result is worth nothing if both doors simply painted everything,
 * which is what the first attempt at this measured with the extents left on.
 *
 * THE SAMPLED COLOURS AGREE TOO, EVERY PIXEL OF THEM, and that is asserted BELOW THE SHARING CLAIM RATHER
 * THAN AS PART OF IT: a linear ramp through two stops and a linear function happen to interpolate alike,
 * and a future difference in the gradient's interpolation rule would be a legitimate change that this line
 * would report. The mask is the promise; the colours are today's measurement. */
static int case_radial_gradient_matches_the_shading(void)
{
	int before = g_fail;
	const size_t side = 32, bpr = side * 4;
	unsigned char *via_shading_buf = (unsigned char *)calloc(side, bpr);
	unsigned char *via_gradient_buf = (unsigned char *)calloc(side, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef shading_ctx, gradient_ctx;
	const CGFloat domain[2] = { 0.0, 1.0 };
	const CGFloat range[6] = { 0, 1, 0, 1, 0, 1 };
	CGFunctionCallbacks callbacks = { 0, shading_evaluate, NULL };
	CGFunctionRef function;
	CGColorRef blue = CGColorCreateGenericRGB(0, 0, 1, 1);
	CGColorRef red = CGColorCreateGenericRGB(1, 0, 0, 1);
	const void *values[2];
	CFArrayRef stops;
	CGGradientRef gradient;
	CGPoint c = { 16.0, 16.0 };
	size_t i, mask_differ = 0, colour_differ = 0, painted = 0;

	check(via_shading_buf != NULL && via_gradient_buf != NULL && rgb != NULL && blue != NULL &&
	      red != NULL, "the test could not set up");
	if (via_shading_buf == NULL || via_gradient_buf == NULL || rgb == NULL || blue == NULL ||
	    red == NULL) {
		return g_fail - before;
	}
	shading_ctx = CGBitmapContextCreate(via_shading_buf, side, side, 8, bpr, rgb,
					    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	gradient_ctx = CGBitmapContextCreate(via_gradient_buf, side, side, 8, bpr, rgb,
					     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(shading_ctx != NULL && gradient_ctx != NULL, "a context was refused");
	if (shading_ctx == NULL || gradient_ctx == NULL) {
		return g_fail - before;
	}
	function = CGFunctionCreate(NULL, 1, domain, 3, range, &callbacks);
	values[0] = blue;
	values[1] = red;
	stops = CFArrayCreate(NULL, values, 2, &kCFTypeArrayCallBacks);
	gradient = CGGradientCreateWithColors(rgb, stops, NULL);
	check(function != NULL && gradient != NULL, "the function or the gradient was refused");
	if (function == NULL || gradient == NULL) {
		return g_fail - before;
	}

	/* THE EXTENTS OFF: the ramp covers the disc where its parameter is defined, and nothing else. */
	{
		CGShadingRef shading = CGShadingCreateRadial(rgb, c, 0.0, c, 8.0, function, false, false);

		check(shading != NULL, "the radial shading was refused");
		if (shading != NULL) {
			memset(via_shading_buf, 0, side * bpr);
			CGContextDrawShading(shading_ctx, shading);
		}
	}
	memset(via_gradient_buf, 0, side * bpr);
	CGContextDrawRadialGradient(gradient_ctx, gradient, c, 0.0, c, 8.0, 0);

	for (i = 0; i < side * side; i++) {
		const unsigned char *a = via_shading_buf + i * 4;
		const unsigned char *b = via_gradient_buf + i * 4;
		int a_painted = (a[0] | a[1] | a[2] | a[3]) != 0;
		int b_painted = (b[0] | b[1] | b[2] | b[3]) != 0;

		if (a_painted != b_painted) {
			mask_differ++;
		} else if (memcmp(a, b, 4) != 0) {
			colour_differ++;
		}
		if (b_painted) {
			painted++;
		}
	}
	check(mask_differ == 0,
	      "the shading and the radial gradient disagree about %zu pixels: the parameter is supposed to "
	      "be shared", mask_differ);
	check(painted > 0 && painted < side * side,
	      "the extents off painted %zu of %zu pixels; a ramp with extents off must cover neither "
	      "nothing nor everything", painted, side * side);
	check(colour_differ == 0,
	      "%zu painted pixels differ in colour between the two doors; the masks agree", colour_differ);

	/* AND WITH THE EXTENTS ON, BOTH COVER EVERYTHING -- which is what the two doors' separate spellings
	 * (a pair of bools against a pair of option flags) have to amount to. */
	memset(via_shading_buf, 0, side * bpr);
	memset(via_gradient_buf, 0, side * bpr);
	{
		CGShadingRef extended = CGShadingCreateRadial(rgb, c, 0.0, c, 8.0, function, true, true);

		check(extended != NULL, "the extended shading was refused");
		if (extended != NULL) {
			CGContextDrawShading(shading_ctx, extended);
		}
	}
	CGContextDrawRadialGradient(gradient_ctx, gradient, c, 0.0, c, 8.0,
				    kCGGradientDrawsBeforeStartLocation |
					    kCGGradientDrawsAfterEndLocation);
	{
		size_t shading_painted = 0, gradient_painted = 0, extended_differ = 0;

		for (i = 0; i < side * side; i++) {
			const unsigned char *a = via_shading_buf + i * 4;
			const unsigned char *b = via_gradient_buf + i * 4;

			if ((a[0] | a[1] | a[2] | a[3]) != 0) {
				shading_painted++;
			}
			if ((b[0] | b[1] | b[2] | b[3]) != 0) {
				gradient_painted++;
			}
			if (memcmp(a, b, 4) != 0) {
				extended_differ++;
			}
		}
		check(shading_painted == side * side && gradient_painted == side * side,
		      "with the extents on, the shading painted %zu and the gradient %zu of %zu pixels",
		      shading_painted, gradient_painted, side * side);
		check(extended_differ == 0, "%zu pixels differ when both extents are on", extended_differ);
	}
	return g_fail - before;
}

/* Two readers the cases below share: how many pixels of a buffer are non-zero, and whether one is. */
static size_t painted_pixels(const unsigned char *buf, size_t side)
{
	size_t i, n = 0;

	for (i = 0; i < side * side; i++) {
		if ((buf[i * 4] | buf[i * 4 + 1] | buf[i * 4 + 2] | buf[i * 4 + 3]) != 0) {
			n++;
		}
	}
	return n;
}

/* THE ROW IS CONVERTED, BECAUSE THE TWO COORDINATES IN PLAY ARE NOT THE SAME ONE. A clip or a stroke is
 * given in USER space, whose origin is the BOTTOM LEFT, and a buffer's first row is the TOP one. This
 * helper takes USER-space y and flips it. IT DID NOT, IN ITS FIRST VERSION, and the clip case failed on a
 * pixel that was inside the clip: the count was right and the spot check was reading the mirror image of
 * the pixel it asked for. The stroke case passed with the same mistake because a rectangle is symmetric --
 * which is how a wrong helper survives one case and breaks the next. */
static int pixel_lit(const unsigned char *buf, size_t side, size_t x, size_t y)
{
	size_t row = side - 1 - y;

	return (buf[(row * side + x) * 4] | buf[(row * side + x) * 4 + 3]) != 0;
}

/* ------------------------------------------------------------------------ */
/* clips, strokes, and the colour component doors                            */
/* ------------------------------------------------------------------------ */

/* A CLIP CONTAINS A LATER FILL -- AND CLIPS ACCUMULATE, WHICH IS THE PART THAT IS EASY TO GET WRONG AND
 * WAS, HERE, IN THE PROBE THAT MEASURED THIS. Every sub-check therefore starts from a FRESH CONTEXT: the
 * first version reused one, so its second measurement was the intersection of two clips and its third was
 * a third of that, and both looked like defects in the library. The last sub-check asserts the
 * accumulation on purpose, so that what misled the probe is held rather than merely known.
 *
 * MEASURED, all four with a fresh context:
 *
 *   one 4x4 rect clip, then fill all     16 painted     (the rect's area)
 *   two rects clip, then fill all        20 painted     (16 + 4 -- the UNION of the rects)
 *   a 5x5 rect path clip, then fill all  25 painted     (the path's area)
 *   two clips in a row                    9 painted     (their INTERSECTION: 3x3) */
static int case_clip_confines_a_fill(void)
{
	int before = g_fail;
	const size_t side = 16, bpr = side * 4;
	unsigned char *buf = (unsigned char *)calloc(side, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGRect two[2] = { { 4, 4, 4, 4 }, { 10, 10, 2, 2 } };
	CGMutablePathRef path;
	CGContextRef ctx;

	check(buf != NULL && rgb != NULL, "the test could not set up");
	if (buf == NULL || rgb == NULL) {
		return g_fail - before;
	}

#define FRESH_CTX() (memset(buf, 0, side * bpr), CGBitmapContextCreate(buf, side, side, 8, bpr, rgb, \
	kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little))
#define FILL_ALL() (CGContextSetRGBFillColor(ctx, 1, 0, 0, 1), CGContextFillRect(ctx, CGRectMake(0, 0, 16, 16)))

	ctx = FRESH_CTX();
	check(ctx != NULL, "the context for the clip checks was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	CGContextClipToRect(ctx, CGRectMake(4, 4, 4, 4));
	FILL_ALL();
	check(painted_pixels(buf, side) == 16, "a 4x4 clip let %zu pixels through, want 16",
	      painted_pixels(buf, side));
	check(!pixel_lit(buf, side, 2, 2) && pixel_lit(buf, side, 5, 5),
	      "the clip did not confine the fill to its own rect");

	ctx = FRESH_CTX();
	CGContextClipToRects(ctx, two, 2);
	FILL_ALL();
	check(painted_pixels(buf, side) == 20, "two rects let %zu pixels through, want 20 (the union)",
	      painted_pixels(buf, side));

	ctx = FRESH_CTX();
	path = CGPathCreateMutable();
	CGPathAddRect(path, NULL, CGRectMake(2, 2, 5, 5));
	CGContextAddPath(ctx, path);
	CGContextClip(ctx);
	FILL_ALL();
	check(painted_pixels(buf, side) == 25, "a 5x5 path clip let %zu pixels through, want 25",
	      painted_pixels(buf, side));

	ctx = FRESH_CTX();
	CGContextClipToRect(ctx, CGRectMake(4, 4, 4, 4));
	CGContextClipToRect(ctx, CGRectMake(2, 2, 5, 5));
	FILL_ALL();
	check(painted_pixels(buf, side) == 9,
	      "two clips in one context let %zu pixels through, want 9: clips ACCUMULATE, and a case that "
	      "reuses a context is measuring their intersection", painted_pixels(buf, side));
#undef FRESH_CTX
#undef FILL_ALL
	return g_fail - before;
}

/* A STROKE PAINTS A RING RATHER THAN A DISC, AND THE HIT TEST ANSWERS ABOUT THE OUTLINE. The second half is
 * CGContext.h's own claim -- "the stroke modes stroke the path FIRST and ask the same question of the
 * outline" -- made observable, and the point that shows it is (0.5, 8) at line width 4: it is OUTSIDE the
 * rectangle and ON the stroked band, so kCGPathStroke contains it and kCGPathFill does not. A first
 * measurement left the width at its default and probed the outline's exact edge, where either answer is
 * defensible; the width is set here so the band has an inside.
 *
 * MEASURED: a 12x12 rect stroked at width 2 paints 96 pixels with its edges lit and (8,8) clear; the hit
 * test at width 4 gives (2,8) stroke=1 fill=1, (8,8) stroke=0 fill=1, (0.5,8) stroke=1 fill=0, and
 * kCGPathFillStroke at the centre 1, because Apple's sentence is "stroked OR filled". */
static int case_stroke_and_hit_test(void)
{
	int before = g_fail;
	const size_t side = 16, bpr = side * 4;
	unsigned char *buf = (unsigned char *)calloc(side, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;

	check(buf != NULL && rgb != NULL, "the test could not set up");
	if (buf == NULL || rgb == NULL) {
		return g_fail - before;
	}
	memset(buf, 0, side * bpr);
	ctx = CGBitmapContextCreate(buf, side, side, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "the context for the stroke checks was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	CGContextSetLineWidth(ctx, 2);
	CGContextSetRGBStrokeColor(ctx, 1, 0, 0, 1);
	CGContextStrokeRect(ctx, CGRectMake(2, 2, 12, 12));
	check(painted_pixels(buf, side) > 0, "stroking a rectangle painted nothing at all");
	check(pixel_lit(buf, side, 2, 8), "the left edge of a stroked rectangle is not painted");
	check(pixel_lit(buf, side, 8, 14), "the top edge of a stroked rectangle is not painted");
	check(!pixel_lit(buf, side, 8, 8), "the INSIDE of a stroked rectangle was painted: a stroke is a ring");

	/* THE HIT TEST, ON A FRESH CONTEXT SO THE CLIP ABOVE CANNOT BE IN PLAY. */
	memset(buf, 0, side * bpr);
	ctx = CGBitmapContextCreate(buf, side, side, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	if (ctx != NULL) {
		CGContextSetLineWidth(ctx, 4);
		CGContextAddRect(ctx, CGRectMake(2, 2, 12, 12));
		check(CGContextPathContainsPoint(ctx, CGPointMake(0.5, 8), kCGPathStroke),
		      "a point on the stroked band but OUTSIDE the rectangle is not contained in stroke mode; "
		      "the header says the stroke modes ask about the outline");
		check(!CGContextPathContainsPoint(ctx, CGPointMake(0.5, 8), kCGPathFill),
		      "a point outside the rectangle is contained in fill mode");
		check(!CGContextPathContainsPoint(ctx, CGPointMake(8, 8), kCGPathStroke),
		      "the centre of the rectangle is contained in STROKE mode; it is not on the outline");
		check(CGContextPathContainsPoint(ctx, CGPointMake(8, 8), kCGPathFill),
		      "the centre of the rectangle is not contained in fill mode");
		check(CGContextPathContainsPoint(ctx, CGPointMake(8, 8), kCGPathFillStroke),
		      "the centre is not contained in the combined mode, which is the union");
	}
	return g_fail - before;
}

/* THE COLOUR'S COMPONENTS, WHICH CGColor.h DESCRIBES IN THREE SENTENCES AND THIS HOLDS ALL THREE:
 * "THE COMPONENTS ARE STORED WITH ALPHA LAST", a gray colour is its own family with its own count, and
 * CGColorEqualToColor is "EQUAL WHEN THE SPACE MODEL AND EVERY COMPONENT MATCH. Not pointer equality".
 *
 * MEASURED: an RGB colour answers 4 components -- 0.250 0.500 0.750 1.000 for the values printed,
 * alpha LAST -- and a gray one answers 2, {0.500, 0.500} for a gray of 0.5 and an alpha of 0.5, because
 * the gray's second component IS its alpha. Two separately built identical colours are equal and are NOT
 * the same pointer, and a red against a gray is not equal, which is the space model being part of it. */
static int case_colour_components(void)
{
	int before = g_fail;
	CGColorRef one = CGColorCreateGenericRGB(0.25, 0.5, 0.75, 1.0);
	CGColorRef same = CGColorCreateGenericRGB(0.25, 0.5, 0.75, 1.0);
	CGColorRef gray = CGColorCreateGenericGray(0.5, 0.5);
	CGColorRef red = CGColorCreateGenericRGB(1.0, 0.0, 0.0, 1.0);
	const CGFloat *c;

	check(one != NULL && same != NULL && gray != NULL && red != NULL, "a colour was refused");
	if (one == NULL || same == NULL || gray == NULL || red == NULL) {
		return g_fail - before;
	}
	check(CGColorGetNumberOfComponents(one) == 4, "an RGB colour answers %zu components, want 4",
	      CGColorGetNumberOfComponents(one));
	c = CGColorGetComponents(one);
	check(c[0] == 0.25 && c[1] == 0.5 && c[2] == 0.75 && c[3] == 1.0,
	      "the components are %.3f %.3f %.3f %.3f, want 0.250 0.500 0.750 1.000 with ALPHA LAST",
	      (double)c[0], (double)c[1], (double)c[2], (double)c[3]);
	check(CGColorGetAlpha(one) == 1.0, "the alpha of an opaque colour is %g", (double)CGColorGetAlpha(one));

	check(CGColorGetNumberOfComponents(gray) == 2, "a gray colour answers %zu components, want 2",
	      CGColorGetNumberOfComponents(gray));
	c = CGColorGetComponents(gray);
	check(c[0] == 0.5 && c[1] == 0.5 && CGColorGetAlpha(gray) == 0.5,
	      "the gray's components are %.3f %.3f and its alpha %.3f, want 0.500 0.500 and 0.500",
	      (double)c[0], (double)c[1], (double)CGColorGetAlpha(gray));

	check(CGColorEqualToColor(one, same), "two identical colours are not equal");
	check(one != same, "two separately built colours are the SAME POINTER");
	check(!CGColorEqualToColor(red, gray), "a red and a gray are equal; the space model is not compared");
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* images: cropping one, and clipping to a mask                              */
/* ------------------------------------------------------------------------ */

/* THE CROP'S RECT IS IN IMAGE SPACE AND A SHADING'S IS NOT, WHICH IS THE WHOLE POINT OF MEASURING THIS. A
 * context's y counts from the bottom; an image's pixels are indexed from the top. The source here is red
 * with a GREEN square drawn at USER (8,8,8,8), which lands in the image's FIRST EIGHT ROWS -- so cropping
 * (8,8,8,8) must return the region eight rows DOWN from the top, which is RED, not the green one directly
 * above it. MEASURED, drawing each crop back at 1:1:
 *
 *   crop (8,8,8,8) -> 8x8, drawn cell (4,4) is R=255 (red)
 *   crop (0,0,8,8) -> 8x8, and it is the green one
 *
 * A case that assumed one convention for both would have called one of these a defect. */
static int case_image_crop(void)
{
	int before = g_fail;
	const size_t side = 16, bpr = side * 4;
	unsigned char *buf = (unsigned char *)calloc(side, bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef src;
	CGImageRef whole;
	/* THE FOUR QUADRANTS AND WHAT EACH RECT MUST RETURN. The rects are in the IMAGE's coordinate space,
	 * which counts y from the TOP; the fills below are in USER space, which counts it from the BOTTOM. */
	struct { CGRect rect; const char *name; unsigned char blue, green, red; } quads[4] = {
		{ { 0, 0, 8, 8 }, "(0,0,8,8)", 0, 255, 0 },	 /* top-left: green */
		{ { 8, 0, 8, 8 }, "(8,0,8,8)", 255, 255, 255 },	 /* top-right: white */
		{ { 0, 8, 8, 8 }, "(0,8,8,8)", 0, 0, 255 },	 /* bottom-left: red */
		{ { 8, 8, 8, 8 }, "(8,8,8,8)", 255, 0, 0 },	 /* bottom-right: blue */
	};
	size_t i;

	check(buf != NULL && rgb != NULL, "the test could not set up");
	if (buf == NULL || rgb == NULL) {
		return g_fail - before;
	}
	memset(buf, 0, side * bpr);
	src = CGBitmapContextCreate(buf, side, side, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(src != NULL, "the source context was refused");
	if (src == NULL) {
		return g_fail - before;
	}
	CGContextSetRGBFillColor(src, 1, 0, 0, 1);
	CGContextFillRect(src, CGRectMake(0, 0, 8, 8));		/* user bottom-left */
	CGContextSetRGBFillColor(src, 0, 0, 1, 1);
	CGContextFillRect(src, CGRectMake(8, 0, 8, 8));		/* user bottom-right */
	CGContextSetRGBFillColor(src, 0, 1, 0, 1);
	CGContextFillRect(src, CGRectMake(0, 8, 8, 8));		/* user top-left */
	CGContextSetRGBFillColor(src, 1, 1, 1, 1);
	CGContextFillRect(src, CGRectMake(8, 8, 8, 8));		/* user top-right */
	whole = CGBitmapContextCreateImage(src);
	check(whole != NULL, "the source image could not be made");
	if (whole == NULL) {
		return g_fail - before;
	}

	/* EACH CROP IS READ FROM ITS OWN BYTES, WITH NOTHING DRAWN AND NOTHING SCALED IN BETWEEN. The first
	 * version of this case drew each crop into a context and sampled a pixel, which puts a scale and a
	 * y-flip between the crop and the answer -- and it reported the green quadrant as not green while the
	 * image's own bytes say it is. The bytes are the crop. */
	for (i = 0; i < 4; i++) {
		CGImageRef cropped = CGImageCreateWithImageInRect(whole, quads[i].rect);
		CFDataRef data;
		const unsigned char *p;
		size_t rowbytes;

		check(cropped != NULL, "the %s crop was refused", quads[i].name);
		if (cropped == NULL) {
			continue;
		}
		check(CGImageGetWidth(cropped) == 8 && CGImageGetHeight(cropped) == 8,
		      "the %s crop is %zux%zu, want 8x8", quads[i].name, CGImageGetWidth(cropped),
		      CGImageGetHeight(cropped));
		data = CGDataProviderCopyData(CGImageGetDataProvider(cropped));
		check(data != NULL, "the %s crop's bytes could not be read", quads[i].name);
		if (data == NULL) {
			continue;
		}
		p = (const unsigned char *)CFDataGetBytePtr(data);
		rowbytes = CGImageGetBytesPerRow(cropped);
		/* B, G, R, A -- the implementation's documented order -- at BOTH CORNERS, which is what makes
		 * this a crop rather than a sample. */
		check(p[0] == quads[i].blue && p[1] == quads[i].green && p[2] == quads[i].red,
		      "the %s crop's (0,0) is B=%u G=%u R=%u, want B=%u G=%u R=%u", quads[i].name, p[0], p[1],
		      p[2], quads[i].blue, quads[i].green, quads[i].red);
		check(p[7 * rowbytes + 28] == quads[i].blue && p[7 * rowbytes + 29] == quads[i].green &&
		      p[7 * rowbytes + 30] == quads[i].red,
		      "the %s crop's (7,7) is B=%u G=%u R=%u, want B=%u G=%u R=%u", quads[i].name,
		      p[7 * rowbytes + 28], p[7 * rowbytes + 29], p[7 * rowbytes + 30], quads[i].blue,
		      quads[i].green, quads[i].red);
	}
	return g_fail - before;
}

/* A CLIP TO A MASK READS THE MASK'S LUMINANCE, AND AN RGB IMAGE IS NOT A MASK AT ALL. Both halves were
 * measured before they were written, and the second is the door's documented contract rather than an
 * accident:
 *
 *   CG-REFUSE: CGContextClipToMask takes an image mask or a DeviceGray image, and this mask is neither
 *
 * MEASURED: a DeviceGray image black on the left half and white on the right, 8 bits per pixel through
 * CGImageCreate -- a BITMAP CONTEXT CANNOT MAKE ONE, because CGBitmapContextCreate supports only two
 * B,G,R,A / R,G,B,A combinations and refuses kCGImageAlphaNone -- clips a fill to EXACTLY THE WHITE HALF,
 * 128 of 256 pixels: white passes and black excludes, which is luminance and not alpha. And an RGB image
 * passed where a mask belongs is REFUSED, leaving the clip as it was, so the fill covers everything: 256
 * of 256, which is what a refusal has to look like if it is to be safe. */
static int case_clip_to_mask(void)
{
	int before = g_fail;
	const size_t side = 16, bpr = side * 4;
	unsigned char *buf = (unsigned char *)calloc(side, bpr);
	static unsigned char rows[16 * 16];	/* the mask's own bytes, alive for the whole case */
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	CGDataProviderRef provider;
	CGImageRef mask;
	CGContextRef ctx;
	size_t x, y, painted = 0;

	check(buf != NULL && rgb != NULL && gray != NULL, "the test could not set up");
	if (buf == NULL || rgb == NULL || gray == NULL) {
		return g_fail - before;
	}
	for (y = 0; y < 16; y++) {
		for (x = 0; x < 16; x++) {
			rows[y * 16 + x] = (x < 8) ? 0 : 255;	/* black left half, white right */
		}
	}
	provider = CGDataProviderCreateWithData(NULL, rows, sizeof rows, NULL);
	check(provider != NULL, "the mask's data provider was refused");
	if (provider == NULL) {
		return g_fail - before;
	}
	mask = CGImageCreate(16, 16, 8, 8, 16, gray, kCGImageAlphaNone, provider, NULL, false,
			     kCGRenderingIntentDefault);
	check(mask != NULL, "a DeviceGray image could not be made for the mask");
	if (mask == NULL) {
		return g_fail - before;
	}
	check(CGImageGetBitsPerPixel(mask) == 8, "the gray mask is %zu bits per pixel, want 8",
	      CGImageGetBitsPerPixel(mask));

	memset(buf, 0, side * bpr);
	ctx = CGBitmapContextCreate(buf, side, side, 8, bpr, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(ctx != NULL, "the context for the mask clip was refused");
	if (ctx == NULL) {
		return g_fail - before;
	}
	CGContextClipToMask(ctx, CGRectMake(0, 0, 16, 16), mask);
	CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
	CGContextFillRect(ctx, CGRectMake(0, 0, 16, 16));
	{
		size_t i;

		for (i = 0; i < side * side; i++) {
			if ((buf[i * 4] | buf[i * 4 + 3]) != 0) {
				painted++;
			}
		}
	}
	check(painted == 128, "a half-white gray mask clipped a fill to %zu of 256 pixels, want 128: the "
			      "rule is luminance", painted);
	check(!pixel_lit(buf, side, 2, 8), "a pixel over the mask's BLACK half was painted");
	check(pixel_lit(buf, side, 12, 8), "a pixel over the mask's WHITE half was not painted");

	/* **AND A SURFACE WHOSE WIDTH IS NOT A MULTIPLE OF FOUR.** The cover image `CGContextClipToMask` builds for
	 * pixman needs a row stride padded to four bytes; passing the surface width worked for every 16- or 32-pixel
	 * canvas this suite used and returned NULL for a 17-pixel one — silently, because the door returns when the
	 * image cannot be made. MEASURED at 1.332 scale on a 1705-pixel window: every clip mask was lost, and toolbar
	 * images were drawn as solid rectangles because their stencil filled the whole rectangle. The same half-mask
	 * must clip half of a 17-pixel-wide fill. */
	{
		const size_t wide = 17;
		unsigned char *buf2 = (unsigned char *)calloc(wide * wide, 4);
		CGContextRef ctx2;
		size_t px = 0, i;

		check(buf2 != NULL, "the odd-width context buffer could not be allocated");
		if (buf2 != NULL) {
			ctx2 = CGBitmapContextCreate(buf2, wide, wide, 8, wide * 4, rgb,
						     kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
			check(ctx2 != NULL, "a %zux%zu context was refused", wide, wide);
			if (ctx2 != NULL) {
				CGContextClipToMask(ctx2, CGRectMake(0, 0, 16, 16), mask);
				CGContextSetRGBFillColor(ctx2, 0, 0, 1, 1);
				CGContextFillRect(ctx2, CGRectMake(0, 0, 16, 16));
				for (i = 0; i < wide * wide; i++) {
					if ((buf2[i * 4] | buf2[i * 4 + 3]) != 0) {
						px++;
					}
				}
				check(px == 128, "the same mask on a 17-pixel-wide surface painted %zu of 256 pixels, "
						  "want 128", px);
				CGContextRelease(ctx2);
			}
			free(buf2);
		}
	}

	/* AND THE REFUSAL, WHICH LEAVES THE CLIP AS IT WAS RATHER THAN MAKING ONE UP. */
	{
		CGImageRef rgb_mask;
		CGContextRef other = CGBitmapContextCreate(buf, side, side, 8, bpr, rgb,
							   kCGImageAlphaPremultipliedFirst |
								   kCGBitmapByteOrder32Little);
		size_t i, unclipped = 0;

		memset(buf, 0, side * bpr);
		CGContextSetRGBFillColor(other, 1, 1, 1, 1);
		CGContextFillRect(other, CGRectMake(0, 0, 8, 16));
		rgb_mask = CGBitmapContextCreateImage(other);
		check(rgb_mask != NULL, "the RGB image for the refusal check could not be made");
		if (rgb_mask != NULL) {
			memset(buf, 0, side * bpr);
			other = CGBitmapContextCreate(buf, side, side, 8, bpr, rgb,
						      kCGImageAlphaPremultipliedFirst |
							      kCGBitmapByteOrder32Little);
			CGContextClipToMask(other, CGRectMake(0, 0, 16, 16), rgb_mask);
			CGContextSetRGBFillColor(other, 0, 0, 1, 1);
			CGContextFillRect(other, CGRectMake(0, 0, 16, 16));
			for (i = 0; i < side * side; i++) {
				if ((buf[i * 4] | buf[i * 4 + 3]) != 0) {
					unclipped++;
				}
			}
			check(unclipped == 256,
			      "an RGB image was accepted as a mask: the fill covered %zu of 256 pixels",
			      unclipped);
		}
	}
	return g_fail - before;
}

/* WHERE A DRAWN IMAGE LANDS, AND WHY BOTH OF THIS SUITE'S EARLIER MISTAKES WERE ABOUT IT. Two probes
 * before this one drew an image into a rect and then argued with the result; neither had measured the rule.
 * The rule, MEASURED with an asymmetric source -- an 8x8 whose FIRST ROW IS WHITE and whose other seven are
 * black, built from its own bytes so nothing is between the data and the answer:
 *
 *   rect (0,0,16,16), 2x      memory rows 0 and 1 white, the rest black
 *   rect (0,0,12,12), 1.5x    memory row 4 white alone, rows 0..3 not painted at all
 *
 * TWO THINGS FOLLOW, AND THEY ARE DIFFERENT THINGS. FIRST: the image's row 0 lands at the rect's TOP, which
 * in a context is its HIGHEST y -- so an image drawn into the bottom-left of a buffer has its first row at
 * the BOTTOM of memory. SECOND: the rect itself is in user space, so a 12-tall rect at y=0 covers that
 * buffer's memory rows 4 through 15 and leaves 0..3 alone. A case that knows only the first rule reads the
 * second one as an off-by-four bug; a case that knows only the second reads the first as one.
 *
 * AND THE 1.5x ANSWER SHOWS THE SAMPLING: exactly ONE row is white, not one and a half, because the image
 * was made with shouldInterpolate false and the scaling is nearest-neighbour. */
/* Draw an image into a rect over a cleared buffer and count how many rows of the MIDDLE COLUMN came out
 * white, reporting the first. The middle column is x=4 below, which is inside every rect these cases use:
 * an earlier probe sampled x=8 against an eight-wide rect and read a column the draw never touched. */
static size_t draw_and_count_white_rows(unsigned char *buf, size_t side, CGImageRef img, CGRect rect,
					size_t *first_row)
{
	CGContextRef ctx;
	size_t i, count = 0;

	*first_row = side;
	memset(buf, 0, side * side * 4);
	ctx = CGBitmapContextCreate(buf, side, side, 8, side * 4, CGColorSpaceCreateDeviceRGB(),
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	if (ctx == NULL) {
		return 0;
	}
	CGContextDrawImage(ctx, rect, img);
	for (i = 0; i < side; i++) {
		if (buf[(i * side + 4) * 4 + 1] > 200) {
			if (*first_row == side) {
				*first_row = i;
			}
			count++;
		}
	}
	return count;
}

static int case_image_draw_placement(void)
{
	int before = g_fail;
	const size_t dst_side = 16, src_side = 8, dst_bpr = dst_side * 4;
	static unsigned char srcbytes[8 * 8 * 4];	/* row 0 white, the rest black, B G R A */
	unsigned char *buf = (unsigned char *)calloc(dst_side, dst_bpr);
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider;
	CGImageRef img;
	size_t x, y, first, count;

	check(buf != NULL && rgb != NULL, "the test could not set up");
	if (buf == NULL || rgb == NULL) {
		return g_fail - before;
	}
	for (y = 0; y < src_side; y++) {
		for (x = 0; x < src_side; x++) {
			unsigned char *p = srcbytes + (y * src_side + x) * 4;
			unsigned char v = (y == 0) ? 255 : 0;

			p[0] = v;
			p[1] = v;
			p[2] = v;
			p[3] = 255;
		}
	}
	provider = CGDataProviderCreateWithData(NULL, srcbytes, sizeof srcbytes, NULL);
	check(provider != NULL, "the source's provider was refused");
	if (provider == NULL) {
		return g_fail - before;
	}
	img = CGImageCreate(src_side, src_side, 8, 32, src_side * 4, rgb,
			    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, NULL,
			    false, kCGRenderingIntentDefault);
	check(img != NULL, "the eight-by-eight source image could not be made");
	if (img == NULL) {
		return g_fail - before;
	}

	/* 2x, FILLING THE WHOLE BUFFER: the image's one white row becomes two, at the very top of memory. */
	count = draw_and_count_white_rows(buf, dst_side, img, CGRectMake(0, 0, 16, 16), &first);
	check(first == 0 && count == 2, "2x painted %zu white rows starting at memory row %zu, want 2 from 0",
	      count, first);

	/* 1.5x, INTO A 12-TALL RECT AT y=0: the rect covers memory rows 4..15 and leaves 0..3 alone, and the
	 * image's row 0 sits at the rect's TOP, which is memory row 4. ONE row, not one and a half, because
	 * the image was made with shouldInterpolate false. */
	count = draw_and_count_white_rows(buf, dst_side, img, CGRectMake(0, 0, 12, 12), &first);
	check(first == 4 && count == 1, "1.5x painted %zu white rows starting at memory row %zu, want 1 from 4",
	      count, first);

	/* 1:1 INTO THE BOTTOM-LEFT: eight rows tall, so the rect covers memory rows 8..15 and the white row is
	 * at its top, which is memory row 8. */
	count = draw_and_count_white_rows(buf, dst_side, img, CGRectMake(0, 0, 8, 8), &first);
	check(first == 8 && count == 1, "a 1:1 draw painted %zu white rows starting at memory row %zu, want 1 "
					"from 8", count, first);

	/* AND NOTHING AT ALL OUTSIDE THE RECT, WHICH IS THE OTHER HALF OF THE RULE. */
	memset(buf, 0, dst_side * dst_bpr);
	{
		CGContextRef ctx = CGBitmapContextCreate(buf, dst_side, dst_side, 8, dst_bpr, rgb,
							 kCGImageAlphaPremultipliedFirst |
								 kCGBitmapByteOrder32Little);
		size_t i, above = 0;

		check(ctx != NULL, "the context for the extent check was refused");
		if (ctx == NULL) {
			return g_fail - before;
		}
		CGContextDrawImage(ctx, CGRectMake(0, 0, 12, 12), img);
		for (i = 0; i < 4 * dst_side; i++) {	/* memory rows 0..3 */
			if ((buf[i * 4] | buf[i * 4 + 1] | buf[i * 4 + 2] | buf[i * 4 + 3]) != 0) {
				above++;
			}
		}
		check(above == 0, "%zu pixels were painted outside a 12-tall rect at y=0, which covers memory "
				  "rows 4..15 of a 16-row buffer", above);
	}
	return g_fail - before;
}

static int run_in_child(const char *name, int (*fn)(void))
{
	pid_t pid;
	int status;

	fflush(NULL);
	pid = fork();
	if (pid == 0) {
		int rc = fn();

		_exit((rc == 0 && g_fail == 0) ? 0 : 1);
	}
	if (pid < 0) {
		fprintf(stderr, "FAIL: %s: fork failed\n", name);
		return 1;
	}
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "FAIL: %s: waitpid failed\n", name);
		return 1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("ok   %s\n", name);
		return 0;
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "FAIL: %s: killed by signal %d\n", name, WTERMSIG(status));
	} else {
		fprintf(stderr, "FAIL: %s: checks failed\n", name);
	}
	return 1;
}

/* THE UNICODE GLYPH STEP, WHICH CORE TEXT'S OWN DOOR WILL SIT ON. It is asserted here rather than in the text
 * repository because the face is here: 'A' and 'a' must map to DIFFERENT non-zero glyphs, an unassigned code
 * point must map to zero while its neighbours still map, and the return must be the number that mapped. */
static int case_font_unichar_mapping(void)
{
	int before = g_fail;
	const char *path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
	CGDataProviderRef provider;
	CGFontRef font;
	uint16_t chars[3];
	CGGlyph glyphs[3];
	int mapped;

	if (access(path, R_OK) != 0) {
		printf("  (not run: %s is not on this machine)\n", path);
		return g_fail - before;
	}
	provider = CGDataProviderCreateWithFilename(path);
	check(provider != NULL, "a provider could not be made from %s", path);
	if (provider == NULL) {
		return g_fail - before;
	}
	font = CGFontCreateWithDataProvider(provider);
	check(font != NULL, "the face could not be made");
	if (font == NULL) {
		return g_fail - before;
	}

	chars[0] = (uint16_t)'A';
	chars[1] = (uint16_t)'a';
	chars[2] = (uint16_t)0xFFFF;	/* an unassigned code point */
	mapped = cg_font_glyphs_for_unichars(font, chars, glyphs, 3);
	check(mapped == 2, "%d of 3 characters mapped, want 2 (0xFFFF is unassigned)", mapped);
	check(glyphs[0] != 0 && glyphs[1] != 0, "'A' and 'a' did not both map: %u and %u",
	      (unsigned)glyphs[0], (unsigned)glyphs[1]);
	check(glyphs[0] != glyphs[1], "'A' and 'a' mapped to the SAME glyph %u", (unsigned)glyphs[0]);
	check(glyphs[2] == 0, "an unassigned code point mapped to glyph %u, want 0", (unsigned)glyphs[2]);

	/* AND A ZERO COUNT ASKS NOTHING RATHER THAN CRASHING, AS DOES A MISSING FACE. */
	check(cg_font_glyphs_for_unichars(font, chars, glyphs, 0) == 0,
	      "a zero-length mapping answered something other than 0");
	check(cg_font_glyphs_for_unichars(NULL, chars, glyphs, 3) == 0,
	      "a mapping with no face answered something other than 0");
	return g_fail - before;
}

/* A TABLE TAG FROM ITS FOUR CHARACTERS, so the case below reads as the format does. */
static uint32_t fn_tag(char a, char b, char c, char d)
{
	return ((uint32_t)(unsigned char)a << 24) | ((uint32_t)(unsigned char)b << 16)
	     | ((uint32_t)(unsigned char)c << 8) | (uint32_t)(unsigned char)d;
}

/* THE RAW TABLE DOORS. Everything the Core Text side owes about a font's TABLES -- variation axes, features, underline
 * metrics, the traits that come from `OS/2` and `head` -- waits for these two, and they are the first doors here that
 * hand back THE FILE'S OWN BYTES rather than an interpretation of them.
 *
 * AND THE TWO-CALL CONTRACT IS WHAT THE CASE HOLDS, because it is the part a caller gets wrong: the size from the first
 * call and the size from the second MUST AGREE, and that agreement is what makes the first call worth having. And the
 * bytes themselves are checked against the format's own rule -- `head` BEGINS WITH ITS VERSION, 0x00010000 for
 * TrueType -- so the case is not merely asking whether something came back. */
static int case_font_tables(void)
{
	int before = g_fail;
	const char *path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
	CGDataProviderRef provider;
	CGFontRef font;
	unsigned char *bytes;
	uint32_t *tags;
	const uint32_t head = fn_tag('h', 'e', 'a', 'd');
	int size, stored, count, i;
	int has_head = 0, has_cmap = 0;
	const uint32_t cmap = fn_tag('c', 'm', 'a', 'p');

	if (access(path, R_OK) != 0) {
		printf("  (not run: %s is not on this machine, so the table doors were NOT exercised)\n", path);
		return g_fail - before;
	}
	provider = CGDataProviderCreateWithFilename(path);
	font = (provider != NULL) ? CGFontCreateWithDataProvider(provider) : NULL;
	check(font != NULL, "the face could not be opened, so the table doors cannot be asked");
	if (font == NULL) {
		return g_fail - before;
	}

	/* ONE TABLE, ASKED TWICE: the size first, then the bytes. */
	size = cg_font_copy_table(font, head, NULL, 0);
	check(size > 0, "the `head` table of a real face reports %d bytes", size);
	if (size > 0) {
		bytes = (unsigned char *)malloc((size_t)size);
		check(bytes != NULL, "the case could not allocate the %d bytes it was promised", size);
		if (bytes != NULL) {
			stored = cg_font_copy_table(font, head, bytes, (size_t)size);
			check(stored == size,
			      "the second call answered %d where the first said %d", stored, size);
			check(bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x00 && bytes[3] == 0x00,
			      "`head` does not begin with version 1.0, so these are not that table's bytes");
			free(bytes);
		}
	}
	/* AND A TABLE NOBODY HAS IS NOT INVENTED. */
	check(cg_font_copy_table(font, fn_tag('z', 'z', 'z', 'z'), NULL, 0) == 0,
	      "a table no face carries reported a size");

	/* AND THE DIRECTORY LISTS THE TABLES A TRUETYPE FACE IS REQUIRED TO HAVE. */
	count = cg_font_table_tags(font, NULL, 0);
	check(count > 0, "the face reports %d tables, and a TrueType face cannot have none", count);
	if (count > 0) {
		tags = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
		check(tags != NULL, "the case could not allocate %d tags", count);
		if (tags != NULL) {
			check(cg_font_table_tags(font, tags, (size_t)count) == count,
			      "the tag count changed between the two calls");
			for (i = 0; i < count; i++) {
				has_head = has_head || (tags[i] == head);
				has_cmap = has_cmap || (tags[i] == cmap);
			}
			check(has_head, "the directory does not list `head`");
			check(has_cmap, "the directory does not list `cmap`");
			/* AND A TABLE IN THE DIRECTORY ANSWERS WITH BYTES, WHICH IS THE TWO DOORS AGREEING. */
			check(cg_font_copy_table(font, head, NULL, 0) == size,
			      "the table's size is not the size the directory reported");
			free(tags);
		}
	}

	/* NULL SAFETY, in the form these doors can answer it. */
	check(cg_font_copy_table(NULL, head, NULL, 0) == 0, "a NULL font produced a table");
	check(cg_font_table_tags(NULL, NULL, 0) == 0, "a NULL font produced a directory");
	CGFontRelease(font);
	return g_fail - before;
}


/* ============================================================================================ *
 * FONT SMOOTHING: SUB-PIXEL ANTIALIASING, AND THE DESTINATION THAT DECIDES IT.                  *
 *                                                                                                *
 * **THE COLOUR IS THE POINT AND THE COLOUR IS WHAT IS MEASURED.** Sub-pixel coverage means a      *
 * pixel's three channels are covered by DIFFERENT amounts — that is where the extra horizontal     *
 * resolution comes from — so the signature of a smoothed glyph is a pixel whose red, green and     *
 * blue differ. Grey coverage cannot produce one: the three channels move together by                *
 * construction. A test that only counted ink could not tell the two apart, so this counts the      *
 * CHANNEL SPLIT, and the same glyph is drawn into two contexts that differ in one thing —          *
 * whether the destination has an alpha channel.                                                    *
 *                                                                                                *
 * THE OTHER HALF IS THAT THE WEIGHT IS UNCHANGED. Smoothing re-samples a glyph; it does not        *
 * redraw it heavier. MEASURED against the outline's own area, the smoothed render of a word puts   *
 * down within a couple of per cent of the grey one (FT_LOAD_TARGET_LCD against FT_LOAD_DEFAULT),   *
 * so the ink count is checked here to within a few per cent rather than left unasked.             *
 * ============================================================================================ */
static int case_font_smoothing(void)
{
	int before = g_fail;
	const char *path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
	const char *word = "Hamburgefons";
	const size_t side = 200;
	const size_t bpr = side * 4;
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider;
	CGFontRef font;
	unsigned char *kept[2] = { NULL, NULL };
	int opaque_fringes = 0, alpha_fringes = 0;
	int opaque_ink = 0, alpha_ink = 0;
	double opaque_mass = 0.0, alpha_mass = 0.0;
	int biggest_split = 0;
	int variant;

	if (access(path, R_OK) != 0) {
		printf("  (not run: %s is not on this machine)\n", path);
		return g_fail - before;
	}
	provider = CGDataProviderCreateWithFilename(path);
	font = (provider != NULL) ? CGFontCreateWithDataProvider(provider) : NULL;
	check(font != NULL, "the face could not be opened, so smoothing cannot be asked for");
	if (font == NULL) {
		return g_fail - before;
	}

	/* TWO CONTEXTS, ONE PROPERTY APART. The bytes are the same format for both — B, G, R, then a byte
	 * that is an ALPHA in one and NOT WRITTEN in the other — so the only difference the renderer can see
	 * is the one this case is about. */
	for (variant = 0; variant < 2; variant++) {
		unsigned char *px = (unsigned char *)calloc(side * side, 4);
		uint32_t info = (variant == 0)
			? (uint32_t)(kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little)
			: (uint32_t)(kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
		CGContextRef ctx;
		size_t x, y;
		int fringes = 0, ink = 0;
		double mass = 0.0;

		if (px == NULL) {
			continue;
		}
		/* WHITE, BECAUSE THE FRINGE SHOWS ON A LIGHT GROUND and because an opaque destination has to be
		 * given its colour: an `x` context never had one. */
		for (x = 0; x < side * side; x++) {
			px[x * 4 + 0] = 255;
			px[x * 4 + 1] = 255;
			px[x * 4 + 2] = 255;
			px[x * 4 + 3] = 255;
		}
		ctx = CGBitmapContextCreate(px, side, side, 8, bpr, rgb, info);
		check(ctx != NULL, "a %s destination was refused", variant == 0 ? "opaque" : "alpha");
		if (ctx == NULL) {
			free(px);
			continue;
		}
		kept[variant] = px;
		CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);
		CGContextSetFont(ctx, font);
		CGContextSetFontSize(ctx, 26.0);
		CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
		CGContextShowTextAtPoint(ctx, 10.0, 60.0, word, strlen(word));

		for (y = 0; y < side; y++) {
			for (x = 0; x < side; x++) {
				unsigned char *p = px + (y * side + x) * 4;   /* memory B, G, R */
				int split = (int)p[2] - (int)p[1];
				int split2 = (int)p[1] - (int)p[0];

				if (split < 0) {
					split = -split;
				}
				if (split2 < 0) {
					split2 = -split2;
				}
				if (split > 8 || split2 > 8) {
					fringes++;
					if (split > biggest_split) {
						biggest_split = split;
					}
					if (split2 > biggest_split) {
						biggest_split = split2;
					}
				}
				if (p[0] < 200 || p[1] < 200 || p[2] < 200) {
					ink++;
				}
				/* **THE INK IS MEASURED AS MASS AND NOT AS A COUNT OF DARK PIXELS**, because that is the
				 * difference sub-pixel coverage makes: it SPREADS a glyph's ink over more pixels at lower
				 * coverage each, so a count of "how many pixels are dark" rises even though the same ink was
				 * put down. Summing the channels' darkness is proportional to the covered area for BOTH
				 * coverages — three samples of a pixel add up to three times its average coverage — so the
				 * two are the same kind of number and can be compared. */
				mass += (255.0 - (double)p[0]) + (255.0 - (double)p[1]) + (255.0 - (double)p[2]);
			}
		}
		if (variant == 0) {
			opaque_fringes = fringes;
			opaque_ink = ink;
			opaque_mass = mass;
		} else {
			alpha_fringes = fringes;
			alpha_ink = ink;
			alpha_mass = mass;
		}
		CGContextRelease(ctx);
	}
	printf("  (opaque: %d fringed pixels, strongest split %d, ink %.0f; alpha: %d fringed, ink %.0f)\n",
	       opaque_fringes, biggest_split, opaque_mass, alpha_fringes, alpha_mass);

	/* 1. THE GLYPHS WERE DRAWN AT ALL, in both destinations. */
	check(opaque_ink > 500, "the word did not reach the opaque destination (%d inked)", opaque_ink);
	check(alpha_ink > 500, "the word did not reach the alpha destination (%d inked)", alpha_ink);

	/* 2. THE OPAQUE ONE IS IN COLOUR: per-channel coverage, which is what sub-pixel antialiasing IS. */
	check(opaque_fringes > 200, "a smoothed glyph put down %d fringed pixels, want hundreds",
	      opaque_fringes);

	/* 3. AND THE ALPHA ONE IS NOT, BECAUSE A LAYER WITH TRANSPARENCY CANNOT CARRY IT. This is the check
	 * that would pass if the gate were missing and the renderer smoothed everything. */
	check(alpha_fringes == 0, "an ALPHA destination got %d fringed pixels — smoothing was not gated on it",
	      alpha_fringes);

	/* 4. AND THE WEIGHT IS THE SAME. Smoothing re-samples a glyph; it does not draw it heavier. The two
	 * ink counts are the same glyph in the two coverages, so they must agree closely. */
	{
		double ratio = (opaque_mass > 0.0) ? alpha_mass / opaque_mass : 0.0;

		check(ratio > 0.9 && ratio < 1.1, "the smoothed and grey inks differ by %.1f%% (%.0f against %.0f)",
		      100.0 * (ratio - 1.0), opaque_mass, alpha_mass);
	}
	/* 5. **AND THE SAMPLES LAND ON THE CHANNELS THEY BELONG TO.** A smoothed glyph and a grey one of the same
	 * word cover the SAME area — check 4 above says so to a fraction of a per cent — but the smoothed one spreads
	 * that coverage ACROSS the three channels of each pixel, so the two bitmaps are not equal pixel for pixel.
	 * What they must agree on is the average: the mean of a pixel's three channels is that pixel's coverage,
	 * whatever the samples did inside it. SO THE CHECK IS THE PER-PIXEL DIFFERENCE BETWEEN THE SMOOTHED AVERAGE AND
	 * THE GREY VALUE, summed.
	 *
	 * IT IS THE CHECK THAT CATCHES A MISALIGNED BITMAP. Entering the samples at a pixel boundary when the glyph's
	 * first sample is not on one writes every channel from its NEIGHBOUR's coverage — the fringe becomes a colour
	 * cast over the whole glyph — and that MOVES COVERAGE FROM ONE PIXEL TO THE NEXT, which is exactly what this
	 * sum measures. A faithful redistribution leaves a small residual; a shifted one leaves a large one. */
	{
		double l1 = 0.0, ink = 0.0;
		size_t x, y;

		for (y = 0; y < side; y++) {
			for (x = 0; x < side; x++) {
				const unsigned char *sm = kept[0] + (y * side + x) * 4;
				const unsigned char *gr = kept[1] + (y * side + x) * 4;
				double average = ((double)sm[0] + (double)sm[1] + (double)sm[2]) / 3.0;

				l1 += average > (double)gr[0] ? average - (double)gr[0]
							      : (double)gr[0] - average;
				ink += 255.0 - (double)gr[0];
			}
		}
		printf("  (the smoothed average differs from the grey render by %.0f, against %.0f of ink: %.1f%%)\n",
		       l1, ink, 100.0 * l1 / (ink > 0.0 ? ink : 1.0));
		check(ink > 0.0 && l1 < 0.5 * ink,
		      "the smoothed average is %.1f%% away from the grey render, which is a SHIFTED bitmap rather "
		      "than a re-sampled one", 100.0 * l1 / (ink > 0.0 ? ink : 1.0));
	}
	/* 6. **AND WHICH WAY THE FRINGE POINTS, WHICH IS THE SHARPEST STATEMENT AVAILABLE ABOUT WHERE THE SAMPLES
	 * WENT.** The engine assumes the destination's three sub-pixels are R, G, B from left to right, and the LEFT
	 * edge of black-on-white ink — ink to the right, paper to the left — is where the coverage RISES rightward:
	 * the red sample (the leftmost of the three) is the one the ink covers LEAST, so it stays brightest and that
	 * edge pixel comes out REDDER THAN IT IS BLUE. The word's right edge is the mirror of it and comes out
	 * BLUER. MEASURED at 26 points: the left edge reads r 253 against b 140, the right r 37 against b 82 — which
	 * is the classic fringe, red on the left of the stroke and blue on the right.
	 *
	 * THAT ORDER IS A FACT ABOUT THE ALIGNMENT AND NOT ABOUT THE GLYPH, which is what makes it worth asserting: a
	 * bitmap entered at a pixel boundary instead of at its first sample writes every channel from its NEIGHBOUR's
	 * coverage, SHIFTING the fringe by one sub-pixel and reversing these two edges — while the ink and the mass
	 * above both still agree, because neither of those can see a shift. */
	{
		int leftmost = -1, rightmost = -1;
		size_t x, y;
		double left_r = 0.0, left_b = 0.0, right_r = 0.0, right_b = 0.0;
		int left_n = 0, right_n = 0;

		for (x = 0; x < side; x++) {
			for (y = 0; y < side; y++) {
				const unsigned char *p = kept[0] + (y * side + x) * 4;

				if (p[0] < 200 || p[1] < 200 || p[2] < 200) {
					if (leftmost < 0) {
						leftmost = (int)x;
					}
					rightmost = (int)x;
					break;
				}
			}
		}
		if (leftmost >= 0) {
			for (y = 0; y < side; y++) {
				const unsigned char *l = kept[0] + (y * side + (size_t)leftmost) * 4;
				const unsigned char *r = kept[0] + (y * side + (size_t)rightmost) * 4;

				if (l[0] < 200 || l[1] < 200 || l[2] < 200) {
					left_r += (double)l[2];		/* memory B, G, R */
					left_b += (double)l[0];
					left_n++;
				}
				if (r[0] < 200 || r[1] < 200 || r[2] < 200) {
					right_r += (double)r[2];
					right_b += (double)r[0];
					right_n++;
				}
			}
		}
		printf("  (left edge %d: r %.0f b %.0f; right edge %d: r %.0f b %.0f)\n", leftmost,
		       left_n ? left_r / left_n : 0.0, left_n ? left_b / left_n : 0.0, rightmost,
		       right_n ? right_r / right_n : 0.0, right_n ? right_b / right_n : 0.0);
		check(left_n > 0 && right_n > 0 && left_r > left_b,
		      "the LEFT edge of the word is not redder than it is blue (r %.0f against b %.0f) — the samples "
		      "are not lined up with the destination's sub-pixels",
		      left_n ? left_r / left_n : 0.0, left_n ? left_b / left_n : 0.0);
		check(right_n > 0 && right_b > right_r,
		      "the RIGHT edge of the word is not bluer than it is red (r %.0f against b %.0f)",
		      right_n ? right_r / right_n : 0.0, right_n ? right_b / right_n : 0.0);
	}
	free(kept[0]);
	free(kept[1]);
	CGFontRelease(font);
	CGDataProviderRelease(provider);
	return g_fail - before;
}


/* **THE PAIR ADJUSTMENT, WHICH IS THE ONE FONT NUMBER NO OTHER DOOR HERE ANSWERS.** An advance is a glyph's own;
 * a KERN is the font's statement about TWO of them, and a line cannot derive it from the advances it already has.
 * Three answers are worth holding: a real pair that the face kerns (negative, and not by accident), a real pair it
 * does not (exactly zero), and a face with NO `kern` table at all (zero for everything, which is a result rather
 * than an absence — the door asks `FT_HAS_KERNING` first so that such a face does not take the engine's error path
 * once per pair). */
static int case_font_kerning(void)
{
	int before = g_fail;
	const char *prop = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
	const char *mono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
	CGDataProviderRef provider;
	CGFontRef font;
	CGGlyph tg[2], og[2], hg[2];
	UniChar t = 'T', o = 'o', h = 'h';
	int kern;

	if (access(prop, R_OK) != 0) {
		printf("  (not run: %s is not on this machine)\n", prop);
		return g_fail - before;
	}
	provider = CGDataProviderCreateWithFilename(prop);
	font = (provider != NULL) ? CGFontCreateWithDataProvider(provider) : NULL;
	check(font != NULL, "the proportional face could not be opened");
	if (font == NULL) {
		return g_fail - before;
	}
	memset(tg, 0, sizeof tg);
	memset(og, 0, sizeof og);
	memset(hg, 0, sizeof hg);
	if (!cg_font_glyphs_for_unichars(font, &t, tg, 1) || !cg_font_glyphs_for_unichars(font, &o, og, 1)
	    || !cg_font_glyphs_for_unichars(font, &h, hg, 1)) {
		printf("  (not run: the face has no glyph for 'T', 'o' or 'h')\n");
		CGFontRelease(font);
		CGDataProviderRelease(provider);
		return g_fail - before;
	}
	kern = cg_font_kern(font, tg[0], og[0]);
	printf("  (the face asks for 'To' to close by %d units of %d)\n", kern, CGFontGetUnitsPerEm(font));
	check(kern < 0, "'To' is not kerned by this face (%d), so the case cannot judge anything", kern);
	check(cg_font_kern(font, tg[0], hg[0]) == 0, "'Th' is kerned by %d, want none",
	      cg_font_kern(font, tg[0], hg[0]));
	check(cg_font_kern(font, og[0], tg[0]) != kern || kern == 0,
	      "the adjustment does not depend on the pair's ORDER, which it must");
	check(cg_font_kern(NULL, tg[0], og[0]) == 0, "a kern with no face answered something other than 0");
	CGFontRelease(font);
	CGDataProviderRelease(provider);

	/* **AND A FACE WHOSE PAIRS ARE ALL IN GPOS, WHICH IS WHAT A MODERN FACE IS.** Inter ships no `kern` table at
	 * all, so the ENGINE's kerning door answers zero for every pair it has — MEASURED, that is why this desktop's
	 * text was set unkerned while Pango (HarfBuzz) closed the same pair by a pixel. This is the check that the
	 * reader on the other side of `cg_font_kern` is doing the work the engine cannot: 'To' closes and 'ck', which
	 * the face has no opinion about, does not. */
	{
		const char *inter = "/usr/share/fonts/opentype/inter/Inter-Regular.otf";

		if (access(inter, R_OK) == 0) {
			UniChar c = 'c', k = 'k';
			CGGlyph cg[2], kg[2];

			provider = CGDataProviderCreateWithFilename(inter);
			font = (provider != NULL) ? CGFontCreateWithDataProvider(provider) : NULL;
			memset(cg, 0, sizeof cg);
			memset(kg, 0, sizeof kg);
			if (font != NULL && cg_font_glyphs_for_unichars(font, &t, tg, 1)
			    && cg_font_glyphs_for_unichars(font, &o, og, 1)
			    && cg_font_glyphs_for_unichars(font, &c, cg, 1)
			    && cg_font_glyphs_for_unichars(font, &k, kg, 1)) {
				int gp = cg_font_kern(font, tg[0], og[0]);

				printf("  (Inter has no kern table; 'To' closes by %d units of %d, 'ck' by %d)\n", gp,
				       CGFontGetUnitsPerEm(font), cg_font_kern(font, cg[0], kg[0]));
				check(gp < 0, "a GPOS-only face was NOT kerned ('To' answered %d), so its pairs are "
					      "still invisible", gp);
				check(cg_font_kern(font, cg[0], kg[0]) == 0, "'ck' is kerned by %d, want none",
				      cg_font_kern(font, cg[0], kg[0]));
			}
			CGFontRelease(font);
			CGDataProviderRelease(provider);
		} else {
			printf("  (not run: %s is not on this machine, so the GPOS path was NOT exercised)\n",
			       inter);
		}
	}

	/* AND A FACE WITH NO `kern` TABLE AND NO PAIRS AT ALL, which a monospace face usually is. */
	if (access(mono, R_OK) == 0) {
		provider = CGDataProviderCreateWithFilename(mono);
		font = (provider != NULL) ? CGFontCreateWithDataProvider(provider) : NULL;
		if (font != NULL && cg_font_glyphs_for_unichars(font, &t, tg, 1)
		    && cg_font_glyphs_for_unichars(font, &o, og, 1)) {
			check(cg_font_kern(font, tg[0], og[0]) == 0,
			      "a face with no kern table answered %d for 'To'", cg_font_kern(font, tg[0], og[0]));
		}
		CGFontRelease(font);
		CGDataProviderRelease(provider);
	}
	return g_fail - before;
}


/* **DOES THE PEN'S FRACTION REACH THE SCREEN?** Measured END TO END — a glyph drawn through a real context into an
 * opaque bitmap at two pen positions half a pixel apart, and the composited ink's centre of mass compared. That is
 * the question a person's eyes are asking, and it is asked of BOTH renderings: grey coverage is one sample to the
 * pixel and the fraction is a translation of the outline, while SMOOTHED coverage is three samples to the pixel and
 * the fraction has to land on that finer grid — different arithmetic, and the window is drawn with the second.
 */
static double fn_surface_centroid(CGContextRef c)
{
	unsigned char *px = (unsigned char *)CGBitmapContextGetData(c);
	int stride = (int)CGBitmapContextGetBytesPerRow(c);
	int w = (int)CGBitmapContextGetWidth(c);
	int h = (int)CGBitmapContextGetHeight(c);
	double mass = 0.0, moment = 0.0;
	int x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned char *p = px + (size_t)y * (size_t)stride + (size_t)x * 4u;   /* memory B, G, R, X */
			double darkness = 255.0 - ((double)p[0] + (double)p[1] + (double)p[2]) / 3.0;

			if (darkness <= 0.0) {
				continue;
			}
			mass += darkness;
			moment += darkness * ((double)x + 0.5);
		}
	}
	return mass > 0.0 ? moment / mass : 0.0;
}

static int case_subpixel_positioning(void)
{
	int before = g_fail;
	const char *path = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGDataProviderRef provider;
	CGFontRef font;
	CGGlyph glyph = 0;
	UniChar l = 'l';
	int smoothing;

	if (access(path, R_OK) != 0) {
		printf("  (not run: %s is not on this machine)\n", path);
		return g_fail - before;
	}
	provider = CGDataProviderCreateWithFilename(path);
	font = (provider != NULL) ? CGFontCreateWithDataProvider(provider) : NULL;
	check(font != NULL, "the face could not be opened");
	if (font == NULL) {
		return g_fail - before;
	}
	if (!cg_font_glyphs_for_unichars(font, &l, &glyph, 1)) {
		printf("  (not run: the face has no glyph for 'l')\n");
		CGFontRelease(font);
		CGDataProviderRelease(provider);
		return g_fail - before;
	}
	for (smoothing = 0; smoothing < 2; smoothing++) {
		double at[5];
		int i;

		for (i = 0; i < 5; i++) {
			unsigned char *px = (unsigned char *)calloc(64u * 48u, 4u);
			CGContextRef ctx = (px != NULL)
				? CGBitmapContextCreate(px, 64, 48, 8, 64 * 4, rgb,
							(uint32_t)(kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little))
				: NULL;

			/* **WHITE, BECAUSE AN OPAQUE DESTINATION IS NOT HANDED A COLOUR** — `x` formats have none, and a
			 * buffer left as zeros is not "empty", it is black, which is why the first version of this measured
			 * every pen position as the bitmap's own centre. */
			if (ctx != NULL) {
				memset(px, 0xFF, 64u * 48u * 4u);
			}

			if (ctx == NULL) {
				free(px);
				at[i] = 0.0;
				continue;
			}
			CGContextSetShouldSmoothFonts(ctx, smoothing ? true : false);
			CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);
			CGContextSetFont(ctx, font);
			CGContextSetFontSize(ctx, 26.0);
			CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
			CGContextShowGlyphsAtPoint(ctx, 10.0 + 0.25 * (double)i, 30.0, &glyph, 1);
			at[i] = fn_surface_centroid(ctx);
			CGContextRelease(ctx);
			free(px);
		}
		printf("  (%s: the pen at 10.00/10.25/10.50/10.75/11.00 puts the ink at %.3f/%.3f/%.3f/%.3f/%.3f)\n",
		       smoothing ? "SMOOTHED" : "grey    ", at[0], at[1], at[2], at[3], at[4]);
		for (i = 1; i < 5; i++) {
			check(at[i] - at[i - 1] > 0.05 && at[i] - at[i - 1] < 0.45,
			      "the %s pen's quarter-pixel step %d moved the ink by %.3f, want about a quarter",
			      smoothing ? "smoothed" : "grey", i, at[i] - at[i - 1]);
		}
		check(at[4] - at[0] > 0.6 && at[4] - at[0] < 1.4,
		      "the %s ink moved %.3f over a WHOLE pixel of pen travel", smoothing ? "smoothed" : "grey",
		      at[4] - at[0]);
	}
	CGFontRelease(font);
	CGDataProviderRelease(provider);
	return g_fail - before;
}


/* ============================================================================================ *
 * WHERE THE CHANNELS LAND, ASKED OF ALL FOUR DESTINATIONS.                                       *
 *                                                                                                *
 * **THIS CASE EXISTS BECAUSE ONE OF THEM WAS WRONG AND NOTHING ASKED.** Four formats are answered  *
 * — each of this library's two memory layouts in an alpha and an OPAQUE spelling — and the order   *
 * of the channels is a property of the LAYOUT, not of whether the fourth byte is an alpha:          *
 * `NoneSkipFirst` is B, G, R, X exactly as `PremultipliedFirst` is. Every writer used to work that  *
 * out for itself, and when the opaque spellings were added one of them asked pixman (`dest ==       *
 * a8r8g8b8`, which an `x8r8g8b8` surface is not) and two spelled it as "PremultipliedLast means      *
 * R, G, B" — so an image drawn onto a WINDOW came out with its red and blue swapped: MEASURED, the  *
 * menu bar's close button read (86, 80, 242) where the theme's `#F25056` is (242, 80, 86).           *
 *                                                                                                *
 * THE EXPECTED BYTES ARE WRITTEN FROM THE TWO LAYOUTS AND NOT FROM THE CODE, which is the only way  *
 * a test of this says anything: a component FIRST in a little-endian word is B, G, R, X in memory,  *
 * and a component LAST in a big-endian word is R, G, B, X.                                          *
 * ============================================================================================ */
static int case_destination_channel_order(void)
{
	int before = g_fail;
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef src;
	CGImageRef image;
	unsigned char *src_px = (unsigned char *)calloc(4u, 4u);
	struct { uint32_t info; const char *name; int red_at; int blue_at; } formats[] = {
		{ kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, "premultiplied first, 32 little", 2, 0 },
		{ kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, "no alpha, skip first, 32 little", 2, 0 },
		{ kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, "premultiplied last, 32 big", 0, 2 },
		{ kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big, "no alpha, skip last, 32 big", 0, 2 },
	};
	size_t f;

	check(src_px != NULL && rgb != NULL, "the test could not allocate its buffer or space");
	if (src_px == NULL || rgb == NULL) {
		return g_fail - before;
	}
	/* A SOURCE WHOSE THREE CHANNELS ARE ALL DIFFERENT, so a swap cannot hide: 230, 102, 51. */
	memset(src_px, 0, 16u);
	src = CGBitmapContextCreate(src_px, 1, 1, 8, 4, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	check(src != NULL, "the source context could not be made");
	if (src == NULL) {
		free(src_px);
		return g_fail - before;
	}
	CGContextSetRGBFillColor(src, 0.9, 0.4, 0.2, 1.0);
	CGContextFillRect(src, CGRectMake(0.0, 0.0, 1.0, 1.0));
	image = CGBitmapContextCreateImage(src);
	check(image != NULL, "the source image could not be made");
	if (image == NULL) {
		CGContextRelease(src);
		free(src_px);
		return g_fail - before;
	}

	for (f = 0; f < sizeof formats / sizeof formats[0]; f++) {
		unsigned char *px = (unsigned char *)calloc(4u, 4u);
		CGContextRef dst = (px != NULL) ? CGBitmapContextCreate(px, 1, 1, 8, 4, rgb, formats[f].info) : NULL;

		check(dst != NULL, "%s was refused", formats[f].name);
		if (dst == NULL) {
			free(px);
			continue;
		}
		CGContextDrawImage(dst, CGRectMake(0.0, 0.0, 1.0, 1.0), image);
		printf("  (%s: bytes %3d %3d %3d %3d — red at %d, blue at %d)\n", formats[f].name, px[0], px[1],
		       px[2], px[3], formats[f].red_at, formats[f].blue_at);
		check(px[formats[f].red_at] > 200 && px[formats[f].red_at] < 245,
		      "%s put %d where RED belongs, want about 230", formats[f].name, px[formats[f].red_at]);
		check(px[1] > 80 && px[1] < 125, "%s put %d where GREEN belongs, want about 102",
		      formats[f].name, px[1]);
		check(px[formats[f].blue_at] > 30 && px[formats[f].blue_at] < 75,
		      "%s put %d where BLUE belongs, want about 51", formats[f].name, px[formats[f].blue_at]);
		CGContextRelease(dst);
		free(px);
	}
	CGImageRelease(image);
	CGContextRelease(src);
	free(src_px);
	return g_fail - before;
}

/* **WRITING AN IMAGE OUT, WHICH IS THE DOOR THE APPKIT NAMED.** The encoder is `cg_image_png_data`
 * (`CGImagePNG.c`, beside the decoder) and `CGImageDestination` is the object Apple documents around it; what this
 * case measures is the ROUND TRIP, because that is the only thing that proves the two halves agree: an image is
 * written, the bytes are read back through this library's own PNG decoder, and the pixels are compared.
 *
 * **AND THE PIXELS ARE CHOSEN TO CATCH THE ONE CONVERSION THAT IS EASY TO GET WRONG.** This library stores
 * PREMULTIPLIED alpha and PNG stores STRAIGHT alpha, so the encoder has to divide and the decoder has to multiply;
 * a semi-transparent pixel is the only thing that notices. The opaque pixel is exact to the byte, and the
 * semi-transparent one is allowed one step either way, because the two roundings are each to nearest and do not have
 * to land on the number that went in.
 *
 * **AND THE REFUSALS ARE PART OF THE CASE**, because a destination that accepted `public.jpeg` and wrote a PNG is
 * the kind of wrong answer a round trip cannot see: the type is checked at creation, a second image is refused, and
 * finalizing twice is refused with the destination still holding what it had. */
static int case_image_destination_png(void)
{
	static const unsigned char kPixels[] = {
		/* BGRA, PREMULTIPLIED — the layout the PNG decoder produces and the blitter reads. */
		0x00, 0x00, 0xFF, 0xFF,	/* opaque red */
		0x00, 0xFF, 0x00, 0xFF,	/* opaque green */
		0x19, 0x32, 0x64, 0x80,	/* straight (200,100,50) at alpha 128, premultiplied */
		0xFF, 0xFF, 0xFF, 0x00	/* fully transparent */
	};
	CGDataProviderRef provider;
	CGImageRef image;
	CGImageRef back;
	CFMutableDataRef data;
	CGImageDestinationRef idst;
	CGDataProviderRef readBack;
	CFArrayRef types;
	const unsigned char *bytes;
	size_t length;
	bool made;

	provider = CGDataProviderCreateWithData(NULL, kPixels, sizeof kPixels, NULL);
	check(provider != NULL, "the pixel provider was made");
	if (provider == NULL) {
		return g_fail;
	}
	image = CGImageCreate(2, 2, 8, 32, 8, CGColorSpaceCreateDeviceRGB(),
			      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, NULL, true,
			      kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	check(image != NULL, "the source image was made from those pixels");
	if (image == NULL) {
		return g_fail;
	}

	/* WHAT THIS BUILD SAYS IT WRITES, WHICH IS THE HONEST ANSWER TO "WHAT CAN I ASK FOR". */
	types = CGImageDestinationCopyTypeIdentifiers();
	check(types != NULL && CFArrayGetCount(types) == 1
	      && CFEqual(CFArrayGetValueAtIndex(types, 0), CFSTR("public.png")),
	      "the destination writes public.png and says so");
	if (types != NULL) {
		CFRelease(types);
	}

	/* AND A TYPE IT DOES NOT WRITE IS REFUSED AT CREATION, not after the images have been added. */
	data = CFDataCreateMutable(kCFAllocatorDefault, 0);
	check(data != NULL, "the mutable data was made");
	idst = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, NULL);
	check(idst == NULL, "a destination asked for public.jpeg is refused rather than writing a PNG");

	idst = (data != NULL) ? CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, NULL) : NULL;
	check(idst != NULL, "a destination for public.png was made");
	if (idst != NULL) {
		CGImageDestinationAddImage(idst, image, NULL);
		check(CGImageDestinationFinalize(idst), "the image was written");
		bytes = (const unsigned char *)CFDataGetBytePtr(data);
		length = (size_t)CFDataGetLength(data);
		check(length > 8 && bytes != NULL && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N'
		      && bytes[3] == 'G',
		      "and the bytes begin with the PNG signature (%zu of them)", length);

		/* **THE SECOND FINALIZE IS REFUSED**, because appending the image to its own output is what an
		 * over-helpful encoder does and it corrupts the file rather than failing. */
		check(!CGImageDestinationFinalize(idst), "a second finalize is refused");

		/* AND THE ROUND TRIP, through this library's own decoder. */
		readBack = (length > 0) ? CGDataProviderCreateWithData(NULL, bytes, length, NULL) : NULL;
		back = (readBack != NULL)
			? CGImageCreateWithPNGDataProvider(readBack, NULL, true, kCGRenderingIntentDefault) : NULL;
		check(back != NULL, "the bytes decode back into an image");
		if (back != NULL) {
			CGDataProviderRef decoded = CGImageGetDataProvider(back);
			size_t decodedLength = 0;
			const unsigned char *p = (const unsigned char *)cg_dataprovider_bytes(decoded, &decodedLength);

			check(CGImageGetWidth(back) == 2 && CGImageGetHeight(back) == 2 && p != NULL
			      && decodedLength == 16,
			      "at the same size and in the same layout (%zux%zu, %zu bytes)",
			      CGImageGetWidth(back), CGImageGetHeight(back), decodedLength);
			if (p != NULL && decodedLength == 16) {
				/* BGRA in memory, and the decoder has premultiplied again. */
				check(p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFF && p[3] == 0xFF,
				      "the opaque red pixel survives exactly (%02X %02X %02X %02X)", p[0], p[1],
				      p[2], p[3]);
				check(p[4] == 0x00 && p[5] == 0xFF && p[6] == 0x00 && p[7] == 0xFF,
				      "the opaque green pixel survives exactly (%02X %02X %02X %02X)", p[4], p[5],
				      p[6], p[7]);
				check(p[8] + 1 >= 0x19 && p[8] <= 0x19 + 1 && p[9] + 1 >= 0x32 && p[9] <= 0x32 + 1
				      && p[10] + 1 >= 0x64 && p[10] <= 0x64 + 1 && p[11] == 0x80,
				      "and the SEMI-TRANSPARENT pixel comes back within one step of where it "
				      "started (%02X %02X %02X %02X, was 19 32 64 80)", p[8], p[9], p[10], p[11]);
				printf("  (encoded %zu bytes for a 2x2 image; semi-transparent pixel %02X %02X %02X %02X)\n",
				       length, p[8], p[9], p[10], p[11]);
			}
			CGImageRelease(back);
		}
		if (readBack != NULL) {
			CGDataProviderRelease(readBack);
		}
		CFRelease(idst);
	}

	/* AND THE FILE DOOR, which is the same encoder with a name to write to. */
	if (data != NULL) {
		CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("/tmp/agora-destination.png"),
							     kCFURLPOSIXPathStyle, false);

		check(url != NULL, "the file URL was made");
		if (url != NULL) {
			idst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);

			made = (idst != NULL);
			if (idst != NULL) {
				CGImageDestinationAddImage(idst, image, NULL);
				made = CGImageDestinationFinalize(idst);
				CFRelease(idst);
			}
			check(made, "and a destination over a URL writes the file");
			if (made) {
				FILE *file = fopen("/tmp/agora-destination.png", "rb");

				check(file != NULL, "the file exists");
				if (file != NULL) {
					fclose(file);
					remove("/tmp/agora-destination.png");
				}
			}
			CFRelease(url);
		}
		CFRelease(data);
	}
	CGImageRelease(image);
	return g_fail;
}

static const struct { const char *name; int (*fn)(void); } g_cases[] = {
	{ "The colour spaces this library answers, by name", case_named_spaces },
	{ "The type-id table's two promises", case_type_ids },
	{ "A provider made from a CFDataRef gives the bytes back", case_data_provider },
	{ "A consumer takes bytes and a gradient takes a CFArray of CGColorRef",
	  case_consumer_and_gradient },
	{ "A bitmap context whose fill reaches the pixels", case_bitmap_context_fill },
	{ "A path: empty, contains, and its box", case_path },
	{ "A quadratic's two boxes and the ink under its arch", case_quad_curve },
	{ "A triangle fills as a triangle, and its mirror mirrors it", case_triangle_fill },
	{ "A font made from bytes", case_font_from_provider },
	{ "A gradient painted into a context", case_gradient_into_context },
	{ "An axial shading painted through a caller's function", case_axial_shading },
	{ "Images decoded from PNG and JPEG files, then drawn", case_images_from_data },
	{ "An image written to PNG and read back, and the refusals beside it", case_image_destination_png },
	{ "A radial shading, and that swapping its circles swaps the ramp", case_radial_shading },
	{ "The radial degenerate cases, and the two refusals", case_radial_degenerate_and_refusals },
	{ "The radial gradient and the radial shading are one ramp", case_radial_gradient_matches_the_shading },
	{ "A clip confines a fill, and clips accumulate", case_clip_confines_a_fill },
	{ "A stroke is a ring, and the hit test asks about the outline", case_stroke_and_hit_test },
	{ "The colour component doors", case_colour_components },
	{ "Cropping an image, and which rows the rect selects", case_image_crop },
	{ "Clipping to a gray mask, and refusing one that is not", case_clip_to_mask },
	{ "Where a drawn image lands, at three scales", case_image_draw_placement },
	{ "The Unicode glyph step Core Text's own door sits on", case_font_unichar_mapping },
	{ "The raw table doors, and the two calls agreeing", case_font_tables },
	{ "The pair adjustment a line needs: the kern table, three ways", case_font_kerning },
	{ "The pen's fraction reaches the screen: the ink moves with it, in both renderings",
	  case_subpixel_positioning },
	{ "Where the channels land, asked of all four destinations", case_destination_channel_order },
	{ "Font smoothing: sub-pixel coverage, and the destination that decides it", case_font_smoothing },
};

int main(void)
{
	size_t i;
	int passed = 0, failed = 0;
	size_t total = sizeof g_cases / sizeof g_cases[0];

	setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 0; i < total; i++) {
		if (run_in_child(g_cases[i].name, g_cases[i].fn) == 0) {
			passed++;
		} else {
			failed++;
		}
	}
	printf("drawing: %d of %d cases passed, %d failed\n", passed, passed + failed, failed);
	return failed == 0 ? 0 : 1;
}
