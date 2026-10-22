/*
 * CGFontAliases.c — the macOS font names, and the bundled face that answers for each.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **NO APPLE FONT IS HERE, AND NO APPLE NAME IS CLAIMED AS OURS.** Agora ships an open, redistributable set
 * (see `fonts/PROVENANCE.md`); macOS applications, nib files and design documents, however, ask for the names
 * the Mac has carried for decades — `Helvetica`, `Lucida Grande`, `Menlo`, `New York`, `SF Pro`. This table is
 * the translation between the two, and it is deliberately a table of REQUESTS rather than a set of renamed
 * font files: the bundled faces keep their own names and licences, and a request for a Mac name is answered
 * by a face whose design plays that role.
 *
 * **IT IS A FALLBACK, NOT A REPLACEMENT.** `cg_font_create_with_name` searches the registry for the name it
 * was given first, and only calls this door when that search comes back empty. A machine with a real
 * `Helvetica` therefore still draws with it; a fresh Agora install, which ships no Apple font, gets
 * Liberation Sans instead of nothing. The same rule makes the table harmless on a host whose fonts the user
 * prefers — put that host's directory in `FN_FONT_PATH` and the exact name wins before this file is asked.
 *
 * **THE SUBSTITUTE MAY BE A FAMILY OR A FULL NAME.** A family is what most requests want (`Helvetica` ->
 * `Liberation Sans`, which resolves to its Regular face), and a full name is what preserves a style when the
 * request itself carried one (`Helvetica-Bold` -> `Liberation Sans Bold`). The resolver below does not care
 * which it returns; `cg_font_create_with_name` searches for it exactly as it would search for a caller's name.
 *
 * **THE CHOICES, SLOT BY SLOT, ARE RECORDED IN `fonts/PROVENANCE.md`, beside the faces themselves.** The
 * short version: Figtree is the SF Pro/system-UI role (Inter is the bundled fallback), Source Serif 4 is New York,
 * IBM Plex Mono is SF Mono and Menlo,
 * Liberation Sans/Serif/Mono are the Helvetica/Arial, Times and Courier roles, Gelasio is Georgia, Jost is
 * Futura, Mulish is Avenir, and the display/script faces are the small OFL families that stand in for the
 * Mac's display and handwriting catalogue.
 */
#include <CoreGraphics/CGFont_internal.h>

#include <stddef.h>
#include <strings.h>

struct fn_font_alias {
	const char *name;	/* what a macOS caller asks for */
	const char *substitute;	/* what the bundled set answers with: a family, or a full name */
};

