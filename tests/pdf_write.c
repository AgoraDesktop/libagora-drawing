/*
 * pdf_write.c — the PDF context's round trip: draw, close, reopen, render, compare the bytes.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **A WRITER IS ONLY PROVED BY ITS OWN READER.** The surface plan's gate for G2 is exactly this loop — create a
 * context, draw, close, reopen, count pages, read metadata, render a page into a bitmap and compare a pixel — and
 * this case is that gate with every kind of thing the recorder claims to support in it: a filled path, text as
 * glyph outlines, an opaque image, an image with alpha (which becomes a /SMask), a fill with an alpha constant
 * (which becomes an /ExtGState), and a clip that must NOT leak past its `Q`.
 *
 * THE FILE IS BUILT IN MEMORY over a CFMutableData consumer, then reopened through CGPDFDocument and drawn with
 * the renderer that landed last. Every assertion is a pixel read back from the second drawing, so a writer bug
 * and a renderer bug cannot cancel each other out silently; the page's structural facts (page count, media boxes,
 * the title) are asked of the reader separately.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGPDFArray.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFObject.h>
#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGPDFStream.h>
#include <CoreGraphics/CGPDFString.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fprintf(stderr, "\n"); \
		} \
	} while (0)

static void pixel_at(const unsigned char *buffer, size_t width, size_t height, CGPoint point,
		     unsigned char out[4])
{
	int x = (int)point.x;
	int y = (int)(height - 1.0 - point.y);

	if (x < 0 || y < 0 || (size_t)x >= width || (size_t)y >= height) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}
	memcpy(out, buffer + ((size_t)y * width + (size_t)x) * 4, 4);
}

static CFDataRef fn_image_data(int alpha)
{
	/* FOUR PIXELS, ROW-MAJOR, IN R,G,B(,A) ORDER: red, green / blue, white. */
	static const unsigned char opaque[16] = {
		255, 0, 0, 0,    0, 255, 0, 0,
		0, 0, 255, 0,    255, 255, 255, 0
	};
	static const unsigned char alpha_data[8] = {
		255, 0, 0, 255,
		0, 0, 0, 0
	};

	if (alpha) {
		return CFDataCreate(kCFAllocatorDefault, alpha_data, sizeof alpha_data);
	}
	return CFDataCreate(kCFAllocatorDefault, opaque, sizeof opaque);
}

static CGImageRef fn_make_image(int alpha)
{
	CFDataRef data = fn_image_data(alpha);
	CGDataProviderRef provider;
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGImageRef image;

	provider = CGDataProviderCreateWithCFData(data);
	CFRelease(data);
	if (provider == NULL || space == NULL) {
		return NULL;
	}
	if (alpha) {
		image = CGImageCreate(2, 1, 8, 32, 8, space,
				      kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider, NULL, false,
				      kCGRenderingIntentDefault);
	} else {
		image = CGImageCreate(2, 2, 8, 32, 8, space,
				      kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big, provider, NULL, false,
				      kCGRenderingIntentDefault);
	}
	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
	return image;
}

