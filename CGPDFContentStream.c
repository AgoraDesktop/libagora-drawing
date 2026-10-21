/*
 * CGPDFContentStream.c — collecting the parts of a page's content, and resolving the names they use.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE TWO THINGS THIS FILE DOES ARE BOTH PLUMBING AND BOTH LOAD-BEARING. First, `/Contents` is normalised: a
 * single stream and an array of streams answer the same list, and a non-stream in that array is skipped rather
 * than aborting the page (a damaged page still has the content it kept). Second, the resource chain is walked:
 * a stream's own `/Resources` first, then the parent's, which is how a form XObject sees the page's fonts while
 * overriding one image.
 *
 * THE DECODED BYTES ARE CACHED LAZILY, ONE STREAM AT A TIME. A page whose content is an array is decoded as the
 * scanner reaches each part, so a scanner that stops early (a callback that fails) has not paid to inflate the
 * rest of the page.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFContentStream.h>
#include <CoreGraphics/CGPDFScanner.h>

#include <stdlib.h>
#include <string.h>

struct CGPDFContentStream {
	int refcount;
	struct CGPDFContentStream *parent;	/* retained */
	cg_pdf_doc *doc;
	cg_pdf_obj **streams;			/* borrowed, arena-owned */
	size_t count;
	CFDataRef *decoded;			/* lazy; owned */
	cg_pdf_obj *resources;			/* borrowed */
};

static CGPDFContentStreamRef fn_content_new(cg_pdf_doc *doc)
{
	CGPDFContentStreamRef content = calloc(1, sizeof *content);

	if (content != NULL) {
		content->refcount = 1;
		content->doc = doc;
	}
	return content;
}

static bool fn_content_push(CGPDFContentStreamRef content, cg_pdf_obj *stream)
{
	cg_pdf_obj **grown;
	CFDataRef *decoded;

	grown = realloc(content->streams, (content->count + 1) * sizeof *grown);
	if (grown == NULL) {
		return false;
	}
	content->streams = grown;
	decoded = realloc(content->decoded, (content->count + 1) * sizeof *decoded);
	if (decoded == NULL) {
		return false;
	}
	content->decoded = decoded;
	content->streams[content->count] = stream;
	content->decoded[content->count] = NULL;
	content->count++;
	return true;
}

CGPDFContentStreamRef CGPDFContentStreamCreateWithPage(CGPDFPageRef page)
{
	cg_pdf_doc *doc;
	const cg_pdf_page_info *info;
	CGPDFContentStreamRef content;
	cg_pdf_obj *contents;

	if (page == NULL) {
		fprintf(stderr, "CG-REFUSE: CGPDFContentStreamCreateWithPage needs a page\n");
		return NULL;
	}
	doc = cg_pdf_page_doc(page);
	if (doc == NULL || cg_pdf_page_index(page) >= doc->page_count) {
		return NULL;
	}
	info = &doc->pages[cg_pdf_page_index(page)];
	content = fn_content_new(doc);
	if (content == NULL) {
		return NULL;
	}
	content->resources = info->resources;
	contents = cg_pdf_dict_get(doc, info->dict, "Contents");
	if (contents == NULL) {
		return content;
	}
	contents = cg_pdf_resolve(doc, contents);
	if (contents != NULL && contents->type == kCGPDFObjectTypeStream) {
		fn_content_push(content, contents);
	} else if (contents != NULL && contents->type == kCGPDFObjectTypeArray) {
		size_t i;

		for (i = 0; i < contents->u.array.count; i++) {
			cg_pdf_obj *part = cg_pdf_resolve(doc, contents->u.array.items[i]);

			if (part != NULL && part->type == kCGPDFObjectTypeStream) {
				fn_content_push(content, part);
			}
		}
	}
	return content;
}

