/*
 * CGPDFOperatorTable.c — the operator table, which is a name-to-callback list and nothing else.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * THE WHOLE FILE IS A LINEAR SEARCH, and that is a measured choice rather than a shortcut: a content stream
 * names a few dozen distinct operators, the table is built once at startup, and a lookup happens once per
 * operator token. A hash table would add allocation, collision handling and a resize policy to a list that never
 * holds more than a screenful.
 *
 * A NAME IS COPIED, because callers pass string literals and stack buffers alike; a second `SetCallback` for the
 * same name REPLACES rather than appends, so a caller can override a default without reading the table.
 */
#include <CoreGraphics/CGPDFOperatorTable.h>

#include <stdlib.h>
#include <string.h>

struct cg_pdf_operator {
	char *name;
	CGPDFOperatorCallback callback;
};

struct CGPDFOperatorTable {
	int refcount;
	struct cg_pdf_operator *entries;
	size_t count;
	size_t cap;
};

CGPDFOperatorTableRef CGPDFOperatorTableCreate(void)
{
	CGPDFOperatorTableRef table = calloc(1, sizeof *table);

	if (table != NULL) {
		table->refcount = 1;
	}
	return table;
}

CGPDFOperatorTableRef CGPDFOperatorTableRetain(CGPDFOperatorTableRef table)
{
	if (table != NULL) {
		table->refcount++;
	}
	return table;
}

void CGPDFOperatorTableRelease(CGPDFOperatorTableRef table)
{
	size_t i;

	if (table == NULL) {
		return;
	}
	if (--table->refcount > 0) {
		return;
	}
	for (i = 0; i < table->count; i++) {
		free(table->entries[i].name);
	}
	free(table->entries);
	free(table);
}

void CGPDFOperatorTableSetCallback(CGPDFOperatorTableRef table, const char *name, CGPDFOperatorCallback callback)
{
	size_t i;

	if (table == NULL || name == NULL) {
		return;
	}
	for (i = 0; i < table->count; i++) {
		if (strcmp(table->entries[i].name, name) == 0) {
			table->entries[i].callback = callback;
			return;
		}
	}
	if (callback == NULL) {
		return;
	}
	if (table->count == table->cap) {
		size_t cap = table->cap ? table->cap * 2 : 16;
		struct cg_pdf_operator *grown = realloc(table->entries, cap * sizeof *grown);

		if (grown == NULL) {
			return;
		}
		table->entries = grown;
		table->cap = cap;
	}
	table->entries[table->count].name = strdup(name);
	if (table->entries[table->count].name == NULL) {
		return;
	}
	table->entries[table->count].callback = callback;
	table->count++;
}

CGPDFOperatorCallback cg_pdf_operator_table_lookup(CGPDFOperatorTableRef table, const char *name)
{
	size_t i;

	if (table == NULL || name == NULL) {
		return NULL;
	}
	for (i = 0; i < table->count; i++) {
		if (strcmp(table->entries[i].name, name) == 0) {
			return table->entries[i].callback;
		}
	}
	return NULL;
}
