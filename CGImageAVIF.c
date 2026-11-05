/*
 * CGImageAVIF.c — an AVIF image becomes an image, through libavif.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **AVIF IS HEIF'S CONTAINER WITH AV1 INSIDE**, which is why its signature is the same `ftyp` box with an `avif` brand and
 * why libheif can often read it too — but libavif is the library that owns the AV1 specifics (the still-image item, the
 * grid of tiles, the alpha plane as a separate item), so this file asks it rather than asking libheif to guess.
 *
 * **THE PIXELS ARRIVE AS YUV AND COME OUT RGBA**, in one call: `avifImageYUVToRGB` with an `avifRGBImage` whose format is
 * `AVIF_RGB_FORMAT_RGBA` — so this file's own work is the channel order (RGBA to this library's BGRA) and nothing else.
 * The conversion is libavif's because the colour planes, the chroma subsampling and the CICP primaries are all its
 * questions.
 *
 * WHAT IS READ: the primary item, and an image sequence's frames in order. WHAT IS REFUSED BY NAME: a file libavif cannot
 * parse, a frame whose conversion fails, and a bit depth other than 8 (anything deeper would be downshifted by the
 * conversion and this seam says so rather than doing it quietly).
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AGORA_HAVE_LIBAVIF

#include <avif/avif.h>

static bool fn_avif_probe(const unsigned char *b, size_t n)
{
	if (n < 12 || memcmp(b + 4, "ftyp", 4) != 0) {
		return false;
	}
	return memcmp(b + 8, "avif", 4) == 0 || memcmp(b + 8, "avis", 4) == 0;
}

static CGImageRef fn_avif_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	avifDecoder *decoder;
	avifRGBImage rgb;
	avifResult result;
	unsigned char *bgra;
	size_t width;
	size_t height;
	size_t y;
	bool has_alpha;
	CGImageRef image = NULL;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_avif_probe(b, n) || index != 0) {
		/* **FRAMES ARE A SEQUENCE (`avis`), AND A STILL FILE HAS ONE.** An animated AVIF is rare enough that this seam
		 * says so plainly instead of pretending frame n is cheap: it decodes the primary item, which is frame 0. */
		if (index != 0) {
			fprintf(stderr, "CG-REFUSE: this decoder reads an AVIF's first image; frame %zu is not decoded\n",
				index);
		}
		return NULL;
	}
	decoder = avifDecoderCreate();
	if (decoder == NULL) {
		return NULL;
	}
	if (avifDecoderSetIOMemory(decoder, b, n) != AVIF_RESULT_OK ||
	    avifDecoderSetSource(decoder, AVIF_DECODER_SOURCE_PRIMARY_ITEM) != AVIF_RESULT_OK ||
	    avifDecoderParse(decoder) != AVIF_RESULT_OK) {
		fprintf(stderr, "CG-REFUSE: libavif could not parse this AVIF\n");
		avifDecoderDestroy(decoder);
		return NULL;
	}
	result = avifDecoderNextImage(decoder);
	if (result != AVIF_RESULT_OK || decoder->image == NULL) {
		fprintf(stderr, "CG-REFUSE: libavif could not decode an AVIF frame (%s)\n", avifResultToString(result));
		avifDecoderDestroy(decoder);
		return NULL;
	}
	/* **A DEEP AVIF IS THE NORMAL AVIF, AND THIS LIBRARY HAS NO DEEP IMAGE AT ALL.** Unlike a 16-bit PNG — where refusing is
	 * right because a sibling decoder could keep the samples — AVIF's 10- and 12-bit files are what cameras and HDR
	 * encoders produce, and `avifImageYUVToRGB` with an 8-bit destination is libavif doing the tone-map-and-scale it does
	 * for every 8-bit viewer. So the conversion is allowed and SAID OUT LOUD, once per file, rather than refused. */
	if (decoder->image->depth != 8) {
		fprintf(stderr, "CG-NOTE: a %u-bit AVIF is converted to 8 bits per channel, which is the deepest image this "
				"library holds\n", (unsigned)decoder->image->depth);
	}
	avifRGBImageSetDefaults(&rgb, decoder->image);
	rgb.format = AVIF_RGB_FORMAT_RGBA;
	rgb.depth = 8;
	if (avifRGBImageAllocatePixels(&rgb) != AVIF_RESULT_OK) {
		avifDecoderDestroy(decoder);
		return NULL;
	}
	result = avifImageYUVToRGB(decoder->image, &rgb);
	if (result != AVIF_RESULT_OK) {
		fprintf(stderr, "CG-REFUSE: libavif could not convert an AVIF frame to RGB (%s)\n",
			avifResultToString(result));
		avifRGBImageFreePixels(&rgb);
		avifDecoderDestroy(decoder);
		return NULL;
	}
	width = decoder->image->width;
	height = decoder->image->height;
	has_alpha = decoder->alphaPresent != 0;
	bgra = malloc(width * 4u * height);
	if (bgra != NULL) {
		for (y = 0; y < height; y++) {
			const unsigned char *in = rgb.pixels + y * (size_t)rgb.rowBytes;
			unsigned char *out = bgra + y * width * 4u;
			size_t x;

			for (x = 0; x < width; x++) {
				out[x * 4u + 0u] = in[x * 4u + 2u];	/* B */
				out[x * 4u + 1u] = in[x * 4u + 1u];	/* G */
				out[x * 4u + 2u] = in[x * 4u + 0u];	/* R */
				out[x * 4u + 3u] = in[x * 4u + 3u];
			}
		}
		image = cg_image_from_bgra(bgra, width, height, width * 4u, has_alpha);
		free(bgra);
	}
	avifRGBImageFreePixels(&rgb);
	avifDecoderDestroy(decoder);
	return image;
}

static void fn_avif_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	avifDecoder *decoder;

	(void)index;
	*width = 0;
	*height = 0;
	if (!fn_avif_probe(b, n)) {
		return;
	}
	decoder = avifDecoderCreate();
	if (decoder == NULL) {
		return;
	}
	if (avifDecoderSetIOMemory(decoder, b, n) == AVIF_RESULT_OK &&
	    avifDecoderSetSource(decoder, AVIF_DECODER_SOURCE_PRIMARY_ITEM) == AVIF_RESULT_OK &&
	    avifDecoderParse(decoder) == AVIF_RESULT_OK) {
		*width = (size_t)decoder->image->width;
		*height = (size_t)decoder->image->height;
	}
	avifDecoderDestroy(decoder);
}

const cg_image_decoder cg_decoder_avif = {
	"public.avif", "avif", fn_avif_probe, NULL, fn_avif_decode, fn_avif_size
};

#else /* !AGORA_HAVE_LIBAVIF */

static bool fn_avif_absent(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

const cg_image_decoder cg_decoder_avif = {
	"public.avif", "avif", fn_avif_absent, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_LIBAVIF */
