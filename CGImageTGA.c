/*
 * CGImageTGA.c — a TGA (Truevision Targa) becomes an image.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **TGA HAS NO SIGNATURE, SO THE PROBE IS A SANITY CHECK RATHER THAN A MATCH.** There is no magic number to compare: the
 * format begins with a length byte, a colour-map type, an image type and a size. The probe therefore accepts only the
 * shapes the format defines (a colour-map type of 0 or 1, an image type of 1, 2, 3, 9, 10 or 11, a non-zero size, and a
 * pixel depth one of the format's own) and the decoder is registered LAST, after every format that can identify itself.
 * A file that is genuinely TGA and nothing else cannot be confused with one of those.
 *
 * WHAT IS READ: colour-mapped (1, 9), true-colour (2, 10) and grayscale (3, 11), 8/15/16/24/32-bit depths, both row
 * origins, and the RLE variants. WHAT IS REFUSED BY NAME: a depth the format does not define, an image type it does not
 * define, and a RLE run that would leave the image it declares.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fn_tga16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static bool fn_tga_shape(const unsigned char *b, size_t n, size_t *skip, size_t *width, size_t *height,
			 unsigned *depth, unsigned *type, unsigned *map_type, size_t *map_start, size_t *map_length,
			 unsigned *map_depth)
{
	unsigned id_length;

	if (n < 18) {
		return false;
	}
	id_length = b[0];
	*map_type = b[1];
	*type = b[2];
	if (!(*map_type == 0 || *map_type == 1)) {
		return false;
	}
	if (!(*type == 1 || *type == 2 || *type == 3 || *type == 9 || *type == 10 || *type == 11)) {
		return false;
	}
	if (*map_type == 1 && !(*type == 1 || *type == 9)) {
		return false;
	}
	*map_start = fn_tga16(b + 3);
	*map_length = fn_tga16(b + 5);
	*map_depth = b[7];
	*width = fn_tga16(b + 12);
	*height = fn_tga16(b + 14);
	*depth = b[16];
	if (*width == 0 || *height == 0) {
		return false;
	}
	if (!(*depth == 8 || *depth == 15 || *depth == 16 || *depth == 24 || *depth == 32)) {
		return false;
	}
	if (*type == 1 || *type == 9) {
		if (*map_depth != 15 && *map_depth != 16 && *map_depth != 24 && *map_depth != 32) {
			return false;
		}
	}
	*skip = 18u + id_length + ((*map_type == 1) ? (*map_length * ((*map_depth + 7u) / 8u)) : 0u);
	return *skip <= n;
}

static bool fn_tga_probe(const unsigned char *b, size_t n)
{
	size_t skip = 0;
	size_t w = 0;
	size_t h = 0;
	unsigned depth = 0;
	unsigned type = 0;
	unsigned map_type = 0;
	size_t map_start = 0;
	size_t map_length = 0;
	unsigned map_depth = 0;

	return fn_tga_shape(b, n, &skip, &w, &h, &depth, &type, &map_type, &map_start, &map_length, &map_depth);
}

static void fn_tga_size(const unsigned char *b, size_t n, size_t index, size_t *w, size_t *h)
{
	size_t skip = 0;
	unsigned depth = 0;
	unsigned type = 0;
	unsigned map_type = 0;
	size_t map_start = 0;
	size_t map_length = 0;
	unsigned map_depth = 0;

	(void)index;
	*w = 0;
	*h = 0;
	if (fn_tga_shape(b, n, &skip, w, h, &depth, &type, &map_type, &map_start, &map_length, &map_depth)) {
		return;
	}
	*w = 0;
	*h = 0;
}

/* One pixel, in the file's own bytes, to B, G, R, A. */
static void fn_tga_pixel(const unsigned char *in, unsigned depth, const unsigned char *map, size_t map_length,
			 unsigned map_depth, unsigned char out[4])
{
	if (depth == 8) {
		out[0] = out[1] = out[2] = in[0];
		out[3] = 255;
		return;
	}
	if (depth == 15 || depth == 16) {
		unsigned value = fn_tga16(in);
		unsigned b5 = (value >> 0) & 0x1fu;
		unsigned g5 = (value >> 5) & 0x1fu;
		unsigned r5 = (value >> 10) & 0x1fu;

		out[0] = (unsigned char)((b5 * 255u) / 31u);
		out[1] = (unsigned char)((g5 * 255u) / 31u);
		out[2] = (unsigned char)((r5 * 255u) / 31u);
		out[3] = ((depth == 16) && (value & 0x8000u) == 0) ? 0 : 255;
		return;
	}
	(void)map;
	(void)map_length;
	(void)map_depth;
	out[0] = in[0];
	out[1] = in[1];
	out[2] = in[2];
	out[3] = (depth == 32) ? in[3] : 255;
}

