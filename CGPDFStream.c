/*
 * CGPDFStream.c — the filters, and the one door that gets a stream's bytes out.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **A FILTER CHAIN IS APPLIED IN THE ORDER THE DICTIONARY LISTS IT.** `/Filter [/FlateDecode /ASCIIHexDecode]`
 * means the file holds hex, and underneath it zlib, and underneath that the data; decoding walks the array from
 * the first entry to the last. `/DecodeParms` is the parallel array (or a single dictionary when the filter is a
 * single name), which is why every decoder takes its own parameters.
 *
 * **FIVE DECODERS AND THE TWO PREDICTORS, WHICH IS THE SET A DOCUMENT'S STRUCTURE NEEDS.** Flate is zlib's and
 * is what every modern file uses for content, cross-reference and object streams; LZW is the pre-1.2 spelling
 * that still appears in scanned archives; ASCIIHex, ASCII85 and RunLength are the simple transports. The
 * predictors are how a Flate stream carries image rows, and the CROSS-REFERENCE STREAM uses the PNG predictor
 * with a width of five bytes — so a reader that skips predictors cannot even open a PDF 1.5 file.
 *
 * **THE IMAGE CODECS ARE NOT DECODED HERE.** DCTDecode, JPXDecode, JBIG2Decode and CCITTFaxDecode are named,
 * reported through `CGPDFDataFormat`, and their payload returned untouched. A JPEG is the image layer's
 * business; decoding it into samples here would be a second JPEG decoder in a library that already has one.
 *
 * **A WRONG `/Length` IS THE COMMON DAMAGE AND IT IS ALREADY HANDLED** by the parser, which scans for
 * `endstream` when the length does not check out. So this file may assume its bytes are the stream's bytes.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFStream.h>

#include <zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* a working buffer for the decoders, outside the document's arena            */
/* ------------------------------------------------------------------------- */

typedef struct {
	unsigned char *bytes;
	size_t len;
	size_t cap;
	bool failed;
} cg_pdf_buffer;

static void fn_buffer_free(cg_pdf_buffer *buf)
{
	free(buf->bytes);
	buf->bytes = NULL;
	buf->len = 0;
	buf->cap = 0;
}

static bool fn_buffer_reserve(cg_pdf_buffer *buf, size_t extra)
{
	size_t need = buf->len + extra;

	if (need <= buf->cap) {
		return true;
	}
	{
		size_t cap = buf->cap ? buf->cap : 256;
		unsigned char *grown;

		while (cap < need) {
			cap *= 2;
		}
		grown = realloc(buf->bytes, cap);
		if (grown == NULL) {
			buf->failed = true;
			return false;
		}
		buf->bytes = grown;
		buf->cap = cap;
	}
	return true;
}

static bool fn_buffer_put(cg_pdf_buffer *buf, const void *bytes, size_t n)
{
	if (n == 0) {
		return true;
	}
	if (!fn_buffer_reserve(buf, n)) {
		return false;
	}
	memcpy(buf->bytes + buf->len, bytes, n);
	buf->len += n;
	return true;
}

static bool fn_buffer_byte(cg_pdf_buffer *buf, unsigned char byte)
{
	return fn_buffer_put(buf, &byte, 1);
}

static CFDataRef fn_buffer_copy(cg_pdf_buffer *buf)
{
	CFDataRef result;

	if (buf->failed) {
		fn_buffer_free(buf);
		return NULL;
	}
	result = CFDataCreate(kCFAllocatorDefault, buf->bytes, (CFIndex)buf->len);
	fn_buffer_free(buf);
	return result;
}

/* ------------------------------------------------------------------------- */
/* the simple transports                                                     */
/* ------------------------------------------------------------------------- */

static CFDataRef fn_ascii_hex(CFDataRef in)
{
	const unsigned char *bytes = CFDataGetBytePtr(in);
	CFIndex len = CFDataGetLength(in);
	cg_pdf_buffer out = { 0 };
	int high = -1;
	CFIndex i;

	for (i = 0; i < len; i++) {
		unsigned char c = bytes[i];
		int digit = -1;

		if (c == '>') {
			break;
		}
		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10;
		}
		if (digit < 0) {
			continue;
		}
		if (high < 0) {
			high = digit;
		} else {
			fn_buffer_byte(&out, (unsigned char)((high << 4) | digit));
			high = -1;
		}
	}
	if (high >= 0) {
		fn_buffer_byte(&out, (unsigned char)(high << 4));
	}
	return fn_buffer_copy(&out);
}

