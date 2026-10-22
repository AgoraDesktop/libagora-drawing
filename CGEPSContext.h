/*
 * CGEPSContext.h — a drawing context whose output is an Encapsulated PostScript file.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **THIS IS AGORA'S EPS PRODUCER, AND IT IS AN EXTENSION RATHER THAN AN APPLE DOOR.** Apple produces EPS
 * through AppKit (`-dataWithEPSInsideRect:`) and converts PostScript to PDF with `CGPSConverter`; it publishes
 * no Core Graphics EPS context. The shape here is deliberately the same as `CGPDFContext`'s — create over a
 * consumer, begin a page, draw, end it, close — because that is the seam `CGContext.c` already records through.
 *
 * AN EPS FILE HAS EXACTLY ONE PAGE. `CGEPSContextBeginPage` may be called once; a second call refuses by name.
 * The bounding box is the page's media box (or the rectangle passed to the create door), and the content is
 * clipped to it. WHAT IS WRITTEN: paths, clips, dashes, flat colours, glyph outlines, opaque images, gradients
 * and shadings (as a Level 3 `shfill` with a sampled function). WHAT REFUSES: alpha, blend modes, transparency
 * groups, soft-masked images, blurred shadows and tiling patterns — PostScript has no primitive for them.
 */
#ifndef CORE_GRAPHICS_CGEPS_CONTEXT_H
#define CORE_GRAPHICS_CGEPS_CONTEXT_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>

#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFURL.h>

CF_IMPLICIT_BRIDGING_ENABLED

/* `bounds` is the EPS bounding box; NULL means US Letter, like the PDF context's default. `auxiliaryInfo` may
 * carry `kCGPDFContextTitle` and `kCGPDFContextCreator`, which become EPS comments. */
CG_EXTERN CGContextRef CGEPSContextCreate(CGDataConsumerRef consumer, const CGRect *bounds,
					  CFDictionaryRef auxiliaryInfo);
CG_EXTERN CGContextRef CGEPSContextCreateWithURL(CFURLRef url, const CGRect *bounds,
						 CFDictionaryRef auxiliaryInfo);

CG_EXTERN void CGEPSContextBeginPage(CGContextRef context, CFDictionaryRef pageInfo);
CG_EXTERN void CGEPSContextEndPage(CGContextRef context);

/* THE ONE WRITE. Releasing the context without it emits NOTHING, the PDF context's contract and the reason a
 * caller cannot forget the step by accident. */
CG_EXTERN void CGEPSContextClose(CGContextRef context);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGEPS_CONTEXT_H */
