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

## The glyph bitmap cache

**A RASTERISED GLYPH IS REMEMBERED, BECAUSE ASKING FOR ONE IS A TRIP THROUGH THE HINTER AND A PAGE ASKS FIFTY TIMES.**
`cg_font_render_glyph` sets the face size, loads the outline, rasterises and warps every coverage sample — per call.
`cg_font_render_glyph_cached` (in `CGFont_internal.h`) answers the same picture from a cache, and every
FILLED-text door goes through it (`CGContextShowGlyphs`, `ShowGlyphsAtPoint`, `ShowGlyphsAtPositions`, `ShowText`,
`ShowTextAtPoint`); the stroke and clip text modes draw outlines and so are not mask work at all.

- **The key is the engine's own input in the engine's own precision**: the face, the glyph, the 26.6 device size
  (`FT_Set_Char_Size`'s argument and not the point size it came from), the four 16.16 transform factors, the two
  26.6 subpixel deltas, whether coverage was smoothed, and the ink polarity. Everything else the rasteriser reads
  (`mirror`, `plain`, the LCD filter, whether the warp applies) is a function of those, so it is not keyed.
- **The entry retains the `CGFontRef`.** The struct is a heap block, so a released face and a new one can share an
  address; holding a reference is what makes the pointer an identity. A font whose last caller lets go stays alive
  until its entries are evicted — bounded by the budget, and the price of a key that cannot alias.
- **4 MiB and 4096 entries by default**, evicted least-recently-used (found by scan, because the scan only happens
  when the budget is already full). `CG_GLYPH_CACHE_BYTES=<n>` overrides it; `0` turns caching off without breaking
  the borrow contract — every rendering then lives in a one-slot holder that the next call drops.
- **The door returns a BORROWED pointer.** The bytes belong to the cache and are valid until the next call into it,
  which is exactly a drawing pass: every consumer runs before the next glyph is asked for. The allocating door is
  still there for a caller that needs to own its buffer.
- **Unsynchronised, like the rest of this file.** `FT_Set_Char_Size` and `FT_Set_Transform` are already face-wide
  state written per glyph, so two threads drawing at once was a race before the cache and is not a new one.

MEASURED (2026, `tests/glyph_cache.c` and a direct clock on the door): a 13 px `A` with a quarter-pixel pen costs
**3.17 µs rendered and 51 ns remembered** — 62× — and a 47-character run through `CGContextShowTextAtPoint` is
**11.8× faster** (62 ms against 5.3 ms for 200 drawings, the cold loop flushing before every one). The suite proves a hit answers the miss's bytes and metrics over a
sweep of size, phase, transform, smoothing and ink; that the budget at zero renders the same pixels end to end
through `CGContextShowTextAtPoint`; that an evicted entry re-renders identically rather than being remembered wrong;
and that a released face and a fresh one do not share an entry.

**AND THE OTHER HALF OF A GLYPH'S COST, WHICH IS REMEMBERED ON THE FONT.** A glyph's *advance* was asked of the
engine per glyph per draw (`cg_glyph_advance` → `CGFontGetGlyphAdvances` → `FT_Load_Glyph`, 713 ns at 13 px), which
after the masks were cached was the largest item in a warm run. An advance is in FONT UNITS, so it is the same at
every size, pen phase and transform: `CGFontGetGlyphAdvances` now records one `int` per glyph on the font, and the
second ask is an array read — **4 ns a glyph**, and a warm 47-character run fell from 92 µs to **44 µs** (0.94 µs a
character). The door's contract is unchanged (font units, a batch refused whole if a glyph will not load, the
caller's array untouched in that case) and the per-font counters are how the test proves the second ask was the
cached one rather than believing a clock.

**AND THE PAINT NO LONGER BUILDS A GLYPH'S IMAGES EITHER, WHICH IS THE SECOND HALF OF THE SAME IDEA.** A mask and a
solid-fill image were allocated and released PER GLYPH — for a box the next glyph usually shares and a colour a whole
run shares. Both are scratch on the context now: the a8 mask GROWS to fit the largest glyph drawn and is refilled in
place, and the solid image is rebuilt only when the colour changes, so a terminal or a page of body text builds it
once. The sub-pixel blend was also asking pixman's region code whether it contained a point once per SAMPLE — three
times per pixel, for an answer the three sub-columns of one pixel cannot disagree about — and it now walks pixels.

MEASURED (2026, best of five, baseline subtracted, same 47-character run through `CGContextShowTextAtPoint` at
13 px): a warm grey glyph **617–658 ns → 447–454 ns** (about 29% less) and a sub-pixel one **1998–2110 ns → 1560–1606
ns** (about 24%).

**AND A GREY RUN IS NOW ONE COMPOSITE PER BATCH RATHER THAN ONE PER GLYPH.** The measurement that says this is worth
doing was taken on pixman directly: 3750 boxes of 8x15 composited one at a time cost 349 ns each, and the same boxes
packed into one strip per row cost 169 ns — the same pixels, half the time, because pixman's fast-path selection and
region setup happen once per strip. `cg_paint_glyph_run` is that, and it is the door every filled text run comes
through: `ShowGlyphs`, `ShowText`, `ShowGlyphsAtPoint` and `ShowGlyphsAtPositions` (which is how Core Text and the
layout manager draw). Glyphs are placed into one a8 strip and the strip is composited once.

**FOUR THINGS ARE DELIBERATELY NOT BATCHED, AND TWO OF THEM ARE MEASUREMENTS.** A recording context's text is the
writer's to record; a stroke or clip text mode is geometry; a clip MASK has to be multiplied into each glyph's own
coverage; and **sub-pixel coverage never uses pixman at all** — it is this file's hand blend, a loop over the glyph's
own pixels with no per-composite setup to amortise. Batching it was MEASURED as a regression (5.6–6.3 ms a frame for
a 150x25 grid against 8.3–8.9 with a strip), so the smoothed path stays per glyph by measurement rather than by
convenience.

TWO GLYPHS THAT OVERLAP ARE PAINTED IN SEPARATE BATCHES, because one combined mask would blend each destination pixel
once where two passes blend it twice. The test is INK, not boxes: adjacent monospace cells overlap by a pixel or two
of bearing that is empty, and flushing on boxes alone left text unbatched (and the batched path *slower*). Where the
boxes do intersect, the shared rectangle is checked for coverage on both sides, and the strip is merged with a
max-copy so that a later glyph's zeroes cannot erase an earlier one's ink.

MEASURED end to end (best of five, interleaved with the previous build on the same machine, a 150x25 grid of realistic
mixed text): **grey 1.58–1.70 ms → 1.26–1.31 ms a frame** (about 20%), and **447–493 ns → 316–338 ns** per glyph;
sub-pixel is unchanged at ~1.6 µs a glyph, which is the honest result of leaving it alone. `CG_TEXT_DIAG=1` makes the
painter report how many glyphs one batch carries, which is the number that says whether this engaged at all.

What remains is the composite itself, the hand blend's shape, and the full-window damage the seam still asks for (see
"What is not done").

