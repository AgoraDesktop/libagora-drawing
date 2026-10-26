/*
 * glyph_advances — the per-font advance cache: same answers, no second load, and the all-or-nothing rule kept.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT THIS HOLDS UP. `CGFontGetGlyphAdvances` is a door every layer above this library measures text with —
 * Core Text's layout asks it for every run, the PDF writer for every glyph it places, and the text drawing path
 * for every glyph it advances past. An advance is in FONT UNITS, so it is the same answer at every size, pen
 * phase and transform, which is what makes it worth remembering and what makes the key small: the face and the
 * glyph. The answers are now recorded per font, and this case proves four things about that:
 *
 *   1. THE ANSWER DID NOT CHANGE. Every value is compared against a SECOND font object opened from the same file,
 *      whose cache is cold and therefore answers from the engine. A cache that remembered a neighbour's advance
 *      would fail here.
 *   2. THE SECOND ASK IS THE CACHED ONE. The per-font counters are deterministic, so this asks them rather than
 *      believing a clock: the first batch is `loads == glyphs, hits == 0`, the second is `hits == glyphs` with the
 *      load count unchanged.
 *   3. THE ALL-OR-NOTHING PROMISE SURVIVES. A batch containing a glyph the face has not got answers FALSE and
 *      leaves the caller's array exactly as it found it — a sentinel in every slot proves it — while the glyphs
 *      that DID load in that batch are remembered, because "this glyph has an advance" does not stop being true
 *      because a later one in the same call was bad.
 *   4. A BATCH AND A TRICKLE AGREE. One call for 26 glyphs answers what 26 calls of one glyph each answer, which is
 *      the property the pass-one/pass-two shape exists to give.
 */
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFont_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static double now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* THE SAME QUESTION ASKED OF A FONT THE CACHE HAS NEVER SEEN, which is the only honest reference: a value the
 * engine answered without anything to remember it in. */
static int reference_advance(CGFontRef font, CGGlyph glyph)
{
	int advance = -1;

	if (!CGFontGetGlyphAdvances(font, &glyph, 1, &advance)) {
		return -1;
	}
	return advance;
}

