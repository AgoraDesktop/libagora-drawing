/*
 * CGFontNames — the name doors, where a name is a Core Foundation string.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * THE SPLIT IS THE COLOUR SPACE'S, FOR THE SAME REASON: `CGFont.c` is C and does not know what an
 * `NSString` is (it hands out C strings, see CGFont_internal.h), and the doors whose Apple signatures
 * return a string are answered here. Whoever changes one half must read the other.
 *
 * THE STRINGS ARE `CFStringRef`, which is Apple's own signature — Agora's Core Graphics depends on Core
 * Foundation, so the Fiwix tree's `NSString *` is gone and this file is C (PROVENANCE.md).
 *
 * EVERY STRING IS CONVERTED IN ONE DIRECTION ONLY: FreeType hands out UTF-8 or the name table's UTF-16BE
 * (already converted by the C half), and this file's only job is `CFStringCreateWithCString` with `+1`
 * ownership, because Apple's Copy doors return a retained string. A name that is not there is NULL, not
 * an empty string: the difference between "no such name" and "a name of zero length" is the difference
 * between a caller testing for NULL and a caller testing for length.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFont_internal.h>

/* ONE BUFFER SIZE FOR EVERY NAME, and it is generous on purpose: a glyph name in a real font is a
 * handful of bytes, a full name can be long, and the C half REFUSES rather than truncates — it answers 0
 * when the name does not fit, so a too-small buffer is a missing name rather than a wrong one. 1024 is
 * over a full name by an order of magnitude. (It is also the size a CFStringRef is read into.) */
#define FN_FONT_NAME_MAX 1024

CGFontRef CGFontCreateWithFontName(CFStringRef name)
{
	char buf[FN_FONT_NAME_MAX];

	if (name == NULL ||
	    !CFStringGetCString(name, buf, (CFIndex)sizeof(buf), kCFStringEncodingUTF8)) {
		return NULL;
	}
	return cg_font_create_with_name(buf);
}

CFStringRef CGFontCopyPostScriptName(CGFontRef font)
{
	const char *name = cg_font_postscript_name(font);

	if (name == NULL) {
		return NULL;
	}
	return CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
}

CFStringRef CGFontCopyFullName(CGFontRef font)
{
	char buf[FN_FONT_NAME_MAX];

	if (cg_font_full_name(font, buf, sizeof(buf)) <= 0) {
		return NULL;
	}
	return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}

CFStringRef CGFontCopyGlyphNameForGlyph(CGFontRef font, CGGlyph glyph)
{
	char buf[FN_FONT_NAME_MAX];

	if (cg_font_glyph_name(font, glyph, buf, sizeof(buf)) <= 0) {
		return NULL;
	}
	return CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
}

CGGlyph CGFontGetGlyphWithGlyphName(CGFontRef font, CFStringRef name)
{
	char buf[FN_FONT_NAME_MAX];

	if (name == NULL ||
	    !CFStringGetCString(name, buf, (CFIndex)sizeof(buf), kCFStringEncodingUTF8)) {
		return (CGGlyph)kCGFontIndexInvalid;
	}
	return cg_font_glyph_with_name(font, buf);
}

/* ------------------------------------------------------------------------- */
/* The variation-axis keys                                                     */
/* ------------------------------------------------------------------------- */

/* THE KEYS OF THE AXES DICTIONARY a caller passes to `CGFontCreateCopyWithVariations`: a dictionary whose
 * keys are the axes ("wght", "wdth") and whose values are themselves dictionaries keyed by these four.
 * They are `CFStringRef` — Apple's type — and their VALUES are this library's own, as the colour names'
 * are: Apple publishes the NAMES and nothing about what a caller sees if it prints one. What matters is
 * that the reader and the writer of the dictionary agree, and they do because both use these declarations. */
const CFStringRef kCGFontVariationAxisName = CFSTR("kCGFontVariationAxisName");
const CFStringRef kCGFontVariationAxisMinValue = CFSTR("kCGFontVariationAxisMinValue");
const CFStringRef kCGFontVariationAxisMaxValue = CFSTR("kCGFontVariationAxisMaxValue");
const CFStringRef kCGFontVariationAxisDefaultValue = CFSTR("kCGFontVariationAxisDefaultValue");
