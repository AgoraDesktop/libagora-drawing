/*
 * image_source.c — the decoder registry: what it claims, and what it draws.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE FIXTURES ARE BUILT HERE, BYTE BY BYTE, INCLUDING THE GIF.** A checked-in picture would be a black box whose bytes
 * nothing in this file could vary, and the two things worth varying — which format the registry picks, and which FRAME it
 * hands back — are exactly what the bytes decide. The formats chosen are the ones whose decoders are this tree's own
 * (BMP, TGA, GIF); TIFF and WebP go through system libraries and are covered by their own probe runs, and the refusal for
 * an unclaimed file is checked too.
 *
 * **AND THE TYPE LIST IS CHECKED AGAINST THE FACT**, not written down twice: `CGImageSourceCopyTypeIdentifiers` is what a
 * bundle's document types should agree with, so the test asks it for the formats whose fixtures it just decoded.
 */
#include <CoreGraphics/CGImageSource.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGDataProvider.h>

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

/* A B, G, R, A pixel of a decoded image, read through the provider the decoder produced. */
static void pixel_of(CGImageRef image, size_t x, size_t y, unsigned char out[4])
{
	CGDataProviderRef provider = CGImageGetDataProvider(image);
	size_t length = 0;
	const unsigned char *bytes = CGDataProviderCopyData(provider) != NULL ? NULL : NULL;
	CFDataRef data;

	(void)bytes;
	(void)length;
	data = CGDataProviderCopyData(provider);
	if (data == NULL) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}
	{
		const unsigned char *p = CFDataGetBytePtr(data) + (y * CGImageGetBytesPerRow(image)) + x * 4u;

		memcpy(out, p, 4);
	}
	CFRelease(data);
}

/* ---- a 2x2 BMP, bottom-up, 24-bit ---------------------------------------------------------------- */
static CFDataRef build_bmp(void)
{
	unsigned char bytes[70];
	size_t at = 14;
	unsigned char *pixels = bytes + 54;
	unsigned i;

	memset(bytes, 0, sizeof bytes);
	bytes[0] = 'B';
	bytes[1] = 'M';
	/* file size 54 + 16 = 70, pixel offset 54 */
	bytes[2] = 70; bytes[6] = 54; bytes[10] = 54;
	bytes[14] = 40;			/* header size */
	bytes[18] = 2;			/* width */
	bytes[22] = 2;			/* height, bottom-up */
	bytes[26] = 1;			/* planes */
	bytes[28] = 24;			/* bits per pixel */
	bytes[34] = 16;			/* image size */
	/* bottom row: red, green; top row: blue, white — B, G, R in the file */
	pixels[0] = 0; pixels[1] = 0; pixels[2] = 255;		/* red */
	pixels[3] = 0; pixels[4] = 255; pixels[5] = 0;		/* green */
	pixels[8] = 255; pixels[9] = 0; pixels[10] = 0;		/* blue */
	pixels[11] = 255; pixels[12] = 255; pixels[13] = 255;	/* white */
	(void)at;
	for (i = 0; i < 70; i++) {
		/* nothing to fix up: the offsets are written above */
	}
	return CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)sizeof bytes);
}

/* ---- a 2x2 TGA, top-origin, 24-bit --------------------------------------------------------------- */
static CFDataRef build_tga(void)
{
	unsigned char bytes[36];
	unsigned char *pixels = bytes + 18;

	memset(bytes, 0, sizeof bytes);
	bytes[2] = 2;			/* uncompressed true colour */
	bytes[12] = 2;			/* width */
	bytes[14] = 2;			/* height */
	bytes[16] = 24;
	bytes[17] = 0x20;		/* top origin */
	/* top row: red, green; bottom row: blue, white — B, G, R */
	pixels[0] = 0; pixels[1] = 0; pixels[2] = 255;
	pixels[3] = 0; pixels[4] = 255; pixels[5] = 0;
	pixels[6] = 255; pixels[7] = 0; pixels[8] = 0;
	pixels[9] = 255; pixels[10] = 255; pixels[11] = 255;
	return CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)sizeof bytes);
}

/* ---- a 2x2 GIF with two frames, red then blue, 0.1 s apart ---------------------------------------- */