static CFDataRef fn_build_pdf(void)
{
	CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
	CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(data);
	CGRect media = CGRectMake(0, 0, 200, 100);
	CFMutableDictionaryRef auxiliary = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
								    &kCFTypeDictionaryKeyCallBacks,
								    &kCFTypeDictionaryValueCallBacks);
	CFStringRef title = CFSTR("Round Trip");
	CFDataRef xmp = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)"<x:xmpmeta>agora</x:xmpmeta>", 26);
	CGContextRef ctx;
	CGFontRef font;
	CGImageRef image;
	CGImageRef alpha_image;
	CFDataRef page_box;

	CFDictionarySetValue(auxiliary, kCGPDFContextTitle, title);
	CFDictionarySetValue(auxiliary, kCGPDFContextCreator, CFSTR("agora-drawing's PDF context"));
	ctx = CGPDFContextCreate(consumer, &media, auxiliary);
	CHECK(ctx != NULL, "CGPDFContextCreate refused");
	if (ctx == NULL) {
		CFRelease(auxiliary);
		CGDataConsumerRelease(consumer);
		CFRelease(data);
		return NULL;
	}
	CGPDFContextAddDocumentMetadata(ctx, xmp);

	/* PAGE ONE */
	CGPDFContextBeginPage(ctx, NULL);
	CGContextSetRGBFillColor(ctx, 0, 0, 1, 1);
	CGContextFillRect(ctx, CGRectMake(10, 10, 50, 30));

	font = CGFontCreateWithFontName(CFSTR("Helvetica"));
	if (font != NULL) {
		CGContextSetFont(ctx, font);
		CGContextSetFontSize(ctx, 20);
		CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
		CGContextSetTextPosition(ctx, 20, 60);
		CGContextShowText(ctx, "Hi", 2);
		/* **A COMPOSITE GLYPH, because `é` is drawn from `e` plus `acute` and the subsetter must carry both
		 * components and rewrite their indices.** The ink above the baseline is the accent; if only the base
		 * letter arrived the upper box stays empty. */
		{
			CGGlyph glyph = CGFontGetGlyphWithGlyphName(font, CFSTR("eacute"));

			if (glyph != 0 && glyph != (CGGlyph)kCGFontIndexInvalid) {
				CGContextSetTextPosition(ctx, 20, 80);
				CGContextShowGlyphs(ctx, &glyph, 1);
			} else {
				CHECK(0, "Liberation Sans has no `eacute` glyph to test the composite closure with");
			}
		}
		CGFontRelease(font);
	} else {
		CHECK(0, "no Helvetica for the PDF context's text");
	}

	image = fn_make_image(0);
	if (image != NULL) {
		CGContextDrawImage(ctx, CGRectMake(110, 10, 20, 20), image);
		CGImageRelease(image);
	}
	alpha_image = fn_make_image(1);
	if (alpha_image != NULL) {
		CGContextDrawImage(ctx, CGRectMake(140, 10, 20, 10), alpha_image);
		CGImageRelease(alpha_image);
	}

	/* A HALF-TRANSPARENT FILL: /ca 0.5 in an /ExtGState, not a changed colour. */
	CGContextSetRGBFillColor(ctx, 1, 0, 0, 0.5);
	CGContextFillRect(ctx, CGRectMake(70, 10, 20, 20));

	/* A CLIP INSIDE A SAVED STATE, so the restored state must not keep it. */
	CGContextSaveGState(ctx);
	CGContextClipToRect(ctx, CGRectMake(150, 50, 30, 20));
	CGContextSetRGBFillColor(ctx, 0, 1, 0, 1);
	CGContextFillRect(ctx, CGRectMake(140, 40, 60, 40));
	CGContextRestoreGState(ctx);
	CGPDFContextEndPage(ctx);

	/* PAGE TWO, with its own media box in the page-info dictionary. */
	{
		CFMutableDictionaryRef info = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
								       &kCFTypeDictionaryKeyCallBacks,
								       &kCFTypeDictionaryValueCallBacks);
		CGRect box = CGRectMake(0, 0, 100, 50);

		page_box = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)&box, (CFIndex)sizeof box);
		CFDictionarySetValue(info, kCGPDFContextMediaBox, page_box);
		CGPDFContextBeginPage(ctx, info);
		CGContextSetRGBFillColor(ctx, 0, 1, 0, 1);
		CGContextFillRect(ctx, CGRectMake(0, 0, 100, 50));
		CGPDFContextEndPage(ctx);
		CFRelease(page_box);
		CFRelease(info);
	}

	CGPDFContextClose(ctx);
	CGContextRelease(ctx);
	CFRelease(xmp);
	CFRelease(auxiliary);
	CGDataConsumerRelease(consumer);
	return data;
}

/* WHAT A FONT RESOURCE SHOULD BE: a Type0 font whose descendant is a CIDFontType2 with a /FontFile2 behind it.
 * This is the structural half of "the text is text" — the pixel half below only says the glyphs landed. */
