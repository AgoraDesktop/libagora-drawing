/*
 * CGPDFStream.h — a dictionary with a run of bytes behind it, and the one door that gets those bytes out.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **`CGPDFStreamCopyData` IS WHERE THE FILTERS ARE APPLIED.** The bytes in the file are the *encoded* bytes —
 * `/Filter /FlateDecode` means they are a zlib stream, and the dictionary's `/DecodeParms` carries the predictor
 * that turns the inflated bytes into rows. The copy door returns the *decoded* bytes, which is the only thing a
 * caller can use, and `dataFormat` says whether the result is something this build decoded or an image codec's
 * payload it deliberately did not touch.
 *
 * WHAT IS DECODED: FlateDecode, LZWDecode, ASCIIHexDecode, ASCII85Decode and RunLengthDecode, with the PNG and
 * TIFF predictors. WHAT IS NOT: the image codecs — DCTDecode, JPXDecode, JBIG2Decode and CCITTFaxDecode. Those
 * are reported through `dataFormat` and their bytes come back untouched, because decoding a JPEG here would be
 * this library pretending to be the image layer.
 */
#ifndef CORE_GRAPHICS_CGPDFSTREAM_H
#define CORE_GRAPHICS_CGPDFSTREAM_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGPDFDictionary.h>

#include <CoreFoundation/CFData.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFObject *CGPDFStreamRef;

/* APPLE'S THREE, IN APPLE'S ORDER. `Raw` is everything this build decoded itself. */
typedef enum CGPDFDataFormat {
	CGPDFDataFormatRaw,
	CGPDFDataFormatJPEGEncoded,
	CGPDFDataFormatJPEG2000
} CGPDFDataFormat;

CG_EXTERN CGPDFDictionaryRef CGPDFStreamGetDictionary(CGPDFStreamRef stream);

/* TRUE WHEN THE STREAM WAS READ, and `format` is then what its bytes are. False when the filter chain names
 * something this build does not decode — with a `CG-REFUSE` line naming the filter. */
CG_EXTERN bool CGPDFStreamGetDataFormat(CGPDFStreamRef stream, CGPDFDataFormat *format);

/* +1, OR NULL. Flate is zlib's, and the predictors are applied here; an image codec's stream comes back as the
 * bytes the file holds. */
CG_EXTERN CFDataRef CGPDFStreamCopyData(CGPDFStreamRef stream, CGPDFDataFormat *format) CF_RETURNS_RETAINED;

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFSTREAM_H */
