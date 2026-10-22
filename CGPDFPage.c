/*
 * CGPDFPage.c — a page's effective geometry, and the transform that fits it onto a surface.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * THE PAGE STORES NOTHING BUT WHERE IT IS. Its boxes and rotation were flattened from the page tree when the
 * document opened (`fn_walk_pages`); this file is the public face of those fields plus the one piece of real
 * arithmetic in the type, `CGPDFPageGetDrawingTransform`.
 *
 * **THE DRAWING TRANSFORM IS FOUR STEPS IN A FIXED ORDER**, and the order is the whole answer: translate the
 * box's origin to zero, rotate the page's own quarter-turn plus the caller's, scale the rotated bounding box
 * into `rect`, and place it there. Rotation happens BEFORE scaling so a rotated page is scaled by its displayed
 * width and height rather than its stored ones — a `/Rotate 90` page of 200×100 needs a scale of `rect.width/100`
 * on x, which is the number a reader that scaled first gets wrong.
 *
 * THE ROTATION IS CLOCKWISE, which is what a PDF's `/Rotate` means, and it is applied as a NEGATIVE angle
 * because Core Graphics' user space is y-up. The four corners are rotated and their bounding box is taken, so
 * the arithmetic does not special-case 90 and 270 — the only difference between them and 0/180 is which numbers
 * fall out of the same four lines.
 *
 * A PAGE BORROWS ITS DOCUMENT, and the document owns the page. That is Apple's contract and it is also what
 * makes the cache in `CGPDFDocumentGetPage` possible: the same call answers the same page object, because there
 * is one owner and no per-call allocation.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGTypeID_internal.h>

#include <math.h>
#include <stdlib.h>

struct CGPDFPage {
	int refcount;
	struct CGPDFDocument *document;
	cg_pdf_doc *pdf;
	size_t index;		/* zero-based; the public page number is index + 1 */
};

CGPDFPageRef cg_pdf_page_create(struct CGPDFDocument *document, cg_pdf_doc *pdf, size_t index)
{
	CGPDFPageRef page;

	if (pdf == NULL || index >= pdf->page_count) {
		return NULL;
	}
	page = calloc(1, sizeof *page);
	if (page == NULL) {
		return NULL;
	}
	page->refcount = 1;	/* the document's */
	page->document = document;
	page->pdf = pdf;
	page->index = index;
	return page;
}

CGPDFPageRef CGPDFPageRetain(CGPDFPageRef page)
{
	if (page != NULL) {
		page->refcount++;
	}
	return page;
}

void CGPDFPageRelease(CGPDFPageRef page)
{
	if (page == NULL) {
		return;
	}
	if (--page->refcount <= 0) {
		free(page);
	}
}

cg_pdf_doc *cg_pdf_page_doc(CGPDFPageRef page)
{
	return page != NULL ? page->pdf : NULL;
}

size_t cg_pdf_page_index(CGPDFPageRef page)
{
	return page != NULL ? page->index : 0;
}

CGTypeID CGPDFPageGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGPDFPAGE;
}

struct CGPDFDocument *CGPDFPageGetDocument(CGPDFPageRef page)
{
	return page != NULL ? page->document : NULL;
}

size_t CGPDFPageGetPageNumber(CGPDFPageRef page)
{
	return page != NULL ? page->index + 1 : 0;
}

CGRect CGPDFPageGetBoxRect(CGPDFPageRef page, CGPDFBox box)
{
	const cg_pdf_page_info *info;

	if (page == NULL || page->index >= page->pdf->page_count) {
		return CGRectZero;
	}
	info = &page->pdf->pages[page->index];
	switch (box) {
	case kCGPDFMediaBox: return info->media;
	case kCGPDFCropBox: return info->crop;
	case kCGPDFBleedBox: return info->bleed;
	case kCGPDFTrimBox: return info->trim;
	case kCGPDFArtBox: return info->art;
	}
	return info->media;
}

int CGPDFPageGetRotationAngle(CGPDFPageRef page)
{
	if (page == NULL || page->index >= page->pdf->page_count) {
		return 0;
	}
	return page->pdf->pages[page->index].rotate;
}

CGPDFDictionaryRef CGPDFPageGetDictionary(CGPDFPageRef page)
{
	if (page == NULL || page->index >= page->pdf->page_count) {
		return NULL;
	}
	return (CGPDFDictionaryRef)page->pdf->pages[page->index].dict;
}

CGAffineTransform CGPDFPageGetDrawingTransform(CGPDFPageRef page, CGPDFBox box, CGRect rect, int rotate,
					       bool preserveAspectRatio)
{
	CGRect source;
	double angle;
	double c;
	double s;
	double xs[4];
	double ys[4];
	double minx;
	double maxx;
	double miny;
	double maxy;
	double bw;
	double bh;
	double sx;
	double sy;
	double tx;
	double ty;
	int total;
	size_t i;

	if (page == NULL) {
		return CGAffineTransformIdentity;
	}
	source = CGPDFPageGetBoxRect(page, box);
	if (source.size.width <= 0 || source.size.height <= 0) {
		return CGAffineTransformIdentity;
	}
	total = (CGPDFPageGetRotationAngle(page) + rotate) % 360;
	if (total < 0) {
		total += 360;
	}
	/* CLOCKWISE IN A Y-UP SPACE IS A NEGATIVE ANGLE. */
	angle = -(double)total * M_PI / 180.0;
	c = cos(angle);
	s = sin(angle);
	xs[0] = 0;
	ys[0] = 0;
	xs[1] = source.size.width * c;
	ys[1] = source.size.width * s;
	xs[2] = source.size.width * c - source.size.height * s;
	ys[2] = source.size.width * s + source.size.height * c;
	xs[3] = -source.size.height * s;
	ys[3] = source.size.height * c;
	minx = maxx = xs[0];
	miny = maxy = ys[0];
	for (i = 1; i < 4; i++) {
		if (xs[i] < minx) minx = xs[i];
		if (xs[i] > maxx) maxx = xs[i];
		if (ys[i] < miny) miny = ys[i];
		if (ys[i] > maxy) maxy = ys[i];
	}
	bw = maxx - minx;
	bh = maxy - miny;
	if (bw <= 0 || bh <= 0) {
		return CGAffineTransformIdentity;
	}
	sx = rect.size.width / bw;
	sy = rect.size.height / bh;
	if (preserveAspectRatio) {
		double uniform = sx < sy ? sx : sy;

		sx = uniform;
		sy = uniform;
	}
	/* CENTRED IN THE DESTINATION, which is what preserveAspectRatio means and is also the harmless answer
	 * when the scale is exact. */
	tx = rect.origin.x - minx * sx + (rect.size.width - bw * sx) / 2.0;
	ty = rect.origin.y - miny * sy + (rect.size.height - bh * sy) / 2.0;

	return CGAffineTransformConcat(
		CGAffineTransformConcat(
			CGAffineTransformConcat(
				CGAffineTransformMakeTranslation(-source.origin.x, -source.origin.y),
				CGAffineTransformMakeRotation(angle)),
			CGAffineTransformMakeScale(sx, sy)),
		CGAffineTransformMakeTranslation(tx, ty));
}
