/*
 * CGPDFScanner.c — the content stream's tokeniser and operand stack.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE LOOP IS TEN LINES AND THE HARD PARTS ARE TWO EDGE CASES.** Read a token; if it is an operand, push it;
 * if it is an operator, call its callback and clear the stack. The two edges are (1) the operand/operator
 * ambiguity of `t`, `f` and `n` — the first characters of `true`, `false`, `null` AND of the `f` fill and `n`
 * no-op operators, which is why the parser is tried first and its position is restored on failure — and (2)
 * inline images, whose data is not tokens at all.
 *
 * **AN INLINE IMAGE IS SKIPPED WHOLESALE AND SAYS SO.** `BI /W 4 /H 4 /CS /G /BPC 8 ID <binary> EI` puts bytes
 * between `ID` and `EI` that may contain anything, `EI` included. The scanner finds `EI` only where it can
 * legally be (after whitespace, with whitespace or a delimiter after it) and consumes the image without offering
 * it to a callback; a renderer that wants inline images parses that range itself. This is a documented boundary
 * of the token model, not an omission hidden in code.
 *
 * THE STACK IS DYNAMIC AND CLEARED PER OPERATOR. Content streams in the wild push a hundred coordinates for a
 * `re`-heavy path, so a fixed small stack would refuse real pages; a fixed large one would waste it. Clearing
 * after each operator is the specification's model and is also what keeps a damaged stream from carrying operands
 * into the next operator.
 *
 * A MALFORMED TOKEN STOPS THE SCAN AND ANSWERS FALSE. Stopping is the honest answer: the rest of the stream's
 * meaning is unknown once a token could not be read, and continuing would run operators on guessed operands.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFOperatorTable.h>
#include <CoreGraphics/CGPDFScanner.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CGPDFScanner {
	int refcount;
	CGPDFContentStreamRef content;
	CGPDFOperatorTableRef table;
	void *info;
	cg_pdf_obj **stack;
	size_t depth;
	size_t cap;
};

/* THE CONTENT STREAM'S DOCUMENT IS NOT PART OF ITS PUBLIC SURFACE, and the scanner needs it to build operand
 * objects. The accessor is internal to the pair. */
cg_pdf_doc *cg_pdf_content_stream_doc(struct CGPDFContentStream *content);

CGPDFScannerRef CGPDFScannerCreate(CGPDFContentStreamRef contentStream, CGPDFOperatorTableRef table, void *info)
{
	CGPDFScannerRef scanner;

	if (contentStream == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFScannerCreate needs a content stream\n");
		return NULL;
	}
	scanner = calloc(1, sizeof *scanner);
	if (scanner == NULL) {
		return NULL;
	}
	scanner->refcount = 1;
	scanner->content = CGPDFContentStreamRetain(contentStream);
	scanner->table = CGPDFOperatorTableRetain(table);
	scanner->info = info;
	return scanner;
}

CGPDFScannerRef CGPDFScannerRetain(CGPDFScannerRef scanner)
{
	if (scanner != NULL) {
		scanner->refcount++;
	}
	return scanner;
}

void CGPDFScannerRelease(CGPDFScannerRef scanner)
{
	if (scanner == NULL) {
		return;
	}
	if (--scanner->refcount > 0) {
		return;
	}
	free(scanner->stack);
	if (scanner->content != NULL) {
		CGPDFContentStreamRelease(scanner->content);
	}
	if (scanner->table != NULL) {
		CGPDFOperatorTableRelease(scanner->table);
	}
	free(scanner);
}

CGPDFContentStreamRef CGPDFScannerGetContentStream(CGPDFScannerRef scanner)
{
	return scanner != NULL ? scanner->content : NULL;
}

size_t CGPDFScannerGetOperandCount(CGPDFScannerRef scanner)
{
	return scanner != NULL ? scanner->depth : 0;
}

static void fn_clear_stack(CGPDFScannerRef scanner)
{
	scanner->depth = 0;
}

static bool fn_push(CGPDFScannerRef scanner, cg_pdf_obj *obj)
{
	if (scanner->depth == scanner->cap) {
		size_t cap = scanner->cap ? scanner->cap * 2 : 32;
		cg_pdf_obj **grown = realloc(scanner->stack, cap * sizeof *grown);

		if (grown == NULL) {
			return false;
		}
		scanner->stack = grown;
		scanner->cap = cap;
	}
	scanner->stack[scanner->depth++] = obj;
	return true;
}

static cg_pdf_obj *fn_top(CGPDFScannerRef scanner)
{
	if (scanner == NULL || scanner->depth == 0) {
		return NULL;
	}
	return scanner->stack[scanner->depth - 1];
}

static cg_pdf_obj *fn_pop(CGPDFScannerRef scanner)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj != NULL) {
		scanner->depth--;
	}
	return obj;
}

bool CGPDFScannerPopObject(CGPDFScannerRef scanner, CGPDFObjectRef *value)
{
	cg_pdf_obj *obj = fn_pop(scanner);

	if (obj == NULL) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFObjectRef)obj;
	}
	return true;
}

bool CGPDFScannerPopBoolean(CGPDFScannerRef scanner, CGPDFBoolean *value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeBoolean) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = obj->u.boolean ? 1 : 0;
	}
	return true;
}

bool CGPDFScannerPopInteger(CGPDFScannerRef scanner, CGPDFInteger *value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeInteger) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = (CGPDFInteger)obj->u.integer;
	}
	return true;
}

