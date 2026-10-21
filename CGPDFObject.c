/*
 * CGPDFObject.c — the lexer, the parser, the object model and the type door.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **A PDF IS NOT A SERIALISATION FORMAT WITH A SCHEMA; IT IS A SMALL POSTSCRIPT WITH A DIRECTORY.** So this
 * file is a tokeniser and a recursive-descent parser, and the whole grammar fits below: seven delimiters, four
 * self-delimiting literals (`true`, `false`, `null` and a keyword), numbers, names, strings, arrays and
 * dictionaries. There is no schema, no version-specific object shape, and nothing here knows what a page is —
 * `CGPDFDocument.c` is what gives the structure meaning.
 *
 * THE PARSER IS DELIBERATELY LENIENT IN ONE DIRECTION AND STRICT IN THE OTHER. A stream whose `/Length` is wrong
 * or indirect is recovered by scanning for `endstream`, because damaged lengths are common in the wild and the
 * content is recoverable; but an object whose TYPE is wrong — a `/Kids` that is not an array, a reference whose
 * generation does not match — is not guessed at, because guessing there invents structure.
 *
 * `#`-ESCAPED NAMES ARE DECODED HERE, ONCE. `/A#20B` is the name "A B"; the bytes are what every door answers,
 * and the NUL terminator is added because Apple's `const char *` names are C strings.
 *
 * THE NUMBER PARSER ACCEPTS THE SPELLINGS A FILE ACTUALLY CONTAINS, including `+1`, `.5` and `5.`, which
 * `strtod` also takes; what it does not accept is a number with a stray character in it, because that is a
 * broken file and not an invitation.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFArray.h>
#include <CoreGraphics/CGPDFDictionary.h>
#include <CoreGraphics/CGPDFObject.h>
#include <CoreGraphics/CGPDFStream.h>
#include <CoreGraphics/CGPDFString.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* the arena                                                                 */
/* ------------------------------------------------------------------------- */

void *cg_pdf_alloc(cg_pdf_doc *doc, size_t size)
{
	void *p;

	if (size == 0) {
		size = 1;
	}
	p = calloc(1, size);
	if (p == NULL) {
		return NULL;
	}
	if (doc->alloc_count == doc->alloc_cap) {
		size_t cap = doc->alloc_cap ? doc->alloc_cap * 2 : 64;
		void **grown = realloc(doc->allocs, cap * sizeof *grown);

		if (grown == NULL) {
			free(p);
			return NULL;
		}
		doc->allocs = grown;
		doc->alloc_cap = cap;
	}
	doc->allocs[doc->alloc_count++] = p;
	return p;
}

cg_pdf_obj *cg_pdf_obj_new(cg_pdf_doc *doc, int type)
{
	cg_pdf_obj *obj = cg_pdf_alloc(doc, sizeof *obj);

	if (obj != NULL) {
		obj->doc = doc;
		obj->type = type;
	}
	return obj;
}

void cg_pdf_doc_free(cg_pdf_doc *doc)
{
	size_t i;

	if (doc == NULL) {
		return;
	}
	for (i = 0; i < doc->alloc_count; i++) {
		free(doc->allocs[i]);
	}
	free(doc->allocs);
	free(doc->xref);
	free(doc->cache);
	free(doc->compressed);
	free(doc->pages);
	if (doc->data != NULL) {
		CFRelease(doc->data);
	}
	free(doc);
}

/* ------------------------------------------------------------------------- */
/* the lexer                                                                 */
/* ------------------------------------------------------------------------- */

