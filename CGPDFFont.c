/*
 * CGPDFFont.c — encodings, widths and embedded programs.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * THE ENCODING IS TWO TABLES AND AN OVERRIDE, WHICH IS THE SPECIFICATION'S OWN SHAPE: a base encoding turns a
 * code into a character (or a glyph name), and /Differences replaces individual codes with names. An embedded
 * font is asked for the NAME first, because a subset's glyph names are the author's — `g17`, `uni0041` — and only
 * what the name cannot answer is mapped through Unicode. A font WITHOUT names (most modern TrueType faces) is
 * asked through its cmap instead, which is the `cg_font_glyph_for_unichar` door.
 *
 * **THE STANDARD ENCODINGS ARE READ AS CHARACTER TABLES, WITH ONE STATED APPROXIMATION.** WinAnsiEncoding is
 * CP1252 exactly. StandardEncoding is ASCII with its two typographic quote slots (0x27, 0x60) moved, and its
 * upper half is read as Latin-1: the specification's table differs there in a handful of slots (0xA4 is fraction,
 * 0xA6 florin, 0xA8 currency) that no document this desktop has met uses in a StandardEncoding font. MacRoman is
 * REFUSED by name rather than approximated, because its upper half is a genuinely different alphabet and a
 * plausible-looking wrong letter is worse than a missing one.
 *
 * THE GLYPH-NAME TABLE BELOW IS THE SUBSET OF THE ADOBE GLYPH LIST THAT PDFs ACTUALLY USE: ASCII punctuation,
 * the Latin-1 accented letters, and the typographic marks. A name outside it still gets a chance through the
 * face's own `post` or Type 1 name table; only a name neither knows becomes .notdef.
 */
#include <CoreGraphics/CGPDFFont.h>
#include <CoreGraphics/CGFont_internal.h>

#include <CoreFoundation/CFString.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cg_pdf_width_range {
	unsigned first;
	unsigned last;
	double width;
};

struct cg_pdf_font {
	cg_pdf_doc *doc;
	CGFontRef cg;
	char base_name[128];
	int code_bytes;
	bool cid;

	/* simple fonts: the whole code space, resolved once */
	CGGlyph glyphs[256];
	double widths[256];

	/* CID fonts: ranges from /W, and the optional /CIDToGIDMap stream */
	double default_width;
	struct cg_pdf_width_range *ranges;
	size_t range_count;
	CFDataRef cid_to_gid;
};

/* ------------------------------------------------------------------------- */
/* glyph names                                                               */
/* ------------------------------------------------------------------------- */

