/*
 * CGPDF_internal — the shape of a parsed PDF, and the parser's own doors.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **THE OBJECT GRAPH IS AN ARENA.** Every parsed object, name, string buffer and grown array is allocated
 * through `cg_pdf_alloc` and freed once, when the document is released. There is deliberately no per-object
 * free and no reference count: PDF objects are shared freely — two pages name the same `/Resources`, a cache
 * holds the same number twice — and an ownership rule invented here would be a second bug surface beside the
 * parser's. The arena makes sharing free and destruction trivial.
 *
 * **GROWN BUFFERS ARE COPIED, NOT `realloc`ed**, which is the cost of that rule: `cg_pdf_alloc` hands back a
 * fresh block, the old one stays in the arena, and both are freed at the end. A PDF's arrays are small; the
 * one place a large buffer grows is a content stream and it is grown in doubling steps, so the waste is
 * bounded by the result.
 *
 * AN UNRESOLVED INDIRECT REFERENCE HAS A TYPE, AND IT IS ZERO — outside Apple's enum, which starts at one, so
 * no public door can ever answer it. `cg_pdf_resolve` is what turns it into the object it names, and every
 * public accessor calls that first.
 */
#ifndef CORE_GRAPHICS_CGPDF_INTERNAL_H
#define CORE_GRAPHICS_CGPDF_INTERNAL_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGPDFArray.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFObject.h>
#include <CoreGraphics/CGPDFStream.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdbool.h>
#include <stddef.h>

/* THE ONE TYPE OUTSIDE APPLE'S ENUM: `n g R`, resolved by `cg_pdf_resolve`. Never visible publicly. */
#define CG_PDF_OBJ_REFERENCE 0

typedef struct CGPDFObject cg_pdf_obj;
typedef struct cg_pdf_doc cg_pdf_doc;
typedef struct CGPDFPage *CGPDFPageRef;

typedef struct cg_pdf_dict_entry {
	char *key;		/* decoded, NUL-terminated, arena-owned */
	cg_pdf_obj *value;
} cg_pdf_dict_entry;

struct CGPDFObject {
	cg_pdf_doc *doc;		/* the document that owns it; resolution needs it */
	int type;		/* CGPDFObjectType, or CG_PDF_OBJ_REFERENCE */
	union {
		bool boolean;
		long long integer;	/* the file's integers are unbounded; 64 bits is beyond every real one */
		double real;
		struct {
			unsigned char *bytes;
			size_t len;
		} name;
		struct {
			unsigned char *bytes;
			size_t len;
		} string;
		struct {
			cg_pdf_obj **items;
			size_t count;
			size_t cap;
		} array;
		struct {
			cg_pdf_dict_entry *entries;
			size_t count;
			size_t cap;
		} dict;
		struct {
			cg_pdf_obj *dict;	/* the stream's dictionary object */
			const unsigned char *raw;	/* into the document's bytes, never owned */
			size_t raw_len;		/* the ENCODED bytes, between `stream` and `endstream` */
		} stream;
		struct {
			unsigned num;
			unsigned gen;
		} ref;
	} u;
};

/* ONE CROSS-REFERENCE ENTRY. `type` is the file's own spelling: 0 free, 1 in use at `a`, 2 inside object
 * stream `a` at index `b`. `present` distinguishes "the file says free" from "no section has said yet", which
 * is what makes newest-first merging work without overwriting a newer answer with an older one. */
typedef struct cg_pdf_xref_entry {
	unsigned char type;
	unsigned char present;
	unsigned long long a;
	unsigned long long b;
} cg_pdf_xref_entry;

/* A PAGE, FLATTENED. The boxes are the effective ones; `dict` is the page's own dictionary. */
typedef struct cg_pdf_page_info {
	cg_pdf_obj *dict;
	cg_pdf_obj *resources;		/* the EFFECTIVE /Resources, inherited or NULL */
	CGRect media;
	CGRect crop;
	CGRect bleed;
	CGRect trim;
	CGRect art;
	int rotate;
} cg_pdf_page_info;

struct cg_pdf_doc {
	CFDataRef data;			/* the whole file, retained */
	const unsigned char *bytes;
	size_t size;

	int major;
	int minor;
	bool encrypted;
	bool unlocked;			/* always false: no decryption here */
	bool damaged;

	cg_pdf_obj *trailer;		/* the merged trailer dictionary */
	cg_pdf_obj *catalog;
	cg_pdf_obj *info;

	cg_pdf_xref_entry *xref;
	size_t xref_count;
	size_t xref_cap;
	cg_pdf_obj **cache;		/* by object number; NULL means "not parsed yet" */

