/*
 * CGFont_internal — the seam between a font and the engine that owns it.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * NOT INSTALLED, NOT INCLUDED BY ANY PUBLIC HEADER, and named the way CGColorSpace_internal.h is
 * for the same reason: a `CGFont` is FreeType's face plus the bookkeeping Apple's API needs, and
 * whoever draws a glyph should be able to ask for the face WITHOUT INCLUDING `<ft2build.h>`.
 *
 * THE HANDLE IS `void *` AND NOT `FT_Face`, DELIBERATELY — the same rule the colour space's
 * `cmsHPROFILE` accessor follows: the value passes through both ways without a cast at the seam,
 * and no header that includes this one inherits FreeType's include layout (the engine's headers
 * live under an `freetype2/` subdirectory of the X prefix, which is exactly the kind of detail a
 * public header may not force on its includers).
 *
 * THE STRING HELPERS ARE C STRINGS, NOT STRING OBJECTS, for the same reason the colour space names' are:
 * CGFont.c does not own a string type. CGFontNames.c turns them into `CFStringRef` at the doors whose
 * Apple signatures return one. (This said "NOT `NSString *` ... turns them into `NSString *`" — the
 * distinction the paragraph draws is C STRINGS against STRING OBJECTS, and the object type on the far
 * side is Core Foundation's now.)
 */
#ifndef CORE_GRAPHICS_CGFONT_INTERNAL_H
#define CORE_GRAPHICS_CGFONT_INTERNAL_H

#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGAffineTransform.h>
#include <stddef.h>

/* THE ENGINE'S FACE, OR NULL. NULL is not a gap: it is what a font with no engine representation
 * answers, and today the only `CGFont` that exists at all is one FreeType opened. A caller that
 * gets NULL has nothing to draw and must skip. */
void *cg_font_face(CGFontRef font);

/* THE POSTSCRIPT NAME, OR NULL — FreeType's own string, borrowed from the face, valid while the
 * font lives. Nullable because a face is allowed not to have one. */
const char *cg_font_postscript_name(CGFontRef font);

/* THE FULL NAME INTO `buf` (UTF-8), FROM THE `name` TABLE: the answer is the number of bytes
 * written without the terminator, 0 when the face has no such name. A caller-provided buffer
 * rather than a borrowed pointer because the name table's string is UTF-16BE and has to be
 * converted, and this library does not hand out a static buffer. */
int cg_font_full_name(CGFontRef font, char *buf, size_t size);

/* THE `name` TABLE'S ENTRY FOR ONE nameID, WHICH `cg_font_full_name` ABOVE IS A SPECIAL CASE OF.
 * Core Text's `CTFontCopyName` takes a KEY rather than a number — Apple's keys are opaque strings —
 * so the mapping from key to nameID has to live at the caller: THIS DOOR SPEAKS NUMBERS. The entry
 * chosen is the one `cg_font_full_name` has always chosen (the Windows Unicode-BMP entry first, then
 * the first Macintosh one), WITH THE MAC FALLBACK'S CAVEAT STATED BECAUSE IT IS VISIBLE HERE: a
 * platform-1 string is MacRoman, not UTF-16, and it is copied byte-for-byte as it always has been.
 * A face with no such entry answers 0 and writes nothing. */
int cg_font_name_entry(CGFontRef font, int name_id, char *buf, size_t size);

