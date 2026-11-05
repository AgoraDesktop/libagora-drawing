/*
 * CGImageICO.c — a Windows icon (ICO/CUR) becomes an image.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **AN ICO IS A DIRECTORY OF IMAGES AND NOT A FILE FORMAT OF ITS OWN.** Every entry is a complete image — a DIB with an
 * AND-mask (the same structure a BMP holds, which is why `cg_dib_decode` reads it for both) or, since Vista, a PNG for
 * the large sizes. So the count is the directory's length and each frame is one entry, decoded by the BMP file's DIB
 * reader with the ICO's "a PNG may be inside" rule switched on.
 *
 * THE SIZE COMES FROM THE DIRECTORY, which is the one thing an ICO states about an image without decoding it — and a
 * width or height of 0 means 256, the field being one byte.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fn_ico16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long fn_ico32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
	       ((unsigned long)p[3] << 24);
}

static bool fn_ico_probe(const unsigned char *b, size_t n)
{
	return n >= 22 && b[0] == 0 && b[1] == 0 && (b[2] == 1 || b[2] == 2) && b[3] == 0 && fn_ico16(b + 4) > 0;
}

static size_t fn_ico_count(const unsigned char *b, size_t n)
{
	size_t count;

	if (!fn_ico_probe(b, n)) {
		return 0;
	}
	count = fn_ico16(b + 4);
	return (count * 16u + 6u <= n) ? count : 0;
}

static CGImageRef fn_ico_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	size_t count;
	size_t entry;
	unsigned long size;
	unsigned long offset;
	size_t width = 0;
	size_t height = 0;
	bool has_alpha = false;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_ico_probe(b, n)) {
		return NULL;
	}
	count = fn_ico16(b + 4);
	if (index >= count || 6u + index * 16u + 16u > n) {
		return NULL;
	}
	entry = 6u + index * 16u;
	size = fn_ico32(b + entry + 8);
	offset = fn_ico32(b + entry + 12);
	if (size == 0 || offset + size > n) {
		fprintf(stderr, "CG-REFUSE: an ICO's entry %zu points outside the file\n", index);
		return NULL;
	}
	return cg_dib_decode(b + offset, size, &width, &height, &has_alpha, true);
}

static void fn_ico_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	size_t entry;

	*width = 0;
	*height = 0;
	if (!fn_ico_probe(b, n) || index >= fn_ico16(b + 4) || 6u + index * 16u + 16u > n) {
		return;
	}
	entry = 6u + index * 16u;
	*width = b[entry] ? b[entry] : 256u;
	*height = b[entry + 1] ? b[entry + 1] : 256u;
}

const cg_image_decoder cg_decoder_ico = {
	"com.microsoft.ico", "ico cur", fn_ico_probe, fn_ico_count, fn_ico_decode, fn_ico_size
};
