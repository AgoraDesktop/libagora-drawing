/*
 * CGImageDestination.c — writing an image out, which in this build means writing a PNG.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **THE OBJECT IS SMALL BECAUSE THE ENCODER IS ELSEWHERE.** `cg_image_png_data` (in `CGImagePNG.c`, beside the
 * decoder) turns a `CGImage` into bytes; this file is the destination Apple's documentation describes — somewhere to
 * put them, a count, a type, and a `Finalize` that is the moment of writing. Keeping the split there means the library
 * has ONE place that knows what a PNG is, and this file has no pixel arithmetic in it at all.
 *
 * **AND THE TYPE IS CHECKED AT CREATION RATHER THAN AT FINALIZE**, so a caller learns that `public.jpeg` is not
 * something this build writes when it asks for it, not after it has added images. The refusal names the supported
 * type, which is also what `CGImageDestinationCopyTypeIdentifiers` answers.
 */
#include <CoreGraphics/CGImageDestination.h>
#include <CoreGraphics/CGImage_internal.h>

#include <CoreFoundation/CFRuntime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* THE ONE UTI THIS BUILD WRITES. Apple names it `kUTTypePNG`; the string is the UTI itself, which is what the API
 * takes, and this tree has no MobileCoreServices header to borrow the constant from. */
#define kCGImageDestinationPNG "public.png"

struct __CGImageDestination {
	CFRuntimeBase base;
	CFMutableDataRef data;		/* when created over memory */
	CFURLRef url;			/* when created over a file */
	CFStringRef type;		/* the UTI, copied */
	size_t count;			/* the caller's hint */
	CGImageRef image;		/* the one image added, retained, or NULL */
	bool finalized;
};

static void fn_destination_finalize(CFTypeRef object)
{
	CGImageDestinationRef idst = (CGImageDestinationRef)object;

	if (idst->data != NULL) {
		CFRelease(idst->data);
		idst->data = NULL;
	}
	if (idst->url != NULL) {
		CFRelease(idst->url);
		idst->url = NULL;
	}
	if (idst->type != NULL) {
		CFRelease(idst->type);
		idst->type = NULL;
	}
	if (idst->image != NULL) {
		CGImageRelease(idst->image);
		idst->image = NULL;
	}
}

CGTypeID CGImageDestinationGetTypeID(void)
{
	static CFTypeID type_id = 0;

	if (type_id == 0) {
		static const CFRuntimeClass klass = {
			0,
			"CGImageDestination",
			NULL,			/* init */
			NULL,			/* copy */
			fn_destination_finalize,
			NULL,			/* equal */
			NULL,			/* hash */
			NULL,			/* copyFormattingDesc */
			NULL			/* copyDebugDesc */
		};

		type_id = _CFRuntimeRegisterClass(&klass);
	}
	return type_id;
}

/* WHETHER THIS BUILD WRITES THE TYPE, which is the question both creation doors and the refusal ask. */
static bool fn_writes_type(CFStringRef type)
{
	return type != NULL && CFEqual(type, CFSTR(kCGImageDestinationPNG));
}

static CGImageDestinationRef fn_destination_new(void)
{
	CGImageDestinationRef idst = (CGImageDestinationRef)_CFRuntimeCreateInstance(
		kCFAllocatorDefault, CGImageDestinationGetTypeID(),
		(CFIndex)(sizeof(struct __CGImageDestination) - sizeof(CFRuntimeBase)), NULL);

	return idst;
}

/* THE ONE PLACE THAT SAYS WHAT CANNOT BE WRITTEN, because two creation doors and the refusal all ask it. */
static void fn_refuse_type(CFStringRef type)
{
	char name[128] = "(none)";

	if (type != NULL) {
		CFStringGetCString(type, name, (CFIndex)sizeof name, kCFStringEncodingUTF8);
	}
	fprintf(stderr, "CG-REFUSE: CGImageDestination does not write `%s`; this build writes %s only (see "
			"CGImageDestinationCopyTypeIdentifiers)\n", name, kCGImageDestinationPNG);
}