CGPDFContentStreamRef CGPDFContentStreamCreateWithStream(CGPDFStreamRef stream, CGPDFDictionaryRef streamResources,
							 CGPDFContentStreamRef parent)
{
	cg_pdf_obj *s = (cg_pdf_obj *)stream;
	CGPDFContentStreamRef content;

	if (s == NULL || s->type != kCGPDFObjectTypeStream) {
		fprintf(stderr, "CG-REFUSE: CGPDFContentStreamCreateWithStream needs a stream\n");
		return NULL;
	}
	content = fn_content_new(s->doc);
	if (content == NULL) {
		return NULL;
	}
	content->resources = (cg_pdf_obj *)streamResources;
	content->parent = CGPDFContentStreamRetain(parent);
	if (!fn_content_push(content, s)) {
		CGPDFContentStreamRelease(content);
		return NULL;
	}
	return content;
}

CGPDFContentStreamRef CGPDFContentStreamRetain(CGPDFContentStreamRef contentStream)
{
	if (contentStream != NULL) {
		contentStream->refcount++;
	}
	return contentStream;
}

void CGPDFContentStreamRelease(CGPDFContentStreamRef contentStream)
{
	size_t i;

	if (contentStream == NULL) {
		return;
	}
	if (--contentStream->refcount > 0) {
		return;
	}
	for (i = 0; i < contentStream->count; i++) {
		if (contentStream->decoded[i] != NULL) {
			CFRelease(contentStream->decoded[i]);
		}
	}
	free(contentStream->decoded);
	free(contentStream->streams);
	if (contentStream->parent != NULL) {
		CGPDFContentStreamRelease(contentStream->parent);
	}
	free(contentStream);
}

CFArrayRef CGPDFContentStreamGetStreams(CGPDFContentStreamRef contentStream)
{
	CFArrayRef array;
	size_t i;

	if (contentStream == NULL || contentStream->count == 0) {
		return CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);
	}
	/* THE STREAMS ARE NOT CORE FOUNDATION TYPES, so the array does not retain them: it is a VIEW of the
	 * document's objects and lives only as long as they do, which is the same contract as the doors that
	 * return them. */
	array = CFArrayCreateMutable(kCFAllocatorDefault, (CFIndex)contentStream->count, NULL);
	if (array == NULL) {
		return NULL;
	}
	for (i = 0; i < contentStream->count; i++) {
		CFArrayAppendValue((CFMutableArrayRef)array, (CGPDFStreamRef)contentStream->streams[i]);
	}
	return array;
}

CGPDFObjectRef CGPDFContentStreamGetResource(CGPDFContentStreamRef contentStream, const char *category,
					     const char *name)
{
	CGPDFContentStreamRef at;

	if (category == NULL || name == NULL) {
		return NULL;
	}
	for (at = contentStream; at != NULL; at = at->parent) {
		cg_pdf_obj *categories = cg_pdf_resolve(at->doc, at->resources);
		cg_pdf_obj *dict;
		cg_pdf_obj *value;

		if (categories == NULL || categories->type != kCGPDFObjectTypeDictionary) {
			continue;
		}
		dict = cg_pdf_dict_get(at->doc, categories, category);
		if (dict == NULL || dict->type != kCGPDFObjectTypeDictionary) {
			continue;
		}
		value = cg_pdf_dict_get(at->doc, dict, name);
		if (value != NULL) {
			return (CGPDFObjectRef)value;
		}
	}
	return NULL;
}

size_t cg_pdf_content_stream_count(struct CGPDFContentStream *content)
{
	return content != NULL ? content->count : 0;
}

CFDataRef cg_pdf_content_stream_data(struct CGPDFContentStream *content, size_t index)
{
	if (content == NULL || index >= content->count) {
		return NULL;
	}
	if (content->decoded[index] == NULL) {
		content->decoded[index] = cg_pdf_stream_data(content->doc, content->streams[index]);
	}
	if (content->decoded[index] == NULL) {
		/* A FILTER THIS BUILD CANNOT DECODE IS NOT AN EMPTY STREAM: the failure has already been reported by
		 * name, and answering an empty data here would let a scanner "succeed" on nothing. */
		fprintf(stderr, "CG-REFUSE: a content stream could not be decoded\n");
		return NULL;
	}
	return content->decoded[index];
}

cg_pdf_doc *cg_pdf_content_stream_doc(struct CGPDFContentStream *content)
{
	return content != NULL ? content->doc : NULL;
}
