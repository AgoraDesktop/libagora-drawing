/*
 * CGPDFPage.h — one page: its boxes, its rotation, and the transform that fits it into a rectangle.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE PAGE FLATTENS INHERITANCE, WHICH IS THE ONLY REASON THIS TYPE EXISTS APART FROM THE DOCUMENT.** In the
 * file `/MediaBox`, `/CropBox` and `/Rotate` may live on any ancestor in the page tree; `CGPDFPageGetBoxRect`
 * answers the EFFECTIVE box, and the effective rotation is the page's or its nearest ancestor's. A caller never
 * walks `/Parent` for a page attribute.
 *
 * **THE BOXES DEFAULT THE WAY THE SPECIFICATION SAYS THEY DO**: CropBox to MediaBox clipped to it, and
 * BleedBox, TrimBox and ArtBox to CropBox. A MediaBox is required, and a page without one answers a zero rect
 * with a `CG-REFUSE` line rather than an invented page size.
 *
 * A PAGE IS OWNED BY ITS DOCUMENT; `CGPDFPageRetain` exists for a caller that needs it longer, and the document
 * must be the thing that outlives it, because the page's dictionaries are the document's objects.
 */
#ifndef CORE_GRAPHICS_CGPDFPAGE_H
#define CORE_GRAPHICS_CGPDFPAGE_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGAffineTransform.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGPDFDictionary.h>

#include <CoreFoundation/CFBase.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFPage *CGPDFPageRef;

typedef enum CGPDFBox {
	kCGPDFMediaBox = 0,
	kCGPDFCropBox = 1,
	kCGPDFBleedBox = 2,
	kCGPDFTrimBox = 3,
	kCGPDFArtBox = 4
} CGPDFBox;

CG_EXTERN CGPDFPageRef CGPDFPageRetain(CGPDFPageRef page);
CG_EXTERN void CGPDFPageRelease(CGPDFPageRef page);
CG_EXTERN CGTypeID CGPDFPageGetTypeID(void);

CG_EXTERN struct CGPDFDocument *CGPDFPageGetDocument(CGPDFPageRef page);
CG_EXTERN size_t CGPDFPageGetPageNumber(CGPDFPageRef page);

/* THE EFFECTIVE BOX, in PDF user space (origin at the bottom left, y upward). A box the file does not define
 * answers the default described in this header's note, never a refusal. */
CG_EXTERN CGRect CGPDFPageGetBoxRect(CGPDFPageRef page, CGPDFBox box);

/* THE EFFECTIVE `/Rotate`, NORMALISED TO 0, 90, 180 OR 270. The file may carry any multiple of 90, negative
 * included; this door answers the quarter turn that is actually displayed. */
CG_EXTERN int CGPDFPageGetRotationAngle(CGPDFPageRef page);

/* **THE TRANSFORM THAT MAPS THIS PAGE INTO `rect`**, including the page's own rotation plus `rotate` degrees of
 * additional clockwise turn. The box is the source, `rect` the destination; with `preserveAspectRatio` the scale
 * is uniform and the page is centred, otherwise it is stretched to both axes.
 *
 * THE RESULT IS A COMPLETE PAGE→CONTEXT TRANSFORM: translate the box origin to zero, rotate clockwise, scale,
 * and place in `rect`. It is the one call Apple's own samples make before `CGContextDrawPDFPage`, which is why
 * the rotation is applied here and only here. */
CG_EXTERN CGAffineTransform CGPDFPageGetDrawingTransform(CGPDFPageRef page, CGPDFBox box, CGRect rect,
							 int rotate, bool preserveAspectRatio);

/* THE PAGE'S OWN DICTIONARY — unresolved inheritance, exactly as the file spells it. */
CG_EXTERN CGPDFDictionaryRef CGPDFPageGetDictionary(CGPDFPageRef page);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFPAGE_H */
