# libagora-drawing

**Core Graphics for Agora** — the 2D drawing and geometry model of Agora's Cocoa-compatible stack:
`CGGeometry` (points, sizes, rects, affine transforms), the colour and colour-space model, font
handling, images, and the bitmap context whose fills are rasterized by pixman.

Agora is an installable desktop environment for Linux and BSD; see the project README in the parent
directory (`../README.md`). `libagora-drawing` sits on `libagora-core` (Core Foundation).

## Provenance

Taken from the CoreGraphics in the Fiwix tree (`../Fiwix/userland/CoreGraphics`), where it was written
**without** CoreFoundation. See [`PROVENANCE.md`](PROVENANCE.md) for the exact revision and what is
being changed.

## Status

**Built, shared, and C throughout, over Core Foundation — and the NS types are converted back to CF.**
This section used to say the opposite on every count ("imported; not yet built, and its NS types have not
been converted back to CF", with lcms2 "not installed on this machine").

- **The build is made**: `build/libagora-drawing.so`, and every `#include` of a Foundation-side header is
  absent. Its only dependencies are Core Foundation and the C engines — pixman, lcms2, libpng, libjpeg and
  FreeType, all present.
- **The types are CF**, which the import's own comments recorded as owed: the name constants and string
  doors are `CFStringRef`; the profile and consumer doors are `CFDataRef`/`CFMutableDataRef`/`CFURLRef`;
  `CGGradientCreateWithColors` takes a `CFArrayRef` **of `CGColorRef`**; `CFTypeID` comes from
  `<CoreFoundation/CFBase.h>`; and the ten Objective-C translation units are C now — see `PROVENANCE.md`.
- **Every colour space this library names answers**, all seven of them, including the four the ledger used to
  list as owed. `CGColorSpaceNames.c` records the deviation the derived ones carry, and
  `CGGenericCMYKProfile.h` the one that could only come from profile bytes.

- **The RAW TABLE DOORS are the newest, and they exist because Core Text's table-reading answers need the file's
  bytes**: `cg_font_copy_table` (one table by its four-character tag) and `cg_font_table_tags` (the directory). Both take
  the two-call contract — size first, bytes second — and both hand back what the file holds rather than an
  interpretation of it, because the side that knows what a `post` header means is the side that asked.

- **AND TEXT IS DRAWN IN COLOUR: SUB-PIXEL ANTIALIASING, WHICH IS WHAT MAC OS X'S TEXT LOOKED LIKE.**
  `CGContextSetShouldSmoothFonts` and `CGContextSetAllowsFontSmoothing` used to refuse by name; they now record,
  and the glyph is rasterised with `FT_LOAD_TARGET_LCD` and the engine's five-tap LCD filter, then composited by a
  hand-written per-channel blend — pixman's mask is one coverage for three channels and cannot say what sub-pixel
  coverage MEANS. What decides whether it happens is the DESTINATION: a pixel has one alpha and three coverages,
  so an opaque destination gets colour and one with an alpha channel gets grey. MEASURED (tests/drawing.c):
  1223 fringed pixels into an opaque destination, **zero** into an alpha one, the same ink to 0.07%, and the
  fringe on the correct side of the stroke — red on the left of black-on-white ink, blue on the right.

### What is not done

- **No umbrella `CoreGraphics.h`**, so Apple's usual `#include <CoreGraphics/CoreGraphics.h>` cannot be
  written; a consumer includes the individual headers.
- **Three translation units are named `*Foundation.c`** — `CGDataProviderFoundation.c`,
  `CGDataConsumerFoundation.c`, `CGGeometryFoundation.c` — and contain neither Foundation nor Objective-C.
  The names are the Fiwix tree's and are kept for now.
- **From C, `<CoreFoundation/CoreFoundation.h>` needs `-fblocks`**, because that umbrella pulls in
  `CFCalendarPriv.h` and `ForSwiftFoundationOnly.h` — internal headers a consumer should not have to
  tolerate.