	/* OBJECTS OUT OF OBJECT STREAMS, keyed by their own number, because they have no file offset. */
	struct cg_pdf_compressed {
		unsigned num;
		cg_pdf_obj *obj;
	} *compressed;
	size_t compressed_count;
	size_t compressed_cap;

	cg_pdf_page_info *pages;
	size_t page_count;
	size_t page_cap;
	bool pages_built;

	void **allocs;
	size_t alloc_count;
	size_t alloc_cap;
};

typedef struct cg_pdf_lexer {
	cg_pdf_doc *doc;
	const unsigned char *data;
	size_t size;
	size_t pos;
} cg_pdf_lexer;

/* THE ARENA. Zeroed, arena-owned, NULL on failure. */
void *cg_pdf_alloc(cg_pdf_doc *doc, size_t size);
cg_pdf_obj *cg_pdf_obj_new(cg_pdf_doc *doc, int type);
bool cg_pdf_dict_set(cg_pdf_obj *dict, char *key, cg_pdf_obj *value);
void cg_pdf_doc_free(cg_pdf_doc *doc);

/* THE PARSER. `cg_pdf_parse_object` answers NULL only for a malformed object or the end of the data; the
 * keywords a container is bounded by are the caller's to consume. */
void cg_pdf_lexer_init(cg_pdf_lexer *lx, cg_pdf_doc *doc, size_t offset);
void cg_pdf_lexer_skip_space(cg_pdf_lexer *lx);
bool cg_pdf_lexer_keyword(cg_pdf_lexer *lx, const char *keyword);
cg_pdf_obj *cg_pdf_parse_object(cg_pdf_lexer *lx, int depth);

/* ONE `n g obj ... endobj` AT THE LEXER'S POSITION. `out_num` gets the object's own number, which is checked
 * against the one the caller expected by the caller. */
cg_pdf_obj *cg_pdf_parse_indirect(cg_pdf_lexer *lx, unsigned *out_num);

/* REFERENCE RESOLUTION, with a depth cap rather than a visited set: a legal chain is one hop. */
cg_pdf_obj *cg_pdf_resolve(cg_pdf_doc *doc, cg_pdf_obj *obj);
cg_pdf_obj *cg_pdf_object_number(cg_pdf_doc *doc, unsigned num);

/* A STREAM'S DECODED BYTES, +1, filters and predictors applied; NULL when a filter is not one this build
 * decodes (the refusal is printed by name). */
CFDataRef cg_pdf_stream_data(cg_pdf_doc *doc, cg_pdf_obj *stream);

/* SMALL READERS USED BY MORE THAN ONE FILE. `cg_pdf_name_is` resolves its argument. */
bool cg_pdf_name_is(cg_pdf_doc *doc, cg_pdf_obj *obj, const char *name);
bool cg_pdf_number(cg_pdf_doc *doc, cg_pdf_obj *obj, double *out);
bool cg_pdf_integer(cg_pdf_doc *doc, cg_pdf_obj *obj, long long *out);
CGRect cg_pdf_rect_from_object(cg_pdf_doc *doc, cg_pdf_obj *obj, bool *ok);

/* A RESOLVED DICTIONARY LOOKUP, for the internal readers that already hold an object rather than a public
 * `CGPDFDictionaryRef`. NULL for a miss and for a non-dictionary. */
cg_pdf_obj *cg_pdf_dict_get(cg_pdf_doc *doc, cg_pdf_obj *dict, const char *key);

/* THE PAGE OBJECT, CREATED BY THE DOCUMENT AND OWNED BY IT. `index` is zero-based here; the public page number
 * is one-based. */
struct CGPDFDocument;
CGPDFPageRef cg_pdf_page_create(struct CGPDFDocument *document, cg_pdf_doc *pdf, size_t index);
cg_pdf_doc *cg_pdf_page_doc(CGPDFPageRef page);
size_t cg_pdf_page_index(CGPDFPageRef page);

/* THE CONTENT STREAM'S OWN ACCESSORS, for the scanner: how many decoded streams it holds and each one's bytes. */
struct CGPDFContentStream;
size_t cg_pdf_content_stream_count(struct CGPDFContentStream *content);
CFDataRef cg_pdf_content_stream_data(struct CGPDFContentStream *content, size_t index);
cg_pdf_doc *cg_pdf_content_stream_doc(struct CGPDFContentStream *content);

#endif /* CORE_GRAPHICS_CGPDF_INTERNAL_H */