bool CGPDFScannerPopNumber(CGPDFScannerRef scanner, CGPDFReal *value)
{
	cg_pdf_obj *obj = fn_top(scanner);
	double number;

	if (obj == NULL) {
		return false;
	}
	if (obj->type == kCGPDFObjectTypeInteger) {
		number = (double)obj->u.integer;
	} else if (obj->type == kCGPDFObjectTypeReal) {
		number = obj->u.real;
	} else {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = number;
	}
	return true;
}

bool CGPDFScannerPopName(CGPDFScannerRef scanner, const char **value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeName) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = (const char *)obj->u.name.bytes;
	}
	return true;
}

bool CGPDFScannerPopString(CGPDFScannerRef scanner, CGPDFStringRef *value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeString) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = (CGPDFStringRef)obj;
	}
	return true;
}

bool CGPDFScannerPopArray(CGPDFScannerRef scanner, CGPDFArrayRef *value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeArray) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = (CGPDFArrayRef)obj;
	}
	return true;
}

bool CGPDFScannerPopDictionary(CGPDFScannerRef scanner, CGPDFDictionaryRef *value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeDictionary) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = (CGPDFDictionaryRef)obj;
	}
	return true;
}

bool CGPDFScannerPopStream(CGPDFScannerRef scanner, CGPDFStreamRef *value)
{
	cg_pdf_obj *obj = fn_top(scanner);

	if (obj == NULL || obj->type != kCGPDFObjectTypeStream) {
		return false;
	}
	fn_pop(scanner);
	if (value != NULL) {
		*value = (CGPDFStreamRef)obj;
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* the inline-image escape                                                   */
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

/* `ID` HAS ALREADY BEEN CONSUMED. The image data begins after a single whitespace byte and ends at an `EI`
 * that is preceded by whitespace and followed by a delimiter, whitespace or the end — the only positions a
 * well-formed one can occupy, because a byte sequence inside the data cannot be told apart from the marker
 * otherwise. */
static void fn_skip_inline_image(cg_pdf_lexer *lx)
{
	if (lx->pos < lx->size && fn_is_space(lx->data[lx->pos])) {
		lx->pos++;
	}
	while (lx->pos + 1 < lx->size) {
		if (lx->data[lx->pos] == 'E' && lx->data[lx->pos + 1] == 'I' && lx->pos > 0 &&
		    fn_is_space(lx->data[lx->pos - 1]) &&
		    (lx->pos + 2 >= lx->size || !fn_is_regular(lx->data[lx->pos + 2]))) {
			lx->pos += 2;
			return;
		}
		lx->pos++;
	}
	lx->pos = lx->size;
}

/* ------------------------------------------------------------------------- */
/* one token                                                                 */
/* ------------------------------------------------------------------------- */

/* ONE TOKEN: 1 WHEN IT WAS CONSUMED, 0 AT THE END OF THE DATA, -1 WHEN WHAT IS THERE IS NEITHER AN OPERAND NOR AN
 * OPERATOR. The third answer is what makes Scan honest -- a stray delimiter stops the stream rather than being
 * skipped as if it were whitespace. */
static int fn_step(CGPDFScannerRef scanner, cg_pdf_lexer *lx)
{
	unsigned char c;
	size_t start;
	char token[64];
	size_t n = 0;
	CGPDFOperatorCallback callback;

	cg_pdf_lexer_skip_space(lx);
	if (lx->pos >= lx->size) {
		return 0;
	}
	c = lx->data[lx->pos];
	if (c == '/' || c == '(' || c == '[' || c == '<' || c == '+' || c == '-' || c == '.' ||
	    (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
		size_t before = lx->pos;
		cg_pdf_obj *obj = cg_pdf_parse_object(lx, 0);

		if (obj != NULL) {
			if (!fn_push(scanner, obj)) {
				lx->pos = lx->size;
				return -1;
			}
			return 1;
		}
		lx->pos = before;
	}
	start = lx->pos;
	while (lx->pos < lx->size && fn_is_regular(lx->data[lx->pos])) {
		if (n + 1 < sizeof token) {
			token[n++] = (char)lx->data[lx->pos];
		}
		lx->pos++;
	}
	token[n] = '\0';
	if (lx->pos == start) {
		/* A STRAY DELIMITER — `]` or `}` with no array or dictionary behind it. */
		fprintf(stderr, "CG-REFUSE: a content stream has a stray `%c` where an operator or operand "
				"belongs\n", c);
		lx->pos++;
		return -1;
	}
	if (strcmp(token, "ID") == 0) {
		fn_skip_inline_image(lx);
		fn_clear_stack(scanner);
		return 1;
	}
	callback = cg_pdf_operator_table_lookup(scanner->table, token);
	if (callback != NULL) {
		callback(scanner, scanner->info);
	}
	fn_clear_stack(scanner);
	return 1;
}

bool CGPDFScannerScan(CGPDFScannerRef scanner)
{
	size_t index;
	size_t count;

	if (scanner == NULL || scanner->content == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFScannerScan needs a scanner with a content stream\n");
		return false;
	}
	count = cg_pdf_content_stream_count(scanner->content);
	for (index = 0; index < count; index++) {
		CFDataRef data = cg_pdf_content_stream_data(scanner->content, index);
		cg_pdf_lexer lx;

		if (data == NULL) {
			return false;
		}
		int step;

		memset(&lx, 0, sizeof lx);
		lx.doc = cg_pdf_content_stream_doc(scanner->content);
		lx.data = CFDataGetBytePtr(data);
		lx.size = (size_t)CFDataGetLength(data);
		lx.pos = 0;
		fn_clear_stack(scanner);
		while ((step = fn_step(scanner, &lx)) == 1) {
			/* each call consumed one token */
		}
		if (step < 0) {
			return false;
		}
	}
	return true;
}
