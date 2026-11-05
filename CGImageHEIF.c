/*
 * CGImageHEIF.c — a HEIC/HEIF image becomes an image, through libheif.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **HEIF IS A CONTAINER AND THE CODEC IS SOMEBODY ELSE'S**: the file is a box structure of images, and the pixels inside are
 * HEVC (or AV1) frames that libheif hands to a plugin — `libde265` for HEVC, `dav1d`/`aom` for AV1. libheif owns the
 * container and the plugin selection; this file owns the two things a Core Graphics seam needs: reading the primaries (and
 * their thumbnails) and asking for RGB rather than the YCbCr planes the file actually stores.
 *
 * **`heif_context_read_from_memory_without_copy` IS THE WHOLE REASON THIS NEEDS NO TEMPORARY FILE.** It borrows the
 * caller's bytes for the duration of the decode, which is exactly what a decoder handed a buffer wants.
 *
 * WHAT IS READ: the primary image, and — when the file holds several top-level images, which is how an animated HEIC
 * stores its frames — each of them in order. WHAT IS REFUSED BY NAME: a file libheif cannot parse, and an image whose
 * chroma conversion fails.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AGORA_HAVE_LIBHEIF

#include <libheif/heif.h>

static bool fn_heif_probe(const unsigned char *b, size_t n)
{
	/* `....ftypheic`, `heix`, `hevc`, `mif1`, `msf1`, `avif` — the file type box, whose major brand is at byte 8. */
	if (n < 12 || memcmp(b + 4, "ftyp", 4) != 0) {
		return false;
	}
	return memcmp(b + 8, "heic", 4) == 0 || memcmp(b + 8, "heix", 4) == 0 || memcmp(b + 8, "hevc", 4) == 0 ||
	       memcmp(b + 8, "hevx", 4) == 0 || memcmp(b + 8, "mif1", 4) == 0 || memcmp(b + 8, "msf1", 4) == 0;
}

static heif_context *fn_heif_open(const unsigned char *bytes, size_t length)
{
	heif_context *context = heif_context_alloc();

	if (context == NULL) {
		return NULL;
	}
	if (heif_context_read_from_memory_without_copy(context, bytes, length, NULL).code != heif_error_Ok) {
		heif_context_free(context);
		return NULL;
	}
	return context;
}

/* HOW MANY TOP-LEVEL IMAGES THE FILE HOLDS. A still HEIC is one; an animation is a grid of them, which is what this
 * counts. A file whose images are all thumbnails of one primary still answers one, because the primary is one. */
static size_t fn_heif_count(const unsigned char *b, size_t n)
{
	heif_context *context;
	int count = 1;

	if (!fn_heif_probe(b, n)) {
		return 0;
	}
	context = fn_heif_open(b, n);
	if (context == NULL) {
		return 0;
	}
	count = heif_context_get_number_of_top_level_images(context);
	heif_context_free(context);
	return (count > 0) ? (size_t)count : 1;
}

static CGImageRef fn_heif_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	heif_context *context;
	heif_image_handle *handle = NULL;
	heif_image *image = NULL;
	int width;
	int height;
	const uint8_t *planes[4] = { NULL, NULL, NULL, NULL };
	int strides[4] = { 0, 0, 0, 0 };
	unsigned char *bgra;
	size_t y;
	bool has_alpha;
	CGImageRef result = NULL;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_heif_probe(b, n)) {
		return NULL;
	}
	context = fn_heif_open(b, n);
	if (context == NULL) {
		fprintf(stderr, "CG-REFUSE: libheif could not read this HEIF container\n");
		return NULL;
	}
	{
		heif_item_id ids[8];
		int got = heif_context_get_list_of_top_level_image_IDs(context, ids, 8);

		if (got <= 0 || index >= (size_t)got) {
			fprintf(stderr, "CG-REFUSE: this HEIF holds %d top-level image(s), so index %zu is past the end\n", got,
				index);
			heif_context_free(context);
			return NULL;
		}
		if (heif_context_get_image_handle(context, ids[index], &handle).code != heif_error_Ok) {
			fprintf(stderr, "CG-REFUSE: libheif could not open a HEIF image handle\n");
			heif_context_free(context);
			return NULL;
		}
	}
	/* **RGB WITH ALPHA, INTERLEAVED, BECAUSE THAT IS WHAT A CGIMAGE IN THIS TREE HOLDS.** libheif converts from the
	 * file's YCbCr (or its RGB, or its monochrome) into this, using the plugin that decodes the frame. */
	{
		struct heif_decoding_options *options = heif_decoding_options_alloc();
		struct heif_error error;

		error = heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, options);
		heif_decoding_options_free(options);
		if (error.code != heif_error_Ok || image == NULL) {
			fprintf(stderr, "CG-REFUSE: libheif could not decode a HEIF frame (%s)\n",
				error.message != NULL ? error.message : "no message");
			heif_image_handle_release(handle);
			heif_context_free(context);
			return NULL;
		}
	}
	width = heif_image_get_width(image, heif_channel_interleaved);
	height = heif_image_get_height(image, heif_channel_interleaved);
	planes[0] = heif_image_get_plane_readonly(image, heif_channel_interleaved, &strides[0]);
	has_alpha = heif_image_handle_has_alpha_channel(handle) != 0;
	if (width <= 0 || height <= 0 || planes[0] == NULL) {
		fprintf(stderr, "CG-REFUSE: a HEIF frame decoded with no pixels\n");
		goto done;
	}
	bgra = malloc((size_t)width * 4u * (size_t)height);
	if (bgra == NULL) {
		goto done;
	}
	for (y = 0; y < (size_t)height; y++) {
		const unsigned char *in = planes[0] + y * (size_t)strides[0];
		unsigned char *out = bgra + y * (size_t)width * 4u;
		size_t x;

		for (x = 0; x < (size_t)width; x++) {
			out[x * 4u + 0u] = in[x * 4u + 2u];	/* B */
			out[x * 4u + 1u] = in[x * 4u + 1u];	/* G */
			out[x * 4u + 2u] = in[x * 4u + 0u];	/* R */
			out[x * 4u + 3u] = in[x * 4u + 3u];
		}
	}
	result = cg_image_from_bgra(bgra, (size_t)width, (size_t)height, (size_t)width * 4u, has_alpha);
	free(bgra);
done:
	if (image != NULL) {
		heif_image_release(image);
	}
	if (handle != NULL) {
		heif_image_handle_release(handle);
	}
	heif_context_free(context);
	return result;
}

static void fn_heif_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	heif_context *context;
	heif_item_id ids[8];
	int got;
	heif_image_handle *handle = NULL;

	*width = 0;
	*height = 0;
	context = fn_heif_open(b, n);
	if (context == NULL) {
		return;
	}
	got = heif_context_get_list_of_top_level_image_IDs(context, ids, 8);
	if (got > 0 && index < (size_t)got && heif_context_get_image_handle(context, ids[index], &handle).code == heif_error_Ok) {
		*width = (size_t)heif_image_handle_get_width(handle);
		*height = (size_t)heif_image_handle_get_height(handle);
		heif_image_handle_release(handle);
	}
	heif_context_free(context);
}

const cg_image_decoder cg_decoder_heif = {
	"public.heic", "heic heif hif", fn_heif_probe, fn_heif_count, fn_heif_decode, fn_heif_size
};

#else /* !AGORA_HAVE_LIBHEIF */

static bool fn_heif_absent(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

const cg_image_decoder cg_decoder_heif = {
	"public.heic", "heic heif hif", fn_heif_absent, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_LIBHEIF */