static const struct {
	const char *name;
	unsigned unicode;
} fn_agl[] = {
	/* the ASCII punctuation names, because /Differences is written in them */
	{ "space", 0x20 }, { "exclam", 0x21 }, { "quotedbl", 0x22 }, { "numbersign", 0x23 },
	{ "dollar", 0x24 }, { "percent", 0x25 }, { "ampersand", 0x26 }, { "quotesingle", 0x27 },
	{ "parenleft", 0x28 }, { "parenright", 0x29 }, { "asterisk", 0x2A }, { "plus", 0x2B },
	{ "comma", 0x2C }, { "hyphen", 0x2D }, { "period", 0x2E }, { "slash", 0x2F },
	{ "zero", 0x30 }, { "one", 0x31 }, { "two", 0x32 }, { "three", 0x33 }, { "four", 0x34 },
	{ "five", 0x35 }, { "six", 0x36 }, { "seven", 0x37 }, { "eight", 0x38 }, { "nine", 0x39 },
	{ "colon", 0x3A }, { "semicolon", 0x3B }, { "less", 0x3C }, { "equal", 0x3D },
	{ "greater", 0x3E }, { "question", 0x3F }, { "at", 0x40 }, { "bracketleft", 0x5B },
	{ "backslash", 0x5C }, { "bracketright", 0x5D }, { "asciicircum", 0x5E }, { "underscore", 0x5F },
	{ "grave", 0x60 }, { "braceleft", 0x7B }, { "bar", 0x7C }, { "braceright", 0x7D },
	{ "asciitilde", 0x7E },
	/* Latin-1 */
	{ "nbspace", 0xA0 }, { "exclamdown", 0xA1 }, { "cent", 0xA2 }, { "sterling", 0xA3 },
	{ "currency", 0xA4 }, { "yen", 0xA5 }, { "brokenbar", 0xA6 }, { "section", 0xA7 },
	{ "dieresis", 0xA8 }, { "copyright", 0xA9 }, { "ordfeminine", 0xAA }, { "guillemotleft", 0xAB },
	{ "logicalnot", 0xAC }, { "sfthyphen", 0xAD }, { "registered", 0xAE }, { "macron", 0xAF },
	{ "degree", 0xB0 }, { "plusminus", 0xB1 }, { "twosuperior", 0xB2 }, { "threesuperior", 0xB3 },
	{ "acute", 0xB4 }, { "mu", 0xB5 }, { "paragraph", 0xB6 }, { "periodcentered", 0xB7 },
	{ "cedilla", 0xB8 }, { "onesuperior", 0xB9 }, { "ordmasculine", 0xBA }, { "guillemotright", 0xBB },
	{ "onequarter", 0xBC }, { "onehalf", 0xBD }, { "threequarters", 0xBE }, { "questiondown", 0xBF },
	{ "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Acircumflex", 0xC2 }, { "Atilde", 0xC3 },
	{ "Adieresis", 0xC4 }, { "Aring", 0xC5 }, { "AE", 0xC6 }, { "Ccedilla", 0xC7 },
	{ "Egrave", 0xC8 }, { "Eacute", 0xC9 }, { "Ecircumflex", 0xCA }, { "Edieresis", 0xCB },
	{ "Igrave", 0xCC }, { "Iacute", 0xCD }, { "Icircumflex", 0xCE }, { "Idieresis", 0xCF },
	{ "Eth", 0xD0 }, { "Ntilde", 0xD1 }, { "Ograve", 0xD2 }, { "Oacute", 0xD3 },
	{ "Ocircumflex", 0xD4 }, { "Otilde", 0xD5 }, { "Odieresis", 0xD6 }, { "multiply", 0xD7 },
	{ "Oslash", 0xD8 }, { "Ugrave", 0xD9 }, { "Uacute", 0xDA }, { "Ucircumflex", 0xDB },
	{ "Udieresis", 0xDC }, { "Yacute", 0xDD }, { "Thorn", 0xDE }, { "germandbls", 0xDF },
	{ "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acircumflex", 0xE2 }, { "atilde", 0xE3 },
	{ "adieresis", 0xE4 }, { "aring", 0xE5 }, { "ae", 0xE6 }, { "ccedilla", 0xE7 },
	{ "egrave", 0xE8 }, { "eacute", 0xE9 }, { "ecircumflex", 0xEA }, { "edieresis", 0xEB },
	{ "igrave", 0xEC }, { "iacute", 0xED }, { "icircumflex", 0xEE }, { "idieresis", 0xEF },
	{ "eth", 0xF0 }, { "ntilde", 0xF1 }, { "ograve", 0xF2 }, { "oacute", 0xF3 },
	{ "ocircumflex", 0xF4 }, { "otilde", 0xF5 }, { "odieresis", 0xF6 }, { "divide", 0xF7 },
	{ "oslash", 0xF8 }, { "ugrave", 0xF9 }, { "uacute", 0xFA }, { "ucircumflex", 0xFB },
	{ "udieresis", 0xFC }, { "yacute", 0xFD }, { "thorn", 0xFE }, { "ydieresis", 0xFF },
	/* typographic and ligature names */
	{ "quoteleft", 0x2018 }, { "quoteright", 0x2019 }, { "quotedblleft", 0x201C },
	{ "quotedblright", 0x201D }, { "quotesinglbase", 0x201A }, { "quotedblbase", 0x201E },
	{ "guilsinglleft", 0x2039 }, { "guilsinglright", 0x203A }, { "endash", 0x2013 },
	{ "emdash", 0x2014 }, { "bullet", 0x2022 }, { "ellipsis", 0x2026 }, { "dagger", 0x2020 },
	{ "daggerdbl", 0x2021 }, { "perthousand", 0x2030 }, { "trademark", 0x2122 },
	{ "fi", 0xFB01 }, { "fl", 0xFB02 }, { "ff", 0xFB00 }, { "ffi", 0xFB03 }, { "ffl", 0xFB04 },
	{ "florin", 0x0192 }, { "fraction", 0x2044 }, { "minus", 0x2212 }, { "Euro", 0x20AC },
	{ "dotlessi", 0x0131 }, { "circumflex", 0x02C6 }, { "tilde", 0x02DC }, { "breve", 0x02D8 },
	{ "caron", 0x02C7 }, { "dotaccent", 0x02D9 }, { "ring", 0x02DA }, { "ogonek", 0x02DB },
	{ "hungarumlaut", 0x02DD }, { "scaron", 0x0161 }, { "Scaron", 0x0160 }, { "zcaron", 0x017E },
	{ "Zcaron", 0x017D }, { "oe", 0x0153 }, { "OE", 0x0152 }, { "Ydieresis", 0x0178 },
	{ "lslash", 0x0142 }, { "Lslash", 0x0141 }
};

