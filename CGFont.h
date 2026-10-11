/*
 * CGFont — a font, its metrics, and its glyphs.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE FONT OBJECT IS APPLE'S API; THE FACE IS FREETYPE'S. This is the first half of Core
 * Graphics' text surface, and it is deliberately the half that rasterises nothing: a font is
 * metrics and glyph identity, and everything a caller can ask about a font in font units is
 * answered here, from the engine's face, offline.
 *
 * WHAT IS NOT HERE YET, AND WHY THAT IS NOT AN OVERSIGHT. The rows this header leaves undeclared
 * are owed, not forgotten, and they fall into three groups with three different reasons:
 *
 *   - ⚠ **`CGFontCreateWithFontName` IS NOT OWED, AND THIS NOTE WAS STALE — IT IS CORRECTED HERE (appkit-plan.md
 *     §A10bt).** The door is DEFINED in `CGFontNames.m`, over `cg_font_create_with_name` in this file: a registry
 *     with the FSH directories, a face-name matcher and a resolve-once cache. What IS still owed is
 *     `CGFontCreateWithPlatformFont`, which needs a platform font reference this tree does not have and whose body
 *     raises and says so. The registry's own decision — where fonts live and how names map to them — was made where
 *     the code is rather than here, which is why this note drifted.
 *   - ⚠ **`CGFontGetTypeID` IS NOT OWED EITHER, AND THIS NOTE WAS STALE IN THE SAME WAY (checked with the
 *     rest of the table).** It said the door "needs the CF type-identity question answered for the WHOLE
 *     library: this tree's CoreGraphics has no type registry". THE REGISTRY EXISTS -- it is `CGTypeID.c`,
 *     whose comment says a door defined beside its class "would put the numbers in eleven places and make
 *     them agree by review", which is exactly the arrangement this note was asking for -- and the door
 *     answers 5, measured, defined in the library rather than declared and forgotten.
 *   - the tables, variations and PostScript-subset families (`CGFontCopyTableTags`,
 *     `CGFontCreateCopyWithVariations`, `CGFontCreatePostScriptSubset`, ...) are a printing and
 *     embedding surface, and the half of the system that would call them is the parked PDF one.
 *
 * THE FILE IS OPENED ONCE PER FONT AND NEVER PER SIZE — a measured rule, not a style preference.
 * The guest's second concurrent open of one file failed with FreeType error 2
 * ("Unknown_File_Format") on a byte-perfect font, because FreeType reports a failed READ as a bad
 * format; the toolkit reached that wall by keeping a face per (family, size, bold). A `CGFont`
 * therefore holds ONE face and sizes it per use, and `CGFontCreateWithDataProvider` opens no file
 * at all.
 *
 * THE BYTES STAY THE PROVIDER'S. Freetype's memory face POINTS INTO the provider's buffer, so the
 * font retains the provider and the provider's own contract — the bytes stay valid until it is
 * released — becomes the font's. Nothing is copied.
 *
 * AND THE STRING DOORS RETURN `CFStringRef`, WHICH IS APPLE'S OWN SIGNATURE. This paragraph used to say
 * they returned `NSString *` and called that "this tree's established spelling"; it is no longer the
 * spelling, because the binding went back to Core Foundation. `CGFontCreateWithFontName` takes a
 * `CFStringRef` and the three Copy doors return one, so a caller needs no Objective-C runtime here.
 */
#ifndef CORE_GRAPHICS_CGFONT_H
#define CORE_GRAPHICS_CGFONT_H

#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGDataProvider.h>

#include <stdbool.h>
#include <stddef.h>

/* AND NO FORWARD DECLARATION OF `NSString` STANDS HERE ANY MORE. One did — under `__OBJC__` as the
 * class, otherwise as the opaque struct a class is at the ABI level — because the string doors in this
 * header were spelled `NSString *`. THEY ARE `CFStringRef` NOW (`CGFontCreateWithFontName`,
 * `CGFontCopyPostScriptName`, `CGFontCopyFullName`, `CGFontCopyGlyphNameForGlyph`, and the variation-axis
 * names below), so nothing here names a Foundation class or needs one: the type a C caller writes a font
 * name with comes from <CoreFoundation/CFString.h> through CGBase.h. */