static bool fn_is_space(unsigned char c)
{
	return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static bool fn_is_delim(unsigned char c)
{
	return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
	       c == '/' || c == '%';
}

static bool fn_is_regular(unsigned char c)
{
	return !fn_is_space(c) && !fn_is_delim(c);
}

void cg_pdf_lexer_init(cg_pdf_lexer *lx, cg_pdf_doc *doc, size_t offset)
{
	lx->doc = doc;
	lx->data = doc->bytes;
	lx->size = doc->size;
	lx->pos = offset <= doc->size ? offset : doc->size;
}

void cg_pdf_lexer_skip_space(cg_pdf_lexer *lx)
{
	while (lx->pos < lx->size) {
		unsigned char c = lx->data[lx->pos];

		if (fn_is_space(c)) {
			lx->pos++;
			continue;
		}
		if (c == '%') {
			while (lx->pos < lx->size && lx->data[lx->pos] != '\n' && lx->data[lx->pos] != '\r') {
				lx->pos++;
			}
			continue;
		}
		break;
	}
}

bool cg_pdf_lexer_keyword(cg_pdf_lexer *lx, const char *keyword)
{
	size_t n = strlen(keyword);

	cg_pdf_lexer_skip_space(lx);
	if (lx->pos + n > lx->size || memcmp(lx->data + lx->pos, keyword, n) != 0) {
		return false;
	}
	/* A KEYWORD ENDS AT A DELIMITER OR AT WHITESPACE. `obj` in `obj<<` is legal; `object` is not `obj`. */
	if (lx->pos + n < lx->size && fn_is_regular(lx->data[lx->pos + n])) {
		return false;
	}
	lx->pos += n;
	return true;
}

/* ------------------------------------------------------------------------- */
/* growable buffers, arena-allocated and copied on growth                     */
/* ------------------------------------------------------------------------- */

static bool fn_bytes_push(unsigned char **buf, size_t *len, size_t *cap, cg_pdf_doc *doc, unsigned char byte)
{
	if (*len == *cap) {
		size_t next = *cap ? *cap * 2 : 32;
		unsigned char *grown = cg_pdf_alloc(doc, next);

		if (grown == NULL) {
			return false;
		}
		if (*len > 0) {
			memcpy(grown, *buf, *len);
		}
		*buf = grown;
		*cap = next;
	}
	(*buf)[(*len)++] = byte;
	return true;
}

static bool fn_obj_push(cg_pdf_obj *array, cg_pdf_obj *item)
{
	if (array->u.array.count == array->u.array.cap) {
		size_t next = array->u.array.cap ? array->u.array.cap * 2 : 8;
		cg_pdf_obj **grown = cg_pdf_alloc(array->doc, next * sizeof *grown);

		if (grown == NULL) {
			return false;
		}
		if (array->u.array.count > 0) {
			memcpy(grown, array->u.array.items, array->u.array.count * sizeof *grown);
		}
		array->u.array.items = grown;
		array->u.array.cap = next;
	}
	array->u.array.items[array->u.array.count++] = item;
	return true;
}

bool cg_pdf_dict_set(cg_pdf_obj *dict, char *key, cg_pdf_obj *value)
{
	size_t i;

	for (i = 0; i < dict->u.dict.count; i++) {
		if (strcmp(dict->u.dict.entries[i].key, key) == 0) {
			/* A DUPLICATE KEY TAKES THE LAST VALUE, which is what every reader in practice does and what
			 * a file with a corrected dictionary means. */
			dict->u.dict.entries[i].value = value;
			return true;
		}
	}
	if (dict->u.dict.count == dict->u.dict.cap) {
		size_t next = dict->u.dict.cap ? dict->u.dict.cap * 2 : 8;
		cg_pdf_dict_entry *grown = cg_pdf_alloc(dict->doc, next * sizeof *grown);

		if (grown == NULL) {
			return false;
		}
		if (dict->u.dict.count > 0) {
			memcpy(grown, dict->u.dict.entries, dict->u.dict.count * sizeof *grown);
		}
		dict->u.dict.entries = grown;
		dict->u.dict.cap = next;
	}
	dict->u.dict.entries[dict->u.dict.count].key = key;
	dict->u.dict.entries[dict->u.dict.count].value = value;
	dict->u.dict.count++;
	return true;
}

/* ------------------------------------------------------------------------- */
/* literals                                                                  */
/* ------------------------------------------------------------------------- */

static int fn_hex_digit(unsigned char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/* THE CALLER HAS ALREADY CHECKED FOR `/`. Decoding happens here so that no other file has to know `#` exists. */
static char *fn_parse_name(cg_pdf_lexer *lx)
{
	size_t start = lx->pos + 1;
	size_t i = start;
	size_t len = 0;
	char *name;

	if (lx->pos < lx->size) {
		lx->pos++;
	}
	name = cg_pdf_alloc(lx->doc, lx->size - start + 1);
	if (name == NULL) {
		return NULL;
	}
	while (i < lx->size && fn_is_regular(lx->data[i])) {
		if (lx->data[i] == '#' && i + 2 < lx->size) {
			int hi = fn_hex_digit(lx->data[i + 1]);
			int lo = fn_hex_digit(lx->data[i + 2]);

			if (hi >= 0 && lo >= 0) {
				name[len++] = (char)((hi << 4) | lo);
				i += 3;
				continue;
			}
		}
		name[len++] = (char)lx->data[i++];
	}
	name[len] = '\0';
	lx->pos = i;
	return name;
}

static cg_pdf_obj *fn_parse_literal_string(cg_pdf_lexer *lx)
{
	cg_pdf_obj *obj = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeString);
	unsigned char *buf = NULL;
	size_t len = 0;
	size_t cap = 0;
	int depth = 1;

	if (obj == NULL) {
		return NULL;
	}
	lx->pos++;	/* the opening `(` */
	while (lx->pos < lx->size) {
		unsigned char c = lx->data[lx->pos++];

		if (c == '\\') {
			unsigned char e;
			int octal = 0;
			int digits = 0;

			if (lx->pos >= lx->size) {
				break;
			}
			e = lx->data[lx->pos++];
			switch (e) {
			case 'n': e = '\n'; break;
			case 'r': e = '\r'; break;
			case 't': e = '\t'; break;
			case 'b': e = '\b'; break;
			case 'f': e = '\f'; break;
			case '(': case ')': case '\\': break;
			case '\r':
				/* A BACKSLASH BEFORE A LINE END IS A CONTINUATION, and CRLF counts as one end. */
				if (lx->pos < lx->size && lx->data[lx->pos] == '\n') {
					lx->pos++;
				}
				continue;
			case '\n':
				continue;
			default:
				if (e >= '0' && e <= '7') {
					octal = e - '0';
					digits = 1;
					while (digits < 3 && lx->pos < lx->size && lx->data[lx->pos] >= '0' &&
					       lx->data[lx->pos] <= '7') {
						octal = octal * 8 + (lx->data[lx->pos] - '0');
						lx->pos++;
						digits++;
					}
					e = (unsigned char)octal;
				}
				/* ANY OTHER ESCAPED CHARACTER IS ITSELF, which the specification says and files rely on. */
				break;
			}
			if (!fn_bytes_push(&buf, &len, &cap, lx->doc, e)) {
				return NULL;
			}
			continue;
		}
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (--depth == 0) {
				break;
			}
		}
		if (!fn_bytes_push(&buf, &len, &cap, lx->doc, c)) {
			return NULL;
		}
	}
	if (buf == NULL) {
		buf = cg_pdf_alloc(lx->doc, 1);
		if (buf == NULL) {
			return NULL;
		}
	}
	obj->u.string.bytes = buf;
	obj->u.string.len = len;
	return obj;
}

