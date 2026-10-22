/*
 * eps_write.c — the EPS context's own round trip: draw, close, let Ghostscript interpret it, read the pixels.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE READER IS GHOSTSCRIPT, BECAUSE THIS TREE HAS NO POSTSCRIPT INTERPRETER.** A writer cannot prove itself
 * by parsing its own text; the only honest check is another implementation reading the file and putting ink
 * where the drawing calls said. The case therefore SKIPS with 77 when `gs` is not on the host. The PPM the
 * device writes is parsed here (P6, 8-bit, no dependencies) so the assertion is a pixel and not a substring.
 *
 * WHAT IS DRAWN: an opaque image whose four quadrants are red, green, blue and white; a filled rectangle; a
 * stroked rectangle; text (as glyph outlines); and an axial gradient. Every one is read back at a point that
 * distinguishes it from its neighbours.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGEPSContext.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGFunction.h>
#include <CoreGraphics/CGShading.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGPath.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fprintf(stderr, "\n"); \
		} \
	} while (0)

static CGImageRef fn_make_image(void)
{
	/* ROW-MAJOR, TOP ROW FIRST: red, green / blue, white. */
	static const unsigned char pixels[16] = {
		255, 0, 0, 0,    0, 255, 0, 0,
		0, 0, 255, 0,    255, 255, 255, 0
	};
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, pixels, sizeof pixels);
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGImageRef image;

	CFRelease(data);
	if (provider == NULL || space == NULL) {
		return NULL;
	}
	image = CGImageCreate(2, 2, 8, 32, 8, space,
			      kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big, provider, NULL, false,
			      kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
	return image;
}

static void fn_shading_evaluate(void *info, const CGFloat *input, CGFloat *output)
{
	(void)info;
	output[0] = 1.0 - input[0];
	output[1] = 0.0;
	output[2] = input[0];
}

static CFDataRef fn_build_eps(void)
{
	CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
	CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(data);
	CGRect box = CGRectMake(0, 0, 200, 100);
	CGContextRef ctx;
	CGImageRef image;
	CGFontRef font;

	ctx = CGEPSContextCreate(consumer, &box, NULL);
	CHECK(ctx != NULL, "CGEPSContextCreate refused");
	if (ctx == NULL) {
		CGDataConsumerRelease(consumer);
		CFRelease(data);
		return NULL;
	}
	CGEPSContextBeginPage(ctx, NULL);
	CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
	CGContextFillRect(ctx, box);

	/* A FILLED RECTANGLE. */
	CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
	CGContextFillRect(ctx, CGRectMake(10, 10, 50, 30));

	/* A STROKED RECTANGLE, four points wide. */
	CGContextSetRGBStrokeColor(ctx, 1, 0, 0, 1);
	CGContextSetLineWidth(ctx, 4);
	CGContextStrokeRect(ctx, CGRectMake(70, 10, 40, 30));

	/* TEXT AS OUTLINES. */
	font = CGFontCreateWithFontName(CFSTR("Helvetica"));
	if (font != NULL) {
		CGContextSetFont(ctx, font);
		CGContextSetFontSize(ctx, 24);
		CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
		CGContextSetTextPosition(ctx, 20, 70);
		CGContextShowText(ctx, "Hi", 2);
		CGFontRelease(font);
	}

	/* AN OPAQUE IMAGE: red green / blue white in a 20x20 square. */
	image = fn_make_image();
	if (image != NULL) {
		CGContextDrawImage(ctx, CGRectMake(120, 10, 20, 20), image);
		CGImageRelease(image);
	}

	/* AN AXIAL GRADIENT, red at the left and blue at the right. */
	{
		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		const CGFloat components[8] = { 1, 0, 0, 1,  0, 0, 1, 1 };
		const CGFloat locations[2] = { 0, 1 };
		CGGradientRef gradient = CGGradientCreateWithColorComponents(space, components, locations, 2);

		if (gradient != NULL) {
			CGContextSaveGState(ctx);
			CGContextClipToRect(ctx, CGRectMake(10, 50, 80, 20));
			CGContextDrawLinearGradient(ctx, gradient, CGPointMake(10, 60), CGPointMake(90, 60), 0);
			CGContextRestoreGState(ctx);
			CGGradientRelease(gradient);
		} else {
			CHECK(0, "the gradient fixture was refused");
		}
		CGColorSpaceRelease(space);
	}
	/* A CGShading, sampled into a PostScript function: red at its start, blue at its end. */
	{
		CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
		CGFunctionCallbacks callbacks = { 0, fn_shading_evaluate, NULL };
		const CGFloat domain[2] = { 0, 1 };
		const CGFloat range[6] = { 0, 1, 0, 1, 0, 1 };
		CGFunctionRef function = CGFunctionCreate(NULL, 1, domain, 3, range, &callbacks);
		CGShadingRef shading = function != NULL ?
			CGShadingCreateAxial(space, CGPointMake(10, 44), CGPointMake(90, 44), function, true, true) :
			NULL;

		if (shading != NULL) {
			CGContextSaveGState(ctx);
			CGContextClipToRect(ctx, CGRectMake(10, 42, 80, 5));
			CGContextDrawShading(ctx, shading);
			CGContextRestoreGState(ctx);
			CGShadingRelease(shading);
		} else {
			CHECK(0, "the CGShading fixture was refused");
		}
		if (function != NULL) {
			CGFunctionRelease(function);
		}
		CGColorSpaceRelease(space);
	}

	CGEPSContextEndPage(ctx);
	CGEPSContextClose(ctx);
	CGContextRelease(ctx);
	CGDataConsumerRelease(consumer);
	return data;
}