CGImageDestinationRef CGImageDestinationCreateWithData(CFMutableDataRef data, CFStringRef type, size_t count,
						       CFDictionaryRef options)
{
	CGImageDestinationRef idst;

	if (data == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationCreateWithData needs a mutable data\n");
		return NULL;
	}
	if (options != NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationCreateWithData does not read its options yet, and "
				"ignoring them would write an image the caller did not ask for\n");
		return NULL;
	}
	if (!fn_writes_type(type)) {
		fn_refuse_type(type);
		return NULL;
	}
	idst = fn_destination_new();
	if (idst == NULL) {
		return NULL;
	}
	idst->data = (CFMutableDataRef)CFRetain(data);
	idst->type = (CFStringRef)CFRetain(type);
	idst->count = count;
	return idst;
}

CGImageDestinationRef CGImageDestinationCreateWithURL(CFURLRef url, CFStringRef type, size_t count,
						      CFDictionaryRef options)
{
	CGImageDestinationRef idst;

	if (url == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationCreateWithURL needs a file URL\n");
		return NULL;
	}
	if (options != NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationCreateWithURL does not read its options yet, and "
				"ignoring them would write an image the caller did not ask for\n");
		return NULL;
	}
	if (!fn_writes_type(type)) {
		fn_refuse_type(type);
		return NULL;
	}
	idst = fn_destination_new();
	if (idst == NULL) {
		return NULL;
	}
	idst->url = (CFURLRef)CFRetain(url);
	idst->type = (CFStringRef)CFRetain(type);
	idst->count = count;
	return idst;
}

void CGImageDestinationAddImage(CGImageDestinationRef idst, CGImageRef image, CFDictionaryRef properties)
{
	if (idst == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationAddImage needs a destination\n");
		return;
	}
	if (properties != NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationAddImage does not read image properties yet, and "
				"ignoring them would write an image the caller did not ask for\n");
		return;
	}
	if (image == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationAddImage needs an image\n");
		return;
	}
	if (idst->image != NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationAddImage was given a second image; a %s "
				"destination here carries ONE, and dropping the second would silently write the "
				"wrong picture\n", kCGImageDestinationPNG);
		return;
	}
	idst->image = (CGImageRef)CGImageRetain(image);
}

bool CGImageDestinationFinalize(CGImageDestinationRef idst)
{
	CFDataRef bytes;
	bool wrote = false;

	if (idst == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationFinalize needs a destination\n");
		return false;
	}
	if (idst->finalized) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationFinalize was already called; finalizing twice "
				"would append the image to its own file\n");
		return false;
	}
	if (idst->image == NULL) {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationFinalize has no image: nothing was added\n");
		return false;
	}
	bytes = cg_image_png_data(idst->image);
	if (bytes == NULL) {
		return false;
	}
	if (idst->data != NULL) {
		CFDataAppendBytes(idst->data, CFDataGetBytePtr(bytes), CFDataGetLength(bytes));
		wrote = true;
	} else if (idst->url != NULL) {
		char path[1024];

		if (CFURLGetFileSystemRepresentation(idst->url, true, (UInt8 *)path, (CFIndex)sizeof path)) {
			FILE *file = fopen(path, "wb");

			if (file != NULL) {
				wrote = (fwrite(CFDataGetBytePtr(bytes), (size_t)CFDataGetLength(bytes), 1, file) == 1);
				fclose(file);
			}
		}
		if (!wrote) {
			fprintf(stderr, "CG-REFUSE: CGImageDestinationFinalize could not write the file this "
					"destination names\n");
		}
	} else {
		fprintf(stderr, "CG-REFUSE: CGImageDestinationFinalize has nowhere to write: the destination "
				"was made over neither data nor a URL\n");
	}
	CFRelease(bytes);
	if (wrote) {
		idst->finalized = true;
	}
	return wrote;
}

CFArrayRef CGImageDestinationCopyTypeIdentifiers(void)
{
	const void *one = CFSTR(kCGImageDestinationPNG);

	return CFArrayCreate(kCFAllocatorDefault, &one, 1, &kCFTypeArrayCallBacks);
}