/* THE RAW BYTES OF ONE TABLE, BY ITS FOUR-CHARACTER TAG -- and this is the door every table-reading answer on the Core
 * Text side waits for. THE CONTRACT IS TWO CALLS AND IT HAS TO BE, because a table's size is the TABLE'S business: ask
 * with `buf == NULL` and the answer is the number of bytes the table holds; ask again with a buffer and the answer is
 * the same number, with as many of those bytes written as fit. A buffer that is TOO SMALL IS NOT AN ERROR for the same
 * reason -- the answer still says how much the table holds.
 *
 * A FACE THAT HAS NO SUCH TABLE, OR IS NOT AN SFNT AT ALL (a bare CFF, a bitmap face), ANSWERS 0. A table of zero
 * length would answer 0 too, and the distinction is not offered: a zero-length table says nothing, which is what a
 * missing one says.
 *
 * AND IT ANSWERS THE FILE'S OWN BYTES WITH NO INTERPRETATION: the Core Text side decides what a `post` header or an
 * `fvar` axis record means, and this door is the bytes rather than an opinion about them. */
int cg_font_copy_table(CGFontRef font, uint32_t tag, unsigned char *buf, size_t size);

/* AND THE TAGS THE FACE CARRIES, IN THE ORDER ITS DIRECTORY LISTS THEM. Same two-call contract: `tags == NULL` answers
 * how many there are, and a buffer answers with the tags and the same count. A FACE WITHOUT A TABLE DIRECTORY ANSWERS 0
 * rather than an error, for the same reason the door above does. */
int cg_font_table_tags(CGFontRef font, uint32_t *tags, size_t capacity);

/* THE GLYPH'S NAME INTO `buf`, same contract, 0 when the face carries no glyph names at all —
 * which is the common case for a subset or a CFF face. */
int cg_font_glyph_name(CGFontRef font, CGGlyph glyph, char *buf, size_t size);

/* AND THE OTHER WAY: the glyph index for a name, which is how a caller with a name (or the
 * PostScript encoding) reaches a glyph without a character map. `kCGFontIndexInvalid` = not found. */
CGGlyph cg_font_glyph_with_name(CGFontRef font, const char *name);

/* A BYTE TO A GLYPH THROUGH THE FONT'S OWN CHARACTER MAP, which is what the string-drawing doors
 * need and what a caller without a character map cannot do for itself. THE BYTE IS READ AS A
 * CHARACTER CODE, not as a glyph index: this library implements the font-specific encoding that way
 * and SAYS SO (see CGContext.h's note on CGTextEncoding), because the alternative — byte as glyph
 * index — gives garbage for every font whose glyph order is not ASCII-aligned, which is all of
 * them. A byte this font has no glyph for answers kCGFontIndexInvalid, which the caller SKIPS
 * rather than drawing .notdef for. */
CGGlyph cg_font_glyph_for_byte(CGFontRef font, unsigned char code, int macroman);
/* THE UNICODE STEP, BESIDE THE BYTE ONE, AND IT IS HERE BECAUSE CORE TEXT OWNS THAT STEP AND CANNOT GET IT
 * FROM THE PUBLIC HEADERS. Apple deprecated Core Graphics' public glyph-mapping doors, so this library struck
 * them, and the step lives in Core Text instead -- CTFontGetGlyphsForCharacters. THAT DOOR NEEDS THE FACE'S
 * cmap, AND THE FACE IS HERE, so the mapping is answered here and Core Text keeps its own two dependencies
 * rather than reaching for a font engine of its own.
 *
 * IT IS AN INTERNAL DOOR, WHICH IS WHY ADDING IT DOES NOT REOPEN THE ERA QUESTION THIS LIBRARY ALREADY
 * SETTLED: what was struck is a DEPRECATED PUBLIC API, not the engine's ability to map a character.
 *
 * **AND A CHARACTER OUTSIDE THE BMP IS TWO UNITS AND ONE GLYPH.** The array is UTF-16, so an emoji arrives as a
 * surrogate pair; the pair is combined here, occupies the HIGH surrogate's slot, leaves 0 in the low one, and
 * counts as two mapped units — because the return value is how callers ask "did every character map", and a pair
 * that mapped is not half-unmapped. Mapping the units separately, which is what this door used to do, asks the
 * cmap for U+D83D: NO font has that, so every astral character in the tree was uncovered and nothing could fall
 * back to the one font that has them. */
