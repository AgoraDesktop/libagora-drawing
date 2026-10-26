/*
 * glyph_cache — the glyph bitmap cache: same answer, less work, and never a stale one.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT THIS HOLDS UP. `cg_font_render_glyph_cached` claims three things and each has a case here:
 *
 *   1. A HIT ANSWERS WHAT THE MISS ANSWERED — every coverage byte and every metric, over a sweep of the
 *      parameters the key is made of (device size, pen phase, transform, smoothing, ink polarity). The reference
 *      is the picture the miss produced, copied out, so a hit that answered a neighbour's bitmap would fail.
 *   2. THE CACHE CHANGES NOTHING — the same sweep is run with the budget at zero, so every rendering is a miss,
 *      and the bytes are compared against the cached run's. A cache is only worth having if its presence is
 *      invisible in the output.
 *   3. AN EVICTED ENTRY IS RE-RENDERED, NOT REMEMBERED WRONG — a budget smaller than the working set forces
 *      evictions, and every case is then asked for a second time and compared against its uncached picture.
 *
 * AND THE FONT'S IDENTITY IS THE FOURTH. The struct a `CGFontRef` points at is a heap block, so a released face's
 * address can be handed to the next face; a cache keyed on that pointer without holding a reference would answer
 * the new face with the old face's glyph. The case releases a font that has cached entries and then asks a fresh
 * one for the same glyph: the answer must be a MISS, and the picture must be the same because it is the same face
 * read twice from disk.
 *
 * THE LAST CASE IS THE ONE THE CALLER ACTUALLY RUNS. Everything above drives the rasteriser directly; text goes
 * through `CGContextShowTextAtPoint`, which renders and composites glyph after glyph while the borrowed coverage
 * pointer is live. Drawing a string into one context with a cold cache and into another with the first one's cache
 * warm must produce identical pixels — which is the only way to prove the borrow contract (`cg_font_render_glyph_cached`
 * returns a pointer the caller must not free) does not reach back into a composite that has already run.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFont_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* THE BUNDLED SET, NOT THE HOST'S: `ctest` runs against the build tree, where nothing is installed yet. The macro
 * is the source directory, supplied by CMake; a checkout that lost it fails loudly instead of passing on whichever
 * monospace face the machine happens to have. */
#ifndef AGORA_TEST_FONT_DIR
#define AGORA_TEST_FONT_DIR ""
#endif

static int failures;

static void check(int ok, const char *what, const char *detail)
{
	printf("  [%s] %s%s%s\n", ok ? "ok" : "FAIL", what, (detail != NULL) ? " — " : "",
	       (detail != NULL) ? detail : "");
	if (!ok) {
		failures++;
	}
}

static CGFontRef load_mono(void)
{
	static const char *paths[] = {
		AGORA_TEST_FONT_DIR "/LiberationMono/LiberationMono-Regular.ttf",
		AGORA_TEST_FONT_DIR "/IBMPlexMono/IBMPlexMono-Regular.ttf"
	};
	size_t i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		CGDataProviderRef provider = CGDataProviderCreateWithFilename(paths[i]);
		CGFontRef font;

		if (provider == NULL) {
			continue;
		}
		font = CGFontCreateWithDataProvider(provider);
		CGDataProviderRelease(provider);
		if (font != NULL) {
			return font;
		}
	}
	return NULL;
}

/* ONE RENDERING OF ONE PICTURE, kept so a later rendering can be compared against it byte for byte. */
struct frozen {
	unsigned char *bytes;
	size_t length;
	int width, height, left, top, samples;
	double advance;
};

static void frozen_take(struct frozen *f, const unsigned char *coverage, int width, int height,
			int left, int top, int samples, double advance)
{
	free(f->bytes);
	/* AN EMPTY BOX HAS NO COVERAGE BYTES TO KEEP: the renderer hands back a one-byte buffer for a glyph with no
	 * ink, and its contents are whatever `malloc` had. Comparing it would fail for a reason that is not the
	 * cache's — so the length is zero and the bytes are not compared. */
	f->length = (width > 0 && height > 0) ? (size_t)width * (size_t)height : 0u;
	f->bytes = (unsigned char *)malloc(f->length);
	if (f->bytes != NULL && coverage != NULL) {
		memcpy(f->bytes, coverage, f->length);
	}
	f->width = width;
	f->height = height;
	f->left = left;
	f->top = top;
	f->samples = samples;
	f->advance = advance;
}

