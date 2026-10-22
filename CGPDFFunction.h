/*
 * CGPDFFunction — the sampled and exponential functions PDF uses for shadings and tint transforms.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **A PDF FUNCTION IS A MAPPING FROM m NUMBERS TO n NUMBERS**, and the format has four types. Three are data —
 * type 0 samples a table, type 2 is `C0 + x^N (C1 - C0)`, type 3 stitches other functions across /Bounds — and
 * type 4 is a PostScript calculator, which is a PROGRAM. This file implements the three data types and refuses
 * the fourth by name: a PostScript interpreter is not something to grow inside a renderer, and a shading that
 * needs one says so instead of drawing the wrong ramp.
 *
 * **THE DOMAIN AND RANGE ARE PART OF THE EVALUATION, NOT THE CALLER'S PROBLEM.** Every input is clipped to
 * /Domain before it is used, and every output is clipped to /Range after, which is what makes a sampled table
 * with a coarse /Size still interpolate to a legal value.
 *
 * THE SAMPLED TYPE INTERPOLATES LINEARLY IN ONE AND TWO DIMENSIONS and takes the nearest sample beyond that. A
 * shading's function is one-dimensional; a DeviceN tint transform can be four- or more, where a reader that
 * interpolated would need 2^4 corners — and a nearest sample of a monotone transform is within one table step.
 */
#ifndef CORE_GRAPHICS_CGPDFFUNCTION_INTERNAL_H
#define CORE_GRAPHICS_CGPDFFUNCTION_INTERNAL_H

#include <CoreGraphics/CGPDF_internal.h>

typedef struct cg_pdf_function cg_pdf_function;

/* NULL, with a `CG-REFUSE` line, for a PostScript calculator, a missing stream, or a table that does not hold
 * the samples its /Size promises. */
cg_pdf_function *cg_pdf_function_create(cg_pdf_doc *doc, cg_pdf_obj *dict);
void cg_pdf_function_free(cg_pdf_function *function);

size_t cg_pdf_function_input_count(cg_pdf_function *function);
size_t cg_pdf_function_output_count(cg_pdf_function *function);

/* TRUE WHEN THE FUNCTION PRODUCED `outputs` VALUES. `inputs` must hold what `input_count` says. */
bool cg_pdf_function_eval(cg_pdf_function *function, const double *inputs, double *outputs);

#endif /* CORE_GRAPHICS_CGPDFFUNCTION_INTERNAL_H */
