/*
 * CGGeometryFoundation — the geometric primitives as CORE FOUNDATION dictionaries.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * SIX DOORS AND ONE SHAPE: three `Create` forms that turn a struct into a dictionary and three `Make` forms
 * that turn it back, all six built on two helpers so that the keys and the ownership rule live in ONE place
 * each. A DOOR THAT WROTE ITS OWN KEY NAMES WOULD BE A DOOR THAT COULD DISAGREE WITH THE OTHER FIVE.
 *
 * THE DICTIONARIES ARE `CFDictionaryRef` AND THE NUMBERS ARE `CFNumberRef`, which is Apple's shape: in the
 * Fiwix tree this file was Objective-C over `NSDictionary`/`NSNumber`; Agora's Core Graphics depends on Core
 * Foundation, so it is C over the CF types (PROVENANCE.md).
 *
 * `CGFloat` IS `double` HERE (the tree is 64-bit only), which is why the numbers go in and come out as
 * doubles: a float in the middle would round a value the caller never rounded.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGGeometry.h>

/* THE KEY NAMES, IN ONE PLACE, and the reason they are these names is in CGGeometry.h: Apple publishes the
 * representation and not its keys, and every other implementation of these doors uses this spelling. */
#define CG_KEY_X CFSTR("X")
#define CG_KEY_Y CFSTR("Y")
#define CG_KEY_W CFSTR("Width")
#define CG_KEY_H CFSTR("Height")

/* THE `Create` SIDE RETURNS A REFERENCE THE CALLER OWNS: `CFDictionaryCreate` is a +1 method and `CFSTR`
 * constants are immortal, so the two helpers below release only the numbers they made. */
static CFDictionaryRef fn_dict2(CFStringRef k0, CGFloat v0, CFStringRef k1, CGFloat v1)
{
	const void *keys[2] = { k0, k1 };
	const void *values[2];
	CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberDoubleType, &v0);
	CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberDoubleType, &v1);
	CFDictionaryRef d;

	if (n0 == NULL || n1 == NULL) {
		if (n0) CFRelease(n0);
		if (n1) CFRelease(n1);
		return NULL;
	}
	values[0] = n0;
	values[1] = n1;
	d = CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
			       &kCFTypeDictionaryValueCallBacks);
	CFRelease(n0);
	CFRelease(n1);
	return d;
}

static CFDictionaryRef fn_dict4(CFStringRef k0, CGFloat v0, CFStringRef k1, CGFloat v1,
				CFStringRef k2, CGFloat v2, CFStringRef k3, CGFloat v3)
{
	const void *keys[4] = { k0, k1, k2, k3 };
	const void *values[4];
	CFNumberRef n0 = CFNumberCreate(NULL, kCFNumberDoubleType, &v0);
	CFNumberRef n1 = CFNumberCreate(NULL, kCFNumberDoubleType, &v1);
	CFNumberRef n2 = CFNumberCreate(NULL, kCFNumberDoubleType, &v2);
	CFNumberRef n3 = CFNumberCreate(NULL, kCFNumberDoubleType, &v3);
	CFDictionaryRef d;

	if (n0 == NULL || n1 == NULL || n2 == NULL || n3 == NULL) {
		if (n0) CFRelease(n0);
		if (n1) CFRelease(n1);
		if (n2) CFRelease(n2);
		if (n3) CFRelease(n3);
		return NULL;
	}
	values[0] = n0;
	values[1] = n1;
	values[2] = n2;
	values[3] = n3;
	d = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
			       &kCFTypeDictionaryValueCallBacks);
	CFRelease(n0);
	CFRelease(n1);
	CFRelease(n2);
	CFRelease(n3);
	return d;
}

/* A NUMBER, OR NOTHING: a dictionary whose value for a key is a string, an array or absent is NOT a
 * representation of a geometric primitive, and the boolean these doors return is how that is said. */
static int fn_number(CFDictionaryRef dict, CFStringRef key, CGFloat *out)
{
	const void *v = CFDictionaryGetValue(dict, key);
	double d;

	if (v == NULL || CFGetTypeID((CFTypeRef)v) != CFNumberGetTypeID()) {
		return 0;
	}
	if (!CFNumberGetValue((CFNumberRef)v, kCFNumberDoubleType, &d)) {
		return 0;
	}
	*out = (CGFloat)d;
	return 1;
}

CFDictionaryRef CGPointCreateDictionaryRepresentation(CGPoint point)
{
	return fn_dict2(CG_KEY_X, point.x, CG_KEY_Y, point.y);
}

bool CGPointMakeWithDictionaryRepresentation(CFDictionaryRef dict, CGPoint *point)
{
	CGFloat x, y;

	if (dict == NULL || point == NULL) {
		return false;
	}
	if (!fn_number(dict, CG_KEY_X, &x) || !fn_number(dict, CG_KEY_Y, &y)) {
		return false;
	}
	point->x = x;
	point->y = y;
	return true;
}

CFDictionaryRef CGSizeCreateDictionaryRepresentation(CGSize size)
{
	return fn_dict2(CG_KEY_W, size.width, CG_KEY_H, size.height);
}

bool CGSizeMakeWithDictionaryRepresentation(CFDictionaryRef dict, CGSize *size)
{
	CGFloat w, h;

	if (dict == NULL || size == NULL) {
		return false;
	}
	if (!fn_number(dict, CG_KEY_W, &w) || !fn_number(dict, CG_KEY_H, &h)) {
		return false;
	}
	size->width = w;
	size->height = h;
	return true;
}

CFDictionaryRef CGRectCreateDictionaryRepresentation(CGRect rect)
{
	return fn_dict4(CG_KEY_X, rect.origin.x, CG_KEY_Y, rect.origin.y, CG_KEY_W, rect.size.width,
			CG_KEY_H, rect.size.height);
}

bool CGRectMakeWithDictionaryRepresentation(CFDictionaryRef dict, CGRect *rect)
{
	CGFloat x, y, w, h;

	if (dict == NULL || rect == NULL) {
		return false;
	}
	if (!fn_number(dict, CG_KEY_X, &x) || !fn_number(dict, CG_KEY_Y, &y)
	    || !fn_number(dict, CG_KEY_W, &w) || !fn_number(dict, CG_KEY_H, &h)) {
		return false;
	}
	rect->origin.x = x;
	rect->origin.y = y;
	rect->size.width = w;
	rect->size.height = h;
	return true;
}