static CFDataRef fn_ascii85(CFDataRef in)
{
	const unsigned char *bytes = CFDataGetBytePtr(in);
	CFIndex len = CFDataGetLength(in);
	cg_pdf_buffer out = { 0 };
	unsigned long tuple = 0;
	int count = 0;
	CFIndex i;

	for (i = 0; i < len; i++) {
		unsigned char c = bytes[i];

		if (c == '~') {
			break;
		}
		if (c == 'z' && count == 0) {
			unsigned char zero[4] = { 0, 0, 0, 0 };

			fn_buffer_put(&out, zero, 4);
			continue;
		}
		if (c < '!' || c > 'u') {
			continue;
		}
		tuple = tuple * 85 + (unsigned long)(c - '!');
		if (++count == 5) {
			unsigned char group[4];

			group[0] = (unsigned char)(tuple >> 24);
			group[1] = (unsigned char)(tuple >> 16);
			group[2] = (unsigned char)(tuple >> 8);
			group[3] = (unsigned char)tuple;
			fn_buffer_put(&out, group, 4);
			tuple = 0;
			count = 0;
		}
	}
	if (count > 0) {
		unsigned char group[4];
		int n = count;

		while (count < 5) {
			tuple = tuple * 85 + 84;
			count++;
		}
		group[0] = (unsigned char)(tuple >> 24);
		group[1] = (unsigned char)(tuple >> 16);
		group[2] = (unsigned char)(tuple >> 8);
		group[3] = (unsigned char)tuple;
		fn_buffer_put(&out, group, (size_t)(n - 1));
	}
	return fn_buffer_copy(&out);
}

static CFDataRef fn_run_length(CFDataRef in)
{
	const unsigned char *bytes = CFDataGetBytePtr(in);
	CFIndex len = CFDataGetLength(in);
	cg_pdf_buffer out = { 0 };
	CFIndex i = 0;

	while (i < len) {
		unsigned char control = bytes[i++];

		if (control == 128) {
			break;
		}
		if (control < 128) {
			size_t n = (size_t)control + 1;

			if (i + (CFIndex)n > len) {
				n = (size_t)(len - i);
			}
			fn_buffer_put(&out, bytes + i, n);
			i += (CFIndex)n;
		} else {
			unsigned char byte;

			if (i >= len) {
				break;
			}
			byte = bytes[i++];
			{
				size_t n = (size_t)(257 - control);
				size_t k;

				for (k = 0; k < n; k++) {
					fn_buffer_byte(&out, byte);
				}
			}
		}
	}
	return fn_buffer_copy(&out);
}

/* ------------------------------------------------------------------------- */
/* Flate                                                                     */
/* ------------------------------------------------------------------------- */

static CFDataRef fn_inflate(CFDataRef in, int window_bits)
{
	z_stream zs;
	cg_pdf_buffer out = { 0 };
	unsigned char chunk[65536];
	int status;

	memset(&zs, 0, sizeof zs);
	if (inflateInit2(&zs, window_bits) != Z_OK) {
		return NULL;
	}
	zs.next_in = (Bytef *)CFDataGetBytePtr(in);
	zs.avail_in = (uInt)CFDataGetLength(in);
	do {
		zs.next_out = chunk;
		zs.avail_out = (uInt)sizeof chunk;
		status = inflate(&zs, Z_NO_FLUSH);
		if (status != Z_OK && status != Z_STREAM_END) {
			/* A RAW DEFLATE STREAM IS NOT A ZLIB STREAM, and a few files confuse the two. The retry is
			 * the caller's, because the header choice is a property of the bytes. */
			inflateEnd(&zs);
			fn_buffer_free(&out);
			return NULL;
		}
		if (!fn_buffer_put(&out, chunk, sizeof chunk - zs.avail_out)) {
			inflateEnd(&zs);
			return NULL;
		}
	} while (status != Z_STREAM_END && zs.avail_in > 0);
	inflateEnd(&zs);
	return fn_buffer_copy(&out);
}

