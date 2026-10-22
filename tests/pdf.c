/*
 * pdf.c — the PDF reader's suite: two files, built byte by byte, and every door that answers about them.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE FIXTURES ARE BUILT HERE RATHER THAN CHECKED IN**, which is the only way the test can assert the
 * cross-reference offsets it is testing against: a checked-in PDF would be a black box whose `/Prev` chain the
 * test could not vary, and the xref-stream fixture in particular has to know each object's offset before the
 * table is written. Building them also means the compressed Flate payload is a known constant, decoded by the
 * library's own filter chain.
 *
 * TWO FILES, BECAUSE THE TWO SPELLINGS OF THE CROSS-REFERENCE ARE THE FORK IN A PDF READER:
 *
 *   - `build_classic` writes `xref` + `trailer`, three pages, inherited `/MediaBox` and `/Rotate`, a UTF-16BE
 *     title, a PDF date, and two filtered streams (one Flate, one ASCIIHex).
 *   - `build_xref_stream` writes the PDF 1.5 form: an `/XRef` stream with `/W [1 4 2]`, and an object stream
 *     holding the document's `/Info` — so the test exercises compressed objects as well as compressed tables.
 *
 * THE GEOMETRY CASE IS THE OTHER HALF OF THE LAYER. `CGPDFPageGetDrawingTransform` is the only real arithmetic
 * in the type, and its failure mode (scaling before rotating) is invisible on an unrotated page; the rotated
 * fixture page is what makes it visible, so the test maps the box's own corners through the transform and
 * checks where they land.
 */
#include <CoreGraphics/CGPDFArray.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGPDFObject.h>
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGPDFContentStream.h>
#include <CoreGraphics/CGPDFOperatorTable.h>
#include <CoreGraphics/CGPDFPage.h>
#include <CoreGraphics/CGPDFScanner.h>
#include <CoreGraphics/CGPDFStream.h>
#include <CoreGraphics/CGPDFString.h>

#include <CoreFoundation/CoreFoundation.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__); \
			fprintf(stderr, "\n"); \
		} \
	} while (0)

#define CHECK_NEAR(value, expected, tolerance, what) \
	CHECK(fabs((double)(value) - (double)(expected)) <= (tolerance), "%s: got %.9f, expected %.9f", \
	      (what), (double)(value), (double)(expected))

/* ------------------------------------------------------------------------- */
/* a byte builder                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
	char *bytes;
	size_t len;
	size_t cap;
} buf_t;

static void buf_put(buf_t *b, const void *bytes, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 1024;

		while (cap < b->len + n + 1) {
			cap *= 2;
		}
		b->bytes = realloc(b->bytes, cap);
		b->cap = cap;
	}
	memcpy(b->bytes + b->len, bytes, n);
	b->len += n;
	b->bytes[b->len] = '\0';
}

static void buf_printf(buf_t *b, const char *fmt, ...)
{
	char small[4096];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(small, sizeof small, fmt, ap);
	va_end(ap);
	if (n > 0) {
		buf_put(b, small, (size_t)n);
	}
}

static CFDataRef buf_data(buf_t *b)
{
	return CFDataCreate(kCFAllocatorDefault, (const UInt8 *)b->bytes, (CFIndex)b->len);
}

/* ------------------------------------------------------------------------- */
/* fixture A: classic cross-reference table                                  */
/* ------------------------------------------------------------------------- */

/* zlib's compressed spelling of "flate-payload", made once and written here so that the test does not link the
 * compressor it is testing the decompressor against. */
static const unsigned char FLATE_PAYLOAD[] = {
	0x78, 0x9c, 0x4b, 0xcb, 0x49, 0x2c, 0x49, 0xd5, 0x2d, 0x48, 0xac, 0xcc, 0xc9,
	0x4f, 0x4c, 0x01, 0x00, 0x23, 0xc9, 0x05, 0x24
};

