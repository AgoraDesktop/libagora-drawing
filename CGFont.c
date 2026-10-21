/*
 * CGFont — the FreeType face, and the answers Apple's font API asks of it.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE ENGINE IS FREETYPE AND IT IS BEHIND A SEAM (CGFont_internal.h), so nothing outside this
 * file includes `<ft2build.h>`. The engine choice is the one the plan's §5 states for every
 * borrowed engine — the SURFACE is ours, the machinery is borrowed — and it is the same shape as
 * pixman for raster, lcms2 for colour and libpng/libjpeg for the codecs.
 *
 * EVERYTHING HERE IS IN FONT UNITS, WHICH IS WHY IT IS OFFLINE AND SIZE-FREE: `FT_LOAD_NO_SCALE`
 * makes FreeType answer in the font's own units, exactly the units `CGFontGetUnitsPerEm` reports,
 * so a metric taken here can be scaled by a caller holding any size. NOTHING IS RASTERISED and no
 * size is ever set — that is what keeps the measured one-face-per-style rule (CGFont.h) true by
 * construction rather than by discipline.
 *
 * ONE ANSWER IS DELIBERATELY ZERO RATHER THAN GUESSED, and it says why where it is:
 * `CGFontGetStemV` (FreeType exposes no stem V for an SFNT face, and Apple's own documentation
 * says 0 when the font has none). THE OTHER TWO ARE NOT ZERO, WHICH IS A MEASURED CORRECTION
 * THIS FILE OWES THE PROBE: `capHeight` and `xHeight` live in the OS/2 table and only from
 * VERSION 2 of it, and DejaVu Sans's table is version 1 -- the first version of this file
 * answered 0, the probe asked why a layout engine had no cap height, and the fix measures the
 * `H` and `x` glyphs instead. A face with neither the field nor the glyph still answers 0.
 *
 * THREADING: THE LIBRARY HANDLE IS INITIALISED ONCE, LAZILY, and that is the whole of this file's
 * shared mutable state. Two threads creating the first font at the same time is a race this tree
 * does not have a primitive for yet; it is named here rather than hidden, and it is the only one.
 */
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFont_internal.h>
#include <CoreGraphics/CGDataProvider_internal.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_LCD_FILTER_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H

#include <stdlib.h>
#include <string.h>

struct CGFont {
	unsigned int retain;
	FT_Face face;

	/* THE PROVIDER IS RETAINED, NOT COPIED: FreeType's memory face POINTS INTO these bytes, so
	 * the provider's lifetime is now the font's. See CGFont.h. */
	CGDataProviderRef provider;

	/* **THE GPOS TABLE, READ ONCE AND KEPT, BECAUSE THE QUESTIONS ASKED OF IT ARE PER PAIR.** A line asks about
	 * every neighbouring pair it has, and reading a 30KB table out of the file for each one would be absurd; the
	 * table is also the only place a modern font's kerning exists at all (see `CGGPosKern.c`). `tried` is what
	 * makes "this face has none" a remembered answer rather than a repeated search. */
	unsigned char *gpos;
	size_t gpos_length;
	int gpos_tried;
};

static FT_Library fn_library_handle;
static int fn_library_ready;

/* THE ENGINE, ONCE. A failure here is permanent for the process (a FreeType that cannot
 * initialise cannot initialise later either), so the flag records the ATTEMPT and every font
 * creation after it answers NULL rather than retrying.
 *
 * **AND THE SUB-PIXEL FILTER IS TURNED ON HERE, BECAUSE THE ENGINE OFFERS IT AND DOES NOT APPLY IT.** An LCD
 * bitmap is three samples per pixel, and without a filter the samples are the outline's own values — which is
 * not what a display does with them: a monitor's three phosphors are convolved by the eye, and text rendered
 * with raw sub-pixel coverage has colour fringes strong enough to read as coloured text. FreeType's
 * `FT_LCD_FILTER_DEFAULT` is the five-tap FIR that does the convolving, and it is OPT-IN — a library that never
 * asks for it gets the fringes. A build without sub-pixel rendering answers an error here, which is not a
 * failure: the smoothing path is then never taken because no glyph comes back in colour. */
static FT_Library fn_library(void)
{
	if (!fn_library_ready) {
		fn_library_ready = 1;
		if (FT_Init_FreeType(&fn_library_handle) != 0) {
			fn_library_handle = NULL;
		} else {
			(void)FT_Library_SetLcdFilter(fn_library_handle, FT_LCD_FILTER_DEFAULT);
		}
	}
	return fn_library_handle;
}

/* ------------------------------------------------------------------------- */
/* the registry's cache: a name resolved once stays resolved                    */
/* ------------------------------------------------------------------------- */

/* RESOLVING A NAME OPENS FONTS UNTIL ONE ANSWERS TO IT, and that costs a file read and a FreeType face — per
 * call, because nothing remembered the answer. MEASURED (2026-10-06): the demo's six text runs re-walked the
 * font directories and re-parsed the faces SIX times, and its text phase was 18.7 ms of a 194 ms drawing —
 * against 5 ms for the whole 900x520 card's fills. THE ARGUMENT FOR THIS IS NOT THAT MEMOISATION IS NICE: a
 * NAME is what a caller has, a FILE is what the registry found, and the caller cannot tell them apart, so the
 * second resolution of one name is pure repeated work.
 *
 * WHAT IT HOLDS IS THE FONT, AND THEREFORE THE FACE AND THE FONT FILE'S BYTES — `CGFont` retains its provider
 * because FreeType's memory face points into them. So it is BOUNDED at a number of faces a text interface
 * actually uses, and the OLDEST ENTRY IS REUSED rather than the cache refusing to grow: the entry being
 * replaced is released by the cache, and any context still holding that font keeps it through its own
 * reference.
 *
 * THE BOUNDARY IS STATED RATHER THAN HIDDEN: the key is the NAME, so a font INSTALLED after that name was
 * resolved is not seen for it — entries are never revalidated, because revalidating means re-reading the
 * directory, which is the thing this exists to stop. THE OTHER INPUT, `FN_FONT_PATH`, IS WATCHED, because it
 * is cheap to watch and because it is a probe's door: if its value changes, the cache is emptied, so a caller
 * that flips it between calls sees the directory it just asked for. And there are no locks: this library has
 * none anywhere, and a registry read by two threads at once is the same question its FreeType handle already
 * answers. */
/* **AND IT IS BIG ENOUGH FOR A CASCADE RATHER THAN FOR ONE NAME.** A per-character font fallback walks the cascade
 * list for every character the current face does not cover, and with EIGHT slots the walk evicted its own entries
 * before it could reuse them — so the directory was re-read for every character anyway, which is how the fuzz sweep
 * measured minutes per line even after misses were cached. MEASURED after this: a repeated miss is 0.0000 s. */
/* **AND IT IS SIZED FOR A WHOLE FONT SET RATHER THAN FOR A CASCADE.** A fallback search walks EVERY installed face —
 * 154 of them on the machine this was measured on — asking each for a name, so a cache smaller than the set evicts its
 * own entries and every search re-reads the directories. MEASURED: the sanitizer suite's hyphenation case ran for more
 * than 280 seconds with this at 64 and finishes in seconds at 512, because the search touched more distinct names than
 * the cache could hold. 512 entries is about 36 KB. */
#define FN_NAME_CACHE	512

static struct {
	char name[64];
	CGFontRef font;
	bool known;		/* the name has been ANSWERED for, whether the answer was a font or nothing */
} fn_name_cache[FN_NAME_CACHE];
static int fn_name_cache_next;
static char fn_name_cache_env[1024];

/* **AND A GENERATION FOR THE FONT SET**, so a caller in another layer can cache what the enumeration answers and know
 * when to throw it away. THE SET CHANGES IN THREE PLACES — a registration, an unregistration, and `FN_FONT_PATH`
 * changing under the process — and all three go through this counter, so a cache keyed on it cannot outlive the facts it
 * was built from. Counting is the whole mechanism: a reader that sees a different number rebuilds, and one that sees the
 * same number is right to reuse.
 *
 * `libagora-text`'s `CTFontMatchingCopyAvailableDescriptors` is the first reader, and the reason this exists: it walked
 * the font directories — opening EVERY font file to read its names — on every call, which made per-character fallback
 * cost a directory walk per character. */
static unsigned long fn_registry_generation = 1;

/* **AND THE CACHE IS EMPTIED IN TWO CASES RATHER THAN ONE**: the search path changing (the probe's door), and
 * the REGISTRY changing (CT-M7h) — because a name that resolved before a registration, or after an
 * unregistration, must resolve again rather than being answered out of a cache that predates the change. */
static void fn_name_cache_flush(void)
{
	int i;

	for (i = 0; i < FN_NAME_CACHE; i++) {
		if (fn_name_cache[i].font != NULL) {
			CGFontRelease(fn_name_cache[i].font);
			fn_name_cache[i].font = NULL;
		}
		fn_name_cache[i].known = false;
	}
	fn_name_cache_next = 0;
}

static void fn_name_cache_note_env(void)
{
	const char *env = getenv("FN_FONT_PATH");
	const char *now = env == NULL ? "" : env;

	if (strcmp(now, fn_name_cache_env) != 0) {
		fn_name_cache_flush();
		fn_registry_generation++;
		snprintf(fn_name_cache_env, sizeof fn_name_cache_env, "%s", now);
	}
}

