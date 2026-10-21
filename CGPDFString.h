/*
 * CGPDFString.h — a PDF string: bytes on the page, and the two readings the API offers.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **A PDF STRING IS BYTES, NOT TEXT, AND `GetBytePtr` SAYS SO.** The literal and hexadecimal spellings are
 * just two ways to write the same sequence, and the parser has already taken out the escapes: `(Hello\)` and
 * `<48656C6C6F29>` answer the same seven bytes.
 *
 * **THE TEXT READING IS PDFDocEncoding OR UTF-16BE, AND WHICH ONE IS A PROPERTY OF THE BYTES.** A leading
 * `FE FF` byte-order mark means UTF-16BE (the PDF 1.5+ way); anything else is PDFDocEncoding, which agrees with
 * Latin-1 for printable ASCII and for most of the upper half and differs in a scatter of slots. That mapping is
 * written out in `CGPDFString.c` because the difference is visible in a document's `/Title`.
 *
 * A DATE IS THE PDF's OWN `D:YYYYMMDDHHmmSSOHH'mm'` SPELLING, not a string Apple's date parser would take.
 */
#ifndef CORE_GRAPHICS_CGPDFSTRING_H
#define CORE_GRAPHICS_CGPDFSTRING_H

#include <CoreGraphics/CGBase.h>

#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFString.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFObject *CGPDFStringRef;

CG_EXTERN size_t CGPDFStringGetLength(CGPDFStringRef string);
CG_EXTERN const unsigned char *CGPDFStringGetBytePtr(CGPDFStringRef string);

/* **+1, AND MAY BE NULL WHEN THE BYTES ARE NOT TEXT.** UTF-16BE with a BOM is decoded by Core Foundation;
 * everything else is decoded through PDFDocEncoding's table. A string containing bytes that table leaves
 * undefined still answers a string — with those slots as U+FFFD — because a title with one bad byte is
 * still a title. */
CG_EXTERN CFStringRef CGPDFStringCopyTextString(CGPDFStringRef string) CF_RETURNS_RETAINED;

/* **+1, OR NULL WHEN THE BYTES ARE NOT A PDF DATE.** The `D:` prefix, the timezone (`Z`, `+`, `-`) and every
 * field after the year are optional in the spelling; a missing field is zero, and a zone-less date is read as
 * UTC rather than as local time, because a PDF has no local time. */
CG_EXTERN CFDateRef CGPDFStringCopyDate(CGPDFStringRef string) CF_RETURNS_RETAINED;

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFSTRING_H */