static CFDataRef fn_flate(CFDataRef in)
{
	CFDataRef out = fn_inflate(in, MAX_WBITS);

	if (out == NULL) {
		out = fn_inflate(in, -MAX_WBITS);
	}
	return out;
}

/* ------------------------------------------------------------------------- */
/* LZW                                                                       */
/* ------------------------------------------------------------------------- */

struct lzw_entry {
	int prefix;
	unsigned char byte;
	int length;
};

static CFDataRef fn_lzw(CFDataRef in, int early_change)
{
	const unsigned char *bytes = CFDataGetBytePtr(in);
	size_t len = (size_t)CFDataGetLength(in);
	cg_pdf_buffer out = { 0 };
	struct lzw_entry *table;
	unsigned char *stack;
	int next_code = 258;
	int code_length = 9;
	unsigned long bit_buffer = 0;
	int bits = 0;
	int previous = -1;
	size_t at = 0;

	table = malloc(4096 * sizeof *table);
	stack = malloc(4096);
	if (table == NULL || stack == NULL) {
		free(table);
		free(stack);
		return NULL;
	}
	{
		int i;

		for (i = 0; i < 256; i++) {
			table[i].prefix = -1;
			table[i].byte = (unsigned char)i;
			table[i].length = 1;
		}
	}
	for (;;) {
		int code;

		while (bits < code_length) {
			if (at >= len) {
				goto done;
			}
			bit_buffer = (bit_buffer << 8) | bytes[at++];
			bits += 8;
		}
		code = (int)((bit_buffer >> (bits - code_length)) & ((1u << code_length) - 1));
		bits -= code_length;

		if (code == 256) {
			next_code = 258;
			code_length = 9;
			previous = -1;
			continue;
		}
		if (code == 257) {
			break;
		}
		if (code > next_code || code >= 4096) {
			break;
		}
		if (previous < 0) {
			/* THE FIRST CODE AFTER A CLEAR IS A SINGLE BYTE, and anything else is a damaged stream. */
			if (code >= 256) {
				break;
			}
			fn_buffer_byte(&out, (unsigned char)code);
			previous = code;
			continue;
		}
		if (code == next_code) {
			/* KWKWK: the entry is the previous string plus its own first byte. */
			unsigned char first;
			int c = previous;
			int n = 0;

			while (c >= 0 && n < 4096) {
				stack[n++] = table[c].byte;
				c = table[c].prefix;
			}
			first = stack[n - 1];
			/* emit previous string, then its first byte */
			{
				int k;

				for (k = n - 1; k >= 0; k--) {
					fn_buffer_byte(&out, stack[k]);
				}
			}
			fn_buffer_byte(&out, first);
			if (next_code < 4096) {
				table[next_code].prefix = previous;
				table[next_code].byte = first;
				table[next_code].length = table[previous].length + 1;
				next_code++;
			}
		} else {
			int c = code;
			int n = 0;

			while (c >= 0 && n < 4096) {
				stack[n++] = table[c].byte;
				c = table[c].prefix;
			}
			{
				int k;

				for (k = n - 1; k >= 0; k--) {
					fn_buffer_byte(&out, stack[k]);
				}
			}
			if (next_code < 4096) {
				table[next_code].prefix = previous;
				table[next_code].byte = stack[n - 1];
				table[next_code].length = table[previous].length + 1;
				next_code++;
			}
		}
		previous = code;
		if (next_code + early_change >= (1 << code_length) && code_length < 12) {
			code_length++;
		}
	}
done:
	free(table);
	free(stack);
	return fn_buffer_copy(&out);
}

/* ------------------------------------------------------------------------- */
/* predictors                                                                */
/* ------------------------------------------------------------------------- */

static unsigned char fn_paeth(unsigned char a, unsigned char b, unsigned char c)
{
	int p = (int)a + (int)b - (int)c;
	int pa = p > (int)a ? p - (int)a : (int)a - p;
	int pb = p > (int)b ? p - (int)b : (int)b - p;
	int pc = p > (int)c ? p - (int)c : (int)c - p;

	if (pa <= pb && pa <= pc) {
		return a;
	}
	if (pb <= pc) {
		return b;
	}
	return c;
}

