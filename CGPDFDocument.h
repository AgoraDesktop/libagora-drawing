/*
 * CGPDFDocument.h — a PDF file, opened, with its pages counted and its metadata reachable.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **THE DOCUMENT IS THE OWNER OF EVERYTHING BELOW IT.** Objects are parsed out of its bytes, its pages borrow
 * its dictionaries, and its cross-reference table is what makes an indirect reference answer at all. That is why
 * there is no `CGPDFObjectRetain`: a caller holds the document, and the document holds the file.
 *
 * **THE FILE IS READ THROUGH CORE GRAPHICS' OWN PROVIDER, NOT A NEW ONE.** `CreateWithURL` makes a
 * `CGDataProvider` over the file and `CreateWithProvider` is the real door — which also means a caller with bytes
 * in memory has no second API to learn.
 *
 * **THIS BUILD READS THE CROSS-REFERENCE IN BOTH SPELLINGS**, the classic `xref` table and the PDF 1.5
 * cross-reference stream, with object streams and a `/Prev` chain. Compression of the objects is the file's
 * business; the doors answer the decompressed structure.
 *
 * **ENCRYPTION IS REPORTED, NOT FAKED.** A document with an `/Encrypt` dictionary opens — so `IsEncrypted` can
 * answer true and a caller can tell an encrypted file from a broken one — but its pages and catalog are refused,
 * because there is no decryption here and a page of ciphertext drawn as content would be a lie.
 */
#ifndef CORE_GRAPHICS_CGPDFDOCUMENT_H
#define CORE_GRAPHICS_CGPDFDOCUMENT_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFPage.h>

#include <CoreFoundation/CFURL.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFDocument *CGPDFDocumentRef;

CG_EXTERN CGPDFDocumentRef CGPDFDocumentCreateWithProvider(CGDataProviderRef provider);
CG_EXTERN CGPDFDocumentRef CGPDFDocumentCreateWithURL(CFURLRef url);

/* **THE OBJECT IS A CORE FOUNDATION TYPE**, so `CFRetain`/`CFRelease` work on it exactly as Apple's does; these
 * two are the spellings Apple's header also declares. */
CG_EXTERN void CGPDFDocumentRetain(CGPDFDocumentRef document);
CG_EXTERN void CGPDFDocumentRelease(CGPDFDocumentRef document);
CG_EXTERN CGTypeID CGPDFDocumentGetTypeID(void);

CG_EXTERN size_t CGPDFDocumentGetNumberOfPages(CGPDFDocumentRef document);

/* **PAGES ARE NUMBERED FROM ONE**, as in the file and in Apple's API — not from zero as arrays are. The page is
 * OWNED BY THE DOCUMENT and is the same object on a second call; a caller that wants to keep it past the
 * document's life makes its own retain with `CGPDFPageRetain`. Out-of-range answers NULL. */
CG_EXTERN CGPDFPageRef CGPDFDocumentGetPage(CGPDFDocumentRef document, size_t pageNumber);

/* THE HEADER'S VERSION: 1 and 4 for `%PDF-1.4`. A file with no readable header answers 0 and 0. */
CG_EXTERN void CGPDFDocumentGetVersion(CGPDFDocumentRef document, int *majorVersion, int *minorVersion);

/* THE TRAILER'S `/Info` AND `/Root`, RESOLVED. Both are borrowed and both may be NULL. */
CG_EXTERN CGPDFDictionaryRef CGPDFDocumentGetInfo(CGPDFDocumentRef document);
CG_EXTERN CGPDFDictionaryRef CGPDFDocumentGetCatalog(CGPDFDocumentRef document);

/* THE TRAILER'S `/ID`, an array of two strings, or NULL. */
CG_EXTERN CGPDFArrayRef CGPDFDocumentGetID(CGPDFDocumentRef document);

CG_EXTERN bool CGPDFDocumentIsEncrypted(CGPDFDocumentRef document);
CG_EXTERN bool CGPDFDocumentIsUnlocked(CGPDFDocumentRef document);
CG_EXTERN bool CGPDFDocumentGetAllowsPrinting(CGPDFDocumentRef document);
CG_EXTERN bool CGPDFDocumentGetAllowsCopying(CGPDFDocumentRef document);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFDOCUMENT_H */