/* THE SAME VERSION OF A RENDERING, ANSWERED AS A BOOLEAN SO A FAILURE NAMES THE FIELD THAT DISAGREED. */
static int frozen_equals(const struct frozen *f, const unsigned char *coverage, int width, int height,
			 int left, int top, int samples, double advance, const char **why)
{
	size_t length = (width > 0 && height > 0) ? (size_t)width * (size_t)height : 0u;

	if (width != f->width || height != f->height) {
		*why = "the coverage box changed between renders";
		return 0;
	}
	if (left != f->left || top != f->top) {
		*why = "the ink's bearing changed between renders";
		return 0;
	}
	if (samples != f->samples) {
		*why = "the sample count changed between renders";
		return 0;
	}
	if (advance != f->advance) {
		*why = "the advance changed between renders";
		return 0;
	}
	if (length != f->length) {
		*why = "the coverage length changed between renders";
		return 0;
	}
	if (length == 0) {
		return 1;	/* a glyph with no ink: there is nothing to compare and the metrics above already agreed */
	}
	if (coverage == NULL || f->bytes == NULL) {
		*why = "one render answered no coverage at all";
		return 0;
	}
	if (memcmp(coverage, f->bytes, length) != 0) {
		*why = "the coverage bytes changed between renders";
		return 0;
	}
	return 1;
}

/* A CASE IS A POINT IN THE KEY'S OWN SPACE. The matrix is the identity in most of them — a scale or a rotation
 * belongs in the key too, but the identity is what a caller drawing upright text hands over, and the transform is
 * checked separately (`case_transform`). */
struct render_case {
	unsigned glyph;
	double size;
	double phase_x, phase_y;
	int smoothing;
	int ink;
	/* THE TRANSFORM IS PART OF THE KEY, so a case may vary it: the sweep below is upright text, and two of the
	 * extra cases are a quarter turn (which is also what makes smoothing fall back to grey) and a scale. */
	CGAffineTransform matrix;
};

#define MAX_CASES 64

static int render_case(CGFontRef font, const struct render_case *c, const unsigned char **coverage,
		       int *width, int *height, int *left, int *top, double *advance, int *samples)
{
	CGPoint delta = { (CGFloat)c->phase_x, (CGFloat)c->phase_y };

	return cg_font_render_glyph_cached(font, (CGGlyph)c->glyph, (CGFloat)c->size, c->matrix, delta,
					   c->smoothing, c->ink, coverage, width, height, left, top,
					   advance, samples);
}

