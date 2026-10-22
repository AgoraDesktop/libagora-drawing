/*
 * CGDataConsumer — the other direction: bytes going somewhere, and who takes them.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * A PROVIDER IS A SOURCE AND A CONSUMER IS A SINK, and the sink's whole interface is ONE CALLBACK: `putBytes`
 * takes `count` bytes from `buffer` and returns HOW MANY IT TOOK — fewer than it was given, or zero, means
 * the sink is full, which is how a caller writing a stream learns to stop. `releaseConsumer` is the pair to
 * the `info` pointer, exactly as a provider's `releaseInfo` is.
 *
 * THE TWO CONVENIENCE FORMS ARE SPELLED WITH APPLE'S TYPES: `CGDataConsumerCreateWithCFData` takes a
 * `CFMutableDataRef` and `CGDataConsumerCreateWithURL` a `CFURLRef`. THEY USED TO TAKE THE `NSMutableData *`
 * AND `NSURL *` those two are toll-free with — the Fiwix tree had no CoreFoundation — and this comment
 * called that "spelled with FOUNDATION types, as this library's other CF-typed doors are". They are
 * CF-typed doors now. BOTH ARE BUILT ON THE CALLBACK FORM — the
 * object is the `info` and the appending (or writing) is the callback — which is why this header's one
 * mechanism is the general one and the two convenience forms add nothing to the contract.
 *
 * `CGDataConsumerPutBytes` IS THIS LIBRARY'S OWN DOOR, AND BOTH ITS USUAL SOURCES SAY IT IS NOT APPLE'S: the
 * name is NOT in the 10.6 header (measured, and this comment said the opposite for one build) and the ledger
 * does not carry it either. IT SHIPS ANYWAY, AS A DOCUMENTED DEVIATION, because a sink with no way to take
 * bytes cannot be used at all: the write side of every consumer in this header goes through this door.
 */
#ifndef CORE_GRAPHICS_CGDATACONSUMER_H
#define CORE_GRAPHICS_CGDATACONSUMER_H

#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGBase.h>

#include <stddef.h>

typedef struct CGDataConsumer *CGDataConsumerRef;

typedef size_t (*CGDataConsumerPutBytesCallback)(void *info, const void *buffer, size_t count);
typedef void (*CGDataConsumerReleaseInfoCallback)(void *info);

/* APPLE'S STRUCT, TRANSCRIBED FIELD FOR FIELD — AND IT HAS NO `version`, unlike its provider siblings, which
 * is the sort of thing reading the header settles and remembering gets wrong. */
typedef struct CGDataConsumerCallbacks {
	CGDataConsumerPutBytesCallback putBytes;
	CGDataConsumerReleaseInfoCallback releaseConsumer;
} CGDataConsumerCallbacks;

/* A CALLBACK CONSUMER WITH NO `putBytes` IS REFUSED: a sink that cannot take bytes cannot answer the only
 * question it exists for, and a later answer of zero would look like a full sink. */
CGDataConsumerRef CGDataConsumerCreate(void *info, const CGDataConsumerCallbacks *callbacks);

/* APPENDS TO THE MUTABLE DATA, which is retained for the consumer's life. */
CGDataConsumerRef CGDataConsumerCreateWithCFData(CFMutableDataRef data);

/* WRITES TO THE FILE A FILE URL NAMES, truncating it, and REFUSES every other scheme by name: this library
 * writes local files. The mutable-data form is the one to use when the bytes should not touch the disk. */
CGDataConsumerRef CGDataConsumerCreateWithURL(CFURLRef url);

/* HOW MANY BYTES THE CONSUMER TOOK — FEWER THAN OFFERED, OR ZERO, MEANS IT IS FULL, and a caller writing a
 * stream stops there rather than assuming its data landed. */
size_t CGDataConsumerPutBytes(CGDataConsumerRef consumer, const void *buffer, size_t count);

/* THE TYPE IDENTITY, as every class in this library publishes it: the same value for every consumer and
 * different from every other class's. See CGBase.h for what the identity means. */
CGTypeID CGDataConsumerGetTypeID(void);

CGDataConsumerRef CGDataConsumerRetain(CGDataConsumerRef consumer);
void CGDataConsumerRelease(CGDataConsumerRef consumer);

#endif /* CORE_GRAPHICS_CGDATACONSUMER_H */
