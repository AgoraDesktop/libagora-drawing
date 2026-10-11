/*
 * CGFont_internal — the seam between a font and the engine that owns it.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
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

/* A FONT BY NAME, FROM THE CONFIGURED FONT DIRECTORIES: the door Apple calls
 * `CGFontCreateWithFontName` needs a REGISTRY — a name has to resolve to a file — and this is this
 * library's: the directories are the FSH ones below, a name matches the face's POSTSCRIPT name, its
 * FULL name or `family style` (case-insensitively, which is a kindness Apple's documentation does not
 * promise), and a name that matches nothing ANSWERS NULL rather than substituting a font the caller did
 * not ask for. `FN_FONT_PATH` overrides the directory list with a colon-separated one, which is how the
 * host probe reaches the tree's own directory. */
CGFontRef cg_font_create_with_name(const char *name);

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
/* `delta` IS THE SUBPIXEL FRACTION OF THE PEN, IN DEVICE UNITS (x right, y down — the engine's own
 * frame is handled inside). A whole-pixel caller passes (0,0) and gets the old behaviour; a caller
 * that has been asked not to subpixel-position passes (0,0) too, because rounding the pen IS that
 * setting. */
int cg_font_render_glyph(CGFontRef font, CGGlyph glyph, CGFloat pixel_size,
                         CGAffineTransform matrix, CGPoint delta, unsigned char **coverage,
                         int *width, int *height, int *left, int *top, double *advance);

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