int cg_font_glyphs_for_unichars(CGFontRef font, const uint16_t *characters, CGGlyph *glyphs, size_t count);

/* A FONT BY NAME, FROM THE CONFIGURED FONT DIRECTORIES: the door Apple calls
 * `CGFontCreateWithFontName` needs a REGISTRY — a name has to resolve to a file — and this is this
 * library's: the directories are the FSH ones below, a name matches the face's POSTSCRIPT name, its
 * FULL name or `family style` (case-insensitively, which is a kindness Apple's documentation does not
 * promise), and a name that matches nothing ANSWERS NULL rather than substituting a font the caller did
 * not ask for. `FN_FONT_PATH` overrides the directory list with a colon-separated one, which is how the
 * host probe reaches the tree's own directory. */
CGFontRef cg_font_create_with_name(const char *name);

/* **THE macOS COMPATIBILITY NAME, OR NULL.** `cg_font_create_with_name` asks this door ONLY after its exact
 * search comes back empty, and searches for whatever it answers. The table lives in `CGFontAliases.c`, where the
 * choices and their licences are recorded; a name that is not in it answers NULL, which is the same answer an
 * unknown name got before the table existed. The result is a FAMILY or a FULL NAME, and the caller does not
 * need to know which — it is searched exactly as a caller's own name would be. */
const char *cg_font_substitute_family(const char *name);

/* **AND THE OTHER DIRECTION: EVERY FACE, ONE AT A TIME, BY PATH.** The door above stops at the FIRST name
 * that matches, so walking the same directories to report ALL of them is a DIFFERENT LOOP rather than a copy
 * of it — and it lives in the same file so that the list of directories a font may live in (and the
 * `FN_FONT_PATH` override) stays in ONE place. The visitor is handed the PATH, because the caller's next step
 * is its own: Core Text makes a data provider from it (`CTFontCollection` is the caller today).
 *
 * NO NAME IS RESOLVED HERE: a path is a path, and whether it names a face is the caller's question to ask
 * (`CGFontCreateWithDataProvider` answers it). That is also why this door cannot be wrong about *matching* —
 * it does not match. */
typedef void (*cg_font_visit)(void *info, const char *path);
void cg_font_enumerate(cg_font_visit visit, void *info);

/* **AND A NUMBER THAT CHANGES WHEN THE FONT SET DOES**, so a caller that wants to cache what `cg_font_enumerate` answers
 * can tell whether its copy is still true. It moves on a registration, an unregistration, and `FN_FONT_PATH` changing —
 * the three things this file knows of that change which faces exist — and it never moves back. */
unsigned long cg_font_registry_generation(void);

/* **THE PROCESS'S REGISTERED FONTS (CT-M7h).** `CTFontManagerRegisterFontsForURL` adds a font FILE to the
 * process's registry, and the two doors above have to see it — that is what makes a registered font
 * *discoverable* rather than merely remembered. So the registry lives HERE, beside the directory list it joins,
 * for the reason CT-M5a recorded one framework over: "what they must not share is a second copy of the
 * directory LIST", and a registration is a third input to the same question rather than a second answer.
 *
 * THE LIST IS THE LIBRARY'S AND THE NUMBERS ARE NOT: a path is registered once (registering it twice is the
 * `AlreadyRegistered` case, which the CALLER's vocabulary names), and unregistering is exact. The storage grows,
 * because a bounded table would need an invented refusal for the full case and the era's error vocabulary has
 * no code for one. */
int cg_font_register(const char *path);
int cg_font_unregister(const char *path);
int cg_font_is_registered(const char *path);