typedef unsigned short CGFontIndex;
typedef CGFontIndex CGGlyph;
typedef struct CGFont *CGFontRef;

/* APPLE'S VALUES, AND THE LAST TWO ARE THE REASON THIS ENUM EXISTS: `kCGFontIndexMax` is the
 * greatest VALID glyph index and `kCGFontIndexInvalid` is the value a lookup returns when it
 * finds nothing — a distinction that would be lost if a caller used 0 for "not found", since 0 is
 * `.notdef` and is a perfectly good answer. */
enum {
	kCGFontIndexMax = ((1 << 16) - 2),
	kCGFontIndexInvalid = ((1 << 16) - 1),
	kCGGlyphMax = kCGFontIndexMax
};

/* A FONT FROM THE BYTES A PROVIDER HOLDS. NULL for a NULL provider, or for bytes FreeType cannot
 * open as a font: this door does NOT guess a format or fall back to a system font, because a font
 * that is not the one the caller asked for is a different font. */
CGFontRef CGFontCreateWithDataProvider(CGDataProviderRef provider);

/* !! `CGFontCreateWithPlatformFont` IS DECLARED AND REFUSED BY NAME, AND APPLE'S OWN HEADER IS THE
 * GROUND: "Create a CGFontRef using `platformFontReference', a pointer to a platform-specific font
 * reference. FOR MACOS X, `platformFontReference' SHOULD BE A POINTER TO AN ATSFontRef." The parameter
 * is `void *` in the 10.2, 10.5 and 10.6 headers, so no ATS type is needed to declare the door — but
 * what it points at is the PLATFORM'S font object, and this platform has no ATS font reference. This
 * library's own faces are FreeType's, and putting one behind this door would make FreeType part of the
 * Apple-shaped public surface, which nothing else in these headers does. SO THE REFUSAL IS THE
 * DECLARED BEHAVIOUR, the same shape as `CGContextBeginPage`: the symbol exists so a ported caller
 * links and is told why. Use `CGFontCreateWithDataProvider` (bytes) or `CGFontCreateWithFontName`
 * (a name) — the two doors this platform can honestly answer. */
CGFontRef CGFontCreateWithPlatformFont(void *platformFontReference);

/* A FONT BY NAME, FROM THE FONT DIRECTORIES — THE REGISTRY, AND IT IS A CHOICE THIS LIBRARY MAKES
 * RATHER THAN ONE APPLE PUBLISHES. A name is matched, case-insensitively, against the face's
 * POSTSCRIPT name and its FULL name (Apple's contract), and also against `family style` and the family
 * alone (this library's kindness, so a caller need not know the PostScript spelling). The directories
 * are the FSH ones, searched in order: /System/Shared/Fonts, /System/Library/Fonts, /Library/Fonts,
 * /System/Fonts; `FN_FONT_PATH` (colon-separated) replaces that list, which is how a host probe
 * reaches this tree's own directory. A NAME THAT MATCHES NOTHING ANSWERS NULL: substituting a fallback
 * font would draw the wrong glyphs in the right places, which is worse than drawing nothing.
 *
 * FONTCONFIG IS DELIBERATELY NOT IN THIS PATH. The X11 stack has it and this library does not need it:
 * Apple's door takes a NAME, not a pattern, so a scan over the font directories answers it exactly —
 * at the cost of opening candidate files, which the one-face-per-style rule bounds. If pattern
 * matching is ever wanted, fontconfig can be layered behind this door rather than in place of it. */
CGFontRef CGFontCreateWithFontName(CFStringRef name);

CGFontRef CGFontRetain(CGFontRef font);
void CGFontRelease(CGFontRef font);

/* THE FACE'S OWN NUMBERS, IN FONT UNITS — the units `CGFontGetUnitsPerEm` reports. A font unit is
 * what a metric is expressed in before any size is chosen, which is why nothing here takes a
 * size and why nothing here rasterises. */