/* **THE DOOR ITSELF**, after the environment watch it depends on: asking for the generation also NOTICES an `FN_FONT_PATH`
 * change, so a reader that only ever asks this question still sees the font set move when the search path does. */
unsigned long cg_font_registry_generation(void)
{
	fn_name_cache_note_env();
	return fn_registry_generation;
}

/* ------------------------------------------------------------------------- */
/* The process's registered fonts (CT-M7h)                                   */
/* ------------------------------------------------------------------------- */

/* **A THIRD INPUT TO ONE QUESTION, NOT A SECOND ANSWER.** `cg_font_create_with_name` searches the registered
 * paths and then the directories; `cg_font_enumerate` visits both. The registry is here rather than in Core
 * Text so that the name a caller registers and the name a caller ENUMERATES cannot come from two lists that
 * drift — the rule `cg_font_enumerate`'s own comment states for the directory list.
 *
 * THE STORAGE GROWS. A bounded table would need a refusal for the full case, and the era's registration-error
 * vocabulary (six codes) has none; an unbounded list has no such case to name. There are no locks, for the
 * reason the name cache gives: this library has none anywhere. */
static char **fn_registered;
static int fn_registered_count;
static int fn_registered_cap;

static int fn_registered_index(const char *path)
{
	int i;

	for (i = 0; i < fn_registered_count; i++) {
		if (strcmp(fn_registered[i], path) == 0) {
			return i;
		}
	}
	return -1;
}

int cg_font_is_registered(const char *path)
{
	return (path != NULL && fn_registered_index(path) >= 0) ? 1 : 0;
}

int cg_font_register(const char *path)
{
	if (path == NULL || path[0] == '\0') {
		return -1;
	}
	if (cg_font_is_registered(path)) {
		return 0;	/* already there; the CALLER says whether that is an error */
	}
	if (fn_registered_count == fn_registered_cap) {
		int cap = (fn_registered_cap == 0) ? 8 : fn_registered_cap * 2;
		char **grown = realloc(fn_registered, (size_t)cap * sizeof fn_registered[0]);

		if (grown == NULL) {
			return -1;
		}
		fn_registered = grown;
		fn_registered_cap = cap;
	}
	fn_registered[fn_registered_count] = strdup(path);
	if (fn_registered[fn_registered_count] == NULL) {
		return -1;
	}
	fn_registered_count++;
	/* A REGISTRATION CHANGES WHAT A NAME RESOLVES TO, so nothing the cache learned before it survives — and the
	 * GENERATION moves with it, so a cache in a layer above rebuilds rather than remembering a list without this file in
	 * it. */
	fn_name_cache_flush();
	fn_registry_generation++;
	return 1;
}

int cg_font_unregister(const char *path)
{
	int i = (path != NULL) ? fn_registered_index(path) : -1;

	if (i < 0) {
		return 0;
	}
	free(fn_registered[i]);
	for (; i + 1 < fn_registered_count; i++) {
		fn_registered[i] = fn_registered[i + 1];
	}
	fn_registered_count--;
	fn_name_cache_flush();
	fn_registry_generation++;
	return 1;
}

/* +1 FOR THE CALLER, WHICH IS WHAT A `Create` OWES — the cache's own reference is not the caller's. */
static CGFontRef fn_name_cache_get(const char *name)
{
	int i;

	for (i = 0; i < FN_NAME_CACHE; i++) {
		if (fn_name_cache[i].known && strcmp(fn_name_cache[i].name, name) == 0) {
			return (fn_name_cache[i].font != NULL) ? CGFontRetain(fn_name_cache[i].font) : NULL;
		}
	}
	return NULL;
}

/* **AND A NAME THAT IS NOT THERE IS AN ANSWER TOO.** The miss was the expensive one: a name that resolves to nothing
 * walked the whole font directory — 25 ms on the machine this was measured on — and NOTHING CACHED IT, so every
 * caller asked again and walked again. A per-character font fallback (Core Text asks `CTFontCreateForString` once per
 * character, so that substitution stays minimal) therefore cost a directory walk per character that no installed face
 * covers, and a line of 512 odd characters took THIRTEEN SECONDS — which is what the fuzz sweep found, as a hang
 * rather than as a wrong answer. MEASURED after this: a repeated miss is 0.0000 s, like a repeated hit.
 *
 * AND IT IS THE CACHE'S OWN CONTRACT THAT MAKES IT SAFE: the two things that can make a name resolve differently, a
 * REGISTRATION and `FN_FONT_PATH`, both FLUSH this cache already, so an entry cannot outlive the fact it records
 * except by a font file appearing in a watched directory behind this library's back — which is the same staleness a
 * cached HIT already carries. */
static bool fn_name_cache_missed(const char *name)
{
	int i;

	for (i = 0; i < FN_NAME_CACHE; i++) {
		if (fn_name_cache[i].known && strcmp(fn_name_cache[i].name, name) == 0 && fn_name_cache[i].font == NULL) {
			return true;
		}
	}
	return false;
}

static void fn_name_cache_put(const char *name, CGFontRef font)
{
	int slot = fn_name_cache_next;

	/* A NAME THIS CACHE CANNOT HOLD IS NOT CACHED, rather than truncated: a shortened key would answer for
	 * a different name. */
	if (strlen(name) >= sizeof fn_name_cache[0].name) {
		return;
	}
	fn_name_cache_next = (fn_name_cache_next + 1) % FN_NAME_CACHE;
	if (fn_name_cache[slot].font != NULL) {
		CGFontRelease(fn_name_cache[slot].font);
	}
	snprintf(fn_name_cache[slot].name, sizeof fn_name_cache[slot].name, "%s", name);
	fn_name_cache[slot].font = (font != NULL) ? CGFontRetain(font) : NULL;
	fn_name_cache[slot].known = true;
}

CGFontRef CGFontCreateWithDataProvider(CGDataProviderRef provider)
{
	CGFontRef font;
	FT_Library lib;
	const void *bytes;
	size_t size;

	if (provider == NULL) {
		return NULL;
	}
	lib = fn_library();
	if (lib == NULL) {
		return NULL;
	}
	bytes = cg_dataprovider_bytes(provider, &size);
	if (bytes == NULL || size == 0) {
		return NULL;
	}
	font = calloc(1, sizeof(struct CGFont));
	if (font == NULL) {
		return NULL;
	}
	if (FT_New_Memory_Face(lib, (const FT_Byte *)bytes, (FT_Long)size, 0, &font->face) != 0) {
		free(font);
		return NULL;
	}
	font->retain = 1;
	font->provider = CGDataProviderRetain(provider);
	return font;
}

/* DECLARED AND ALWAYS REFUSES — see CGFont.h, where Apple's own sentence is the ground ("For MacOS X,
 * `platformFontReference' should be a pointer to an ATSFontRef"). The named alternatives are the two
 * doors this platform can honestly answer, and they are named IN THE MESSAGE for the same reason the
 * header names them: a ported caller reading this needs somewhere to go. */
CGFontRef CGFontCreateWithPlatformFont(void *platformFontReference)
{
	(void)platformFontReference;
	fprintf(stderr, "CG-REFUSE: CGFontCreateWithPlatformFont takes a pointer to a platform-specific "
			"font reference — on MacOS X an ATSFontRef, in this header's own words — and this "
			"platform has no ATS; use CGFontCreateWithDataProvider or CGFontCreateWithFontName\n");
	return NULL;
}

CGFontRef CGFontRetain(CGFontRef font)
{
	if (font != NULL) {
		font->retain++;
	}
	return font;
}

void CGFontRelease(CGFontRef font)
{
	if (font == NULL) {
		return;
	}
	if (font->retain > 1) {
		font->retain--;
		return;
	}
	if (font->face != NULL) {
		FT_Done_Face(font->face);
	}
	if (font->provider != NULL) {
		CGDataProviderRelease(font->provider);
	}
	free(font->gpos);
	free(font);
}

void *cg_font_face(CGFontRef font)
{
	return font == NULL ? NULL : (void *)font->face;
}

size_t CGFontGetNumberOfGlyphs(CGFontRef font)
{
	if (font == NULL || font->face == NULL) {
		return 0;
	}
	return (size_t)font->face->num_glyphs;
}

int CGFontGetUnitsPerEm(CGFontRef font)
{
	if (font == NULL || font->face == NULL) {
		return 0;
	}
	return (int)font->face->units_per_EM;
}

/* THE FOUR VERTICAL METRICS COME FROM THE FACE'S OWN HEAD/HHEA NUMBERS, in font units, which is
 * what FreeType puts in `ascender`/`descender`/`height` for a scalable face. (The 26.6 values in
 * `face->size->metrics` are NOT used anywhere in this file: they are the SIZED ones, and a font
 * here has no size.)
 *
 * **AND TWO OF THE FOUR ARE NOT THE FACE FIELD OF THE SAME NAME, WHICH IS THE WHOLE OF THIS
 * COMMENT.** The names line up — `ascender` with `GetAscent`, `descender` with `GetDescent`,
 * `height` with `GetLeading` — and MEASURED, that coincidence cost this tree a line height of
 * 1.94 em: a 13-point font measured 25.2 points tall, so every label, menu row and button in the
 * AppKit was roughly ten points taller than the ink it carried.
 *
 *   * `face->descender` IS NEGATIVE — FreeType's convention, and the convention the font file's
 *     own hhea table uses. `CGFontGetDescent` answers a DESCENT, which is a magnitude, so the
 *     sign is dropped here. `NSFont -descender` puts it back (that door's contract is a negative
 *     value), which is why the negation lives in exactly one of the two layers.
 *   * `face->height` IS THE LINE HEIGHT, NOT THE LEADING: FreeType's own documentation calls it
 *     "the vertical distance between two consecutive baselines", which is ascent + descent +
 *     gap. Its callers here ADD it to ascent and descent — `CTRunGetTypographicBounds` and
 *     `CTFramesetter` both compute `ascent + descent + leading` — so answering it as the leading
 *     counts the whole line twice. THE LEADING IS THE PART LEFT OVER, which is the subtraction
 *     below; for a font whose line gap is zero (DejaVu Sans, the face this tree measures with)
 *     that leftover is exactly 0, and a 13-point line comes out 15.7 points — 1.21 em, which is
 *     what a line height is supposed to be. */
