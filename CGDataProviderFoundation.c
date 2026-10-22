/*
 * CGDataProviderFoundation — the provider's CORE FOUNDATION-object forms.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * APPLE'S NAME SAYS `CFData` AND SO DOES THE ARGUMENT — Agora's Core Graphics depends on Core Foundation,
 * so the `…WithCFData` form takes the CFDataRef Apple's header names (PROVENANCE.md). In the Fiwix tree
 * this was an NSData and the file was Objective-C; it is C now.
 *
 * AND THERE IS NO SECOND IMPLEMENTATION HERE. A provider made from a CFData IS a provider made from a
 * caller's bytes — the same constructor, the same release callback, the same ownership rule — with the
 * CFData as the thing the callback lets go of. `CGDataProviderCreateWithData` already adopts a buffer and
 * calls back when the provider dies; that is exactly what a CFData needs, so this file is a bridge and not
 * a second provider.
 *
 * NO ARC, matching the library's policy: the retains and releases below are explicit, and they are the whole
 * of the ownership story.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGDataProvider_internal.h>

#include <limits.h>
#include <stdio.h>

/* THE CALLBACK THAT MAKES THE PROVIDER'S LIFETIME HOLD THE CFDATA UP: `info` is the object the creator
 * retained, and `data` — the pointer taken from `CFDataGetBytePtr` — is valid for exactly as long as that
 * object lives. Letting the pointer outlast the object is the bug this function exists to prevent. */
static void cg_release_data_object(void *info, const void *data, size_t size)
{
	(void)data;
	(void)size;
	CFRelease((CFDataRef)info);
}

CGDataProviderRef CGDataProviderCreateWithCFData(CFDataRef data)
{
	CGDataProviderRef provider;

	if (data == NULL) {
		fprintf(stderr, "CG-REFUSE: CGDataProviderCreateWithCFData needs data\n");
		return NULL;
	}
	/* `CFDataGetBytePtr` IS BORROWED AND `CFDataGetLength` IS THE SIZE, so the retain above the call is what
	 * makes the borrow live as long as the provider. IF THE CONSTRUCTOR REFUSES, the retain has to be undone
	 * here — it is the only place that knows the object was taken. */
	provider = CGDataProviderCreateWithData((void *)CFRetain(data), CFDataGetBytePtr(data),
						(size_t)CFDataGetLength(data), cg_release_data_object);
	if (provider == NULL) {
		CFRelease(data);
	}
	return provider;
}

/* A PROVIDER OVER A URL'S BYTES: `file:` only, and the rest refused by name. The reading itself is the
 * filename form's — the same code that already reads a font or a profile — so this door is a URL-to-path
 * step and a refusal, and NOT a second file reader.
 *
 * WHY NOT MORE: `http:` and its family would put a network stack under a type whose whole promise is
 * "these bytes are here, and who lets go of them", and every byte this library reads is local. */
CGDataProviderRef CGDataProviderCreateWithURL(CFURLRef url)
{
	char path[PATH_MAX];

	if (url == NULL ||
	    !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, (CFIndex)sizeof(path))) {
		fprintf(stderr, "CG-REFUSE: CGDataProviderCreateWithURL reads FILE URLs only; this library "
			"has no network layer under a provider\n");
		return NULL;
	}
	return CGDataProviderCreateWithFilename(path);
}

/* THE BYTES AS A CORE FOUNDATION OBJECT — the read side of the same door, and A COPY: the caller owns the
 * result, so a provider may be released the moment its caller is done. Handing back a view of the provider's
 * memory would tie the two lifetimes together for no reason a caller asked for. */
CFDataRef CGDataProviderCopyData(CGDataProviderRef provider)
{
	const void *bytes;
	size_t size = 0;

	bytes = cg_dataprovider_bytes(provider, &size);
	if (bytes == NULL) {
		return NULL;
	}
	return CFDataCreate(NULL, bytes, (CFIndex)size);
}