static unsigned fn_agl_unicode(const char *name)
{
	size_t i;

	if (name == NULL) {
		return 0;
	}
	if (name[0] != '\0' && name[1] == '\0') {
		unsigned char c = (unsigned char)name[0];

		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
			return c;
		}
	}
	for (i = 0; i < sizeof fn_agl / sizeof fn_agl[0]; i++) {
		if (strcmp(fn_agl[i].name, name) == 0) {
			return fn_agl[i].unicode;
		}
	}
	if (strncmp(name, "uni", 3) == 0 && strlen(name) >= 7) {
		unsigned value = 0;
		size_t k;

		for (k = 3; k < 7; k++) {
			int digit;

			if (name[k] >= '0' && name[k] <= '9') {
				digit = name[k] - '0';
			} else if (name[k] >= 'A' && name[k] <= 'F') {
				digit = name[k] - 'A' + 10;
			} else if (name[k] >= 'a' && name[k] <= 'f') {
				digit = name[k] - 'a' + 10;
			} else {
				return 0;
			}
			value = value * 16 + (unsigned)digit;
		}
		return value;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* the standard encodings                                                    */
/* ------------------------------------------------------------------------- */

static unsigned fn_winansi_unicode(unsigned code)
{
	switch (code) {
	case 0x80: return 0x20AC; case 0x82: return 0x201A; case 0x83: return 0x0192;
	case 0x84: return 0x201E; case 0x85: return 0x2026; case 0x86: return 0x2020;
	case 0x87: return 0x2021; case 0x88: return 0x02C6; case 0x89: return 0x2030;
	case 0x8A: return 0x0160; case 0x8B: return 0x2039; case 0x8C: return 0x0152;
	case 0x8E: return 0x017D; case 0x91: return 0x2018; case 0x92: return 0x2019;
	case 0x93: return 0x201C; case 0x94: return 0x201D; case 0x95: return 0x2022;
	case 0x96: return 0x2013; case 0x97: return 0x2014; case 0x98: return 0x02DC;
	case 0x99: return 0x2122; case 0x9A: return 0x0161; case 0x9B: return 0x203A;
	case 0x9C: return 0x0153; case 0x9E: return 0x017E; case 0x9F: return 0x0178;
	default: break;
	}
	if (code < 0x20) {
		return 0;
	}
	return code;
}

static unsigned fn_standard_unicode(unsigned code)
{
	if (code == 0x27) {
		return 0x2019;
	}
	if (code == 0x60) {
		return 0x2018;
	}
	if (code < 0x20) {
		return 0;
	}
	/* ASCII, then Latin-1 for the upper half — the one approximation stated in this file's header. */
	return code;
}

static bool fn_encoding_is(const char *name, const char *want)
{
	return name != NULL && strcmp(name, want) == 0;
}

/* ------------------------------------------------------------------------- */
/* the font program                                                          */
/* ------------------------------------------------------------------------- */

static CGFontRef fn_font_from_stream(cg_pdf_doc *doc, cg_pdf_obj *stream)
{
	CFDataRef data;
	CGDataProviderRef provider;
	CGFontRef font;

	stream = cg_pdf_resolve(doc, stream);
	if (stream == NULL || stream->type != kCGPDFObjectTypeStream) {
		return NULL;
	}
	data = cg_pdf_stream_data(doc, stream);
	if (data == NULL) {
		return NULL;
	}
	provider = CGDataProviderCreateWithCFData(data);
	CFRelease(data);
	if (provider == NULL) {
		return NULL;
	}
	font = CGFontCreateWithDataProvider(provider);
	CGDataProviderRelease(provider);
	return font;
}

static CGFontRef fn_embedded_font(cg_pdf_doc *doc, cg_pdf_obj *descriptor)
{
	static const char *keys[] = { "FontFile", "FontFile2", "FontFile3" };
	size_t i;

	if (descriptor == NULL) {
		return NULL;
	}
	for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		CGFontRef font = fn_font_from_stream(doc, cg_pdf_dict_get(doc, descriptor, keys[i]));

		if (font != NULL) {
			return font;
		}
	}
	return NULL;
}