struct ppm {
	unsigned char *data;
	size_t width;
	size_t height;
};

/* A PPM TOKEN, WITH THE COMMENTS AND WHITESPACE GHOSTSCRIPT WRITES AFTER THE MAGIC. */
static int fn_ppm_token(FILE *file, char *out, size_t cap)
{
	size_t length = 0;
	int c;

	do {
		c = fgetc(file);
		if (c == '#') {
			while ((c = fgetc(file)) != EOF && c != '\n') {
				/* a comment line */
			}
		}
	} while (c != EOF && (c == ' ' || c == '\t' || c == '\r' || c == '\n'));
	if (c == EOF) {
		return 0;
	}
	while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
		if (length + 1 < cap) {
			out[length++] = (char)c;
		}
		c = fgetc(file);
	}
	out[length] = '\0';
	return 1;
}

static int fn_read_ppm(const char *path, struct ppm *out)
{
	FILE *file = fopen(path, "rb");
	char token[32];
	unsigned width = 0;
	unsigned height = 0;
	unsigned maxval = 0;

	if (file == NULL) {
		return 0;
	}
	if (!fn_ppm_token(file, token, sizeof token) || strcmp(token, "P6") != 0) {
		fclose(file);
		return 0;
	}
	if (!fn_ppm_token(file, token, sizeof token)) {
		fclose(file);
		return 0;
	}
	width = (unsigned)strtoul(token, NULL, 10);
	if (!fn_ppm_token(file, token, sizeof token)) {
		fclose(file);
		return 0;
	}
	height = (unsigned)strtoul(token, NULL, 10);
	if (!fn_ppm_token(file, token, sizeof token)) {
		fclose(file);
		return 0;
	}
	maxval = (unsigned)strtoul(token, NULL, 10);
	if (width == 0 || height == 0 || maxval != 255) {
		fclose(file);
		return 0;
	}
	out->width = width;
	out->height = height;
	out->data = malloc((size_t)width * height * 3);
	if (out->data == NULL) {
		fclose(file);
		return 0;
	}
	if (fread(out->data, 3, (size_t)width * height, file) != (size_t)width * height) {
		free(out->data);
		out->data = NULL;
		fclose(file);
		return 0;
	}
	fclose(file);
	return 1;
}

/* PPM ROWS ARE TOP-DOWN; the drawing's points are bottom-up, like PostScript's. */
static void fn_pixel(const struct ppm *image, int x, int y_from_bottom, unsigned char rgb[3])
{
	int y = (int)image->height - 1 - y_from_bottom;

	if (x < 0 || y < 0 || (size_t)x >= image->width || (size_t)y >= image->height) {
		rgb[0] = rgb[1] = rgb[2] = 0;
		return;
	}
	memcpy(rgb, image->data + ((size_t)y * image->width + (size_t)x) * 3, 3);
}

static int fn_near(const unsigned char rgb[3], int r, int g, int b, int tolerance)
{
	return abs((int)rgb[0] - r) <= tolerance && abs((int)rgb[1] - g) <= tolerance &&
	       abs((int)rgb[2] - b) <= tolerance;
}

