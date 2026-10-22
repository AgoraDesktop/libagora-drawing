## A clip mask's edges are rounded OUTWARD

Two things here were MEASURED wrong from a toolbar and fixed together. First, `cg_image_mask_coverage` sampled a
scaled mask with NEAREST NEIGHBOUR — `(int)(u * width)` and one texel — so a 16-pixel stencil drawn into a 24-point
cell came out blocky and dithered; it interpolates the four surrounding samples when the image asks for it
(`CGImageMaskCreate`'s `shouldInterpolate`), and a 1:1 draw still lands on texel centres, so nothing changes at a
native size. Second, `cg_device_bounds` truncated its device edges with a cast (`(int)maxx`), and that is the one
conversion both the image sampler and `CGContextClipToMask` use. A rectangle whose right edge lands inside a
pixel therefore never asked the mask about that last, partly-covered column — and for a clip mask the unsampled
column keeps "keep everything", so a stencilled fill painted a one-pixel fringe along its right edge. MEASURED on a
live toolbar: a vertical line beside every icon at a third of the ink's strength. It is `floor` on the low edges and
`ceil` on the high ones now, which is the rule that includes every pixel the rectangle touches.

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

## The default font set

**Agora ships its own fonts, because inheriting the host's is how one desktop stops looking like one desktop.**
`fonts/` holds an open-licensed set modelled on the roles macOS fills with its own catalogue — Figtree for the
system UI (Inter remains the fallback and the explicit `Inter` name), Source Serif 4 for New York, IBM Plex Mono
for SF Mono/Menlo, Liberation for the Helvetica/Arial, Times and Courier roles, and the display, handwriting and
symbol faces recorded in `fonts/PROVENANCE.md`. No Apple font is included or redistributable.

The set installs under `<prefix>/share/fonts/agora` and is **the first directory the name registry searches**
(`AGORA_FONT_DIR` in `CMakeLists.txt`, compiled into `CGFont.c`). Host fonts are searched after it, and
`FN_FONT_PATH` replaces the list. `CGFontAliases.c` maps macOS names to the bundled families only when the exact
name is absent, so a real Helvetica on the host still wins. Every family carries its upstream licence; the
three static instances and the families that reserve a name are recorded in `fonts/PROVENANCE.md`.

## Status

**AND A PDF READER IS HERE: THE DOCUMENT, THE PAGE TREE, THE OBJECTS AND THE CONTENT STREAM.** It is the G2 reader
the surface plan parked, built in-tree rather than over pdfium — a reader-only C++ library in a C stack would have
had to lend its own object model to `CGPDFObject`, which is the one thing the plan said not to do. The doors are
`CGPDFDocument`, `CGPDFPage`, `CGPDFObject`, `CGPDFArray`, `CGPDFDictionary`, `CGPDFString`, `CGPDFStream`,
`CGPDFContentStream`, `CGPDFScanner` and `CGPDFOperatorTable`. BOTH CROSS-REFERENCE SPELLINGS ARE READ (the
classic `xref` table and the PDF 1.5 stream, with `/XRefStm`), and so are OBJECT STREAMS, because a file that
compresses its dictionaries into one stream is now the normal case rather than the exotic one. The filters are
Flate (zlib), LZW, ASCIIHex, ASCII85 and RunLength, with the PNG and TIFF predictors — a PDF 1.5 cross-reference
stream cannot be read at all without the PNG predictor, so it is not an optional refinement. A page FLATTENS ITS
INHERITANCE on open (`/MediaBox`, `/CropBox`, `/Rotate` from any ancestor), `CGPDFPageGetDrawingTransform` does
the quarter-turn-then-scale arithmetic, and the scanner tokenises a content stream into operands and operators
with callbacks. MEASURED (tests/pdf.c): two fixtures built byte by byte — a classic-xref three-page file with an
inherited box, a UTF-16BE title, a PDF date and two filtered streams, and a 1.5 file whose `/Info` lives inside an
object stream — every structural door asserted, both fixtures reopened from a file URL, and the scanner's five
callbacks checked operand by operand.

**AND A PDF PAGE DRAWS: PATHS, COLOUR, TEXT, IMAGES, FORMS AND SHADINGS.** `CGContextDrawPDFPage` installs an
operator table over the content-stream scanner and turns each operator into the matching Core Graphics call, so a
page is drawn in its OWN user space (`CGPDFPageGetDrawingTransform` fits and rotates it). Text is drawn as glyph
OUTLINES through `cg_font_glyph_outline` — a PDF's positioning is not Core Text's, and filling the outline is what
the text-showing operators mean — which also makes the render modes (fill, stroke, clip) one path operation each.
Fonts come from the embedded /FontFile, /FontFile2 or /FontFile3 program through FreeType, with the `ABCDEF+`
subset tag stripped and the standard fourteen falling back by name; /Encoding is a base table plus /Differences,
and a Type 1 font that carries NONE is read through the program's own Adobe encoding — the case every pdfTeX
document is, and without it `efficient` draws with a breve where the `ffi` ligature belongs; Identity-H CID fonts
read /CIDToGIDMap and /W. Colour is converted to RGB once, including /ICCBased through
little CMS, /Indexed, /Separation and /DeviceN tint transforms, and the CalRGB and Lab formulas. Images are DCT
through the JPEG decoder or raw samples converted per colour space; form XObjects recurse with their /Matrix and
/BBox; axial and radial shadings sample their function into a gradient. MEASURED (tests/pdf.c, one page): a
DeviceRGB rectangle at its centre pixel, black text ink somewhere in the string's box, the four quadrants of a
2×2 image in their own colours, a red-to-blue axial shading inside the clip it was given and NOT outside it, and
a form XObject's square — each read back from the destination's bytes. AND MEASURED against real files rather
than only fixtures: `/usr/share/doc/quilt/quilt.pdf` (pdfTeX, twelve pages of Type 1 text) and
`/usr/share/cups/data/default-testpage.pdf` (paths, quadratic logo outlines, CMYK colour wheels) render as poppler
renders them.

**AND A PDF IS WRITTEN: `CGPDFContext` RECORDS DRAWING INTO CONTENT STREAMS AND WRITES THE FILE.** This is the
half `dataWithPDFInsideRect:` and print-to-PDF need, and it is a RECORDING context rather than a raster one: the
context has no real surface, every painting door writes the matching operators (paths replayed from
`CGPathApply`, quads raised to cubics), images become Flate-compressed image XObjects with a /SMask when they
carry alpha, alpha and blend become /ExtGState, and clips and `q`/`Q` are the PDF state they spell. Pages are
begun and ended explicitly and carry their own /MediaBox; `Close` writes the header, objects, classic xref and
trailer through the caller's CGDataConsumer. **TEXT IS EMBEDDED WHEN THE FACE CAN BE**: a TrueType/`glyf` program
becomes a Type0/CIDFontType2 font with Identity-H and a /ToUnicode CMap, so the page's text is selectable and
searchable at any scale. **THE PROGRAM IS SUBSETTED**: only the glyphs the document used, their composite
components and `.notdef` are kept, renumbered, and named through a `/CIDToGIDMap`; the content streams carry CIDs,
so the renumbering never rewrites a page. A two-glyph page's font is a few kilobytes rather than the whole face,
and `pdffonts` reports the six-letter subset tag. **A NON-CID-KEYED CFF FACE IS EMBEDDED TOO**, as a PDF Type 1
font: the bare `CFF ` table becomes `/FontFile3 /Type1C`, each used glyph gets a one-byte code and a
`/Differences` name from the CFF charset, and a face using more than 255 glyphs is split across shards — the
255-code limit is PDF's, not this writer's. A CFF program is embedded WHOLE (CFF subsetting is a different problem
from `glyf` subsetting) and a CID-keyed CFF still falls back to outlines; `tests/pdf_cff.c` checks the structure,
the rendered ink and the shard boundary, and skips when the host has no CFF face; a CFF or program-less face falls back to exact vector OUTLINES, and the writer says which
it did. MEASURED
(tests/pdf_write.c): a two-page document drawn, closed, reopened by this library's own reader, and rendered by
its own renderer — rectangle, text ink, an opaque image's samples, an alpha image's transparent half, a /ca 0.5
fill and a clip that must not survive its `Q`, all read back as pixels; and the same file opens in poppler with
its title, creator, XMP metadata and page sizes intact — and `pdftotext` extracts the string, `pdffonts` reports
the face embedded with a Unicode map, which is the check that "text, not outlines" means something outside this
tree.

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

- **AND WHERE THE CHANNELS LAND IS DECIDED IN ONE PLACE, `cg_channel_slots`, FROM THE ALPHA'S POSITION.** A
  destination's colour order is a property of its LAYOUT and not of its fourth byte: `NoneSkipFirst` is B, G, R, X
  exactly as `PremultipliedFirst` is, and `NoneSkipLast` is R, G, B as `PremultipliedLast` is. Three writers used to
  work that out separately and ALL THREE got an opaque spelling wrong — the image sampler by comparing the
  destination's pixman format to `a8r8g8b8`, which a window's `x8r8g8b8` surface is not, and two others by reading
  "`PremultipliedLast`" as "R, G, B" with a fall-through for everything else. MEASURED, the window chrome's close
  button came out (86, 80, 242) where the theme's `#F25056` is (242, 80, 86). The image sampler, the plus-darker
  hand composite and the smoothed glyph's per-channel blend all ask the helper now, and a case draws one image
  whose channels are all different into each of the four destinations and asserts the byte at each position.

- **AND TEXT IS DRAWN IN COLOUR: SUB-PIXEL ANTIALIASING, WHICH IS WHAT MAC OS X'S TEXT LOOKED LIKE.**
  `CGContextSetShouldSmoothFonts` and `CGContextSetAllowsFontSmoothing` used to refuse by name; they now record,
  and the glyph is rasterised with `FT_LOAD_TARGET_LCD` and the engine's five-tap LCD filter, then composited by a
  hand-written per-channel blend — pixman's mask is one coverage for three channels and cannot say what sub-pixel
  coverage MEANS. What decides whether it happens is the DESTINATION: a pixel has one alpha and three coverages,
  so an opaque destination gets colour and one with an alpha channel gets grey. MEASURED (tests/drawing.c):
  1223 fringed pixels into an opaque destination, **zero** into an alpha one, the same ink to 0.07%, and the
  fringe on the correct side of the stroke — red on the left of black-on-white ink, blue on the right.

- **AND THE FONT'S PAIR ADJUSTMENTS REACH THE LAYOUT: KERNING, FROM WHICHEVER TABLE THE FACE KEEPS IT IN.**
  `cg_font_kern` asks the engine for the old `kern` table and falls back to a reader of this library's own for
  **GPOS** — because THE FACE THIS DESKTOP DRAWS WITH HAS NO `kern` TABLE AT ALL, so before this every pair it
  carries was invisible and the text was set unkerned: MEASURED, `To` sat 1.02 pixels too far apart at 13 points,
  which is the amount Pango (HarfBuzz) closes the same pair by. `CGGPosKern.c` reads one feature — `kern` — and the
  two PairPos subtable formats, extension lookups included; ligatures, marks and contextual positioning are
  HarfBuzz's and are stated as absent. The table is read once per font and kept, and every read is bounds-checked.

- **AND THE PEN'S SUB-PIXEL POSITION REACHES THE SCREEN, IN BOTH RENDERINGS.** The fraction of the pen's device
  position is passed to the engine per glyph, and MEASURED end to end — one glyph drawn at five pen positions a
  quarter-pixel apart, the composited ink's centre of mass compared — the grey rendering moves the ink 1.000 for a
  pixel of travel and the smoothed one 0.250, 0.253, 0.248, 0.250 per quarter. IT DID NOT ALWAYS: the bearing of an
  LCD bitmap is in WHOLE PIXELS (`bitmap_left` is reported in pixels for an LCD bitmap exactly as for a grey one)
  and this file divided it by three, which put every smoothed glyph's samples in a different sub-column for every
  phase and made the ink JUMP four tenths of a pixel as the pen moved — a wobble in the spacing of every line on
  the desktop. `tests/drawing.c` asks it of both renderings, because a check that asked only about grey would be
  asking about a path a person does not see.

### What is not done

- **PDF EMBEDS TRUETYPE AND NON-CID CFF; NOT TYPE 1 OR CID-KEYED CFF.** A Type 1 face has no `/FontFile2` or
  `/Type1C` route here, and a CID-keyed CFF cannot be a simple Type 1 font, so both fall back to outlines; the
  writer names that fallback when it happens. CFF programs are embedded whole rather than subsetted. TrueType
  subsetting keeps the glyphs and their composites, not the shaping tables (`GSUB`, `GPOS`, `kern`) — a PDF places
  every glyph explicitly, so those cannot affect the page. **AXIAL AND RADIAL GRADIENTS ARE RECORDED** as PDF
  shadings whose stitching function is built from the gradient's stops; a gradient with PER-STOP ALPHA is refused
  (no alpha channel in a DeviceRGB function, and no soft mask is written). **`CGContextDrawShading` IS RECORDED**
  for a DeviceGray, DeviceRGB or DeviceCMYK shade: its CGFunction is sampled into a type 0 function and the
  geometry becomes a type 2 or 3 shading. **COLORED TILING PATTERNS ARE RECORDED**: the caller's `drawPattern` is
  run into a pattern content stream (with its own `/Resources`, including whatever the tile uses), and the page
  selects it with `/Pattern cs` and `scn` or `SCN`; this renderer draws one by rasterising the tile once and tiling
  it through a CGPattern. The PDF context still REFUSES BY NAME uncolored patterns, shadows, transparency layers
  and clip-to-mask, so a caller knows when a page lacks them.
- **PDF encryption is reported, not decrypted.** `CGPDFDocumentIsEncrypted` answers true and the pages, catalog and
  info refuse by name; `IsUnlocked` is therefore false for an encrypted file and there is no password door.
- **The scanner skips inline images rather than parsing them**, so the renderer does not draw them either, and
  `CGPSConverter` (PostScript) does not exist. The RENDERER also refuses Type 3 fonts, named or vertical CMaps,
  patterns, soft masks and mesh shadings by name; DCT images are drawn, while JPX, JBIG2 and CCITT streams are
  named through `CGPDFDataFormat` and their bytes returned untouched.
- **No umbrella `CoreGraphics.h`**, so Apple's usual `#include <CoreGraphics/CoreGraphics.h>` cannot be
  written; a consumer includes the individual headers.
- **Three translation units are named `*Foundation.c`** — `CGDataProviderFoundation.c`,
  `CGDataConsumerFoundation.c`, `CGGeometryFoundation.c` — and contain neither Foundation nor Objective-C.
  The names are the Fiwix tree's and are kept for now.
- **From C, `<CoreFoundation/CoreFoundation.h>` needs `-fblocks`**, because that umbrella pulls in
  `CFCalendarPriv.h` and `ForSwiftFoundationOnly.h` — internal headers a consumer should not have to
  tolerate.