struct fn_font_probe {
	int count;
	int type0;
	int embedded;
};

static void fn_font_entry(const char *key, CGPDFObjectRef value, void *info)
{
	struct fn_font_probe *probe = info;
	CGPDFDictionaryRef font = NULL;
	CGPDFArrayRef descendants = NULL;
	CGPDFDictionaryRef descendant = NULL;
	CGPDFDictionaryRef descriptor = NULL;
	CGPDFStreamRef file = NULL;
	const char *subtype = NULL;

	(void)key;
	probe->count++;
	if (CGPDFObjectGetType(value) != kCGPDFObjectTypeDictionary ||
	    !CGPDFObjectGetValue(value, kCGPDFObjectTypeDictionary, &font)) {
		return;
	}
	if (CGPDFDictionaryGetName(font, "Subtype", &subtype) && strcmp(subtype, "Type0") == 0) {
		probe->type0++;
	}
	if (CGPDFDictionaryGetArray(font, "DescendantFonts", &descendants) &&
	    CGPDFArrayGetDictionary(descendants, 0, &descendant) &&
	    CGPDFDictionaryGetDictionary(descendant, "FontDescriptor", &descriptor) &&
	    CGPDFDictionaryGetStream(descriptor, "FontFile2", &file)) {
		probe->embedded++;
	}
}

