/*
 * CGImageRAW.cpp — a camera's raw file becomes an image, through LibRaw.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THERE IS NO "RAW FORMAT"; THERE ARE A HUNDRED CAMERAS' CONTAINERS AND ONE LIBRARY THAT KNOWS THEM.** LibRaw is that
 * library, it is C++, and this file is the seam: `open_buffer` reads the caller's bytes without a temporary file,
 * `unpack` and `dcraw_process` do the demosaic and the exposure the way every raw converter does, and
 * `dcraw_make_mem_image` hands back an 8-bit RGB bitmap.
 *
 * **THE PROBE IS A TABLE OF SIGNATURES AND NOT A GUESS.** Most containers are TIFF-shaped (`II*\0` plus a vendor tag), and
 * a TIFF probe that accepted them would claim every camera raw file as a TIFF — which decodes as the embedded preview at
 * best. So the well-known non-TIFF magics are named here, the TIFF-shaped ones are told apart by their vendor tag at byte
 * 8, and LibRaw does the final identification in `open_buffer`: a file this table claims but LibRaw declines is refused by
 * name rather than drawn as garbage.
 *
 * WHAT IS DELIBERATELY NOT DONE: a demosaic tuned for photography (LibRaw's default is used), and colour management beyond
 * LibRaw's own camera matrix. A viewer shows what the camera recorded; a converter is a different program.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef AGORA_HAVE_LIBRAW

#include <libraw/libraw.h>

namespace {
/* THE SIGNATURES. `offset` and `magic` are compared as bytes so that a probe never reads past the buffer. */
struct RawSignature {
	size_t offset;
	const char *magic;
	size_t length;
};

const RawSignature kSignatures[] = {
	{ 8, "CR\x02\x00", 4 },		/* Canon CR2: a TIFF with this at byte 8 */
	{ 8, "CR\x03\x00", 4 },
	{ 4, "ftypcrx ", 8 },		/* Canon CR3 */
	{ 4, "ftypheic", 8 },		/* not raw, but some HEIF files carry a raw item */
	{ 0, "FUJIFILMCCD-RAW", 15 },	/* Fujifilm RAF */
	{ 0, "IIRO", 4 },		/* Olympus ORF */
	{ 0, "IIRS", 4 },
	{ 0, "MMOR", 4 },
	{ 0, "IIU\x00", 4 },		/* Pentax PEF */
	{ 0, "FOVb", 4 },		/* Sigma X3F */
	{ 0, "\x00MRM", 4 },		/* Minolta MRW */
	{ 0, "II\x1a\x00", 4 },		/* Phase One IIQ */
	{ 0, "MM\x00\x2a", 4 }		/* a TIFF-shaped raw: disambiguated below */
};
} /* namespace */

extern "C" bool fn_raw_probe(const unsigned char *bytes, size_t length)
{
	size_t i;

	for (i = 0; i < sizeof kSignatures / sizeof kSignatures[0]; i++) {
		const RawSignature &signature = kSignatures[i];

		if (length < signature.offset + signature.length) {
			continue;
		}
		if (std::memcmp(bytes + signature.offset, signature.magic, signature.length) != 0) {
			continue;
		}
		/* **THE TIFF-SHAPED ONES ARE THE COMMON CASE AND THE AMBIGUOUS ONE.** A plain TIFF and a NEF/DNG/ARW share
		 * `MM\0*`; LibRaw's own open is what tells them apart, and it is cheap enough for a signature question
		 * because it stops at the header. */
		if (signature.offset == 0 && (signature.magic[0] == 'M' || signature.magic[0] == 'I')) {
			LibRaw probe;

			if (probe.open_buffer(const_cast<unsigned char *>(bytes), length) != LIBRAW_SUCCESS) {
				return false;
			}
			{
				bool is_raw = probe.imgdata.idata.filters != 0 || probe.imgdata.idata.colors > 3 ||
					      probe.imgdata.rawdata.raw_image != nullptr;

				probe.recycle();
				return is_raw;
			}
		}
		return true;
	}
	return false;
}

static CGImageRef fn_raw_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	LibRaw raw;
	int status;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (index != 0) {
		return NULL;
	}
	status = raw.open_buffer(const_cast<unsigned char *>(bytes), length);
	if (status != LIBRAW_SUCCESS) {
		std::fprintf(stderr, "CG-REFUSE: LibRaw could not identify this file (%s)\n", libraw_strerror(status));
		return NULL;
	}
	if ((status = raw.unpack()) != LIBRAW_SUCCESS) {
		std::fprintf(stderr, "CG-REFUSE: LibRaw could not unpack a raw file (%s)\n", libraw_strerror(status));
		raw.recycle();
		return NULL;
	}
	if ((status = raw.dcraw_process()) != LIBRAW_SUCCESS) {
		std::fprintf(stderr, "CG-REFUSE: LibRaw could not develop a raw file (%s)\n", libraw_strerror(status));
		raw.recycle();
		return NULL;
	}
	{
		libraw_processed_image_t *image = raw.dcraw_make_mem_image(&status);

		if (image == NULL || status != LIBRAW_SUCCESS || image->type != LIBRAW_IMAGE_BITMAP || image->colors != 3) {
			std::fprintf(stderr, "CG-REFUSE: LibRaw produced no 8-bit RGB bitmap\n");
			if (image != NULL) {
				LibRaw::dcraw_clear_mem(image);
			}
			raw.recycle();
			return NULL;
		}
		{
			size_t width = image->width;
			size_t height = image->height;
			unsigned char *bgra = static_cast<unsigned char *>(std::malloc(width * 4u * height));
			CGImageRef result = NULL;
			size_t y;

			if (bgra != nullptr) {
				for (y = 0; y < height; y++) {
					const unsigned char *in = image->data + y * width * 3u;
					unsigned char *out = bgra + y * width * 4u;
					size_t x;

					for (x = 0; x < width; x++) {
						out[x * 4u + 0u] = in[x * 3u + 2u];
						out[x * 4u + 1u] = in[x * 3u + 1u];
						out[x * 4u + 2u] = in[x * 3u + 0u];
						out[x * 4u + 3u] = 255;
					}
				}
				result = cg_image_from_bgra(bgra, width, height, width * 4u, false);
				std::free(bgra);
			}
			LibRaw::dcraw_clear_mem(image);
			raw.recycle();
			return result;
		}
	}
}

static void fn_raw_size(const unsigned char *bytes, size_t length, size_t index, size_t *width, size_t *height)
{
	LibRaw raw;

	(void)index;
	*width = 0;
	*height = 0;
	if (raw.open_buffer(const_cast<unsigned char *>(bytes), length) != LIBRAW_SUCCESS) {
		raw.recycle();
		return;
	}
	*width = static_cast<size_t>(raw.imgdata.sizes.width);
	*height = static_cast<size_t>(raw.imgdata.sizes.height);
	raw.recycle();
}

extern "C" const cg_image_decoder cg_decoder_raw = {
	"public.camera-raw-image", "cr2 cr3 nef arw orf rw2 raf dng pef srw x3f mrw iiq", fn_raw_probe, NULL,
	fn_raw_decode, fn_raw_size
};

#else /* !AGORA_HAVE_LIBRAW */

extern "C" bool fn_raw_probe(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

extern "C" const cg_image_decoder cg_decoder_raw = {
	"public.camera-raw-image", "cr2 cr3 nef arw orf rw2 raf dng pef srw x3f mrw iiq", fn_raw_probe, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_LIBRAW */