static cg_pdf_obj *fn_parse_hex_string(cg_pdf_lexer *lx)
{
	cg_pdf_obj *obj = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeString);
	unsigned char *buf = NULL;
	size_t len = 0;
	size_t cap = 0;
	int high = -1;

	if (obj == NULL) {
		return NULL;
	}
	lx->pos++;	/* the opening `<` */
	while (lx->pos < lx->size) {
		unsigned char c = lx->data[lx->pos++];
		int digit;

		if (c == '>') {
			break;
		}
		digit = fn_hex_digit(c);
		if (digit < 0) {
			continue;	/* whitespace, and anything else a lenient file contains */
		}
		if (high < 0) {
			high = digit;
		} else {
			if (!fn_bytes_push(&buf, &len, &cap, lx->doc, (unsigned char)((high << 4) | digit))) {
				return NULL;
			}
			high = -1;
		}
	}
	if (high >= 0) {
		if (!fn_bytes_push(&buf, &len, &cap, lx->doc, (unsigned char)(high << 4))) {
			return NULL;
		}
	}
	if (buf == NULL) {
		buf = cg_pdf_alloc(lx->doc, 1);
		if (buf == NULL) {
			return NULL;
		}
	}
	obj->u.string.bytes = buf;
	obj->u.string.len = len;
	return obj;
}

/* ONE TOKEN OF NUMBER CHARACTERS. The caller restores the position if the token turns out to be an integer
 * that begins an indirect reference. */
