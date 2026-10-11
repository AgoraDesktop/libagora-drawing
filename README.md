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

### What is not done

- **No umbrella `CoreGraphics.h`**, so Apple's usual `#include <CoreGraphics/CoreGraphics.h>` cannot be
  written; a consumer includes the individual headers.
- **Three translation units are named `*Foundation.c`** — `CGDataProviderFoundation.c`,
  `CGDataConsumerFoundation.c`, `CGGeometryFoundation.c` — and contain neither Foundation nor Objective-C.
  The names are the Fiwix tree's and are kept for now.
- **From C, `<CoreFoundation/CoreFoundation.h>` needs `-fblocks`**, because that umbrella pulls in
  `CFCalendarPriv.h` and `ForSwiftFoundationOnly.h` — internal headers a consumer should not have to
  tolerate.
