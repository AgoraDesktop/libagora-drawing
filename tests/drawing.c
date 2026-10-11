/*
 * drawing — libagora-drawing's own test suite.
 *
 * WHY THIS EXISTS AT ALL: until it did, NOTHING IN THIS TREE HELD THIS LIBRARY. Every defect that turned
 * up in it was found by a throwaway probe in /tmp — the CFEqual segfault that made
 * CGColorSpaceCreateWithName crash on kCGColorSpaceAdobeRGB1998 among them — which means the verification
 * existed only as long as the probe file did, and nothing would notice the defect coming back. The
 * bridge's suite in libagora-base is a different thing: it tests the two libraries TOGETHER and knows
 * nothing about CoreGraphics' own surface.
 *
 * ONE CHILD PER CASE, COPIED FROM THAT SUITE BECAUSE THE REASON IS THE SAME: three different things can
 * end a case — a wrong answer (counted), an assertion, and a CRASH, which no handler can see. A crash in a
 * single-process run loses every case after it; a child costs one case and the tally survives. The counts
 * cannot cross the fork, so the child reports through its exit status and the parent counts CASES, while
 * the FAIL detail lines still come from the child on the same stderr.
 *
 * THE CASES ARE THE ONES THAT WERE ONLY EVER VERIFIED BY HAND, and each one names the thing it is
 * protecting rather than asserting a number for its own sake: the two promises CGTypeID.c makes are
 * stability and uniqueness, NOT particular ids — so a reshuffle of the table must not fail this suite,
 * while two classes sharing an id must.
 */
#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGColor.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGFont.h>
#include <CoreGraphics/CGFunction.h>
#include <CoreGraphics/CGGradient.h>
#include <CoreGraphics/CGImage.h>
#include <CoreGraphics/CGLayer.h>
#include <CoreGraphics/CGPath.h>
#include <CoreGraphics/CGPattern.h>
#include <CoreGraphics/CGShading.h>

#include <CoreFoundation/CoreFoundation.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

static void check(int ok, const char *fmt, ...)
{
	va_list ap;

	g_checks++;
	if (ok) {
		return;
	}
	g_fail++;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* the colour spaces, by name                                                */
/* ------------------------------------------------------------------------ */

/* EVERY NAME THIS LIBRARY ANSWERS AND ONE IT DOES NOT. The seven were owed in four different ways until
 * recently — three built from values this library chose and records as a deviation, one from profile
 * bytes embedded in the library — so what is held here is that they REACH A PROFILE AT ALL, that the
 * component count follows the model, and that the name a space was made with comes back out of it.
 *
 * AND THE REFUSAL IS A CASE, NOT AN ABSENCE: a name this library cannot answer must return NULL and say
 * so, never quietly become a default. Drawing in the wrong colour space is a wrong answer, and a silent
 * one. */
static int case_named_spaces(void)
{
	int before = g_fail;

	/* AUTOMATIC, NOT static: the constants are `extern const CFStringRef` VARIABLES, whose values are not
	 * constant expressions, so a static aggregate cannot be initialised from them -- which is what the
	 * first version of this case got wrong. */
	struct { const char *name; CFStringRef constant; size_t components; } table[] = {
		{ "kCGColorSpaceSRGB",                kCGColorSpaceSRGB, 3 },
		{ "kCGColorSpaceAdobeRGB1998",        kCGColorSpaceAdobeRGB1998, 3 },
		{ "kCGColorSpaceGenericGrayGamma2_2", kCGColorSpaceGenericGrayGamma2_2, 1 },
		{ "kCGColorSpaceGenericGray",         kCGColorSpaceGenericGray, 1 },
		{ "kCGColorSpaceGenericRGB",          kCGColorSpaceGenericRGB, 3 },
		{ "kCGColorSpaceGenericRGBLinear",    kCGColorSpaceGenericRGBLinear, 3 },
		{ "kCGColorSpaceGenericCMYK",         kCGColorSpaceGenericCMYK, 4 },
	};
	size_t i;

	for (i = 0; i < sizeof table / sizeof table[0]; i++) {
		CGColorSpaceRef space = CGColorSpaceCreateWithName(table[i].constant);
		CFStringRef back;

		check(space != NULL, "%s was refused; it answers a profile", table[i].name);
		if (space == NULL) {
			continue;
		}
		check(CGColorSpaceGetNumberOfComponents(space) == table[i].components,
		      "%s answers %zu components, want %zu", table[i].name,
		      CGColorSpaceGetNumberOfComponents(space), table[i].components);
		back = CGColorSpaceCopyName(space);
		check(back != NULL && CFEqual(back, table[i].constant),
		      "%s does not round-trip through CGColorSpaceCopyName", table[i].name);
	}

	check(CGColorSpaceCreateWithName(CFSTR("kCGColorSpaceNoSuchName")) == NULL,
	      "a name this library does not answer was ACCEPTED rather than refused");

	/* THE DEVICE SPACES ARE SINGLETONS AND THE NAMED ONES ARE ONE OBJECT PER NAME, which is a contract
	 * CGColorSpace.h states out loud; a caller may hold one, compare it with the next call's answer, and
	 * release it. */
	check(CGColorSpaceCreateDeviceRGB() == CGColorSpaceCreateDeviceRGB(),
	      "the device RGB space is not one object");
	check(CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB) ==
	      CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB),
	      "a named space is not one object per name");
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the type-id table                                                        */
/* ------------------------------------------------------------------------ */

