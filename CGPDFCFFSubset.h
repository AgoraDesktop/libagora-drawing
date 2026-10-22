/*
 * CGPDFCFFSubset — a non-CID-keyed CFF program reduced to the glyphs a document used.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **THE PDF WRITER SELECTS CFF GLYPHS BY NAME, NOT BY INDEX**, through a Type 1 font's /Encoding
 * /Differences. That is what makes subsetting a CFF simpler than subsetting `glyf`: the glyph NAMES and
 * their SIDs can stay, while the CharStrings INDEX is rebuilt with only the used glyphs and the charset
 * is rewritten with the same SIDs in the new order. No name, width or /Differences entry changes.
 *
 * WHAT IS REBUILT: the Top DICT's `charset` and `CharStrings` offsets, the CharStrings INDEX, and a
 * format-0 charset. WHAT IS COPIED VERBATIM: the header, Name INDEX, String INDEX, Global Subrs, and
 * the Private DICT together with its Local Subrs (so the Private's relative `Subrs` offset stays valid).
 * The Encoding is DROPPED: PDF's /Differences is the encoding that selects glyphs, and a CFF encoding
 * would be a second, unused mapping.
 *
 * **CONSERVATIVE BY DESIGN.** A CID-keyed CFF, a predefined Expert charset, an FDArray, a `seac`-style
 * `endchar` (which references glyphs by StandardEncoding code, not by the subset's indices), or any
 * malformed structure makes this function return NULL, and the caller embeds the whole program. A
 * subset that is smaller but wrong is worse than a full program that is right.
 */
#ifndef CORE_GRAPHICS_CGPDFCFFSUBSET_INTERNAL_H
#define CORE_GRAPHICS_CGPDFCFFSUBSET_INTERNAL_H

#include <stddef.h>

typedef struct cg_pdf_cff_subset cg_pdf_cff_subset;

/* NULL when the program cannot be subset safely. `glyphs` are OLD glyph indices. */
cg_pdf_cff_subset *cg_pdf_cff_subset_create(const unsigned char *program, size_t length, const unsigned *glyphs,
					    size_t count);
void cg_pdf_cff_subset_free(cg_pdf_cff_subset *subset);
const unsigned char *cg_pdf_cff_subset_data(cg_pdf_cff_subset *subset, size_t *length);

#endif /* CORE_GRAPHICS_CGPDFCFFSUBSET_INTERNAL_H */