static CFDataRef build_classic(void)
{
	buf_t b = { 0 };
	size_t offsets[10] = { 0 };
	const char *content = "BT /F1 12 Tf 10 20 Td (Hello) Tj ET\n";
	const char *hex = "4142434445>";
	size_t xref;
	int i;

	buf_printf(&b, "%%PDF-1.4\n");

	offsets[1] = b.len;
	buf_printf(&b, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

	offsets[2] = b.len;
	buf_printf(&b, "2 0 obj\n<< /Type /Pages /Kids [3 0 R 5 0 R 6 0 R] /Count 3 "
		       "/MediaBox [0 0 200 100] >>\nendobj\n");

	offsets[3] = b.len;
	buf_printf(&b, "3 0 obj\n<< /Type /Page /Parent 2 0 R /Rotate 90 /Contents 4 0 R >>\nendobj\n");

	offsets[4] = b.len;
	buf_printf(&b, "4 0 obj\n<< /Length %zu >>\nstream\n%s endstream\nendobj\n", strlen(content) + 1, content);

	offsets[5] = b.len;
	buf_printf(&b, "5 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 50] >>\nendobj\n");

	offsets[6] = b.len;
	buf_printf(&b, "6 0 obj\n<< /Type /Page /Parent 2 0 R /CropBox [10 10 90 40] >>\nendobj\n");

	offsets[7] = b.len;
	buf_printf(&b, "7 0 obj\n<< /Title <FEFF00480065006C006C006F> "
		       "/CreationDate (D:20240102030405Z) /Flate 8 0 R /Hex 9 0 R >>\nendobj\n");

	offsets[8] = b.len;
	buf_printf(&b, "8 0 obj\n<< /Length %zu /Filter /FlateDecode >>\nstream\n", sizeof FLATE_PAYLOAD);
	buf_put(&b, FLATE_PAYLOAD, sizeof FLATE_PAYLOAD);
	buf_printf(&b, "\nendstream\nendobj\n");

	offsets[9] = b.len;
	buf_printf(&b, "9 0 obj\n<< /Length %zu /Filter /ASCIIHexDecode >>\nstream\n%s\nendstream\nendobj\n",
		   strlen(hex), hex);

	xref = b.len;
	buf_printf(&b, "xref\n0 10\n");
	buf_printf(&b, "0000000000 65535 f \n");
	for (i = 1; i < 10; i++) {
		buf_printf(&b, "%010zu 00000 n \n", offsets[i]);
	}
	buf_printf(&b, "trailer\n<< /Size 10 /Root 1 0 R /Info 7 0 R /ID [<00112233> <44556677>] >>\n");
	buf_printf(&b, "startxref\n%zu\n%%%%EOF\n", xref);

	{
		CFDataRef data = buf_data(&b);

		free(b.bytes);
		return data;
	}
}

/* ------------------------------------------------------------------------- */
/* fixture B: cross-reference stream and an object stream                    */
/* ------------------------------------------------------------------------- */

static void put_row(buf_t *b, unsigned type, unsigned long long a, unsigned long long c)
{
	unsigned char row[7];

	row[0] = (unsigned char)type;
	row[1] = (unsigned char)(a >> 24);
	row[2] = (unsigned char)(a >> 16);
	row[3] = (unsigned char)(a >> 8);
	row[4] = (unsigned char)a;
	row[5] = (unsigned char)(c >> 8);
	row[6] = (unsigned char)c;
	buf_put(b, row, sizeof row);
}

static CFDataRef build_xref_stream(void)
{
	buf_t b = { 0 };
	size_t offsets[8] = { 0 };
	const char *objstm_body = "7 0 << /Title (objstm title) >>";
	size_t xref;
	size_t xref_data;
	int i;

	buf_printf(&b, "%%PDF-1.5\n");

	offsets[1] = b.len;
	buf_printf(&b, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

	offsets[2] = b.len;
	buf_printf(&b, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 300 300] >>\nendobj\n");

	offsets[3] = b.len;
	buf_printf(&b, "3 0 obj\n<< /Type /Page /Parent 2 0 R >>\nendobj\n");

	offsets[4] = b.len;
	buf_printf(&b, "4 0 obj\n<< /Length 3 >>\nstream\nq Q\nendstream\nendobj\n");

	offsets[5] = b.len;
	buf_printf(&b, "5 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Length %zu >>\nstream\n%s\nendstream\nendobj\n",
		   strlen(objstm_body), objstm_body);

	offsets[6] = b.len;
	buf_printf(&b, "6 0 obj\n<< /Type /XRef /Size 8 /W [1 4 2] /Index [0 8] /Root 1 0 R /Info 7 0 R "
		       "/Length 56 >>\nstream\n");
	xref_data = b.len;
	put_row(&b, 0, 0, 65535);			/* 0 free */
	for (i = 1; i <= 6; i++) {
		put_row(&b, 1, offsets[i], 0);		/* 1..6 in use at their offsets */
	}
	put_row(&b, 2, 5, 0);				/* 7 is object 0 of stream 5 */
	buf_printf(&b, "\nendstream\nendobj\n");
	(void)xref_data;

	xref = offsets[6];
	buf_printf(&b, "startxref\n%zu\n%%%%EOF\n", xref);

	{
		CFDataRef data = buf_data(&b);

		free(b.bytes);
		return data;
	}
}

/* ------------------------------------------------------------------------- */
/* the cases                                                                 */
/* ------------------------------------------------------------------------- */

static void test_classic(void)
{
	CFDataRef data = build_classic();
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
	CGPDFPageRef page1;
	CGPDFPageRef page3;
	CGPDFDictionaryRef info;
	CGPDFDictionaryRef catalog;
	CGPDFArrayRef id;
	CGPDFStringRef string;
	CGPDFStreamRef stream;
	CGPDFDataFormat format;
	CFStringRef text;
	CFDateRef date;
	CFAbsoluteTime expected;
	struct tm tm;
	int major = 0;
	int minor = 0;

	CHECK(document != NULL, "the classic-xref fixture did not open");
	if (document == NULL) {
		return;
	}
	CHECK(CGPDFDocumentGetTypeID() == CFGetTypeID(document),
	      "CGPDFDocumentGetTypeID is not the id Core Foundation assigned the registered class");
	CGPDFDocumentGetVersion(document, &major, &minor);
	CHECK(major == 1 && minor == 4, "version is %d.%d, expected 1.4", major, minor);
	CHECK(CGPDFDocumentGetNumberOfPages(document) == 3, "page count is %zu, expected 3",
	      CGPDFDocumentGetNumberOfPages(document));
	CHECK(!CGPDFDocumentIsEncrypted(document), "an unencrypted document reported encrypted");
	CHECK(CGPDFDocumentIsUnlocked(document), "an unencrypted document reported locked");
	CHECK(CGPDFDocumentGetAllowsPrinting(document) && CGPDFDocumentGetAllowsCopying(document),
	      "an unencrypted document does not allow printing and copying");

	catalog = CGPDFDocumentGetCatalog(document);
	CHECK(catalog != NULL, "the catalog is missing");
	{
		const char *type = NULL;

		CHECK(CGPDFDictionaryGetName(catalog, "Type", &type) && type != NULL && strcmp(type, "Catalog") == 0,
		      "the catalog's /Type is not Catalog");
	}

	page1 = CGPDFDocumentGetPage(document, 1);
	CHECK(page1 != NULL, "page 1 is missing");
	CHECK(CGPDFDocumentGetPage(document, 1) == page1, "the same page number answered two objects");
	CHECK(CGPDFDocumentGetPage(document, 4) == NULL, "a fourth page exists in a three-page document");
	CHECK(CGPDFDocumentGetPage(document, 0) == NULL, "page zero answered a page");
	CHECK(CGPDFPageGetPageNumber(page1) == 1, "the page number is not 1");
	CHECK(CGPDFPageGetDocument(page1) == document, "the page does not name its document");
	CHECK(CGPDFPageGetRotationAngle(page1) == 90, "the page's rotation is %d, expected 90",
	      CGPDFPageGetRotationAngle(page1));
	{
		CGRect media = CGPDFPageGetBoxRect(page1, kCGPDFMediaBox);
		CGRect crop = CGPDFPageGetBoxRect(page1, kCGPDFCropBox);

		CHECK_NEAR(media.size.width, 200, 1e-9, "page 1 media width");
		CHECK_NEAR(media.size.height, 100, 1e-9, "page 1 media height");
		CHECK_NEAR(crop.size.width, 200, 1e-9, "the inherited CropBox is not the MediaBox");
	}

	/* THE GEOMETRY: a 200x100 page turned a quarter turn is 100x200 on screen, so into a 200x100 rect with
	 * preserveAspectRatio it scales by 0.5 and is centred, 75 points from the left. The box's own corners
	 * are what is checked, because a scale-first implementation gets the same scale and a different corner. */
	{
		CGAffineTransform t = CGPDFPageGetDrawingTransform(page1, kCGPDFMediaBox,
								  CGRectMake(0, 0, 200, 100), 0, true);
		CGPoint origin = CGPointApplyAffineTransform(CGPointMake(0, 0), t);
		CGPoint corner = CGPointApplyAffineTransform(CGPointMake(200, 100), t);

		CHECK_NEAR(origin.x, 75, 1e-9, "rotated page's (0,0) x");
		CHECK_NEAR(origin.y, 100, 1e-9, "rotated page's (0,0) y");
		CHECK_NEAR(corner.x, 125, 1e-9, "rotated page's (200,100) x");
		CHECK_NEAR(corner.y, 0, 1e-9, "rotated page's (200,100) y");
	}

	page3 = CGPDFDocumentGetPage(document, 3);
	CHECK(page3 != NULL, "page 3 is missing");
	{
		CGRect crop = CGPDFPageGetBoxRect(page3, kCGPDFCropBox);
		CGRect media = CGPDFPageGetBoxRect(page3, kCGPDFMediaBox);

		CHECK_NEAR(media.size.width, 200, 1e-9, "page 3 does not inherit the MediaBox");
		CHECK_NEAR(crop.origin.x, 10, 1e-9, "page 3 crop x");
		CHECK_NEAR(crop.origin.y, 10, 1e-9, "page 3 crop y");
		CHECK_NEAR(crop.size.width, 80, 1e-9, "page 3 crop width");
		CHECK_NEAR(crop.size.height, 30, 1e-9, "page 3 crop height");
	}

	info = CGPDFDocumentGetInfo(document);
	CHECK(info != NULL, "the document has no /Info");
	if (info != NULL) {
		CHECK(CGPDFDictionaryGetString(info, "Title", &string), "the title is not a string");
		text = string != NULL ? CGPDFStringCopyTextString(string) : NULL;
		CHECK(text != NULL, "the title did not convert to text");
		if (text != NULL) {
			CHECK(CFStringCompare(text, CFSTR("Hello"), 0) == kCFCompareEqualTo,
			      "the UTF-16BE title did not read back as \"Hello\"");
			CFRelease(text);
		}
		CHECK(CGPDFDictionaryGetString(info, "CreationDate", &string), "the creation date is missing");
		date = string != NULL ? CGPDFStringCopyDate(string) : NULL;
		CHECK(date != NULL, "the PDF date did not parse");
		if (date != NULL) {
			memset(&tm, 0, sizeof tm);
			tm.tm_year = 2024 - 1900;
			tm.tm_mon = 1 - 1;
			tm.tm_mday = 2;
			tm.tm_hour = 3;
			tm.tm_min = 4;
			tm.tm_sec = 5;
			expected = (double)timegm(&tm) - 978307200.0;
			CHECK_NEAR(CFDateGetAbsoluteTime(date), expected, 1e-6, "the PDF date's absolute time");
			CFRelease(date);
		}

		CHECK(CGPDFDictionaryGetStream(info, "Flate", &stream), "the Flate stream did not resolve");
		if (stream != NULL) {
			CFDataRef bytes = CGPDFStreamCopyData(stream, &format);

			CHECK(bytes != NULL, "the Flate stream did not decode");
			CHECK(format == CGPDFDataFormatRaw, "a Flate stream reported an image format");
			if (bytes != NULL) {
				CHECK(CFDataGetLength(bytes) == 13 &&
				      memcmp(CFDataGetBytePtr(bytes), "flate-payload", 13) == 0,
				      "the Flate payload decoded to the wrong bytes");
				CFRelease(bytes);
			}
		}
		CHECK(CGPDFDictionaryGetStream(info, "Hex", &stream), "the hex stream did not resolve");
		if (stream != NULL) {
			CFDataRef bytes = CGPDFStreamCopyData(stream, &format);

			CHECK(bytes != NULL, "the ASCIIHex stream did not decode");
			if (bytes != NULL) {
				CHECK(CFDataGetLength(bytes) == 5 &&
				      memcmp(CFDataGetBytePtr(bytes), "ABCDE", 5) == 0,
				      "the ASCIIHex payload decoded to the wrong bytes");
				CFRelease(bytes);
			}
		}
	}

	id = CGPDFDocumentGetID(document);
	CHECK(id != NULL && CGPDFArrayGetCount(id) == 2, "the /ID array is not two elements");
	if (id != NULL) {
		CGPDFStringRef first = NULL;

		CHECK(CGPDFArrayGetString(id, 0, &first), "the first /ID element is not a string");
		if (first != NULL) {
			CHECK(CGPDFStringGetLength(first) == 4 &&
			      memcmp(CGPDFStringGetBytePtr(first), "\x00\x11\x22\x33", 4) == 0,
			      "the first /ID string is not the file's identifier bytes");
		}
	}

	CGPDFDocumentRelease(document);
	CGDataProviderRelease(provider);
	CFRelease(data);
}

static void test_xref_stream(void)
{
	CFDataRef data = build_xref_stream();
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
	CGPDFDictionaryRef info;
	CGPDFStringRef string;
	int major = 0;
	int minor = 0;

	CHECK(document != NULL, "the xref-stream fixture did not open");
	if (document == NULL) {
		return;
	}
	CGPDFDocumentGetVersion(document, &major, &minor);
	CHECK(major == 1 && minor == 5, "version is %d.%d, expected 1.5", major, minor);
	CHECK(CGPDFDocumentGetNumberOfPages(document) == 1, "page count is %zu, expected 1",
	      CGPDFDocumentGetNumberOfPages(document));
	{
		CGPDFPageRef page = CGPDFDocumentGetPage(document, 1);
		CGRect media = page != NULL ? CGPDFPageGetBoxRect(page, kCGPDFMediaBox) : CGRectZero;

		CHECK(page != NULL, "the xref-stream fixture's page is missing");
		CHECK_NEAR(media.size.width, 300, 1e-9, "the xref-stream page's media width");
		CHECK_NEAR(media.size.height, 300, 1e-9, "the xref-stream page's media height");
	}

	/* THE COMPRESSED OBJECT: /Info is 7, and object 7 lives inside object stream 5. */
	info = CGPDFDocumentGetInfo(document);
	CHECK(info != NULL, "the /Info object inside an object stream did not resolve");
	if (info != NULL) {
		CFStringRef text;

		CHECK(CGPDFDictionaryGetString(info, "Title", &string), "the object stream's title is missing");
		text = string != NULL ? CGPDFStringCopyTextString(string) : NULL;
		CHECK(text != NULL && CFStringCompare(text, CFSTR("objstm title"), 0) == kCFCompareEqualTo,
		      "the object stream's title did not read back");
		if (text != NULL) {
			CFRelease(text);
		}
	}

	CGPDFDocumentRelease(document);
	CGDataProviderRelease(provider);
	CFRelease(data);
}

/* ------------------------------------------------------------------------- */
/* the content-stream scanner                                                */
/* ------------------------------------------------------------------------- */

/* THE FIXTURE'S CONTENT IS `BT /F1 12 Tf 10 20 Td (Hello) Tj ET`, so the scanner's job is exactly to hand each
 * callback the operands in the order the stream pushed them. Popping is LIFO, which is what makes the Tf case
 * read the size before the name. */
struct scan_probe {
	int bt;
	int et;
	int tf;
	int td;
	int tj;
	int wrong;
	double size;
	char name[16];
	double x;
	double y;
	char text[32];
};

static void on_bt(CGPDFScannerRef scanner, void *info)
{
	struct scan_probe *probe = info;

	probe->bt++;
	if (CGPDFScannerGetOperandCount(scanner) != 0) {
		probe->wrong++;
	}
}

static void on_et(CGPDFScannerRef scanner, void *info)
{
	struct scan_probe *probe = info;

	probe->et++;
	(void)scanner;
}

static void on_tf(CGPDFScannerRef scanner, void *info)
{
	struct scan_probe *probe = info;
	const char *name = NULL;
	CGPDFReal size = 0;

	if (!CGPDFScannerPopNumber(scanner, &size) || !CGPDFScannerPopName(scanner, &name)) {
		probe->wrong++;
		return;
	}
	probe->tf++;
	probe->size = size;
	snprintf(probe->name, sizeof probe->name, "%s", name);
}

static void on_td(CGPDFScannerRef scanner, void *info)
{
	struct scan_probe *probe = info;
	CGPDFReal x = 0;
	CGPDFReal y = 0;

	if (!CGPDFScannerPopNumber(scanner, &y) || !CGPDFScannerPopNumber(scanner, &x)) {
		probe->wrong++;
		return;
	}
	probe->td++;
	probe->x = x;
	probe->y = y;
}

static void on_tj(CGPDFScannerRef scanner, void *info)
{
	struct scan_probe *probe = info;
	CGPDFStringRef string = NULL;
	CFStringRef text;

	if (!CGPDFScannerPopString(scanner, &string) || string == NULL) {
		probe->wrong++;
		return;
	}
	probe->tj++;
	text = CGPDFStringCopyTextString(string);
	if (text != NULL) {
		CFStringGetCString(text, probe->text, (CFIndex)sizeof probe->text, kCFStringEncodingUTF8);
		CFRelease(text);
	}
}

static void test_scanner(void)
{
	CFDataRef data = build_classic();
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
	CGPDFPageRef page;
	CGPDFContentStreamRef content;
	CGPDFOperatorTableRef table;
	CGPDFScannerRef scanner;
	struct scan_probe probe;
	bool scanned;

	CHECK(document != NULL, "the scanner fixture did not open");
	if (document == NULL) {
		return;
	}
	page = CGPDFDocumentGetPage(document, 1);
	CHECK(page != NULL, "the scanner fixture has no page 1");
	content = CGPDFContentStreamCreateWithPage(page);
	CHECK(content != NULL, "the page's content stream did not open");
	CHECK(CGPDFContentStreamGetStreams(content) != NULL, "the content stream answered no streams");
	table = CGPDFOperatorTableCreate();
	memset(&probe, 0, sizeof probe);
	CGPDFOperatorTableSetCallback(table, "BT", on_bt);
	CGPDFOperatorTableSetCallback(table, "ET", on_et);
	CGPDFOperatorTableSetCallback(table, "Tf", on_tf);
	CGPDFOperatorTableSetCallback(table, "Td", on_td);
	CGPDFOperatorTableSetCallback(table, "Tj", on_tj);
	scanner = CGPDFScannerCreate(content, table, &probe);
	CHECK(scanner != NULL, "the scanner did not open");
	scanned = CGPDFScannerScan(scanner);
	CHECK(scanned, "the content stream did not scan");
	CHECK(probe.wrong == 0, "a callback saw the wrong operands (%d)", probe.wrong);
	CHECK(probe.bt == 1 && probe.et == 1, "BT/ET ran %d/%d times, expected once each", probe.bt, probe.et);
	CHECK(probe.tf == 1, "Tf ran %d times", probe.tf);
	CHECK_NEAR(probe.size, 12, 1e-9, "Tf's font size");
	CHECK(strcmp(probe.name, "F1") == 0, "Tf's font name is \"%s\", expected F1", probe.name);
	CHECK(probe.td == 1, "Td ran %d times", probe.td);
	CHECK_NEAR(probe.x, 10, 1e-9, "Td's x");
	CHECK_NEAR(probe.y, 20, 1e-9, "Td's y");
	CHECK(probe.tj == 1, "Tj ran %d times", probe.tj);
	CHECK(strcmp(probe.text, "Hello") == 0, "Tj's string is \"%s\", expected Hello", probe.text);
	CHECK(CGPDFScannerGetContentStream(scanner) == content, "the scanner lost its content stream");

	CGPDFScannerRelease(scanner);
	CGPDFOperatorTableRelease(table);
	CGPDFContentStreamRelease(content);
	CGPDFDocumentRelease(document);
	CGDataProviderRelease(provider);
	CFRelease(data);
}


/* ------------------------------------------------------------------------- */
/* fixture C: a page that draws                                               */
/* ------------------------------------------------------------------------- */

/* THE CONTENT IS THREE OPERATORS' WORTH OF EACH THING THE RENDERER MUST DO: a filled rectangle in DeviceRGB,
 * a string in a simple font, and an image XObject scaled into a twenty-point square. The image is
 * UNCOMPRESSED RGB — twelve bytes of it — because the point of the fixture is the DRAWING, not the decoder the
 * other fixtures already cover. */
static CFDataRef build_drawing(void)
{
	buf_t b = { 0 };
	size_t offsets[10] = { 0 };
	const char *content =
		"q 0 0 1 rg 10 10 50 30 re f Q\n"
		"BT /F1 20 Tf 0 0 0 rg 20 60 Td (Hi) Tj ET\n"
		"q 20 0 0 20 110 10 cm /Im1 Do Q\n"
		"q 160 60 30 20 re W n /S1 sh Q\n"
		"q 1 0 0 1 10 50 cm /Fm1 Do Q\n";
	const char *form = "1 0 0 rg 0 0 10 10 re f\n";
	static const unsigned char samples[12] = {
		255, 0, 0,   0, 255, 0,
		0, 0, 255,   255, 255, 255
	};
	size_t xref;
	int i;

	buf_printf(&b, "%%PDF-1.4\n");

	offsets[1] = b.len;
	buf_printf(&b, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

	offsets[2] = b.len;
	buf_printf(&b, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 200 100] >>\nendobj\n");

	offsets[3] = b.len;
	buf_printf(&b, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
		       "/Resources << /Font << /F1 5 0 R >> /XObject << /Im1 6 0 R /Fm1 9 0 R >> "
		       "/Shading << /S1 7 0 R >> >> /Contents 4 0 R >>\nendobj\n");

	offsets[4] = b.len;
	buf_printf(&b, "4 0 obj\n<< /Length %zu >>\nstream\n", strlen(content));
	buf_put(&b, content, strlen(content));
	buf_printf(&b, "endstream\nendobj\n");

	offsets[5] = b.len;
	buf_printf(&b, "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
		       "/Encoding /WinAnsiEncoding >>\nendobj\n");

	offsets[6] = b.len;
	buf_printf(&b, "6 0 obj\n<< /Type /XObject /Subtype /Image /Width 2 /Height 2 "
		       "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Length 12 >>\nstream\n");
	buf_put(&b, samples, sizeof samples);
	buf_printf(&b, "\nendstream\nendobj\n");

	/* AN AXIAL SHADING AND ITS EXPONENTIAL FUNCTION: red at the start, blue at the end */
	offsets[7] = b.len;
	buf_printf(&b, "7 0 obj\n<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [160 70 190 70] "
		       "/Function 8 0 R /Extend [true true] >>\nendobj\n");

	offsets[8] = b.len;
	buf_printf(&b, "8 0 obj\n<< /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >>\nendobj\n");

	/* A FORM XOBJECT, which exercises the recursive content stream and its /Matrix and /BBox. */
	offsets[9] = b.len;
	buf_printf(&b, "9 0 obj\n<< /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << >> "
		       "/Length %zu >>\nstream\n", strlen(form));
	buf_put(&b, form, strlen(form));
	buf_printf(&b, "endstream\nendobj\n");

	xref = b.len;
	buf_printf(&b, "xref\n0 10\n");
	buf_printf(&b, "0000000000 65535 f \n");
	for (i = 1; i < 10; i++) {
		buf_printf(&b, "%010zu 00000 n \n", offsets[i]);
	}
	buf_printf(&b, "trailer\n<< /Size 10 /Root 1 0 R >>\n");
	buf_printf(&b, "startxref\n%zu\n%%%%EOF\n", xref);

	{
		CFDataRef data = buf_data(&b);

		free(b.bytes);
		return data;
	}
}

/* A PIXEL AT A POINT IN PDF USER SPACE. The bitmap context's default CTM is `[1 0 0 -1 0 height]`, so user y
 * counts UP from the bottom while memory rows count down from the top — which is the mapping this helper owns
 * and every assertion below is written against. */
static void pixel_at(const unsigned char *buffer, size_t width, size_t height, CGPoint point,
		     unsigned char out[4])
{
	int x = (int)point.x;
	int y = (int)(height - 1.0 - point.y);

	if (x < 0 || y < 0 || (size_t)x >= width || (size_t)y >= height) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}
	memcpy(out, buffer + ((size_t)y * width + (size_t)x) * 4, 4);
}

static void test_render(void)
{
	CFDataRef data = build_drawing();
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);
	CGPDFPageRef page;
	CGColorSpaceRef rgb;
	CGContextRef ctx;
	size_t width = 200;
	size_t height = 100;
	unsigned char *buffer = calloc(height, width * 4);
	unsigned char px[4];
	int text_ink = 0;
	size_t x;
	size_t y;

	CHECK(document != NULL, "the drawing fixture did not open");
	if (document == NULL) {
		return;
	}
	page = CGPDFDocumentGetPage(document, 1);
	CHECK(page != NULL, "the drawing fixture has no page");
	rgb = CGColorSpaceCreateDeviceRGB();
	ctx = CGBitmapContextCreate(buffer, width, height, 8, width * 4, rgb,
				    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CHECK(ctx != NULL, "the render target was refused");
	if (page == NULL || ctx == NULL) {
		return;
	}
	CGContextDrawPDFPage(ctx, page);

	/* THE RECTANGLE: opaque blue at its centre, and nothing at all where the page is empty. */
	pixel_at(buffer, width, height, CGPointMake(35, 25), px);
	CHECK(px[3] > 200 && px[0] > 200 && px[1] < 60 && px[2] < 60,
	      "the filled rectangle is B=%u G=%u R=%u A=%u, expected opaque blue", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, width, height, CGPointMake(160, 80), px);
	CHECK(px[3] < 20, "the page is painted where the fixture draws nothing (A=%u)", px[3]);

	/* THE TEXT: the fixture's `Hi` is black at 20 points on a baseline at y=60, so SOME pixel in the box above
	 * the baseline must be dark ink. A particular glyph's shape is the font's business; that ink landed is
	 * the renderer's. */
	for (y = 61; y < 78 && !text_ink; y++) {
		for (x = 20; x < 48; x++) {
			pixel_at(buffer, width, height, CGPointMake((CGFloat)x, (CGFloat)y), px);

			if (px[3] > 100 && px[0] < 100 && px[1] < 100 && px[2] < 100) {
				text_ink = 1;
				break;
			}
		}
	}
	CHECK(text_ink, "no ink was drawn where the fixture's text is");

	/* THE IMAGE: a 2x2 RGB XObject scaled to twenty points, top row red and green, bottom row blue and white. */
	pixel_at(buffer, width, height, CGPointMake(115, 25), px);
	CHECK(px[3] > 200 && px[2] > 200 && px[0] < 60 && px[1] < 60,
	      "the image's top-left sample is B=%u G=%u R=%u A=%u, expected red", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, width, height, CGPointMake(125, 25), px);
	CHECK(px[3] > 200 && px[1] > 200 && px[0] < 60 && px[2] < 60,
	      "the image's top-right sample is B=%u G=%u R=%u A=%u, expected green", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, width, height, CGPointMake(115, 15), px);
	CHECK(px[3] > 200 && px[0] > 200 && px[1] < 60 && px[2] < 60,
	      "the image's bottom-left sample is B=%u G=%u R=%u A=%u, expected blue", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, width, height, CGPointMake(125, 15), px);
	CHECK(px[3] > 200 && px[0] > 200 && px[1] > 200 && px[2] > 200,
	      "the image's bottom-right sample is B=%u G=%u R=%u A=%u, expected white", px[0], px[1], px[2], px[3]);

	/* THE SHADING, INSIDE THE CLIP THE CONTENT SET: red at its left end, blue at its right, and nothing at all
	 * one point outside the clip rectangle even though the gradient's own geometry would reach there. */
	pixel_at(buffer, width, height, CGPointMake(165, 70), px);
	CHECK(px[3] > 200 && px[2] > 150 && px[0] < 100,
	      "the shading's start is B=%u G=%u R=%u A=%u, expected red", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, width, height, CGPointMake(185, 70), px);
	CHECK(px[3] > 200 && px[0] > 150 && px[2] < 100,
	      "the shading's end is B=%u G=%u R=%u A=%u, expected blue", px[0], px[1], px[2], px[3]);
	pixel_at(buffer, width, height, CGPointMake(155, 70), px);
	CHECK(px[3] < 20, "the shading was painted outside its clip (A=%u)", px[3]);

	/* THE FORM: a red square a form XObject drew through its own /Matrix and /BBox. */
	pixel_at(buffer, width, height, CGPointMake(15, 55), px);
	CHECK(px[3] > 200 && px[2] > 200 && px[0] < 60 && px[1] < 60,
	      "the form XObject's square is B=%u G=%u R=%u A=%u, expected red", px[0], px[1], px[2], px[3]);

	if (getenv("AGORA_PDF_DUMP") != NULL) {
		FILE *f = fopen("/tmp/pdf-render.ppm", "wb");
		if (f != NULL) {
			fprintf(f, "P6\n%zu %zu\n255\n", width, height);
			for (y = 0; y < height; y++) {
				for (x = 0; x < width; x++) {
					unsigned char q[4];
					pixel_at(buffer, width, height, CGPointMake((CGFloat)x, (CGFloat)y), q);
					fputc(q[2], f); fputc(q[1], f); fputc(q[0], f);
				}
			}
			fclose(f);
		}
	}
	CGContextRelease(ctx);
	CGColorSpaceRelease(rgb);
	CGPDFDocumentRelease(document);
	CGDataProviderRelease(provider);
	CFRelease(data);
	free(buffer);
}

static void test_refusals(void)
{
	static const char not_a_pdf[] = "this is not a PDF file\n";
	CFDataRef data = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)not_a_pdf,
				      (CFIndex)strlen(not_a_pdf));
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
	CGPDFDocumentRef document = CGPDFDocumentCreateWithProvider(provider);

	CHECK(document == NULL, "a file with no PDF header opened as a document");

	CGDataProviderRelease(provider);
	CFRelease(data);
}

static void test_url(void)
{
	CFDataRef data = build_classic();
	char path[] = "/tmp/agora-pdf-XXXXXX";
	int fd = mkstemp(path);
	CFURLRef url;
	CGPDFDocumentRef document;

	CHECK(fd >= 0, "could not make a temporary file");
	if (fd < 0) {
		CFRelease(data);
		return;
	}
	CHECK(write(fd, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data)) == CFDataGetLength(data),
	      "could not write the fixture");
	close(fd);

	url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)path,
						      (CFIndex)strlen(path), false);
	document = CGPDFDocumentCreateWithURL(url);
	CHECK(document != NULL, "the document did not open from a file URL");
	if (document != NULL) {
		CHECK(CGPDFDocumentGetNumberOfPages(document) == 3, "the file-URL document has the wrong count");
		CGPDFDocumentRelease(document);
	}
	CFRelease(url);
	unlink(path);
	CFRelease(data);
}

int main(void)
{
	test_classic();
	test_xref_stream();
	test_scanner();
	test_render();
	test_refusals();
	test_url();
	if (failures != 0) {
		fprintf(stderr, "%d PDF check(s) failed\n", failures);
		return 1;
	}
	printf("pdf: all checks passed\n");
	return 0;
}