int main(void)
{
	CFDataRef eps = fn_build_eps();
	const char *eps_path = "/tmp/agora-eps-out.eps";
	const char *ppm_path = "/tmp/agora-eps-out.ppm";
	FILE *file;
	struct ppm image;
	char command[512];
	int status;

	if (eps == NULL) {
		return 1;
	}
	file = fopen(eps_path, "wb");
	if (file == NULL) {
		CFRelease(eps);
		return 1;
	}
	fwrite(CFDataGetBytePtr(eps), 1, (size_t)CFDataGetLength(eps), file);
	fclose(file);

	{
		const char *bytes = (const char *)CFDataGetBytePtr(eps);
		size_t length = (size_t)CFDataGetLength(eps);

		CHECK(length > 100, "the EPS is too short to be a document");
		CHECK(strncmp(bytes, "%!PS-Adobe-3.0 EPSF-3.0", strlen("%!PS-Adobe-3.0 EPSF-3.0")) == 0, "the EPS header is missing");
		CHECK(strstr(bytes, "%%BoundingBox: 0 0 200 100") != NULL, "the bounding box is not 200x100");
		CHECK(strstr(bytes, "%%EOF") != NULL, "the EPS trailer is missing");
	}

	if (system("gs --version >/dev/null 2>&1") != 0) {
		fprintf(stderr, "SKIP: Ghostscript is not installed\n");
		CFRelease(eps);
		return 77;
	}
	snprintf(command, sizeof command,
		 "gs -q -dNOPAUSE -dBATCH -dSAFER -sDEVICE=ppmraw -dEPSCrop -r72 -sOutputFile=%s %s",
		 ppm_path, eps_path);
	status = system(command);
	CHECK(status == 0, "Ghostscript could not interpret the EPS (status %d)", status);
	if (status != 0) {
		CFRelease(eps);
		return 1;
	}
	if (!fn_read_ppm(ppm_path, &image)) {
		fprintf(stderr, "FAIL: Ghostscript wrote no readable PPM\n");
		CFRelease(eps);
		return 1;
	}
	CHECK(image.width == 200 && image.height == 100, "Ghostscript produced %zux%zu, not 200x100",
	      image.width, image.height);

	{
		unsigned char rgb[3];

		fn_pixel(&image, 20, 20, rgb);
		CHECK(fn_near(rgb, 0, 0, 255, 8), "the blue rectangle is (%d,%d,%d)", rgb[0], rgb[1], rgb[2]);
		fn_pixel(&image, 35, 80, rgb);
		CHECK(fn_near(rgb, 0, 0, 0, 70), "the text did not put ink at (35,80): (%d,%d,%d)", rgb[0],
		      rgb[1], rgb[2]);
		fn_pixel(&image, 108, 25, rgb);
		CHECK(fn_near(rgb, 255, 0, 0, 8), "the stroked border is (%d,%d,%d)", rgb[0], rgb[1], rgb[2]);
		fn_pixel(&image, 125, 25, rgb);
		CHECK(fn_near(rgb, 255, 0, 0, 12) || fn_near(rgb, 0, 255, 0, 12),
		      "the image's top half is (%d,%d,%d), not red/green", rgb[0], rgb[1], rgb[2]);
		fn_pixel(&image, 125, 15, rgb);
		CHECK(fn_near(rgb, 0, 0, 255, 12) || fn_near(rgb, 255, 255, 255, 12),
		      "the image's bottom half is (%d,%d,%d), not blue/white", rgb[0], rgb[1], rgb[2]);
		fn_pixel(&image, 15, 60, rgb);
		CHECK(rgb[0] > 200 && rgb[2] < 60, "the gradient's left end is (%d,%d,%d)", rgb[0], rgb[1],
		      rgb[2]);
		fn_pixel(&image, 85, 60, rgb);
		CHECK(rgb[2] > 200 && rgb[0] < 60, "the gradient's right end is (%d,%d,%d)", rgb[0], rgb[1],
		      rgb[2]);
		fn_pixel(&image, 15, 44, rgb);
		CHECK(rgb[0] > 200 && rgb[2] < 60, "the shading's left end is (%d,%d,%d)", rgb[0], rgb[1],
		      rgb[2]);
		fn_pixel(&image, 85, 44, rgb);
		CHECK(rgb[2] > 200 && rgb[0] < 60, "the shading's right end is (%d,%d,%d)", rgb[0], rgb[1],
		      rgb[2]);
	}
	free(image.data);
	CFRelease(eps);
	if (failures != 0) {
		fprintf(stderr, "%d EPS check(s) failed\n", failures);
	}
	return failures == 0 ? 0 : 1;
}