/* CGTypeID.c MAKES TWO PROMISES AND THIS IS BOTH OF THEM: the same class always answers the same id, and
 * two classes never share one. THE IDS THEMSELVES ARE NOT ASSERTED — measured they are 2..13 for the
 * types this library mints, with CGColor at 256 because it is a registered CORE FOUNDATION type rather
 * than one minted here — because those numbers are the table's business and reshuffling it must not fail
 * this suite. Uniqueness and stability must. */
static int case_type_ids(void)
{
	int before = g_fail;

	struct { const char *name; CGTypeID id; } t[13];
	int i, j, unique = 1, stable = 1, nonzero = 1;

	t[0].name = "CGColor";         t[0].id = CGColorGetTypeID();
	t[1].name = "CGColorSpace";    t[1].id = CGColorSpaceGetTypeID();
	t[2].name = "CGContext";       t[2].id = CGContextGetTypeID();
	t[3].name = "CGDataConsumer";  t[3].id = CGDataConsumerGetTypeID();
	t[4].name = "CGDataProvider";  t[4].id = CGDataProviderGetTypeID();
	t[5].name = "CGFont";          t[5].id = CGFontGetTypeID();
	t[6].name = "CGFunction";      t[6].id = CGFunctionGetTypeID();
	t[7].name = "CGGradient";      t[7].id = CGGradientGetTypeID();
	t[8].name = "CGImage";         t[8].id = CGImageGetTypeID();
	t[9].name = "CGLayer";         t[9].id = CGLayerGetTypeID();
	t[10].name = "CGPath";         t[10].id = CGPathGetTypeID();
	t[11].name = "CGPattern";      t[11].id = CGPatternGetTypeID();
	t[12].name = "CGShading";      t[12].id = CGShadingGetTypeID();

	for (i = 0; i < 13; i++) {
		if (t[i].id == 0) {
			nonzero = 0;
			fprintf(stderr, "FAIL: %s answers type id 0\n", t[i].name);
		}
		for (j = i + 1; j < 13; j++) {
			if (t[i].id == t[j].id) {
				unique = 0;
				fprintf(stderr, "FAIL: %s and %s share type id %lu\n",
					t[i].name, t[j].name, (unsigned long)t[i].id);
			}
		}
	}
	check(nonzero, "a type id of 0 identifies nothing");
	check(unique, "two classes share a type id; the table's second promise is broken");

	stable = (CGColorGetTypeID() == t[0].id) && (CGFontGetTypeID() == t[5].id) &&
		 (CGLayerGetTypeID() == t[9].id) && (CGColorSpaceGetTypeID() == t[1].id);
	check(stable, "a class answered a different type id on a second call");
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the data provider and consumer                                           */
/* ------------------------------------------------------------------------ */

/* A PROVIDER MADE FROM A CFDataRef HANDS THE SAME BYTES BACK. That is Apple's `CreateWithCFData` and its
 * `CopyData` pair, and the note in CGDataProvider.h claimed for a while that the accessor was absent — it
 * is not, and this case is what would have caught the note, since a header cannot. */
static int case_data_provider(void)
{
	int before = g_fail;

	static const char *text = "the bytes a provider must give back\n";
	CFDataRef source = CFDataCreate(NULL, (const UInt8 *)text, (CFIndex)strlen(text));
	CGDataProviderRef provider = CGDataProviderCreateWithCFData(source);
	CFDataRef back;

	check(provider != NULL, "CGDataProviderCreateWithCFData refused a CFDataRef");
	if (provider == NULL) {
		return g_fail - before;
	}
	back = CGDataProviderCopyData(provider);
	check(back != NULL, "CGDataProviderCopyData answered nothing");
	check(back != NULL && CFEqual(source, back), "the provider gave back different bytes");
	check(back != NULL && CFDataGetLength(back) == (CFIndex)strlen(text),
	      "the provider gave back %ld bytes, want %zu", back ? (long)CFDataGetLength(back) : -1L,
	      strlen(text));
	return g_fail - before;
}

/* A CONSUMER OVER A CFMutableDataRef TAKES BYTES AND THE SINK ENDS UP HOLDING THEM. Both refusals the
 * door makes are cases as well: a gradient needs at least two stops to interpolate between, and a stop
 * that is not a colour would draw something, and something wrong. */
static int case_consumer_and_gradient(void)
{
	int before = g_fail;

	CFMutableDataRef sink = CFDataCreateMutable(NULL, 0);
	CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(sink);
	const char *payload = "written through the consumer";
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGColorRef first = CGColorCreateGenericRGB(1, 0, 0, 1);
	CGColorRef second = CGColorCreateGenericRGB(0, 0, 1, 1);
	const void *values[2];
	CFArrayRef ramp;
	CGGradientRef gradient;

	check(consumer != NULL, "CGDataConsumerCreateWithCFData refused a CFMutableDataRef");
	if (consumer != NULL) {
		check(CGDataConsumerPutBytes(consumer, payload, strlen(payload)) == strlen(payload),
		      "the consumer took a different number of bytes than it was given");
		check(CFDataGetLength(sink) == (CFIndex)strlen(payload) &&
		      memcmp(CFDataGetBytePtr(sink), payload, strlen(payload)) == 0,
		      "the bytes did not arrive in the sink");
	}

	check(rgb != NULL && first != NULL && second != NULL, "a colour or the device RGB space was refused");
	if (rgb == NULL || first == NULL || second == NULL) {
		return g_fail - before;
	}
	values[0] = first;
	values[1] = second;
	ramp = CFArrayCreate(NULL, values, 2, &kCFTypeArrayCallBacks);
	gradient = CGGradientCreateWithColors(rgb, ramp, NULL);
	check(gradient != NULL,
	      "a gradient was refused an array of CGColorRef -- the elements ARE the colours");

	/* ONE STOP IS REFUSED: there is nothing to interpolate between. */
	{
		const void *lonely[1];
		CFArrayRef one;

		lonely[0] = first;
		one = CFArrayCreate(NULL, lonely, 1, &kCFTypeArrayCallBacks);
		check(CGGradientCreateWithColors(rgb, one, NULL) == NULL, "a one-stop gradient was ACCEPTED");
	}

	/* AND A NULL ELEMENT IS REFUSED RATHER THAN CAST, which is the difference between a wrong gradient
	 * and no gradient. */
	{
		const void *hole[2];
		CFArrayRef broken;

		/* NO CALLBACKS HERE, DELIBERATELY, and this was measured the hard way: kCFTypeArrayCallBacks RETAINS
		 * every value, and Core Foundation HALTS -- SIGILL -- on a NULL one. An array that can hold a NULL
		 * element is made with NULL callbacks, and THAT is the array this door has to refuse. */
		hole[0] = first;
		hole[1] = NULL;
		broken = CFArrayCreate(NULL, hole, 2, NULL);
		check(CGGradientCreateWithColors(rgb, broken, NULL) == NULL,
		      "a gradient with a NULL stop was ACCEPTED");
	}
	return g_fail - before;
}

/* ------------------------------------------------------------------------ */
/* the runner                                                               */
/* ------------------------------------------------------------------------ */

static int run_in_child(const char *name, int (*fn)(void))
{
	pid_t pid;
	int status;

	fflush(NULL);
	pid = fork();
	if (pid == 0) {
		int rc = fn();

		_exit((rc == 0 && g_fail == 0) ? 0 : 1);
	}
	if (pid < 0) {
		fprintf(stderr, "FAIL: %s: fork failed\n", name);
		return 1;
	}
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "FAIL: %s: waitpid failed\n", name);
		return 1;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		printf("ok   %s\n", name);
		return 0;
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "FAIL: %s: killed by signal %d\n", name, WTERMSIG(status));
	} else {
		fprintf(stderr, "FAIL: %s: checks failed\n", name);
	}
	return 1;
}

static const struct { const char *name; int (*fn)(void); } g_cases[] = {
	{ "The colour spaces this library answers, by name", case_named_spaces },
	{ "The type-id table's two promises", case_type_ids },
	{ "A provider made from a CFDataRef gives the bytes back", case_data_provider },
	{ "A consumer takes bytes and a gradient takes a CFArray of CGColorRef",
	  case_consumer_and_gradient },
};

int main(void)
{
	size_t i;
	int passed = 0, failed = 0;
	size_t total = sizeof g_cases / sizeof g_cases[0];

	setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 0; i < total; i++) {
		if (run_in_child(g_cases[i].name, g_cases[i].fn) == 0) {
			passed++;
		} else {
			failed++;
		}
	}
	printf("drawing: %d of %d cases passed, %d failed\n", passed, passed + failed, failed);
	return failed == 0 ? 0 : 1;
}
