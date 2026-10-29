/*
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
/*
 * CoreGraphics.h — the umbrella.
 *
 * A consumer writes `#include <CoreGraphics/CoreGraphics.h>` (or `#import`s it from Objective-C) and gets the whole
 * drawing layer, the way `<Foundation/Foundation.h>` and `<AppKit/AppKit.h>` are the umbrellas of the two layers
 * above it. Core Graphics is a C API with a `CG` prefix, so the spelling here is `#include`; the two are
 * interchangeable in a file that is Objective-C, and a C file can use this one without a compiler extension.
 *
 * **EVERY HEADER THIS COMPONENT INSTALLS IS EITHER NAMED HERE OR NAMED AS INTERNAL, AND `tests/umbrella.m` IS THE
 * CENSUS THAT SAYS SO.** `CMakeLists.txt` installs `*.h` with a glob, so a header becomes part of the installed
 * surface the moment its file exists; a list that drifted would be a directory listing pretending to be an
 * interface, and nothing would fail until a consumer could not compile. The test fails with the header that is in
 * neither list, so adding a class means classifying it — one line here or one line there.
 *
 * **THE `*_internal.h` HEADERS ARE ABSENT ON PURPOSE.** They carry a context's own state (its stack of saved states,
 * the PDF parser's cursor, the image rep's caches) and are installed because sibling components build against this
 * prefix, not because a program should import them.
 *
 * **A C FILE NEEDS `-fblocks` TO INCLUDE THIS, AND THAT IS CORE FOUNDATION'S REQUIREMENT RATHER THAN THIS LAYER'S.**
 * `CoreGraphics/CGBase.h` includes `<CoreFoundation/CoreFoundation.h>`, whose headers declare block types; clang
 * answers "blocks support disabled" without the flag. MEASURED with a two-line `.c` consumer: it compiled and ran
 * with `-fblocks`, and refused without it. The flag is the caller's, and it is named here so the refusal is not a
 * surprise at the first `#include`.
 *
 * **THE ORDER IS A READING ORDER, NOT A DEPENDENCY ONE.** Every header here includes what it needs, so any of them
 * may be included alone; the sections exist so that a person looking for a name has somewhere to look other than the
 * alphabet. `CoreGraphics/CGBase.h` is first because it is where the CF-style reference conventions are written down.
 */
#ifndef COREGRAPHICS_COREGRAPHICS_H
#define COREGRAPHICS_COREGRAPHICS_H

/* THE BASE, GEOMETRY AND TRANSFORMS */
#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGAffineTransform.h>

/* CONTEXTS, AND WHAT THEY READ AND WRITE */
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGWindowLevel.h>

/* COLOUR AND COLOUR SPACES */
#include <CoreGraphics/CGColor.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGGenericCMYKProfile.h>

/* PAINT: GRADIENTS, SHADINGS, PATTERNS AND FUNCTIONS */
#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGShading.h>
#include <CoreGraphics/CGPattern.h>
#include <CoreGraphics/CGFunction.h>

/* PATHS AND LAYERS */
#include <CoreGraphics/CGPath.h>
#include <CoreGraphics/CGLayer.h>

/* IMAGES */
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGImageDestination.h>

/* FONTS */
#include <CoreGraphics/CGFont.h>

/* PDF: THE DOCUMENT, ITS OBJECTS, AND PRODUCING ONE */
#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGPDFArray.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFObject.h>
#include <CoreGraphics/CGPDFStream.h>
#include <CoreGraphics/CGPDFString.h>
#include <CoreGraphics/CGPDFScanner.h>
#include <CoreGraphics/CGPDFOperatorTable.h>
#include <CoreGraphics/CGPDFContentStream.h>
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGPDFFont.h>
#include <CoreGraphics/CGPDFFunction.h>
#include <CoreGraphics/CGPDFSubset.h>
#include <CoreGraphics/CGPDFCFFSubset.h>

/* EPS */
#include <CoreGraphics/CGEPSContext.h>
#include <CoreGraphics/CGEPSWriter.h>

#endif /* COREGRAPHICS_COREGRAPHICS_H */
