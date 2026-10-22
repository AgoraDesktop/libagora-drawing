/*
 * CGPDFContentStream.h — a page's drawing instructions as a run of streams, plus the resources they name.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **A CONTENT STREAM IS NOT ONE STREAM.** A page's `/Contents` may be a single stream or an ARRAY of them, and
 * the specification defines the sequence as if the parts were concatenated — which is how a writer that cannot
 * know a page's length up front emits it. The type exists to make that one thing: `CreateWithPage` collects the
 * parts in order, and the scanner reads them in order.
 *
 * **THE RESOURCES ARE THE OTHER HALF.** `/F1` in a content stream is a name into the page's `/Resources`
 * `/Font` dictionary; a form XObject's content stream has its own resources and a parent to fall back on. That
 * parent chain is why this object has a parent at all, and `GetResource` is the door that walks it — the
 * scanner never does.
 *
 * EVERYTHING A CONTENT STREAM HOLDS IS BORROWED FROM ITS DOCUMENT. There is no copy here: the streams and the
 * resource dictionaries are the parsed objects, which is what makes a page's content free to open.
 */
#ifndef CORE_GRAPHICS_CGPDFCONTENTSTREAM_H
#define CORE_GRAPHICS_CGPDFCONTENTSTREAM_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGPDFStream.h>

#include <CoreFoundation/CFArray.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFContentStream *CGPDFContentStreamRef;

/* THE PAGE'S /Contents IN ORDER, with the page's effective /Resources as the first resource dictionary. A page
 * with no content answers an object with no streams, which scans to nothing rather than failing. */
CG_EXTERN CGPDFContentStreamRef CGPDFContentStreamCreateWithPage(CGPDFPageRef page) CF_RETURNS_RETAINED;

/* ONE STREAM, with its own resources and an optional parent whose resources are consulted after them. This is
 * the form a form XObject's scanner uses. */
CG_EXTERN CGPDFContentStreamRef CGPDFContentStreamCreateWithStream(CGPDFStreamRef stream,
								 CGPDFDictionaryRef streamResources,
								 CGPDFContentStreamRef parent) CF_RETURNS_RETAINED;

CG_EXTERN CGPDFContentStreamRef CGPDFContentStreamRetain(CGPDFContentStreamRef contentStream);
CG_EXTERN void CGPDFContentStreamRelease(CGPDFContentStreamRef contentStream);

/* THE STREAMS, IN ORDER, as a +1 array of borrowed `CGPDFStreamRef`s (the array is the only thing owned). */
CG_EXTERN CFArrayRef CGPDFContentStreamGetStreams(CGPDFContentStreamRef contentStream) CF_RETURNS_RETAINED;

/* **A RESOURCE BY CATEGORY AND NAME**: `GetResource(cs, "Font", "F1")`, `GetResource(cs, "XObject", "Im1")`.
 * The lookup is the stream's own `/Resources`, then the parent's, which is what the specification's inheritance
 * means for a form. The answer is BORROWED and NULL when the name is not defined. */
CG_EXTERN CGPDFObjectRef CGPDFContentStreamGetResource(CGPDFContentStreamRef contentStream, const char *category,
						       const char *name);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFCONTENTSTREAM_H */
