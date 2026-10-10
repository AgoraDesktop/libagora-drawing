/*
 * CGGradientColors — the `CFArray` form of `CGGradientCreateWithColors`.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE ARRAY IS A CORE FOUNDATION ARRAY OF `CGColorRef`, which is Apple's signature: `CGGradientCreateWithColors`
 * takes a `CFArrayRef` whose elements are the colours themselves. The Fiwix tree wrapped each `CGColorRef` in an
 * `NSValue` because its array was a Foundation one and an object array cannot hold a bare pointer; a CFArray
 * holds `const void *`, so the wrapper is gone and the conversion to C is the whole of the change
 * (PROVENANCE.md).
 *
 * WHAT THIS FILE DOES NOT DO IS CONVERT COLOURS. It unwraps and hands an array of `CGColorRef` to
 * `cg_gradient_create_with_colors`, which is the same function the components form calls — so a stop means one
 * thing in this library, whichever door it came through.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGGradient_internal.h>

#include <stdio.h>
#include <stdlib.h>

CGGradientRef CGGradientCreateWithColors(CGColorSpaceRef space, CFArrayRef colors,
					 const CGFloat *locations)
{
	CGColorRef *refs;
	CGGradientRef g;
	CFIndex count;
	CFIndex i;

	if (colors == NULL) {
		fprintf(stderr, "CG-REFUSE: CGGradientCreateWithColors needs an array of colours\n");
		return NULL;
	}
	/* CHECKED HERE RATHER THAN IN THE SHARED BUILDER, because "fewer than two" has a different
	 * cause in this form — an array the caller built short — and because the array's count is
	 * what has to be walked to build the C array below. The builder checks it again, which costs
	 * nothing and keeps that function correct on its own. */
	count = CFArrayGetCount(colors);
	if (count < 2) {
		fprintf(stderr, "CG-REFUSE: a gradient needs at least two stops to interpolate "
				"between; the array holds %ld\n", (long)count);
		return NULL;
	}
	refs = calloc((size_t)count, sizeof(CGColorRef));
	if (refs == NULL) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		CGColorRef element = (CGColorRef)CFArrayGetValueAtIndex(colors, i);

		/* A NULL ELEMENT IS A REFUSAL, NOT A CAST: a gradient stop that is not a colour would draw
		 * something, and something wrong. */
		if (element == NULL) {
			fprintf(stderr, "CG-REFUSE: gradient stop %ld is NULL — the array this function "
					"takes holds CGColorRef; see CGGradient.h\n", (long)i);
			free(refs);
			return NULL;
		}
		refs[i] = element;
	}
	g = cg_gradient_create_with_colors(space, refs, (size_t)count, locations);
	free(refs);
	return g;
}
