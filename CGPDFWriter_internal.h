/*
 * CGPDFWriter_internal — the recorder behind `CGPDFContext`, and the seam `CGContext.c` calls into.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **A PDF CONTEXT IS A CONTEXT WITH NO SURFACE.** Its `CGContext` has a 1×1 bitmap only so the fifty doors that
 * ask `c->data != NULL` keep their ordinary shape; every door that would PAINT is intercepted here and writes
 * content-stream operators instead. So the writer is not a second drawing engine: it observes the same calls the
 * rasterizer sees and says them again in PDF.
 *
 * **THE PAINT STATE TRAVELS WITH EACH PAINT.** Rather than mirror every setter into the content stream — and
 * then have to keep two state machines in step across `q`/`Q` — the writer emits the colour, line state and
 * effective alpha immediately before the operator that uses them. `CGContext.c` fills `cg_pdf_paint_state` from
 * its own state at the moment of painting, which is the only moment at which those values are known to be the
 * ones the caller meant.
 *
 * WHAT IS RECORDED: `q`/`Q`, `cm`, path construction and painting, clips, images (with an /SMask when the image
 * has alpha), and text as GLYPH OUTLINES. WHAT IS REFUSED BY NAME: gradients and shadings, patterns, shadows,
 * transparency layers and clip-to-mask. Text is vector and correct but not selectable — embedding a font program
 * is a later landing, and calling an outline a glyph would be the sort of lie this tree refuses.
 */
#ifndef CORE_GRAPHICS_CGPDFWRITER_INTERNAL_H
#define CORE_GRAPHICS_CGPDFWRITER_INTERNAL_H

#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGPath.h>

#include <stdbool.h>
#include <stddef.h>

struct cg_pdf_writer;

typedef struct cg_pdf_paint_state {
	CGFloat fill[4];	/* rgba; [3] is the colour's own alpha */
	CGFloat stroke[4];
	CGFloat alpha;		/* the context's, multiplied at paint time */
	CGBlendMode blend;
	CGFloat line_width;
	CGFloat miter_limit;
	int cap;
	int join;
	CGFloat dash[16];
	size_t dash_count;
	CGFloat dash_phase;
} cg_pdf_paint_state;

enum {
	CG_PDF_PAINT_FILL = 0,
	CG_PDF_PAINT_EOFILL,
	CG_PDF_PAINT_STROKE,
	CG_PDF_PAINT_FILLSTROKE,
	CG_PDF_PAINT_EOFILLSTROKE
};

struct cg_pdf_writer *cg_pdf_writer_create(CGDataConsumerRef consumer, const CGRect *mediaBox,
					   CFDictionaryRef auxiliaryInfo);
void cg_pdf_writer_free(struct cg_pdf_writer *writer);

bool cg_pdf_writer_begin_page(struct cg_pdf_writer *writer, const CGRect *mediaBox);

/* THE FIVE BOXES. The media box is set by `begin_page`; the other four are optional and are stored for the
 * page dictionary. See CGPDFContext.h for the page-info keys. */
enum {
	CG_PDF_BOX_MEDIA = 0,
	CG_PDF_BOX_CROP,
	CG_PDF_BOX_BLEED,
	CG_PDF_BOX_TRIM,
	CG_PDF_BOX_ART
};
void cg_pdf_writer_set_page_box(struct cg_pdf_writer *writer, int which, CGRect box);
bool cg_pdf_writer_box_from_dict(CFDictionaryRef dict, CFStringRef key, CGRect *out);
void cg_pdf_writer_end_page(struct cg_pdf_writer *writer);
bool cg_pdf_writer_close(struct cg_pdf_writer *writer);
void cg_pdf_writer_set_metadata(struct cg_pdf_writer *writer, CFDataRef metadata);
size_t cg_pdf_writer_page_count(struct cg_pdf_writer *writer);

void cg_pdf_writer_save(struct cg_pdf_writer *writer);
void cg_pdf_writer_restore(struct cg_pdf_writer *writer);
void cg_pdf_writer_ctm(struct cg_pdf_writer *writer, CGAffineTransform t);

void cg_pdf_writer_paint_path(struct cg_pdf_writer *writer, CGPathRef path, int operation,
			      const cg_pdf_paint_state *paint);
void cg_pdf_writer_clip_path(struct cg_pdf_writer *writer, CGPathRef path, bool even_odd);
void cg_pdf_writer_clip_rect(struct cg_pdf_writer *writer, CGRect rect);
void cg_pdf_writer_clear_rect(struct cg_pdf_writer *writer, CGRect rect);
void cg_pdf_writer_image(struct cg_pdf_writer *writer, CGRect rect, CGImageRef image,
			 const cg_pdf_paint_state *paint);
void cg_pdf_writer_refuse(struct cg_pdf_writer *writer, const char *what);

/* **A GLYPH AS TEXT, WHEN THE FACE CAN BE EMBEDDED.** True for a font this writer embedded (a TrueType/`glyf`
 * program as a Type0/CIDFontType2 font, Identity-H) and false otherwise — a CFF or Type 1 face, or a face with
 * no program behind it — in which case the caller draws the glyph's OUTLINE instead. `mode` is the text drawing
 * mode's number: 0 fill, 1 stroke, 2 fill+stroke. */
bool cg_pdf_writer_text_glyph(struct cg_pdf_writer *writer, CGFontRef font, CGGlyph glyph, CGPoint pen,
			      CGFloat font_size, CGAffineTransform text_matrix, int mode,
			      const cg_pdf_paint_state *paint);

/* A CONTEXT WITH NO SURFACE, owned by the caller and pointing at `writer` (which the context then owns too). */
CGContextRef cg_context_create_pdf(struct cg_pdf_writer *writer);
struct cg_pdf_writer *cg_context_pdf_writer(CGContextRef context);

#endif /* CORE_GRAPHICS_CGPDFWRITER_INTERNAL_H */
