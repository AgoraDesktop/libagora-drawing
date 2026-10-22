/*
 * CGEPSContext.c — the public door for the EPS writer.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * The thin half, exactly as `CGPDFContext.c` is thin: the page-info vocabulary is Apple's, the URL door turns a
 * URL into a consumer, and `Close` is the moment the bytes are written. The recording lives in CGEPSWriter.c
 * behind the same `cg_pdf_writer_*` seam the PDF context uses.
 */
#include <CoreGraphics/CGEPSContext.h>
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGPDFWriter_internal.h>

#include <stdio.h>

CGContextRef CGEPSContextCreate(CGDataConsumerRef consumer, const CGRect *bounds, CFDictionaryRef auxiliaryInfo)
{
	struct cg_pdf_writer *writer;
	CGContextRef context;

	writer = cg_pdf_writer_create_eps(consumer, bounds, auxiliaryInfo);
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

CGContextRef CGEPSContextCreateWithURL(CFURLRef url, const CGRect *bounds, CFDictionaryRef auxiliaryInfo)
{
	CGDataConsumerRef consumer;
	CGContextRef context;

	if (url == NULL) {
		fprintf(stderr, "CG-REFUSE: CGEPSContextCreateWithURL needs a file URL\n");
		return NULL;
	}
	consumer = CGDataConsumerCreateWithURL(url);
	if (consumer == NULL) {
		return NULL;
	}
	context = CGEPSContextCreate(consumer, bounds, auxiliaryInfo);
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

void CGEPSContextBeginPage(CGContextRef context, CFDictionaryRef pageInfo)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);
	CGRect media;
	CGRect *media_pointer = NULL;

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGEPSContextBeginPage needs an EPS context\n");
		return;
	}
	if (cg_pdf_writer_box_from_dict(pageInfo, kCGPDFContextMediaBox, &media)) {
		media_pointer = &media;
	}
	(void)cg_pdf_writer_begin_page(writer, media_pointer);
	fn_apply_page_boxes(context, pageInfo);
}

void CGEPSContextEndPage(CGContextRef context)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGEPSContextEndPage needs an EPS context\n");
		return;
	}
	cg_pdf_writer_end_page(writer);
}

void CGEPSContextClose(CGContextRef context)
{
	struct cg_pdf_writer *writer = cg_context_pdf_writer(context);

	if (writer == NULL) {
		fprintf(stderr, "CG-REFUSE: CGEPSContextClose needs an EPS context\n");
		return;
	}
	(void)cg_pdf_writer_close(writer);
}