static void fn_check_page_one(CGPDFDocumentRef document)
{
	CGPDFPageRef page = CGPDFDocumentGetPage(document, 1);
	unsigned char buffer[200 * 100 * 4];
	unsigned char px[4];
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	int text_ink = 0;
	size_t x;
	size_t y;

	CHECK(page != NULL, "the written document has no page 1");
	if (page == NULL) {
		CGColorSpaceRelease(rgb);
		return;
	}
	{
		CGPDFDictionaryRef page_dict = CGPDFPageGetDictionary(page);
		CGPDFDictionaryRef resources = NULL;
		CGPDFDictionaryRef fonts = NULL;
		struct fn_font_probe probe = { 0, 0, 0 };

		CHECK(page_dict != NULL && CGPDFDictionaryGetDictionary(page_dict, "Resources", &resources),
		      "the written page has no /Resources");
		CHECK(resources != NULL && CGPDFDictionaryGetDictionary(resources, "Font", &fonts),
		      "the written page carries no /Font resource");
		if (fonts != NULL) {
			CGPDFDictionaryApply(fonts, fn_font_entry, &probe);
		}
		CHECK(probe.count > 0, "the page's /Font dictionary is empty");
		CHECK(probe.type0 > 0, "the written text is not a Type0 font");
		CHECK(probe.embedded > 0, "the written font has no embedded /FontFile2");
	}
	{
		CGPDFDictionaryRef page_dict = CGPDFPageGetDictionary(page);
		CGPDFStreamRef contents = NULL;

		CHECK(page_dict != NULL && CGPDFDictionaryGetStream(page_dict, "Contents", &contents),
		      "the page has no content stream");
		if (contents != NULL) {
			CGPDFDataFormat format;
			CFDataRef bytes = CGPDFStreamCopyData(contents, &format);

			CHECK(bytes != NULL, "the page's content stream did not decode");
			if (bytes != NULL) {
				const char *text = (const char *)CFDataGetBytePtr(bytes);
				size_t length = (size_t)CFDataGetLength(bytes);
				int has_tj = 0;
				size_t i;

				for (i = 0; i + 2 <= length; i++) {
					if (text[i] == 'T' && text[i + 1] == 'j') {
						has_tj = 1;
						break;
					}
				}
				CHECK(has_tj, "the written text was recorded as OUTLINES, not a Tj operator");
				CFRelease(bytes);
			}
		}
	}
	memset(buffer, 0, sizeof buffer);
	ctx = CGBitmapContextCreate(buffer, 200, 100, 8, 200 * 4, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CHECK(ctx != NULL, "the round-trip render target was refused");
	if (ctx == NULL) {
		CGColorSpaceRelease(rgb);
		return;
	}
	CGContextDrawPDFPage(ctx, page);

	pixel_at(buffer, 200, 100, CGPointMake(35, 25), px);
	CHECK(px[3] > 200 && px[0] > 200 && px[1] < 60 && px[2] < 60,
	      "the written rectangle is B=%u G=%u R=%u A=%u, expected opaque blue", px[0], px[1], px[2], px[3]);

	for (y = 61; y < 78 && !text_ink; y++) {
		for (x = 20; x < 45; x++) {
			pixel_at(buffer, 200, 100, CGPointMake((CGFloat)x, (CGFloat)y), px);
			if (px[3] > 100 && px[0] < 100 && px[1] < 100 && px[2] < 100) {
				text_ink = 1;
				break;
			}
		}
	}
	CHECK(text_ink, "the written text produced no ink");

	pixel_at(buffer, 200, 100, CGPointMake(115, 25), px);
	CHECK(px[3] > 200 && px[2] > 200 && px[0] < 60 && px[1] < 60,
	      "the written image's red sample is B=%u G=%u R=%u A=%u", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, 200, 100, CGPointMake(115, 15), px);
	CHECK(px[3] > 200 && px[0] > 200 && px[1] < 60 && px[2] < 60,
	      "the written image's blue sample is B=%u G=%u R=%u A=%u", px[0], px[1], px[2], px[3]);

	pixel_at(buffer, 200, 100, CGPointMake(145, 15), px);
	CHECK(px[3] > 200 && px[2] > 200 && px[0] < 60 && px[1] < 60,
	      "the image's alpha half is not opaque red (B=%u G=%u R=%u A=%u)", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, 200, 100, CGPointMake(155, 15), px);
	CHECK(px[3] < 30, "the image's transparent half was painted (A=%u)", px[3]);

	{
		int accent_ink = 0;
		int body_ink = 0;

		for (x = 20; x < 40; x++) {
			for (y = 80; y < 97; y++) {
				pixel_at(buffer, 200, 100, CGPointMake((CGFloat)x, (CGFloat)y), px);
				if (px[3] > 100 && px[0] < 100 && px[1] < 100 && px[2] < 100) {
					if (y >= 91) {
						accent_ink = 1;
					} else {
						body_ink = 1;
					}
				}
			}
		}
		CHECK(body_ink, "the composite glyph's base letter produced no ink");
		CHECK(accent_ink, "the composite glyph's accent produced no ink — its component was not subsetted");
	}

	pixel_at(buffer, 200, 100, CGPointMake(80, 20), px);
	/* THE DESTINATION IS PREMULTIPLIED, so a 50% red over transparent is R=A=128, not R=255. */
	CHECK(px[3] > 60 && px[3] < 220 && px[2] > 100 && px[2] < 180 && px[0] < 60 && px[1] < 60,
	      "the half-transparent fill is B=%u G=%u R=%u A=%u, expected half red", px[0], px[1], px[2], px[3]);

	pixel_at(buffer, 200, 100, CGPointMake(160, 60), px);
	CHECK(px[3] > 200 && px[1] > 200 && px[0] < 60 && px[2] < 60,
	      "the clipped fill did not reach its clip (B=%u G=%u R=%u A=%u)", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, 200, 100, CGPointMake(145, 60), px);
	CHECK(px[3] < 30, "the clip leaked outside its rectangle (A=%u)", px[3]);

	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
}

static void fn_check_page_two(CGPDFDocumentRef document)
{
	CGPDFPageRef page = CGPDFDocumentGetPage(document, 2);
	CGRect box = page != NULL ? CGPDFPageGetBoxRect(page, kCGPDFMediaBox) : CGRectZero;
	unsigned char buffer[100 * 50 * 4];
	unsigned char px[4];
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;

	CHECK(page != NULL, "the written document has no page 2");
	if (page == NULL) {
		return;
	}
	CHECK((int)box.size.width == 100 && (int)box.size.height == 50,
	      "page 2's media box is %gx%g, expected 100x50", (double)box.size.width, (double)box.size.height);
	memset(buffer, 0, sizeof buffer);
	ctx = CGBitmapContextCreate(buffer, 100, 50, 8, 100 * 4, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	if (ctx == NULL) {
		CGColorSpaceRelease(rgb);
		return;
	}
	CGContextDrawPDFPage(ctx, page);
	pixel_at(buffer, 100, 50, CGPointMake(50, 25), px);
	CHECK(px[3] > 200 && px[1] > 200 && px[0] < 60 && px[2] < 60,
	      "page 2 is B=%u G=%u R=%u A=%u, expected opaque green", px[0], px[1], px[2], px[3]);
	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
}

int main(void)
{
	CFDataRef data = fn_build_pdf();
	const unsigned char *bytes;
	CGDataProviderRef provider;
	CGPDFDocumentRef document;
	CGPDFDictionaryRef info;
	CGPDFDictionaryRef catalog;
	CGPDFStringRef title = NULL;
	CFStringRef text = NULL;

	CHECK(data != NULL, "the PDF context produced no data");
	if (data == NULL) {
		return 1;
	}
	bytes = CFDataGetBytePtr(data);
	CHECK(CFDataGetLength(data) > 100 && memcmp(bytes, "%PDF-1.4", 8) == 0,
	      "the written data does not begin with a PDF header");
	CHECK(memcmp(bytes + CFDataGetLength(data) - 6, "%%EOF\n", 6) == 0 ||
	      memcmp(bytes + CFDataGetLength(data) - 5, "%%EOF", 5) == 0,
	      "the written data does not end with %%EOF");

	provider = CGDataProviderCreateWithCFData(data);
	document = CGPDFDocumentCreateWithProvider(provider);
	CHECK(document != NULL, "the written document did not reopen");
	if (document == NULL) {
		CGDataProviderRelease(provider);
		CFRelease(data);
		return 1;
	}
	CHECK(CGPDFDocumentGetNumberOfPages(document) == 2, "the document has %zu pages, expected 2",
	      CGPDFDocumentGetNumberOfPages(document));
	info = CGPDFDocumentGetInfo(document);
	CHECK(info != NULL, "the written document has no /Info");
	if (info != NULL) {
		CHECK(CGPDFDictionaryGetString(info, "Title", &title), "the written /Title is missing");
		if (title != NULL) {
			text = CGPDFStringCopyTextString(title);
			CHECK(text != NULL && CFStringCompare(text, CFSTR("Round Trip"), 0) == kCFCompareEqualTo,
			      "the written title did not read back");
			if (text != NULL) {
				CFRelease(text);
			}
		}
	}
	catalog = CGPDFDocumentGetCatalog(document);
	CHECK(catalog != NULL, "the written document has no catalog");
	if (catalog != NULL) {
		CGPDFStreamRef metadata = NULL;

		CHECK(CGPDFDictionaryGetStream(catalog, "Metadata", &metadata),
		      "the written catalog has no /Metadata stream");
		if (metadata != NULL) {
			CGPDFDataFormat format;
			CFDataRef packet = CGPDFStreamCopyData(metadata, &format);

			CHECK(packet != NULL && CFDataGetLength(packet) >= 26 &&
			      memcmp(CFDataGetBytePtr(packet), "<x:xmpmeta>", 11) == 0,
			      "the metadata packet did not read back");
			if (packet != NULL) {
				CFRelease(packet);
			}
		}
	}

	fn_check_page_one(document);
	fn_check_page_two(document);

	CGPDFDocumentRelease(document);
	CGDataProviderRelease(provider);
	CFRelease(data);
	if (failures != 0) {
		fprintf(stderr, "%d PDF write check(s) failed\n", failures);
		return 1;
	}
	printf("pdf_write: all checks passed\n");
	return 0;
}
