/*
 * CGImageJP2.c — a JPEG 2000 codestream becomes an image, through OpenJPEG.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **JPEG 2000 IS A CODEC RATHER THAN A PICTURE: a `.jp2` file wraps a codestream in a box structure, and a `.j2k` is the
 * codestream with no wrapper at all.** OpenJPEG reads both, and the only thing that differs is which codec is created —
 * which is why the probe below accepts both signatures and the decoder asks them apart again.
 *
 * **THE STREAM IS THE MEMORY STREAM, WHICH IS FOUR CALLBACKS AND NOT A TEMPORARY FILE.** OpenJPEG's default reader wants a
 * path; `opj_stream_default_create` plus the reader, skipper and seeker over the caller's own bytes is what keeps a decode
 * from touching the filesystem — and `opj_stream_set_user_data_length` is what makes OpenJPEG refuse a truncated file
 * instead of reading past it.
 *
 * WHAT IS READ: 1, 3 and 4 components at 8 bits per component — gray, RGB and RGBA. WHAT IS REFUSED BY NAME: any other
 * precision (a 16-bit codestream downshifted to 8 would be a silent loss), any other component count, and a file whose
 * codestream OpenJPEG itself refuses. Its own handlers are silenced because this tree's rule is one named line per refusal
 * and OpenJPEG writes several per failure.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AGORA_HAVE_OPENJPEG

#include <openjpeg.h>

typedef struct {
	const unsigned char *bytes;
	size_t length;
	size_t at;
} fn_jp2_memory;

static bool fn_jp2_probe(const unsigned char *b, size_t n)
{
	/* THE RAW CODESTREAM starts with the SOC marker 0xFF4F; the JP2 wrapper starts with the signature box, whose box type
	 * at byte 4 is `jP  `. */
	if (n >= 2 && b[0] == 0xff && b[1] == 0x4f) {
		return true;
	}
	return n >= 12 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x00 && b[3] == 0x0c && b[4] == 'j' && b[5] == 'P' &&
	       b[6] == ' ' && b[7] == ' ';
}

static void fn_jp2_quiet(const char *message, void *data)
{
	(void)message;
	(void)data;
}

static OPJ_SIZE_T fn_jp2_read(void *buffer, OPJ_SIZE_T size, void *data)
{
	fn_jp2_memory *memory = (fn_jp2_memory *)data;
	size_t left;

	if (memory == NULL || size == 0) {
		return (OPJ_SIZE_T)-1;
	}
	left = memory->length - memory->at;
	if (left == 0) {
		return (OPJ_SIZE_T)-1;	/* end of stream, which OpenJPEG reads as such */
	}
	if ((size_t)size > left) {
		size = (OPJ_SIZE_T)left;
	}
	memcpy(buffer, memory->bytes + memory->at, (size_t)size);
	memory->at += (size_t)size;
	return size;
}

static OPJ_OFF_T fn_jp2_skip(OPJ_OFF_T size, void *data)
{
	fn_jp2_memory *memory = (fn_jp2_memory *)data;

	if (memory == NULL || size < 0) {
		return -1;
	}
	if ((size_t)size > memory->length - memory->at) {
		size = (OPJ_OFF_T)(memory->length - memory->at);
	}
	memory->at += (size_t)size;
	return size;
}

static OPJ_BOOL fn_jp2_seek(OPJ_OFF_T position, void *data)
{
	fn_jp2_memory *memory = (fn_jp2_memory *)data;

	if (memory == NULL || position < 0 || (size_t)position > memory->length) {
		return OPJ_FALSE;
	}
	memory->at = (size_t)position;
	return OPJ_TRUE;
}