static size_t put_codes(unsigned char *out, const unsigned *codes, size_t count)
{
	size_t bit = 0;
	size_t i;

	memset(out, 0, 32);
	for (i = 0; i < count; i++) {
		unsigned shift;

		for (shift = 0; shift < 3; shift++) {
			if ((codes[i] >> shift) & 1u) {
				out[bit / 8] |= (unsigned char)(1u << (bit % 8));
			}
			bit++;
		}
	}
	return (bit + 7) / 8;
}

static CFDataRef build_gif(void)
{
	unsigned char bytes[160];
	size_t at = 0;
	unsigned char lzw[32];
	size_t lzw_length;
	/* CLEAR, 0, 0, CLEAR, 0, 0, END: eight literals all at the initial three-bit width, which is why the CLEAR is
	 * there twice — the dictionary would have widened the code after the third. */
	/* **THE PALETTE'S ENTRIES ARE R, G, B IN FILE ORDER**, so index 1 (255,0,0) is red and index 2 (0,0,255) is blue;
	 * the streams name those indices. */
	static const unsigned red_codes[] = { 4, 1, 1, 4, 1, 1, 5 };
	static const unsigned blue_codes[] = { 4, 2, 2, 4, 2, 2, 5 };

	memset(bytes, 0, sizeof bytes);
	memcpy(bytes, "GIF89a", 6);
	bytes[6] = 2; bytes[7] = 0;		/* width 2 */
	bytes[8] = 2; bytes[9] = 0;		/* height 2 */
	bytes[10] = 0x81;			/* global table, 4 entries */
	bytes[11] = 0; bytes[12] = 0;
	at = 13;
	/* the global table: index 0 black, 1 blue, 2 red, 3 white */
	bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 255; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 255;
	bytes[at++] = 255; bytes[at++] = 255; bytes[at++] = 255;
	/* frame 1: red, delay 10 centiseconds */
	bytes[at++] = 0x21; bytes[at++] = 0xf9; bytes[at++] = 4; bytes[at++] = 0; bytes[at++] = 10;
	bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 0x2c; bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 2; bytes[at++] = 0; bytes[at++] = 2; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 2;			/* LZW minimum code size */
	lzw_length = put_codes(lzw, red_codes, sizeof red_codes / sizeof red_codes[0]);
	bytes[at++] = (unsigned char)lzw_length;
	memcpy(bytes + at, lzw, lzw_length);
	at += lzw_length;
	bytes[at++] = 0;			/* end of the sub-block chain */
	/* frame 2: blue, delay 10 centiseconds */
	bytes[at++] = 0x21; bytes[at++] = 0xf9; bytes[at++] = 4; bytes[at++] = 0; bytes[at++] = 10;
	bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 0x2c; bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 2; bytes[at++] = 0; bytes[at++] = 2; bytes[at++] = 0; bytes[at++] = 0;
	bytes[at++] = 2;
	lzw_length = put_codes(lzw, blue_codes, sizeof blue_codes / sizeof blue_codes[0]);
	bytes[at++] = (unsigned char)lzw_length;
	memcpy(bytes + at, lzw, lzw_length);
	at += lzw_length;
	bytes[at++] = 0;
	bytes[at++] = 0x3b;			/* trailer */
	return CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)at);
}

/* ---- the checks ---------------------------------------------------------------------------------- */

static CGImageSourceRef source_with(CFDataRef data, const char *what)
{
	CGImageSourceRef source = CGImageSourceCreateWithData(data, NULL);

	CHECK(source != NULL, "%s did not open", what);
	return source;
}

static void type_is(CGImageSourceRef source, const char *expected, const char *what)
{
	CFStringRef type = CGImageSourceGetType(source);
	char actual[64] = "?";

	CHECK(type != NULL, "%s has no type identifier", what);
	if (type != NULL) {
		CFStringGetCString(type, actual, sizeof actual, kCFStringEncodingUTF8);
		CHECK(strcmp(actual, expected) == 0, "%s is `%s`, expected `%s`", what, actual, expected);
		CFRelease(type);
	}
}

