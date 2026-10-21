/*
 * fonts — the bundled default set and the macOS compatibility names.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed. SPDX-License-Identifier: MIT
 *
 * WHAT THIS HOLDS UP. `fonts/PROVENANCE.md` says which open face fills each macOS role, and `CGFontAliases.c`
 * says what a request for a macOS name resolves to. Both are claims until a lookup proves them, so this case
 * points `FN_FONT_PATH` at the source tree's own `fonts/` directory and asks the actual door — the same
 * `CGFontCreateWithFontName` that every Core Text and AppKit name goes through.
 *
 * IT ALSO PINS THE ORDER THE ALIAS TABLE IS NOT ALLOWED TO CHANGE: an exact name is searched first, and a
 * substitute is consulted only when that search is empty. The "unknown name" case is the control — if the table
 * ever starts answering names it was not given, that case fails rather than quietly substituting something.
 *
 * THE TEST SETS `FN_FONT_PATH` RATHER THAN ASSUMING AN INSTALL because `ctest` runs against the build tree. The
 * macro is the source directory, supplied by CMake; if it is missing the case cannot run and says so instead of
 * passing for the wrong reason.
 */
#include <CoreGraphics/CGFont.h>
#include <CoreFoundation/CFString.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int failed;

static void check(int ok, const char *what, const char *detail)
{
	printf("  [%s] %s%s%s\n", ok ? "ok" : "FAIL", what,
	       (detail != NULL) ? " — " : "", (detail != NULL) ? detail : "");
	if (!ok) {
		failed++;
	}
}

/* THE POSTSCRIPT NAME OF A FACE, INTO A C BUFFER. A name that is absent is an empty string, so a caller can
 * compare it without a second NULL test. */
static void postscript_name(CGFontRef font, char *out, size_t size)
{
	CFStringRef name;

	out[0] = '\0';
	if (font == NULL || size == 0) {
		return;
	}
	name = CGFontCopyPostScriptName(font);
	if (name != NULL) {
		if (!CFStringGetCString(name, out, (CFIndex)size, kCFStringEncodingUTF8)) {
			out[0] = '\0';
		}
		CFRelease(name);
	}
}

/* THE NAME DOOR TAKES A `CFStringRef`, AND THE TEST DOES NOT KEEP ONE: this helper makes the string, asks, and
 * releases the string, so the case below reads as the request it is making rather than as a CF ownership exercise. */
static CGFontRef create_named(const char *name)
{
	CFStringRef string = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CGFontRef font = (string != NULL) ? CGFontCreateWithFontName(string) : NULL;

	if (string != NULL) {
		CFRelease(string);
	}
	return font;
}

/* ONE REQUEST: the face must resolve, and its PostScript name must START WITH the family's real spelling. The
 * prefix rather than an exact spelling is deliberate — the style suffix is the face's business, and this case is
 * about the family the request landed on. */
static void expect_family(const char *request, const char *postscript_prefix)
{
	char name[256];
	CGFontRef font = create_named(request);
	char detail[512];

	postscript_name(font, name, sizeof name);
	snprintf(detail, sizeof detail, "%s -> %s", request, name[0] != '\0' ? name : "(none)");
	check(font != NULL, "the request resolves", detail);
	check(font == NULL || strncasecmp(name, postscript_prefix, strlen(postscript_prefix)) == 0,
	      "and it is the expected family", detail);
	if (font != NULL) {
		CGFontRelease(font);
	}
}