int CGFontGetAscent(CGFontRef font)
{
	return (font == NULL || font->face == NULL) ? 0 : (int)font->face->ascender;
}

int CGFontGetDescent(CGFontRef font)
{
	return (font == NULL || font->face == NULL) ? 0 : -(int)font->face->descender;
}

int CGFontGetLeading(CGFontRef font)
{
	int lineHeight;
	int glyphs;

	if (font == NULL || font->face == NULL) {
		return 0;
	}
	lineHeight = (int)font->face->height;
	glyphs = (int)font->face->ascender - (int)font->face->descender;
	/* A FONT WHOSE TABLE DISAGREES WITH ITSELF — a height SHORTER than ascent + descent — answers zero
	 * rather than a negative gap, because a negative leading would pull the next line UP through this
	 * one. FreeType's own `height` is normally the larger of the two, so this is a guard against a
	 * broken face and not a case a caller should ever meet. */
	return (lineHeight > glyphs) ? (lineHeight - glyphs) : 0;
}

/* THE OS/2 METRICS, AND THE VERSION CHECK IS THE POINT: sxHeight and sCapHeight were ADDED in
 * OS/2 version 2. A version-0 or -1 table has no such fields, so reading them would read whatever
 * bytes follow — which is why a font whose table is older answers 0 and says so in the header. */