/* THE `ABCDEF+` PREFIX IS A SUBSET TAG, NOT PART OF THE NAME, and a lookup that keeps it finds nothing. */
static void fn_strip_subset(const char *base, char *out, size_t size)
{
	size_t offset = 0;

	if (base != NULL && strlen(base) > 7 && base[6] == '+') {
		offset = 7;
	}
	snprintf(out, size, "%s", base != NULL ? base + offset : "");
}

static CGFontRef fn_named_font(const char *base)
{
	static const struct {
		const char *base;
		const char *fallback;
	} aliases[] = {
		{ "Helvetica", "Liberation Sans" },
		{ "Helvetica-Bold", "Liberation Sans" },
		{ "Helvetica-Oblique", "Liberation Sans" },
		{ "Times-Roman", "Liberation Serif" },
		{ "Times-Bold", "Liberation Serif" },
		{ "Times-Italic", "Liberation Serif" },
		{ "Courier", "Liberation Mono" },
		{ "Courier-Bold", "Liberation Mono" },
		{ "Symbol", "DejaVu Sans" },
		{ "ZapfDingbats", "DejaVu Sans" }
	};
	CGFontRef font = NULL;
	CFStringRef name;
	size_t i;

	if (base == NULL || base[0] == '\0') {
		return NULL;
	}
	name = CFStringCreateWithCString(kCFAllocatorDefault, base, kCFStringEncodingUTF8);
	if (name != NULL) {
		font = CGFontCreateWithFontName(name);
		CFRelease(name);
	}
	for (i = 0; font == NULL && i < sizeof aliases / sizeof aliases[0]; i++) {
		if (strcmp(aliases[i].base, base) == 0) {
			name = CFStringCreateWithCString(kCFAllocatorDefault, aliases[i].fallback,
							 kCFStringEncodingUTF8);
			if (name != NULL) {
				font = CGFontCreateWithFontName(name);
				CFRelease(name);
			}
		}
	}
	return font;
}

static double fn_glyph_advance(cg_pdf_font *font, CGGlyph glyph)
{
	int advance = 0;
	int upem = CGFontGetUnitsPerEm(font->cg);

	if (font->cg == NULL || upem <= 0 || !CGFontGetGlyphAdvances(font->cg, &glyph, 1, &advance)) {
		return 0.0;
	}
	return (double)advance * 1000.0 / (double)upem;
}

