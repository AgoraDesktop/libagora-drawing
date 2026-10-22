/*
 * CGPDFDictionary.c — the dictionary's doors.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * A DICTIONARY IS A SMALL LINEAR TABLE, AND THAT IS THE RIGHT STRUCTURE HERE: PDF dictionaries are a handful of
 * keys, they are built once, and they are read a handful of times while a page is walked. A hash would be more
 * machinery than the data deserves, and the file's order is the only order `Apply` could preserve anyway.
 *
 * LOOKUP IS BY THE DECODED NAME, so `"MediaBox"` finds `/MediaBox`, and `"A B"` finds `/A#20B`. Values are
 * resolved before they are handed out, exactly as array elements are.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFDictionary.h>

#include <string.h>

static cg_pdf_obj *fn_lookup(CGPDFDictionaryRef dict, const char *key)
{
	cg_pdf_obj *d = (cg_pdf_obj *)dict;
	size_t i;

	if (d == NULL || d->type != kCGPDFObjectTypeDictionary || key == NULL) {
		return NULL;
	}
	for (i = 0; i < d->u.dict.count; i++) {
		if (strcmp(d->u.dict.entries[i].key, key) == 0) {
			return cg_pdf_resolve(d->doc, d->u.dict.entries[i].value);
		}
	}
	return NULL;
}

size_t CGPDFDictionaryGetCount(CGPDFDictionaryRef dict)
{
	cg_pdf_obj *d = (cg_pdf_obj *)dict;

	return (d != NULL && d->type == kCGPDFObjectTypeDictionary) ? d->u.dict.count : 0;
}

bool CGPDFDictionaryGetObject(CGPDFDictionaryRef dict, const char *key, CGPDFObjectRef *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFObjectRef)element;
	}
	return true;
}

bool CGPDFDictionaryGetBoolean(CGPDFDictionaryRef dict, const char *key, CGPDFBoolean *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeBoolean) {
		return false;
	}
	if (value != NULL) {
		*value = element->u.boolean ? 1 : 0;
	}
	return true;
}

bool CGPDFDictionaryGetInteger(CGPDFDictionaryRef dict, const char *key, CGPDFInteger *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeInteger) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFInteger)element->u.integer;
	}
	return true;
}

bool CGPDFDictionaryGetNumber(CGPDFDictionaryRef dict, const char *key, CGPDFReal *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);
	double number;

	if (element == NULL) {
		return false;
	}
	if (element->type == kCGPDFObjectTypeInteger) {
		number = (double)element->u.integer;
	} else if (element->type == kCGPDFObjectTypeReal) {
		number = element->u.real;
	} else {
		return false;
	}
	if (value != NULL) {
		*value = number;
	}
	return true;
}

bool CGPDFDictionaryGetName(CGPDFDictionaryRef dict, const char *key, const char **value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeName) {
		return false;
	}
	if (value != NULL) {
		*value = (const char *)element->u.name.bytes;
	}
	return true;
}

bool CGPDFDictionaryGetString(CGPDFDictionaryRef dict, const char *key, CGPDFStringRef *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeString) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFStringRef)element;
	}
	return true;
}

bool CGPDFDictionaryGetArray(CGPDFDictionaryRef dict, const char *key, CGPDFArrayRef *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeArray) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFArrayRef)element;
	}
	return true;
}

bool CGPDFDictionaryGetDictionary(CGPDFDictionaryRef dict, const char *key, CGPDFDictionaryRef *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeDictionary) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFDictionaryRef)element;
	}
	return true;
}

bool CGPDFDictionaryGetStream(CGPDFDictionaryRef dict, const char *key, CGPDFStreamRef *value)
{
	cg_pdf_obj *element = fn_lookup(dict, key);

	if (element == NULL || element->type != kCGPDFObjectTypeStream) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFStreamRef)element;
	}
	return true;
}

void CGPDFDictionaryApply(CGPDFDictionaryRef dict, CGPDFDictionaryApplierFunction applier, void *info)
{
	cg_pdf_obj *d = (cg_pdf_obj *)dict;
	size_t i;

	if (d == NULL || d->type != kCGPDFObjectTypeDictionary || applier == NULL) {
		return;
	}
	for (i = 0; i < d->u.dict.count; i++) {
		cg_pdf_obj *value = cg_pdf_resolve(d->doc, d->u.dict.entries[i].value);

		if (value != NULL) {
			applier(d->u.dict.entries[i].key, (CGPDFObjectRef)value, info);
		}
	}
}
