/*
 * colour_glyphs — a glyph that is a picture, drawn at the size the caller asked for.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT THIS HOLDS UP, AND WHY IT IS A TEST AND NOT A SCREENSHOT. "It can draw emoji" is four separate claims, and
 * three of them were false before the work this case belongs to:
 *
 *   1. THE FACE HAS METRICS AT ALL. A colour font on a Linux desktop is a BITMAP-ONLY face (Noto Color Emoji has no
 *      outlines, one embedded strike, and FreeType reports `units_per_EM == 0`), so the em grid comes from the sfnt
 *      `head` table and ascent and descent from `hhea`. Every one of those numbers was zero, which made the text
 *      tier's `size / upem` scale zero or infinite and made the shaper refuse the face outright.
 *   2. THE CHARACTER MAPS. U+1F600 is outside the BMP, so it arrives as a surrogate pair; mapping the units one at
 *      a time asked the cmap for U+D83D and answered "no glyph" for the only characters this font exists for.
 *   3. THE PICTURE IS THE REQUESTED SIZE. `FT_Set_Char_Size` on a strike face selects nothing and answers a 0x0
 *      bitmap; the strike is selected and scaled instead, which is why a 13-pixel rendering here is about sixteen
 *      pixels wide and NOT the strike's 136.
 *   4. IT LANDS IN COLOUR, IGNORING THE INK. The rasteriser used to refuse `FT_PIXEL_MODE_BGRA` outright. The last
 *      case draws through `CGContextShowGlyphsAtPositions` with a BLACK fill and counts coloured destination pixels,
 *      which is the one assertion that says the picture reached the glass rather than a mask of it.
 *
 * THE HOST'S FONT, WITH A SKIP. There is no colour font in this tree's bundled set (nor in most distributions'
 * default install), so the case asks the registry for the well-known families and SKIPS with 77 when none is
 * present — the same rule the PDF and EPS cases use for `gs`. A machine without one has nothing to measure;
 * pretending otherwise would be a case that passes by not running.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFont_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what, const char *detail)
{
	printf("  [%s] %s%s%s\n", ok ? "ok" : "FAIL", what, (detail != NULL) ? " — " : "",
	       (detail != NULL) ? detail : "");
	if (!ok) {
		failures++;
	}
}

/* THE FAMILIES A COLOUR FACE IS SHIPPED UNDER, best known first. The registry resolves these through the ordinary
 * name door, so a host with any one of them reaches the same code path an application would. */
static CGFontRef load_colour(void)
{
	static const char *names[] = {
		"Noto Color Emoji",
		"Apple Color Emoji",
		"Segoe UI Emoji",
		"EmojiOne Color",
		"Twemoji Mozilla"
	};
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		CFStringRef name = CFStringCreateWithCString(NULL, names[i], kCFStringEncodingUTF8);
		CGFontRef font;

		if (name == NULL) {
			continue;
		}
		font = CGFontCreateWithFontName(name);
		CFRelease(name);
		if (font != NULL) {
			printf("  (colour face: %s)\n", names[i]);
			return font;
		}
	}
	return NULL;
}

/* THE FIRST EMOJI THIS FACE ACTUALLY MAPS, as a surrogate pair in UTF-16 — the form every caller sees. The list is
 * of ordinary "face" emoji, because a font that ships a colour smiley is the case this is about, and a face that
 * maps none of them cannot demonstrate anything. */
static int first_emoji(CGFontRef font, uint16_t pair[2], CGGlyph *glyph)
{
	static const unsigned codes[] = { 0x1F600, 0x1F642, 0x1F680, 0x1F44D, 0x1F525 };
	size_t i;

	for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		uint16_t units[2];
		CGGlyph glyphs[2] = { 0, 0 };
		unsigned c = codes[i];

		units[0] = (uint16_t)(0xD800u + ((c - 0x10000u) >> 10));
		units[1] = (uint16_t)(0xDC00u + ((c - 0x10000u) & 0x3FFu));
		if (cg_font_glyphs_for_unichars(font, units, glyphs, 2) == 2 && glyphs[0] != 0) {
			pair[0] = units[0];
			pair[1] = units[1];
			*glyph = glyphs[0];
			return 1;
		}
	}
	return 0;
}

