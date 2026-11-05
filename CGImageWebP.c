/*
 * CGImageWebP.c — a WebP still or animation becomes images, through libwebp.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **WEBP IS TWO FORMATS UNDER ONE SIGNATURE**: a still image (lossy VP8 or lossless VP8L) and, since 2013, an animation
 * (the ANMF chunks of an extended file). libwebp draws both — `WebPDecodeBGRA` for a still picture and the demuxer's
 * `WebPAnimDecoder` for the frames — and it is a system library here (`libwebp-dev`), so this file is the seam between
 * the registry's "frame N of M" and libwebp's own iteration.
 *
 * **THE PIXEL ORDER IS ALREADY THIS LIBRARY'S.** `WebPDecodeBGRA` writes B, G, R, A in memory, which is exactly the
 * straight layout `cg_image_from_bgra` premultiplies — so the still path copies nothing and the animation path hands the
 * demuxer's own buffer over the same way, once per frame.
 *
 * A frame's DELAY is the difference between its timestamp and the next one, which is what the demuxer reports and what an
 * animation needs; the last frame's delay is the loop's own remainder, which the demuxer also gives.
 *
 * The build without libwebp compiles this file to a decoder whose probe says NO, so `-CopyTypeIdentifiers` simply does
 * not offer WebP.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AGORA_HAVE_WEBP

#include <webp/decode.h>
#include <webp/demux.h>

static bool fn_webp_probe(const unsigned char *b, size_t n)
{
	return n >= 16 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0;
}

/* **THE DEMUXER IS THE ONE DOOR THAT KNOWS HOW MANY FRAMES A FILE HAS**, and it is cheap to build: it parses the chunk
 * headers and nothing else. A still image reports one frame, so this answers for both kinds. */
static size_t fn_webp_count(const unsigned char *b, size_t n)
{
	WebPData data;
	WebPAnimDecoderOptions options;
	WebPAnimDecoder *decoder;
	WebPAnimInfo info;
	size_t count = 1;

	if (!fn_webp_probe(b, n)) {
		return 0;
	}
	data.bytes = b;
	data.size = n;
	if (!WebPAnimDecoderOptionsInit(&options)) {
		return 1;
	}
	options.color_mode = MODE_BGRA;
	options.use_threads = 0;
	decoder = WebPAnimDecoderNew(&data, &options);
	if (decoder == NULL) {
		return 1;	/* a still image the demuxer declines still decodes through WebPDecodeBGRA */
	}
	if (WebPAnimDecoderGetInfo(decoder, &info)) {
		count = (size_t)info.frame_count;
	}
	WebPAnimDecoderDelete(decoder);
	return (count == 0) ? 1 : count;
}

static void fn_webp_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	int w = 0;
	int h = 0;

	(void)index;
	*width = 0;
	*height = 0;
	if (WebPGetInfo(b, n, &w, &h)) {
		*width = (size_t)w;
		*height = (size_t)h;
	}
}

static CGImageRef fn_webp_still(const unsigned char *b, size_t n, bool *has_alpha)
{
	WebPBitstreamFeatures features;
	uint8_t *pixels;
	int w = 0;
	int h = 0;
	CGImageRef image;

	memset(&features, 0, sizeof features);
	*has_alpha = false;
	if (WebPGetFeatures(b, n, &features) == VP8_STATUS_OK) {
		*has_alpha = features.has_alpha != 0;
	}
	/* WEBPDECODE IS THE STRAIGHT ONE: WebPDecodeBGRA writes B, G, R, A — this library's layout — and hands back a
	 * buffer the caller frees. */
	pixels = WebPDecodeBGRA(b, n, &w, &h);
	if (pixels == NULL || w <= 0 || h <= 0) {
		free(pixels);
		return NULL;
	}
	image = cg_image_from_bgra(pixels, (size_t)w, (size_t)h, (size_t)w * 4u, *has_alpha);
	WebPFree(pixels);
	return image;
}

static CGImageRef fn_webp_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	WebPData data;
	WebPAnimDecoderOptions options;
	WebPAnimDecoder *decoder;
	WebPAnimInfo info;
	uint8_t *buffer = NULL;
	int timestamp = 0;
	int previous = 0;
	size_t frame = 0;
	CGImageRef image = NULL;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_webp_probe(b, n)) {
		return NULL;
	}
	if (fn_webp_count(b, n) <= 1 && index == 0) {
		bool has_alpha = false;

		return fn_webp_still(b, n, &has_alpha);
	}
	data.bytes = b;
	data.size = n;
	if (!WebPAnimDecoderOptionsInit(&options)) {
		return NULL;
	}
	options.color_mode = MODE_BGRA;
	options.use_threads = 0;
	decoder = WebPAnimDecoderNew(&data, &options);
	if (decoder == NULL) {
		fprintf(stderr, "CG-REFUSE: libwebp could not open this animation\n");
		return NULL;
	}
	if (!WebPAnimDecoderGetInfo(decoder, &info)) {
		WebPAnimDecoderDelete(decoder);
		return NULL;
	}
	/* **THE DEMUXER HANDS OUT FRAMES IN ORDER AND CANNOT SEEK BACK**, so frame n means iterating n times — the same
	 * shape a GIF's canvas walk has, and for the same reason: a frame is only meaningful after the ones before it. */
	while (WebPAnimDecoderHasMoreFrames(decoder)) {
		if (!WebPAnimDecoderGetNext(decoder, &buffer, &timestamp)) {
			break;
		}
		if (frame == index) {
			image = cg_image_from_bgra(buffer, (size_t)info.canvas_width, (size_t)info.canvas_height,
						   (size_t)info.canvas_width * 4u, true);
			if (delay != NULL) {
				*delay = (double)(timestamp - previous) / 1000.0;
			}
			break;
		}
		previous = timestamp;
		frame++;
	}
	WebPAnimDecoderDelete(decoder);
	return image;
}

const cg_image_decoder cg_decoder_webp = {
	"org.webmproject.webp", "webp", fn_webp_probe, fn_webp_count, fn_webp_decode, fn_webp_size
};

#else /* !AGORA_HAVE_WEBP */

static bool fn_webp_absent(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

const cg_image_decoder cg_decoder_webp = {
	"org.webmproject.webp", "webp", fn_webp_absent, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_WEBP */
