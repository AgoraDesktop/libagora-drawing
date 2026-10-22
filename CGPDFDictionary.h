/*
 * CGPDFDictionary.h — the PDF's only keyed container, which is how every PDF structure is spelled.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * A KEY IS A NAME, so it is a C string here: `/MediaBox` is looked up as `"MediaBox"`, decoded, with `#xx`
 * escapes already removed. VALUES ARE RESOLVED, exactly as an array's elements are — a page's `/Contents` is
 * normally a reference, and every typed door hands back the stream it names rather than the reference.
 *
 * **A MISSING KEY ANSWERS NULL OR FALSE, AND THAT IS NOT AN ERROR.** PDF inheritance means a key genuinely may
 * be absent on a page and present on its `/Pages` ancestor; the flattening that honours inheritance happens in
 * `CGPDFPage`, and a caller reading a page dictionary directly is reading the file, not Apple's model.
 */
#ifndef CORE_GRAPHICS_CGPDFDICTIONARY_H
#define CORE_GRAPHICS_CGPDFDICTIONARY_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGPDFObject.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFObject *CGPDFDictionaryRef;

typedef struct CGPDFObject *CGPDFStringRef;
typedef struct CGPDFObject *CGPDFArrayRef;
typedef struct CGPDFObject *CGPDFStreamRef;

typedef void (*CGPDFDictionaryApplierFunction)(const char *key, CGPDFObjectRef value, void *info);

CG_EXTERN size_t CGPDFDictionaryGetCount(CGPDFDictionaryRef dict);

CG_EXTERN bool CGPDFDictionaryGetObject(CGPDFDictionaryRef dict, const char *key, CGPDFObjectRef *value);
CG_EXTERN bool CGPDFDictionaryGetBoolean(CGPDFDictionaryRef dict, const char *key, CGPDFBoolean *value);
CG_EXTERN bool CGPDFDictionaryGetInteger(CGPDFDictionaryRef dict, const char *key, CGPDFInteger *value);
CG_EXTERN bool CGPDFDictionaryGetNumber(CGPDFDictionaryRef dict, const char *key, CGPDFReal *value);
CG_EXTERN bool CGPDFDictionaryGetName(CGPDFDictionaryRef dict, const char *key, const char **value);
CG_EXTERN bool CGPDFDictionaryGetString(CGPDFDictionaryRef dict, const char *key, CGPDFStringRef *value);
CG_EXTERN bool CGPDFDictionaryGetArray(CGPDFDictionaryRef dict, const char *key, CGPDFArrayRef *value);
CG_EXTERN bool CGPDFDictionaryGetDictionary(CGPDFDictionaryRef dict, const char *key,
					    CGPDFDictionaryRef *value);
CG_EXTERN bool CGPDFDictionaryGetStream(CGPDFDictionaryRef dict, const char *key, CGPDFStreamRef *value);

/* IN THE FILE'S OWN ORDER, because that is the only order a dictionary has. The key handed to the applier is
 * borrowed from the document and NUL-terminated. */
CG_EXTERN void CGPDFDictionaryApply(CGPDFDictionaryRef dict, CGPDFDictionaryApplierFunction applier, void *info);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFDICTIONARY_H */
