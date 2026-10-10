/*
 * CGDataConsumerFoundation — the two convenience sinks, where the object is the `info` pointer.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * BOTH ARE THE CALLBACK FORM WITH A CORE FOUNDATION OBJECT BEHIND IT, which is the design: the mutable
 * data one appends, the file one writes, and neither adds anything to the contract `CGDataConsumer.h`
 * states. What they DO add is the lifetime rule — the object, or the file descriptor, belongs to the
 * consumer and is let go by `releaseConsumer` — and, for the file, the refusal of every scheme that is
 * not a local file. In the Fiwix tree these were Objective-C over NSMutableData/NSURL; Agora's Core
 * Graphics depends on Core Foundation, so the file is C over CFMutableDataRef/CFURLRef (PROVENANCE.md).
 *
 * THE PARTIAL WRITE IS THE BUG THIS FILE'S LOOP EXISTS TO PREVENT: `write(2)` returning less than it was
 * given is ordinary, and a sink that answered the full count after a short write would tell its caller
 * that bytes landed which never did. The callback answers WHAT IT WROTE and the caller decides.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <CoreGraphics/CGDataConsumer.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- the mutable-data sink ---------------------------------------------------------------- */

static size_t fn_mutable_put(void *info, const void *buffer, size_t count)
{
	CFDataAppendBytes((CFMutableDataRef)info, (const UInt8 *)buffer, (CFIndex)count);
	return count;
}

static void fn_mutable_release(void *info)
{
	CFRelease((CFMutableDataRef)info);
}

CGDataConsumerRef CGDataConsumerCreateWithCFData(CFMutableDataRef data)
{
	static const CGDataConsumerCallbacks callbacks = { fn_mutable_put, fn_mutable_release };
	CGDataConsumerRef consumer;

	if (data == NULL) {
		fprintf(stderr, "CG-REFUSE: CGDataConsumerCreateWithCFData needs a mutable data to append "
				"to\n");
		return NULL;
	}
	consumer = CGDataConsumerCreate((void *)CFRetain(data), &callbacks);
	if (consumer == NULL) {
		CFRelease(data);
	}
	return consumer;
}

/* --- the file sink ------------------------------------------------------------------------ */

struct fn_file_sink {
	int fd;
};

static size_t fn_file_put(void *info, const void *buffer, size_t count)
{
	struct fn_file_sink *sink = info;
	size_t done = 0;

	while (done < count) {
		ssize_t n = write(sink->fd, (const char *)buffer + done, count - done);

		if (n <= 0) {
			break;	/* a full disk or an error: the count says how much LANDED */
		}
		done += (size_t)n;
	}
	return done;
}

static void fn_file_release(void *info)
{
	struct fn_file_sink *sink = info;

	if (sink->fd >= 0) {
		close(sink->fd);
	}
	free(sink);
}

CGDataConsumerRef CGDataConsumerCreateWithURL(CFURLRef url)
{
	static const CGDataConsumerCallbacks callbacks = { fn_file_put, fn_file_release };
	struct fn_file_sink *sink;
	CGDataConsumerRef consumer;
	char path[PATH_MAX];

	if (url == NULL ||
	    !CFURLGetFileSystemRepresentation(url, true, (UInt8 *)path, (CFIndex)sizeof(path))) {
		fprintf(stderr, "CG-REFUSE: CGDataConsumerCreateWithURL writes FILE URLs only; this library "
				"writes local files, and the mutable-data form is what to use otherwise\n");
		return NULL;
	}
	sink = malloc(sizeof *sink);
	if (sink == NULL) {
		return NULL;
	}
	sink->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (sink->fd < 0) {
		fprintf(stderr, "CG-REFUSE: CGDataConsumerCreateWithURL cannot write %s: %s\n",
			path, strerror(errno));
		free(sink);
		return NULL;
	}
	consumer = CGDataConsumerCreate(sink, &callbacks);
	if (consumer == NULL) {
		close(sink->fd);
		free(sink);
	}
	return consumer;
}
