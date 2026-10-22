/*
 * CGPDFContext.h — a drawing context whose output is a PDF file.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **THIS IS THE WRITER THE READER'S OTHER HALF NEEDED**: `NSView -dataWithPDFInsideRect:`, print-to-PDF and every
 * "export" dialog stand on it. A caller draws into the context exactly as it draws into a bitmap; each page is a
 * content stream, each `CGContextDrawImage` becomes an image XObject, and `CGPDFContextClose` is the moment the
 * file is written through the `CGDataConsumer` the context was made over.
 *
 * **PAGES ARE BEGUN AND ENDED, NOT SIZED BY THE SURFACE.** `CGPDFContextBeginPage` starts one (`CGContextBeginPage`
 * does the same); the page dictionary's boxes come from the page-info keys below, and a page whose info has no
 * `/MediaBox` uses the one the context was created with. `CGPDFContextClose` closes a page still open, writes the
 * cross-reference table and the trailer, and is the only call that produces bytes.
 *
 * **TEXT IS VECTOR GLYPH OUTLINES, NOT AN EMBEDDED FONT PROGRAM — YET.** The geometry is exact and scalable, but a
 * consumer cannot select or search the text in this build's output. That is stated rather than implied because it
 * is the one respect in which this writer's output differs from Apple's; embedding the face is the next landing.
 * AXIAL AND RADIAL GRADIENTS ARE RECORDED, as PDF shadings with a stitching function built from their stops; a
 * gradient whose stops carry PER-STOP ALPHA is refused (a DeviceRGB function has no alpha channel and a soft mask
 * is not written). `CGContextDrawShading` is recorded too: a shade in DeviceGray, DeviceRGB or DeviceCMYK is
 * SAMPLED into a type 0 function, because a CGFunction is a callback rather than a PDF function expression. **COLORED
 * TILING PATTERNS ARE RECORDED**: the caller's `drawPattern` runs into a pattern content stream, and the page
 * selects it with `scn`/`SCN` (the CGPattern object this library creates is always colored and no-distortion).
 * **A SHADOW WITH ZERO BLUR IS RECORDED** for paths and text: the silhouette is offset, painted in the shadow colour
 * and put behind the content. A BLURRED shadow has no PDF primitive and is refused, as is a shadow on an image or a
 * shading (there is no path to silhouette). **A TRANSPARENCY LAYER IS AN ISOLATED TRANSPARENCY GROUP**: its content
 * becomes a Form XObject with `/Group << /S /Transparency /I true >>`, drawn at `End` with the alpha and blend in
 * effect at `Begin`, so that alpha is applied to the GROUP once rather than to each operation. Clip-to-mask is
 * REFUSED BY NAME (`CG-REFUSE`), so a caller learns it was not recorded instead of receiving a page that quietly
 * lacks it.
 *
 * THE AUXILIARY AND PAGE-INFO DICTIONARIES are Apple's: the keys below, a `CFStringRef` value for each textual
 * field, and a `CFDataRef` wrapping a `CGRect` for each box.
 */
#ifndef CORE_GRAPHICS_CGPDFCONTEXT_H
#define CORE_GRAPHICS_CGPDFCONTEXT_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>

#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>

CF_IMPLICIT_BRIDGING_ENABLED

/* THE DOCUMENT'S FIELDS, read from the dictionary passed to `CGPDFContextCreate`. */
CG_EXTERN const CFStringRef kCGPDFContextTitle;
CG_EXTERN const CFStringRef kCGPDFContextAuthor;
CG_EXTERN const CFStringRef kCGPDFContextSubject;
CG_EXTERN const CFStringRef kCGPDFContextKeywords;
CG_EXTERN const CFStringRef kCGPDFContextCreator;
CG_EXTERN const CFStringRef kCGPDFContextProducer;

/* THE BOXES, each a `CFDataRef` holding one `CGRect`; valid in the auxiliary dictionary (the default for every
 * page) and in a page-info dictionary (that page's own). */
CG_EXTERN const CFStringRef kCGPDFContextMediaBox;
CG_EXTERN const CFStringRef kCGPDFContextCropBox;
CG_EXTERN const CFStringRef kCGPDFContextBleedBox;
CG_EXTERN const CFStringRef kCGPDFContextTrimBox;
CG_EXTERN const CFStringRef kCGPDFContextArtBox;

/* A `CGRect` for `mediaBox` is Apple's contract; NULL means US Letter. `auxiliaryInfo` may be NULL. */
CG_EXTERN CGContextRef CGPDFContextCreate(CGDataConsumerRef consumer, const CGRect *mediaBox,
					  CFDictionaryRef auxiliaryInfo);
CG_EXTERN CGContextRef CGPDFContextCreateWithURL(CFURLRef url, const CGRect *mediaBox,
						 CFDictionaryRef auxiliaryInfo);

/* **THE WRITE.** Closes an open page, then emits the header, every object, the cross-reference table and the
 * trailer through the consumer. Calling it twice is refused; releasing the context without it writes NOTHING,
 * which is Apple's contract and the reason the door exists. */
CG_EXTERN void CGPDFContextClose(CGContextRef context);

CG_EXTERN void CGPDFContextBeginPage(CGContextRef context, CFDictionaryRef pageInfo);
CG_EXTERN void CGPDFContextEndPage(CGContextRef context);

/* AN XMP PACKET, stored as a /Metadata XML stream and named from the catalog. `metadata` is retained until the
 * document is written. */
CG_EXTERN void CGPDFContextAddDocumentMetadata(CGContextRef context, CFDataRef metadata);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFCONTEXT_H */