size_t CGFontGetNumberOfGlyphs(CGFontRef font);
int CGFontGetUnitsPerEm(CGFontRef font);
int CGFontGetAscent(CGFontRef font);
int CGFontGetDescent(CGFontRef font);
int CGFontGetLeading(CGFontRef font);
int CGFontGetCapHeight(CGFontRef font);
int CGFontGetXHeight(CGFontRef font);
CGFloat CGFontGetStemV(CGFontRef font);
CGFloat CGFontGetItalicAngle(CGFontRef font);
CGRect CGFontGetFontBBox(CGFontRef font);

/* THE TWO ARRAY FORMS, and they answer FALSE rather than partially: a caller that asked for
 * twenty advances and got nineteen cannot tell which one is missing, so a font that cannot
 * measure every glyph it was asked about says so and writes nothing it cannot stand behind. */
bool CGFontGetGlyphAdvances(CGFontRef font, const CGGlyph glyphs[], size_t count, int advances[]);
bool CGFontGetGlyphBBoxes(CGFontRef font, const CGGlyph glyphs[], size_t count, CGRect bboxes[]);

/* BY NAME, IN BOTH DIRECTIONS. A font without glyph names answers `kCGFontIndexInvalid` and NULL
 * — that is a font fact, not a failure. */
CGGlyph CGFontGetGlyphWithGlyphName(CGFontRef font, CFStringRef name);
CFStringRef CGFontCopyGlyphNameForGlyph(CGFontRef font, CGGlyph glyph);

/* BOTH RETURN +1, which is what `Copy` means; NULL when the face carries no such name. */
CFStringRef CGFontCopyPostScriptName(CGFontRef font);
CFStringRef CGFontCopyFullName(CGFontRef font);

/* THE TYPE IDENTITY OF THIS CLASS: a `CFTypeID`, the same for every object of the class and
 * different from every other class's. The value is THIS LIBRARY'S (Apple's are runtime-assigned and
 * published nowhere), which is why the header says so rather than implying a constant someone could
 * port; identity is the whole of what the door promises. See CGTypeID_internal.h. */
CGTypeID CGFontGetTypeID(void);

/* ------------------------------------------------------------------------- */
/* The PostScript formats, the glyph bounds, and the variation axis names      */
/* ------------------------------------------------------------------------- */

/* APPLE'S ENUM, TRANSCRIBED WITH ITS VALUES — AND THE SPACING OF THOSE VALUES IS THE POINT: the types
 * are numbered 1, 3 and 42 as the PostScript specification numbers them, not 1, 2, 3. A caller that
 * stored one and printed it would notice; a caller that passed one straight through would not, which
 * is exactly why the numbers are written down here rather than counted. */
enum CGFontPostScriptFormat {
	kCGFontPostScriptFormatType1 = 1,
	kCGFontPostScriptFormatType3 = 3,
	kCGFontPostScriptFormatType42 = 42
};
typedef enum CGFontPostScriptFormat CGFontPostScriptFormat;

/* THE GLYPH BOUNDS, IN APPLE'S OWN SHAPE: a second unnamed enumeration whose members name the ends of
 * the glyph range, with the maximum defined AS `kCGGlyphMax` above rather than by repeating a number.
 * `CGGlyphMin` IS ZERO AND MEANS "A GLYPH EXISTS AT THIS INDEX" — the missing-glyph sentinel in this
 * family is `kCGFontIndexInvalid`, not 0, which is what these two constants exist to keep apart. */
enum {
	CGGlyphMin = 0,
	CGGlyphMax = kCGGlyphMax
};

/* THE VARIATION-AXIS NAMES: `CFStringRef` CONSTANTS HERE, WHICH IS WHAT APPLE'S HEADER DECLARES TOO.
 * (This said `NSString *` "here" against `CFStringRef` in Apple's header, "this tree's binding for every
 * CF type"; the binding is Core Foundation now.) They are THE KEYS OF THE AXES DICTIONARY a caller
 * passes to `CGFontCreateCopyWithVariations`, so their spelling is part of the interface — and with both
 * sides `CFStringRef` there is one name a caller writes and one the dictionary is read by. */
extern const CFStringRef kCGFontVariationAxisName;
extern const CFStringRef kCGFontVariationAxisMinValue;
extern const CFStringRef kCGFontVariationAxisMaxValue;
extern const CFStringRef kCGFontVariationAxisDefaultValue;

#endif /* CORE_GRAPHICS_CGFONT_H */