static int fn_os2_metric(FT_Face face, int want_cap_height)
{
	TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(face, ft_sfnt_os2);

	if (os2 != NULL && os2->version >= 2) {
		return want_cap_height ? (int)os2->sCapHeight : (int)os2->sxHeight;
	}
	/* AND WHEN THE TABLE DOES NOT STATE IT, THE ANSWER IS DERIVED RATHER THAN ZERO -- WHICH IS A
	 * MEASURED CORRECTION, NOT A REFINEMENT: the font probe asked for a cap height and got 0 from
	 * DejaVu Sans, whose OS/2 table is version 1 and carries no such field, and a layout engine
	 * that cannot answer `how tall is a capital` cannot place a line of text at all. THE
	 * REFERENCE GLYPHS ARE THE USUAL ONES: `H` for the cap height, `x` for the x-height, measured
	 * with the same unscaled load the advances use, so the number is in the same units as
	 * everything else here. A face with neither the field nor the glyph answers 0. */
	{
		FT_UInt index = FT_Get_Char_Index(face, want_cap_height ? 0x48u : 0x78u);

		if (index == 0
		    || FT_Load_Glyph(face, index, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0) {
			return 0;
		}
		return (int)face->glyph->metrics.height;
	}
}

int CGFontGetCapHeight(CGFontRef font)
{
	if (font == NULL || font->face == NULL) {
		return 0;
	}
	return fn_os2_metric(font->face, 1);
}

int CGFontGetXHeight(CGFontRef font)
{
	if (font == NULL || font->face == NULL) {
		return 0;
	}
	return fn_os2_metric(font->face, 0);
}

CGFloat CGFontGetStemV(CGFontRef font)
{
	(void)font;
	/* ZERO, AND IT IS APPLE'S OWN ANSWER FOR THIS CASE: a stem V is a PostScript hint (a CFF or
	 * Type 1 notion), Apple's documentation says the answer is 0 when the font has none, and an
	 * SFNT face does not. FreeType has no stem V to give either, so there is nothing to be lost by
	 * saying so. A CFF font would be the case that needs the OpenType `CFF ` table's Private
	 * DICT — recorded, not guessed at. */
	return 0.0;
}

CGFloat CGFontGetItalicAngle(CGFontRef font)
{
	if (font == NULL || font->face == NULL) {
		return 0.0;
	}
	/* THE ANGLE IS THE SFNT `post` TABLE'S, REACHED THROUGH THE TABLE ACCESSOR RATHER THAN A
	 * FACE MEMBER -- `FT_FaceRec` has no public `postscript` field (that is the internal
	 * TT_FaceRec layout), so `FT_Get_Sfnt_Table(ft_sfnt_post)` is the portable way to the same
	 * number. It is a 16.16 fixed in DEGREES, counter-clockwise positive, which is exactly
	 * Apple's own units, so the only conversion is the fixed-point shift. A face with no `post`
	 * table answers 0.0 -- correct for a face whose angle is not stated. */
	{
		TT_Postscript *post = (TT_Postscript *)FT_Get_Sfnt_Table(font->face, ft_sfnt_post);

		if (post == NULL) {
			return 0.0;
		}
		return (CGFloat)(post->italicAngle / 65536.0);
	}
}

CGRect CGFontGetFontBBox(CGFontRef font)
{
	CGRect box = CGRectMake(0.0, 0.0, 0.0, 0.0);

	if (font == NULL || font->face == NULL) {
		return box;
	}
	/* THE GLOBAL BBOX, in font units, as the face's own HEAD table states it. */
	box.origin.x = (CGFloat)font->face->bbox.xMin;
	box.origin.y = (CGFloat)font->face->bbox.yMin;
	box.size.width = (CGFloat)(font->face->bbox.xMax - font->face->bbox.xMin);
	box.size.height = (CGFloat)(font->face->bbox.yMax - font->face->bbox.yMin);
	return box;
}

/* ONE LOAD, IN FONT UNITS, IS WHAT EVERY GLYPH ANSWER IS BUILT ON — advances and boxes both.
 * `FT_LOAD_NO_HINTING` goes with `FT_LOAD_NO_SCALE` because there is no size to hint for, and a
 * hinted metric would be a metric of some size rather than of the font. */
static int fn_load_glyph(FT_Face face, CGGlyph glyph)
{
	return FT_Load_Glyph(face, (FT_UInt)glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) == 0;
}

/* THE TWO-PASS RULE THE HEADER PROMISES: a caller that asked for twenty answers must not get
 * nineteen and a half. Pass one LOADS every glyph and refuses the whole request if any fails;
 * pass two writes. It costs a second load per glyph and buys an all-or-nothing answer, which is
 * what `false` means. */
bool CGFontGetGlyphAdvances(CGFontRef font, const CGGlyph glyphs[], size_t count, int advances[])
{
	size_t i;

	if (font == NULL || font->face == NULL || glyphs == NULL || advances == NULL) {
		return false;
	}
	for (i = 0; i < count; i++) {
		if (!fn_load_glyph(font->face, glyphs[i])) {
			return false;
		}
	}
	for (i = 0; i < count; i++) {
		(void)fn_load_glyph(font->face, glyphs[i]);
		advances[i] = (int)font->face->glyph->metrics.horiAdvance;
	}
	return true;
}

/* **THE ADJUSTMENT A PAIR OF GLYPHS ASKS FOR, IN FONT UNITS — WHICH IS WHAT KERNING IS.** A font's own
 * typographer says that an `o` belongs closer under a `T` than the sum of their two advances puts it, and the
 * `kern` table is where that opinion is written down: MEASURED, DejaVu Sans asks for `To` to close by 348 units of
 * 2048 — a sixth of an em, which at 13 points is two pixels of daylight between the two letters.
 *
 * IT IS IN FONT UNITS FOR THE REASON `CGFontGetGlyphAdvances` IS: the caller holds the size and the em size, and
 * a door that scaled here would be the second place that knows how a font becomes points.
 *
 * ZERO IS THE ORDINARY ANSWER AND NOT A FAILURE — most pairs are not in the table — and it is also what a face
 * WITHOUT kerning answers, which is `FT_HAS_KERNING`'s question and is asked first so that a font with no table
 * does not take the engine's error path once per pair. THE OLD `kern` TABLE IS WHAT THE ENGINE READS: a face
 * whose kerning lives only in `GPOS` answers zero here, which is stated in the header rather than left to be
 * discovered. */
int cg_font_kern(CGFontRef font, CGGlyph left, CGGlyph right)
{
	FT_Vector kern;

	if (font == NULL || font->face == NULL) {
		return 0;
	}
	/* **THE OLD TABLE FIRST, AND THE MODERN ONE WHEN THERE IS NO OLD ONE.** `FT_HAS_KERNING` is the engine saying
	 * whether the face HAS a `kern` table; a face without one used to get a zero for every pair from here, and
	 * MEASURED that is exactly the case this desktop was in — its UI face keeps every pair in GPOS, where FreeType
	 * does not look. So a face with the old table is answered by the engine, and a face without one is answered by
	 * this library's own reader. A face with BOTH would be answered by the engine's, which is the older of the two
	 * and the one a font that still ships it means. */
	if (FT_HAS_KERNING(font->face)) {
		if (FT_Get_Kerning(font->face, (FT_UInt)left, (FT_UInt)right, FT_KERNING_UNSCALED, &kern) != 0) {
			return 0;
		}
		return (int)kern.x;
	}
	if (!font->gpos_tried) {
		const uint32_t tag = ((uint32_t)'G' << 24) | ((uint32_t)'P' << 16) | ((uint32_t)'O' << 8)
				   | (uint32_t)'S';
		int size;

		font->gpos_tried = 1;
		size = cg_font_copy_table(font, tag, NULL, 0);
		if (size > 0) {
			font->gpos = (unsigned char *)malloc((size_t)size);
			if (font->gpos != NULL) {
				if (cg_font_copy_table(font, tag, font->gpos, (size_t)size) == size) {
					font->gpos_length = (size_t)size;
				} else {
					free(font->gpos);
					font->gpos = NULL;
				}
			}
		}
	}
	return cg_gpos_kern(font->gpos, font->gpos_length, (unsigned)left, (unsigned)right);
}

bool CGFontGetGlyphBBoxes(CGFontRef font, const CGGlyph glyphs[], size_t count, CGRect bboxes[])
{
	size_t i;

	if (font == NULL || font->face == NULL || glyphs == NULL || bboxes == NULL) {
		return false;
	}
	for (i = 0; i < count; i++) {
		if (!fn_load_glyph(font->face, glyphs[i])) {
			return false;
		}
	}
	for (i = 0; i < count; i++) {
		FT_Glyph_Metrics *m;

		(void)fn_load_glyph(font->face, glyphs[i]);
		m = &font->face->glyph->metrics;
		/* THE BOX'S ORIGIN IS THE LOWER-LEFT CORNER, so the y is the top bearing MINUS the
		 * height: a glyph box is stated from the baseline up, and a caller drawing it needs the
		 * corner, not the bearing. x and width come straight across, because the left bearing IS
		 * the left edge. */
		bboxes[i] = CGRectMake((CGFloat)m->horiBearingX,
				       (CGFloat)(m->horiBearingY - m->height),
				       (CGFloat)m->width, (CGFloat)m->height);
	}
	return true;
}

const char *cg_font_postscript_name(CGFontRef font)
{
	if (font == NULL || font->face == NULL) {
		return NULL;
	}
	return FT_Get_Postscript_Name(font->face);
}

/* THE RAW TABLE DOORS -- SEE CGFont_internal.h FOR THE TWO-CALL CONTRACT AND WHAT THEY ARE FOR.
 *
 * AND `FT_Load_Sfnt_Table` IS THE WHOLE IMPLEMENTATION, WHICH IS THE POINT OF PUTTING THEM HERE RATHER THAN IN CORE
 * TEXT: FreeType already holds the file's table directory, so the seam hands back bytes it has had all along, and the
 * alternative -- Core Text parsing an sfnt itself -- would be a second font engine in a tree that has one.
 *
 * `HEAD` AND FRIENDS ARE NOT PARSED HERE. The door is the file's bytes, and its callers are the doors that know what
 * the bytes mean. */
int cg_font_copy_table(CGFontRef font, uint32_t tag, unsigned char *buf, size_t size)
{
	/* `length` IS IN AND OUT: in, the room the caller gave; out, the table's true size whether or not it fit. */
	FT_ULong length = (buf != NULL) ? (FT_ULong)size : 0;

	if (font == NULL || font->face == NULL) {
		return 0;
	}
	if (FT_Load_Sfnt_Table(font->face, (FT_ULong)tag, 0, buf, &length) != 0) {
		return 0;
	}
	return (int)length;
}

int cg_font_table_tags(CGFontRef font, uint32_t *tags, size_t capacity)
{
	FT_ULong tag = 0;
	FT_ULong length = 0;
	FT_UInt index = 0;
	size_t count = 0;

	if (font == NULL || font->face == NULL) {
		return 0;
	}
	/* THE ENUMERATION ENDS WHEN THE DIRECTORY DOES: FreeType reports Table_Missing once the index is past the last
	 * entry, which is the only way this call says "no more". */
	while (FT_Sfnt_Table_Info(font->face, index, &tag, &length) == 0) {
		if (tags != NULL && count < capacity) {
			tags[count] = (uint32_t)tag;
		}
		count++;
		index++;
	}
	return (int)count;
}

/* UTF-16BE (the Windows name table) INTO UTF-8, IN CALLER MEMORY. Surrogate pairs are handled
 * because a full name is allowed to contain anything; a lone surrogate is skipped rather than
 * encoded, which is the same choice every UTF-16 decoder in this tree makes (see NSString.m). */
static int fn_utf16be_to_utf8(const unsigned char *s, size_t len, char *out, size_t outsize)
{
	size_t i = 0, o = 0;

	while (i + 1 < len) {
		unsigned long cp = ((unsigned long)s[i] << 8) | s[i + 1];

		i += 2;
		if (cp == 0) {
			break;
		}
		if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
			unsigned long lo = ((unsigned long)s[i] << 8) | s[i + 1];

			if (lo >= 0xDC00 && lo <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
		}
		if (cp < 0x80) {
			if (o + 1 >= outsize) {
				return 0;
			}
			out[o++] = (char)cp;
		} else if (cp < 0x800) {
			if (o + 2 >= outsize) {
				return 0;
			}
			out[o++] = (char)(0xC0 | (cp >> 6));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			if (o + 3 >= outsize) {
				return 0;
			}
			out[o++] = (char)(0xE0 | (cp >> 12));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		} else {
			if (o + 4 >= outsize) {
				return 0;
			}
			out[o++] = (char)(0xF0 | (cp >> 18));
			out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[o] = '\0';
	return (int)o;
}

/* THE FULL NAME IS NAME-ID 4, AND THE PREFERENCE ORDER IS STATED RATHER THAN INCIDENTAL: the
 * Windows (platform 3, encoding 1) record is UTF-16BE and is the one every modern font carries;
 * the Mac (platform 1) record is a single-byte encoding whose ASCII range is ASCII, which is what
 * this tree's other decoders assume too. The first record that MATCHES a preferred shape wins;
 * when none does, the answer is 0 and the caller gets NULL. */
/* THE `name` TABLE'S ENTRY FOR ONE nameID. `cg_font_full_name` below IS this function with nameID 4,
 * and it stays its own door because the PUBLIC `CGFontCopyFullName` names it. The entry chosen is the
 * one this loop has always chosen — the Windows Unicode-BMP entry (platform 3, encoding 1) preferred,
 * then the first Macintosh one — and the Mac fallback is copied BYTE FOR BYTE because a platform-1
 * string is MacRoman, not UTF-16. */
int cg_font_name_entry(CGFontRef font, int name_id, char *buf, size_t size)
{
	FT_SfntName name;
	FT_UInt count, i;
	int have_mac = 0;

	if (font == NULL || font->face == NULL || buf == NULL || size == 0) {
		return 0;
	}
	count = FT_Get_Sfnt_Name_Count(font->face);
	for (i = 0; i < count; i++) {
		if (FT_Get_Sfnt_Name(font->face, i, &name) != 0 || (int)name.name_id != name_id) {
			continue;
		}
		if (name.platform_id == 3 && name.encoding_id == 1) {
			return fn_utf16be_to_utf8((const unsigned char *)name.string,
						  (size_t)name.string_len, buf, size);
		}
		if (name.platform_id == 1 && !have_mac) {
			size_t n = (size_t)name.string_len;

			have_mac = 1;
			if (n + 1 > size) {
				continue;
			}
			memcpy(buf, name.string, n);
			buf[n] = '\0';
		}
	}
	if (have_mac) {
		return (int)strlen(buf);
	}
	return 0;
}

int cg_font_full_name(CGFontRef font, char *buf, size_t size)
{
	return cg_font_name_entry(font, 4, buf, size);
}

int cg_font_glyph_name(CGFontRef font, CGGlyph glyph, char *buf, size_t size)
{
	if (font == NULL || font->face == NULL || buf == NULL || size == 0) {
		return 0;
	}
	if (!FT_HAS_GLYPH_NAMES(font->face)) {
		return 0;
	}
	if (FT_Get_Glyph_Name(font->face, (FT_UInt)glyph, buf, (FT_UInt)size) != 0) {
		return 0;
	}
	return (int)strlen(buf);
}

/* THE MACROMAN HIGH HALF, GENERATED FROM THE STANDARD MAPPING RATHER THAN RECALLED: 0x00-0x7F is
 * ASCII and 0x80-0xFF is what Mac OS Roman defines (0x80 = A-diaeresis, 0xA9 = copyright,
 * 0xFF = caron). Python's codec IS that standard table, which is what generated these 128
 * constants. */
static const unsigned short fn_macroman_high[128] = {
	0x00C4, /* 0x80 */
	0x00C5, /* 0x81 */
	0x00C7, /* 0x82 */
	0x00C9, /* 0x83 */
	0x00D1, /* 0x84 */
	0x00D6, /* 0x85 */
	0x00DC, /* 0x86 */
	0x00E1, /* 0x87 */
	0x00E0, /* 0x88 */
	0x00E2, /* 0x89 */
	0x00E4, /* 0x8A */
	0x00E3, /* 0x8B */
	0x00E5, /* 0x8C */
	0x00E7, /* 0x8D */
	0x00E9, /* 0x8E */
	0x00E8, /* 0x8F */
	0x00EA, /* 0x90 */
	0x00EB, /* 0x91 */
	0x00ED, /* 0x92 */
	0x00EC, /* 0x93 */
	0x00EE, /* 0x94 */
	0x00EF, /* 0x95 */
	0x00F1, /* 0x96 */
	0x00F3, /* 0x97 */
	0x00F2, /* 0x98 */
	0x00F4, /* 0x99 */
	0x00F6, /* 0x9A */
	0x00F5, /* 0x9B */
	0x00FA, /* 0x9C */
	0x00F9, /* 0x9D */
	0x00FB, /* 0x9E */
	0x00FC, /* 0x9F */
	0x2020, /* 0xA0 */
	0x00B0, /* 0xA1 */
	0x00A2, /* 0xA2 */
	0x00A3, /* 0xA3 */
	0x00A7, /* 0xA4 */
	0x2022, /* 0xA5 */
	0x00B6, /* 0xA6 */
	0x00DF, /* 0xA7 */
	0x00AE, /* 0xA8 */
	0x00A9, /* 0xA9 */
	0x2122, /* 0xAA */
	0x00B4, /* 0xAB */
	0x00A8, /* 0xAC */
	0x2260, /* 0xAD */
	0x00C6, /* 0xAE */
	0x00D8, /* 0xAF */
	0x221E, /* 0xB0 */
	0x00B1, /* 0xB1 */
	0x2264, /* 0xB2 */
	0x2265, /* 0xB3 */
	0x00A5, /* 0xB4 */
	0x00B5, /* 0xB5 */
	0x2202, /* 0xB6 */
	0x2211, /* 0xB7 */
	0x220F, /* 0xB8 */
	0x03C0, /* 0xB9 */
	0x222B, /* 0xBA */
	0x00AA, /* 0xBB */
	0x00BA, /* 0xBC */
	0x03A9, /* 0xBD */
	0x00E6, /* 0xBE */
	0x00F8, /* 0xBF */
	0x00BF, /* 0xC0 */
	0x00A1, /* 0xC1 */
	0x00AC, /* 0xC2 */
	0x221A, /* 0xC3 */
	0x0192, /* 0xC4 */
	0x2248, /* 0xC5 */
	0x2206, /* 0xC6 */
	0x00AB, /* 0xC7 */
	0x00BB, /* 0xC8 */
	0x2026, /* 0xC9 */
	0x00A0, /* 0xCA */
	0x00C0, /* 0xCB */
	0x00C3, /* 0xCC */
	0x00D5, /* 0xCD */
	0x0152, /* 0xCE */
	0x0153, /* 0xCF */
	0x2013, /* 0xD0 */
	0x2014, /* 0xD1 */
	0x201C, /* 0xD2 */
	0x201D, /* 0xD3 */
	0x2018, /* 0xD4 */
	0x2019, /* 0xD5 */
	0x00F7, /* 0xD6 */
	0x25CA, /* 0xD7 */
	0x00FF, /* 0xD8 */
	0x0178, /* 0xD9 */
	0x2044, /* 0xDA */
	0x20AC, /* 0xDB */
	0x2039, /* 0xDC */
	0x203A, /* 0xDD */
	0xFB01, /* 0xDE */
	0xFB02, /* 0xDF */
	0x2021, /* 0xE0 */
	0x00B7, /* 0xE1 */
	0x201A, /* 0xE2 */
	0x201E, /* 0xE3 */
	0x2030, /* 0xE4 */
	0x00C2, /* 0xE5 */
	0x00CA, /* 0xE6 */
	0x00C1, /* 0xE7 */
	0x00CB, /* 0xE8 */
	0x00C8, /* 0xE9 */
	0x00CD, /* 0xEA */
	0x00CE, /* 0xEB */
	0x00CF, /* 0xEC */
	0x00CC, /* 0xED */
	0x00D3, /* 0xEE */
	0x00D4, /* 0xEF */
	0xF8FF, /* 0xF0 */
	0x00D2, /* 0xF1 */
	0x00DA, /* 0xF2 */
	0x00DB, /* 0xF3 */
	0x00D9, /* 0xF4 */
	0x0131, /* 0xF5 */
	0x02C6, /* 0xF6 */
	0x02DC, /* 0xF7 */
	0x00AF, /* 0xF8 */
	0x02D8, /* 0xF9 */
	0x02D9, /* 0xFA */
	0x02DA, /* 0xFB */
	0x00B8, /* 0xFC */
	0x02DD, /* 0xFD */
	0x02DB, /* 0xFE */
	0x02C7, /* 0xFF */
};

/* THE BYTE, THROUGH THE ENCODING THE CALLER CHOSE. The font-specific reading is the byte AS a character
 * code (see CGContext.h); the MacRoman one is the byte as a MAC OS ROMAN character, whose code point the
 * table above gives — so one byte reaches two different glyphs, which is the whole reason the encoding is
 * a parameter and not a constant. */
CGGlyph cg_font_glyph_for_byte(CGFontRef font, unsigned char code, int macroman)
{
	FT_ULong ch = (FT_ULong)code;
	FT_UInt index;

	if (font == NULL || font->face == NULL) {
		return (CGGlyph)kCGFontIndexInvalid;
	}
	if (macroman && code >= 0x80) {
		ch = (FT_ULong)fn_macroman_high[code - 0x80];
	}
	index = FT_Get_Char_Index(font->face, ch);
	if (index == 0) {
		/* ZERO IS "THIS FONT HAS NO GLYPH FOR THAT CODE" — and it is also .notdef's index, which is
		 * why it travels as kCGFontIndexInvalid and not as a glyph to draw. */
		return (CGGlyph)kCGFontIndexInvalid;
	}
	return (CGGlyph)index;
}

/* THE UNICODE STEP, IN A BATCH, BECAUSE THAT IS THE SHAPE CORE TEXT'S DOOR HAS: CTFontGetGlyphsForCharacters
 * takes an array of UniChars and fills an array of glyphs. ONE CALL PER CHARACTER would be that loop in the
 * caller, and the loop belongs where the face is.
 *
 * THE ANSWERS ARE FREETYPE'S AND THEREFORE APPLE'S: FT_Get_Char_Index answers 0 for a character this face has
 * no glyph for, and 0 is ALSO `.notdef`'s index -- which is what Apple's own door reports in that slot -- so
 * the array is filled with 0 where the mapping failed and the RETURN says how many mapped. A caller that
 * needs Apple's yes-or-no asks whether the return equals the count. */
int cg_font_glyphs_for_unichars(CGFontRef font, const uint16_t *characters, CGGlyph *glyphs, size_t count)
{
	size_t i;
	int mapped = 0;

	if (font == NULL || font->face == NULL || characters == NULL || glyphs == NULL) {
		return 0;
	}
	for (i = 0; i < count; i++) {
		FT_UInt index = FT_Get_Char_Index(font->face, (FT_ULong)characters[i]);

		glyphs[i] = (CGGlyph)index;
		if (index != 0) {
			mapped++;
		}
	}
	return mapped;
}

CGGlyph cg_font_glyph_with_name(CGFontRef font, const char *name)
{
	FT_UInt index;

	if (font == NULL || font->face == NULL || name == NULL) {
		return (CGGlyph)kCGFontIndexInvalid;
	}
	if (!FT_HAS_GLYPH_NAMES(font->face)) {
		return (CGGlyph)kCGFontIndexInvalid;
	}
	index = FT_Get_Name_Index(font->face, (FT_String *)(uintptr_t)name);
	if (index == 0) {
		/* ZERO IS "NOT FOUND" HERE AND IT IS ALSO .notdef, which has no name: a named lookup
		 * that answers 0 has found nothing, and kCGFontIndexInvalid is how that travels. */
		return (CGGlyph)kCGFontIndexInvalid;
	}
	return (CGGlyph)index;
}

CGGlyph cg_font_glyph_for_unichar(CGFontRef font, unsigned unichar)
{
	FT_UInt index;

	if (font == NULL || font->face == NULL) {
		return (CGGlyph)kCGFontIndexInvalid;
	}
	index = FT_Get_Char_Index(font->face, (FT_ULong)unichar);
	if (index == 0) {
		return (CGGlyph)kCGFontIndexInvalid;
	}
	return (CGGlyph)index;
}

CGDataProviderRef cg_font_copy_provider(CGFontRef font)
{
	if (font == NULL || font->provider == NULL) {
		return NULL;
	}
	return CGDataProviderRetain(font->provider);
}

bool cg_font_has_truetype_outlines(CGFontRef font)
{
	FT_ULong length = 0;

	if (font == NULL || font->face == NULL || !FT_IS_SFNT(font->face)) {
		return false;
	}
	return FT_Load_Sfnt_Table(font->face, FT_MAKE_TAG('g', 'l', 'y', 'f'), 0, NULL, &length) == 0 &&
	       length > 0;
}

size_t cg_font_unicode_map(CGFontRef font, unsigned *map, size_t count)
{
	FT_CharMap saved;
	FT_UInt glyph = 0;
	FT_ULong code;
	size_t filled = 0;

	if (font == NULL || font->face == NULL || map == NULL || count == 0) {
		return 0;
	}
	memset(map, 0, count * sizeof *map);
	saved = font->face->charmap;
	if (FT_Select_Charmap(font->face, FT_ENCODING_UNICODE) != 0) {
		return 0;
	}
	code = FT_Get_First_Char(font->face, &glyph);
	while (glyph != 0) {
		if ((size_t)glyph < count && map[glyph] == 0) {
			map[glyph] = (unsigned)code;
			filled++;
		}
		code = FT_Get_Next_Char(font->face, code, &glyph);
	}
	if (saved != NULL) {
		FT_Set_Charmap(font->face, saved);
	}
	return filled;
}

CGGlyph cg_font_glyph_for_code(CGFontRef font, unsigned code)
{
	static const FT_Encoding encodings[] = {
		FT_ENCODING_ADOBE_CUSTOM, FT_ENCODING_ADOBE_STANDARD, FT_ENCODING_ADOBE_EXPERT
	};
	FT_CharMap saved;
	CGGlyph result = (CGGlyph)kCGFontIndexInvalid;
	size_t i;

	if (font == NULL || font->face == NULL) {
		return result;
	}
	saved = font->face->charmap;
	for (i = 0; i < sizeof encodings / sizeof encodings[0]; i++) {
		FT_UInt index;

		if (FT_Select_Charmap(font->face, encodings[i]) != 0) {
			continue;
		}
		index = FT_Get_Char_Index(font->face, (FT_ULong)code);
		if (index != 0) {
			result = (CGGlyph)index;
			break;
		}
	}
	if (saved != NULL) {
		FT_Set_Charmap(font->face, saved);
	}
	return result;
}

/* ------------------------------------------------------------------------- */
/* Rasterisation: the one door that asks the engine for pixels                */
/* ------------------------------------------------------------------------- */

int cg_font_render_glyph(CGFontRef font, CGGlyph glyph, CGFloat pixel_size, CGAffineTransform matrix,
			 CGPoint delta, int smoothing, unsigned char **coverage, int *width, int *height,
			 int *left, int *top, double *advance, int *samples)
{
	FT_Matrix m;
	FT_Vector d;
	FT_GlyphSlot slot;
	unsigned char *buf;
	int w, h, row, mirror = 0;
	int per_pixel = 1;

	if (font == NULL || font->face == NULL || coverage == NULL || width == NULL || height == NULL) {
		return 0;
	}
	if (pixel_size <= 0.0) {
		return 0;
	}
	/* THE RESOLUTIONS ARE 72 DPI, WHICH IS THE 1:1 CASE — measured: passing 0 ("the default") is
	 * REFUSED by the engine in this tree and every glyph came back as a silent nothing. */
	if (FT_Set_Char_Size(font->face, 0, (FT_F26Dot6)(pixel_size * 64.0 + 0.5), 72, 72) != 0) {
		fprintf(stderr, "CG-REFUSE: the engine would not set the font size (%.2f)\n",
			(double)pixel_size);
		return 0;
	}
	/* **A SMOOTHED GLYPH IS THREE SAMPLES ALONG ONE SCREEN AXIS, WHICH ONLY MEANS ANYTHING WHILE THE GLYPH'S OWN
	 * VERTICALS ARE STILL VERTICAL ON THE SCREEN.** A rotation or a shear turns the sub-pixel axis away from the
	 * destination's, and the three samples then describe a direction nobody is looking along — the fringe would
	 * be a colour cast, not resolution. So a turned glyph falls back to grey coverage and says nothing, which is
	 * what Apple's own renderer does with turned text. The gate is on the transform the glyph is RASTERISED
	 * with — the text matrix composed with the CTM, which is what `matrix` is here. */
	if (smoothing && (matrix.b != 0.0 || matrix.c != 0.0)) {
		smoothing = 0;
	}
	/* A REFLECTION IS TAKEN OUT OF THE ENGINE'S MATRIX AND PUT BACK INTO THE ROWS, because a bitmap
	 * context's CTM is flipped and handing that flip to FreeType makes it report bitmap_top = 0 for
	 * every glyph (measured): the ink then lands at the pen and runs off the surface. */
	if (matrix.a * matrix.d - matrix.b * matrix.c < 0.0) {
		if (matrix.b != 0.0 || matrix.c != 0.0) {
			fprintf(stderr, "CG-REFUSE: a mirrored text matrix with a shear or rotation is not "
					"implemented; this library handles a pure reflection\n");
			return 0;
		}
		mirror = 1;
		matrix.d = -matrix.d;
	}
	m.xx = (FT_Fixed)(matrix.a * 65536.0);
	m.xy = (FT_Fixed)(matrix.b * 65536.0);
	m.yx = (FT_Fixed)(matrix.c * 65536.0);
	m.yy = (FT_Fixed)(matrix.d * 65536.0);
	/* THE DELTA IS THE SUBPIXEL FRACTION, and it is applied AFTER the matrix in 26.6 pixels — which is
	 * what makes a fractional pen land between pixels instead of being rounded away. THE Y IS NEGATED
	 * WHEN THE REFLECTION WAS: the delta lives in the engine's frame, and taking the bitmap context's
	 * flip out of the matrix (above) turned that frame's y around, so a device-space downward dy is an
	 * upward dy here. Getting this wrong is a glyph that moves the wrong way for half-pixel pens and
	 * sits still for whole ones, which the probe's fractional-y bracket check catches. */
	d.x = (FT_Pos)(delta.x * 64.0);
	d.y = (FT_Pos)((mirror ? -delta.y : delta.y) * 64.0);
	FT_Set_Transform(font->face, &m, &d);

	/* **THE TARGET IS THE RENDERER'S HALF OF SMOOTHING, AND IT IS NOT ONLY A RENDER MODE.** `FT_LOAD_TARGET_LCD`
	 * is what tells a font's own hinter that the grid it is fitting to is a THIRD of a pixel wide — asking for
	 * `FT_RENDER_MODE_LCD` alone gets the colour bitmap drawn from outlines that were grid-fitted for a whole-pixel
	 * grid, which is blurrier than the grey version it is supposed to improve on. */
	if (FT_Load_Glyph(font->face, (FT_UInt)glyph, smoothing ? (FT_LOAD_DEFAULT | FT_LOAD_TARGET_LCD)
							       : FT_LOAD_DEFAULT) != 0) {
		fprintf(stderr, "CG-REFUSE: the engine would not load glyph %u\n", (unsigned)glyph);
		return 0;
	}
	slot = font->face->glyph;
	if (FT_Render_Glyph(slot, smoothing ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL) != 0) {
		fprintf(stderr, "CG-REFUSE: the engine would not render glyph %u\n", (unsigned)glyph);
		return 0;
	}
	/* TWO COVERAGE FORMATS COME BACK AND BOTH ARE UNDERSTOOD: one sample per pixel, and — when smoothing was
	 * asked for — three, one per sub-pixel column. Anything else is a face this library cannot place. */
	if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD) {
		per_pixel = 3;
	} else if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
		fprintf(stderr, "CG-REFUSE: this font renders glyph coverage this library cannot use "
				"(pixel mode %d)\n", slot->bitmap.pixel_mode);
		return 0;
	}
	w = (int)slot->bitmap.width;
	h = (int)slot->bitmap.rows;
	buf = malloc((size_t)(w > 0 && h > 0 ? w * h : 1));
	if (buf == NULL) {
		return 0;
	}
	/* THE ROWS ARE **NOT** REVERSED WHEN THE FLIP WAS TAKEN OUT OF THE MATRIX, which is a correction
	 * the PICTURE made and no check in the probe did. FreeType's bitmap rows are TOP-DOWN whatever
	 * direction its outline ran, so after the un-flip they are ALREADY in the device's own order
	 * (y down) — reversing them beat the glyph twice and every letter came out vertically mirrored.
	 * The probe's `L` check passed on that upside-down picture, because a mirrored `L` still has its
	 * STEM crossing the baseline, which is all that check measured; it now compares two bands. */
	for (row = 0; row < h; row++) {
		memcpy(buf + (size_t)row * (size_t)w,
		       slot->bitmap.buffer + (size_t)row * (size_t)(slot->bitmap.pitch < 0
								    ? -slot->bitmap.pitch : slot->bitmap.pitch),
		       (size_t)w);
	}
	*coverage = buf;
	/* **`*width` IS A COUNT OF SAMPLES AND NOT OF PIXELS**, and `*samples` says how many of them make a pixel.
	 * `*left` IS IN WHOLE PIXELS FOR BOTH — MEASURED, and worth stating because the obvious guess is wrong: the
	 * engine reports an LCD bitmap's bearing in pixels exactly as it does a grey one's, and the finer grid lives
	 * entirely in the samples' own values. A caller that divided it by three would place every smoothed glyph in
	 * a different sub-column for every pen phase. */
	*width = w;
	*height = h;
	if (samples != NULL) {
		*samples = per_pixel;
	}
	if (left != NULL) {
		*left = slot->bitmap_left;
	}
	if (top != NULL) {
		*top = slot->bitmap_top;
	}
	if (advance != NULL) {
		*advance = slot->advance.x / 64.0;
	}
	(void)mirror;
	return 1;
}

/* ------------------------------------------------------------------------- */
/* The registry: a name, and the directories that can answer for it           */
/* ------------------------------------------------------------------------- */
/* THE INCLUDES RIDE WITH THE CODE THAT NEEDS THEM, which is a habit this file keeps: `dirent.h` is here
 * because a directory scan is here, and a reader does not have to look at the top of a 500-line file to
 * learn why. */
#include <dirent.h>
#include <strings.h>

/* **THE BUNDLED DEFAULT SET COMES FIRST, AND IT IS COMPILED IN RATHER THAN GUESSED.** The fonts Agora ships
 * live under `<prefix>/share/fonts/agora`; putting that directory first is what makes a fresh Agora install look
 * the same on every host instead of inheriting whichever families the distribution happened to install. A host
 * that already has a face of the same name still wins a TIE only if it is found first, and it is not — the walk
 * below keeps the FIRST of two equal-ranked matches, and this is the first directory. An administrator who wants
 * the host's copy can put it earlier through `FN_FONT_PATH`, which replaces this list entirely. */
#ifndef AGORA_FONT_DIR
#define AGORA_FONT_DIR "/usr/local/share/fonts/agora"
#endif

/* **THE FONT DIRECTORIES, AND THE MAC'S ARE NOT ENOUGH — WHICH WAS MEASURED THE HARD WAY.** This list was four
 * Mac-shaped paths, and on Linux NOT ONE OF THEM EXISTS, so the registry enumerated nothing, every face lookup
 * answered NULL, every string measured zero, and a menu was a column of empty rows three layers above the cause.
 * The Mac's paths stay, because this tree does run there; the Linux and FreeBSD ones are added, because that is
 * what Agora is for. THE TWO LAYOUTS DIFFER and the walk below is what reconciles them. */
static const char *fn_font_dirs[] = {
	/* Agora's own set, then this tree's staging and the Mac-shaped paths. */
	AGORA_FONT_DIR,
	"/System/Shared/Fonts",
	"/System/Library/Fonts",
	"/Library/Fonts",
	"/System/Fonts",
	/* Linux: the distribution's own, then the local administrator's. */
	"/usr/share/fonts",
	"/usr/local/share/fonts",
	/* FreeBSD, and the X11 tree it also keeps faces in. */
	"/usr/local/lib/X11/fonts",
	"/usr/X11R6/lib/X11/fonts",
};

/* **HOW DEEP THE WALK GOES, AND WHY IT MUST WALK AT ALL.** The Mac flattens its faces into the directories above,
 * so one listing is enough there. LINUX DOES NOT: `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` is TWO levels
 * below the root, and the level in between is a FORMAT directory — so a one-level listing finds `truetype`, which
 * is not a font, and nothing else. The depth is bounded rather than unlimited so that a symlink loop is a short
 * walk instead of a hang. */
#define FN_FONT_MAX_DEPTH 4

static int fn_has_font_suffix(const char *name)
{
	size_t n = strlen(name);

	if (n > 4 && (strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".otf") == 0)) {
		return 1;
	}
	return n > 4 && strcasecmp(name + n - 4, ".ttc") == 0;
}

