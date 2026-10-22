/*
 * CGPDFFunction.c — sampled, exponential and stitched functions, and the refusal of the programmable one.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * THE SAMPLED TABLE IS THE ONLY ONE WITH REAL ARITHMETIC, and it is worth stating what "sample" means: /Size
 * gives the count along each axis, /BitsPerSample the width of each sample, and the samples are packed
 * MSB-first and CONTIGUOUSLY — a 4-bit table's second value begins in the low nibble of the first byte. The
 * decode step is what turns an integer sample into a number in /Range: `Decode[d0 d1 ...]` maps sample 0 to d0
 * and the axis' maximum to d1.
 *
 * TYPE 2'S `N` IS A REAL EXPONENT, and 1 is the common case: `C0 + x (C1 - C0)` with x normalised to /Domain.
 * A NEGATIVE x with a fractional N is undefined by the specification; it is treated as zero here rather than
 * producing a NaN, because a shading with a NaN colour is a black page.
 *
 * TYPE 3'S /ENCODE IS THE PART READERS GET WRONG: each sub-function's input is not clipped to its own /Domain
 * but mapped from the parent's domain through /Encode, so a stitched ramp can put two subdomains in one. That
 * mapping is applied here before the sub-function sees the input.
 */
#include <CoreGraphics/CGPDFFunction.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CG_PDF_FUNCTION_MAX_DIMS 8

struct cg_pdf_function {
	int type;
	size_t inputs;
	size_t outputs;

	/* type 0 */
	double domain[CG_PDF_FUNCTION_MAX_DIMS * 2];
	double range[CG_PDF_FUNCTION_MAX_DIMS * 2];
	int size[CG_PDF_FUNCTION_MAX_DIMS];
	double encode[CG_PDF_FUNCTION_MAX_DIMS * 2];
	double decode[CG_PDF_FUNCTION_MAX_DIMS * 2];
	int bits;
	unsigned char *samples;
	size_t sample_bytes;

	/* type 2 */
	double c0[CG_PDF_FUNCTION_MAX_DIMS];
	double c1[CG_PDF_FUNCTION_MAX_DIMS];
	double exponent;

	/* type 3 */
	cg_pdf_function **sub;
	size_t sub_count;
	double bounds[CG_PDF_FUNCTION_MAX_DIMS];
	double stitch_encode[CG_PDF_FUNCTION_MAX_DIMS * 2];
};

static bool fn_numbers(cg_pdf_doc *doc, cg_pdf_obj *array, double *out, size_t count)
{
	size_t i;

	array = cg_pdf_resolve(doc, array);
	if (array == NULL || array->type != kCGPDFObjectTypeArray || array->u.array.count < count) {
		return false;
	}
	for (i = 0; i < count; i++) {
		if (!cg_pdf_number(doc, array->u.array.items[i], &out[i])) {
			return false;
		}
	}
	return true;
}

static bool fn_default(double *out, size_t count, double first, double second)
{
	size_t i;

	for (i = 0; i < count; i++) {
		out[i * 2] = first;
		out[i * 2 + 1] = second;
	}
	return true;
}

static void fn_free(cg_pdf_function *function)
{
	size_t i;

	if (function == NULL) {
		return;
	}
	for (i = 0; i < function->sub_count; i++) {
		cg_pdf_function_free(function->sub[i]);
	}
	free(function->sub);
	free(function->samples);
	free(function);
}

/* A SAMPLE, MSB-FIRST, FROM A BYTE BUFFER THAT MAY END MID-BYTE. */
static unsigned fn_sample_bits(const unsigned char *bytes, size_t bit_offset, int bits)
{
	unsigned value = 0;
	int i;

	for (i = 0; i < bits; i++) {
		size_t at = bit_offset + (size_t)i;

		value = (value << 1) | ((bytes[at >> 3] >> (7 - (at & 7))) & 1u);
	}
	return value;
}

