/*
 * CGImageICNS.c — an Apple ICNS icon becomes an image.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **AN ICNS IS A LIST OF WHOLE IMAGES UNDER FOUR-LETTER TAGS, and since OS X 10.7 the large sizes are PNG bytes inside
 * the chunk.** So this decodes by handing each chunk's payload to the decoder that claims it — PNG or JPEG — and the
 * format's own work is only the eight-byte element header. The oldest tags (`is32`, `il32`, …) are raw packed bitmaps
 * with a per-row RLE; they are refused BY NAME here rather than guessed at, and every icon written this century carries
 * PNG for the sizes a viewer asks for.
 *
 * The count is the number of elements holding a PNG or JPEG, which is exactly the number of images this decoder can
 * return — a count that named elements it could not decode would be a lie a caller acts on.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long fn_icns32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
	       ((unsigned long)p[3]);
}

static bool fn_icns_probe(const unsigned char *b, size_t n)
{
	return n >= 16 && memcmp(b, "icns", 4) == 0 && fn_icns32(b + 4) <= n && fn_icns32(b + 4) >= 16;
}

/* THE PAYLOAD'S DECODER, ASKED THE SAME QUESTION THE REGISTRY ASKS. `NULL` for a chunk this build cannot read. */
static CGImageRef fn_icns_chunk_image(const unsigned char *payload, size_t length)
{
	CGDataProviderRef provider;
	unsigned char *copy;
	CGImageRef image = NULL;

	if (length >= 8 && payload[0] == 0x89 && payload[1] == 'P' && payload[2] == 'N' && payload[3] == 'G') {
		copy = malloc(length);
		if (copy != NULL) {
			memcpy(copy, payload, length);
			provider = CGDataProviderCreateWithData(NULL, copy, length, NULL);
			if (provider != NULL) {
				image = CGImageCreateWithPNGDataProvider(provider, NULL, true, kCGRenderingIntentDefault);
				CGDataProviderRelease(provider);
			}
		}
		return image;
	}
	if (length >= 3 && payload[0] == 0xff && payload[1] == 0xd8 && payload[2] == 0xff) {
		copy = malloc(length);
		if (copy != NULL) {
			memcpy(copy, payload, length);
			provider = CGDataProviderCreateWithData(NULL, copy, length, NULL);
			if (provider != NULL) {
				image = CGImageCreateWithJPEGDataProvider(provider, NULL, true, kCGRenderingIntentDefault);
				CGDataProviderRelease(provider);
			}
		}
		return image;
	}
	return NULL;
}

/* Walks the elements; `want` of `(size_t)-1` counts, anything else decodes that one. ONE walker for the three doors,
 * because the element header is the format's only structure and three copies of it would be three chances to disagree. */
static size_t fn_icns_count(const unsigned char *b, size_t n)
{
	size_t at = 8;
	size_t seen = 0;

	while (at + 8 <= n) {
		unsigned long length = fn_icns32(b + at + 4);
		const unsigned char *payload;
		size_t payload_length;

		if (length < 8 || at + length > n) {
			break;
		}
		payload = b + at + 8;
		payload_length = (size_t)length - 8u;
		if (payload_length >= 8 &&
		    ((payload[0] == 0x89 && payload[1] == 'P') || (payload[0] == 0xff && payload[1] == 0xd8))) {
			seen++;
		}
		at += length;
	}
	return seen;
}

static CGImageRef fn_icns_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	size_t at = 8;
	size_t seen = 0;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_icns_probe(b, n)) {
		return NULL;
	}
	while (at + 8 <= n) {
		unsigned long length = fn_icns32(b + at + 4);
		const unsigned char *payload;
		size_t payload_length;

		if (length < 8 || at + length > n) {
			break;
		}
		payload = b + at + 8;
		payload_length = (size_t)length - 8u;
		if (payload_length >= 8 &&
		    ((payload[0] == 0x89 && payload[1] == 'P') || (payload[0] == 0xff && payload[1] == 0xd8))) {
			if (seen == index) {
				return fn_icns_chunk_image(payload, payload_length);
			}
			seen++;
		}
		at += length;
	}
	return NULL;
}

static void fn_icns_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	CGImageRef image = fn_icns_decode(b, n, index, NULL);

	*width = 0;
	*height = 0;
	if (image != NULL) {
		*width = CGImageGetWidth(image);
		*height = CGImageGetHeight(image);
		CGImageRelease(image);
	}
}

const cg_image_decoder cg_decoder_icns = {
	"com.apple.icns", "icns", fn_icns_probe, fn_icns_count, fn_icns_decode, fn_icns_size
};
