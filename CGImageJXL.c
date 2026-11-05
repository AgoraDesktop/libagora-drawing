/*
 * CGImageJXL.c — a JPEG XL image becomes an image, through libjxl.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **JPEG XL IS A CODESTREAM WITH TWO SPELLINGS**: the bare codestream starts with `0xFF 0x0A`, and the container — which is
 * what a `.jxl` file usually is — starts with a box structure whose signature box says `JXL `. The probe accepts both, and
 * libjxl reads both through the same decoder.
 *
 * **ITS C API IS AN EVENT LOOP RATHER THAN A CALL**: `JxlDecoderProcessInput` returns what it has reached — basic info,
 * then the frame, then the image — and the caller answers each one. That is why this file is a loop: ask for the size,
 * allocate, ask for the pixels. A decoder that assumed a single call would work on small files and fail on large ones,
 * because libjxl processes what has arrived so far.
 *
 * WHAT IS READ: the first frame, 8-bit, as BGRA (`JXL_DEC_BASIC_INFO` gives the size; the output buffer format is
 * `JXL_TYPE_UINT8` with four channels). WHAT IS REFUSED BY NAME: a file libjxl cannot parse, and a request for a frame
 * other than the first — an animation's later frames are a decoder loop of their own, and pretending otherwise would
 * return frame 0 for frame 3.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AGORA_HAVE_LIBJXL

#include <jxl/decode.h>

static bool fn_jxl_probe(const unsigned char *b, size_t n)
{
	if (n >= 2 && b[0] == 0xff && b[1] == 0x0a) {
		return true;	/* bare codestream */
	}
	return n >= 12 && b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x00 && b[3] == 0x0c && b[4] == 'J' && b[5] == 'X' &&
	       b[6] == 'L' && b[7] == ' ';
}

/* ONE PASS OVER THE STREAM. `want_pixels` false answers the size only, which is what `-size` asks — the events are the
 * same until `JXL_DEC_NEED_IMAGE_OUT_BUFFER`, so the two doors share this function. */
static bool fn_jxl_run(const unsigned char *bytes, size_t length, size_t want_frame, bool want_pixels,
		       size_t *width, size_t *height, CGImageRef *out_image)
{
	JxlDecoder *decoder;
	JxlPixelFormat format;
	size_t frame = 0;
	bool ok = false;

	if (out_image != NULL) {
		*out_image = NULL;
	}
	decoder = JxlDecoderCreate(NULL);
	if (decoder == NULL) {
		return false;
	}
	format.num_channels = 4;
	format.data_type = JXL_TYPE_UINT8;
	format.endianness = JXL_NATIVE_ENDIAN;
	format.align = 0;
	if (JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
	    JxlDecoderSetInput(decoder, bytes, length) != JXL_DEC_SUCCESS) {
		JxlDecoderDestroy(decoder);
		return false;
	}
	JxlDecoderCloseInput(decoder);
	for (;;) {
		JxlDecoderStatus status = JxlDecoderProcessInput(decoder);

		if (status == JXL_DEC_ERROR) {
			fprintf(stderr, "CG-REFUSE: libjxl could not decode this file\n");
			break;
		}
		if (status == JXL_DEC_SUCCESS) {
			ok = (*width > 0 && *height > 0);
			break;
		}
		if (status == JXL_DEC_BASIC_INFO) {
			JxlBasicInfo info;

			if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) {
				break;
			}
			*width = info.xsize;
			*height = info.ysize;
			continue;
		}
		if (status == JXL_DEC_FRAME) {
			const JxlFrameHeader *header = NULL;

			(void)header;
			continue;
		}
		if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
			size_t needed = 0;

			if (JxlDecoderImageOutBufferSize(decoder, &format, &needed) != JXL_DEC_SUCCESS) {
				break;
			}
			if (!want_pixels || frame != want_frame) {
				/* A frame we do not want still has to be given a buffer or the stream cannot advance; the
				 * cheapest honest thing is to skip the file's later frames entirely (see the header). */
				if (frame != want_frame) {
					fprintf(stderr, "CG-REFUSE: this decoder reads a JPEG XL's first frame; frame %zu is not "
							"decoded\n", want_frame);
					break;
				}
			}
			{
				unsigned char *pixels = malloc(needed);
				CGImageRef image;

				if (pixels == NULL) {
					break;
				}
				if (JxlDecoderSetImageOutBuffer(decoder, &format, pixels, needed) != JXL_DEC_SUCCESS) {
					free(pixels);
					break;
				}
				{
					JxlDecoderStatus next = JxlDecoderProcessInput(decoder);

					if (next != JXL_DEC_FULL_IMAGE) {
						free(pixels);
						break;
					}
				}
				/* JXL'S FOUR CHANNELS ARE R, G, B, A — this library's are B, G, R, A. */
				{
					size_t count = (*width) * (*height);
					size_t i;

					for (i = 0; i < count; i++) {
						unsigned char b = pixels[i * 4u + 2u];
						unsigned char g = pixels[i * 4u + 1u];
						unsigned char r = pixels[i * 4u + 0u];
						unsigned char a = pixels[i * 4u + 3u];

						pixels[i * 4u + 0u] = b;
						pixels[i * 4u + 1u] = g;
						pixels[i * 4u + 2u] = r;
						pixels[i * 4u + 3u] = a;
					}
				}
				image = cg_image_from_bgra(pixels, *width, *height, *width * 4u, true);
				if (image != NULL && out_image != NULL) {
					*out_image = image;
					ok = true;
				} else if (image != NULL) {
					CGImageRelease(image);
				}
				break;
			}
		}
		if (status == JXL_DEC_NEED_MORE_INPUT) {
			fprintf(stderr, "CG-REFUSE: this JPEG XL file is truncated\n");
			break;
		}
		continue;
	}
	JxlDecoderDestroy(decoder);
	return ok;
}

static CGImageRef fn_jxl_decode(const unsigned char *b, size_t n, size_t index, double *delay)
{
	size_t width = 0;
	size_t height = 0;
	CGImageRef image = NULL;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_jxl_probe(b, n)) {
		return NULL;
	}
	(void)fn_jxl_run(b, n, index, true, &width, &height, &image);
	return image;
}

static void fn_jxl_size(const unsigned char *b, size_t n, size_t index, size_t *width, size_t *height)
{
	CGImageRef image = NULL;

	(void)index;
	*width = 0;
	*height = 0;
	if (!fn_jxl_probe(b, n)) {
		return;
	}
	(void)fn_jxl_run(b, n, 0, false, width, height, &image);
}

const cg_image_decoder cg_decoder_jxl = {
	"public.jpeg-xl", "jxl", fn_jxl_probe, NULL, fn_jxl_decode, fn_jxl_size
};

#else /* !AGORA_HAVE_LIBJXL */

static bool fn_jxl_absent(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

const cg_image_decoder cg_decoder_jxl = {
	"public.jpeg-xl", "jxl", fn_jxl_absent, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_LIBJXL */