/* ONE GLYPH RASTERISED ... see CGContext.c for how it is placed. */
/* **`pixel_size` IS DEVICE PIXELS PER EM, WHICH IS NOT THE FONT SIZE.** A font size is in points and the
 * transform is what turns points into pixels, so the caller derives this from its own transform and hands the
 * rasteriser a GRID rather than a size (see CGContext.c's `cg_show_one_glyph_fill`). That is the whole of the
 * correction: the engine hints against the size it is given, and `matrix` — a rotation and/or shear with the
 * scale divided out — reaches it only AFTER hinting.
 *
 * `delta` IS THE SUBPIXEL FRACTION OF THE PEN, IN DEVICE UNITS (x right, y down — the engine's own
 * frame is handled inside). A whole-pixel caller passes (0,0) and gets the old behaviour; a caller
 * that has been asked not to subpixel-position passes (0,0) too, because rounding the pen IS that
 * setting.
 *
 * `smoothing` ASKS FOR SUB-PIXEL COVERAGE — the colour antialiasing Mac OS X is remembered for — and
 * THE ANSWER SAYS WHAT WAS RENDERED rather than what was asked for: `*samples` comes back 3 when the
 * coverage is three samples to the pixel and 1 when it is grey, because a glyph the engine cannot turn
 * (a rotated one) is rendered grey and the caller has to blend what it actually got. With 3, the
 * bitmap is `*width` SAMPLES wide — three to a pixel, in sub-pixel order — and `*left` is in PIXELS,
 * as it is with 1.
 *
 * **AND `*samples` COMES BACK 4 FOR A COLOUR GLYPH, WHICH IS A PICTURE RATHER THAN A MASK.** The bytes are
 * `*width * *height * 4` PREMULTIPLIED BGRA, blue first, with `*width` counted in PIXELS; the caller must not
 * tint it and must not treat it as coverage (see CGContext.c's colour composite). It is not a smoothing answer —
 * a colour glyph is never sub-pixel antialiased — which is why `cg_font_has_colour` exists: the caller can ask the
 * face before the render, which keeps a flag that cannot matter out of the glyph cache's key.
 *
 * A FACE WITH NO OUTLINES IS DRAWN FROM ITS STRIKE: the strike is selected, the picture is scaled from the
 * strike's em to `pixel_size`, and `*left`, `*top` and `*advance` are scaled with it. At a size the strike
 * happens to match, nothing is scaled and the numbers are the engine's own.
 *
 * `*advance` IS IN DEVICE PIXELS AT THE SIZE GIVEN, and no caller places glyphs with it: the layout's advances are
 * Core Text's, taken from the font's own metrics and unhinted (see CGContext.h). It is answered for the doors that
 * ask the engine rather than kept in step with a pen.
 *
 * `ink` IS THE COVERAGE WARP'S POLARITY, and it is the caller's answer because the caller is the one that knows
 * the fill colour: `CG_FONT_INK_NONE` leaves the coverage exactly as the engine rendered it, and the two
 * polarities ask for the same warp at the strength that side of the contrast needs (`cg_font_coverage_warp`).
 * IT IS APPLIED HERE, BEFORE THE CALLER BLENDS, because it is a property of the MASK and not of one compositing
 * path: grey coverage is handed to pixman and sub-pixel coverage to a hand blend, so a warp applied at either
 * of those would leave the other one out. */
enum {
	CG_FONT_INK_NONE = 0,
	CG_FONT_INK_DARK = 1,
	CG_FONT_INK_LIGHT = 2
};

/* **DOES THIS FACE HAVE COLOUR GLYPHS?** `FT_HAS_COLOR`, answered here so a caller decides *before* rendering
 * rather than from the sample count afterward: the two things a caller does differently are (a) ask for grey
 * rather than sub-pixel coverage and (b) paint the result instead of blending a mask, and only the first one has to
 * be known in advance — it is part of the glyph cache's key, so learning it from the render would key one picture
 * twice. A face can mix colour and ordinary glyphs; the flag is the face's, and the per-glyph answer is `*samples`
 * coming back 4. */
int cg_font_has_colour(CGFontRef font);