static CGImageRef fn_tga_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	size_t skip = 0;
	size_t width = 0;
	size_t height = 0;
	unsigned depth = 0;
	unsigned type = 0;
	unsigned map_type = 0;
	size_t map_start = 0;
	size_t map_length = 0;
	unsigned map_depth = 0;
	bool rle;
	bool top_origin;
	bool has_alpha = false;
	size_t bytes_per_pixel;
	size_t map_bytes = 0;
	unsigned char *bgra;
	size_t at;
	size_t pixels;
	size_t i;
	CGImageRef image;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (index != 0) {
		return NULL;
	}
	if (!fn_tga_shape(bytes, length, &skip, &width, &height, &depth, &type, &map_type, &map_start, &map_length,
			  &map_depth)) {
		fprintf(stderr, "CG-REFUSE: these bytes are not a TGA this decoder reads\n");
		return NULL;
	}
	rle = (type == 9 || type == 10 || type == 11);
	top_origin = (bytes[17] & 0x20u) != 0;
	bytes_per_pixel = ((type == 1 || type == 9) ? (map_depth + 7u) / 8u : (depth + 7u) / 8u);
	map_bytes = map_length * ((map_depth + 7u) / 8u);
	bgra = malloc(width * 4u * height);
	if (bgra == NULL) {
		return NULL;
	}
	at = skip;
	pixels = width * height;
	for (i = 0; i < pixels; ) {
		unsigned run = 1;
		bool is_run = false;
		unsigned char value[4];

		if (at + bytes_per_pixel > length) {
			fprintf(stderr, "CG-REFUSE: a TGA's pixels run past the end of the file\n");
			free(bgra);
			return NULL;
		}
		if (rle) {
			unsigned header = bytes[at++];

			run = (unsigned)(header & 0x7fu) + 1u;
			is_run = (header & 0x80u) != 0;
		}
		{
			const unsigned char *source = bytes + at;

			fn_tga_pixel(source, (type == 1 || type == 9) ? map_depth : depth,
				     (map_type == 1) ? bytes + 18 + bytes[0] : NULL, map_length, map_depth, value);
		}
		if (is_run) {
			at += bytes_per_pixel;
		}
		{
			unsigned n;

			for (n = 0; n < run && i < pixels; n++, i++) {
				size_t x = i % width;
				size_t y = i / width;
				size_t row = top_origin ? y : (height - 1u - y);
				unsigned char *out = bgra + (row * width + x) * 4u;

				if (map_type == 1) {
					/* THE COLOUR-MAPPED FORMS INDEX A TABLE, so the pixel's bytes are an index, not a colour. */
					size_t entry;

					if (depth == 8) {
						entry = bytes[at + (is_run ? 0u : n * bytes_per_pixel)];
					} else {
						entry = fn_tga16(bytes + at + (is_run ? 0u : n * bytes_per_pixel));
					}
					if (entry < map_length) {
						const unsigned char *m = bytes + 18 + bytes[0] + entry * ((map_depth + 7u) / 8u);

						fn_tga_pixel(m, map_depth, NULL, 0, 0, value);
					} else {
						value[0] = value[1] = value[2] = 0;
						value[3] = 255;
					}
				}
				out[0] = value[0];
				out[1] = value[1];
				out[2] = value[2];
				out[3] = value[3];
				if (value[3] != 255) {
					has_alpha = true;
				}
			}
		}
		if (!is_run) {
			at += bytes_per_pixel * run;
		}
	}
	image = cg_image_from_bgra(bgra, width, height, width * 4u, has_alpha);
	free(bgra);
	(void)map_bytes;
	(void)map_start;
	return image;
}

const cg_image_decoder cg_decoder_tga = {
	"com.truevision.tga-image", "tga targa icb vda vst", fn_tga_probe, NULL, fn_tga_decode, fn_tga_size
};