static int build_cases(CGFontRef font, struct render_case *cases)
{
	static const double sizes[] = { 9.0, 13.0, 26.4, 41.0 };
	static const double phases[] = { 0.0, 0.25, 0.5, 0.75 };
	int n = 0;
	size_t s, p;

	for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		for (p = 0; p < sizeof(phases) / sizeof(phases[0]); p++) {
			int smoothing;

			for (smoothing = 0; smoothing <= 1; smoothing++) {
				cases[n].glyph = cg_font_glyph_for_unichar(font, 'A');
				cases[n].size = sizes[s];
				cases[n].phase_x = phases[p];
				cases[n].phase_y = phases[p] / 2.0;
				cases[n].smoothing = smoothing;
				cases[n].ink = CG_FONT_INK_DARK;
				cases[n].matrix = CGAffineTransformIdentity;
				n++;
			}
		}
	}
	/* AND THE THREE INK AND GLYPH SHAPES THE SWEEP ABOVE DOES NOT CROSS: no warp at all, the light polarity, a
	 * glyph with a descender, and one with none (a space, whose box is empty and whose entry still has to be
	 * remembered or it is rasterised on every word break). */
	cases[n].glyph = cg_font_glyph_for_unichar(font, 'g');
	cases[n].size = 13.0;
	cases[n].phase_x = 0.0;
	cases[n].phase_y = 0.0;
	cases[n].smoothing = 0;
	cases[n].ink = CG_FONT_INK_NONE;
	cases[n].matrix = CGAffineTransformIdentity;
	n++;
	cases[n].glyph = cg_font_glyph_for_unichar(font, 'W');
	cases[n].size = 13.0;
	cases[n].phase_x = 0.0;
	cases[n].phase_y = 0.0;
	cases[n].smoothing = 0;
	cases[n].ink = CG_FONT_INK_LIGHT;
	cases[n].matrix = CGAffineTransformIdentity;
	n++;
	cases[n].glyph = cg_font_glyph_for_unichar(font, ' ');
	cases[n].size = 13.0;
	cases[n].phase_x = 0.0;
	cases[n].phase_y = 0.0;
	cases[n].smoothing = 0;
	cases[n].ink = CG_FONT_INK_DARK;
	cases[n].matrix = CGAffineTransformIdentity;
	n++;
	/* A TRANSFORM THE ENGINE CANNOT BITMAP-STRIKE: a quarter turn, which is also the shape that makes smoothing
	 * fall back to grey — the answer says what was rendered, so the entry remembers `samples == 1`. */
	cases[n].glyph = cg_font_glyph_for_unichar(font, 'A');
	cases[n].size = 26.4;
	cases[n].phase_x = 0.0;
	cases[n].phase_y = 0.0;
	cases[n].smoothing = 1;
	cases[n].ink = CG_FONT_INK_DARK;
	cases[n].matrix = CGAffineTransformMake(0.0, 1.0, -1.0, 0.0, 0.0, 0.0);
	n++;
	/* AND TWO SCALES, WHICH ARE THE ONE PART OF THE MATRIX A RENDERER USES AFTER DIVIDING IT OUT: half and
	 * double, on the same glyph and size, so the key cannot confuse them with the identity case beside them. */
	cases[n].glyph = cg_font_glyph_for_unichar(font, 'A');
	cases[n].size = 13.0;
	cases[n].phase_x = 0.0;
	cases[n].phase_y = 0.0;
	cases[n].smoothing = 0;
	cases[n].ink = CG_FONT_INK_DARK;
	cases[n].matrix = CGAffineTransformMakeScale(0.5, 0.5);
	n++;
	cases[n].glyph = cg_font_glyph_for_unichar(font, 'A');
	cases[n].size = 13.0;
	cases[n].phase_x = 0.0;
	cases[n].phase_y = 0.0;
	cases[n].smoothing = 0;
	cases[n].ink = CG_FONT_INK_DARK;
	cases[n].matrix = CGAffineTransformMakeScale(2.0, 2.0);
	n++;
	return n;
}

static double now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------------- */
/* The cases                                                                  */
/* ------------------------------------------------------------------------- */