/* DOES THIS FACE ANSWER TO THAT NAME? The POSTSCRIPT NAME and the FULL name are Apple's contract; the
 * family and `family style` forms are a kindness this library adds and the header says so — they are what
 * makes "DejaVu Sans" find DejaVuSans.ttf without the caller knowing the PostScript spelling. */
static int fn_face_is_named(FT_Face face, const char *name)
{
	char buf[256];
	const char *ps = FT_Get_Postscript_Name(face);

	if (ps != NULL && strcasecmp(ps, name) == 0) {
		return 1;
	}
	if (face->family_name != NULL && face->style_name != NULL) {
		snprintf(buf, sizeof buf, "%s %s", face->family_name, face->style_name);
		if (strcasecmp(buf, name) == 0) {
			return 1;
		}
	}
	if (face->family_name != NULL && strcasecmp(face->family_name, name) == 0) {
		return 1;
	}
	return face->style_name != NULL && strcasecmp(face->style_name, name) == 0;
}

static CGFontRef fn_try_file(const char *path, const char *name)
{
	CGDataProviderRef provider = CGDataProviderCreateWithFilename(path);
	CGFontRef font;
	char buf[256];
	int match;

	if (provider == NULL) {
		return NULL;
	}
	font = CGFontCreateWithDataProvider(provider);
	CGDataProviderRelease(provider);
	if (font == NULL) {
		return NULL;
	}
	match = fn_face_is_named(font->face, name);
	if (!match && cg_font_full_name(font, buf, sizeof buf) > 0 && strcasecmp(buf, name) == 0) {
		match = 1;
	}
	if (!match) {
		CGFontRelease(font);
		return NULL;
	}
	return font;
}

