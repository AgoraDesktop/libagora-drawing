/*
 * CGImageBMP.c — a BMP file, and the DIB an ICO wraps, become images.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE TWO FORMATS SHARE ONE DECODER BECAUSE THEY SHARE ONE STRUCTURE.** An ICO entry's payload IS a DIB — a
 * `BITMAPINFOHEADER` with its height doubled, its rows padded to four bytes and an AND-mask after the pixels — so
 * `cg_dib_decode` reads it once and both files call it. The only difference is where the DIB starts (BMP has a 14-byte
 * `BITMAPFILEHEADER` first) and that the ICO may instead hold a PNG.
 *
 * WHAT IS READ: 1, 4, 8, 16, 24 and 32 bits per pixel, top-down (`biHeight` negative) and bottom-up, `BI_RGB` and
 * `BI_BITFIELDS`, and the palette forms. WHAT IS REFUSED BY NAME: the RLE and Huffman/JPEG/PNG-in-BMP encodings
 * (`BI_RLE8`, `BI_RLE4`, `BI_JPEG`, `BI_PNG`, `BI_ALPHABITFIELDS`), OS/2's 12-byte core header, and any header that is
 * smaller than the fields it promises — each of those is a different parser, and a silent guess would draw garbage.
 *
 * **AND 32-BIT BI_RGB IS THE ONE PLACE THE FORMAT LIES.** The fourth byte is documented as "must be 0", and half the
 * world writes alpha into it. The rule here is the one every reader settles on: if ANY pixel's fourth byte is non-zero,
 * the file is using it as alpha; otherwise the image is opaque. That is measurable and it is what makes a Windows icon
 * with a transparent corner work while a plain 32-bit BMP still looks right.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned fn_le16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long fn_le32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
	       ((unsigned long)p[3] << 24);
}

/* THE DIB, WITHOUT ITS CALLER'S WRAPPER. `dib` points at a `BITMAPINFOHEADER`; `*has_alpha` says whether a 32-bit image's
 * fourth byte is alpha (see the file's note), and `flip_rows` is the caller's answer for ICO, whose rows are always
 * bottom-up whatever the header claims. */