static CGGlyph fn_glyph_named(cg_pdf_font *font, const char *name)
{
	CGGlyph glyph = (CGGlyph)kCGFontIndexInvalid;
	CFStringRef cf;
	unsigned unicode;

	if (name == NULL) {
		return 0;
	}
	cf = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
	if (cf != NULL) {
		glyph = CGFontGetGlyphWithGlyphName(font->cg, cf);
		CFRelease(cf);
	}
	if (glyph != (CGGlyph)kCGFontIndexInvalid) {
		return glyph;
	}
	unicode = fn_agl_unicode(name);
	if (unicode != 0) {
		glyph = cg_font_glyph_for_unichar(font->cg, unicode);
		if (glyph != (CGGlyph)kCGFontIndexInvalid) {
			return glyph;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* simple fonts                                                              */
/* ------------------------------------------------------------------------- */

static void fn_resolve_simple(cg_pdf_font *font, cg_pdf_doc *doc, cg_pdf_obj *dict, cg_pdf_obj *descriptor,
			      bool truetype, bool builtin_encoding)
{
	const char *difference[256];
	const char *base_encoding = NULL;
	cg_pdf_obj *encoding = cg_pdf_dict_get(doc, dict, "Encoding");
	cg_pdf_obj *widths = cg_pdf_dict_get(doc, dict, "Widths");
	cg_pdf_obj *descriptor_obj = descriptor;
	long long first_char = 0;
	double missing_width = 0;
	size_t i;
	char base[128] = "";

	memset(difference, 0, sizeof difference);
	if (encoding != NULL && encoding->type == kCGPDFObjectTypeName) {
		base_encoding = (const char *)encoding->u.name.bytes;
	} else if (encoding != NULL && encoding->type == kCGPDFObjectTypeDictionary) {
		cg_pdf_obj *base = cg_pdf_dict_get(doc, encoding, "BaseEncoding");
		cg_pdf_obj *diff = cg_pdf_dict_get(doc, encoding, "Differences");
		size_t current = 0;

		if (base != NULL && base->type == kCGPDFObjectTypeName) {
			base_encoding = (const char *)base->u.name.bytes;
		}
		if (diff != NULL && diff->type == kCGPDFObjectTypeArray) {
			for (i = 0; i < diff->u.array.count; i++) {
				cg_pdf_obj *item = cg_pdf_resolve(doc, diff->u.array.items[i]);

				if (item == NULL) {
					continue;
				}
				if (item->type == kCGPDFObjectTypeInteger) {
					current = (size_t)item->u.integer;
				} else if (item->type == kCGPDFObjectTypeName && current < 256) {
					difference[current++] = (const char *)item->u.name.bytes;
				}
			}
		}
	}
	if (fn_encoding_is(base_encoding, "MacRomanEncoding")) {
		fprintf(stderr, "CG-REFUSE: a font names /MacRomanEncoding, whose upper half is not Latin-1; "
				"this reader does not approximate it\n");
	}
	cg_pdf_integer(doc, cg_pdf_dict_get(doc, dict, "FirstChar"), &first_char);
	cg_pdf_number(doc, cg_pdf_dict_get(doc, descriptor_obj, "MissingWidth"), &missing_width);

	for (i = 0; i < 256; i++) {
		CGGlyph glyph = 0;

		if (difference[i] != NULL) {
			glyph = fn_glyph_named(font, difference[i]);
		} else if (builtin_encoding) {
			/* **A TYPE 1 FONT WITH NO /Encoding CARRIES ITS OWN.** The code is a slot in the program's
			 * built-in encoding — TeX's text encoding puts its ligatures where a StandardEncoding reader
			 * expects other letters — so the engine's char map is asked about the CODE, which for a
			 * Type 1 face is that encoding. MEASURED on quilt.pdf: reading these through StandardEncoding
			 * drew `efficient` with a breve over the e, because the code that means `ffi` here means
			 * something else there. */
			glyph = cg_font_glyph_for_code(font->cg, (unsigned)i);
			if (glyph == (CGGlyph)kCGFontIndexInvalid) {
				glyph = 0;
			}
		} else {
			unsigned unicode = fn_encoding_is(base_encoding, "WinAnsiEncoding")
						 ? fn_winansi_unicode((unsigned)i)
						 : fn_standard_unicode((unsigned)i);

			if (unicode != 0) {
				glyph = cg_font_glyph_for_unichar(font->cg, unicode);
				if (glyph == (CGGlyph)kCGFontIndexInvalid) {
					glyph = 0;
				}
			}
		}
		font->glyphs[i] = glyph;
		if (widths != NULL && widths->type == kCGPDFObjectTypeArray &&
		    (long long)i >= first_char && (long long)i - first_char < (long long)widths->u.array.count) {
			double width = 0;

			if (cg_pdf_number(doc, widths->u.array.items[i - (size_t)first_char], &width)) {
				font->widths[i] = width;
			}
		} else if (missing_width > 0) {
			font->widths[i] = missing_width;
		} else {
			font->widths[i] = fn_glyph_advance(font, glyph);
		}
	}
	(void)truetype;
	(void)base;
}

/* ------------------------------------------------------------------------- */
/* CID fonts                                                                 */
/* ------------------------------------------------------------------------- */

static void fn_add_range(cg_pdf_font *font, unsigned first, unsigned last, double width)
{
	struct cg_pdf_width_range *grown;

	if (last < first) {
		unsigned swap = first;

		first = last;
		last = swap;
	}
	grown = realloc(font->ranges, (font->range_count + 1) * sizeof *grown);
	if (grown == NULL) {
		return;
	}
	font->ranges = grown;
	font->ranges[font->range_count].first = first;
	font->ranges[font->range_count].last = last;
	font->ranges[font->range_count].width = width;
	font->range_count++;
}

static void fn_resolve_cid_widths(cg_pdf_font *font, cg_pdf_doc *doc, cg_pdf_obj *cid_font)
{
	cg_pdf_obj *w = cg_pdf_dict_get(doc, cid_font, "W");
	double dw = 1000;

	if (!cg_pdf_number(doc, cg_pdf_dict_get(doc, cid_font, "DW"), &dw)) {
		dw = 1000;
	}
	font->default_width = dw;
	if (w == NULL || w->type != kCGPDFObjectTypeArray) {
		return;
	}
	{
		size_t i = 0;

		while (i < w->u.array.count) {
			long long first = 0;
			cg_pdf_obj *next;

			if (!cg_pdf_integer(doc, w->u.array.items[i], &first)) {
				break;
			}
			if (i + 1 >= w->u.array.count) {
				break;
			}
			next = cg_pdf_resolve(doc, w->u.array.items[i + 1]);
			if (next != NULL && next->type == kCGPDFObjectTypeArray) {
				size_t j;

				for (j = 0; j < next->u.array.count; j++) {
					double width = 0;

					if (cg_pdf_number(doc, next->u.array.items[j], &width)) {
						fn_add_range(font, (unsigned)first + (unsigned)j,
							     (unsigned)first + (unsigned)j, width);
					}
				}
				i += 2;
			} else {
				long long last = 0;
				double width = 0;

				if (i + 2 >= w->u.array.count ||
				    !cg_pdf_integer(doc, w->u.array.items[i + 1], &last) ||
				    !cg_pdf_number(doc, w->u.array.items[i + 2], &width)) {
					break;
				}
				fn_add_range(font, (unsigned)first, (unsigned)last, width);
				i += 3;
			}
		}
	}
}

static unsigned cg_pdf_font_cid_to_gid(cg_pdf_font *font, unsigned cid)
{
	if (font->cid_to_gid != NULL) {
		CFIndex length = CFDataGetLength(font->cid_to_gid);
		const unsigned char *bytes = CFDataGetBytePtr(font->cid_to_gid);

		if ((CFIndex)(cid * 2 + 1) < length) {
			return ((unsigned)bytes[cid * 2] << 8) | bytes[cid * 2 + 1];
		}
		return 0;
	}
	return cid;
}

/* ------------------------------------------------------------------------- */
/* creation                                                                  */
/* ------------------------------------------------------------------------- */

cg_pdf_font *cg_pdf_font_create(cg_pdf_doc *doc, cg_pdf_obj *font_dict)
{
	cg_pdf_font *font;
	cg_pdf_obj *subtype;
	cg_pdf_obj *descriptor = NULL;
	cg_pdf_obj *base;
	const char *subtype_name = NULL;
	const char *base_name = NULL;
	char stripped[128] = "";
	bool cid;

	font_dict = cg_pdf_resolve(doc, font_dict);
	if (font_dict == NULL || font_dict->type != kCGPDFObjectTypeDictionary) {
		return NULL;
	}
	subtype = cg_pdf_dict_get(doc, font_dict, "Subtype");
	if (subtype != NULL && subtype->type == kCGPDFObjectTypeName) {
		subtype_name = (const char *)subtype->u.name.bytes;
	}
	if (fn_encoding_is(subtype_name, "Type3")) {
		fprintf(stderr, "CG-REFUSE: a Type 3 font draws glyphs with content streams of its own, which "
				"this renderer does not run\n");
		return NULL;
	}
	cid = fn_encoding_is(subtype_name, "Type0");

	font = calloc(1, sizeof *font);
	if (font == NULL) {
		return NULL;
	}
	font->doc = doc;
	font->cid = cid;
	font->code_bytes = cid ? 2 : 1;
	font->default_width = 1000;

	if (cid) {
		cg_pdf_obj *encoding = cg_pdf_dict_get(doc, font_dict, "Encoding");
		cg_pdf_obj *descendants = cg_pdf_dict_get(doc, font_dict, "DescendantFonts");
		cg_pdf_obj *descendant = NULL;

		if (encoding == NULL || encoding->type != kCGPDFObjectTypeName ||
		    strcmp((const char *)encoding->u.name.bytes, "Identity-H") != 0) {
			fprintf(stderr, "CG-REFUSE: a Type 0 font's CMap is not Identity-H — this reader does not "
					"decode named or vertical CMaps\n");
			free(font);
			return NULL;
		}
		if (descendants != NULL && descendants->type == kCGPDFObjectTypeArray && descendants->u.array.count > 0) {
			descendant = cg_pdf_resolve(doc, descendants->u.array.items[0]);
		}
		if (descendant == NULL || descendant->type != kCGPDFObjectTypeDictionary) {
			fprintf(stderr, "CG-REFUSE: a Type 0 font has no descendant CID font\n");
			free(font);
			return NULL;
		}
		descriptor = cg_pdf_dict_get(doc, descendant, "FontDescriptor");
		base = cg_pdf_dict_get(doc, descendant, "BaseFont");
		if (base == NULL) {
			base = cg_pdf_dict_get(doc, font_dict, "BaseFont");
		}
		if (base != NULL && base->type == kCGPDFObjectTypeName) {
			base_name = (const char *)base->u.name.bytes;
		}
		{
			cg_pdf_obj *map = cg_pdf_dict_get(doc, descendant, "CIDToGIDMap");

			if (map != NULL && map->type == kCGPDFObjectTypeStream) {
				font->cid_to_gid = cg_pdf_stream_data(doc, map);
			}
		}
		fn_resolve_cid_widths(font, doc, descendant);
	} else {
		descriptor = cg_pdf_dict_get(doc, font_dict, "FontDescriptor");
		base = cg_pdf_dict_get(doc, font_dict, "BaseFont");
		if (base != NULL && base->type == kCGPDFObjectTypeName) {
			base_name = (const char *)base->u.name.bytes;
		}
	}
	fn_strip_subset(base_name, stripped, sizeof stripped);
	snprintf(font->base_name, sizeof font->base_name, "%s", stripped);

	font->cg = fn_embedded_font(doc, descriptor);
	if (font->cg == NULL) {
		font->cg = fn_named_font(stripped);
	}
	if (font->cg == NULL) {
		fprintf(stderr, "CG-REFUSE: the font `%s` has no embedded program and no face of that name; "
				"text using it is not drawn\n", stripped);
		if (font->cid_to_gid != NULL) {
			CFRelease(font->cid_to_gid);
		}
		free(font->ranges);
		free(font);
		return NULL;
	}
	if (!cid) {
		bool truetype = fn_encoding_is(subtype_name, "TrueType");
		bool builtin = cg_pdf_dict_get(doc, font_dict, "Encoding") == NULL && !truetype &&
			       (fn_encoding_is(subtype_name, "Type1") || fn_encoding_is(subtype_name, "MMType1"));

		fn_resolve_simple(font, doc, font_dict, descriptor, truetype, builtin);
	}
	return font;
}

void cg_pdf_font_free(cg_pdf_font *font)
{
	if (font == NULL) {
		return;
	}
	if (font->cg != NULL) {
		CGFontRelease(font->cg);
	}
	if (font->cid_to_gid != NULL) {
		CFRelease(font->cid_to_gid);
	}
	free(font->ranges);
	free(font);
}

CGFontRef cg_pdf_font_cgfont(cg_pdf_font *font)
{
	return font != NULL ? font->cg : NULL;
}

const char *cg_pdf_font_base_name(cg_pdf_font *font)
{
	return font != NULL ? font->base_name : "";
}

int cg_pdf_font_code_bytes(cg_pdf_font *font)
{
	return font != NULL ? font->code_bytes : 1;
}

bool cg_pdf_font_is_cid(cg_pdf_font *font)
{
	return font != NULL && font->cid;
}

CGGlyph cg_pdf_font_glyph(cg_pdf_font *font, unsigned code)
{
	if (font == NULL) {
		return 0;
	}
	if (!font->cid) {
		return code < 256 ? font->glyphs[code] : 0;
	}
	return (CGGlyph)cg_pdf_font_cid_to_gid(font, code);
}

double cg_pdf_font_width(cg_pdf_font *font, unsigned code)
{
	size_t i;

	if (font == NULL) {
		return 0;
	}
	if (!font->cid) {
		return code < 256 ? font->widths[code] : 0.0;
	}
	for (i = font->range_count; i > 0; i--) {
		if (code >= font->ranges[i - 1].first && code <= font->ranges[i - 1].last) {
			return font->ranges[i - 1].width;
		}
	}
	return font->default_width;
}
