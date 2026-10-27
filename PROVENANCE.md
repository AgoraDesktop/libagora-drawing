# Provenance

`libagora-drawing` is Core Graphics for Agora. Its source is taken from the CoreGraphics that lives in
the Fiwix tree.

| | |
|---|---|
| Source | `../Fiwix/userland/CoreGraphics` |
| Source revision | Fiwix `df3147a451e88d8d2256537e6d6dcdf69471695f` (2026-10-10) |
| Extraction date | 2026-10-10 |
| Extraction | the whole `userland/CoreGraphics` tree, verbatim — 63 files (21 `.c`, 10 `.m`, 31 `.h`) |
| Licence | MIT, © 2026 Kyle J. Cardoza (per the per-file headers; see `LICENSE`) |

## Why it is being changed

The Fiwix Core Graphics was written **without CoreFoundation**, deliberately, and its own headers say
so — `CGBase.h` records that this tree's CG "declares its signatures with FOUNDATION types rather than
inventing a CF layer (the retracted CoreFoundation plan)", and `CGColorSpace.h`, `CGColor.h` and
`CGFont.h` each note that a name or a string door is `CFStringRef` *in Apple's header* and
`NSString *` *here*.

Agora **has** CoreFoundation (`libagora-core`), so `libagora-drawing` must be converted **back** to the
CF types, so that Core Graphics depends on Core Foundation the way Apple's does:

- **`NSString *` → `CFStringRef`**, and `CFStringCreate*` / `CFStringGet*` in place of `-UTF8String`
  and friends. The name constants become `CFStringRef` constants:
  `kCGColorSpaceGenericGray`/`GenericRGB`/`GenericCMYK`/`GenericRGBLinear`/`SRGB`/`AdobeRGB1998`/
  `GenericGrayGamma2_2`, `kCGColorWhite`/`Black`/`Clear`, and the font variation-axis names;
- **`NSArray *` → `CFArrayRef`** — `CGGradientCreateWithColors(CGColorSpaceRef, CFArrayRef, …)`;
- **`NSData *` → `CFDataRef`** — `CGColorSpaceCreateWithICCProfile`, `CGColorSpaceCopyICCProfile`,
  and the `…WithCFData` provider/consumer forms (`CGDataProviderFoundation.m`,
  `CGDataConsumerFoundation.m`);
- **`CFTypeID`** comes from `<CoreFoundation/CFBase.h>`. Today `CGBase.h` mints `CGTypeID` and aliases
  Apple's name onto it (`typedef CGTypeID CFTypeID`) — the opposite direction from Apple's
  `typedef CGTypeID CFTypeID` where `CFTypeID` is Core Foundation's;
- the Foundation-flavoured translation units — `CGColor.m`, `CGColorNames.m`, `CGColorSpaceNames.m`,
  `CGDataConsumerFoundation.m`, `CGDataProviderFoundation.m`, `CGFontNames.m`,
  `CGGeometryFoundation.m`, `CGGradientColors.m`, `NSCoderCGGeometry.{h,m}`,
  `NSValueCGGeometry.h` — become CF-based, and can be C rather than Objective-C.

## How a falsification is recorded here

**FALSIFICATION OUTPUT IS CAPTURED TO A FILE AND QUOTED FROM THE FILE.** Twice in this library's short history
the record of a falsification has described something other than what the run printed, and both times the
fault was the same: writing the output from EXPECTATION rather than from the output itself.

`0150d16` INVENTED A LINE THAT NEVER APPEARED. It reported
`FAIL: CFGetTypeID(descriptor) is 256 and CTFontDescriptorGetTypeID() is 1` for a run whose whole transcript was
`FAIL: … killed by signal 4`. The measured failure was the stronger one — SIGILL means Core Foundation halted
inside `_CFRuntimeCreateInstance`, before the first assertion, so a type id the runtime does not own cannot be
put on an object at all. Corrected in `87ba9f6`.

