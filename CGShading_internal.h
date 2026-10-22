/*
 * CGShading_internal.h — the one question a context asks a shading.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * NOT INSTALLED, NOT INCLUDED BY ANY PUBLIC HEADER, named the way the other internal headers are.
 *
 * ONE FUNCTION, AND THE GRADIENT HAS THREE BECAUSE THE GRADIENT'S CALLER CHOOSES THE GEOMETRY AT
 * DRAW TIME. A shading does not: `CGShadingCreateAxial` and `CGShadingCreateRadial` are two
 * constructors that fix the geometry inside the object, so the context's draw verb has nothing to
 * select and asks one question — what colour is USER-space (x, y)? — exactly as the paint evaluator
 * wants to be asked. That the geometry is stored rather than passed is the only structural difference
 * between the two paints, and it is why this header is one declaration and CGGradient_internal.h
 * is three.
 */
#ifndef CORE_GRAPHICS_CGSHADING_INTERNAL_H
#define CORE_GRAPHICS_CGSHADING_INTERNAL_H

#include <CoreGraphics/CGGeometry.h>
#include <CoreGraphics/CGShading.h>

/* Writes the colour at USER-space (x, y) into `rgba`, or four zeros when the point lies past an end
 * the shading does not extend — the same "not painted" signal the gradients use, and for the same
 * reason (a transparent sample is a no-op under every operator pixman has). */
void cg_shading_sample(CGShadingRef shading, CGFloat x, CGFloat y, CGFloat rgba[4]);

/* **THE SHADING'S OWN STATE, FOR A CALLER THAT MUST WRITE IT DOWN.** The PDF writer samples the function and
 * emits the geometry as a shading; these doors are the only way it can see either, because Apple's public
 * CGShading.h has no getters. */
CGColorSpaceRef cg_shading_color_space(CGShadingRef shading);
CGFunctionRef cg_shading_function(CGShadingRef shading);
bool cg_shading_is_radial(CGShadingRef shading);
void cg_shading_geometry(CGShadingRef shading, CGPoint *start, CGPoint *end, CGFloat *start_radius,
			 CGFloat *end_radius);
void cg_shading_extend(CGShadingRef shading, bool *extend_start, bool *extend_end);

#endif /* CORE_GRAPHICS_CGSHADING_INTERNAL_H */
