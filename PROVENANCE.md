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
