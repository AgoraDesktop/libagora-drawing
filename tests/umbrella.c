/*
 * umbrella — the Core Graphics umbrella header, measured: it compiles, and it names every header this component
 * installs.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * **THE SAME CENSUS THE APPKIT AND FOUNDATION MAKE, ONE LAYER DOWN, AND IN C.** `CMakeLists.txt` installs `*.h` with
 * a glob, so a header becomes part of the installed surface the moment its file exists; the umbrella is the only
 * thing that says which of them a consumer should include, and it can drift without anything failing to build. This
 * probe fails on the header nobody classified.
 *
 * **IT IS A C FILE, WHICH IS THE OTHER HALF OF THE CLAIM.** Core Graphics is a C API; a program that includes this
 * umbrella from a `.c` file must compile, not merely a `.m` one. The include below is `#include`, and the checks use
 * no Objective-C.
 */
#include <CoreGraphics/CoreGraphics.h>

#include <stdio.h>
#include <string.h>

/* THE HEADERS THAT ARE INSTALLED AND DELIBERATELY NOT IN THE UMBRELLA: a context's saved-state stack, the PDF
 * parser's cursor, the image rep's caches. They are installed for a sibling component's build, not for a program's
 * `#include`. */
static const char *s_internal[] = {
	"CGColorSpace_internal",
	"CGContext_internal",
	"CGDataProvider_internal",
	"CGFont_internal",
	"CGFunction_internal",
	"CGGradient_internal",
	"CGImage_internal",
	"CGPaint_internal",
	"CGPath_internal",
	"CGPattern_internal",
	"CGPDF_internal",
	"CGPDFWriter_internal",
	"CGShading_internal",
	"CGTypeID_internal",
	NULL,
};

static int failed;

static void check(int ok, const char *what)
{
	printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failed++;
	}
}

static int is_internal(const char *name)
{
	int i;

	for (i = 0; s_internal[i] != NULL; i++) {
		if (strcmp(s_internal[i], name) == 0) {
			return 1;
		}
	}
	return 0;
}

/* THE UMBRELLA'S OWN LIST, read as text: the includes ARE the specification. */
#define UMBRELLA_LINE "#include <CoreGraphics/"
#define UMBRELLA_LINE_LEN (sizeof(UMBRELLA_LINE) - 1)
#define UMBRELLA_SUFFIX ".h>"
#define UMBRELLA_SUFFIX_LEN (sizeof(UMBRELLA_SUFFIX) - 1)

static int parse_umbrella(const char **names, int capacity, int *count)
{
	FILE *file = fopen(COREGRAPHICS_UMBRELLA, "r");
	char line[512];
	static char storage[256][64];
	int used = 0;

	if (file == NULL) {
		return 0;
	}
	while (fgets(line, sizeof line, file) != NULL) {
		size_t length = strlen(line);
		size_t body;

		while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
			line[--length] = '\0';
		}
		if (length < UMBRELLA_LINE_LEN + UMBRELLA_SUFFIX_LEN ||
		    strncmp(line, UMBRELLA_LINE, UMBRELLA_LINE_LEN) != 0 ||
		    strcmp(line + length - UMBRELLA_SUFFIX_LEN, UMBRELLA_SUFFIX) != 0) {
			continue;
		}
		body = length - UMBRELLA_LINE_LEN - UMBRELLA_SUFFIX_LEN;
		if (body >= sizeof storage[0] || used >= capacity || used >= 256) {
			continue;
		}
		memcpy(storage[used], line + UMBRELLA_LINE_LEN, body);
		storage[used][body] = '\0';
		names[used] = storage[used];
		used++;
	}
	fclose(file);
	*count = used;
	return 1;
}

int main(void)
{
	const char *umbrella[256];
	int umbrellaCount = 0;
	int installed = 0;
	int unclassified = 0, dangling = 0, both = 0;
	FILE *listing;
	char command[512];
	char name[128];

	check(parse_umbrella(umbrella, 256, &umbrellaCount), "the umbrella's includes can be read");
	printf("umbrella names %d header(s)\n", umbrellaCount);
	check(umbrellaCount > 30, "the umbrella names the surface rather than a sample of it");

	/* THE INSTALLED SET, from the source directory: the same list the glob installs. */
	snprintf(command, sizeof command, "ls %s/*.h | sed -e 's,.*/,,' -e 's,\\.h$,,'", COREGRAPHICS_SOURCE_DIR);
	listing = popen(command, "r");
	check(listing != NULL, "the source directory can be listed");
	if (listing != NULL) {
		while (fgets(name, sizeof name, listing) != NULL) {
			size_t length = strlen(name);
			int named = 0, i;

			while (length > 0 && (name[length - 1] == '\n' || name[length - 1] == '\r')) {
				name[--length] = '\0';
			}
			if (strcmp(name, "CoreGraphics") == 0) {
				continue;	/* the umbrella is not one of the headers it names */
			}
			installed++;
			for (i = 0; i < umbrellaCount; i++) {
				if (strcmp(umbrella[i], name) == 0) {
					named = 1;
					both += is_internal(name) ? 1 : 0;
					break;
				}
			}
			if (!named && !is_internal(name)) {
				printf("       unclassified: %s\n", name);
				unclassified++;
			}
		}
		pclose(listing);
	}
	printf("the source directory holds %d header(s)\n", installed);
	check(installed > 0, "the source directory was actually read");
	check(unclassified == 0, "every installed header is in the umbrella or named as internal");
	check(both == 0, "no header is both public and internal");

	/* EVERY NAME IN THE UMBRELLA EXISTS, so a rename cannot leave a dangling include behind. */
	{
		int i;

		for (i = 0; i < umbrellaCount; i++) {
			char path[512];
			FILE *probe;

			snprintf(path, sizeof path, "%s/%s.h", COREGRAPHICS_SOURCE_DIR, umbrella[i]);
			probe = fopen(path, "r");
			if (probe == NULL) {
				printf("       dangling: %s\n", umbrella[i]);
				dangling++;
			} else {
				fclose(probe);
			}
		}
	}
	check(dangling == 0, "every header the umbrella names exists");

	/* AND THE UMBRELLA ACTUALLY DECLARES THE LAYER. The include above compiled; these say it was not an empty file:
	 * the geometry helpers a C caller writes first, and a colour space that has to be created. */
	{
		CGRect rect = CGRectMake(1.0, 2.0, 30.0, 40.0);
		CGPoint point = CGPointMake(5.0, 6.0);
		CGColorSpaceRef space;

		check(CGRectGetWidth(rect) == 30.0, "CGRectMake and CGRectGetWidth are declared and agree");
		check(CGRectGetMaxY(rect) == 42.0, "the rect's own arithmetic is reachable");
		check(point.x == 5.0 && point.y == 6.0, "CGPointMake is reachable");
		space = CGColorSpaceCreateDeviceRGB();
		check(space != NULL, "a colour space can be created through the umbrella");
		if (space != NULL) {
			CGColorSpaceRelease(space);
		}
		check(CGContextGetTypeID() != 0, "the context's type id is reachable");
		check(CGPDFDocumentGetTypeID() != 0, "the PDF document's type id is reachable");
	}
	printf("%s\n", failed == 0 ? "all checks passed" : "FAILURES");
	return failed == 0 ? 0 : 1;
}
