/*
 * CGPDFContext.c — the door a caller holds: create a PDF context, begin and end pages, close it.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * THE PUBLIC HALF IS DELIBERATELY THIN. It owns the page-info dictionary's vocabulary (the box keys are
 * `CFData`-wrapped `CGRect`s, the text fields are `CFString`s), it turns a URL into a consumer, and it is the
 * only place that knows `CGPDFContextClose` is the moment bytes appear. Everything the drawing doors record
 * lives in `CGPDFWriter.c`.
 *
 * A CONTEXT RELEASED WITHOUT `Close` WRITES NOTHING — Apple's contract, and the reason `Close` is not just a
 * destructor: a caller that forgets it gets no file rather than half a file.
 */
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGPDFWriter_internal.h>

#include <stdio.h>

const CFStringRef kCGPDFContextTitle = CFSTR("kCGPDFContextTitle");
const CFStringRef kCGPDFContextAuthor = CFSTR("kCGPDFContextAuthor");
const CFStringRef kCGPDFContextSubject = CFSTR("kCGPDFContextSubject");
const CFStringRef kCGPDFContextKeywords = CFSTR("kCGPDFContextKeywords");
const CFStringRef kCGPDFContextCreator = CFSTR("kCGPDFContextCreator");
const CFStringRef kCGPDFContextProducer = CFSTR("kCGPDFContextProducer");
const CFStringRef kCGPDFContextMediaBox = CFSTR("kCGPDFContextMediaBox");
const CFStringRef kCGPDFContextCropBox = CFSTR("kCGPDFContextCropBox");
const CFStringRef kCGPDFContextBleedBox = CFSTR("kCGPDFContextBleedBox");
const CFStringRef kCGPDFContextTrimBox = CFSTR("kCGPDFContextTrimBox");
const CFStringRef kCGPDFContextArtBox = CFSTR("kCGPDFContextArtBox");

CGContextRef CGPDFContextCreate(CGDataConsumerRef consumer, const CGRect *mediaBox, CFDictionaryRef auxiliaryInfo)
{
	struct cg_pdf_writer *writer;
	CGContextRef context;

	writer = cg_pdf_writer_create(consumer, mediaBox, auxiliaryInfo);
	if (writer == NULL) {
		return NULL;
	}
	context = cg_context_create_pdf(writer);
	if (context == NULL) {
		cg_pdf_writer_free(writer);
		return NULL;
	}
	return context;
}

CGContextRef CGPDFContextCreateWithURL(CFURLRef url, const CGRect *mediaBox, CFDictionaryRef auxiliaryInfo)
{
	CGDataConsumerRef consumer;
	CGContextRef context;

	if (url == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextCreateWithURL needs a file URL\n");
		return NULL;
	}
	consumer = CGDataConsumerCreateWithURL(url);
	if (consumer == NULL) {
		return NULL;
	}
	context = CGPDFContextCreate(consumer, mediaBox, auxiliaryInfo);
	CGDataConsumerRelease(consumer);
	return context;
}

static void fn_apply_page_boxes(CGContextRef context, CFDictionaryRef pageInfo)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);
	static const struct {
		CFStringRef key;
		int which;
	} boxes[] = {
		{ NULL, CG_PDF_BOX_MEDIA },
		{ NULL, CG_PDF_BOX_CROP },
		{ NULL, CG_PDF_BOX_BLEED },
		{ NULL, CG_PDF_BOX_TRIM },
		{ NULL, CG_PDF_BOX_ART }
	};
	CFStringRef keys[5];
	size_t i;

	keys[0] = kCGPDFContextMediaBox;
	keys[1] = kCGPDFContextCropBox;
	keys[2] = kCGPDFContextBleedBox;
	keys[3] = kCGPDFContextTrimBox;
	keys[4] = kCGPDFContextArtBox;
	for (i = 0; i < 5; i++) {
		CGRect box;

		if (cg_pdf_writer_box_from_dict(pageInfo, keys[i], &box)) {
			cg_pdf_writer_set_page_box(writer, boxes[i].which, box);
		}
	}
}

void CGPDFContextBeginPage(CGContextRef context, CFDictionaryRef pageInfo)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);
	CGRect media;
	CGRect *media_pointer = NULL;

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextBeginPage needs a PDF context\n");
		return;
	}
	if (cg_pdf_writer_box_from_dict(pageInfo, kCGPDFContextMediaBox, &media)) {
		media_pointer = &media;
	}
	(void)cg_pdf_writer_begin_page(writer, media_pointer);
	fn_apply_page_boxes(context, pageInfo);
}

void CGPDFContextEndPage(CGContextRef context)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextEndPage needs a PDF context\n");
		return;
	}
	cg_pdf_writer_end_page(writer);
}

void CGPDFContextClose(CGContextRef context)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextClose needs a PDF context\n");
		return;
	}
	(void)cg_pdf_writer_close(writer);
}

void CGPDFContextAddDocumentMetadata(CGContextRef context, CFDataRef metadata)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContextAddDocumentMetadata needs a PDF context\n");
		return;
	}
	cg_pdf_writer_set_metadata(writer, metadata);
}