int main(void)
{
	CGFontRef font, reference;
	CGGlyph run[26];
	CGFontAdvanceStats stats;
	int advances[26];
	int i;
	int same = 1;
	double t0, t1, cold, warm;

	font = load_mono();
	reference = load_mono();
	if (font == NULL || reference == NULL) {
		printf("  [FAIL] no bundled monospace face could be loaded from \"%s\"\n", AGORA_TEST_FONT_DIR);
		return 1;
	}
	for (i = 0; i < 26; i++) {
		run[i] = (CGGlyph)cg_font_glyph_for_unichar(font, (unsigned)('A' + i));
	}
	printf("the advance cache, on a face with %zu glyphs\n", CGFontGetNumberOfGlyphs(font));

	/* ---- 1. THE ANSWER DID NOT CHANGE ------------------------------------------------ */
	memset(advances, 0, sizeof(advances));
	check(CGFontGetGlyphAdvances(font, run, 26, advances), "the first batch answers", NULL);
	for (i = 0; i < 26; i++) {
		int want = reference_advance(reference, run[i]);

		if (advances[i] != want) {
			char label[96];

			snprintf(label, sizeof(label), "glyph %u: cached %d, engine %d", (unsigned)run[i],
				 advances[i], want);
			check(0, "every advance matches a font whose cache is cold", label);
			same = 0;
		}
	}
	if (same) {
		check(1, "every advance matches a font whose cache is cold", NULL);
	}
	/* AND THE REFERENCE FONT'S OWN COUNTERS ARE THE CONTROL: it loaded its glyphs, and this font's cache did not
	 * get them by accident. */
	cg_font_advance_cache_stats(reference, &stats);
	check(stats.loads >= 26 && stats.hits == 0, "the reference font answered from the engine", NULL);

	/* ---- 2. THE SECOND ASK IS THE CACHED ONE ----------------------------------------- */
	cg_font_advance_cache_stats(font, &stats);
	check(stats.loads == 26, "the first batch loaded each glyph exactly once", NULL);
	check(stats.hits == 0, "and took nothing from the cache", NULL);
	check(stats.known == 26, "and recorded all 26", NULL);
	check(stats.capacity == (unsigned)CGFontGetNumberOfGlyphs(font),
	      "the array is the face's glyph count", NULL);

	{
		int second[26];

		memset(second, 0, sizeof(second));
		check(CGFontGetGlyphAdvances(font, run, 26, second), "the second batch answers", NULL);
		check(memcmp(advances, second, sizeof(advances)) == 0, "and answers the same numbers", NULL);
	}
	cg_font_advance_cache_stats(font, &stats);
	check(stats.hits == 26, "the second batch was answered from the cache", NULL);
	check(stats.loads == 26, "the second batch loaded nothing", NULL);

	/* THE MEASUREMENT, PRINTED AND NOT ASSERTED: a clock on a shared machine is not a contract. */
	{
		int reps = 20000;
		int advance = 0;
		CGGlyph one = run[0];
		CGFontRef cold_font = load_mono();

		if (cold_font != NULL) {
			t0 = now_seconds();
			(void)CGFontGetGlyphAdvances(cold_font, &one, 1, &advance);
			cold = now_seconds() - t0;
			t0 = now_seconds();
			for (i = 0; i < reps; i++) {
				(void)CGFontGetGlyphAdvances(font, &one, 1, &advance);
			}
			warm = (now_seconds() - t0) / (double)reps;
			printf("  ... one cold glyph: %.0f ns; a warm ask: %.0f ns (%.0fx)\n", cold * 1e9,
			       warm * 1e9, (warm > 0.0) ? cold / warm : 0.0);
			CGFontRelease(cold_font);
		}
	}

	/* ---- 4. A BATCH AND A TRICKLE AGREE (before the refusal case, which perturbs nothing) ---- */
	{
		int trickle[26];
		int agree = 1;

		for (i = 0; i < 26; i++) {
			trickle[i] = reference_advance(reference, run[i]);
			if (trickle[i] != advances[i]) {
				agree = 0;
			}
		}
		check(agree, "26 calls of one glyph agree with one call of 26", NULL);
	}

	/* ---- 3. THE ALL-OR-NOTHING PROMISE ----------------------------------------------- */
	{
		/* A FRESH FONT, so "what got recorded" is a number this case owns rather than one 26 earlier asks chose.
		 * The batch is two glyphs the face has, one it has not, and a fourth it has: the promise is that the
		 * CALLER's array is untouched, and the design is that the two which loaded are still remembered, because
		 * "this glyph has an advance" does not stop being true because a later one was bad. */
		CGFontRef partial = load_mono();
		CGGlyph bad[4];
		int sentinel[4] = { 111, 222, 333, 444 };
		CGFontAdvanceStats after;
		int handled;
		int again[1] = { 0 };

		check(partial != NULL, "a third font opened for the refusal case", NULL);
		if (partial != NULL) {
			bad[0] = (CGGlyph)cg_font_glyph_for_unichar(partial, 'A');
			bad[1] = (CGGlyph)cg_font_glyph_for_unichar(partial, 'B');
			bad[2] = (CGGlyph)(CGFontGetNumberOfGlyphs(partial) + 5);
			bad[3] = (CGGlyph)cg_font_glyph_for_unichar(partial, 'C');
			handled = CGFontGetGlyphAdvances(partial, bad, 4, sentinel);
			check(handled == 0, "a batch with a glyph the face has not got is refused", NULL);
			check(sentinel[0] == 111 && sentinel[1] == 222 && sentinel[2] == 333
			      && sentinel[3] == 444, "and the caller's array is untouched", NULL);
			cg_font_advance_cache_stats(partial, &after);
			check(after.known == 2, "the two glyphs that did load are remembered", NULL);
			check(after.loads == 2, "and the two that did not were not counted as loads", NULL);
			check(CGFontGetGlyphAdvances(partial, &bad[0], 1, again) && again[0] == advances[0],
			      "asking for one of them again answers the same number", NULL);
			cg_font_advance_cache_stats(partial, &after);
			check(after.hits == 1 && after.loads == 2, "and answers it from the cache", NULL);
			CGFontRelease(partial);
		}
	}

	/* ---- AND WITH NOTHING TO ASK, NOTHING HAPPENS ------------------------------------- */
	check(CGFontGetGlyphAdvances(font, run, 0, advances), "an empty batch answers", NULL);

	/* ---- A SECOND FONT OBJECT IS A SECOND CACHE -------------------------------------- */
	cg_font_advance_cache_stats(reference, &stats);
	check(stats.loads >= 26, "the reference font's cache is its own", NULL);

	CGFontRelease(font);
	CGFontRelease(reference);
	printf("\n%d failure(s)\n", failures);
	return failures == 0 ? 0 : 1;
}
