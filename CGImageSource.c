/*
 * CGImageSource.c — the registry of decoders, and the object that hands one image out of a file.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE FORMAT IS DECIDED BY THE BYTES AND BY NOTHING ELSE.** A file called `photo.jpg` that holds a PNG opens as a PNG,
 * and one called `photo.png` that holds a GIF opens as a GIF — which is what every reader does and what the AppKit's own
 * representation registry already does for its two formats. Extensions appear in exactly one place: the refusal's text,
 * so a person who picked the wrong file learns which formats this build reads.
 *
 * **AND A DECODER IS FOUND ONCE PER SOURCE, NOT ONCE PER FRAME.** Probing for every frame of a 200-frame GIF would read
 * the signature 200 times; the source decides on creation and keeps the answer.
 */
#include <CoreGraphics/CGImageSource.h>
#include <CoreGraphics/CGImageSource_internal.h>
#include <CoreGraphics/CGImage.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const CFStringRef kCGImagePropertyPixelWidth = CFSTR("PixelWidth");
const CFStringRef kCGImagePropertyPixelHeight = CFSTR("PixelHeight");
const CFStringRef kCGImagePropertyGIFDictionary = CFSTR("{GIF}");
const CFStringRef kCGImagePropertyGIFDelayTime = CFSTR("DelayTime");
const CFStringRef kCGImagePropertyGIFUnclampedDelayTime = CFSTR("UnclampedDelayTime");

/* PNG AND JPEG ARE THE TWO DECODERS THAT ALREADY EXISTED, and their doors take a provider rather than bytes. These two
 * helpers are the whole of the registry's knowledge of them, so there is one decoder per format in the tree and not two
 * spellings of the same one. */
static bool fn_png_probe(const unsigned char *b, size_t n)
{
	return n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
}

static bool fn_jpeg_probe(const unsigned char *b, size_t n)
{
	return n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff;
}

/* `CGDataProviderCreateWithData` releases its bytes with a callback of this shape, and `free` does not have it. */
static void fn_free_bytes(void *info, const void *data, size_t size)
{
	(void)info;
	(void)size;
	free((void *)data);
}

static CGImageRef fn_provider_decode(CGImageRef (*decoder)(CGDataProviderRef, const CGFloat *, bool,
							  CGColorRenderingIntent),
				     const unsigned char *bytes, size_t length)
{
	unsigned char *copy;
	CGDataProviderRef provider;
	CGImageRef image;

	if (decoder == NULL || bytes == NULL || length == 0) {
		return NULL;
	}
	copy = malloc(length);
	if (copy == NULL) {
		return NULL;
	}
	memcpy(copy, bytes, length);
	provider = CGDataProviderCreateWithData(NULL, copy, length, fn_free_bytes);
	if (provider == NULL) {
		free(copy);
		return NULL;
	}
	image = decoder(provider, NULL, true, kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	return image;
}

static CGImageRef fn_png_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	if (delay != NULL) {
		*delay = 0.0;
	}
	return (index == 0) ? fn_provider_decode(CGImageCreateWithPNGDataProvider, bytes, length) : NULL;
}

static CGImageRef fn_jpeg_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	if (delay != NULL) {
		*delay = 0.0;
	}
	return (index == 0) ? fn_provider_decode(CGImageCreateWithJPEGDataProvider, bytes, length) : NULL;
}

static size_t fn_one_image(const unsigned char *bytes, size_t length)
{
	(void)bytes;
	(void)length;
	return 1;
}

static void fn_size_unknown(const unsigned char *b, size_t n, size_t i, size_t *w, size_t *h)
{
	(void)b;
	(void)n;
	(void)i;
	*w = 0;
	*h = 0;
}

static const cg_image_decoder fn_png = {
	"public.png", "png", fn_png_probe, fn_one_image, fn_png_decode, fn_size_unknown
};

static const cg_image_decoder fn_jpeg = {
	"public.jpeg", "jpg jpeg jpe", fn_jpeg_probe, fn_one_image, fn_jpeg_decode, fn_size_unknown
};

/* **THE ORDER IS THE MESSAGE'S, AND IT IS THE ONLY ORDER THAT MATTERS.** Signatures are self-identifying (a PNG starts
 * with 0x89 P N G and a GIF with `GIF8`), so no probe can claim another format's file; the order is alphabetical-after-
 * PNG-and-JPEG only so that `-CopyTypeIdentifiers` reads the way a person would write the list. */
static const cg_image_decoder *const fn_decoders[] = {
	&fn_png,
	&fn_jpeg,
	&cg_decoder_tiff,
	&cg_decoder_webp,
	&cg_decoder_gif,
	&cg_decoder_bmp,
	&cg_decoder_ico,
	&cg_decoder_icns,
	&cg_decoder_tga
};

#define FN_DECODER_COUNT (sizeof fn_decoders / sizeof fn_decoders[0])