cg_pdf_function *cg_pdf_function_create(cg_pdf_doc *doc, cg_pdf_obj *dict)
{
	cg_pdf_function *function;
	cg_pdf_obj *type_obj;
	cg_pdf_obj *stream = NULL;
	long long type = -1;

	dict = cg_pdf_resolve(doc, dict);
	if (dict != NULL && dict->type == kCGPDFObjectTypeStream) {
		/* A SAMPLED FUNCTION IS A STREAM; its keys live in the stream's dictionary. */
		stream = dict;
		dict = dict->u.stream.dict;
	}
	if (dict == NULL || dict->type != kCGPDFObjectTypeDictionary) {
		return NULL;
	}
	type_obj = cg_pdf_dict_get(doc, dict, "FunctionType");
	if (!cg_pdf_integer(doc, type_obj, &type)) {
		fprintf(stderr, "CG-REFUSE: a PDF function has no /FunctionType\n");
		return NULL;
	}
	function = calloc(1, sizeof *function);
	if (function == NULL) {
		return NULL;
	}
	function->type = (int)type;
	function->inputs = 1;
	function->outputs = 1;
	function->exponent = 1.0;

	if (type == 4) {
		fprintf(stderr, "CG-REFUSE: a type 4 PDF function is a PostScript program, and this reader has "
				"no evaluator for it; the shading it belongs to is not drawn\n");
		fn_free(function);
		return NULL;
	}

	{
		cg_pdf_obj *domain = cg_pdf_dict_get(doc, dict, "Domain");
		cg_pdf_obj *range = cg_pdf_dict_get(doc, dict, "Range");

		if (domain != NULL && domain->type == kCGPDFObjectTypeArray) {
			function->inputs = domain->u.array.count / 2;
			if (function->inputs > CG_PDF_FUNCTION_MAX_DIMS) {
				function->inputs = CG_PDF_FUNCTION_MAX_DIMS;
			}
			fn_numbers(doc, domain, function->domain, function->inputs * 2);
		} else {
			fn_default(function->domain, function->inputs, 0.0, 1.0);
		}
		if (range != NULL && range->type == kCGPDFObjectTypeArray) {
			function->outputs = range->u.array.count / 2;
			if (function->outputs > CG_PDF_FUNCTION_MAX_DIMS) {
				function->outputs = CG_PDF_FUNCTION_MAX_DIMS;
			}
			fn_numbers(doc, range, function->range, function->outputs * 2);
		} else {
			fn_default(function->range, function->outputs, 0.0, 1.0);
		}
	}

	if (type == 0) {
		cg_pdf_obj *size_obj = cg_pdf_dict_get(doc, dict, "Size");
		cg_pdf_obj *bits_obj = cg_pdf_dict_get(doc, dict, "BitsPerSample");
		cg_pdf_obj *encode = cg_pdf_dict_get(doc, dict, "Encode");
		cg_pdf_obj *decode = cg_pdf_dict_get(doc, dict, "Decode");
		CFDataRef data;
		long long bits = 8;
		size_t total = 1;
		size_t i;

		function->inputs = size_obj != NULL && size_obj->type == kCGPDFObjectTypeArray ?
				   size_obj->u.array.count : 1;
		if (function->inputs > CG_PDF_FUNCTION_MAX_DIMS) {
			function->inputs = CG_PDF_FUNCTION_MAX_DIMS;
		}
		if (function->inputs == 0 || size_obj == NULL) {
			fn_free(function);
			return NULL;
		}
		for (i = 0; i < function->inputs; i++) {
			double v;

			if (!cg_pdf_number(doc, size_obj->u.array.items[i], &v) || v < 1 || v > 65536) {
				fn_free(function);
				return NULL;
			}
			function->size[i] = (int)v;
			total *= (size_t)v;
		}
		if (cg_pdf_integer(doc, bits_obj, &bits) && (bits == 1 || bits == 2 || bits == 4 || bits == 8 ||
							    bits == 12 || bits == 16)) {
			function->bits = (int)bits;
		} else {
			function->bits = 8;
		}
		if (encode == NULL || !fn_numbers(doc, encode, function->encode, function->inputs * 2)) {
			for (i = 0; i < function->inputs; i++) {
				function->encode[i * 2] = 0.0;
				function->encode[i * 2 + 1] = (double)(function->size[i] - 1);
			}
		}
		if (decode == NULL || !fn_numbers(doc, decode, function->decode, function->outputs * 2)) {
			fn_default(function->decode, function->outputs, 0.0, 1.0);
		}
		if (stream == NULL) {
			fprintf(stderr, "CG-REFUSE: a type 0 PDF function is a stream, and this one is a plain "
					"dictionary\n");
			fn_free(function);
			return NULL;
		}
		data = cg_pdf_stream_data(doc, stream);
		if (data == NULL) {
			fn_free(function);
			return NULL;
		}
		/* **A SAMPLE IS `outputs` VALUES, NOT ONE.** The first version counted only the table's entries, so a
		 * three-output function copied the first 256 bytes of a 768-byte table — the red channel of every
		 * sample — and a red-to-blue shading came out red-to-black. MEASURED by the CGShading round trip. */
		function->sample_bytes = total * function->outputs * (size_t)function->bits;
		function->sample_bytes = (function->sample_bytes + 7) / 8;
		if ((size_t)CFDataGetLength(data) < function->sample_bytes) {
			fprintf(stderr, "CG-REFUSE: a sampled PDF function's stream holds %ld bytes and its /Size "
					"needs %zu\n", (long)CFDataGetLength(data), function->sample_bytes);
			CFRelease(data);
			fn_free(function);
			return NULL;
		}
		function->samples = malloc(function->sample_bytes);
		if (function->samples == NULL) {
			CFRelease(data);
			fn_free(function);
			return NULL;
		}
		memcpy(function->samples, CFDataGetBytePtr(data), function->sample_bytes);
		CFRelease(data);
		return function;
	}

	if (type == 2) {
		cg_pdf_obj *c0 = cg_pdf_dict_get(doc, dict, "C0");
		cg_pdf_obj *c1 = cg_pdf_dict_get(doc, dict, "C1");
		double n = 1.0;

		if (c0 != NULL) {
			function->outputs = c0->u.array.count;
			if (function->outputs > CG_PDF_FUNCTION_MAX_DIMS) {
				function->outputs = CG_PDF_FUNCTION_MAX_DIMS;
			}
			fn_numbers(doc, c0, function->c0, function->outputs);
		}
		if (c1 != NULL) {
			size_t count = c1->u.array.count;

			if (count > CG_PDF_FUNCTION_MAX_DIMS) {
				count = CG_PDF_FUNCTION_MAX_DIMS;
			}
			if (c0 == NULL) {
				function->outputs = count;
			}
			fn_numbers(doc, c1, function->c1, count);
		} else {
			size_t i;

			for (i = 0; i < function->outputs; i++) {
				function->c1[i] = 1.0;
			}
		}
		if (cg_pdf_number(doc, cg_pdf_dict_get(doc, dict, "N"), &n)) {
			function->exponent = n;
		}
		/* THE OUTPUT COUNT IS ONLY KNOWN NOW — it comes from C0 when /Range is absent — so the default
		 * /Range has to be filled in after it, or every component but the first clips to [0,0] and a blue
		 * ramp comes out red-then-black. */
		if (cg_pdf_dict_get(doc, dict, "Range") == NULL) {
			fn_default(function->range, function->outputs, 0.0, 1.0);
		}
		return function;
	}

	if (type == 3) {
		cg_pdf_obj *functions = cg_pdf_dict_get(doc, dict, "Functions");
		cg_pdf_obj *bounds = cg_pdf_dict_get(doc, dict, "Bounds");
		cg_pdf_obj *encode = cg_pdf_dict_get(doc, dict, "Encode");
		size_t i;

		if (functions == NULL || functions->type != kCGPDFObjectTypeArray || functions->u.array.count == 0) {
			fn_free(function);
			return NULL;
		}
		function->sub_count = functions->u.array.count;
		function->sub = calloc(function->sub_count, sizeof *function->sub);
		if (function->sub == NULL) {
			fn_free(function);
			return NULL;
		}
		for (i = 0; i < function->sub_count; i++) {
			function->sub[i] = cg_pdf_function_create(doc, functions->u.array.items[i]);
			if (function->sub[i] == NULL) {
				fn_free(function);
				return NULL;
			}
		}
		if (bounds != NULL) {
			fn_numbers(doc, bounds, function->bounds, function->sub_count > 0 ? function->sub_count - 1 : 0);
		}
		if (encode == NULL || !fn_numbers(doc, encode, function->stitch_encode, function->sub_count * 2)) {
			for (i = 0; i < function->sub_count; i++) {
				function->stitch_encode[i * 2] = function->domain[0];
				function->stitch_encode[i * 2 + 1] = function->domain[1];
			}
		}
		function->inputs = 1;
		/* **THE STITCHING FUNCTION'S OUTPUT COUNT IS ITS SUB-FUNCTIONS'.** Without this it kept the default
		 * of ONE output, so a red-to-blue ramp came out red-to-BLACK: only the red channel was copied and the
		 * green and blue stayed zero. MEASURED by the PDF writer's gradient round trip; poppler drew the same
		 * file correctly because the file was right. */
		function->outputs = function->sub[0]->outputs;
		if (cg_pdf_dict_get(doc, dict, "Range") == NULL) {
			fn_default(function->range, function->outputs, 0.0, 1.0);
		}
		return function;
	}

	fprintf(stderr, "CG-REFUSE: PDF function type %lld is not one this reader knows\n", type);
	fn_free(function);
	return NULL;
}