static bool fn_scan_number(cg_pdf_lexer *lx, long long *integer, double *real, bool *is_integer)
{
	size_t start = lx->pos;
	char token[64];
	size_t n = 0;
	bool seen_dot = false;
	bool seen_exponent = false;

	while (lx->pos < lx->size && fn_is_regular(lx->data[lx->pos])) {
		unsigned char c = lx->data[lx->pos];

		if (n + 1 >= sizeof token) {
			return false;
		}
		if (c == '.') {
			if (seen_dot) {
				return false;
			}
			seen_dot = true;
		} else if (c == 'e' || c == 'E') {
			seen_exponent = true;
		} else if (!(c >= '0' && c <= '9') && c != '+' && c != '-') {
			return false;
		}
		token[n++] = (char)c;
		lx->pos++;
	}
	if (n == 0) {
		return false;
	}
	token[n] = '\0';
	if (!seen_dot && !seen_exponent) {
		char *end = NULL;

		*integer = strtoll(token, &end, 10);
		if (end != NULL && *end == '\0') {
			*is_integer = true;
			*real = (double)*integer;
			return true;
		}
	}
	{
		char *end = NULL;

		*real = strtod(token, &end);
		if (end == NULL || *end != '\0') {
			lx->pos = start;
			return false;
		}
	}
	*is_integer = false;
	*integer = (long long)*real;
	return true;
}

static cg_pdf_obj *fn_parse_number(cg_pdf_lexer *lx)
{
	long long integer = 0;
	double real = 0;
	bool is_integer = false;
	size_t after_first;
	cg_pdf_obj *obj;

	if (!fn_scan_number(lx, &integer, &real, &is_integer)) {
		return NULL;
	}
	if (is_integer) {
		/* `n g R` IS THREE TOKENS AND THIS IS THE ONLY PLACE IT CAN BE SEEN, because the second integer
		 * and the R are not an object on their own. The position is restored when they are absent. */
		after_first = lx->pos;
		{
			long long gen = 0;
			double gen_real = 0;
			bool gen_is_integer = false;
			size_t gen_pos;

			cg_pdf_lexer_skip_space(lx);
			gen_pos = lx->pos;
			if (fn_scan_number(lx, &gen, &gen_real, &gen_is_integer) && gen_is_integer) {
				size_t after_second = lx->pos;

				cg_pdf_lexer_skip_space(lx);
				if (lx->pos < lx->size && lx->data[lx->pos] == 'R' &&
				    (lx->pos + 1 >= lx->size || !fn_is_regular(lx->data[lx->pos + 1]))) {
					lx->pos++;
					obj = cg_pdf_obj_new(lx->doc, CG_PDF_OBJ_REFERENCE);
					if (obj != NULL) {
						obj->u.ref.num = (unsigned)integer;
						obj->u.ref.gen = (unsigned)gen;
					}
					return obj;
				}
				lx->pos = after_second;
			} else {
				lx->pos = gen_pos;
			}
		}
		lx->pos = after_first;
	}
	obj = cg_pdf_obj_new(lx->doc, is_integer ? kCGPDFObjectTypeInteger : kCGPDFObjectTypeReal);
	if (obj == NULL) {
		return NULL;
	}
	if (is_integer) {
		obj->u.integer = integer;
	} else {
		obj->u.real = real;
	}
	return obj;
}

/* ------------------------------------------------------------------------- */
/* containers                                                                */
/* ------------------------------------------------------------------------- */

static cg_pdf_obj *fn_parse_array(cg_pdf_lexer *lx, int depth)
{
	cg_pdf_obj *array = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeArray);

	if (array == NULL) {
		return NULL;
	}
	lx->pos++;	/* `[` */
	for (;;) {
		cg_pdf_obj *item;

		cg_pdf_lexer_skip_space(lx);
		if (lx->pos >= lx->size) {
			return NULL;
		}
		if (lx->data[lx->pos] == ']') {
			lx->pos++;
			return array;
		}
		item = cg_pdf_parse_object(lx, depth + 1);
		if (item == NULL || !fn_obj_push(array, item)) {
			return NULL;
		}
	}
}