`e6b31a0` OMITTED LINES THAT DID APPEAR, which is the same fault wearing different clothes. It quoted two FAIL
lines from a run that printed four; the two dropped lines were the ones showing the falsification working
properly, so the record described a weaker test than the one that ran. Corrected in the commit that adds this
paragraph.

Both corrections stay in the history rather than being squashed into the commits they fix, because a tree that
hides how it keeps making a mistake cannot learn from it. The rule is three words long and has already been
worth it: quote the file.

## Agora changes

**The conversion is made.** THIS SECTION USED TO SAY "not yet made — the import is verbatim", which was
true of the import and has not been true since. The import was **21 `.c`, 10 `.m` and 31 `.h`**; this
repository is **29 `.c`, 30 `.h` and no `.m` at all** — the ten Foundation translation units became C.

| The plan's item | What this tree does now |
|---|---|
| `NSString *` → `CFStringRef`, `CFStringCreate*`/`CFStringGet*` for the message doors | the name constants and every string door; the variation-axis constants included |
| `NSArray *` → `CFArrayRef` | `CGGradientCreateWithColors` takes a `CFArrayRef` **of `CGColorRef`**, and the elements ARE the colours — the import's `NSValue` pointer-wrappers are gone. The header still described them for a while after; `a1a8a73` corrected it |
| `NSData *` → `CFDataRef` | `CGColorSpaceCreateWithICCProfile` / `CopyICCProfile`, `CGDataProviderCreateWithCFData`, `CGDataConsumerCreateWithCFData`; the URL and mutable-data forms are `CFURLRef` and `CFMutableDataRef` |
| `CFTypeID` from `<CoreFoundation/CFBase.h>` | done. `CGTypeID` remains this library's spelling of the identity VALUES; `CFTypeID` is Core Foundation's type, which is the direction Apple's headers have |
| the Foundation-flavoured translation units become CF-based, and C rather than Objective-C | they are C: `CGColorNames.c`, `CGColorSpaceNames.c`, `CGDataConsumerFoundation.c`, `CGDataProviderFoundation.c`, `CGFontNames.c`, `CGGeometryFoundation.c`, `CGGradientColors.c`. `NSCoderCGGeometry.{h,m}` and `NSValueCGGeometry.h` are gone |

**How far the conversion went was measured rather than assumed.** An audit of every header and source for
live Foundation residue — every `NS[A-Z]…` name, `NS_` macro, `@interface`, `@class`, `#import` and
`objc_msgSend` — found exactly FOUR lines in the whole library: `@class NSString;` and its opaque-typedef
twin in `CGColorSpace.h` and `CGFont.h`, with no signature naming one. They were dead, and they are gone
(`a1a8a73`), along with eleven comment blocks that still described the Foundation binding — one of them
claiming a `.m` file unwrapped `NSValues` that no longer existed.

`README.md` records what is still not done.

**The raw table doors, and why they belong on this side of the seam (2026-10-11).** `cg_font_copy_table` reads one table
by its four-character tag and `cg_font_table_tags` enumerates the directory. They were added because the Core Text
library owes a whole FAMILY of answers that are table reads — `fvar`/`avar` for variation, `feat` for features, `post`
for the underline metrics, `OS/2` and `head` for the traits, and a raw `CTFontCopyTable` — and those doors were owed
with the reason that NO SEAM DOOR READS A TABLE.

**AND THE IMPLEMENTATION IS ONE CALL, WHICH IS THE ARGUMENT FOR PUTTING IT HERE RATHER THAN THERE**: FreeType holds the
file's table directory already, so `FT_Load_Sfnt_Table` and `FT_Sfnt_Table_Info` hand back bytes this library has had
all along. The alternative — Core Text parsing an sfnt itself — would be a SECOND FONT ENGINE in a tree that has one,
and it would have to reimplement the directory, the tag lookup and the whole-file formats to reach the same bytes.