/* **THE RANK OF A MATCH, BECAUSE THE FIRST FACE THAT ANSWERS IS NOT THE BEST ONE.** MEASURED: asking for the
 * FAMILY `DejaVu Sans` answered `DejaVuSans-Bold`. Both files carry that family, so both match, and the walk
 * stopped at whichever the directory listed first — and `-` SORTS BEFORE `.`, so `DejaVuSans-Bold.ttf` is first
 * EVERY time and the bold face won EVERY time. An exact full-name match beats a family match, and a REGULAR face
 * beats a styled one; that is enough to pick the plain face of a family WITHOUT THIS CODE KNOWING ANY FOUNDRY'S
 * NAMING CONVENTIONS, which is the only kind of rule that belongs down here. */
static int fn_match_rank(CGFontRef font, const char *name)
{
	static const char *const style_words[] = { "bold", "italic", "oblique" };
	char full[256];
	size_t i;

	if (cg_font_full_name(font, full, (int)sizeof full) <= 0) {
		return 2;		/* it answers to the name and says nothing about a style: take it */
	}
	if (strcasecmp(full, name) == 0) {
		return 3;		/* the full name ITSELF was asked for */
	}
	for (i = 0; i < sizeof style_words / sizeof style_words[0]; i++) {
		const char *word = style_words[i];
		const char *p;

		for (p = full; *p != '\0'; p++) {
			if (strncasecmp(p, word, strlen(word)) == 0) {
				return 1;	/* a styled face of the family, which is only ever a fallback */
			}
		}
	}
	return 2;
}