static CGImageRef fn_jp2_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	fn_jp2_memory memory;
	opj_dparameters_t parameters;
	opj_codec_t *codec;
	opj_stream_t *stream;
	opj_image_t *image = NULL;
	OPJ_CODEC_FORMAT format;
	unsigned char *bgra;
	size_t width;
	size_t height;
	size_t x;
	size_t y;
	bool has_alpha;
	CGImageRef result = NULL;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (index != 0 || !fn_jp2_probe(bytes, length)) {
		return NULL;
	}
	memory.bytes = bytes;
	memory.length = length;
	memory.at = 0;
	format = (bytes[0] == 0xff && bytes[1] == 0x4f) ? OPJ_CODEC_J2K : OPJ_CODEC_JP2;
	codec = opj_create_decompress(format);
	if (codec == NULL) {
		return NULL;
	}
	opj_set_info_handler(codec, fn_jp2_quiet, NULL);
	opj_set_warning_handler(codec, fn_jp2_quiet, NULL);
	opj_set_error_handler(codec, fn_jp2_quiet, NULL);
	opj_set_default_decoder_parameters(&parameters);
	if (!opj_setup_decoder(codec, &parameters)) {
		opj_destroy_codec(codec);
		return NULL;
	}
	stream = opj_stream_default_create(OPJ_TRUE);
	if (stream == NULL) {
		opj_destroy_codec(codec);
		return NULL;
	}
	opj_stream_set_user_data(stream, &memory, NULL);
	opj_stream_set_user_data_length(stream, length);
	opj_stream_set_read_function(stream, fn_jp2_read);
	opj_stream_set_skip_function(stream, fn_jp2_skip);
	opj_stream_set_seek_function(stream, fn_jp2_seek);
	if (!opj_read_header(stream, codec, &image) || !opj_decode(codec, stream, image) ||
	    !opj_end_decompress(codec, stream)) {
		fprintf(stderr, "CG-REFUSE: OpenJPEG could not read this JPEG 2000 codestream\n");
		goto done;
	}
	if (image->numcomps != 1 && image->numcomps != 3 && image->numcomps != 4) {
		fprintf(stderr, "CG-REFUSE: a JPEG 2000 with %u components is not a layout this decoder reads\n",
			image->numcomps);
		goto done;
	}
	if (image->comps[0].prec != 8) {
		fprintf(stderr, "CG-REFUSE: a %u-bit JPEG 2000 is not read by this decoder, and 8-bit output would lose "
				"the caller's data\n", image->comps[0].prec);
		goto done;
	}
	width = image->comps[0].w;
	height = image->comps[0].h;
	has_alpha = (image->numcomps == 4);
	bgra = malloc(width * 4u * height);
	if (bgra == NULL) {
		goto done;
	}
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			unsigned char *out = bgra + (y * width + x) * 4u;

			if (image->numcomps == 1) {
				unsigned char grey = (unsigned char)image->comps[0].data[y * width + x];

				out[0] = out[1] = out[2] = grey;
				out[3] = 255;
			} else {
				out[0] = (unsigned char)image->comps[2].data[y * width + x];	/* B */
				out[1] = (unsigned char)image->comps[1].data[y * width + x];	/* G */
				out[2] = (unsigned char)image->comps[0].data[y * width + x];	/* R */
				out[3] = has_alpha ? (unsigned char)image->comps[3].data[y * width + x] : 255;
			}
		}
	}
	result = cg_image_from_bgra(bgra, width, height, width * 4u, has_alpha);
	free(bgra);
done:
	if (image != NULL) {
		opj_image_destroy(image);
	}
	opj_stream_destroy(stream);
	opj_destroy_codec(codec);
	return result;
}

/* THE SIZE WITHOUT DECODING IS NOT CHEAP HERE — OpenJPEG's header has to be read — so it is left unanswered rather than
 * paid twice: a caller that wants the size decodes, which is what this tier's viewer does anyway. */
static void fn_jp2_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	(void)b;
	(void)n;
	(void)index;
	*width = 0;
	*height = 0;
}

const cg_image_decoder cg_decoder_jp2 = {
	"public.jpeg-2000", "jp2 j2k jpx jpf", fn_jp2_probe, NULL, fn_jp2_decode, fn_jp2_size
};

#else /* !AGORA_HAVE_OPENJPEG */

static bool fn_jp2_absent(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

const cg_image_decoder cg_decoder_jp2 = {
	"public.jpeg-2000", "jp2 j2k jpx jpf", fn_jp2_absent, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_OPENJPEG */