static const struct fn_font_alias fn_font_aliases[] = {
	/* ---- the system faces, which are the default UI family --------------------------------- */
	{ "SF Pro",			"Figtree" },
	{ "SF Pro Text",		"Figtree" },
	{ "SF Pro Display",		"Figtree" },
	{ "SF UI Text",			"Figtree" },
	{ "SF UI Display",		"Figtree" },
	{ ".SF NS",			"Figtree" },
	{ ".AppleSystemUIFont",		"Figtree" },
	{ "System Font",		"Figtree" },
	{ "Lucida Grande",		"Figtree" },
	{ "LucidaGrande",		"Figtree" },
	{ "Geneva",			"Figtree" },
	{ "Skia",			"Figtree" },
	{ "Seravek",			"Figtree" },

	/* ---- Helvetica and Arial, which are one role here ------------------------------------- */
	{ "Helvetica",			"Liberation Sans" },
	{ "Helvetica-Bold",		"Liberation Sans Bold" },
	{ "Helvetica-Oblique",		"Liberation Sans Italic" },
	{ "Helvetica-BoldOblique",	"Liberation Sans Bold Italic" },
	{ "Helvetica Neue",		"Liberation Sans" },
	{ "HelveticaNeue",		"Liberation Sans" },
	{ "HelveticaNeue-Bold",		"Liberation Sans Bold" },
	{ "Arial",			"Liberation Sans" },
	{ "ArialMT",			"Liberation Sans" },
	{ "Arial-BoldMT",		"Liberation Sans Bold" },
	{ "Arial-ItalicMT",		"Liberation Sans Italic" },
	{ "Arial-BoldItalicMT",		"Liberation Sans Bold Italic" },
	{ "Verdana",			"Liberation Sans" },
	{ "Verdana-Bold",		"Liberation Sans Bold" },
	{ "Tahoma",			"Liberation Sans" },
	{ "Tahoma-Bold",		"Liberation Sans Bold" },

	/* ---- the system serif and the system mono -------------------------------------------- */
	{ "New York",			"Source Serif 4" },
	{ "NewYork",			"Source Serif 4" },
	{ "SF Serif",			"Source Serif 4" },
	{ "SF Mono",			"IBM Plex Mono" },
	{ "SFMono-Regular",		"IBM Plex Mono" },
	{ "SFMono-Bold",		"IBM Plex Mono Bold" },
	{ "Menlo",			"IBM Plex Mono" },
	{ "Menlo-Regular",		"IBM Plex Mono" },
	{ "Menlo-Bold",			"IBM Plex Mono Bold" },
	{ "Menlo-Italic",		"IBM Plex Mono Italic" },
	{ "Monaco",			"IBM Plex Mono" },
	{ "Andale Mono",		"IBM Plex Mono" },

	/* ---- the document faces -------------------------------------------------------------- */
	{ "Times",			"Liberation Serif" },
	{ "Times-Roman",		"Liberation Serif" },
	{ "Times-Bold",			"Liberation Serif Bold" },
	{ "Times-Italic",		"Liberation Serif Italic" },
	{ "Times-BoldItalic",		"Liberation Serif Bold Italic" },
	{ "Times New Roman",		"Liberation Serif" },
	{ "TimesNewRomanPSMT",		"Liberation Serif" },
	{ "TimesNewRomanPS-BoldMT",	"Liberation Serif Bold" },
	{ "TimesNewRomanPS-ItalicMT",	"Liberation Serif Italic" },
	{ "Courier",			"Liberation Mono" },
	{ "Courier-Bold",		"Liberation Mono Bold" },
	{ "Courier-Oblique",		"Liberation Mono Italic" },
	{ "Courier New",		"Liberation Mono" },
	{ "CourierNewPSMT",		"Liberation Mono" },
	{ "Georgia",			"Gelasio" },
	{ "Georgia-Bold",		"Gelasio Bold" },
	{ "Georgia-Italic",		"Gelasio Italic" },
	{ "Charter",			"Source Serif 4" },
	{ "Iowan Old Style",		"Source Serif 4" },
	{ "Hoefler Text",		"EB Garamond" },
	{ "Baskerville",		"Libre Baskerville" },
	{ "Big Caslon",			"Libre Baskerville" },
	{ "Palatino",			"Source Serif 4" },
	{ "Palatino Linotype",		"Source Serif 4" },
	{ "Book Antiqua",		"Source Serif 4" },
	{ "Didot",			"Source Serif 4" },
	{ "Bodoni 72",			"Source Serif 4" },

	/* ---- the geometric and humanist sans ------------------------------------------------- */
	{ "Futura",			"Jost" },
	{ "Futura-Medium",		"Jost" },
	{ "Avenir",			"Mulish" },
	{ "Avenir Next",		"Mulish" },
	{ "AvenirNext-Regular",		"Mulish" },
	{ "Gill Sans",			"Fira Sans" },
	{ "Trebuchet MS",		"Fira Sans" },
	{ "Optima",			"Fira Sans" },

	/* ---- the rounded face ---------------------------------------------------------------- */
	{ "SF Pro Rounded",		"Nunito" },
	{ "Arial Rounded MT",		"Nunito" },
	{ "Arial Rounded MT Bold",	"Nunito Bold" },

	/* ---- the display and handwriting faces ----------------------------------------------- */
	{ "Copperplate",		"Cinzel" },
	{ "Herculanum",			"Cinzel" },
	{ "Impact",			"Anton" },
	{ "Rockwell",			"Zilla Slab" },
	{ "Superclarendon",		"Zilla Slab" },
	{ "Clarendon",			"Zilla Slab" },
	{ "American Typewriter",	"Special Elite" },
	{ "Comic Sans",			"Comic Neue" },
	{ "Comic Sans MS",		"Comic Neue" },
	{ "Chalkboard",			"Comic Neue" },
	{ "Chalkboard SE",		"Comic Neue" },
	{ "Marker Felt",		"Patrick Hand" },
	{ "Bradley Hand",		"Patrick Hand" },
	{ "Brush Script MT",		"Pacifico" },
	{ "SignPainter",		"Pacifico" },
	{ "Zapfino",			"Pinyon Script" },
	{ "Snell Roundhand",		"Pinyon Script" },
	{ "Apple Chancery",		"Pinyon Script" },
	{ "Savoye LET",			"Pinyon Script" },
	{ "Party LET",			"Pinyon Script" },
	{ "Trattatello",		"Pinyon Script" },

	/* ---- symbols and emoji --------------------------------------------------------------- */
	{ "Apple Symbols",		"Noto Sans Symbols 2" },
	{ "Symbol",			"Noto Sans Symbols 2" },
	{ "Zapf Dingbats",		"Noto Sans Symbols 2" },
	{ "LastResort",			"Noto Sans Symbols 2" },
	{ "STIXGeneral",		"Noto Sans Math" },
	{ "Apple Color Emoji",		"Noto Emoji" },
	{ "Apple Emoji",		"Noto Emoji" },
	{ "Emoji",			"Noto Emoji" },

	/* ---- the CSS generic families, which a web view or a design tool names ---------------- */
	{ "sans-serif",			"Figtree" },
	{ "system-ui",			"Figtree" },
	{ "serif",			"Source Serif 4" },
	{ "monospace",			"IBM Plex Mono" },
};

const char *cg_font_substitute_family(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0') {
		return NULL;
	}
	for (i = 0; i < sizeof fn_font_aliases / sizeof fn_font_aliases[0]; i++) {
		if (strcasecmp(name, fn_font_aliases[i].name) == 0) {
			return fn_font_aliases[i].substitute;
		}
	}
	return NULL;
}