struct CGImageSource {
	CFDataRef data;			/* RETAINED: the file's bytes, which every frame decodes from */
	const cg_image_decoder *decoder;	/* chosen once, at creation */
};

/* THE ONE PLACE THAT PREMULTIPLIES AND BUILDS THE IMAGE, so every decoder hands back pixels in one layout and this
 * library's promise — premultiplied alpha — is kept once. */
CGImageRef cg_image_from_bgra(const unsigned char *bgra, size_t width, size_t height, size_t stride, bool has_alpha)
{
	unsigned char *pixels;
	size_t row;
	CGDataProviderRef provider;
	CGImageRef image;

	if (bgra == NULL || width == 0 || height == 0 || stride < width * 4) {
		fprintf(stderr, "CG-REFUSE: a decoder produced a %zux%zu image with a %zu-byte stride\n", width, height,
			stride);
		return NULL;
	}
	pixels = malloc(width * 4 * height);
	if (pixels == NULL) {
		return NULL;
	}
	for (row = 0; row < height; row++) {
		const unsigned char *source = bgra + row * stride;
		unsigned char *target = pixels + row * width * 4;
		size_t x;

		if (!has_alpha) {
			for (x = 0; x < width; x++) {
				target[x * 4 + 0] = source[x * 4 + 0];
				target[x * 4 + 1] = source[x * 4 + 1];
				target[x * 4 + 2] = source[x * 4 + 2];
				target[x * 4 + 3] = 255;
			}
			continue;
		}
		for (x = 0; x < width; x++) {
			unsigned int alpha = source[x * 4 + 3];

			target[x * 4 + 0] = (unsigned char)(((unsigned int)source[x * 4 + 0] * alpha + 127u) / 255u);
			target[x * 4 + 1] = (unsigned char)(((unsigned int)source[x * 4 + 1] * alpha + 127u) / 255u);
			target[x * 4 + 2] = (unsigned char)(((unsigned int)source[x * 4 + 2] * alpha + 127u) / 255u);
			target[x * 4 + 3] = (unsigned char)alpha;
		}
	}
	provider = CGDataProviderCreateWithData(NULL, pixels, width * 4 * height, fn_free_bytes);
	if (provider == NULL) {
		free(pixels);
		return NULL;
	}
	image = CGImageCreate(width, height, 8, 32, width * 4, CGColorSpaceCreateDeviceRGB(),
			      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, provider, NULL, true,
			      kCGRenderingIntentDefault);
	CGDataProviderRelease(provider);
	return image;
}

CGImageSourceRef CGImageSourceCreateWithData(CFDataRef data, CFDictionaryRef options)
{
	CGImageSourceRef source;
	const unsigned char *bytes;
	size_t length;
	size_t i;

	(void)options;
	if (data == NULL || CFDataGetLength(data) <= 0) {
		return NULL;
	}
	bytes = CFDataGetBytePtr(data);
	length = (size_t)CFDataGetLength(data);
	source = calloc(1, sizeof *source);
	if (source == NULL) {
		return NULL;
	}
	for (i = 0; i < FN_DECODER_COUNT; i++) {
		const cg_image_decoder *decoder = fn_decoders[i];

		if (decoder != NULL && decoder->probe != NULL && decoder->probe(bytes, length)) {
			source->decoder = decoder;
			break;
		}
	}
	if (source->decoder == NULL) {
		fprintf(stderr, "CG-REFUSE: no image decoder in this build claims these bytes; it reads:\n");
		for (i = 0; i < FN_DECODER_COUNT; i++) {
			const cg_image_decoder *decoder = fn_decoders[i];

			if (decoder != NULL) {
				fprintf(stderr, "  %s (%s)\n", decoder->uti, decoder->extensions);
			}
		}
		free(source);
		return NULL;
	}
	source->data = CFDataCreateCopy(kCFAllocatorDefault, data);
	return source;
}

CGImageSourceRef CGImageSourceCreateWithDataProvider(CGDataProviderRef provider, CFDictionaryRef options)
{
	const void *bytes;
	size_t length = 0;
	CFDataRef data;
	CGImageSourceRef source;

	if (provider == NULL) {
		return NULL;
	}
	bytes = cg_dataprovider_bytes(provider, &length);
	if (bytes == NULL || length == 0) {
		return NULL;
	}
	data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)bytes, (CFIndex)length);
	if (data == NULL) {
		return NULL;
	}
	source = CGImageSourceCreateWithData(data, options);
	CFRelease(data);
	return source;
}

CFStringRef CGImageSourceGetType(CGImageSourceRef source)
{
	if (source == NULL || source->decoder == NULL) {
		return NULL;
	}
	return CFStringCreateWithCString(kCFAllocatorDefault, source->decoder->uti, kCFStringEncodingUTF8);
}

size_t CGImageSourceGetCount(CGImageSourceRef source)
{
	const unsigned char *bytes;
	size_t count;

	if (source == NULL || source->data == NULL || source->decoder == NULL) {
		return 0;
	}
	if (source->decoder->count == NULL) {
		return 1;
	}
	bytes = CFDataGetBytePtr(source->data);
	count = source->decoder->count(bytes, (size_t)CFDataGetLength(source->data));
	return (count == 0) ? 1 : count;
}