CGImageRef cg_dib_decode(const unsigned char *dib, size_t length, size_t *out_width, size_t *out_height,
			 bool *out_has_alpha, bool allow_png)
{
	unsigned long header_size;
	long width;
	long height;
	unsigned planes;
	unsigned bpp;
	unsigned long compression;
	unsigned long colors_used;
	size_t palette_entries = 0;
	size_t palette_size = 0;
	size_t offset;
	size_t stride;
	size_t row_bytes;
	unsigned char *bgra;
	size_t y;
	bool top_down;
	bool has_alpha = false;
	CGImageRef image;

	*out_width = 0;
	*out_height = 0;
	*out_has_alpha = false;
	if (dib == NULL || length < 16) {
		return NULL;
	}
	/* AN ICO MAY STORE A PNG INSTEAD OF A DIB, which Windows has allowed since Vista and which every icon theme uses
	 * for its large sizes. The PNG is handed to this tree's own PNG decoder rather than to a second implementation. */
	if (allow_png && length >= 8 && dib[0] == 0x89 && dib[1] == 'P' && dib[2] == 'N' && dib[3] == 'G') {
		CGDataProviderRef provider;
		unsigned char *copy = malloc(length);
		CGImageRef png = NULL;

		if (copy != NULL) {
			memcpy(copy, dib, length);
			provider = CGDataProviderCreateWithData(NULL, copy, length, NULL);
			if (provider != NULL) {
				png = CGImageCreateWithPNGDataProvider(provider, NULL, true, kCGRenderingIntentDefault);
				CGDataProviderRelease(provider);
			}
		}
		if (png != NULL) {
			*out_width = CGImageGetWidth(png);
			*out_height = CGImageGetHeight(png);
			*out_has_alpha = true;
		}
		return png;
	}
	header_size = fn_le32(dib);
	if (header_size == 12) {
		fprintf(stderr, "CG-REFUSE: an OS/2 12-byte bitmap header is not read by this decoder\n");
		return NULL;
	}
	if (header_size < 40 || length < header_size) {
		fprintf(stderr, "CG-REFUSE: a bitmap header of %lu bytes is not one this decoder reads\n", header_size);
		return NULL;
	}
	width = (long)fn_le32(dib + 4);
	height = (long)fn_le32(dib + 8);
	planes = fn_le16(dib + 12);
	bpp = fn_le16(dib + 14);
	compression = fn_le32(dib + 16);
	colors_used = fn_le32(dib + 32);
	if (width <= 0 || height == 0 || planes != 1) {
		fprintf(stderr, "CG-REFUSE: a bitmap of %ldx%ld pixels on %u plane(s) is not an image\n", width, height,
			planes);
		return NULL;
	}
	if (compression != 0 && compression != 3) {
		fprintf(stderr, "CG-REFUSE: bitmap compression %lu (RLE, Huffman, JPEG or alpha-bitfields) is not "
				"read by this decoder\n", compression);
		return NULL;
	}
	if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
		fprintf(stderr, "CG-REFUSE: a %u-bit bitmap is not a depth this decoder reads\n", bpp);
		return NULL;
	}
	top_down = (height < 0);
	if (height < 0) {
		height = -height;
	}
	/* THE AND-MASK AN ICO APPENDS IS NOT PART OF THE IMAGE: the caller's height is doubled and the caller has already
	 * halved it, so nothing here has to know about the mask. */
	if (bpp <= 8) {
		if (colors_used == 0) {
			colors_used = (bpp == 1) ? 2u : (1u << bpp);
		}
		if (colors_used > 256u) {
			fprintf(stderr, "CG-REFUSE: a bitmap palette of %lu entries is larger than a palette can be\n",
				colors_used);
			return NULL;
		}
		palette_entries = (size_t)colors_used;
		palette_size = palette_entries * 4u;	/* the file's entries are B, G, R, 0 */
	} else if (compression == 3) {
		palette_size = 12;		/* BI_BITFIELDS: three (or four) channel masks */
	}
	offset = header_size;
	if (offset + palette_size > length) {
		fprintf(stderr, "CG-REFUSE: a bitmap's palette runs past the end of its data\n");
		return NULL;
	}
	/* THE PALETTE STAYS WHERE IT IS: `dib + header_size` is read by the pixel loop below, which is why
	 * `palette_size` is added to the PIXEL offset and not to the palette's. */
	offset += palette_size;
	row_bytes = ((size_t)width * (size_t)bpp + 31u) / 32u * 4u;
	stride = row_bytes;
	if (offset + row_bytes * (size_t)height > length) {
		fprintf(stderr, "CG-REFUSE: a %ldx%ld bitmap's pixels run past the end of its data\n", width, height);
		return NULL;
	}
	bgra = malloc((size_t)width * 4u * (size_t)height);
	if (bgra == NULL) {
		return NULL;
	}
	for (y = 0; y < (size_t)height; y++) {
		size_t source_row = top_down ? y : ((size_t)height - 1u - y);
		const unsigned char *in = dib + offset + source_row * stride;
		unsigned char *out = bgra + y * (size_t)width * 4u;
		size_t x;

		for (x = 0; x < (size_t)width; x++) {
			unsigned char b = 0;
			unsigned char g = 0;
			unsigned char r = 0;
			unsigned char a = 255;

			if (bpp == 1 || bpp == 4 || bpp == 8) {
				unsigned index;

				if (bpp == 1) {
					index = (in[x / 8u] >> (7u - (unsigned)(x % 8u))) & 1u;
				} else if (bpp == 4) {
					index = (in[x / 2u] >> ((x % 2u) == 0 ? 4u : 0u)) & 0x0fu;
				} else {
					index = in[x];
				}
				if (index < palette_entries) {
					const unsigned char *entry = dib + header_size + (size_t)index * 4u;

					b = entry[0];
					g = entry[1];
					r = entry[2];
				}
			} else if (bpp == 16) {
				unsigned value = fn_le16(in + x * 2u);
				unsigned r5;
				unsigned g5;
				unsigned b5;

				if (compression == 3) {
					/* THE MASKS THE FILE NAMES, READ AS BIT POSITIONS. A 555 and a 565 file differ only here. */
					unsigned long rmask = fn_le32(dib + header_size);
					unsigned long gmask = fn_le32(dib + header_size + 4);
					unsigned long bmask = fn_le32(dib + header_size + 8);
					unsigned rshift = 0;
					unsigned gshift = 0;
					unsigned bshift = 0;

					while (((rmask >> rshift) & 1u) == 0 && rshift < 32) rshift++;
					while (((gmask >> gshift) & 1u) == 0 && gshift < 32) gshift++;
					while (((bmask >> bshift) & 1u) == 0 && bshift < 32) bshift++;
					r5 = (unsigned)((value & rmask) >> rshift);
					g5 = (unsigned)((value & gmask) >> gshift);
					b5 = (unsigned)((value & bmask) >> bshift);
					r = (unsigned char)((r5 * 255u) / ((rmask >> rshift) > 0 ? (rmask >> rshift) : 31u));
					g = (unsigned char)((g5 * 255u) / ((gmask >> gshift) > 0 ? (gmask >> gshift) : 31u));
					b = (unsigned char)((b5 * 255u) / ((bmask >> bshift) > 0 ? (bmask >> bshift) : 31u));
				} else {
					r5 = (value >> 10) & 0x1fu;
					g5 = (value >> 5) & 0x1fu;
					b5 = value & 0x1fu;
					r = (unsigned char)((r5 * 255u) / 31u);
					g = (unsigned char)((g5 * 255u) / 31u);
					b = (unsigned char)((b5 * 255u) / 31u);
				}
			} else if (bpp == 24) {
				b = in[x * 3u];
				g = in[x * 3u + 1u];
				r = in[x * 3u + 2u];
			} else {
				b = in[x * 4u];
				g = in[x * 4u + 1u];
				r = in[x * 4u + 2u];
				a = in[x * 4u + 3u];
			}
			out[x * 4u + 0u] = b;
			out[x * 4u + 1u] = g;
			out[x * 4u + 2u] = r;
			out[x * 4u + 3u] = a;
		}
	}
	/* **THE FOURTH BYTE DECIDES FOR ITSELF.** If any pixel of a 32-bit image used it, the file meant alpha and the
	 * pixels keep it; if none did, the format's "must be 0" is true and the image is opaque — which is what makes a
	 * plain 32-bit BMP look right and a Windows icon with a transparent corner work. */
	if (bpp == 32) {
		size_t i;
		size_t count = (size_t)width * (size_t)height;

		for (i = 0; i < count; i++) {
			if (bgra[i * 4u + 3u] != 0) {
				has_alpha = true;
				break;
			}
		}
		if (!has_alpha) {
			for (i = 0; i < count; i++) {
				bgra[i * 4u + 3u] = 255;
			}
		}
	}
	image = cg_image_from_bgra(bgra, (size_t)width, (size_t)height, (size_t)width * 4u, has_alpha);
	free(bgra);
	if (image != NULL) {
		*out_width = (size_t)width;
		*out_height = (size_t)height;
		*out_has_alpha = has_alpha;
	}
	return image;
}

