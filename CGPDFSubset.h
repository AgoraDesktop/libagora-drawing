/*
 * CGPDFSubset — a TrueType program reduced to the glyphs a document used, with the glyphs renumbered.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **WHY RENUMBER AND NOT MERELY BLANK.** Emptying unused `glyf` data leaves `loca` and `hmtx` as large as the
 * original face: a 4,000-glyph font still pays 16 KB for each, and a CJK face's 65,000 glyphs pay a quarter of a
 * megabyte before a single outline. Compact IDs are what makes a two-glyph subset two glyphs wide. The writer
 * therefore numbers the glyphs it uses through CIDs and gives the reader a `/CIDToGIDMap`; this file produces the
 * program whose glyph order that map names.
 *
 * **THE CLOSURE IS THE PART THAT IS EASY TO MISS.** A composite glyph (`é` is `e` plus an acute) references other
 * glyphs by index, and those components are drawn even though the caller never selected them. They are pulled into
 * the subset and their component records are rewritten to the new numbers; without both halves the accented glyph
 * renders as empty space.
 *
 * **WHAT IS KEPT:** `head`, `hhea`, `maxp`, `loca`, `glyf`, `hmtx`, the hinting tables (`cvt `, `fpgm`, `prep`,
 * `gasp`) and `name`/`OS/2` for identification. **WHAT IS DROPPED:** `cmap` (PDF's Identity-H plus the
 * `/CIDToGIDMap` is the mapping that matters), `post`, `GSUB`, `GPOS`, `kern` and everything else — the writer
 * places every glyph explicitly, so shaping and kerning tables cannot affect the page. A subset is not a font for
 * text editing; it is a font for THIS page, which is what a PDF embeds.
 *
 * THE OUTPUT IS A SINGLE-FONT SFNT with long `loca` offsets (simpler than tracking the short form's even-offset
 * rule), every table four-byte aligned, per-table checksums computed and the `head` check-sum adjustment applied.
 */
#ifndef CORE_GRAPHICS_CGPDFSUBSET_INTERNAL_H
#define CORE_GRAPHICS_CGPDFSUBSET_INTERNAL_H

#include <CoreGraphics/CGFont.h>

#include <stddef.h>

typedef struct cg_pdf_subset cg_pdf_subset;

/* NULL when the face's program cannot be read as an SFNT with `glyf` outlines. `glyphs` are OLD glyph indices;
 * the subset always keeps `.notdef` and the composite closure. */
cg_pdf_subset *cg_pdf_subset_create(CGFontRef font, const unsigned *glyphs, size_t count);
void cg_pdf_subset_free(cg_pdf_subset *subset);

/* THE NEW INDEX FOR AN OLD GLYPH, or 0xFFFFFFFF when the glyph is not in the subset. */
unsigned cg_pdf_subset_new_gid(cg_pdf_subset *subset, unsigned old_gid);
size_t cg_pdf_subset_glyph_count(cg_pdf_subset *subset);
const unsigned char *cg_pdf_subset_data(cg_pdf_subset *subset, size_t *length);

#endif /* CORE_GRAPHICS_CGPDFSUBSET_INTERNAL_H */
