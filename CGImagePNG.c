/*
 * CGImagePNG — a PNG becomes a CGImage.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE DECODER IS LIBPNG (1.6.x, zlib-style licence) AND THIS FILE IS ONLY THE SEAM, because the
 * tree ALREADY CARRIES IT: `third_party/x11/libpng` is vendored and built into `.build/x11-prefix`
 * for the X stack — FreeType reads sbix colour glyphs with it — and that prefix is already on this
 * library's include and link lines, with `libpng16.so.16` already staged into the guest beside
 * `libz.so.1`. So supporting PNG added A LINK FLAG and no new dependency at all. That is the
 * measured finding, not an assumption: `png.h` and `libpng16.so.16.47.0` were found in the prefix
 * before this file was written.
 *
 * THE SIMPLIFIED API (`png_image_*`) IS THE RIGHT HALF OF LIBPNG HERE, and not only because it is
 * shorter: it is the half that is NOT deprecated, it takes a memory buffer rather than a FILE, and
 * it expands every input the format allows — palette, gray, 16-bit — into one requested layout. So
 * there is exactly ONE decode path and one place for a surprise to live.
 *
 * AND THE LAYOUT IT PRODUCES IS THIS LIBRARY'S OWN. `PNG_FORMAT_BGRA` writes B, G, R, A in memory,
 * which is what `kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little` means, so the bytes
 * arrive in the right ORDER and the only work left is the PREMULTIPLY that the format promises and
 * PNG does not supply: a PNG's samples are straight (non-premultiplied), and a CGImage in this
 * format must be premultiplied. One pass, in place, rounding to nearest.
 *
 * THE IMAGE IS HANDED TO `CGImageCreate` RATHER THAN BUILT HERE, so the chart is validated in the
 * ONE place that validates charts: if this function ever produced something the blit cannot draw,
 * that call is where it would be caught.
 *
 * WHAT IS REFUSED RATHER THAN APPROXIMATED: a `decode` array (as in `CGImageCreate`), bytes that
 * are not a PNG, and a 16-BIT PNG — libpng would downshift one to 8 bits for the layout requested
 * here, which is a silent loss of the caller's data. The refusal names the reason.
 *
 * ONE DOCUMENTED BOUNDARY: an image made this way reports DEVICE RGB, whatever the PNG's own colour
 * information says. Carrying a gAMA/iCCP chunk into a colour space is real work and belongs with an
 * ICC-aware reader, not here.
 */
#include <CoreGraphics/CGDataProvider_internal.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGImage_internal.h>
#include <CoreGraphics/CGDataProvider_internal.h>

#include <png.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* `CGDataProviderCreateWithData` takes (info, data, size), and `free` has no such shape, so the
 * buffer gets one line of glue. It runs on the LAST release of the provider, which is what makes
 * handing libpng's output straight to a provider honest rather than leaky. */
static void cg_png_free(void *info, const void *data, size_t size)
{
	(void)info;
	(void)size;
	free((void *)data);
}