**BOTH DOORS TAKE A TWO-CALL CONTRACT AND THAT IS NOT AN AFFORDANCE**: a table's size is the table's business, and a
caller who guesses a buffer size is a caller who truncates silently. ASK WITH `buf == NULL` FOR THE SIZE; ASK AGAIN
WITH ROOM FOR THE BYTES, AND THE SAME NUMBER COMES BACK. A buffer that is too small is not an error for the same reason
— the answer still reports how much the table holds. A FACE WITH NO SUCH TABLE, OR NO TABLE DIRECTORY AT ALL, ANSWERS
0, and the zero-length table that would answer 0 as well is not distinguished: a table with no bytes says what a
missing one says.

**AND THE DOORS ANSWER THE FILE'S BYTES RATHER THAN AN OPINION ABOUT THEM.** The seam does not parse `head`, does not
check an `fvar` version and does not interpret a `post` header; the side that knows what those bytes mean is the side
that asked for them. That keeps the one font engine in the tree from growing a second, partial model of the format.

**AND THE CASE HOLDS THE PART A CALLER GETS WRONG**: the size from the first call and the size from the second must
AGREE, a real face's `head` table must BEGIN WITH ITS VERSION (0x00010000), the directory must list the tables TrueType
requires, the two doors must agree about a table's size, and a tag no face carries must answer 0 rather than invent a
table. The file is checked for first and its absence is REPORTED rather than counted as a pass, which is this suite's
existing rule.

`README.md` records what is still not done.

**Colour glyphs, and the strike that stands in for outlines (2026-10-26).** `cg_font_render_glyph` understood two
coverage formats — grey and sub-pixel — and printed `CG-REFUSE: this font renders glyph coverage this library cannot
use` for anything else, which included the only format a colour font emits. Three separate facts had to be true before
an emoji could be drawn, and the third one is why the first two were not enough:

1. **The rasteriser had to accept `FT_PIXEL_MODE_BGRA`** — four premultiplied channels, blue first, `width` counted in
   pixels — and report `*samples = 4`, the value `CGContext.c` reads as "a picture, not a mask". `FT_LOAD_COLOR` is
   asked for whenever a bitmap may be used at all (a no-op for a monochrome glyph), and `FT_Render_Glyph` is skipped
   when the load already produced a bitmap, because a strike is already a picture.
2. **The paint had to be its own composite.** A run of text is one a8 mask in one colour; a picture is neither, so a
   colour glyph ends the pending batch and is composited by `cg_composite_glyph_colour`, whose arithmetic is the
   premultiplied blend. The fill colour is ignored (that is Apple's behaviour) and the global alpha is not (it is not
   the fill colour's).
3. **The face had to have metrics, and a bitmap face does not.** Noto Color Emoji reports `units_per_EM == 0` and zero
   ascender and descender, so `CTFont`'s `size / upem` scale was zero and `CTShaperCreate` refused the face outright;
   and `FT_Set_Char_Size` on a strike-only face selects nothing, so the glyph came back 0x0 at every text size. The
   answers are the sfnt's own — `head` for the em grid, `hhea` for the vertical metrics, both read with the
   `FT_Load_Sfnt_Table`/`FT_Get_Sfnt_Table` doors this file already used for names — and the strike is SELECTED and
   SCALED to the device size, bearings and advance included.

The preference question was measured too, and it belongs to `libagora-text` rather than to this library: the
family-name order puts DejaVu Sans — which has a monochrome outline smiley at U+1F600 — before Noto, so a fallback for
an emoji reached a face that would paint it in the caller's ink. The first attempt paid for the walk inside the shared
font search, which the AppKit uses for the UI font as well, and that made the application's first frames seconds late;
the second paid **6.0 s** on the first fallback by asking the registry for each candidate's name; the third reads
PATHS, keeps the open face, and costs **17.9 ms once and 1 µs thereafter**. What this library holds is the picture and
the paint; which face supplies it is Core Text's decision, and it now prefers colour. `libagora-text`'s README has the
numbers.

`README.md` records what is still not done.