## Colour glyphs, and the strike a face has instead of outlines

**A COLOUR GLYPH IS A PICTURE, NOT A MASK, AND THE ENGINE HANDS IT OVER AS FOUR PREMULTIPLIED CHANNELS.** FreeType
returns it in `FT_PIXEL_MODE_BGRA` — blue first, one byte each, the colour channels already multiplied by alpha —
and this library used to accept only grey and sub-pixel coverage, printing a refusal for anything else. Every colour
font in the world answers in exactly that format, so the refusal was the whole of "Agora cannot draw an emoji".

- **`*samples` IS 4 FOR A COLOUR GLYPH**, where 3 is sub-pixel coverage and 1 is grey. `*width` is counted in PIXELS
  (an LCD bitmap's width is three samples per pixel) and the bytes are `width * height * 4`. `cg_font_has_colour` is
  the same question asked of the FACE before a render, which is what keeps a smoothing flag that cannot matter out of
  the glyph cache's key: a picture has no sub-pixel columns, so a face with colour glyphs is asked for grey.
- **THE PAINT IS ITS OWN COMPOSITE** (`cg_composite_glyph_colour`): the source's components go down as they are and
  the destination is attenuated by the source's alpha and by the context's GLOBAL alpha. The caller's FILL COLOUR is
  ignored — a colour glyph has no ink colour to choose — and the clip and clip mask are the same two tests the
  smoothed path makes. A batch cannot carry one: a run is one a8 mask painted in one colour, so a colour glyph
  FLUSHES the pending strip first and is painted on its own, in order.
- **AN OPERATOR THAT CANNOT TAKE PREMULTIPLIED COLOUR FALLS BACK TO THE ALPHA.** `plusDarker` and the rest are hand
  loops over one source colour, so a picture under them is painted as its ALPHA as coverage in the caller's ink —
  the shape in the right place, in the right colour, without the picture's own. That is stated rather than silently
  dropping ink.
- **THE ONLY COLOUR FONT A LINUX DESKTOP HAS IS A BITMAP STRIKE, WHICH IS A SECOND PROBLEM.** Noto Color Emoji has no
  outlines: one embedded strike at 109 ppem, 136x128 pixels for an emoji, and `units_per_EM == 0` in FreeType, because
  the engine keeps the scalable path's copy of that number. `FT_Set_Char_Size` to a text size selects NO strike and
  the glyph comes back 0x0 — so a renderer that merely ACCEPTED BGRA would still have drawn nothing at 13 points.
  The strike is therefore SELECTED (`FT_Select_Size`, the largest) and the picture SCALED to the device size the
  caller asked for: box-averaged when it shrinks, interpolated when it grows, premultiplied so the channels average
  correctly. The left bearing, the top bearing and the advance are scaled with it, so the picture lands in the cell
  the layout reserved.
- **AND THE FACE'S METRICS COME FROM THE SFNT, WHICH IS WHERE THEY STILL ARE.** A bitmap face reports
  `units_per_EM`, `ascender`, `descender` and `height` as ZERO, and the text tier divides by the first of those for
  every metric it has — so an emoji run measured zero and `CTShaperCreate` refused the face outright. `head` still
  says 2048 and `hhea` still says 1900/-500, so those are read through the same sfnt door the name tables use.
  MEASURED: Noto Color Emoji answers `upem=2048 ascent=1900 descent=500`, and its advance for U+1F600 is 2550 units —
  16.19 points at 13, which is the emoji's own width.
- **THE GLYPH CACHE CHARGES A COLOUR PICTURE FOUR BYTES A PIXEL.** An entry accounted at one byte a pixel would let
  the cache hold four times the memory it reports; the same change makes an LCD entry's bill honest too.
- **WHICH FACE the text tier picks for an emoji is Core Text's decision, and it is made**: the cascade prefers a face
  that draws the character in colour, ahead of the registry's family-name order, at a cost paid once (17.9 ms for the
  search, 1 µs thereafter). `a😀b` in a monospaced face is three runs with the middle one in Noto Color Emoji. See
  `libagora-text`'s README for the design and the two slower shapes that were measured first.
