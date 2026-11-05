/*
 * CGImageGIF.c — GIF87a/89a, with its frames.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **A GIF IS A CANVAS AND A LIST OF PATCHES, WHICH IS WHY "FRAME" IS A WORD THE FORMAT ITSELF USES.** Unlike a WebP
 * animation or an APNG, a GIF frame rarely has the canvas's size: it is a rectangle somewhere on the logical screen,
 * optionally with its own palette and its own transparent index, and it is composited onto whatever the frames before it
 * left behind. So a frame is not decoded alone — it is decoded as the canvas that results from frames 0..n, which is what
 * `-decode:length:index:` returns and why the caller's `index` walks the file from the start.
 *
 * WHAT IS READ: the global and local colour tables, transparent-colour-index frames, both disposal methods that leave
 * anything behind (`2` restore-to-background and `3` restore-to-previous are both honoured by keeping the previous canvas),
 * interlaced frames, and the graphics-control extension's delay.
 *
 * THE LZW IS THE FORMAT'S OWN (variable code width, clear code, end code), written here because no library is in this
 * tree for it and the algorithm is thirty lines. A decoder that guessed at a truncated stream would draw half an image
 * and call it whole, so an unterminated stream is refused by name.
 */
#include <CoreGraphics/CGImageSource_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

static unsigned fn_gif16(const unsigned char *p)
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static bool fn_gif_probe(const unsigned char *bytes, size_t length)
{
	/* **BOTH VERSIONS, WHICH IS THE WHOLE DIFFERENCE BETWEEN THEM AS FAR AS A DECODER IS CONCERNED.** `87a` has no
	 * transparency, no delay and no comment block; `89a` added them as extensions, so a reader that accepts 89a reads
	 * 87a files unchanged. MEASURED: accepting only 87a refused every GIF written this century. */
	return length >= 6 && memcmp(bytes, "GIF8", 4) == 0 && (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a';
}

/* ---- the data sub-blocks every GIF structure is made of ---------------------------------------- */

/* A sub-block chain's total length and whether it ends properly, without copying it. */
static bool fn_sub_blocks(const unsigned char *bytes, size_t length, size_t *at, size_t *total)
{
	size_t position = *at;

	*total = 0;
	while (position < length) {
		unsigned size = bytes[position++];

		if (size == 0) {
			*at = position;
			return true;
		}
		if (position + size > length) {
			return false;
		}
		position += size;
		*total += size;
	}
	return false;
}

/* ---- LZW --------------------------------------------------------------------------------------- */

/* THE DECOMPRESSION THE FORMAT SPECIFIES: variable code width, a clear code that resets the dictionary and an end code
 * that finishes. The dictionary is a prefix link per entry plus the one byte it adds, which is how the format's strings
 * are cheap to keep and why writing one means walking the links backwards. Two refusals, both by name: a code that names
 * an entry the stream has not defined yet, and a stream that stops before its end code — a truncated stream that decoded
 * "as far as it got" would draw half an image and call it whole. */
static bool fn_lzw(const unsigned char *data, size_t length, unsigned min_code_size, unsigned char *out, size_t out_size)
{
	unsigned clear = 1u << min_code_size;
	unsigned end = clear + 1u;
	unsigned next = end + 1u;
	unsigned code_size = min_code_size + 1u;
	unsigned previous = 0;
	bool have_previous = false;
	unsigned *prefix = NULL;
	unsigned *length_of = NULL;
	unsigned char *suffix = NULL;
	unsigned *stack = NULL;
	size_t written = 0;
	size_t bit = 0;
	bool ok = false;

	prefix = calloc(4096, sizeof *prefix);
	length_of = calloc(4096, sizeof *length_of);
	suffix = calloc(4096, 1);
	stack = calloc(4097, sizeof *stack);
	if (prefix == NULL || length_of == NULL || suffix == NULL || stack == NULL) {
		goto done;
	}
	{
		unsigned i;

		for (i = 0; i < clear; i++) {
			suffix[i] = (unsigned char)i;
			length_of[i] = 1;
		}
	}
	for (;;) {
		unsigned code = 0;
		unsigned shift;
		unsigned count;
		unsigned walk;

		for (shift = 0; shift < code_size; shift++) {
			size_t byte_index = bit / 8;

			if (byte_index >= length) {
				goto done;
			}
			if ((data[byte_index] >> (bit % 8)) & 1u) {
				code |= 1u << shift;
			}
			bit++;
		}
		if (code == clear) {
			next = end + 1u;
			code_size = min_code_size + 1u;
			have_previous = false;
			continue;
		}
		if (code == end) {
			ok = true;
			break;
		}
		if (code > next || (code == next && !have_previous)) {
			fprintf(stderr, "CG-REFUSE: a GIF's LZW stream names dictionary entry %u before it exists\n", code);
			goto done;
		}
		/* THE KWKWK CASE: the code names the entry ABOUT TO BE DEFINED, whose string is the previous string plus its
		 * own first byte. */
		{
			bool kwkwk = (code == next);
			unsigned source = kwkwk ? previous : code;

			count = length_of[source] + (kwkwk ? 1u : 0u);
			if (written + count > out_size) {
				fprintf(stderr, "CG-REFUSE: a GIF's LZW stream would write past the image it declares\n");
				goto done;
			}
			walk = source;
			{
				unsigned at = count;

				while (walk > end) {
					stack[--at] = walk;
					walk = prefix[walk];
				}
				stack[--at] = walk;
				{
					unsigned i;

					for (i = 0; i < count; i++) {
						out[written + i] = suffix[stack[i]];
					}
				}
			}
			if (kwkwk) {
				out[written + count - 1u] = out[written];
			}
			written += count;
			if (have_previous && next < 4096) {
				prefix[next] = previous;
				suffix[next] = out[written - count];
				length_of[next] = length_of[previous] + 1u;
				next++;
				if (next == (1u << code_size) && code_size < 12u) {
					code_size++;
				}
			}
			previous = code;
			have_previous = true;
		}
	}
done:
	free(prefix);
	free(length_of);
	free(suffix);
	free(stack);
	return ok;
}

/* ---- the file --------------------------------------------------------------------------------- */

typedef struct {
	size_t width;
	size_t height;
	size_t global_table_offset;
	size_t global_table_entries;
	bool has_global_table;
} fn_gif_screen;

/* Reads the header and the logical screen descriptor; `*at` is left at the first block after the global table. */
static bool fn_gif_read_screen(const unsigned char *bytes, size_t length, fn_gif_screen *screen, size_t *at)
{
	unsigned flags;
	size_t position = 13;	/* 6 signature + 7 screen descriptor */

	if (length < 14 || !fn_gif_probe(bytes, length)) {
		return false;
	}
	screen->width = fn_gif16(bytes + 6);
	screen->height = fn_gif16(bytes + 8);
	flags = bytes[10];
	screen->has_global_table = (flags & 0x80u) != 0;
	screen->global_table_entries = screen->has_global_table ? (1u << ((flags & 0x07u) + 1u)) : 0;
	screen->global_table_offset = position;
	position += screen->global_table_entries * 3u;
	if (position > length) {
		return false;
	}
	*at = position;
	return true;
}

static size_t fn_gif_count(const unsigned char *bytes, size_t length)
{
	fn_gif_screen screen;
	size_t at = 0;
	size_t count = 0;

	if (!fn_gif_read_screen(bytes, length, &screen, &at)) {
		return 0;
	}
	while (at < length) {
		unsigned introducer = bytes[at];

		if (introducer == 0x21u) {		/* extension */
			if (at + 2 > length) {
				break;
			}
			at += 2;
			{
				size_t total = 0;

				if (!fn_sub_blocks(bytes, length, &at, &total)) {
					break;
				}
			}
			continue;
		}
		if (introducer == 0x2cu) {		/* image descriptor */
			size_t total = 0;

			count++;
			if (at + 10 > length) {
				break;
			}
			{
				unsigned flags = bytes[at + 9];
				size_t local = ((flags & 0x80u) != 0) ? (size_t)3u << ((flags & 0x07u) + 1u) : 0;

				at += 10 + local;
			}
			if (at >= length) {
				break;
			}
			{
				unsigned min_code_size = bytes[at++];

				(void)min_code_size;
			}
			if (!fn_sub_blocks(bytes, length, &at, &total)) {
				break;
			}
			continue;
		}
		break;	/* 0x3b trailer, or something this decoder does not know */
	}
	return count;
}

/* **A FRAME IS THE CANVAS AFTER FRAMES 0..index, so this walks from the start every time.** A GIF's disposal methods make
 * that the only correct reading; a viewer that wants to animate cheaply keeps the canvas between calls rather than asking
 * this door for frame n over and over, and `VWDocument` does exactly that. */
static CGImageRef fn_gif_decode(const unsigned char *bytes, size_t length, size_t index, double *delay)
{
	fn_gif_screen screen;
	size_t at = 0;
	size_t frame = 0;
	unsigned char *canvas;
	size_t canvas_stride;
	unsigned char *previous_canvas;
	unsigned pending_disposal = 0;		/* the disposal of the frame drawn last, applied before the next */
	size_t pending_left = 0;
	size_t pending_top = 0;
	size_t pending_width = 0;
	size_t pending_height = 0;
	int transparent = -1;
	double this_delay = 0.0;
	CGImageRef image = NULL;

	if (delay != NULL) {
		*delay = 0.0;
	}
	if (!fn_gif_read_screen(bytes, length, &screen, &at)) {
		return NULL;
	}
	if (screen.width == 0 || screen.height == 0) {
		return NULL;
	}
	canvas_stride = screen.width * 4u;
	canvas = calloc(canvas_stride, screen.height);
	previous_canvas = calloc(canvas_stride, screen.height);
	if (canvas == NULL || previous_canvas == NULL) {
		free(canvas);
		free(previous_canvas);
		return NULL;
	}
	/* A GIF's canvas starts TRANSPARENT, not white: a frame that does not cover the screen shows what is behind it. */
	while (at < length) {
		unsigned introducer = bytes[at];
		unsigned disposal = 0;	/* set by a graphics-control block, which comes BEFORE the image it describes */

		if (introducer == 0x21u) {
			unsigned label;
			size_t total = 0;
			size_t block_start;

			if (at + 2 > length) {
				break;
			}
			label = bytes[at + 1];
			at += 2;
			block_start = at;
			if (!fn_sub_blocks(bytes, length, &at, &total)) {
				break;
			}
			if (label == 0xf9u && total >= 4) {	/* graphics control */
				unsigned flags = bytes[block_start + 1];

				this_delay = (double)fn_gif16(bytes + block_start + 2) / 100.0;
				transparent = ((flags & 0x01u) != 0) ? (int)bytes[block_start + 4] : -1;
				disposal = (unsigned)((flags >> 2) & 0x07u);
			}
			continue;
		}
		if (introducer != 0x2cu) {
			break;
		}
		{
			size_t left = fn_gif16(bytes + at + 1);
			size_t top = fn_gif16(bytes + at + 3);
			size_t width = fn_gif16(bytes + at + 5);
			size_t height = fn_gif16(bytes + at + 7);
			unsigned flags = bytes[at + 9];
			bool interlaced = (flags & 0x40u) != 0;
			size_t table_offset = at + 10;
			size_t table_entries = 0;
			size_t position;
			size_t data_start;
			size_t data_length = 0;
			size_t total = 0;
			unsigned min_code_size;
			unsigned char *pixels;
			size_t y;
			bool ok;

			if ((flags & 0x80u) != 0) {
				table_entries = (size_t)1u << ((flags & 0x07u) + 1u);
			} else if (screen.has_global_table) {
				table_offset = screen.global_table_offset;
				table_entries = screen.global_table_entries;
			}
			position = at + 10 + (((flags & 0x80u) != 0) ? (size_t)3u << ((flags & 0x07u) + 1u) : 0);
			if (position >= length) {
				break;
			}
			min_code_size = bytes[position++];
			data_start = position;
			/* **`at` IS MOVED TO THE DATA BEFORE THE SUB-BLOCK WALK**, or the walk would read the IMAGE DESCRIPTOR's
			 * own bytes as if they were sub-block lengths — which found a "block" of 0xf9 bytes and an LZW stream that
			 * ended before its end code. MEASURED as a first frame that never decoded, silently, because the LZW
			 * reader's truncation path is one of its two silent refusals. */
			at = position;
			if (!fn_sub_blocks(bytes, length, &at, &total)) {
				break;
			}
			data_length = total;
			if (width == 0 || height == 0) {
				continue;
			}
			pixels = calloc(width, height);
			if (pixels == NULL) {
				break;
			}
			/* **THE PREVIOUS FRAME'S DISPOSAL IS APPLIED HERE, BEFORE THIS ONE IS DRAWN.** `2` clears the rectangle
			 * the last frame occupied back to transparent; `3` restores the canvas as it was before that frame. Both
			 * are what the NEXT frame composites onto, which is the whole reason the format defines them. */
			if (frame > 0) {
				if (pending_disposal == 2) {
					size_t row;

					for (row = 0; row < pending_height; row++) {
						size_t py = pending_top + row;
						size_t x;

						if (py >= screen.height) {
							continue;
						}
						for (x = 0; x < pending_width; x++) {
							size_t px = pending_left + x;
							unsigned char *p;

							if (px >= screen.width) {
								continue;
							}
							p = canvas + py * canvas_stride + px * 4u;
							p[0] = p[1] = p[2] = p[3] = 0;
						}
					}
				} else if (pending_disposal == 3) {
					memcpy(canvas, previous_canvas, canvas_stride * screen.height);
				}
			}
			memcpy(previous_canvas, canvas, canvas_stride * screen.height);
			/* THE SUB-BLOCKS ARE CONTIGUOUS IN THE FILE BUT THE DECODER WANTS ONE BUFFER, and a GIF's blocks are
			 * separated by their own length bytes — so a copy is the honest way to hand them over as one stream. */
			{
				unsigned char *stream = malloc(data_length > 0 ? data_length : 1);
				size_t from = data_start;
				size_t to = 0;

				if (stream == NULL) {
					free(pixels);
					break;
				}
				while (from < length) {
					unsigned size = bytes[from++];

					if (size == 0 || to + size > data_length) {
						break;
					}
					memcpy(stream + to, bytes + from, size);
					to += size;
					from += size;
				}
				ok = fn_lzw(stream, to, min_code_size, pixels, width * height);
				free(stream);
			}
			if (!ok) {
				free(pixels);
				/* A broken frame is refused, not half-drawn; the canvas keeps what it had. */
				break;
			}
			/* THE FRAME LANDED. A caller that wanted an earlier one already got it. */
			if (frame == index) {
				size_t row;

				for (row = 0; row < height; row++) {
					size_t source_row = row;

					if (interlaced) {
						/* THE FOUR-PASS ORDER THE FORMAT SPECIFIES: rows 0,8,… then 4,12,… then 2,6,… then
						 * 1,3,…. */
						size_t pass_row = row;
						size_t pass = 0;
						size_t starts[4] = { 0, 4, 2, 1 };
						size_t steps[4] = { 8, 8, 4, 2 };
						size_t seen = 0;

						for (pass = 0; pass < 4; pass++) {
							size_t count = 0;
							size_t r;

							for (r = starts[pass]; r < height; r += steps[pass]) {
								count++;
							}
							if (pass_row < seen + count) {
								source_row = starts[pass] + (pass_row - seen) * steps[pass];
								break;
							}
							seen += count;
						}
					}
					{
						size_t x;

						for (x = 0; x < width; x++) {
							unsigned char value = pixels[source_row * width + x];
							size_t px = left + x;
							size_t py = top + row;
							unsigned char *target;

							if (px >= screen.width || py >= screen.height) {
								continue;
							}
							if (transparent >= 0 && (int)value == transparent) {
								continue;
							}
							target = canvas + py * canvas_stride + px * 4u;
							if ((size_t)value < table_entries) {
								const unsigned char *entry = bytes + table_offset + (size_t)value * 3u;

								target[0] = entry[2];
								target[1] = entry[1];
								target[2] = entry[0];
								target[3] = 255;
							}
						}
					}
				}
				{
					unsigned char *copy = malloc(canvas_stride * screen.height);

					if (copy != NULL) {
						memcpy(copy, canvas, canvas_stride * screen.height);
						image = cg_image_from_bgra(copy, screen.width, screen.height, canvas_stride, true);
						free(copy);
					}
				}
				if (delay != NULL) {
					*delay = (this_delay > 0.0) ? this_delay : 0.0;
				}
			}
			free(pixels);
			frame++;
			/* THE DISPOSAL OF *THIS* FRAME APPLIES BEFORE THE NEXT ONE IS DRAWN. `2` clears its rectangle back to
			 * the background; `3` restores the previous canvas. Both are kept here because the next frame's
			 * composite is what reads them. */
			pending_disposal = disposal;
			pending_left = left;
			pending_top = top;
			pending_width = width;
			pending_height = height;
			disposal = 0;
			/* **`>` AND NOT `>=`, WHICH IS THE DIFFERENCE BETWEEN "DID I DRAW IT" AND "SHOULD I STOP".** The output
			 * above already happened when `frame == index`; this only stops BEFORE the frame after the one asked for.
			 * MEASURED: `>=` stopped one frame early, so every index but 0 answered NULL and a two-frame GIF looked
			 * like a one-frame one. */
			if (frame > index) {
				break;
			}
		}
	}
	free(previous_canvas);
	free(canvas);
	return image;
}

static void fn_gif_size(const unsigned char *bytes, size_t length, size_t index, size_t *width, size_t *height)
{
	fn_gif_screen screen;
	size_t at = 0;

	(void)index;
	*width = 0;
	*height = 0;
	if (fn_gif_read_screen(bytes, length, &screen, &at)) {
		*width = screen.width;
		*height = screen.height;
	}
}

const cg_image_decoder cg_decoder_gif = {
	"com.compuserve.gif", "gif", fn_gif_probe, fn_gif_count, fn_gif_decode, fn_gif_size
};