int cg_font_render_glyph(CGFontRef font, CGGlyph glyph, CGFloat pixel_size,
                         CGAffineTransform matrix, CGPoint delta, int smoothing, int ink,
                         unsigned char **coverage, int *width, int *height, int *left, int *top,
                         double *advance, int *samples);

/* **THE SAME RENDERING, REMEMBERED.** Same arguments and the same meaning for every out-parameter, with one
 * difference that a caller has to know: `*coverage` is a BORROWED pointer into the cache and must not be freed.
 * The bytes stay valid until the next call into this door (or `cg_font_glyph_cache_flush`, which drops everything),
 * which is what a drawing pass wants — every glyph it renders is consumed before the next one is asked for.
 *
 * It exists because `cg_font_render_glyph` sets the face size, rasterises and warps PER CALL, and a page of text or
 * a terminal's grid asks for the same few dozen pictures thousands of times. The key is the engine's own input in the
 * engine's own precision (face, glyph, 26.6 device size, four 16.16 matrix factors, two 26.6 deltas, smoothing, ink
 * polarity), so a hit is a rendering the engine would have reproduced byte for byte. A REFUSAL IS NOT CACHED: the
 * renderer's zero comes back as a zero and is retried next time, which keeps its diagnostic on stderr.
 *
 * ALLOCATION FAILURE IS A REFUSAL rather than a half-kept rendering: the door's promise is a pointer the caller must
 * not free, so there is nowhere to put a picture that could not be recorded, and handing one over unrecorded would
 * leak it. It answers zero and says so, like every other refusal in this file. */
int cg_font_render_glyph_cached(CGFontRef font, CGGlyph glyph, CGFloat pixel_size,
                                CGAffineTransform matrix, CGPoint delta, int smoothing, int ink,
                                const unsigned char **coverage, int *width, int *height, int *left,
                                int *top, double *advance, int *samples);

/* **THE CACHE'S THREE PROBE DOORS.** `set_budget` replaces the `CG_GLYPH_CACHE_BYTES` default (bytes; 0 turns
 * caching off but NOT the borrow contract — every rendering then lives in the one-slot holder), `flush` empties the
 * cache, and `stats` answers what is in it now and what it has done since the last flush. A flush zeroes the
 * counters as well, because "forget what you know" and "how hard did you work" are the two questions a probe asks
 * and the second is unanswerable after the first if the counts survive. */
typedef struct {
	unsigned long hits;
	unsigned long misses;
	unsigned long evictions;
	unsigned entries;
	size_t bytes;
	size_t budget;
} CGGlyphCacheStats;

void cg_font_glyph_cache_set_budget(size_t bytes);
void cg_font_glyph_cache_flush(void);
void cg_font_glyph_cache_stats(CGGlyphCacheStats *stats);

/* **THE ADVANCE CACHE, WHICH IS A DIFFERENT SHAPE FROM THE BITMAP ONE AND FOR A REASON WORTH STATING.** An advance
 * is in FONT UNITS — `CGFontGetGlyphAdvances` says so — so it is the same answer at every size, pen phase and
 * transform, and the cache is one `int` per glyph with a byte beside it saying whether it is known. It lives ON THE
 * FONT (allocated on the first advance question, freed with the font) rather than in the bitmap cache's LRU, because
 * a picture is worth hundreds of bytes and there are a few dozen of them while an advance is worth four and there is
 * one per glyph.
 *
 * It is INVISIBLE AT THE DOOR: `CGFontGetGlyphAdvances` still answers font units, still refuses a batch whole when a
 * glyph will not load, and still leaves the caller's array untouched in that case. What changed is the cost — the
 * second ask is an array read instead of `FT_Load_Glyph`, MEASURED at 713 ns against 15 ns for a 13 px glyph.
 *
 * The counters are per font, and `known` is how many glyphs that font has an answer for, so a probe can tell a warm
 * cache from one that never records anything. */