static cg_pdf_obj *fn_parse_dictionary(cg_pdf_lexer *lx, int depth)
{
	cg_pdf_obj *dict = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeDictionary);

	if (dict == NULL) {
		return NULL;
	}
	lx->pos += 2;	/* `<<` */
	for (;;) {
		char *key;
		cg_pdf_obj *value;

		cg_pdf_lexer_skip_space(lx);
		if (lx->pos >= lx->size) {
			return NULL;
		}
		if (lx->data[lx->pos] == '>' && lx->pos + 1 < lx->size && lx->data[lx->pos + 1] == '>') {
			lx->pos += 2;
			return dict;
		}
		if (lx->data[lx->pos] != '/') {
			return NULL;
		}
		key = fn_parse_name(lx);
		if (key == NULL) {
			return NULL;
		}
		value = cg_pdf_parse_object(lx, depth + 1);
		if (value == NULL) {
			return NULL;
		}
		if (!cg_pdf_dict_set(dict, key, value)) {
			return NULL;
		}
	}
}

cg_pdf_obj *cg_pdf_parse_object(cg_pdf_lexer *lx, int depth)
{
	unsigned char c;

	if (depth > 64) {
		fprintf(stderr, "CG-REFUSE: a PDF object nested deeper than 64 levels — this is a loop or a "
				"damaged file, and the parser stops rather than recursing\n");
		return NULL;
	}
	cg_pdf_lexer_skip_space(lx);
	if (lx->pos >= lx->size) {
		return NULL;
	}
	c = lx->data[lx->pos];
	switch (c) {
	case '/':
		{
			cg_pdf_obj *name = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeName);
			char *bytes;

			if (name == NULL) {
				return NULL;
			}
			bytes = fn_parse_name(lx);
			if (bytes == NULL) {
				return NULL;
			}
			name->u.name.bytes = (unsigned char *)bytes;
			name->u.name.len = strlen(bytes);
			return name;
		}
	case '(':
		return fn_parse_literal_string(lx);
	case '<':
		if (lx->pos + 1 < lx->size && lx->data[lx->pos + 1] == '<') {
			return fn_parse_dictionary(lx, depth);
		}
		return fn_parse_hex_string(lx);
	case '[':
		return fn_parse_array(lx, depth);
	default:
		break;
	}
	if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
		return fn_parse_number(lx);
	}
	if (c == 't') {
		if (cg_pdf_lexer_keyword(lx, "true")) {
			cg_pdf_obj *obj = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeBoolean);

			if (obj != NULL) {
				obj->u.boolean = true;
			}
			return obj;
		}
		return NULL;
	}
	if (c == 'f') {
		if (cg_pdf_lexer_keyword(lx, "false")) {
			cg_pdf_obj *obj = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeBoolean);

			if (obj != NULL) {
				obj->u.boolean = false;
			}
			return obj;
		}
		return NULL;
	}
	if (c == 'n') {
		if (cg_pdf_lexer_keyword(lx, "null")) {
			return cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeNull);
		}
	}
	/* A KEYWORD THAT IS NOT AN OBJECT (`endobj`, `stream`, a content-stream operator) is the caller's. */
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* indirect objects                                                          */
/* ------------------------------------------------------------------------- */

static bool fn_read_int(cg_pdf_lexer *lx, long long *out)
{
	long long integer = 0;
	double real = 0;
	bool is_integer = false;

	cg_pdf_lexer_skip_space(lx);
	if (!fn_scan_number(lx, &integer, &real, &is_integer) || !is_integer) {
		return false;
	}
	*out = integer;
	return true;
}

/* THE STREAM'S BYTES, BY ITS `/Length` WHEN THAT IS HONEST AND BY SCANNING WHEN IT IS NOT. The indirect case is
 * the one that matters here: during cross-reference loading no table exists yet, so a `/Length 12 0 R` cannot be
 * resolved — which is exactly why every reader has the scan fallback. */
