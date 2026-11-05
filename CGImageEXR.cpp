/*
 * CGImageEXR.cpp — an OpenEXR image becomes an image, through OpenEXR's own reader.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THIS IS ONE OF THE TWO C++ FILES IN THIS LIBRARY, AND THE REASON IS THE LIBRARY'S RATHER THAN OURS.** OpenEXR 3 does
 * ship a C core (`openexr.h`), but its shape is chunk-level — contexts, decoders, per-channel pixel types — and every
 * caller ends up re-implementing the scanline loop that `ImfRgbaFile` already has and already tests. So this seam uses the
 * one door that says "give me the picture as half-float RGBA", which is also the door that handles the formats OpenEXR is
 * full of: tiled files, multiple parts, deep data that cannot be RGBA at all, and the compression schemes below RGBA.
 *
 * **AND THE PICTURE IS FLOAT, WHICH A CGIMAGE IN THIS TREE IS NOT.** The conversion to 8 bits is the caller's own choice of
 * exposure: EXR's values are scene-referred and unbounded, so a straight clamp at 1.0 is what a viewer wants (it is what
 * every thumbnail does) and it is stated here rather than hidden in a gamma curve nobody asked for. Linear-to-sRGB is NOT
 * applied, because this library's images are device RGB and a viewer that wanted the display transform would ask for it.
 *
 * The interface stays C: a `cg_image_decoder` is a struct of function pointers, and these five functions are `static` C++
 * with C linkage where they need it.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>

#ifdef AGORA_HAVE_OPENEXR

#include <OpenEXR/ImfRgbaFile.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfStdIO.h>
#include <Imath/ImathBox.h>

/* **OPENEXR WANTS ITS OWN STREAM INTERFACE, NOT `std::istream`** — `StdIFStream` adapts one, and its constructor takes an
 * `std::ifstream`, so a memory buffer needs a `std::streambuf` of its own under that. This is the whole of the glue. */
namespace {
class MemoryBuffer : public std::streambuf {
public:
	MemoryBuffer(const unsigned char *data, size_t size) : start_(static_cast<char *>(std::malloc(size))), size_(size)
	{
		std::memcpy(start_, data, size);
		setg(start_, start_, start_ + size);
	}
	~MemoryBuffer() override { std::free(start_); }

protected:
	/* **SEEKING IS NOT OPTIONAL HERE**: `RgbaInputFile` reads the header and then seeks to the first scanline, so a buffer
	 * that could only read forward made every file end early — MEASURED as "Error reading pixel data ... Unexpected end of
	 * file" on a 40x30 image that ImageMagick had just written. */
	pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override
	{
		if ((which & std::ios_base::in) == 0) {
			return pos_type(off_type(-1));
		}
		off_type base = (direction == std::ios_base::beg) ? 0
			      : (direction == std::ios_base::cur) ? (gptr() - eback())
								  : static_cast<off_type>(size_);
		off_type target = base + offset;

		if (target < 0 || static_cast<size_t>(target) > size_) {
			return pos_type(off_type(-1));
		}
		setg(start_, start_ + target, start_ + size_);
		return pos_type(target);
	}
	pos_type seekpos(pos_type position, std::ios_base::openmode which) override
	{
		return seekoff(off_type(position), std::ios_base::beg, which);
	}

private:
	char *start_;
	size_t size_;
};

struct MemoryInput : public std::ifstream {
	MemoryInput(const unsigned char *data, size_t size) : buffer(data, size) { init(&buffer); }
	MemoryBuffer buffer;
};
}

extern "C" bool fn_exr_probe(const unsigned char *bytes, size_t length)
{
	return length >= 4 && bytes[0] == 0x76 && bytes[1] == 0x2f && bytes[2] == 0x31 && bytes[3] == 0x01;
}

static CGImageRef fn_exr_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	(void)index;			/* an EXR holds one picture; a multipart file's first part is the picture */
	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_exr_probe(bytes, length)) {
		return NULL;
	}
	try {
		/* NO TEMPORARY FILE: the bytes are a `std::ifstream`-shaped buffer OpenEXR's stream adapter accepts. */
		MemoryInput stream(bytes, length);
		Imf::StdIFStream input(stream, "memory.exr");
		Imf::RgbaInputFile file(input);
		Imath::Box2i window = file.dataWindow();
		const int width = window.max.x - window.min.x + 1;
		const int height = window.max.y - window.min.y + 1;
		Imf::Array2D<Imf::Rgba> pixels;

		if (width <= 0 || height <= 0) {
			return NULL;
		}
		pixels.resizeErase(height, width);
		file.setFrameBuffer(&pixels[0][0] - window.min.x - window.min.y * width, 1, width);
		file.readPixels(window.min.y, window.max.y);
		{
			unsigned char *bgra = static_cast<unsigned char *>(std::malloc(static_cast<size_t>(width) * 4u * static_cast<size_t>(height)));
			CGImageRef image;
			int y;
			bool has_alpha = false;

			if (bgra == nullptr) {
				return NULL;
			}
			for (y = 0; y < height; y++) {
				for (int x = 0; x < width; x++) {
					const Imf::Rgba &p = pixels[y][x];
					unsigned char *out = bgra + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;

					/* CLAMPED AT 1.0, ROUNDED RATHER THAN TRUNCATED: a half-float is not an 8-bit sample and a
					 * truncating cast would darken every value between the steps by up to one of them. */
					auto toByte = [](half value) -> unsigned char {
						float f = static_cast<float>(value);

						if (!(f > 0.0f)) {
							return 0;
						}
						if (f >= 1.0f) {
							return 255;
						}
						return static_cast<unsigned char>(f * 255.0f + 0.5f);
					};

					out[0] = toByte(p.b);
					out[1] = toByte(p.g);
					out[2] = toByte(p.r);
					out[3] = toByte(p.a);
					if (out[3] != 255) {
						has_alpha = true;
					}
				}
			}
			image = cg_image_from_bgra(bgra, static_cast<size_t>(width), static_cast<size_t>(height),
						   static_cast<size_t>(width) * 4u, has_alpha);
			std::free(bgra);
			return image;
		}
	} catch (const std::exception &error) {
		std::fprintf(stderr, "CG-REFUSE: OpenEXR could not read this image (%s)\n", error.what());
		return NULL;
	}
}

static void fn_exr_size(const unsigned char *bytes, size_t length, size_t index, size_t *width, size_t *height)
{
	(void)index;
	*width = 0;
	*height = 0;
	if (!fn_exr_probe(bytes, length)) {
		return;
	}
	try {
		MemoryInput stream(bytes, length);
		Imf::StdIFStream input(stream, "memory.exr");
		Imf::RgbaInputFile file(input);
		Imath::Box2i window = file.dataWindow();

		*width = static_cast<size_t>(window.max.x - window.min.x + 1);
		*height = static_cast<size_t>(window.max.y - window.min.y + 1);
	} catch (const std::exception &) {
		*width = 0;
		*height = 0;
	}
}

extern "C" const cg_image_decoder cg_decoder_exr = {
	"com.ilm.openexr-image", "exr", fn_exr_probe, NULL, fn_exr_decode, fn_exr_size
};

#else /* !AGORA_HAVE_OPENEXR */

extern "C" bool fn_exr_probe(const unsigned char *b, size_t n)
{
	(void)b;
	(void)n;
	return false;
}

extern "C" const cg_image_decoder cg_decoder_exr = {
	"com.ilm.openexr-image", "exr", fn_exr_probe, NULL, NULL, NULL
};

#endif /* AGORA_HAVE_OPENEXR */