static void check_bmp(void)
{
	CFDataRef data = build_bmp();
	CGImageSourceRef source = source_with(data, "a BMP");
	CGImageRef image;
	unsigned char px[4];

	if (source == NULL) {
		CFRelease(data);
		return;
	}
	type_is(source, "com.microsoft.bmp", "a BMP");
	CHECK(CGImageSourceGetCount(source) == 1, "a BMP holds one image");
	image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
	CHECK(image != NULL, "a BMP's image did not decode");
	if (image != NULL) {
		CHECK(CGImageGetWidth(image) == 2 && CGImageGetHeight(image) == 2, "a BMP is 2x2");
		/* THE FILE'S BOTTOM ROW IS ITS IMAGE'S BOTTOM ROW, so the decoded top-left is the file's LAST row. */
		pixel_of(image, 1, 0, px);
		CHECK(px[0] > 200 && px[1] > 200 && px[2] > 200, "a BMP's top-right is white, got B=%u G=%u R=%u", px[0],
		      px[1], px[2]);
		pixel_of(image, 0, 1, px);
		CHECK(px[0] < 60 && px[1] < 60 && px[2] > 200, "a BMP's bottom-left is red, got B=%u G=%u R=%u", px[0], px[1],
		      px[2]);
		CGImageRelease(image);
	}
	CGImageSourceRelease(source);
	CFRelease(data);
}

static void check_tga(void)
{
	CFDataRef data = build_tga();
	CGImageSourceRef source = source_with(data, "a TGA");
	CGImageRef image;
	unsigned char px[4];

	if (source == NULL) {
		CFRelease(data);
		return;
	}
	type_is(source, "com.truevision.tga-image", "a TGA");
	image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
	CHECK(image != NULL, "a TGA's image did not decode");
	if (image != NULL) {
		pixel_of(image, 0, 0, px);
		CHECK(px[0] < 60 && px[1] < 60 && px[2] > 200, "a TGA's top-left is red, got B=%u G=%u R=%u", px[0], px[1],
		      px[2]);
		pixel_of(image, 2 - 1, 2 - 1, px);
		CHECK(px[0] > 200 && px[1] > 200 && px[2] > 200, "a TGA's bottom-right is white");
		CGImageRelease(image);
	}
	CGImageSourceRelease(source);
	CFRelease(data);
}

static void check_gif(void)
{
	CFDataRef data = build_gif();
	CGImageSourceRef source = source_with(data, "a GIF");
	CGImageRef first;
	CGImageRef second;
	unsigned char px[4];
	CFDictionaryRef properties;

	if (source == NULL) {
		CFRelease(data);
		return;
	}
	type_is(source, "com.compuserve.gif", "a GIF");
	CHECK(CGImageSourceGetCount(source) == 2, "the GIF holds %zu image(s), expected 2",
	      CGImageSourceGetCount(source));
	first = CGImageSourceCreateImageAtIndex(source, 0, NULL);
	second = CGImageSourceCreateImageAtIndex(source, 1, NULL);
	CHECK(first != NULL && second != NULL, "both GIF frames decoded");
	if (first != NULL) {
		pixel_of(first, 0, 0, px);
		CHECK(px[2] > 200 && px[0] < 60, "the GIF's first frame is red, got B=%u G=%u R=%u", px[0], px[1], px[2]);
		CGImageRelease(first);
	}
	if (second != NULL) {
		pixel_of(second, 0, 0, px);
		CHECK(px[0] > 200 && px[2] < 60, "the GIF's second frame is blue, got B=%u G=%u R=%u", px[0], px[1], px[2]);
		CGImageRelease(second);
	}
	/* THE DELAY IS THE FRAME'S, and the second frame's is the one a caller animating from here reads. */
	properties = CGImageSourceCopyPropertiesAtIndex(source, 1, NULL);
	CHECK(properties != NULL, "the GIF's frame 1 has no properties");
	if (properties != NULL) {
		CFDictionaryRef gif = CFDictionaryGetValue(properties, kCGImagePropertyGIFDictionary);
		CFNumberRef seconds = (gif != NULL) ? CFDictionaryGetValue(gif, kCGImagePropertyGIFDelayTime) : NULL;
		double delay = 0.0;

		CHECK(seconds != NULL && CFNumberGetValue(seconds, kCFNumberDoubleType, &delay),
		      "the GIF's frame 1 has no delay");
		CHECK(delay > 0.09 && delay < 0.11, "the GIF's frame 1 delay is %.3f s, expected 0.1", delay);
		CFRelease(properties);
	}
	CGImageSourceRelease(source);
	CFRelease(data);
}

static void check_refusal(void)
{
	static const char text[] = "this is not an image\n";
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)text, (CFIndex)strlen(text));
	CGImageSourceRef source = CGImageSourceCreateWithData(data, NULL);

	CHECK(source == NULL, "a text file opened as an image source");
	CFRelease(data);
}