static void fn_read_stream(cg_pdf_lexer *lx, cg_pdf_obj *dict, cg_pdf_obj **out)
{
	size_t start;
	size_t len = 0;
	bool have_len = false;
	bool scanned = false;
	cg_pdf_obj *entry;

	if (!cg_pdf_lexer_keyword(lx, "stream")) {
		return;
	}
	if (lx->pos < lx->size && lx->data[lx->pos] == '\r') {
		lx->pos++;
	}
	if (lx->pos < lx->size && lx->data[lx->pos] == '\n') {
		lx->pos++;
	}
	start = lx->pos;

	entry = NULL;
	{
		size_t i;

		for (i = 0; i < dict->u.dict.count; i++) {
			if (strcmp(dict->u.dict.entries[i].key, "Length") == 0) {
				entry = dict->u.dict.entries[i].value;
				break;
			}
		}
	}
	if (entry != NULL && entry->type == kCGPDFObjectTypeInteger && entry->u.integer >= 0) {
		unsigned long long n = (unsigned long long)entry->u.integer;

		if (n <= lx->size - start) {
			size_t after = start + (size_t)n;
			size_t probe = after;

			while (probe < lx->size && fn_is_space(lx->data[probe])) {
				probe++;
			}
			if (probe + 9 <= lx->size && memcmp(lx->data + probe, "endstream", 9) == 0) {
				len = (size_t)n;
				have_len = true;
				lx->pos = after;
			}
		}
	}
	if (!have_len) {
		size_t i;

		for (i = start; i + 9 <= lx->size; i++) {
			if (memcmp(lx->data + i, "endstream", 9) == 0) {
				len = i - start;
				lx->pos = i;
				have_len = true;
				scanned = true;
				break;
			}
		}
	}
	if (!have_len) {
		return;
	}
	/* THE EOL BEFORE `endstream` IS NOT PART OF THE DATA when the length was recovered by scanning; a real
	 * `/Length` is exact and is not trimmed. */
	if (scanned) {
		if (len > 0 && lx->data[start + len - 1] == '\n') {
			len--;
			if (len > 0 && lx->data[start + len - 1] == '\r') {
				len--;
			}
		} else if (len > 0 && lx->data[start + len - 1] == '\r') {
			len--;
		}
	}
	cg_pdf_lexer_keyword(lx, "endstream");

	*out = cg_pdf_obj_new(lx->doc, kCGPDFObjectTypeStream);
	if (*out != NULL) {
		(*out)->u.stream.dict = dict;
		(*out)->u.stream.raw = lx->data + start;
		(*out)->u.stream.raw_len = len;
	}
}

cg_pdf_obj *cg_pdf_parse_indirect(cg_pdf_lexer *lx, unsigned *out_num)
{
	long long number = 0;
	long long generation = 0;
	cg_pdf_obj *obj;

	cg_pdf_lexer_skip_space(lx);
	if (!fn_read_int(lx, &number) || !fn_read_int(lx, &generation)) {
		return NULL;
	}
	(void)generation;
	if (!cg_pdf_lexer_keyword(lx, "obj")) {
		return NULL;
	}
	obj = cg_pdf_parse_object(lx, 0);
	if (obj == NULL) {
		return NULL;
	}
	if (obj->type == kCGPDFObjectTypeDictionary) {
		size_t save = lx->pos;

		fn_read_stream(lx, obj, &obj);
		if (obj->type != kCGPDFObjectTypeStream) {
			lx->pos = save;
		}
	}
	cg_pdf_lexer_keyword(lx, "endobj");
	if (out_num != NULL) {
		*out_num = (unsigned)number;
	}
	return obj;
}

/* ------------------------------------------------------------------------- */
/* resolution                                                                */
/* ------------------------------------------------------------------------- */

cg_pdf_obj *cg_pdf_resolve(cg_pdf_doc *doc, cg_pdf_obj *obj)
{
	int hops = 0;

	while (obj != NULL && obj->type == CG_PDF_OBJ_REFERENCE) {
		if (++hops > 32) {
			fprintf(stderr, "CG-REFUSE: a PDF reference chain longer than 32 hops — this is a cycle, "
					"and the object is answered as null\n");
			return NULL;
		}
		obj = cg_pdf_object_number(doc, obj->u.ref.num);
	}
	return obj;
}

/* ------------------------------------------------------------------------- */
/* the small readers                                                         */
/* ------------------------------------------------------------------------- */

bool cg_pdf_name_is(cg_pdf_doc *doc, cg_pdf_obj *obj, const char *name)
{
	obj = cg_pdf_resolve(doc, obj);
	return obj != NULL && obj->type == kCGPDFObjectTypeName && strcmp((const char *)obj->u.name.bytes, name) == 0;
}