int main(void)
{
	const char *dir = AGORA_TEST_FONT_DIR;
	char buffer[512];
	CGFontRef inter;
	CFStringRef full;

	if (dir == NULL || dir[0] == '\0') {
		printf("  [FAIL] AGORA_TEST_FONT_DIR is not defined, so the bundled set was not exercised\n");
		return 1;
	}
	if (setenv("FN_FONT_PATH", dir, 1) != 0) {
		printf("  [FAIL] FN_FONT_PATH could not be set to %s\n", dir);
		return 1;
	}
	printf("  (font directory: %s)\n", dir);

	/* ---- the set itself, by its real names ------------------------------------------------ */
	expect_family("Figtree", "Figtree-");
	expect_family("Inter", "Inter-");
	expect_family("Inter Display", "InterDisplay-");
	expect_family("Source Serif 4", "SourceSerif4-");
	expect_family("IBM Plex Mono", "IBMPlexMono-");
	expect_family("Liberation Sans", "LiberationSans");
	expect_family("Liberation Serif", "LiberationSerif");
	expect_family("Liberation Mono", "LiberationMono");
	expect_family("Gelasio", "Gelasio-");
	expect_family("Jost", "Jost");
	expect_family("Mulish", "Mulish-");
	expect_family("Nunito", "Nunito-");
	expect_family("Libre Baskerville", "LibreBaskerville-");
	expect_family("EB Garamond", "EBGaramond-");
	expect_family("Cinzel", "Cinzel-");
	expect_family("Zilla Slab", "ZillaSlab-");
	expect_family("Fira Sans", "FiraSans-");
	expect_family("Comic Neue", "ComicNeue-");
	expect_family("Pinyon Script", "PinyonScript-");
	expect_family("Pacifico", "Pacifico-");
	expect_family("Patrick Hand", "PatrickHand-");
	expect_family("Anton", "Anton-");
	expect_family("Special Elite", "SpecialElite-");
	expect_family("Noto Sans Symbols 2", "NotoSansSymbols2-");
	expect_family("Noto Sans Math", "NotoSansMath-");
	expect_family("Noto Emoji", "NotoEmoji-");

	/* ---- the macOS names, through the compatibility table -------------------------------- */
	expect_family("SF Pro Text", "Figtree-");
	expect_family("SF Pro Display", "Figtree-");
	expect_family("system-ui", "Figtree-");
	expect_family("Helvetica", "LiberationSans");
	expect_family("Arial", "LiberationSans");
	expect_family("Times New Roman", "LiberationSerif");
	expect_family("Courier New", "LiberationMono");
	expect_family("Menlo", "IBMPlexMono-");
	expect_family("SF Mono", "IBMPlexMono-");
	expect_family("New York", "SourceSerif4-");
	expect_family("Georgia", "Gelasio-");
	expect_family("Futura", "Jost");
	expect_family("Avenir", "Mulish-");
	expect_family("Baskerville", "LibreBaskerville-");
	expect_family("SF Pro Rounded", "Nunito-");
	expect_family("Apple Symbols", "NotoSansSymbols2-");
	expect_family("Apple Color Emoji", "NotoEmoji-");

	/* A FULL NAME STILL CARRIES ITS STYLE, which is why the table may answer with one. */
	expect_family("Helvetica-Bold", "LiberationSans-Bold");
	expect_family("Times-Bold", "LiberationSerif-Bold");
	expect_family("Inter Bold", "Inter-Bold");

	/* ---- and the control: an unknown name is still unknown ------------------------------- */
	inter = create_named("NoSuchAgoraFace");
	check(inter == NULL, "an unknown name still answers NULL rather than a substitute", NULL);
	if (inter != NULL) {
		CGFontRelease(inter);
	}

	/* THE FAMILY NAME IS THE TYPOGRAPHIC ONE, not the legacy nameID 1 that spells a style out. */
	inter = create_named("Inter SemiBold");
	check(inter != NULL, "Inter SemiBold resolves as a full name", NULL);
	if (inter != NULL) {
		full = CGFontCopyFullName(inter);
		buffer[0] = '\0';
		if (full != NULL) {
			CFStringGetCString(full, buffer, (CFIndex)sizeof buffer, kCFStringEncodingUTF8);
			CFRelease(full);
		}
		check(strstr(buffer, "SemiBold") != NULL, "and the face says SemiBold", buffer);
		CGFontRelease(inter);
	}

	printf("%s: %d failure(s)\n", failed == 0 ? "PASS" : "FAIL", failed);
	return failed == 0 ? 0 : 1;
}