int main(void)
{
	CGFontRef font = load_colour();
	CGGlyph glyph = 0;
	uint16_t pair[2] = { 0, 0 };
	int upem, ascent, descent;
	CGGlyph mapped[2] = { 0, 0 };
	unsigned char *cov = NULL;
	int w = 0, h = 0, left = 0, top = 0, samples = 0;
	double advance = 0.0;

	if (font == NULL) {
		printf("SKIP: no colour font on this host (Noto Color Emoji, Apple Color Emoji, Segoe UI Emoji, "
		       "EmojiOne Color, Twemoji Mozilla)\n");
		return 77;
	}
	check(cg_font_has_colour(font) != 0, "the face says it has colour glyphs (FT_HAS_COLOR)", NULL);
	upem = CGFontGetUnitsPerEm(font);
	ascent = CGFontGetAscent(font);
	descent = CGFontGetDescent(font);
	{
		char detail[128];

		snprintf(detail, sizeof detail, "upem=%d ascent=%d descent=%d", upem, ascent, descent);
		check(upem > 0 && ascent > 0 && descent > 0,
		      "a bitmap-only face still reports the sfnt's em grid and vertical metrics", detail);
	}

	if (!first_emoji(font, pair, &glyph)) {
		printf("FAIL: this colour face maps none of the emoji this case knows\n");
		return 1;
	}

	/* ---- THE CHARACTER MAPS: ONE GLYPH FOR TWO UNITS -------------------------------------- */
	check(cg_font_glyphs_for_unichars(font, pair, mapped, 2) == 2 && mapped[0] == glyph && mapped[1] == 0,
	      "an astral character maps to ONE glyph, with 0 in the low surrogate's slot", NULL);
	{
		uint16_t lone = pair[0];
		CGGlyph lone_glyph[1] = { 0 };

		check(cg_font_glyphs_for_unichars(font, &lone, lone_glyph, 1) == 0,
		      "a lone high surrogate maps nothing — it is not a character", NULL);
	}
	{
		int units_advance = 0;

		check(CGFontGetGlyphAdvances(font, &glyph, 1, &units_advance) &&
		      upem > 0 && units_advance > 0 && units_advance < upem * 3,
		      "the glyph's advance is a sane number of font units, in the grid the sfnt reports", NULL);
	}

	/* ---- THE RASTER: A PICTURE, AT THE SIZE ASKED FOR -------------------------------------- */
	check(cg_font_render_glyph(font, glyph, 13.0, CGAffineTransformIdentity, CGPointMake(0.0, 0.0), 0,
				   CG_FONT_INK_NONE, &cov, &w, &h, &left, &top, &advance, &samples) &&
	      samples == 4 && w > 0 && h > 0,
	      "a colour glyph renders as four premultiplied channels", NULL);

	/* **THE STRIKE IS NOT THE SIZE.** Noto's single strike is 136x128 at 109 ppem; a 13-pixel ask that answered 136
	 * would mean the picture was placed unscaled, which is the failure the scaling exists to prevent. The band is
	 * generous because the exact em box differs per family, and the point is "a text size", not "sixteen". */
	{
		char detail[128];

		snprintf(detail, sizeof detail, "%dx%d at 13 px, advance=%.2f", w, h, advance);
		check(w > 0 && w < 40 && h > 0 && h < 40, "the picture is a text size, not the strike's own", detail);
	}

	/* THE ALPHA AND THE PREMULTIPLICATION: a colour glyph's channels cannot exceed its coverage. */
	{
		long opaque = 0, coloured = 0, over = 0;
		int i;

		for (i = 0; i < w * h; i++) {
			const unsigned char *p = cov + (size_t)i * 4u;

			if (p[3] != 0) {
				opaque++;
				if (p[0] != p[1] || p[1] != p[2]) {
					coloured++;
				}
			}
			if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) {
				over++;
			}
		}
		{
			char detail[160];

			snprintf(detail, sizeof detail, "%ld opaque, %ld coloured, %ld over alpha", opaque, coloured, over);
			check(opaque > 0 && coloured > 0 && over == 0,
			      "the picture has ink, the ink is COLOUR, and it is premultiplied", detail);
		}
	}

	/* ---- AND IT SCALES PROPORTIONALLY, WHICH IS WHAT MAKES IT USABLE AT ANY SIZE ------------ */
	{
		unsigned char *big = NULL;
		int bw = 0, bh = 0, bl = 0, bt = 0, bsamples = 0;
		double badvance = 0.0;

		if (cg_font_render_glyph(font, glyph, 52.0, CGAffineTransformIdentity, CGPointMake(0.0, 0.0), 0,
					 CG_FONT_INK_NONE, &big, &bw, &bh, &bl, &bt, &badvance, &bsamples)) {
			char detail[160];
			int dw = abs(bw - 4 * w);
			int dh = abs(bh - 4 * h);
			double da = badvance - 4.0 * advance;

			snprintf(detail, sizeof detail, "13 px %dx%d -> 52 px %dx%d", w, h, bw, bh);
			check(bsamples == 4 && dw <= 3 && dh <= 3 && da > -1.0 && da < 1.0,
			      "four times the size is four times the picture and four times the advance", detail);
			free(big);
		} else {
			check(0, "a 52-pixel rendering of the same glyph", "the rasteriser refused it");
		}
	}

	/* ---- THE CACHE ANSWERS THE SAME PICTURE ----------------------------------------------- */
	{
		CGGlyphCacheStats stats;
		const unsigned char *again = NULL;
		int aw = 0, ah = 0, asamples = 0;
		int al = 0, at = 0;
		double aadv = 0.0;
		unsigned long hits;

		/* THE FIRST ASK IS THE MISS AND THE SECOND IS THE HIT, which is the whole claim: the cache remembers a
		 * four-channel picture by the same key it remembers a coverage plane, and answers it byte for byte. */
		cg_font_glyph_cache_flush();
		(void)cg_font_render_glyph_cached(font, glyph, 13.0, CGAffineTransformIdentity, CGPointMake(0.0, 0.0),
						  0, CG_FONT_INK_NONE, &again, &aw, &ah, &al, &at, &aadv, &asamples);
		cg_font_glyph_cache_stats(&stats);
		hits = stats.hits;
		if (cg_font_render_glyph_cached(font, glyph, 13.0, CGAffineTransformIdentity, CGPointMake(0.0, 0.0),
						0, CG_FONT_INK_NONE, &again, &aw, &ah, &al, &at, &aadv, &asamples)) {
			int same = (asamples == 4 && aw == w && ah == h &&
				    memcmp(again, cov, (size_t)w * (size_t)h * 4u) == 0);

			check(same, "the cached rendering is the same picture, sample count and all", NULL);
			cg_font_glyph_cache_stats(&stats);
			check(stats.hits == hits + 1, "and the second ask was a HIT, not a second render", NULL);
			/* THE BUDGET IS FOUR BYTES A PIXEL, which is what makes the cache's accounting honest: a colour
			 * glyph admitted as one byte a pixel would let it hold four times the memory it reports. */
			check(stats.bytes >= (size_t)w * (size_t)h * 4u,
			      "and it was charged four bytes a pixel against the cache budget", NULL);
		} else {
			check(0, "the cached rendering of a colour glyph", "the door refused it");
		}
	}

	/* ---- THE PIXELS: BLACK INK, AND THE PICTURE STILL ARRIVES IN COLOUR -------------------- */
	{
		enum { W = 64, H = 64 };
		unsigned char *px = (unsigned char *)calloc((size_t)W * (size_t)H * 4u, 1);
		CGColorSpaceRef space;
		CGContextRef c;
		long coloured = 0, inked = 0;
		int x, y;

		check(px != NULL, "a bitmap to draw the glyph into", NULL);
		space = CGColorSpaceCreateDeviceRGB();
		c = (px != NULL && space != NULL)
			? CGBitmapContextCreate(px, W, H, 8, (size_t)W * 4u, space,
						kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
			: NULL;
		check(c != NULL, "a bitmap context", NULL);
		if (c != NULL) {
			CGContextSetRGBFillColor(c, 1.0, 1.0, 1.0, 1.0);
			CGContextFillRect(c, CGRectMake(0.0, 0.0, W, H));
			/* THE INK IS BLACK ON PURPOSE: a colour glyph has no ink colour, so a renderer that tinted the
			 * picture black — or drew its alpha as coverage — would leave nothing coloured in the result. */
			CGContextSetRGBFillColor(c, 0.0, 0.0, 0.0, 1.0);
			CGContextSetFont(c, font);
			CGContextSetFontSize(c, 24.0);
			CGContextSetTextMatrix(c, CGAffineTransformIdentity);
			{
				CGPoint pen = CGPointMake(10.0, 12.0);

				CGContextShowGlyphsAtPositions(c, &glyph, &pen, 1);
			}
			for (y = 0; y < H; y++) {
				for (x = 0; x < W; x++) {
					unsigned char *p = px + ((size_t)y * (size_t)W + (size_t)x) * 4u;
					int max = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
					int min = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);

					if (p[0] != 255 || p[1] != 255 || p[2] != 255) {
						inked++;
					}
					if (max - min > 40) {
						coloured++;
					}
				}
			}
			{
				char detail[128];

				snprintf(detail, sizeof detail, "%ld inked pixels, %ld of them coloured", inked, coloured);
				check(inked > 100 && coloured > 40,
				      "a colour glyph lands in a bitmap context IN ITS OWN COLOURS, under a black fill", detail);
			}
			CGContextRelease(c);
		}
		if (space != NULL) {
			CGColorSpaceRelease(space);
		}
		free(px);
	}

	free(cov);
	CGFontRelease(font);

	printf("\ncolour_glyphs: %s (%d failure%s)\n", failures == 0 ? "all checks passed" : "FAILURES",
	       failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}
