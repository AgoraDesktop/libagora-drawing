/*
 * pdf_cff.c — a CFF face embedded as a PDF Type 1 font: /FontFile3 /Type1C, /Differences, and a second shard.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **CFF IS THE OTHER WAY A FONT CAN BE OUTLINED**, and PDF's route for a NON-CID-keyed one is a Type 1 font whose
 * `/Encoding /Differences` names each glyph: a one-byte code selects a name, the name selects a glyph in the CFF
 * charset. That is what this checks — the file's `FontFile3` is `/Subtype /Type1C`, the font is `/Subtype /Type1`,
 * the content is `Tj` rather than outlines, and the page still renders.
 *
 * A PDF Type 1 font has 255 codes, so a face with more than that used glyphs is split into shards; the second half
 * of the test shows 300 glyphs and asks the page for more than one font resource. Glyphs the CFF charset does not
 * name cannot be selected this way and fall back to outlines by design, so the shard assertion tolerates a face
 * whose names are sparse — it asks for at least two resources, not for exactly two.
 *
 * THE FONT IS THE HOST'S, because this tree vendors no CFF face. The test SKIPS (77) when none of the candidates
 * exists; that is the same rule the compositor tests follow for a machine without one. Inconsolata first: it is
 * SIL OFL, and a CFF face.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGPDFArray.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGPDFObject.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGPDFStream.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

/* THE `CFF ` TABLE'S LENGTH IN THE OTF, so the test can tell a subset from the whole program. */
static long fn_cff_table_length(const char *path)
{
	FILE *file = fopen(path, "rb");
	unsigned char header[12];
	unsigned tables;
	unsigned i;
	long result = -1;

	if (file == NULL) {
		return -1;
	}
	if (fread(header, 1, sizeof header, file) != sizeof header) {
		fclose(file);
		return -1;
	}
	tables = ((unsigned)header[4] << 8) | header[5];
	for (i = 0; i < tables; i++) {
		unsigned char entry[16];

		if (fread(entry, 1, sizeof entry, file) != sizeof entry) {
			break;
		}
		if (entry[0] == 'C' && entry[1] == 'F' && entry[2] == 'F' && entry[3] == ' ') {
			result = ((long)entry[12] << 24) | ((long)entry[13] << 16) | ((long)entry[14] << 8) |
				 entry[15];
			break;
		}
	}
	fclose(file);
	return result;
}

static const char *fn_find_cff_font(void)
{
	static const char *candidates[] = {
		"/usr/share/fonts/truetype/inconsolata/Inconsolata.otf",
		"/usr/share/fonts/opentype/inconsolata/Inconsolata.otf",
		"/usr/local/share/fonts/Inconsolata.otf",
		NULL
	};
	size_t i;

	for (i = 0; candidates[i] != NULL; i++) {
		if (access(candidates[i], R_OK) == 0) {
			return candidates[i];
		}
	}
	return NULL;
}

struct fn_fonts {
	int count;
	int type1;
	int type1c;
	long file_length;
};

static void fn_font_entry(const char *key, CGPDFObjectRef value, void *info)
{
	struct fn_fonts *fonts = info;
	CGPDFDictionaryRef font = NULL;
	CGPDFDictionaryRef descriptor = NULL;
	CGPDFStreamRef file = NULL;
	const char *subtype = NULL;

	(void)key;
	fonts->count++;
	if (CGPDFObjectGetType(value) != kCGPDFObjectTypeDictionary ||
	    !CGPDFObjectGetValue(value, kCGPDFObjectTypeDictionary, &font)) {
		return;
	}
	if (CGPDFDictionaryGetName(font, "Subtype", &subtype) && strcmp(subtype, "Type1") == 0) {
		fonts->type1++;
	}
	if (CGPDFDictionaryGetDictionary(font, "FontDescriptor", &descriptor) &&
	    CGPDFDictionaryGetStream(descriptor, "FontFile3", &file)) {
		CGPDFDictionaryRef dict = CGPDFStreamGetDictionary(file);

		if (dict != NULL && CGPDFDictionaryGetName(dict, "Subtype", &subtype) &&
		    strcmp(subtype, "Type1C") == 0) {
			CGPDFInteger length = 0;

			fonts->type1c++;
			if (CGPDFDictionaryGetInteger(dict, "Length", &length)) {
				fonts->file_length = (long)length;
			}
		}
	}
}