/* ---- BMP -------------------------------------------------------------------------------------- */

static bool fn_bmp_probe(const unsigned char *bytes, size_t length)
{
	return length >= 18 && bytes[0] == 'B' && bytes[1] == 'M';
}

static void fn_bmp_size(const unsigned char *bytes, size_t length, size_t index, size_t *width, size_t *height)
{
	(void)index;
	*width = 0;
	*height = 0;
	if (length < 26) {
		return;
	}
	*width = (size_t)fn_le32(bytes + 18);
	{
		long h = (long)fn_le32(bytes + 22);

		*height = (size_t)((h < 0) ? -h : h);
	}
}

static CGImageRef fn_bmp_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	size_t width = 0;
	size_t height = 0;
	bool has_alpha = false;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (index != 0 || length < 14) {
		return NULL;
	}
	/* **THE DIB STARTS AT BYTE 14, NOT AT `bfOffBits`.** The file header's offset names the PIXELS; the header that
	 * describes them follows the file header, and the DIB reader works the pixel offset out from the header's own size
	 * and the palette. MEASURED: using `bfOffBits` read the pixels as a header — "a bitmap header of 4291307560 bytes". */
	return cg_dib_decode(bytes + 14, length - 14, &width, &height, &has_alpha, false);
}

const cg_image_decoder cg_decoder_bmp = {
	"com.microsoft.bmp", "bmp dib", fn_bmp_probe,
	NULL, fn_bmp_decode, fn_bmp_size
};
