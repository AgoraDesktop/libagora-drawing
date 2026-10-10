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

## Agora changes

Not yet made — the import is verbatim. This section records the conversion as it lands.