- **WHAT IS NOT DONE.** A TURNED OR SHEARED colour glyph falls back to the outline path (a strike is a picture at one
  size and ignores the transform), which for an outline-less face draws NOTHING rather than a picture at the wrong
  angle. **COLR v1 and SVG colour descriptions are not handled** — a COLR v0 font renders as far as FreeType paints
  its layers, and an SVG-in-OpenType face draws its fallback outlines (Noto Color Emoji as installed here is CBDT and
  is the case measured).

MEASURED (`tests/colour_glyphs.c`, the host's Noto Color Emoji): `upem=2048 ascent=1900 descent=500`, U+1F600 maps to
one glyph with 0 in the low surrogate's slot, a 13-pixel rendering is **16x15** premultiplied pixels (168 opaque, all
168 coloured, none over alpha) where the strike is 136x128, four times the size is four times the picture and the
advance (65x61, `advance 16.22 -> 64.9`), the cache answers the same bytes on the second ask and bills them at four
bytes a pixel, and a bitmap context painted with a BLACK fill receives **548 inked pixels, 513 of them coloured**. The
end-to-end case is `a😀b` through `CTLineDraw` at 32 points: **821 coloured of 1287 inked pixels**, the emoji's own
yellow at the pen and the two letters in the black fill.

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
font, and its program is SUBSETTED: only the used glyphs' charstrings are kept, the charset is rebuilt with the
same SIDs (so `/Differences` still selects by name), and the Private DICT with its Local Subrs travels whole. A
`seac`-style charstring, a CID-keyed CFF or an Expert charset makes the subsetter fall back to the whole program
rather than risk a wrong one; the bare `CFF ` table becomes `/FontFile3 /Type1C`, each used glyph gets a one-byte code and a
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
  asking about a path a person does not see. Those numbers are measured with the quantisation doors CLOSED, which is
  the honest way to ask the question — see the entry below for the snapping that is on by default.

