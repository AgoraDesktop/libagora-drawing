/*
 * CGPDFFont — a PDF font dictionary turned into what the drawing layer needs: a CGFont, glyphs and widths.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **THREE SPACES ARE COLLAPSED HERE, AND THAT IS THE WHOLE JOB.** A PDF byte is a CODE in the content stream; the
 * font dictionary says which GLYPH that code selects and how wide it is; the drawing layer draws a glyph by its
 * engine index. So this file owns: the /Encoding tables (a named base encoding plus a /Differences array), the
 * /Widths and /W metrics, and the embedded font program — and it answers `glyph` and `width` for a code.
 *
 * **AN EMBEDDED PROGRAM WINS, AND A MISSING ONE FALLS BACK BY NAME.** /FontFile, /FontFile2 and /FontFile3 are
 * read through the same filter chain the rest of the reader uses, and FreeType opens Type 1, TrueType and CFF
 * alike; a font with no program is looked up by its /BaseFont (with the `ABCDEF+` subset prefix removed), which
 * is how the standard fourteen reach the bundled faces. A document that embeds a font is therefore rendered with
 * ITS glyphs even when the name would have found a different face.
 *
 * **SIMPLE FONTS RESOLVE ALL 256 CODES AT CREATION**, because a content stream asks about a few dozen of them
 * and a lookup should not re-walk /Differences per glyph. CID fonts keep their /W ranges and resolve per code.
 * Identity-H is the only CMap read: a named CMap stream is a program in its own right, and a vertical one would
 * lay text out along the wrong axis, so both are refused rather than approximated.
 *
 * THE METRICS ARE IN 1/1000 OF AN EM, which is the unit /Widths and /W are written in; the caller multiplies by
 * the font size. A font with no /Widths at all — an unembedded standard face — falls back to the glyph's own
 * advance, which is what a viewer that knows the standard fourteen does.
 */
#ifndef CORE_GRAPHICS_CGPDFFONT_INTERNAL_H
#define CORE_GRAPHICS_CGPDFFONT_INTERNAL_H

#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGPDF_internal.h>

typedef struct cg_pdf_font cg_pdf_font;

/* NULL, with a `CG-REFUSE` line naming the subtype, for a Type 3 font, a vertical CMap or a font whose program
 * will not open and whose name finds nothing. */
cg_pdf_font *cg_pdf_font_create(cg_pdf_doc *doc, cg_pdf_obj *font_dict);
void cg_pdf_font_free(cg_pdf_font *font);

CGFontRef cg_pdf_font_cgfont(cg_pdf_font *font);
const char *cg_pdf_font_base_name(cg_pdf_font *font);

/* 1 for a simple font, 2 for a CID font — the number of bytes one code occupies in the content stream. */
int cg_pdf_font_code_bytes(cg_pdf_font *font);
bool cg_pdf_font_is_cid(cg_pdf_font *font);

/* THE GLYPH A CODE SELECTS. 0 (.notdef) when the encoding defines no glyph for it, never `kCGFontIndexInvalid`:
 * the renderer draws .notdef for a code a font cannot map, which is what a viewer does. */
CGGlyph cg_pdf_font_glyph(cg_pdf_font *font, unsigned code);

/* THE ADVANCE IN 1/1000 EM. Zero for a code the font cannot map. */
double cg_pdf_font_width(cg_pdf_font *font, unsigned code);

#endif /* CORE_GRAPHICS_CGPDFFONT_INTERNAL_H */
