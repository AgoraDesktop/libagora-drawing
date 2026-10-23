/*
 * flatten_memory — what a fill COSTS the heap, measured rather than assumed.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WHAT THIS HOLDS UP: `cg_traps_for_path` flattens a path into two arrays it owns — the unclipped
 * vertices (`fl.pts`) and the clipped outline built from them (`fl.edges`) — and it must release
 * BOTH. It released only the edges, so every flattened fill and clip that allocated vertices
 * retained them for the life of the process: a background repaint or a clipping path in a loop grew
 * the heap without bound while the context itself was perfectly well released. This case is the
 * measurement that made the second `free` necessary and keeps it necessary.
 *
 * HOW IT MEASURES, and why not `mallinfo`. The obvious door — glibc's `mallinfo2`, sampled before and
 * after a loop — was tried and is NOT good enough here: the first rounds move by megabytes of ARENA
 * growth that no code leak caused, so a real 512-byte-per-fill loss hides under the noise and a
 * correct run still shows a delta. This test instead interposes the four allocation doors themselves
 * (`malloc`, `calloc`, `realloc`, `free`) in the test executable, which the dynamic loader prefers over
 * libc's for calls made from the shared library, and keeps an EXACT accounting of outstanding usable
 * bytes and allocation count. The bookkeeping is switched on only around the measured region, so the
 * loop below cannot be perturbed by anything this test does on its own behalf.
 *
 * WHAT THE EXPECTATION IS: zero. Not "small": a fill that allocates and releases its scratch returns
 * exactly the heap it borrowed, so 256 repetitions must retain nothing and count no owners left over.
 * `path` is the control — a path that is only BUILT and never flattened must also retain nothing —
 * and the two fill modes are the two places the flattener is read: a filled path and a clip.
 *
 * THIS IS NOT WHOLE-PROCESS LEAK CERTIFICATION. It says these doors, driven this way, retain nothing.
 *
 * glibc only: `__libc_malloc` and `malloc_usable_size` are glibc's, and the interposition depends on
 * the executable's symbols winning the global lookup. Anywhere else the case SKIPS with 77.
 */
/* `__GLIBC__` IS NOT A PREDEFINED MACRO — the C library's own <features.h> defines it, so a libc
 * header has to be read before the test can ask which libc this is. This one is included for that
 * reason and is otherwise unused on the measuring side. */
#include <stdio.h>

#if defined(__GLIBC__) && defined(__linux__)

#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGPath.h>
#include <CoreFoundation/CoreFoundation.h>

#include <malloc.h>
#include <stddef.h>

/* THE REAL DOORS, REACHED PAST OUR OWN INTERPOSITIONS. Using libc's internal entry points rather than
 * `dlsym(RTLD_NEXT, …)` keeps resolution out of the allocator: `dlsym` allocates on first call, which
 * would recurse into the very function being resolved. */
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

/* ONE SIGNAL, ONE COUNTER. `malloc_usable_size` gives the bytes actually owned for a pointer, so a
 * `free` subtracts exactly what its `malloc` added and a `realloc` adds only the difference. */
static int tracking;
static long long outstanding;
static long long allocations;

void *malloc(size_t n)
{
	void *p = __libc_malloc(n);

	if (p != NULL && tracking) {
		outstanding += (long long)malloc_usable_size(p);
		allocations++;
	}
	return p;
}

void *calloc(size_t count, size_t size)
{
	void *p = __libc_calloc(count, size);

	if (p != NULL && tracking) {
		outstanding += (long long)malloc_usable_size(p);
		allocations++;
	}
	return p;
}

void *realloc(void *old, size_t n)
{
	long long was = (old != NULL && tracking) ? (long long)malloc_usable_size(old) : 0;
	void *p = __libc_realloc(old, n);

	if (p != NULL && tracking) {
		outstanding += (long long)malloc_usable_size(p) - was;
		allocations++;
	}
	return p;
}

void free(void *p)
{
	if (p != NULL && tracking) {
		outstanding -= (long long)malloc_usable_size(p);
	}
	__libc_free(p);
}

static int failed;

static void check(int ok, const char *name)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", name);
	if (!ok) {
		failed++;
	}
}

/* THE MEASURED REGION: take the baseline, run `reps` of the door, report what stayed. The counters are
 * reset rather than carried across cases, because each case is a separate claim. */
static void measure(const char *name, void (*door)(CGContextRef), CGContextRef c, int reps)
{
	long long kept;
	long long counted;

	tracking = 1;
	outstanding = 0;
	allocations = 0;
	for (int i = 0; i < reps; i++) {
		door(c);
	}
	tracking = 0;
	kept = outstanding;
	counted = allocations;

	printf("  %-22s reps=%-4d retained=%-8lld allocated=%lld\n", name, reps, kept, counted);
	check(kept == 0, "the door retains no bytes");
}

/* THE DOORS. Each adds its own path, so the loop measures the FLATTENING rather than a path that was
 * built once outside the measurement. */

static void fill_rect_path(CGContextRef c)
{
	CGContextBeginPath(c);
	CGContextAddRect(c, CGRectMake(2, 2, 40, 40));
	CGContextFillPath(c);
}

static void fill_curve_path(CGContextRef c)
{
	CGContextBeginPath(c);
	CGContextAddEllipseInRect(c, CGRectMake(2, 2, 40, 40));
	CGContextFillPath(c);
}

static void clip_curve_path(CGContextRef c)
{
	CGContextSaveGState(c);
	CGContextBeginPath(c);
	CGContextAddEllipseInRect(c, CGRectMake(2, 2, 40, 40));
	CGContextClip(c);
	CGContextRestoreGState(c);
}

/* THE CONTROL: the path is BUILT and released, never flattened, so nothing here reaches the fill's
 * flattener and nothing may be retained — which is what tells the two fill cases apart from a bug in
 * the accounting itself. */
static void build_only(CGContextRef c)
{
	CGContextBeginPath(c);
	CGContextAddRect(c, CGRectMake(2, 2, 40, 40));
	CGContextBeginPath(c);
}

int main(void)
{
	unsigned char bits[64 * 64 * 4];
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef c = CGBitmapContextCreate(bits, 64, 64, 8, 64 * 4, space,
					      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);

	if (c == NULL) {
		printf("flatten_memory: cannot make a bitmap context\n");
		return 77;
	}

	/* WARM THE CACHES FIRST. The first fills grow pixman's and the font machinery's own one-time
	 * state, none of it the fill's and none of it retained per call; measuring from cold attributes that
	 * to the door. */
	for (int i = 0; i < 32; i++) {
		fill_curve_path(c);
		clip_curve_path(c);
	}

	measure("fill-rect", fill_rect_path, c, 256);
	measure("fill-curve", fill_curve_path, c, 256);
	measure("clip-curve", clip_curve_path, c, 256);
	measure("build-only", build_only, c, 256);

	CGContextRelease(c);
	CGColorSpaceRelease(space);

	printf("flatten_memory: %d case(s) failed\n", failed);
	return failed == 0 ? 0 : 1;
}

#else

int main(void)
{
	printf("flatten_memory: SKIP — this case needs glibc's allocator accounting\n");
	return 77;
}

#endif