- **AND THE SIZE A GLYPH IS RASTERISED AT IS A DEVICE SIZE, WHICH IS WHAT MAKES A SCALED DESKTOP SHARP RATHER THAN
  MERELY THE RIGHT SIZE.** A font size is in points and the CTM is what turns points into pixels, but the rasteriser
  used to hand the point size to the engine as its hinting grid and the CTM as a TRANSFORM — which FreeType applies
  *after* hinting, in its own words: "hinting is unaltered by the transformation and is performed on the character
  size given in the last call to `FT_Set_Char_Size`". So every non-1.0 output was hinted for a grid that is not on
  the screen. MEASURED (Inter `H` at 13 points on a 2x output): a 16x22 bitmap where a real 26-px em is 16x20, nine
  per cent more ink, every stem a 13-px decision doubled; at the 96-dpi X11 head's 1.33203125 it was 12.3% heavy and
  a row too tall. The scale is now DIVIDED OUT of the transform and multiplied into the em, so the transform that
  reaches the engine is a rotation and/or shear and nothing else — and the property is a test rather than a claim:
  the same glyph drawn at the same DEVICE size, once with the size in the font and once with it in the CTM, must be
  the same picture, to a per cent and to the row.

- **AND SMALL TEXT IS NOT MERELY SMALLER: TWO SIZE POLICIES, BOTH OF THEM MEASURED.** A stem narrower than a pixel
  has no fully-covered pixel in it, so at this machine's 0.8046875 scale a 13-point stem is 0.85 of a device pixel
  and the eye reads it lighter than the same design at a pixel and a half. No hinting policy can fix that — 0.85 px
  of ink snapped to the grid is still 0.85 px — so the rasteriser applies a **coverage warp** below 16 px per em:
  each coverage sample is raised to an exponent that goes to 1.0 at the threshold and strengthens to a floor at
  8 px, with a stronger curve for light ink on a dark ground (which blooms, and so reads thinner still). It is
  applied to the MASK, once, because grey coverage goes to pixman and sub-pixel coverage to a hand blend and a warp
  at either would leave the other out. MEASURED, a ten-letter word at 13 points: at 0.8046875 it weighs 1.009 of
  the same word on a whole-pixel desktop in grey and 0.994 in colour (1.042 and 1.022 with the ink light on a dark
  ground, where the curve is stronger), against **0.954 and 0.948 with the warp turned off** — which is the thinning
  it exists to undo, and the test's floor sits above those numbers rather than above zero.
  Beside it, the **LCD filter is chosen by size**: the engine's five-tap FIR is the right convolution of a
  monitor's phosphors, but at ten pixels an em it takes a third of the peak coverage off a stem (MEASURED, Inter
  `l`: 206 against 218 through the three-tap at 10.46 px, 171 against 179 at 8 px, and three samples narrower), so
  below 14 px the three-tap is used. And a small GREY glyph is loaded with `FT_LOAD_TARGET_LIGHT`: full hinting
  grid-fits a stem's horizontal edges as well as its vertical ones, and the ADVANCES here are Core Text's own and
  unhinted, so a fully hinted outline on an unhinted advance is a glyph moved inside its own advance — invisible at
  26 px, uneven rhythm at 10.

- **AND QUANTISATION IS IMPLEMENTED RATHER THAN REFUSED, WHICH IS WHAT "A CONTEXT QUANTIZES SUBPIXEL POSITIONS"
  NEEDED.** The two doors used to refuse by name, on the grounds that the 10.6 header never says TO WHAT; what the
  refusal cost was the reason quantisation exists — a mask that can be reused, and a stem that stays where it was.
  The quantum is a THIRD of a pixel for sub-pixel coverage (a third IS a sub-column, so a fraction between two of
  them has nowhere to land) and a QUARTER for grey (no sub-columns, and a quarter is the finest step that still
  repeats), and both doors default ON, as they do on the system this library duplicates. The whole part and the
  fraction are read from the same snapped value, so they cannot disagree, and closing either door restores the exact
  fraction. The case that measures the pen's travel now asks BOTH questions: with the doors closed a quarter-pixel
  step still moves the ink a quarter, and with them open a pen at 10.25 must land exactly where a pen at 10 + 1/3
  lands — an equality, because that IS what snapping means.

### What is not done