static bool fn_content_has_tj(CGPDFPageRef page)
{
	CGPDFDictionaryRef dict = CGPDFPageGetDictionary(page);
	CGPDFStreamRef contents = NULL;
	CGPDFDataFormat format;
	CFDataRef bytes;
	const char *text;
	size_t length;
	size_t i;

	if (dict == NULL || !CGPDFDictionaryGetStream(dict, "Contents", &contents)) {
		return false;
	}
	bytes = CGPDFStreamCopyData(contents, &format);
	if (bytes == NULL) {
		return false;
	}
	text = (const char *)CFDataGetBytePtr(bytes);
	length = (size_t)CFDataGetLength(bytes);
	for (i = 0; i + 2 <= length; i++) {
		if (text[i] == 'T' && text[i + 1] == 'j') {
			CFRelease(bytes);
			return true;
		}
	}
	CFRelease(bytes);
	return false;
}

static CFDataRef fn_build_pdf(CGFontRef font, int many_glyphs)
{
	CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
	CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(data);
	CGRect box = CGRectMake(0, 0, 300, 100);
	CGContextRef ctx;

	if (data == NULL || consumer == NULL) {
		if (consumer != NULL) CGDataConsumerRelease(consumer);
		if (data != NULL) CFRelease(data);
		return NULL;
	}
	ctx = CGPDFContextCreate(consumer, &box, NULL);
	CGDataConsumerRelease(consumer);
	if (ctx == NULL) {
		CFRelease(data);
		return NULL;
	}
	CGPDFContextBeginPage(ctx, NULL);
	CGContextSetFont(ctx, font);
	CGContextSetFontSize(ctx, 40);
	CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
	CGContextSetTextPosition(ctx, 20, 40);
	CGContextShowText(ctx, "Wag", 3);
	if (many_glyphs) {
		size_t count = CGFontGetNumberOfGlyphs(font);
		CGGlyph *glyphs;
		size_t i;

		if (count > 300) {
			count = 300;
		}
		glyphs = calloc(count, sizeof *glyphs);
		if (glyphs != NULL) {
			for (i = 0; i < count; i++) {
				glyphs[i] = (CGGlyph)(i + 1);
			}
			CGContextSetTextPosition(ctx, 10, 85);
			CGContextShowGlyphs(ctx, glyphs, count);
			free(glyphs);
		}
	}
	CGPDFContextEndPage(ctx);
	CGPDFContextClose(ctx);
	CGContextRelease(ctx);
	return data;
}