/* The name door's descent. **IT KEEPS THE BEST MATCH RATHER THAN THE FIRST**, which is the whole of how it
 * differs from the enumeration below — and why the two are separate walks rather than one walk with a flag. */
static CGFontRef fn_create_name_in_directory(const char *directory, const char *name, int depth)
{
	DIR *d = opendir(directory);
	struct dirent *e;
	CGFontRef best = NULL;
	int best_rank = 0;

	if (d == NULL) {
		return NULL;		/* a file, or an unreadable directory: nothing to descend into */
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];
		CGFontRef found = NULL;
		int rank;

		if (e->d_name[0] == '.') {
			continue;	/* `.`, `..`, and anything hidden */
		}
		if (snprintf(path, sizeof path, "%s/%s", directory, e->d_name) >= (int)sizeof path) {
			continue;
		}
		if (fn_has_font_suffix(e->d_name)) {
			found = fn_try_file(path, name);
		} else if (depth > 0) {
			found = fn_create_name_in_directory(path, name, depth - 1);
		}
		if (found == NULL) {
			continue;
		}
		rank = fn_match_rank(found, name);
		if (rank > best_rank) {
			best_rank = rank;
			if (best != NULL) {
				CGFontRelease(best);
			}
			best = found;
		} else {
			CGFontRelease(found);
		}
	}
	closedir(d);
	return best;
}

/* **ONE NAME, ONE PASS OVER THE REGISTRY.** The registered files come first, then the directories, and the best
 * match is kept across directories rather than stopping at the first: a styled face found early must not end the
 * search when a plain one is sitting in the next directory along. This is the exact-name half only; the alias
 * fallback lives in the door below, so an exact match and a compatibility substitute are separate questions. */
static CGFontRef fn_search_exact(const char *name, const char *const *dirs, int ndirs)
{
	CGFontRef best = NULL;
	int i;

	/* **THE REGISTERED PATHS COME FIRST**, because a caller that has just registered a font file expects the
	 * name it registered to resolve to THAT file rather than to whatever a directory happens to hold under the
	 * same name — and a registration is the more specific statement of the two. */
	for (i = 0; i < fn_registered_count; i++) {
		CGFontRef font = fn_try_file(fn_registered[i], name);

		if (font != NULL) {
			return font;
		}
	}
	/* **AND THE BEST MATCH IS KEPT ACROSS DIRECTORIES TOO**, not just within one. */
	for (i = 0; i < ndirs; i++) {
		CGFontRef font = fn_create_name_in_directory(dirs[i], name, FN_FONT_MAX_DEPTH);

		if (font == NULL) {
			continue;
		}
		if (best == NULL || fn_match_rank(font, name) > fn_match_rank(best, name)) {
			if (best != NULL) {
				CGFontRelease(best);
			}
			best = font;
		} else {
			CGFontRelease(font);
		}
		if (fn_match_rank(best, name) >= 3) {
			break;		/* the full name itself was asked for; nothing can beat that */
		}
	}
	return best;
}