/* ---- THE FORMATS WHOSE FIXTURES ARE CHECKED IN -----------------------------------------------------
 *
 * **A JP2, AN AVIF, A HEIC AND AN EXR CANNOT BE BUILT BYTE BY BYTE IN A TEST THE WAY A BMP CAN** — a JPEG 2000 codestream is
 * an arithmetic-coded image, an AVIF is an AV1 frame, a HEIC is an HEVC frame, and an EXR is a chunked float image, each
 * produced by an encoder rather than assembled. So those four are checked in: 40x30, solid rgb(200,40,40), a few hundred
 * bytes each (the EXR is 8 KB because it is float), and the test decodes each one and checks the colour it was made with.
 * A fixture whose pixels were not checked would only prove that a decoder ran.
 */
static CFDataRef load_fixture(const char *name)
{
	char path[512];
	FILE *file;
	long size;
	unsigned char *bytes;
	CFDataRef data;

	snprintf(path, sizeof path, "%s/%s", AGORA_FIXTURE_DIR, name);
	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "FAIL: the fixture %s is missing\n", path);
		failures++;
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	bytes = malloc((size_t)size);
	if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
		fclose(file);
		free(bytes);
		failures++;
		return NULL;
	}
	fclose(file);
	data = CFDataCreate(kCFAllocatorDefault, bytes, (CFIndex)size);
	free(bytes);
	return data;
}

static void check_fixture(const char *name, const char *uti)
{
	CFDataRef data = load_fixture(name);
	CGImageSourceRef source;
	CGImageRef image;
	unsigned char px[4];

	if (data == NULL) {
		return;
	}
	source = CGImageSourceCreateWithData(data, NULL);
	CHECK(source != NULL, "%s did not open", name);
	if (source != NULL) {
		type_is(source, uti, name);
		image = CGImageSourceCreateImageAtIndex(source, 0, NULL);
		CHECK(image != NULL, "%s did not decode", name);
		if (image != NULL) {
			CHECK(CGImageGetWidth(image) == 40 && CGImageGetHeight(image) == 30, "%s is not 40x30", name);
			pixel_of(image, 20, 15, px);
			/* THE FIXTURE IS SOLID rgb(200,40,40); a lossy codec moves a channel by a few steps, which is why this
			 * is a range rather than an equality. */
			CHECK(px[2] > 170 && px[2] < 230 && px[1] < 80 && px[0] < 80,
			      "%s's centre is not red, got B=%u G=%u R=%u", name, px[0], px[1], px[2]);
			CGImageRelease(image);
		}
		CGImageSourceRelease(source);
	}
	CFRelease(data);
}

/* THE LIST A BUNDLE'S DOCUMENT TYPES SHOULD AGREE WITH: every format whose fixture above decoded has to be in it. */
static void check_type_list(void)
{
	static const char *const expected[] = { "public.png", "public.jpeg", "com.compuserve.gif", "com.microsoft.bmp",
					       "com.microsoft.ico", "com.apple.icns", "com.truevision.tga-image",
					       "public.jpeg-2000", "public.avif", "public.heic", "com.ilm.openexr-image" };
	CFArrayRef utis = CGImageSourceCopyTypeIdentifiers();
	size_t i;

	CHECK(utis != NULL, "the registry has no type list");
	if (utis == NULL) {
		return;
	}
	for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
		CFStringRef wanted = CFStringCreateWithCString(kCFAllocatorDefault, expected[i], kCFStringEncodingUTF8);
		CFIndex at;
		int found = 0;

		for (at = 0; at < CFArrayGetCount(utis); at++) {
			CFStringRef candidate = CFArrayGetValueAtIndex(utis, at);

			if (CFStringCompare(candidate, wanted, 0) == kCFCompareEqualTo) {
				found = 1;
				break;
			}
		}
		CHECK(found, "the type list does not contain %s", expected[i]);
		CFRelease(wanted);
	}
	CFRelease(utis);
}

int main(void)
{
	check_bmp();
	check_tga();
	check_gif();
	check_fixture("red.jp2", "public.jpeg-2000");
	check_fixture("red.avif", "public.avif");
	check_fixture("red.heic", "public.heic");
	check_fixture("red.exr", "com.ilm.openexr-image");
	check_refusal();
	check_type_list();
	if (failures != 0) {
		fprintf(stderr, "%d image-source check(s) failed\n", failures);
		return 1;
	}
	printf("image_source: all checks passed\n");
	return 0;
}
