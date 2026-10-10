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

**Imported; not yet built, and its NS types have not been converted back to CF.** Two things follow.

1. **The types must go back to CF.** Fiwix's Core Graphics was deliberately written against
   *Foundation* types — `NSString *`, `NSArray *`, `NSData *` — where Apple's headers say
   `CFStringRef`, `CFArrayRef`, `CFDataRef` (its own comments record the substitution). It must be
   converted back so that Core Graphics depends on `libagora-core` the way Apple's does.
2. **A build.** It needs `libagora-core`, and pixman, libpng, libjpeg, FreeType and **lcms2**
   (lcms2 is not installed on this machine).