CGFontRef cg_font_create_with_name(const char *name)
{
	char override_buf[1024];
	const char *dirs[16];
	CGFontRef best = NULL;
	int ndirs = 0;
	int i;

	if (name == NULL || name[0] == '\0') {
		return NULL;
	}
	/* THE CACHE FIRST, and the override is watched one line before it: a probe that changes FN_FONT_PATH
	 * between two calls of the same name must see the directory it just asked for, and that variable is
	 * the only thing this function reads besides the fonts themselves. */
	fn_name_cache_note_env();
	{
		CGFontRef cached = fn_name_cache_get(name);

		if (cached != NULL) {
			return cached;
		}
		if (fn_name_cache_missed(name)) {
			return NULL;	/* asked and answered: nothing by this name is installed */
		}
	}
	{
		/* THE OVERRIDE IS THE PROBE'S DOOR IN, AND THE ONLY WAY TO SEARCH ELSEWHERE: this library has no
		 * settings domain of its own — system.fonts.conf belongs to the X11 stack's fontconfig, and
		 * reading it here would put libconfig inside CoreGraphics. */
		const char *env = getenv("FN_FONT_PATH");

		if (env != NULL && env[0] != '\0' && strlen(env) < sizeof override_buf) {
			char *p;

			strcpy(override_buf, env);
			p = strtok(override_buf, ":");
			while (p != NULL && ndirs < 16) {
				dirs[ndirs++] = p;
				p = strtok(NULL, ":");
			}
		}
	}
	if (ndirs == 0) {
		for (i = 0; i < (int)(sizeof fn_font_dirs / sizeof fn_font_dirs[0]); i++) {
			dirs[ndirs++] = fn_font_dirs[i];
		}
	}
	best = fn_search_exact(name, dirs, ndirs);
	/* **AND ONLY IF THE NAME ITSELF IS ABSENT DOES THE macOS COMPATIBILITY TABLE SPEAK.** This ordering is the
	 * whole contract: a host that really has `Helvetica` answers with its own `Helvetica`, and the substitute is
	 * what a fresh Agora install gets instead of NULL. A substitute is a FAMILY or a FULL NAME, so a request for
	 * `Helvetica-Bold` can land on a bold face rather than on the regular one. */
	if (best == NULL) {
		const char *substitute = cg_font_substitute_family(name);

		if (substitute != NULL && strcasecmp(substitute, name) != 0) {
			best = fn_search_exact(substitute, dirs, ndirs);
		}
	}
	if (best != NULL) {
		/* THE CACHE TAKES A REFERENCE OF ITS OWN AND THE CALLER STILL GETS ITS +1: this door Creates, and a font
		 * that came from the cache is no exception to that. */
		fn_name_cache_put(name, best);
	} else {
		fn_name_cache_put(name, NULL);	/* AND THE MISS IS THE ENTRY THAT PAYS FOR ITSELF */
	}
	return best;
}

/* ------------------------------------------------------------------------- */
/* Every face, for a caller that wants them all                              */
/* ------------------------------------------------------------------------- */

/* THE SAME DIRECTORY RESOLUTION THE DOOR ABOVE DOES, and deliberately a separate copy of those ten lines
 * rather than a helper both call: the CREATE door's loop must return the moment a name matches while this one
 * must visit every file, so the two loops share their INPUT (which directories, and the override) and nothing
 * else. What they must not share is a second copy of the directory LIST, and they do not — `fn_font_dirs` is
 * read here exactly as it is there. */
/* The enumerator's descent: EVERY face under a directory, which is the other half of the pair above. */
static void fn_enumerate_directory(const char *directory, cg_font_visit visit, void *info, int depth)
{
	DIR *d = opendir(directory);
	struct dirent *e;

	if (d == NULL) {
		return;
	}
	while ((e = readdir(d)) != NULL) {
		char path[1024];

		if (e->d_name[0] == '.') {
			continue;
		}
		if (snprintf(path, sizeof path, "%s/%s", directory, e->d_name) >= (int)sizeof path) {
			continue;
		}
		if (fn_has_font_suffix(e->d_name)) {
			visit(info, path);
			continue;
		}
		if (depth > 0) {
			fn_enumerate_directory(path, visit, info, depth - 1);
		}
	}
	closedir(d);
}

void cg_font_enumerate(cg_font_visit visit, void *info)
{
	const char *dirs[16];
	char override_buf[1024];
	int ndirs = 0;
	size_t i;

	if (visit == NULL) {
		return;
	}
	{
		const char *env = getenv("FN_FONT_PATH");

		if (env != NULL && env[0] != '\0' && strlen(env) < sizeof override_buf) {
			char *p;

			strcpy(override_buf, env);
			p = strtok(override_buf, ":");
			while (p != NULL && ndirs < 16) {
				dirs[ndirs++] = p;
				p = strtok(NULL, ":");
			}
		}
	}
	if (ndirs == 0) {
		for (i = 0; i < sizeof fn_font_dirs / sizeof fn_font_dirs[0]; i++) {
			dirs[ndirs++] = fn_font_dirs[i];
		}
	}
	for (i = 0; i < (size_t)ndirs; i++) {
		fn_enumerate_directory(dirs[i], visit, info, FN_FONT_MAX_DEPTH);
	}
	/* **AND THE REGISTERED PATHS LAST, which is the other half of "one question, several inputs"**: a caller
	 * asking what fonts exist is asking about the registry too, and CT-M7a's proof — that the URL, family and
	 * PostScript lists are ONE list seen three ways — only holds if this walk is the one place they come from.
	 * LAST rather than first because the directories are what the system ships and a registration is an
	 * addition to them. */
	for (i = 0; i < (size_t)fn_registered_count; i++) {
		visit(info, fn_registered[i]);
	}
}

/* ------------------------------------------------------------------------- */
/* The outline: the same glyph as geometry                                    */
/* ------------------------------------------------------------------------- */
#include FT_OUTLINE_H

struct fn_outline_ctx {
	const cg_outline_sink *sink;
	void *info;
	CGAffineTransform m;
};

/* FONT UNITS FROM THE ENGINE, THE EM, THEN THE TEXT MATRIX — AND NO Y FLIP, because these points end up in
 * a CGPath that the drawing machinery transforms with the CTM itself. That is the whole difference from the
 * mask route, which must un-flip the engine's frame because it bypasses the CTM. */
static void fn_outline_point(struct fn_outline_ctx *o, const FT_Vector *v, CGFloat *x, CGFloat *y)
{
	/* NO DIVISION: `FT_LOAD_NO_SCALE` means these points are ALREADY in font units. (The 26.6 convention
	 * belongs to a SCALED outline — the mask route's, which asks the engine to size the glyph. Dividing
	 * here as well shrinks the glyph by that factor and then the em shrinks it again, leaving a
	 * half-pixel path: the first run of this measured a 32pt `A` stroked to FIVE pixels at the pen's own
	 * bottom edge, and a clip with no area at all.) */
	CGPoint p = CGPointApplyAffineTransform(CGPointMake((CGFloat)v->x, (CGFloat)v->y), o->m);

	*x = p.x;
	*y = p.y;
}

static int fn_decompose_move(const FT_Vector *to, void *user)
{
	struct fn_outline_ctx *o = user;
	CGFloat x, y;

	fn_outline_point(o, to, &x, &y);
	o->sink->move_to(o->info, x, y);
	return 0;
}

static int fn_decompose_line(const FT_Vector *to, void *user)
{
	struct fn_outline_ctx *o = user;
	CGFloat x, y;

	fn_outline_point(o, to, &x, &y);
	o->sink->line_to(o->info, x, y);
	return 0;
}

static int fn_decompose_conic(const FT_Vector *control, const FT_Vector *to, void *user)
{
	struct fn_outline_ctx *o = user;
	CGFloat cx, cy, x, y;

	fn_outline_point(o, control, &cx, &cy);
	fn_outline_point(o, to, &x, &y);
	if (o->sink->conic_to != NULL) {
		o->sink->conic_to(o->info, cx, cy, x, y);
	}
	return 0;
}

static int fn_decompose_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user)
{
	struct fn_outline_ctx *o = user;
	CGFloat c1x, c1y, c2x, c2y, x, y;

	fn_outline_point(o, c1, &c1x, &c1y);
	fn_outline_point(o, c2, &c2x, &c2y);
	fn_outline_point(o, to, &x, &y);
	if (o->sink->cubic_to != NULL) {
		o->sink->cubic_to(o->info, c1x, c1y, c2x, c2y, x, y);
	}
	return 0;
}

int cg_font_glyph_outline(CGFontRef font, CGGlyph glyph, CGFloat pixel_size, CGAffineTransform matrix,
			  const cg_outline_sink *sink, void *info)
{
	static const FT_Outline_Funcs funcs = {
		fn_decompose_move, fn_decompose_line, fn_decompose_conic, fn_decompose_cubic, 0, 0
	};
	struct fn_outline_ctx ctx;
	FT_GlyphSlot slot;
	CGFloat em;

	if (font == NULL || font->face == NULL || sink == NULL || pixel_size <= 0.0) {
		return 0;
	}
	/* NO SIZE IS SET: `FT_LOAD_NO_SCALE` below asks for the outline in FONT UNITS, which is the design
	 * outline — a hinted or grid-fitted one is a rasterisation choice, right for a mask and wrong for
	 * geometry. The em is applied to the points here instead. */
	if (FT_Load_Glyph(font->face, (FT_UInt)glyph, FT_LOAD_NO_SCALE) != 0) {
		fprintf(stderr, "CG-REFUSE: the engine would not load glyph %u for its outline\n",
			(unsigned)glyph);
		return 0;
	}
	slot = font->face->glyph;
	if (slot->outline.n_points == 0) {
		/* NO OUTLINE IS SUCCESS: a space has none, and so does a bitmap-only face. */
		return 1;
	}
	em = pixel_size / (CGFloat)font->face->units_per_EM;
	ctx.sink = sink;
	ctx.info = info;
	ctx.m = CGAffineTransformConcat(CGAffineTransformMakeScale(em, em), matrix);
	if (FT_Outline_Decompose(&slot->outline, &funcs, &ctx) != 0) {
		return 0;
	}
	if (sink->close_path != NULL) {
		sink->close_path(info);
	}
	return 1;
}