static void fn_check_pixels(CGPDFPageRef page)
{
	unsigned char buffer[300 * 100 * 4];
	unsigned char *pixel;
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	int ink = 0;
	size_t x;
	size_t y;

	memset(buffer, 0, sizeof buffer);
	ctx = CGBitmapContextCreate(buffer, 300, 100, 8, 300 * 4, space,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorSpaceRelease(space);
	CHECK(ctx != NULL, "the CFF page's render target was refused");
	if (ctx == NULL) {
		return;
	}
	{
		CGAffineTransform transform = CGPDFPageGetDrawingTransform(page, kCGPDFCropBox,
									  CGRectMake(0, 0, 300, 100), 0, true);

		CGContextConcatCTM(ctx, transform);
		CGContextDrawPDFPage(ctx, page);
	}
	for (y = 40; y < 80 && !ink; y++) {
		for (x = 20; x < 200; x++) {
			pixel = buffer + ((size_t)(100 - 1 - (int)y) * 300 + x) * 4;
			if (pixel[3] > 100 && pixel[0] < 100 && pixel[1] < 100 && pixel[2] < 100) {
				ink = 1;
				break;
			}
		}
	}
	CHECK(ink, "the embedded CFF text produced no ink");
	CGContextRelease(ctx);
}

int main(void)
{
	const char *path = fn_find_cff_font();
	CGDataProviderRef provider;
	CGFontRef font;
	CFDataRef data;
	CGDataProviderRef reopen;
	CGPDFDocumentRef document;
	CGPDFPageRef page;
	CGPDFDictionaryRef page_dict;
	CGPDFDictionaryRef resources;
	CGPDFDictionaryRef fonts;
	struct fn_fonts probe = { 0, 0, 0 };
	CFDataRef many;

	if (path == NULL) {
		printf("pdf_cff: SKIP — no CFF font found on this host\n");
		return 77;
	}
	provider = CGDataProviderCreateWithFilename(path);
	font = provider != NULL ? CGFontCreateWithDataProvider(provider) : NULL;
	if (provider != NULL) {
		CGDataProviderRelease(provider);
	}
	CHECK(font != NULL, "the CFF font at %s would not open", path);
	if (font == NULL) {
		return 1;
	}
	data = fn_build_pdf(font, 0);
	CHECK(data != NULL, "the CFF PDF was not written");
	reopen = data != NULL ? CGDataProviderCreateWithCFData(data) : NULL;
	document = reopen != NULL ? CGPDFDocumentCreateWithProvider(reopen) : NULL;
	CHECK(document != NULL, "the CFF PDF did not reopen");
	page = document != NULL ? CGPDFDocumentGetPage(document, 1) : NULL;
	CHECK(page != NULL, "the CFF PDF has no page");
	if (page != NULL) {
		page_dict = CGPDFPageGetDictionary(page);
		CHECK(page_dict != NULL && CGPDFDictionaryGetDictionary(page_dict, "Resources", &resources),
		      "the CFF page has no resources");
		CHECK(resources != NULL && CGPDFDictionaryGetDictionary(resources, "Font", &fonts),
		      "the CFF page has no font resource");
		if (fonts != NULL) {
			CGPDFDictionaryApply(fonts, fn_font_entry, &probe);
		}
		CHECK(probe.count == 1, "the CFF page names %d fonts, expected 1", probe.count);
		CHECK(probe.type1 == 1, "the CFF font is not a Type 1 font");
		CHECK(probe.type1c == 1, "the CFF font has no /FontFile3 /Type1C program");
		{
			long whole = fn_cff_table_length(path);

			CHECK(whole > 0, "the test could not read the font's CFF table length");
			CHECK(probe.file_length > 0, "the written /FontFile3 has no length");
			CHECK(whole > 0 && probe.file_length < whole,
			      "the written CFF is %ld bytes and the whole program is %ld — it was not subsetted",
			      probe.file_length, whole);
		}
		CHECK(fn_content_has_tj(page), "the CFF text was written as outlines, not Tj");
		fn_check_pixels(page);
	}

	/* THE SHARDS: 300 glyphs cannot fit a Type 1 font's 255 codes, so the page must name more than one. */
	many = fn_build_pdf(font, 1);
	if (many != NULL) {
		CGDataProviderRef p2 = CGDataProviderCreateWithCFData(many);
		CGPDFDocumentRef doc2 = p2 != NULL ? CGPDFDocumentCreateWithProvider(p2) : NULL;
		CGPDFPageRef page2 = doc2 != NULL ? CGPDFDocumentGetPage(doc2, 1) : NULL;

		CHECK(page2 != NULL, "the many-glyph CFF PDF did not reopen");
		if (page2 != NULL) {
			CGPDFDictionaryRef d2 = CGPDFPageGetDictionary(page2);
			CGPDFDictionaryRef r2 = NULL;
			CGPDFDictionaryRef f2 = NULL;

			if (CGPDFDictionaryGetDictionary(d2, "Resources", &r2) &&
			    CGPDFDictionaryGetDictionary(r2, "Font", &f2)) {
				CHECK(CGPDFDictionaryGetCount(f2) >= 2,
				      "300 CFF glyphs produced %zu font resources, expected at least 2 shards",
				      CGPDFDictionaryGetCount(f2));
			} else {
				CHECK(0, "the many-glyph CFF page has no font resources");
			}
		}
		if (doc2 != NULL) {
			CGPDFDocumentRelease(doc2);
		}
		if (p2 != NULL) {
			CGDataProviderRelease(p2);
		}
		CFRelease(many);
	} else {
		CHECK(0, "the many-glyph CFF PDF was not written");
	}

	if (document != NULL) {
		CGPDFDocumentRelease(document);
	}
	if (reopen != NULL) {
		CGDataProviderRelease(reopen);
	}
	if (data != NULL) {
		CFRelease(data);
	}
	CGFontRelease(font);
	if (failures != 0) {
		fprintf(stderr, "%d CFF check(s) failed\n", failures);
		return 1;
	}
	printf("pdf_cff: all checks passed\n");
	return 0;
}
