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
#include <CoreGraphics/CGFunction.h>
#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGImage.h>
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

static const struct { const char *name; int (*fn)(void); } g_cases[] = {
	{ "The colour spaces this library answers, by name", case_named_spaces },
	{ "The type-id table's two promises", case_type_ids },
	{ "A provider made from a CFDataRef gives the bytes back", case_data_provider },
	{ "A consumer takes bytes and a gradient takes a CFArray of CGColorRef",
	  case_consumer_and_gradient },
	{ "A bitmap context whose fill reaches the pixels", case_bitmap_context_fill },
	{ "A path: empty, contains, and its box", case_path },
	{ "A font made from bytes", case_font_from_provider },
	{ "A gradient painted into a context", case_gradient_into_context },
	{ "An axial shading painted through a caller's function", case_axial_shading },
	{ "Images decoded from PNG and JPEG files, then drawn", case_images_from_data },
	{ "A radial shading, and that swapping its circles swaps the ramp", case_radial_shading },
	{ "The radial degenerate cases, and the two refusals", case_radial_degenerate_and_refusals },
	{ "The radial gradient and the radial shading are one ramp", case_radial_gradient_matches_the_shading },
	{ "A clip confines a fill, and clips accumulate", case_clip_confines_a_fill },
	{ "A stroke is a ring, and the hit test asks about the outline", case_stroke_and_hit_test },
	{ "The colour component doors", case_colour_components },
	{ "Cropping an image, and which rows the rect selects", case_image_crop },
	{ "Clipping to a gray mask, and refusing one that is not", case_clip_to_mask },
	{ "Where a drawn image lands, at three scales", case_image_draw_placement },
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
