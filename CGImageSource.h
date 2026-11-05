/*
 * CGImageSource.h — bytes become an image, and the format is decided by the bytes.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THIS IS IMAGEIO'S DOOR, AND IT EXISTS SO THAT A VIEWER DOES NOT HAVE TO KNOW A FORMAT'S NAME TO OPEN IT.** The
 * AppKit's representation registry can only ask the classes an application LINKS whether they claim a file, which is why
 * `NSBitmapImageRep` is PNG-and-JPEG and nothing else; here the question is asked of the bytes once, by one registry, and
 * a new format is a new file registered in it. `CGImageSourceCopyTypeIdentifiers` is the answer to "what can this build
 * read", and it is the same door Apple's ImageIO publishes for exactly that question.
 *
 * **A SOURCE MAY HOLD SEVERAL IMAGES.** A GIF, a WebP animation, a multi-page TIFF and an ICNS icon all do, so
 * `-GetCount` and `-CreateImageAtIndex` are the two doors a viewer animates through, and `-CopyPropertiesAtIndex` carries
 * the one property an animation needs: how long each frame is shown.
 *
 * Written from `CGImageSource.h` in the MacOSX10.6 SDK's ImageIO framework (see PROVENANCE.md). The subset implemented
 * here is the creation, the count, the type, the image, the properties and the type list; the incremental and thumbnail
 * doors are named as absent in README.md rather than stubbed.
 */
#ifndef CORE_GRAPHICS_CGIMAGESOURCE_H
#define CORE_GRAPHICS_CGIMAGESOURCE_H

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGDataProvider.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGImageSource *CGImageSourceRef;

/* THE PROPERTIES' KEYS THIS BUILD ANSWERS. Apple keeps these in ImageIO's dictionary; the two an animation needs are the
 * GIF ones, and the pixel size is the one a caller asks before decoding. */
extern const CFStringRef kCGImagePropertyPixelWidth;
extern const CFStringRef kCGImagePropertyPixelHeight;
extern const CFStringRef kCGImagePropertyGIFDictionary;
extern const CFStringRef kCGImagePropertyGIFDelayTime;
extern const CFStringRef kCGImagePropertyGIFUnclampedDelayTime;

/* THE SOURCE RETAINS WHAT IT IS GIVEN, so a caller may release the data as soon as this returns. NULL for bytes no
 * decoder in this build claims — the refusal is printed by the registry, once, naming what it does know. */
CGImageSourceRef CGImageSourceCreateWithData(CFDataRef data, CFDictionaryRef options) CF_RETURNS_RETAINED;
CGImageSourceRef CGImageSourceCreateWithDataProvider(CGDataProviderRef provider, CFDictionaryRef options)
	CF_RETURNS_RETAINED;

/* THE TYPE OF THE FIRST IMAGE, as a UTI (`public.png`, `com.compuserve.gif`, …), or NULL for an empty source. */
CFStringRef CGImageSourceGetType(CGImageSourceRef source);

/* HOW MANY IMAGES THE SOURCE HOLDS. A still image is one; a GIF is as many frames as it has. */
size_t CGImageSourceGetCount(CGImageSourceRef source);

/* THE IMAGE AT `index`, or NULL past the end. Every index is decoded on demand, so a 200-frame GIF costs one frame. */
CGImageRef CGImageSourceCreateImageAtIndex(CGImageSourceRef source, size_t index, CFDictionaryRef options)
	CF_RETURNS_RETAINED;

/* WHAT THE SOURCE KNOWS ABOUT ONE IMAGE WITHOUT DECODING IT: its pixel size, and — for an animation — the delay its
 * frame is shown for, in seconds, under the GIF dictionary's two keys. */
CFDictionaryRef CGImageSourceCopyPropertiesAtIndex(CGImageSourceRef source, size_t index, CFDictionaryRef options)
	CF_RETURNS_RETAINED;

/* **EVERY UTI THIS BUILD CAN DECODE** — the measured answer to "what formats does it support", and the list a bundle's
 * declared document types should agree with. */
CFArrayRef CGImageSourceCopyTypeIdentifiers(void) CF_RETURNS_RETAINED;

void CGImageSourceRetain(CGImageSourceRef source);
void CGImageSourceRelease(CGImageSourceRef source);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGIMAGESOURCE_H */
