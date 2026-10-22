/*
 * CGPDFArray.h — a PDF array, and the typed doors that save a caller the two-step.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE TYPED DOORS ARE NOT CONVENIENCES, THEY ARE THE REFERENCE RESOLVER.** `CGPDFArrayGetDictionary(a, i)`
 * is `CGPDFArrayGetObject` followed by a resolve followed by a type check — and the resolve is the part a
 * caller cannot do for itself, because the indirect tables live on the document. A `/Kids` array holds
 * references; every door here hands back the object those references name.
 *
 * AN INDEX OUT OF RANGE ANSWERS NULL OR FALSE AND PRINTS NOTHING: a caller that walks `GetCount` never sees it,
 * and a caller that guesses is answered honestly rather than crashing.
 */
#ifndef CORE_GRAPHICS_CGPDFARRAY_H
#define CORE_GRAPHICS_CGPDFARRAY_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGPDFObject.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFObject *CGPDFArrayRef;

/* THE REFERENCE TYPES THE TYPED DOORS NAME. Each is the same struct forward-declared by its own header; the
 * repetition is what lets this header be read alone. */
typedef struct CGPDFObject *CGPDFStringRef;
typedef struct CGPDFObject *CGPDFDictionaryRef;
typedef struct CGPDFObject *CGPDFStreamRef;

/* A NAME IS THE DECODED BYTES, NUL-TERMINATED, AND BORROWED FROM THE DOCUMENT — `#xx` escapes are already
 * taken out, so `/A#20B` answers "A B" and a caller compares it with `strcmp`. */
typedef void (*CGPDFArrayApplierFunction)(size_t index, CGPDFObjectRef value, void *info);

CG_EXTERN size_t CGPDFArrayGetCount(CGPDFArrayRef array);

CG_EXTERN bool CGPDFArrayGetObject(CGPDFArrayRef array, size_t index, CGPDFObjectRef *value);
CG_EXTERN bool CGPDFArrayGetNull(CGPDFArrayRef array, size_t index);
CG_EXTERN bool CGPDFArrayGetBoolean(CGPDFArrayRef array, size_t index, CGPDFBoolean *value);
CG_EXTERN bool CGPDFArrayGetInteger(CGPDFArrayRef array, size_t index, CGPDFInteger *value);
CG_EXTERN bool CGPDFArrayGetNumber(CGPDFArrayRef array, size_t index, CGPDFReal *value);
CG_EXTERN bool CGPDFArrayGetName(CGPDFArrayRef array, size_t index, const char **value);
CG_EXTERN bool CGPDFArrayGetString(CGPDFArrayRef array, size_t index, CGPDFStringRef *value);
CG_EXTERN bool CGPDFArrayGetArray(CGPDFArrayRef array, size_t index, CGPDFArrayRef *value);
CG_EXTERN bool CGPDFArrayGetDictionary(CGPDFArrayRef array, size_t index, CGPDFDictionaryRef *value);
CG_EXTERN bool CGPDFArrayGetStream(CGPDFArrayRef array, size_t index, CGPDFStreamRef *value);

/* IN ORDER, FROM ZERO. The applier sees the RESOLVED object, exactly as the typed doors do. */
CG_EXTERN void CGPDFArrayApply(CGPDFArrayRef array, CGPDFArrayApplierFunction applier, void *info);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFARRAY_H */