/* 1. A HIT IS THE PICTURE THE MISS MADE. */
static void case_hit_is_the_miss(CGFontRef font, const struct render_case *cases, int count)
{
	struct frozen frozen[MAX_CASES];
	CGGlyphCacheStats stats;
	int i;

	memset(frozen, 0, sizeof(frozen));
	cg_font_glyph_cache_set_budget(4u * 1024u * 1024u);
	printf("\nthe first pass renders, the second is answered from the cache\n");
	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;

		if (!render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
				 &advance, &samples)) {
			check(0, "a case rendered nothing", NULL);
			continue;
		}
		frozen_take(&frozen[i], coverage, w, h, left, top, samples, advance);
	}
	cg_font_glyph_cache_stats(&stats);
	check(stats.misses == (unsigned long)count, "every first render was a miss", NULL);
	check(stats.hits == 0, "no first render was answered from the cache", NULL);
	check(stats.entries == (unsigned)count, "every rendering was kept", NULL);
	check(stats.bytes <= stats.budget, "the cache is inside its budget", NULL);

	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		const char *why = "";
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;
		char label[96];

		if (!render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
				 &advance, &samples)) {
			snprintf(label, sizeof(label), "case %d rendered nothing the second time", i);
			check(0, label, NULL);
			continue;
		}
		snprintf(label, sizeof(label),
			 "case %d (%.1f px, phase %.2f/%.2f, smoothing %d, ink %d) is byte-identical",
			 i, cases[i].size, cases[i].phase_x, cases[i].phase_y, cases[i].smoothing,
			 cases[i].ink);
		check(frozen_equals(&frozen[i], coverage, w, h, left, top, samples, advance, &why), label, why);
	}
	cg_font_glyph_cache_stats(&stats);
	check(stats.hits == (unsigned long)count, "every second render was a hit", NULL);
	check(stats.misses == (unsigned long)count, "the second pass rendered nothing new", NULL);

	/* THE CACHED DOOR AND THE ALLOCATING ONE MUST AGREE: the cache is a rememberer and not a second renderer. */
	{
		const unsigned char *borrowed = NULL;
		unsigned char *owned = NULL;
		int bw = 0, bh = 0, bl = 0, bt = 0, bs = 0;
		int ow = 0, oh = 0, ol = 0, ot = 0, os = 0;
		double ba = 0.0, oa = 0.0;
		int same;

		render_case(font, &cases[1], &borrowed, &bw, &bh, &bl, &bt, &ba, &bs);
		same = cg_font_render_glyph(font, (CGGlyph)cases[1].glyph, (CGFloat)cases[1].size,
					    cases[1].matrix,
					    (CGPoint){ (CGFloat)cases[1].phase_x, (CGFloat)cases[1].phase_y },
					    cases[1].smoothing, cases[1].ink, &owned, &ow, &oh, &ol, &ot,
					    &oa, &os);
		check(same && ow == bw && oh == bh && ol == bl && ot == bt && os == bs && oa == ba
		      && memcmp(owned, borrowed, (size_t)bw * (size_t)bh) == 0,
		      "the cached door answers exactly what cg_font_render_glyph answers", NULL);
		free(owned);
	}

	for (i = 0; i < count; i++) {
		free(frozen[i].bytes);
	}
}

/* 2. THE CACHE IS INVISIBLE: every rendering a miss, and the same bytes as the cached run's. */
static void case_uncached_matches_cached(CGFontRef font, const struct render_case *cases, int count)
{
	struct frozen frozen[MAX_CASES];
	CGGlyphCacheStats stats;
	int i;

	memset(frozen, 0, sizeof(frozen));
	cg_font_glyph_cache_set_budget(4u * 1024u * 1024u);
	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;

		render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
			    &advance, &samples);
		frozen_take(&frozen[i], coverage, w, h, left, top, samples, advance);
	}

	printf("\nwith the budget at zero every rendering is a miss and the pictures are unchanged\n");
	cg_font_glyph_cache_set_budget(0);
	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		const char *why = "";
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;
		char label[96];

		if (!render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
				 &advance, &samples)) {
			check(0, "an uncached render refused a case the cached one answered", NULL);
			continue;
		}
		snprintf(label, sizeof(label), "case %d is identical with caching off", i);
		check(frozen_equals(&frozen[i], coverage, w, h, left, top, samples, advance, &why), label, why);
	}
	cg_font_glyph_cache_stats(&stats);
	check(stats.budget == 0, "the budget really is zero", NULL);
	check(stats.hits == 0, "nothing was answered from the cache", NULL);
	check(stats.misses == (unsigned long)count, "every rendering went to the rasteriser", NULL);
	check(stats.entries == 0, "nothing was kept", NULL);

	for (i = 0; i < count; i++) {
		free(frozen[i].bytes);
	}
}