typedef struct {
	unsigned long loads;	/* glyphs this font asked the engine about */
	unsigned long hits;	/* answers taken from the array instead */
	unsigned known;		/* how many glyphs the array has an advance for */
	unsigned capacity;	/* the face's glyph count, once the array exists */
} CGFontAdvanceStats;

void cg_font_advance_cache_stats(CGFontRef font, CGFontAdvanceStats *stats);

/* **THE COVERAGE WARP'S EXPONENT, ASKED AS A NUMBER SO THAT IT CAN BE MEASURED RATHER THAN TRUSTED.** `ppem` is
 * DEVICE pixels per em and `ink` is one of the two polarities above; the answer is the exponent
 * `cg_font_render_glyph` raises each coverage sample to — 1.0 at and above the size where an antialiased stem is
 * thick enough not to thin, and lower (for BOTH polarities, which is the non-obvious half: see the definition)
 * the smaller the text. */
double cg_font_coverage_warp(double ppem, int ink);

/* **THE LCD FILTER THE RASTERISER CHOOSES FOR A DEVICE SIZE, ASKED THE SAME WAY.** The engine's five-tap FIR is
 * the right convolution of a monitor's phosphors and the three-tap is the right one for a stem that is barely a
 * pixel wide (the measurement is in CGFont.c); the answer is a `FT_LcdFilter` value, which this header states as
 * an int so that nothing outside the one file includes `<ft2build.h>`. */
int cg_font_lcd_filter_for(double ppem);

/* **A UNICODE CODEPOINT'S GLYPH, WHICH IS THE ENGINE'S CHARACTER MAP AND NOT A NAME LOOKUP.** It is the door a
 * PDF reader needs for a simple font whose /Encoding is a standard one: the encoding maps a byte to a character,
 * and the character maps through the font's own cmap. `kCGFontIndexInvalid` when the face has no glyph for it.
 * The public `CGFontGetGlyphsForUnichars` is still owed; this is the primitive it will be built on. */
CGGlyph cg_font_glyph_for_unichar(CGFontRef font, unsigned unichar);

/* **A CODE IN THE FACE'S OWN BUILT-IN ENCODING — which for a Type 1 font is NOT its Unicode char map.** FreeType
 * synthesises a Unicode char map from a Type 1 face's glyph names, and that map is what `cg_font_glyph_for_unichar`
 * reads; a TeX font's built-in encoding puts `ff`, `fi`, `fl`, `ffi` and `ffl` in the low slots, so asking the
 * Unicode map about code 12 finds nothing while the font's own Adobe encoding names a ligature there. This door
 * asks the Adobe charmaps FreeType exposes for exactly this, restores the caller's char map, and answers
 * `kCGFontIndexInvalid` when no such map exists (a TrueType face has none, and its codes go through the cmap). */
CGGlyph cg_font_glyph_for_code(CGFontRef font, unsigned code);

/* **THE FONT PROGRAM ITSELF, FOR A CALLER THAT MUST EMBED IT.** The provider is the one the face was opened
 * from — a file for `CGFontCreateWithFontName`, the caller's bytes for `CGFontCreateWithDataProvider` — and it is
 * retained for the caller. NULL for a face with no program behind it. */
CGDataProviderRef cg_font_copy_provider(CGFontRef font);

/* **WHETHER THIS FACE'S GLYPHS ARE `glyf` OUTLINES**, which is the question an embedder asks before writing a
 * `/FontFile2`: TrueType and OpenType-with-TrueType-outlines say yes; a CFF/Type 1 face says no, because its
 * program is not something a CIDFontType2 may point at. */
bool cg_font_has_truetype_outlines(CGFontRef font);

/* **GLYPH INDEX → UNICODE, WHICH IS WHAT A /ToUnicode CMap NEEDS.** Fills `map[glyph]` for as many glyphs as the
 * face's Unicode char map names, leaving the rest zero, and answers how many it filled. The char map is restored
 * afterwards. A glyph with no Unicode (a ligature the cmap does not name) stays zero and is simply absent from the
 * CMap, which is the honest answer rather than a guess. */