CGImageRef CGImageSourceCreateImageAtIndex(CGImageSourceRef source, size_t index, CFDictionaryRef options)
{
	(void)options;
	if (source == NULL || source->data == NULL || source->decoder == NULL || source->decoder->decode == NULL) {
		return NULL;
	}
	if (index >= CGImageSourceGetCount(source)) {
		fprintf(stderr, "CG-REFUSE: this source holds %zu image(s), so index %zu is past the end\n",
			CGImageSourceGetCount(source), index);
		return NULL;
	}
	return source->decoder->decode(CFDataGetBytePtr(source->data), (size_t)CFDataGetLength(source->data), index,
				       NULL);
}

CFDictionaryRef CGImageSourceCopyPropertiesAtIndex(CGImageSourceRef source, size_t index, CFDictionaryRef options)
{
	CFMutableDictionaryRef properties;
	CFMutableDictionaryRef gif;

	(void)options;
	if (source == NULL || source->data == NULL || source->decoder == NULL) {
		return NULL;
	}
	properties = CFDictionaryCreateMutable(kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
					       &kCFTypeDictionaryValueCallBacks);
	if (properties == NULL) {
		return NULL;
	}
	if (source->decoder->size != NULL) {
		size_t width = 0;
		size_t height = 0;

		source->decoder->size(CFDataGetBytePtr(source->data), (size_t)CFDataGetLength(source->data), index,
				      &width, &height);
		if (width > 0 && height > 0) {
			CFNumberRef w = CFNumberCreate(kCFAllocatorDefault, kCFNumberCFIndexType, &width);
			CFNumberRef h = CFNumberCreate(kCFAllocatorDefault, kCFNumberCFIndexType, &height);

			if (w != NULL) {
				CFDictionarySetValue(properties, kCGImagePropertyPixelWidth, w);
				CFRelease(w);
			}
			if (h != NULL) {
				CFDictionarySetValue(properties, kCGImagePropertyPixelHeight, h);
				CFRelease(h);
			}
		}
	}
	/* THE FRAME'S DELAY, WHICH IS THE ONE PROPERTY AN ANIMATION NEEDS. A decoder that knows it answers through the same
	 * decode door (the frame is decoded to be measured) — a still image answers 0 and the key is simply absent. */
	if (source->decoder->decode != NULL && CGImageSourceGetCount(source) > 1) {
		double delay = 0.0;
		CGImageRef frame = source->decoder->decode(CFDataGetBytePtr(source->data),
							  (size_t)CFDataGetLength(source->data), index, &delay);

		if (frame != NULL) {
			CGImageRelease(frame);
		}
		if (delay > 0.0) {
			CFNumberRef seconds = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &delay);

			gif = CFDictionaryCreateMutable(kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
							&kCFTypeDictionaryValueCallBacks);
			if (seconds != NULL && gif != NULL) {
				CFDictionarySetValue(gif, kCGImagePropertyGIFDelayTime, seconds);
				CFDictionarySetValue(gif, kCGImagePropertyGIFUnclampedDelayTime, seconds);
				CFDictionarySetValue(properties, kCGImagePropertyGIFDictionary, gif);
			}
			if (seconds != NULL) {
				CFRelease(seconds);
			}
			if (gif != NULL) {
				CFRelease(gif);
			}
		}
	}
	return properties;
}

/* **THE MEASURED ANSWER TO "WHAT FORMATS DOES THIS BUILD READ", which is what a bundle's declared types are checked
 * against.** A decoder whose library was not found at build time is not in the registry, so it is not in this list —
 * the omission is the honest statement, not a promise the application cannot keep. */
CFArrayRef CGImageSourceCopyTypeIdentifiers(void)
{
	CFMutableArrayRef list = CFArrayCreateMutable(kCFAllocatorDefault, (CFIndex)FN_DECODER_COUNT,
						      &kCFTypeArrayCallBacks);
	size_t i;

	if (list == NULL) {
		return NULL;
	}
	for (i = 0; i < FN_DECODER_COUNT; i++) {
		const cg_image_decoder *decoder = fn_decoders[i];

		if (decoder != NULL) {
			CFStringRef uti = CFStringCreateWithCString(kCFAllocatorDefault, decoder->uti,
								    kCFStringEncodingUTF8);

			if (uti != NULL) {
				CFArrayAppendValue(list, uti);
				CFRelease(uti);
			}
		}
	}
	return list;
}

void CGImageSourceRetain(CGImageSourceRef source)
{
	(void)source;	/* a source is not reference counted in this build: it is created, used and released once */
}

void CGImageSourceRelease(CGImageSourceRef source)
{
	if (source == NULL) {
		return;
	}
	if (source->data != NULL) {
		CFRelease(source->data);
	}
	free(source);
}