/* 3. EVICTION IS A RE-RENDER, NOT A WRONG ANSWER. */
static void case_eviction(CGFontRef font, const struct render_case *cases, int count)
{
	struct frozen frozen[MAX_CASES];
	CGGlyphCacheStats stats;
	int i;

	memset(frozen, 0, sizeof(frozen));
	/* THE UNCACHED TRUTH FIRST, so nothing below can be compared against a picture the cache made up. */
	cg_font_glyph_cache_set_budget(0);
	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;

		render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
			    &advance, &samples);
		frozen_take(&frozen[i], coverage, w, h, left, top, samples, advance);
	}

	printf("\na budget smaller than the working set evicts, and an evicted case re-renders identically\n");
	/* 4 KiB BELOW THE WORKING SET ON PURPOSE: the sweep is mostly 13- and 41-pixel pictures, so a handful fit and
	 * the rest must push the oldest ones out. */
	cg_font_glyph_cache_set_budget(4096);
	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;

		render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
			    &advance, &samples);
	}
	cg_font_glyph_cache_stats(&stats);
	check(stats.evictions > 0, "the budget forced evictions rather than growing", NULL);
	check(stats.bytes <= 4096, "the cache stayed inside the 4 KiB budget", NULL);

	/* A SECOND SWEEP IN THE SAME ORDER: most of these are misses (the sweep is longer than the budget), and every
	 * one of them has to reproduce its picture exactly. */
	for (i = 0; i < count; i++) {
		const unsigned char *coverage = NULL;
		const char *why = "";
		int w = 0, h = 0, left = 0, top = 0, samples = 0;
		double advance = 0.0;
		char label[96];

		if (!render_case(font, &cases[i], &coverage, &w, &h, &left, &top,
				 &advance, &samples)) {
			continue;
		}
		snprintf(label, sizeof(label), "case %d survived eviction with its picture intact", i);
		check(frozen_equals(&frozen[i], coverage, w, h, left, top, samples, advance, &why), label, why);
	}
	cg_font_glyph_cache_stats(&stats);
	check(stats.bytes <= 4096, "the second sweep stayed inside the budget too", NULL);

	for (i = 0; i < count; i++) {
		free(frozen[i].bytes);
	}
}

/* 4. A CACHED FONT IS RETAINED, AND IDENTITY IS NOT AN ADDRESS. */
static void case_font_identity(CGFontRef font, const struct render_case *sample)
{
	struct frozen frozen;
	CGGlyphCacheStats stats;
	const unsigned char *coverage = NULL;
	int w = 0, h = 0, left = 0, top = 0, samples = 0;
	double advance = 0.0;
	CGFontRef second;

	printf("\na released face and a fresh one do not share a cache entry\n");
	/* A FLUSH RATHER THAN A BUDGET: `set_budget` is a no-op when the number it is given is the one already in
	 * force, so the entries the string case left behind would still be here and the counts below would be the
	 * whole run's instead of this case's. */
	cg_font_glyph_cache_flush();
	cg_font_glyph_cache_set_budget(4u * 1024u * 1024u);
	render_case(font, sample, &coverage, &w, &h, &left, &top, &advance, &samples);
	frozen_take(&frozen, coverage, w, h, left, top, samples, advance);
	CGFontRelease(font);	/* the cache holds a reference, so the face must still be alive for that entry */

	second = load_mono();
	check(second != NULL, "the same face was loaded a second time", NULL);
	if (second != NULL) {
		const char *why = "";

		coverage = NULL;
		render_case(second, sample, &coverage, &w, &h, &left, &top, &advance, &samples);
		cg_font_glyph_cache_stats(&stats);
		check(stats.misses == 2, "the new font object is a MISS, not a hole-in-one", NULL);
		check(stats.hits == 0, "no entry was served for a different font object", NULL);
		check(stats.entries == 2, "the two fonts hold two entries", NULL);
		check(frozen_equals(&frozen, coverage, w, h, left, top, samples, advance, &why),
		      "the fresh face draws the same glyph the released one did", why);
		CGFontRelease(second);
	}
	free(frozen.bytes);
	/* THE FLUSH IS THE OTHER HALF OF THE REFERENCE: dropping two entries whose fonts have had their caller's
	 * reference released must release the faces too, and a run that crashes here has double-freed one. */
	cg_font_glyph_cache_flush();
	cg_font_glyph_cache_stats(&stats);
	check(stats.entries == 0 && stats.bytes == 0, "flush emptied the cache", NULL);
}

/* 5. THE CALLER'S PATH: a string drawn cold and warm is the same pixels. */
static int draw_string(CGFontRef font, unsigned char *pixels, size_t width, size_t height)
{
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx;
	static const char *text = "Hamburgefonstiv 0123456789 (A) [ag] — 13 px";

	memset(pixels, 0xFF, width * height * 4u);
	ctx = CGBitmapContextCreate(pixels, width, height, 8, width * 4u, rgb,
				    (uint32_t)(kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little));
	CGColorSpaceRelease(rgb);
	if (ctx == NULL) {
		return 0;
	}
	CGContextSetShouldSmoothFonts(ctx, false);
	CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);
	CGContextSetFont(ctx, font);
	CGContextSetFontSize(ctx, 13.0);
	CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
	CGContextShowTextAtPoint(ctx, 3.0, 24.0, text, strlen(text));
	CGContextRelease(ctx);
	return 1;
}

