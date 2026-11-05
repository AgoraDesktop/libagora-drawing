/*
 * CGImageTIFF.c — a TIFF becomes an image, through libtiff.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **TIFF IS THE ONE FORMAT IN THIS SET WHERE WRITING A DECODER WOULD BE A PROJECT RATHER THAN A FILE**: compression is a
 * list (none, LZW, Deflate, PackBits, JPEG, CCITT…), the same tag can be a scalar or an array, and a file is a chain of
 * directories whose meaning depends on the photometric tag. libtiff is the reference reader for all of it, it is a system
 * library here (`libtiff-dev`), and this file is the seam: a memory client so no temporary file is written, one directory
 * per frame, and `TIFFReadRGBAImage` — which is libtiff's own "give me the picture in 8-bit RGBA" door and therefore the
 * one that honours every photometric and planar configuration it knows.
 *
 * **AND ITS ROWS ARRIVE BOTTOM-UP**, because TIFF's origin is the bottom-left; `TIFFReadRGBAImage` hands back rows in that
 * order whatever the file's `Orientation` tag says about display, so the flip is here and is stated rather than assumed.
 *
 * The build without libtiff compiles this file to a decoder whose probe says NO, so the registry simply does not offer
 * TIFF — the honest statement `-CopyTypeIdentifiers` then makes.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AGORA_HAVE_TIFF

#include <tiffio.h>

typedef struct {
	const unsigned char *bytes;
	size_t length;
	size_t position;
} fn_tiff_memory;

static tmsize_t fn_tiff_read(thandle_t handle, void *buffer, tmsize_t size)
{
	fn_tiff_memory *memory = (fn_tiff_memory *)handle;
	tmsize_t available;

	if (memory == NULL || size <= 0) {
		return 0;
	}
	available = (tmsize_t)(memory->length - memory->position);
	if (size > available) {
		size = available;
	}
	if (size <= 0) {
		return 0;
	}
	memcpy(buffer, memory->bytes + memory->position, (size_t)size);
	memory->position += (size_t)size;
	return size;
}

static tmsize_t fn_tiff_write(thandle_t handle, void *buffer, tmsize_t size)
{
	(void)handle;
	(void)buffer;
	(void)size;
	return 0;
}

static toff_t fn_tiff_seek(thandle_t handle, toff_t offset, int whence)
{
	fn_tiff_memory *memory = (fn_tiff_memory *)handle;
	toff_t target;

	if (memory == NULL) {
		return (toff_t)-1;
	}
	if (whence == SEEK_SET) {
		target = offset;
	} else if (whence == SEEK_CUR) {
		target = (toff_t)memory->position + offset;
	} else {
		target = (toff_t)memory->length + offset;
	}
	if (target > (toff_t)memory->length) {
		return (toff_t)-1;
	}
	memory->position = (size_t)target;
	return target;
}

static int fn_tiff_close(thandle_t handle)
{
	(void)handle;
	return 0;
}

static toff_t fn_tiff_byte_size(thandle_t handle)
{
	fn_tiff_memory *memory = (fn_tiff_memory *)handle;

	return memory != NULL ? (toff_t)memory->length : 0;
}

static int fn_tiff_map(thandle_t handle, void **base, toff_t *size)
{
	fn_tiff_memory *memory = (fn_tiff_memory *)handle;

	if (memory == NULL) {
		return 0;
	}
	*base = (void *)memory->bytes;
	*size = (toff_t)memory->length;
	return 1;
}

static void fn_tiff_unmap(thandle_t handle, void *base, toff_t size)
{
	(void)handle;
	(void)base;
	(void)size;
}

static TIFF *fn_tiff_open(const unsigned char *bytes, size_t length, fn_tiff_memory *memory)
{
	memory->bytes = bytes;
	memory->length = length;
	memory->position = 0;
	return TIFFClientOpen("memory", "rm", (thandle_t)memory, fn_tiff_read, fn_tiff_write, fn_tiff_seek,
			      fn_tiff_close, fn_tiff_byte_size, fn_tiff_map, fn_tiff_unmap);
}

static bool fn_tiff_probe(const unsigned char *b, size_t n)
{
	return n >= 8 && ((b[0] == 'I' && b[1] == 'I' && b[2] == 0x2a && b[3] == 0) ||
			  (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && b[3] == 0x2a));
}

static size_t fn_tiff_count(const unsigned char *b, size_t n)
{
	fn_tiff_memory memory;
	TIFF *tiff;
	size_t count = 0;

	if (!fn_tiff_probe(b, n)) {
		return 0;
	}
	tiff = fn_tiff_open(b, n, &memory);
	if (tiff == NULL) {
		return 0;
	}
	count = (size_t)TIFFNumberOfDirectories(tiff);
	TIFFClose(tiff);
	return count;
}

static void fn_tiff_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	fn_tiff_memory memory;
	TIFF *tiff;
	uint32_t w = 0;
	uint32_t h = 0;

	*width = 0;
	*height = 0;
	tiff = fn_tiff_open(b, n, &memory);
	if (tiff == NULL) {
		return;
	}
	if (TIFFSetDirectory(tiff, (uint16_t)index) == 1 && TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &w) == 1 &&
	    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &h) == 1) {
		*width = w;
		*height = h;
	}
	TIFFClose(tiff);
}

static CGImageRef fn_tiff_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	fn_tiff_memory memory;
	TIFF *tiff;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t *rgba;
	unsigned char *bgra;
	size_t row;
	bool has_alpha = false;
	CGImageRef image;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_tiff_probe(b, n)) {
		return NULL;
	}
	tiff = fn_tiff_open(b, n, &memory);
	if (tiff == NULL) {
		fprintf(stderr, "CG-REFUSE: libtiff could not open these bytes\n");
		return NULL;
	}
	if (TIFFSetDirectory(tiff, (uint16_t)index) != 1 || TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) != 1 ||
	    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) != 1 || width == 0 || height == 0) {
		fprintf(stderr, "CG-REFUSE: a TIFF directory %zu has no readable size\n", index);
		TIFFClose(tiff);
		return NULL;
	}
	rgba = (uint32_t *)_TIFFmalloc((tmsize_t)(width * height * 4u));
	if (rgba == NULL) {
		TIFFClose(tiff);
		return NULL;
	}
	if (TIFFReadRGBAImageOriented(tiff, width, height, rgba, ORIENTATION_TOPLEFT, 0) != 1) {
		fprintf(stderr, "CG-REFUSE: libtiff could not read a TIFF directory %zu into RGBA\n", index);
		_TIFFfree(rgba);
		TIFFClose(tiff);
		return NULL;
	}
	bgra = malloc((size_t)width * 4u * (size_t)height);
	if (bgra == NULL) {
		_TIFFfree(rgba);
		TIFFClose(tiff);
		return NULL;
	}
	/* TIFFReadRGBAImageOriented with TOPLEFT already puts row 0 at the TOP, so this is a straight channel swap:
	 * libtiff's word is R,G,B,A in memory order and this library's is B,G,R,A. */
	for (row = 0; row < (size_t)height; row++) {
		size_t x;

		for (x = 0; x < (size_t)width; x++) {
			uint32_t pixel = rgba[row * (size_t)width + x];
			unsigned char *out = bgra + (row * (size_t)width + x) * 4u;

			out[0] = (unsigned char)(pixel & 0xffu);		/* B */
			out[1] = (unsigned char)((pixel >> 8) & 0xffu);	/* G */
			out[2] = (unsigned char)((pixel >> 16) & 0xffu);	/* R */
			out[3] = (unsigned char)((pixel >> 24) & 0xffu);	/* A */
			if (out[3] != 255) {
				has_alpha = true;
			}
		}
	}
	image = cg_image_from_bgra(bgra, width, height, (size_t)width * 4u, has_alpha);
	free(bgra);
	_TIFFfree(rgba);
	TIFFClose(tiff);
	return image;
}

const cg_image_decoder cg_decoder_tiff = {
	"public.tiff", "tif tiff", fn_tiff_probe, fn_tiff_count, fn_tiff_decode, fn_tiff_size
};

#else /* !AGORA_HAVE_TIFF */

static bool fn_tiff_absent(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

const cg_image_decoder cg_decoder_tiff = {
	"public.tiff", "tif tiff", fn_tiff_absent, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_TIFF */