- **THE GLYPH'S BITMAP, ADVANCE, PAINT IMAGES AND GREY RUNS ARE ALL KEPT; SUB-PIXEL RUNS AND THE WHOLE-WINDOW
  DAMAGE ARE NOT.** A grey run is one composite per batch now (see above) and a grey glyph costs ~0.33 µs; a sub-pixel
  one still costs ~1.6 µs, because that coverage goes through this file's hand blend rather than pixman and batching it
  was measured as a regression. What is left there is the blend's own shape — three channels written in one pass over
  a contiguous strip, with the region test hoisted out of the loop it currently sits in — which is a rewrite of one
  loop, not a cache. And the client still damages its WHOLE window on every commit
  (`wl_surface_damage_buffer(0, 0, width, height)`), so a terminal's scrolling repaint costs the compositor a full
  window per frame whatever this library saves; that is a change in the seam (`AGEndFrame`'s damage rectangle), not in
  this repository.
- **TEXT AS OUTLINES IS OUTSIDE BOTH CACHES.** PDF embedding and text-as-path use `cg_font_glyph_outline`, which
  neither cache covers — a PDF places each glyph once, and a stroked or clipped glyph is geometry rather than a
  mask, so there is nothing to reuse in either.
- **PDF EMBEDS TRUETYPE AND NON-CID CFF; NOT TYPE 1 OR CID-KEYED CFF.** A Type 1 face has no `/FontFile2` or
  `/Type1C` route here, and a CID-keyed CFF cannot be a simple Type 1 font, so both fall back to outlines; the
  writer names that fallback when it happens. CFF subsetting is conservative — CID-keyed CFF, Expert charsets and
  `seac` charstrings embed whole — but the common non-CID face is reduced to its used glyphs. Both subsetters keep
  the glyphs and their composites, not the shaping tables (`GSUB`, `GPOS`, `kern`): a PDF places every glyph
  explicitly, so those cannot affect the page. **AXIAL AND RADIAL GRADIENTS ARE RECORDED** as PDF
  shadings whose stitching function is built from the gradient's stops; a gradient with PER-STOP ALPHA is refused
  (no alpha channel in a DeviceRGB function, and no soft mask is written). **`CGContextDrawShading` IS RECORDED**
  for a DeviceGray, DeviceRGB or DeviceCMYK shade: its CGFunction is sampled into a type 0 function and the
  geometry becomes a type 2 or 3 shading. **COLORED TILING PATTERNS ARE RECORDED**: the caller's `drawPattern` is
  run into a pattern content stream (with its own `/Resources`, including whatever the tile uses), and the page
  selects it with `/Pattern cs` and `scn` or `SCN`; this renderer draws one by rasterising the tile once and tiling
  it through a CGPattern. **A ZERO-BLUR SHADOW IS RECORDED** for paths and text: the silhouette is offset, painted
  in the shadow colour and put behind the content. A BLURRED shadow has no PDF primitive and is refused, as is a
  shadow on an image or a shading. **A TRANSPARENCY LAYER IS WRITTEN AS AN ISOLATED TRANSPARENCY GROUP**: its content
  is a Form XObject with `/Group << /S /Transparency /I true >>`, drawn at End with the alpha and blend in effect at
  Begin. The PDF context still REFUSES BY NAME uncolored patterns and clip-to-mask, so a caller knows when a page
  lacks them. **THE RENDERER COMPOSITES A TRANSPARENCY GROUP OFFSCREEN** at device resolution (bounded to 4096
  pixels a side) and draws the result with the alpha and blend in effect at `Do`, so a preview agrees with external
  viewers: a 50%-alpha group is (255,127,127) over white in both. A form without a `/Group` keeps the inline path.
- **EPS IS PRODUCED, NOT INTERPRETED.** `CGEPSContext` — an Agora extension, because Apple publishes no Core
  Graphics EPS context — records the same drawing calls as PostScript: paths, clips, dashes, flat colours, glyph
  outlines, opaque images, and gradients/shadings as Level 3 `shfill` with a stitching function. Alpha, blend modes,
  transparency groups, soft-masked images, blurred shadows and tiling patterns refuse by name (`EPS-REFUSE`), and
  text is outlines rather than an embedded font program. **THERE IS STILL NO POSTSCRIPT INTERPRETER**:
  `CGPSConverter` (PostScript → PDF) and `NSEPSImageRep` remain absent, because either needs one; Ghostscript is a
  test reader, not a dependency (the EPS test SKIPs with 77 when it is not installed).
- **PDF encryption is reported, not decrypted.** `CGPDFDocumentIsEncrypted` answers true and the pages, catalog and
  info refuse by name; `IsUnlocked` is therefore false for an encrypted file and there is no password door.
- **The scanner skips inline images rather than parsing them**, so the renderer does not draw them either. The
  RENDERER also refuses Type 3 fonts, named or vertical CMaps,
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
