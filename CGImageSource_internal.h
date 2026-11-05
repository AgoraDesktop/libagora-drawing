/*
 * CGImageSource_internal.h — the shape every decoder in this build answers to.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **A DECODER IS FOUR ANSWERS ABOUT ONE FILE, AND NONE OF THEM IS "WHAT IS THIS CALLED".** The registry asks each decoder
 * whether it CLAIMS the bytes (a signature, not an extension — the same rule the AppKit's representation registry
 * follows), how many images the file holds, the size of one of them without decoding it, and then the image itself. A
 * format whose file can hold several images is why count and index exist at all: a GIF, a WebP animation, a multi-page
 * TIFF and an ICNS all do.
 *
 * THE PIXELS ARE ALWAYS THE SAME THING: 8-bit B, G, R, A in memory, STRAIGHT (not premultiplied) at the decoder's edge.
 * `cg_image_from_bgra` is the ONE place that premultiplies and builds the image, because the premultiply is this
 * library's format promise rather than any format's, and a decoder that did it its own way would be a second copy of a
 * rule that has already been measured once (see `CGImagePNG.c`).
 */
#ifndef CORE_GRAPHICS_CGIMAGESOURCE_INTERNAL_H
#define CORE_GRAPHICS_CGIMAGESOURCE_INTERNAL_H

#include <CoreGraphics/CGImage.h>

#include <stdbool.h>
#include <stddef.h>

typedef struct cg_image_decoder {
	const char *uti;		/* `public.tiff`, `com.compuserve.gif`, … */
	const char *extensions;		/* what the refusal message offers a person: "tif tiff" */
	bool (*probe)(const unsigned char *bytes, size_t length);
	size_t (*count)(const unsigned char *bytes, size_t length);
	/* `delay` is the seconds this frame is shown for, or 0 when the format has no idea. */
	CGImageRef (*decode)(const unsigned char *bytes, size_t length, size_t index, double *delay);
	/* The size WITHOUT decoding; 0x0 when the decoder cannot answer that cheaply. */
	void (*size)(const unsigned char *bytes, size_t length, size_t index, size_t *width, size_t *height);
} cg_image_decoder;

/* 8-bit B, G, R, A in memory, straight — premultiplied here and handed to `CGImageCreate`. NULL for a chart this library
 * cannot make an image of (no pixels, or a stride narrower than the width). */
CGImageRef cg_image_from_bgra(const unsigned char *bgra, size_t width, size_t height, size_t stride, bool has_alpha);

/* THE DECODERS THIS BUILD CARRIES. Each is defined in its own file, beside the format's comments; the ones that need an
 * external library are compiled only when that library was found (see CMakeLists.txt). */
extern const cg_image_decoder cg_decoder_gif;
extern const cg_image_decoder cg_decoder_bmp;
extern const cg_image_decoder cg_decoder_ico;
extern const cg_image_decoder cg_decoder_tga;
extern const cg_image_decoder cg_decoder_icns;
extern const cg_image_decoder cg_decoder_tiff;
extern const cg_image_decoder cg_decoder_webp;

/* THE DIB AN ICO WRAPS IS THE SAME STRUCTURE A BMP HOLDS, so `CGImageBMP.c` reads it once for both. `allow_png` is the
 * ICO's rule that a large entry may be a PNG instead; `out_*` are the answers the registry's `-size` door needs. */
CGImageRef cg_dib_decode(const unsigned char *dib, size_t length, size_t *out_width, size_t *out_height,
			 bool *out_has_alpha, bool allow_png);

/* PNG and JPEG keep their own provider-shaped doors (`CGImageCreateWithPNGDataProvider`, `…JPEG…`), which are Apple's
 * names and already tested; the registry wraps them rather than re-implementing two decoders. */
bool cg_png_probe(const unsigned char *bytes, size_t length);
bool cg_jpeg_probe(const unsigned char *bytes, size_t length);

/* THE INTERNAL HEADER THE PROVIDER API NEEDS, for a decoder that has to hand embedded bytes (an ICO's PNG, a TIFF's
 * samples) back to one of those doors. */
#include <CoreGraphics/CGDataProvider_internal.h>

#endif /* CORE_GRAPHICS_CGIMAGESOURCE_INTERNAL_H */