static void case_text_path(CGFontRef font)
{
	const size_t width = 420, height = 40;
	unsigned char *cold = (unsigned char *)malloc(width * height * 4u);
	unsigned char *warm = (unsigned char *)malloc(width * height * 4u);
	CGGlyphCacheStats stats;
	double t0, cold_seconds, warm_seconds;
	int reps, i;

	printf("\na string drawn through CGContext is the same pixels with a cold and a warm cache\n");
	if (cold == NULL || warm == NULL) {
		check(0, "the pixel buffers could not be allocated", NULL);
		free(cold);
		free(warm);
		return;
	}
	cg_font_glyph_cache_set_budget(4u * 1024u * 1024u);
	check(draw_string(font, cold, width, height), "the cold drawing produced a context", NULL);
	cg_font_glyph_cache_stats(&stats);
	check(stats.misses > 0, "the cold drawing rendered its glyphs", NULL);
	check(draw_string(font, warm, width, height), "the warm drawing produced a context", NULL);
	cg_font_glyph_cache_stats(&stats);
	check(stats.hits > 0, "the warm drawing was answered from the cache", NULL);
	check(memcmp(cold, warm, width * height * 4u) == 0, "the two drawings are pixel-identical", NULL);

	/* AND THE MEASUREMENT THE CACHE EXISTS FOR, PRINTED RATHER THAN ASSERTED: one glyph per frame sixty times a
	 * second is a terminal; a clock on a shared machine is not a contract, so a ratio is reported and only the
	 * "the warm loop did no rendering" half is checked. */
	reps = 200;
	/* **THE COLD LOOP FLUSHES BEFORE EVERY DRAW, WHICH IS THE ONLY HONEST "WITHOUT A CACHE" HERE.** A flush once
	 * in front of two hundred identical drawings measures nothing: the first drawing warms the cache and the other
	 * 199 are warm — which is how a first version of this reported 1.01x and was believed for an afternoon. */
	t0 = now_seconds();
	for (i = 0; i < reps; i++) {
		cg_font_glyph_cache_flush();
		draw_string(font, cold, width, height);
	}
	cold_seconds = now_seconds() - t0;
	cg_font_glyph_cache_stats(&stats);
	{
		unsigned long cold_misses = stats.misses;

		t0 = now_seconds();
		for (i = 0; i < reps; i++) {
			draw_string(font, warm, width, height);
		}
		warm_seconds = now_seconds() - t0;
		cg_font_glyph_cache_stats(&stats);
		check(stats.misses == cold_misses, "the warm loop rendered nothing new", NULL);
		printf("  ... %d drawings: %.1f ms cold, %.1f ms warm (%.2fx)\n", reps, cold_seconds * 1e3,
		       warm_seconds * 1e3, (warm_seconds > 0.0) ? cold_seconds / warm_seconds : 0.0);
	}

	free(cold);
	free(warm);
}

int main(void)
{
	struct render_case cases[MAX_CASES];
	CGFontRef font;
	int count;
	CGGlyphCacheStats stats;

	/* THE ENVIRONMENT'S BUDGET IS IRRELEVANT TO EVERY CASE BELOW — each sets its own — but a run with
	 * `CG_GLYPH_CACHE_BYTES=0` in the environment must still pass, so nothing here reads it. */
	unsetenv("CG_GLYPH_CACHE_BYTES");
	font = load_mono();
	if (font == NULL) {
		printf("  [FAIL] no bundled monospace face could be loaded from \"%s\"\n", AGORA_TEST_FONT_DIR);
		return 1;
	}
	count = build_cases(font, cases);
	printf("the glyph bitmap cache: %d cases on %s\n", count, "LiberationMono/IBMPlexMono");

	case_hit_is_the_miss(font, cases, count);
	case_uncached_matches_cached(font, cases, count);
	case_eviction(font, cases, count);
	case_text_path(font);

	/* THE IDENTITY CASE OWNS ITS FONT: it releases the reference this function holds, so it goes last. */
	case_font_identity(font, &cases[1]);

	cg_font_glyph_cache_stats(&stats);
	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