static CFDataRef fn_predictor(CFDataRef in, long long predictor, long long colors, long long bpc, long long columns)
{
	size_t row_in;
	size_t row_out;
	size_t bpp;
	const unsigned char *src;
	size_t src_len;
	cg_pdf_buffer out = { 0 };

	if (predictor <= 1) {
		return (CFDataRef)CFRetain(in);
	}
	if (colors <= 0) {
		colors = 1;
	}
	if (bpc <= 0) {
		bpc = 8;
	}
	if (columns <= 0) {
		columns = 1;
	}
	row_out = (size_t)((columns * colors * bpc + 7) / 8);
	bpp = (size_t)((colors * bpc + 7) / 8);
	if (bpp < 1) {
		bpp = 1;
	}
	src = CFDataGetBytePtr(in);
	src_len = (size_t)CFDataGetLength(in);

	if (predictor == 2) {
		/* TIFF PREDICTOR: EACH SAMPLE ADDS THE ONE A PIXEL TO ITS LEFT. Eight-bit samples only — the
		 * sub-byte case is legal but vanishingly rare, and refused rather than mis-decoded. */
		size_t at;

		if (bpc != 8) {
			fprintf(stderr, "CG-REFUSE: a TIFF predictor with %lld bits per component — this reader "
					"implements the eight-bit case only\n", bpc);
			return NULL;
		}
		if (!fn_buffer_put(&out, src, src_len)) {
			return NULL;
		}
		for (at = 0; at + row_out <= src_len; at += row_out) {
			size_t i;

			for (i = bpp; i < row_out; i++) {
				out.bytes[at + i] = (unsigned char)(out.bytes[at + i] + out.bytes[at + i - bpp]);
			}
		}
		return fn_buffer_copy(&out);
	}

	/* PNG PREDICTORS 10..15: EVERY ROW CARRIES ITS OWN FILTER BYTE, and 15 means "whichever per row" just as
	 * 10..14 mean "this one, always". The decoder reads the byte either way, which is why they are one path. */
	row_in = row_out + 1;
	{
		size_t rows = src_len / row_in;
		size_t r;

		if (!fn_buffer_reserve(&out, rows * row_out)) {
			return NULL;
		}
		for (r = 0; r < rows; r++) {
			const unsigned char *in_row = src + r * row_in;
			unsigned char filter = in_row[0];
			unsigned char *row = out.bytes + out.len;
			const unsigned char *above = r > 0 ? out.bytes + (r - 1) * row_out : NULL;
			size_t i;

			out.len += row_out;
			for (i = 0; i < row_out; i++) {
				unsigned char x = in_row[1 + i];
				unsigned char a = i >= bpp ? row[i - bpp] : 0;
				unsigned char b = above != NULL ? above[i] : 0;
				unsigned char c = (above != NULL && i >= bpp) ? above[i - bpp] : 0;

				switch (filter) {
				case 0: row[i] = x; break;
				case 1: row[i] = (unsigned char)(x + a); break;
				case 2: row[i] = (unsigned char)(x + b); break;
				case 3: row[i] = (unsigned char)(x + ((unsigned)(a + b) >> 1)); break;
				case 4: row[i] = (unsigned char)(x + fn_paeth(a, b, c)); break;
				default:
					/* AN UNKNOWN FILTER IS NOT GUESSED AT: the row would be noise. */
					fn_buffer_free(&out);
					return NULL;
				}
			}
		}
	}
	return fn_buffer_copy(&out);
}

/* ------------------------------------------------------------------------- */
/* the chain                                                                 */
/* ------------------------------------------------------------------------- */

static bool fn_is_image_codec(const char *name)
{
	return strcmp(name, "DCTDecode") == 0 || strcmp(name, "DCT") == 0 || strcmp(name, "JPXDecode") == 0 ||
	       strcmp(name, "JBIG2Decode") == 0 || strcmp(name, "CCITTFaxDecode") == 0 ||
	       strcmp(name, "CCF") == 0;
}