bool cg_pdf_number(cg_pdf_doc *doc, cg_pdf_obj *obj, double *out)
{
	obj = cg_pdf_resolve(doc, obj);
	if (obj == NULL) {
		return false;
	}
	if (obj->type == kCGPDFObjectTypeInteger) {
		*out = (double)obj->u.integer;
		return true;
	}
	if (obj->type == kCGPDFObjectTypeReal) {
		*out = obj->u.real;
		return true;
	}
	return false;
}

bool cg_pdf_integer(cg_pdf_doc *doc, cg_pdf_obj *obj, long long *out)
{
	double d;

	if (!cg_pdf_number(doc, obj, &d)) {
		return false;
	}
	*out = (long long)d;
	return true;
}

cg_pdf_obj *cg_pdf_dict_get(cg_pdf_doc *doc, cg_pdf_obj *dict, const char *key)
{
	size_t i;

	dict = cg_pdf_resolve(doc, dict);
	if (dict == NULL || dict->type != kCGPDFObjectTypeDictionary) {
		return NULL;
	}
	for (i = 0; i < dict->u.dict.count; i++) {
		if (strcmp(dict->u.dict.entries[i].key, key) == 0) {
			return cg_pdf_resolve(doc, dict->u.dict.entries[i].value);
		}
	}
	return NULL;
}

CGRect cg_pdf_rect_from_object(cg_pdf_doc *doc, cg_pdf_obj *obj, bool *ok)
{
	CGRect rect = CGRectNull;
	double v[4];
	size_t i;
	cg_pdf_obj *array;

	*ok = false;
	obj = cg_pdf_resolve(doc, obj);
	if (obj == NULL || obj->type != kCGPDFObjectTypeArray || obj->u.array.count < 4) {
		return rect;
	}
	array = obj;
	for (i = 0; i < 4; i++) {
		if (!cg_pdf_number(doc, array->u.array.items[i], &v[i])) {
			return rect;
		}
	}
	rect.origin.x = v[0] < v[2] ? v[0] : v[2];
	rect.origin.y = v[1] < v[3] ? v[1] : v[3];
	rect.size.width = fabs(v[2] - v[0]);
	rect.size.height = fabs(v[3] - v[1]);
	*ok = true;
	return rect;
}

/* ------------------------------------------------------------------------- */
/* CGPDFObject's public doors                                                */
/* ------------------------------------------------------------------------- */

CGPDFObjectType CGPDFObjectGetType(CGPDFObjectRef object)
{
	if (object == NULL || object->type == CG_PDF_OBJ_REFERENCE) {
		return kCGPDFObjectTypeNull;
	}
	return (CGPDFObjectType)object->type;
}

bool CGPDFObjectGetValue(CGPDFObjectRef object, CGPDFObjectType type, void *value)
{
	if (object == NULL || object->type == CG_PDF_OBJ_REFERENCE || (int)type != object->type) {
		return false;
	}
	switch (type) {
	case kCGPDFObjectTypeNull:
		return true;
	case kCGPDFObjectTypeBoolean:
		if (value != NULL) {
			*(CGPDFBoolean *)value = object->u.boolean ? 1 : 0;
		}
		return true;
	case kCGPDFObjectTypeInteger:
		if (value != NULL) {
			*(CGPDFInteger *)value = (CGPDFInteger)object->u.integer;
		}
		return true;
	case kCGPDFObjectTypeReal:
		if (value != NULL) {
			*(CGPDFReal *)value = (CGPDFReal)object->u.real;
		}
		return true;
	case kCGPDFObjectTypeName:
		if (value != NULL) {
			*(char **)value = (char *)object->u.name.bytes;
		}
		return true;
	case kCGPDFObjectTypeString:
		if (value != NULL) {
			*(CGPDFStringRef *)value = (CGPDFStringRef)object;
		}
		return true;
	case kCGPDFObjectTypeArray:
		if (value != NULL) {
			*(CGPDFArrayRef *)value = (CGPDFArrayRef)object;
		}
		return true;
	case kCGPDFObjectTypeDictionary:
		if (value != NULL) {
			*(CGPDFDictionaryRef *)value = (CGPDFDictionaryRef)object;
		}
		return true;
	case kCGPDFObjectTypeStream:
		if (value != NULL) {
			*(CGPDFStreamRef *)value = (CGPDFStreamRef)object;
		}
		return true;
	}
	return false;
}
