/*
 * CGPDFObject.h — one value out of a PDF file, and the question every caller asks it: what are you?
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **A PDF OBJECT IS BORROWED, NOT RETAINED.** There is no `CGPDFObjectRetain` in Apple's header and there is
 * none here: an object belongs to the document that parsed it and lives exactly as long as that document. The
 * array and dictionary doors below hand out the same borrowed reference, which is why an entire walk of a page
 * tree allocates nothing.
 *
 * **INDIRECT REFERENCES ARE RESOLVED BEFORE THIS HEADER SEES THEM.** In the file an array's element may be
 * `4 0 R`; every door in this library resolves that reference against the document's cross-reference table, so
 * a caller never meets the reference itself. A reference that cannot be resolved answers as a NULL object —
 * `kCGPDFObjectTypeNull` — rather than a failure, because that is what a damaged file means at this level.
 *
 * THE TYPES AND THEIR VALUES ARE APPLE'S, in Apple's order and with Apple's numbering, because a caller writes
 * `kCGPDFObjectTypeInteger` and not a number.
 */
#ifndef CORE_GRAPHICS_CGPDFOBJECT_H
#define CORE_GRAPHICS_CGPDFOBJECT_H

#include <CoreGraphics/CGBase.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFObject *CGPDFObjectRef;

/* APPLE'S THREE SCALAR SPELLINGS, from its own CGPDFObject.h. `CGPDFInteger` is a `long int`, not an `int`, and
 * that is load-bearing: a PDF integer is up to 64 bits, and a 32-bit answer would truncate a file's `/Length`. */
typedef unsigned char CGPDFBoolean;
typedef long int CGPDFInteger;
typedef CGFloat CGPDFReal;

/* APPLE'S ENUM, STARTING AT ONE so that a zeroed object is not a type. `kCGPDFObjectTypeNull` is a real type
 * here — a PDF `null` — and an unresolvable reference answers with it. */
typedef enum CGPDFObjectType {
	kCGPDFObjectTypeNull = 1,
	kCGPDFObjectTypeBoolean,
	kCGPDFObjectTypeInteger,
	kCGPDFObjectTypeReal,
	kCGPDFObjectTypeName,
	kCGPDFObjectTypeString,
	kCGPDFObjectTypeArray,
	kCGPDFObjectTypeDictionary,
	kCGPDFObjectTypeStream
} CGPDFObjectType;

/* WHAT THIS OBJECT IS. A NULL object is `kCGPDFObjectTypeNull`, never a crash. */
CG_EXTERN CGPDFObjectType CGPDFObjectGetType(CGPDFObjectRef object);

/* **THE VALUE, IN THE SHAPE THE TYPE NAMES.** `value` points at the matching C type:
 *
 *   kCGPDFObjectTypeBoolean     CGPDFBoolean *        (unsigned char)
 *   kCGPDFObjectTypeInteger     CGPDFInteger *        (long)
 *   kCGPDFObjectTypeReal        CGPDFReal *           (double)
 *   kCGPDFObjectTypeName        char **               (the decoded name, NUL-terminated)
 *   kCGPDFObjectTypeString      CGPDFStringRef *
 *   kCGPDFObjectTypeArray       CGPDFArrayRef *
 *   kCGPDFObjectTypeDictionary  CGPDFDictionaryRef *
 *   kCGPDFObjectTypeStream      CGPDFStreamRef *
 *   kCGPDFObjectTypeNull        may be NULL
 *
 * FALSE WHEN THE OBJECT IS NOT THAT TYPE, which is the whole answer: the caller compares types with it rather
 * than casting. A `null` asks for no value and is the one type whose `value` may be NULL. */
CG_EXTERN bool CGPDFObjectGetValue(CGPDFObjectRef object, CGPDFObjectType type, void *value);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFOBJECT_H */