CGImageRef CGImageCreateWithPNGDataProvider(CGDataProviderRef provider, const CGFloat *decode,
					    bool shouldInterpolate,
					    CGColorRenderingIntent intent)
{
	png_image image;
	png_alloc_size_t size;
	const void *bytes;
	unsigned char *pixels;
	size_t length = 0;
	size_t count;
	size_t i;
	CGDataProviderRef output;
	CGImageRef result;

	/* THE SAME RULE `CGImageCreate` FOLLOWS, for the same reason: ignoring `decode` would draw an
	 * image the caller did not ask for. */
	if (decode != NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageCreateWithPNGDataProvider does not implement the "
				"decode array yet, and ignoring it would draw an image the caller did not "
				"ask for\n");
		return NULL;
	}
	if (provider == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageCreateWithPNGDataProvider needs a data provider\n");
		return NULL;
	}
	bytes = cg_dataprovider_bytes(provider, &length);
	if (bytes == NULL || length == 0) {
		fprintf(stderr, "CG-REFUSE: CGImageCreateWithPNGDataProvider was given an empty "
				"provider\n");
		return NULL;
	}

	image.version = PNG_IMAGE_VERSION;
	image.opaque = NULL;
	if (!png_image_begin_read_from_memory(&image, bytes, length)) {
		fprintf(stderr, "CG-REFUSE: CGImageCreateWithPNGDataProvider: not a PNG this libpng "
				"can read (%s)\n", image.message);
		png_image_free(&image);
		return NULL;
	}
	/* 16-BIT IS REFUSED BEFORE THE LAYOUT IS REQUESTED: `PNG_FORMAT_BGRA` is 1 byte per channel,
	 * so asking for it would make libpng scale the caller's samples down without saying so. */
	if ((image.format & PNG_FORMAT_FLAG_LINEAR) != 0) {
		fprintf(stderr, "CG-REFUSE: CGImageCreateWithPNGDataProvider does not support 16-bit "
				"PNGs yet, and 8-bit output would lose the caller's data\n");
		png_image_free(&image);
		return NULL;
	}
	image.format = PNG_FORMAT_BGRA;
	size = PNG_IMAGE_SIZE(image);
	pixels = malloc((size_t)size);
	if (pixels == NULL) {
		png_image_free(&image);
		return NULL;
	}
	/* A NULL background means "do not composite onto anything": the alpha channel is what the
	 * caller gets, which is what an image decoder should hand back. */
	if (!png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
		fprintf(stderr, "CG-REFUSE: CGImageCreateWithPNGDataProvider could not decode the "
				"image (%s)\n", image.message);
		free(pixels);
		png_image_free(&image);
		return NULL;
	}
	png_image_free(&image);

	/* THE PREMULTIPLY PASS, IN PLACE. A PNG's samples are straight; this library's format is
	 * premultiplied. Rounded to nearest ((v * a + 127) / 255) rather than truncated, because a
	 * truncating divide darkens every semi-transparent pixel by up to one 8-bit step — which is
	 * measurable, and the probe measures it. */
	count = (size_t)image.width * (size_t)image.height;
	for (i = 0; i < count; i++) {
		unsigned char *p = pixels + i * 4u;
		unsigned int alpha = p[3];

		if (alpha == 255u) {
			continue;
		}
		if (alpha == 0u) {
			p[0] = p[1] = p[2] = 0;
			continue;
		}
		p[0] = (unsigned char)(((unsigned int)p[0] * alpha + 127u) / 255u);
		p[1] = (unsigned char)(((unsigned int)p[1] * alpha + 127u) / 255u);
		p[2] = (unsigned char)(((unsigned int)p[2] * alpha + 127u) / 255u);
	}

	/* THE BYTES BECOME A PROVIDER THAT OWNS THEM, and `CGImageCreate` retains that provider, so
	 * releasing our reference here is correct: the image keeps the pixels alive. */
	output = CGDataProviderCreateWithData(NULL, pixels, (size_t)size, cg_png_free);
	if (output == NULL) {
		free(pixels);
		return NULL;
	}
	result = CGImageCreate((size_t)image.width, (size_t)image.height, 8, 32,
			       (size_t)image.width * 4u, CGColorSpaceCreateDeviceRGB(),
			       kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, output,
			       NULL, shouldInterpolate, intent);
	CGDataProviderRelease(output);
	return result;
}

/* =================================================================================================
 * THE OTHER DIRECTION: AN IMAGE BECOMES PNG BYTES.
 * =============================================================================================== */
/* **THE SAME SIMPLIFIED API WRITES AS READS, WHICH IS WHY THIS IS THE SAME FILE.** `png_image_write_to_memory` takes
 * a buffer of 8-bit samples in a named layout, so the whole job is turning the image's own layout into one —
 * and that matrix is `cg_image_layout`'s, not a second copy of it: the encoder asks where the channels are exactly
 * as the blitter does, so a chart this library can DRAW is a chart it can write.
 *
 * **AND THE ALPHA HAS TO BE UN-PREMULTIPLIED, BECAUSE THIS LIBRARY DOES NOT STORE WHAT PNG STORES.** The decoder
 * above premultiplies for the same reason, and the inverse is rounded to nearest rather than truncated: a truncating
 * divide darkens every semi-transparent pixel by up to one 8-bit step, which is measurable and which the round-trip
 * test in `tests/drawing.c` measures.
 *
 * **AND THE OUTPUT IS ALWAYS RGBA.** An image with no alpha writes 255 into it; that costs a channel and is the
 * honest simple thing, because the alternative is a second format decision that would have to agree with the
 * decoder's. */
