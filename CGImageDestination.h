/*
 * CGImageDestination.h — writing an image out: the door that turns a `CGImage` into bytes of a named type.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **THIS IS THE HALF OF CORE GRAPHICS'S IMAGE STORY THIS TREE DID NOT HAVE, AND THE ONE THE APPKIT NAMED.** Every
 * encoding door in `NSBitmapImageRep` refused with a measured reason — "this tree's CoreGraphics has the PNG and JPEG
 * decoders and no `CGImageDestination`, so there is nothing to encode with" — and this is that object. Its shape is
 * Apple's: a destination is created over a MUTABLE DATA or a URL, images are added to it, and `Finalize` is what
 * actually writes them.
 *
 * **AND `CGImageDestinationRef` IS A REAL CORE FOUNDATION TYPE, unlike most of this library's objects.** Apple's is
 * one, and a caller's habit is `CFRelease` — so `CFRetain`, `CFRelease` and `CFGetTypeID` work here exactly as the
 * documentation describes, and `CGImageDestinationGetTypeID` answers Core Foundation's id for the class rather than a
 * number from `CGTypeID_internal.h`'s table. `CGColor` is the other class that crossed that line; the note in
 * `CGTypeID_internal.h` is where the rule lives.
 *
 * **THE TYPES ARE UNIFORM TYPE IDENTIFIERS — STRINGS — AND THIS BUILD WRITES ONE OF THEM.** Apple passes a UTI from
 * `kUTTypePNG` and friends; this tree has no `MobileCoreServices` header, so the strings are written here rather than
 * invented as new constants, and `CGImageDestinationCopyTypeIdentifiers` is the door that answers what this build
 * can write rather than what the API knows about. **PNG ONLY**: the encoder is `CGImagePNG.c`'s
 * (`png_image_write_to_memory`, the same simplified libpng API its decoder reads with), and every other type is
 * REFUSED BY NAME, because an object that accepted `public.jpeg` and wrote a PNG would be silently wrong.
 */
#ifndef CORE_GRAPHICS_CGIMAGEDESTINATION_H
#define CORE_GRAPHICS_CGIMAGEDESTINATION_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGImage.h>

#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFURL.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct __CGImageDestination *CGImageDestinationRef;

/* THE CLASS IDENTITY. Unlike `CGImageGetTypeID` and its neighbours this answers CORE FOUNDATION's id for this
 * class, because this class IS a Core Foundation type — see the note at the top of this header. */
CG_EXTERN CGTypeID CGImageDestinationGetTypeID(void);

/* **THE TWO WAYS TO HAVE SOMEWHERE TO WRITE.** With `data`, bytes are APPENDED when the destination is finalized;
 * with `url`, a file is written at that moment by name. `type` is the UTI and must be one this build writes; `count`
 * is a hint Apple uses to size the output and this build only checks against what it can carry (one image, since PNG
 * is a single-image format here). `options` is not read yet, and a caller that passes one is not silently ignored:
 * the door refuses it by name, the way the rest of this library refuses what it cannot honour. */
CG_EXTERN CGImageDestinationRef CGImageDestinationCreateWithData(CFMutableDataRef data, CFStringRef type,
								size_t count, CFDictionaryRef options);
CG_EXTERN CGImageDestinationRef CGImageDestinationCreateWithURL(CFURLRef url, CFStringRef type, size_t count,
							       CFDictionaryRef options);

/* ONE IMAGE, AND A SECOND IS REFUSED BY NAME. Apple's destination takes a sequence because TIFF and PDF carry
 * several; PNG does not, and accepting a second image and dropping it is the silent wrongness this tree avoids.
 * `properties` is not read yet, and is refused when it is not NULL, for the same reason `options` is. */
CG_EXTERN void CGImageDestinationAddImage(CGImageDestinationRef idst, CGImageRef image, CFDictionaryRef properties);

/* **THE WRITE.** True when the bytes were made and handed to the destination's data or file; false when the
 * destination already finalized, holds no image, or could not encode — each with a `CG-REFUSE` line naming which. */
CG_EXTERN bool CGImageDestinationFinalize(CGImageDestinationRef idst);

/* WHAT THIS BUILD WRITES, WHICH IS THE HONEST ANSWER TO "WHAT CAN I ASK FOR": an array of UTIs, `public.png` in it
 * and nothing else. A caller that checks its type against this list never meets the refusal. */
CG_EXTERN CFArrayRef CGImageDestinationCopyTypeIdentifiers(void);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGIMAGEDESTINATION_H */
