/*
 * CGPDFArray.c — the array's doors, which are the array plus the resolver.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * EVERY DOOR HERE RESOLVES ITS ELEMENT FIRST. A `/Kids` array in a real file holds `3 0 R`, not a dictionary;
 * so `CGPDFArrayGetDictionary` is the object alone only when the file wrote it inline, which is rare. The
 * resolver lives on the object's document, which is why the internal object carries its document pointer.
 *
 * THE TYPED DOORS DO NOT COERCE. `CGPDFArrayGetInteger` on a real answers false rather than truncating, because
 * a caller that asked for the integer spelling is asking a question about the file; `CGPDFArrayGetNumber` is the
 * door for "either spelling, as a double", which is what the rectangle and transformation readers use.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFArray.h>

#include <string.h>

static cg_pdf_obj *fn_element(CGPDFArrayRef array, size_t index)
{
	cg_pdf_obj *a = (cg_pdf_obj *)array;

	if (a == NULL || a->type != kCGPDFObjectTypeArray || index >= a->u.array.count) {
		return NULL;
	}
	return cg_pdf_resolve(a->doc, a->u.array.items[index]);
}

size_t CGPDFArrayGetCount(CGPDFArrayRef array)
{
	cg_pdf_obj *a = (cg_pdf_obj *)array;

	return (a != NULL && a->type == kCGPDFObjectTypeArray) ? a->u.array.count : 0;
}

bool CGPDFArrayGetObject(CGPDFArrayRef array, size_t index, CGPDFObjectRef *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFObjectRef)element;
	}
	return true;
}

bool CGPDFArrayGetNull(CGPDFArrayRef array, size_t index)
{
	cg_pdf_obj *element = fn_element(array, index);

	return element != NULL && element->type == kCGPDFObjectTypeNull;
}

bool CGPDFArrayGetBoolean(CGPDFArrayRef array, size_t index, CGPDFBoolean *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeBoolean) {
		return false;
	}
	if (value != NULL) {
		*value = element->u.boolean ? 1 : 0;
	}
	return true;
}

bool CGPDFArrayGetInteger(CGPDFArrayRef array, size_t index, CGPDFInteger *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeInteger) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFInteger)element->u.integer;
	}
	return true;
}

bool CGPDFArrayGetNumber(CGPDFArrayRef array, size_t index, CGPDFReal *value)
{
	cg_pdf_obj *element = fn_element(array, index);
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

bool CGPDFArrayGetName(CGPDFArrayRef array, size_t index, const char **value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeName) {
		return false;
	}
	if (value != NULL) {
		*value = (const char *)element->u.name.bytes;
	}
	return true;
}

bool CGPDFArrayGetString(CGPDFArrayRef array, size_t index, CGPDFStringRef *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeString) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFStringRef)element;
	}
	return true;
}

bool CGPDFArrayGetArray(CGPDFArrayRef array, size_t index, CGPDFArrayRef *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeArray) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFArrayRef)element;
	}
	return true;
}

bool CGPDFArrayGetDictionary(CGPDFArrayRef array, size_t index, CGPDFDictionaryRef *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeDictionary) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFDictionaryRef)element;
	}
	return true;
}

bool CGPDFArrayGetStream(CGPDFArrayRef array, size_t index, CGPDFStreamRef *value)
{
	cg_pdf_obj *element = fn_element(array, index);

	if (element == NULL || element->type != kCGPDFObjectTypeStream) {
		return false;
	}
	if (value != NULL) {
		*value = (CGPDFStreamRef)element;
	}
	return true;
}

void CGPDFArrayApply(CGPDFArrayRef array, CGPDFArrayApplierFunction applier, void *info)
{
	cg_pdf_obj *a = (cg_pdf_obj *)array;
	size_t i;

	if (a == NULL || a->type != kCGPDFObjectTypeArray || applier == NULL) {
		return;
	}
	for (i = 0; i < a->u.array.count; i++) {
		cg_pdf_obj *element = cg_pdf_resolve(a->doc, a->u.array.items[i]);

		if (element != NULL) {
			applier(i, (CGPDFObjectRef)element, info);
		}
	}
}