void cg_pdf_function_free(cg_pdf_function *function)
{
	fn_free(function);
}

size_t cg_pdf_function_input_count(cg_pdf_function *function)
{
	return function != NULL ? function->inputs : 0;
}

size_t cg_pdf_function_output_count(cg_pdf_function *function)
{
	return function != NULL ? function->outputs : 0;
}

static double fn_clip(double v, double lo, double hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static bool fn_eval(cg_pdf_function *function, const double *in, double *out)
{
	size_t i;
	size_t j;

	switch (function->type) {
	case 0: {
		double x[CG_PDF_FUNCTION_MAX_DIMS];
		int index[CG_PDF_FUNCTION_MAX_DIMS];
		int index2[CG_PDF_FUNCTION_MAX_DIMS];
		double frac[CG_PDF_FUNCTION_MAX_DIMS];
		for (i = 0; i < function->inputs; i++) {
			double d0 = function->domain[i * 2];
			double d1 = function->domain[i * 2 + 1];
			double e0 = function->encode[i * 2];
			double e1 = function->encode[i * 2 + 1];
			double value = fn_clip(in[i], d0 < d1 ? d0 : d1, d0 < d1 ? d1 : d0);
			double mapped;

			if (d1 == d0) {
				mapped = e0;
			} else {
				mapped = e0 + (value - d0) * (e1 - e0) / (d1 - d0);
			}
			if (mapped < 0) mapped = 0;
			if (mapped > (double)(function->size[i] - 1)) mapped = (double)(function->size[i] - 1);
			index[i] = (int)floor(mapped);
			frac[i] = mapped - (double)index[i];
			if (index[i] >= function->size[i] - 1) {
				index[i] = function->size[i] - 1;
				frac[i] = 0.0;
			}
			index2[i] = index[i] + 1 < function->size[i] ? index[i] + 1 : index[i];
			x[i] = mapped;
		}
		/* ONE DIMENSION: LINEAR. TWO: BILINEAR. MORE: NEAREST, stated in the header. */
		for (j = 0; j < function->outputs; j++) {
			double lo;
			double hi;
			if (function->inputs == 1) {
				size_t s0 = (size_t)index[0];
				size_t s1 = (size_t)index2[0];

				lo = (double)fn_sample_bits(function->samples,
							    (s0 * function->outputs + j) * function->bits,
							    function->bits);
				hi = (double)fn_sample_bits(function->samples,
							    (s1 * function->outputs + j) * function->bits,
							    function->bits);
				lo += (hi - lo) * frac[0];
			} else if (function->inputs == 2) {
				size_t a = (size_t)index[0] * (size_t)function->size[1] + (size_t)index[1];
				size_t b = (size_t)index2[0] * (size_t)function->size[1] + (size_t)index[1];
				size_t c = (size_t)index[0] * (size_t)function->size[1] + (size_t)index2[1];
				size_t d = (size_t)index2[0] * (size_t)function->size[1] + (size_t)index2[1];
				double v00 = (double)fn_sample_bits(function->samples,
								    (a * function->outputs + j) * function->bits,
								    function->bits);
				double v10 = (double)fn_sample_bits(function->samples,
								    (b * function->outputs + j) * function->bits,
								    function->bits);
				double v01 = (double)fn_sample_bits(function->samples,
								    (c * function->outputs + j) * function->bits,
								    function->bits);
				double v11 = (double)fn_sample_bits(function->samples,
								    (d * function->outputs + j) * function->bits,
								    function->bits);

				lo = v00 * (1 - frac[0]) * (1 - frac[1]) + v10 * frac[0] * (1 - frac[1]) +
				     v01 * (1 - frac[0]) * frac[1] + v11 * frac[0] * frac[1];
			} else {
				size_t flat = 0;

				for (i = 0; i < function->inputs; i++) {
					flat = flat * (size_t)function->size[i] + (size_t)index[i];
				}
				lo = (double)fn_sample_bits(function->samples, (flat * function->outputs + j) * function->bits,
							    function->bits);
			}
			{
				double max_sample = (double)((1u << function->bits) - 1);
				double encode0 = function->decode[j * 2];
				double encode1 = function->decode[j * 2 + 1];
				double normalised = max_sample > 0 ? lo / max_sample : 0.0;

				out[j] = encode0 + normalised * (encode1 - encode0);
			}
			{
				double r0 = function->range[j * 2];
				double r1 = function->range[j * 2 + 1];

				out[j] = fn_clip(out[j], r0 < r1 ? r0 : r1, r0 < r1 ? r1 : r0);
			}
		}
		(void)x;
		return true;
	}
	case 2: {
		double d0 = function->domain[0];
		double d1 = function->domain[1];
		double x = d1 == d0 ? 0.0 : (in[0] - d0) / (d1 - d0);

		if (x < 0) x = 0;
		if (x > 1) x = 1;
		if (x > 0) {
			x = pow(x, function->exponent);
		} else if (function->exponent < 0) {
			x = 0;
		}
		for (j = 0; j < function->outputs; j++) {
			out[j] = function->c0[j] + x * (function->c1[j] - function->c0[j]);
			{
				double r0 = function->range[j * 2];
				double r1 = function->range[j * 2 + 1];

				out[j] = fn_clip(out[j], r0 < r1 ? r0 : r1, r0 < r1 ? r1 : r0);
			}
		}
		return true;
	}
	case 3: {
		size_t which = 0;
		double mapped;

		while (which + 1 < function->sub_count && in[0] > function->bounds[which]) {
			which++;
		}
		{
			double lo = which == 0 ? function->domain[0] : function->bounds[which - 1];
			double hi = which + 1 >= function->sub_count ? function->domain[1] : function->bounds[which];
			double e0 = function->stitch_encode[which * 2];
			double e1 = function->stitch_encode[which * 2 + 1];

			mapped = hi == lo ? e0 : e0 + (in[0] - lo) * (e1 - e0) / (hi - lo);
		}
		{
			double sub_in[CG_PDF_FUNCTION_MAX_DIMS];
			double sub_out[CG_PDF_FUNCTION_MAX_DIMS];
			size_t k;

			for (k = 0; k < function->sub[which]->inputs && k < CG_PDF_FUNCTION_MAX_DIMS; k++) {
				sub_in[k] = mapped;
			}
			if (!fn_eval(function->sub[which], sub_in, sub_out)) {
				return false;
			}
			for (j = 0; j < function->outputs && j < function->sub[which]->outputs; j++) {
				out[j] = sub_out[j];
			}
			return true;
		}
	}
	}
	return false;
}

bool cg_pdf_function_eval(cg_pdf_function *function, const double *inputs, double *outputs)
{
	if (function == NULL || inputs == NULL || outputs == NULL) {
		return false;
	}
	return fn_eval(function, inputs, outputs);
}