CFDataRef cg_image_png_data(CGImageRef image)
{
	png_image png;
	CGDataProviderRef provider;
	const void *bytes;
	size_t length = 0;
	size_t width;
	size_t height;
	size_t bytesPerRow;
	size_t stored = 0;
	int straight = 0;
	int map[4];
	unsigned char *rgba;
	size_t count;
	size_t i;
	void *memory = NULL;
	png_alloc_size_t memorySize = 0;
	CFDataRef result;

	if (image == NULL) {
		fprintf(stderr, "CG-REFUSE: cg_image_png_data needs an image\n");
		return NULL;
	}
	provider = CGImageGetDataProvider(image);
	bytes = (provider != NULL) ? cg_dataprovider_bytes(provider, &length) : NULL;
	width = CGImageGetWidth(image);
	height = CGImageGetHeight(image);
	bytesPerRow = CGImageGetBytesPerRow(image);
	if (bytes == NULL || width == 0 || height == 0) {
		fprintf(stderr, "CG-REFUSE: cg_image_png_data cannot read this image's pixels\n");
		return NULL;
	}
	if (CGImageGetBitsPerComponent(image) != 8) {
		fprintf(stderr, "CG-REFUSE: cg_image_png_data writes 8-bit samples and this image has %zu-bit "
				"ones; scaling them would lose the caller's data\n",
			CGImageGetBitsPerComponent(image));
		return NULL;
	}
	cg_image_layout(CGImageGetColorSpace(image), CGImageGetAlphaInfo(image),
			CGImageGetBitmapInfo(image) & ~0x1fu, map, (int *)&stored, &straight);
	if (stored == 0 || stored > 4) {
		fprintf(stderr, "CG-REFUSE: cg_image_png_data cannot read this image's channel layout\n");
		return NULL;
	}
	count = width * height;
	rgba = (unsigned char *)malloc(count * 4u);
	if (rgba == NULL) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		const unsigned char *p = (const unsigned char *)bytes + (i / width) * bytesPerRow
					 + (i % width) * stored;
		unsigned int a = (map[3] >= 0) ? p[map[3]] : 255u;
		unsigned int b = (map[0] >= 0) ? p[map[0]] : 0u;
		unsigned int g = (map[1] >= 0) ? p[map[1]] : 0u;
		unsigned int r = (map[2] >= 0) ? p[map[2]] : 0u;

		if (!straight && a != 255u) {
			if (a == 0u) {
				b = g = r = 0u;
			} else {
				b = (b * 255u + a / 2u) / a;
				g = (g * 255u + a / 2u) / a;
				r = (r * 255u + a / 2u) / a;
				if (b > 255u) { b = 255u; }
				if (g > 255u) { g = 255u; }
				if (r > 255u) { r = 255u; }
			}
		}
		rgba[i * 4u + 0u] = (unsigned char)r;
		rgba[i * 4u + 1u] = (unsigned char)g;
		rgba[i * 4u + 2u] = (unsigned char)b;
		rgba[i * 4u + 3u] = (unsigned char)a;
	}

	memset(&png, 0, sizeof png);
	png.version = PNG_IMAGE_VERSION;
	png.width = (png_uint_32)width;
	png.height = (png_uint_32)height;
	png.format = PNG_FORMAT_RGBA;
	/* A NULL BUFFER IS HOW LIBPNG IS ASKED HOW MUCH ROOM THE RESULT NEEDS; it answers the size and writes nothing. */
	if (png_image_write_to_memory(&png, NULL, &memorySize, 0, rgba, 0, NULL) == 0 || memorySize == 0) {
		fprintf(stderr, "CG-REFUSE: cg_image_png_data could not size the PNG (%s)\n", png.message);
		png_image_free(&png);
		free(rgba);
		return NULL;
	}
	memory = malloc((size_t)memorySize);
	if (memory == NULL) {
		png_image_free(&png);
		free(rgba);
		return NULL;
	}
	if (png_image_write_to_memory(&png, memory, &memorySize, 0, rgba, 0, NULL) == 0) {
		fprintf(stderr, "CG-REFUSE: cg_image_png_data could not encode the PNG (%s)\n", png.message);
		png_image_free(&png);
		free(memory);
		free(rgba);
		return NULL;
	}
	png_image_free(&png);
	free(rgba);
	result = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)memory, (CFIndex)memorySize);
	free(memory);
	return result;
}