size_t cg_font_unicode_map(CGFontRef font, unsigned *map, size_t count);

/* **THE CFF PROGRAM AND ITS DIALECT.** A CFF face can be embedded as a PDF Type 1 font when it is NOT CID-keyed;
 * `cg_font_copy_cff_program` returns the bare `CFF ` table (from an OpenType wrapper) or the provider's bytes (a
 * bare CFF file), and the two questions say whether that route is legal for this face. */
bool cg_font_is_cff(CGFontRef font);
bool cg_font_is_cid_keyed(CGFontRef font);
CFDataRef cg_font_copy_cff_program(CGFontRef font) CF_RETURNS_RETAINED;

/* **THE PAIR ADJUSTMENT BETWEEN TWO GLYPHS, IN FONT UNITS, OR ZERO.** This is the `kern` table, which is what a
 * font's typographer wrote about specific pairs — `To`, `AV`, `P.` — as opposed to the per-glyph advance every
 * other door here answers. It exists because a LINE has to apply it: Core Text's typesetting kerns, and without
 * this the positions a line hands a drawing door are the unkerned ones, which is measurable as daylight between
 * letters that were designed to touch.
 *
 * THE ENGINE READS THE `kern` TABLE ONLY. A face whose kerning is in `GPOS` (which is most modern text faces and
 * many old ones) answers zero, and this door cannot tell that face from one with no kerning at all — the
 * distinction needs a shaper this library does not have. */
int cg_font_kern(CGFontRef font, CGGlyph left, CGGlyph right);

/* **AND THE READER FOR THE PAIRS A MODERN FACE KEEPS IN `GPOS`**, which is what `cg_font_kern` falls back to when
 * the face has no `kern` table — the case for the typeface this desktop draws with. The bytes are the whole GPOS
 * table, the answer is in font units like every other metric here, and a table this file cannot make sense of
 * answers 0. See `CGGPosKern.c` for how much of OpenType it does and does not read. */
int cg_gpos_kern(const unsigned char *gpos, size_t length, unsigned left, unsigned right);

/* AND THE SAME GLYPH AS GEOMETRY, which is what stroking and clipping need — and what a PDF context
 * and Core Text need too. The outline is decomposed into moves, lines, quadratics and cubics, and
 * CONIC SEGMENTS ARE PASSED THROUGH AS CONICS because a CGPath holds quadratics: converting them here
 * would be a lossy detour through a layer that can say what the engine said.
 *
 * THE POINTS ARRIVE IN USER SPACE, RELATIVE TO THE PEN (the em is `pixel_size`, the text matrix is
 * applied), so the caller adds its pen and hands the path to the drawing machinery, WHICH APPLIES THE
 * CTM ITSELF — which is why this route needs NO MIRRORING, unlike the mask route, which must un-flip
 * the engine's frame because it bypasses the CTM. A glyph with no outline (a space, or a bitmap-only
 * face) is a SUCCESS with no segments, not a failure. */
typedef struct {
	void (*move_to)(void *info, CGFloat x, CGFloat y);
	void (*line_to)(void *info, CGFloat x, CGFloat y);
	void (*conic_to)(void *info, CGFloat cx, CGFloat cy, CGFloat x, CGFloat y);
	void (*cubic_to)(void *info, CGFloat c1x, CGFloat c1y, CGFloat c2x, CGFloat c2y, CGFloat x,
			 CGFloat y);
	void (*close_path)(void *info);
} cg_outline_sink;

int cg_font_glyph_outline(CGFontRef font, CGGlyph glyph, CGFloat pixel_size, CGAffineTransform matrix,
                          const cg_outline_sink *sink, void *info);

#endif /* CORE_GRAPHICS_CGFONT_INTERNAL_H */
