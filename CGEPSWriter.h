/*
 * CGEPSWriter.h — the PostScript half of the page-description recorder.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **EPS IS THE SAME SEAM AS PDF, WITH A DIFFERENT ALPHABET.** `CGContext.c` does not know which format it is
 * recording; it calls the `cg_pdf_writer_*` doors, and those doors dispatch here when the writer was made by
 * `cg_pdf_writer_create_eps`. That is why the EPS writer can be added without a second set of painting hooks
 * and why the two formats cannot drift about WHAT a drawing call means.
 *
 * THE SUBSET IS DELIBERATE. A path, a clip, a dash pattern, a flat colour, a glyph OUTLINE and an opaque image
 * have exact PostScript spellings and are written. A gradient or a shading is written as a PostScript Level 3
 * `shfill` with a sampled type 0 function. What PostScript has no primitive for — alpha, blend modes, soft
 * masks, transparency groups, blurred shadows and tiling patterns — is REFUSED BY NAME (`EPS-REFUSE`) rather
 * than approximated, and the caller's page keeps the part that could be said honestly.
 */
#ifndef CORE_GRAPHICS_CGEPSWRITER_H
#define CORE_GRAPHICS_CGEPSWRITER_H

#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGShading.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGPattern.h>
#include <CoreGraphics/CGPath.h>

#include "CGPDFWriter_internal.h"

#include <CoreFoundation/CFDictionary.h>

struct cg_eps_writer;

struct cg_eps_writer *cg_eps_writer_create(CGDataConsumerRef consumer, const CGRect *bounds,
					   CFDictionaryRef auxiliaryInfo);
void cg_eps_writer_free(struct cg_eps_writer *writer);

bool cg_eps_writer_begin_page(struct cg_eps_writer *writer, const CGRect *mediaBox);
void cg_eps_writer_set_page_box(struct cg_eps_writer *writer, int which, CGRect box);
void cg_eps_writer_end_page(struct cg_eps_writer *writer);
bool cg_eps_writer_close(struct cg_eps_writer *writer);
void cg_eps_writer_set_metadata(struct cg_eps_writer *writer, CFDataRef metadata);
size_t cg_eps_writer_page_count(struct cg_eps_writer *writer);

void cg_eps_writer_save(struct cg_eps_writer *writer);
void cg_eps_writer_restore(struct cg_eps_writer *writer);
void cg_eps_writer_ctm(struct cg_eps_writer *writer, CGAffineTransform t);
void cg_eps_writer_paint_path(struct cg_eps_writer *writer, CGPathRef path, int operation,
			      const cg_pdf_paint_state *paint);
void cg_eps_writer_clip_path(struct cg_eps_writer *writer, CGPathRef path, bool even_odd);
void cg_eps_writer_clip_rect(struct cg_eps_writer *writer, CGRect rect);
void cg_eps_writer_clear_rect(struct cg_eps_writer *writer, CGRect rect);
void cg_eps_writer_image(struct cg_eps_writer *writer, CGRect rect, CGImageRef image,
			 const cg_pdf_paint_state *paint);
void cg_eps_writer_refuse(const char *what);

bool cg_eps_writer_gradient(struct cg_eps_writer *writer, CGGradientRef gradient, bool radial,
			    CGPoint start0, CGFloat radius0, CGPoint start1, CGFloat radius1,
			    CGGradientDrawingOptions options, const cg_pdf_paint_state *paint);
bool cg_eps_writer_shading(struct cg_eps_writer *writer, CGShadingRef shading,
			   const cg_pdf_paint_state *paint);

/* A POSTSCRIPT PATTERN IS NOT WRITTEN YET: `begin` refuses by name and answers false, which the caller reads
 * as "no tile to record", and the paint that would have selected it is skipped by `paint_path`. */
bool cg_eps_writer_begin_pattern(struct cg_eps_writer *writer, CGPatternRef pattern);
void cg_eps_writer_end_pattern(struct cg_eps_writer *writer, CGPatternRef pattern);

/* TRANSPARENCY HAS NO POSTSCRIPT PRIMITIVE. `begin` refuses a non-opaque or blended group by name and answers
 * false, which makes the caller draw the content inline; an opaque Normal group is flattened exactly, because
 * that is what it was. */
bool cg_eps_writer_begin_group(struct cg_eps_writer *writer, CGRect bbox, CGFloat alpha, CGBlendMode blend);
bool cg_eps_writer_in_group(struct cg_eps_writer *writer);
void cg_eps_writer_end_group(struct cg_eps_writer *writer);

/* FALSE MEANS "DRAW THE OUTLINE": EPS keeps no font program in this landing, so `CGContext.c`'s existing
 * outline fallback is the text path. */
bool cg_eps_writer_text_glyph(struct cg_eps_writer *writer, CGFontRef font, CGGlyph glyph, CGPoint pen,
			      CGFloat font_size, CGAffineTransform text_matrix, int mode,
			      const cg_pdf_paint_state *paint);

#endif /* CORE_GRAPHICS_CGEPSWRITER_H */