static CFDataRef fn_decode(cg_pdf_doc *doc, const char *name, CFDataRef in, cg_pdf_obj *parms)
{
	long long predictor = 1;
	long long colors = 1;
	long long bpc = 8;
	long long columns = 1;
	long long early_change = 1;

	if (parms != NULL) {
		cg_pdf_integer(doc, cg_pdf_dict_get(doc, parms, "Predictor"), &predictor);
		cg_pdf_integer(doc, cg_pdf_dict_get(doc, parms, "Colors"), &colors);
		cg_pdf_integer(doc, cg_pdf_dict_get(doc, parms, "BitsPerComponent"), &bpc);
		cg_pdf_integer(doc, cg_pdf_dict_get(doc, parms, "Columns"), &columns);
		cg_pdf_integer(doc, cg_pdf_dict_get(doc, parms, "EarlyChange"), &early_change);
	}

	if (strcmp(name, "FlateDecode") == 0 || strcmp(name, "Fl") == 0) {
		CFDataRef inflated = fn_flate(in);
		CFDataRef result;

		if (inflated == NULL) {
			fprintf(stderr, "CG-REFUSE: a FlateDecode stream did not inflate\n");
			return NULL;
		}
		result = fn_predictor(inflated, predictor, colors, bpc, columns);
		CFRelease(inflated);
		return result;
	}
	if (strcmp(name, "LZWDecode") == 0 || strcmp(name, "LZW") == 0) {
		CFDataRef decoded = fn_lzw(in, early_change ? 1 : 0);
		CFDataRef result;

		if (decoded == NULL) {
			fprintf(stderr, "CG-REFUSE: an LZWDecode stream did not decode\n");
			return NULL;
		}
		result = fn_predictor(decoded, predictor, colors, bpc, columns);
		CFRelease(decoded);
		return result;
	}
	if (strcmp(name, "ASCIIHexDecode") == 0 || strcmp(name, "AHx") == 0) {
		return fn_ascii_hex(in);
	}
	if (strcmp(name, "ASCII85Decode") == 0 || strcmp(name, "A85") == 0) {
		return fn_ascii85(in);
	}
	if (strcmp(name, "RunLengthDecode") == 0 || strcmp(name, "RL") == 0) {
		return fn_run_length(in);
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* public doors                                                              */
/* ------------------------------------------------------------------------- */

CGPDFDictionaryRef CGPDFStreamGetDictionary(CGPDFStreamRef stream)
{
	cg_pdf_obj *s = (cg_pdf_obj *)stream;

	if (s == NULL || s->type != kCGPDFObjectTypeStream) {
		return NULL;
	}
	return (CGPDFDictionaryRef)s->u.stream.dict;
}

static bool fn_filter_names(cg_pdf_doc *doc, cg_pdf_obj *dict, cg_pdf_obj ***names, size_t *count)
{
	cg_pdf_obj *filter = NULL;

	*names = NULL;
	*count = 0;
	for (size_t i = 0; i < dict->u.dict.count; i++) {
		if (strcmp(dict->u.dict.entries[i].key, "Filter") == 0) {
			filter = cg_pdf_resolve(doc, dict->u.dict.entries[i].value);
			break;
		}
	}
	if (filter == NULL) {
		return true;
	}
	if (filter->type == kCGPDFObjectTypeName) {
		cg_pdf_obj **one = cg_pdf_alloc(doc, sizeof *one);

		if (one == NULL) {
			return false;
		}
		one[0] = filter;
		*names = one;
		*count = 1;
		return true;
	}
	if (filter->type == kCGPDFObjectTypeArray) {
		*names = filter->u.array.items;
		*count = filter->u.array.count;
		return true;
	}
	return false;
}

static bool fn_has_codec(cg_pdf_doc *doc, cg_pdf_obj *dict)
{
	cg_pdf_obj **names = NULL;
	size_t count = 0;
	size_t i;

	if (!fn_filter_names(doc, dict, &names, &count)) {
		return false;
	}
	for (i = 0; i < count; i++) {
		cg_pdf_obj *name = cg_pdf_resolve(doc, names[i]);

		if (name != NULL && name->type == kCGPDFObjectTypeName &&
		    fn_is_image_codec((const char *)name->u.name.bytes)) {
			return true;
		}
	}
	return false;
}

CFDataRef cg_pdf_stream_data(cg_pdf_doc *doc, cg_pdf_obj *stream)
{
	cg_pdf_obj *dict;
	cg_pdf_obj **names = NULL;
	cg_pdf_obj *parms = NULL;
	size_t count = 0;
	size_t i;
	CFDataRef data;

	stream = cg_pdf_resolve(doc, stream);
	if (stream == NULL || stream->type != kCGPDFObjectTypeStream) {
		return NULL;
	}
	dict = stream->u.stream.dict;
	if (fn_has_codec(doc, dict)) {
		return CFDataCreate(kCFAllocatorDefault, stream->u.stream.raw, (CFIndex)stream->u.stream.raw_len);
	}
	if (!fn_filter_names(doc, dict, &names, &count)) {
		fprintf(stderr, "CG-REFUSE: a stream's /Filter is neither a name nor an array\n");
		return NULL;
	}
	for (i = 0; i < dict->u.dict.count; i++) {
		if (strcmp(dict->u.dict.entries[i].key, "DecodeParms") == 0 ||
		    strcmp(dict->u.dict.entries[i].key, "DP") == 0) {
			parms = cg_pdf_resolve(doc, dict->u.dict.entries[i].value);
			break;
		}
	}
	data = CFDataCreate(kCFAllocatorDefault, stream->u.stream.raw, (CFIndex)stream->u.stream.raw_len);
	if (data == NULL) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		cg_pdf_obj *name = cg_pdf_resolve(doc, names[i]);
		cg_pdf_obj *this_parms = parms;
		CFDataRef next;

		if (name == NULL || name->type != kCGPDFObjectTypeName) {
			CFRelease(data);
			fprintf(stderr, "CG-REFUSE: a stream's /Filter array holds something that is not a name\n");
			return NULL;
		}
		if (parms != NULL && parms->type == kCGPDFObjectTypeArray) {
			this_parms = i < parms->u.array.count ? parms->u.array.items[i] : NULL;
		} else if (parms != NULL && parms->type == kCGPDFObjectTypeNull) {
			this_parms = NULL;
		}
		next = fn_decode(doc, (const char *)name->u.name.bytes, data, this_parms);
		CFRelease(data);
		if (next == NULL) {
			fprintf(stderr, "CG-REFUSE: a stream names the filter `%s`, which this reader does not decode\n",
				(const char *)name->u.name.bytes);
			return NULL;
		}
		data = next;
	}
	return data;
}

bool CGPDFStreamGetDataFormat(CGPDFStreamRef stream, CGPDFDataFormat *format)
{
	cg_pdf_obj *s = (cg_pdf_obj *)stream;
	cg_pdf_obj *dict;
	cg_pdf_obj **names = NULL;
	size_t count = 0;
	size_t i;

	if (s == NULL || s->type != kCGPDFObjectTypeStream) {
		return false;
	}
	dict = s->u.stream.dict;
	if (!fn_filter_names(s->doc, dict, &names, &count)) {
		return false;
	}
	if (format != NULL) {
		*format = CGPDFDataFormatRaw;
	}
	for (i = 0; i < count; i++) {
		cg_pdf_obj *name = cg_pdf_resolve(s->doc, names[i]);

		if (name == NULL || name->type != kCGPDFObjectTypeName) {
			continue;
		}
		if (fn_is_image_codec((const char *)name->u.name.bytes)) {
			if (format != NULL) {
				*format = (strcmp((const char *)name->u.name.bytes, "JPXDecode") == 0) ?
						  CGPDFDataFormatJPEG2000 : CGPDFDataFormatJPEGEncoded;
			}
			return true;
		}
	}
	return true;
}

CFDataRef CGPDFStreamCopyData(CGPDFStreamRef stream, CGPDFDataFormat *format)
{
	cg_pdf_obj *s = (cg_pdf_obj *)stream;

	if (s == NULL || s->type != kCGPDFObjectTypeStream) {
		return NULL;
	}
	if (!CGPDFStreamGetDataFormat(stream, format)) {
		return NULL;
	}
	return cg_pdf_stream_data(s->doc, s);
}
