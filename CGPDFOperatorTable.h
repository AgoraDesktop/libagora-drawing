/*
 * CGPDFOperatorTable.h — content-stream operators by name, and the callback that runs when one is seen.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * **AN OPERATOR TABLE IS WHAT MAKES THE SCANNER GENERIC.** The scanner knows how to tokenise a content stream and
 * how to hold operands; what `Tj`, `re` or `Do` MEAN is a callback the caller installs. That split is why the same
 * scanner drives page drawing, content inspection and a form's recursion: the table differs, the tokeniser does
 * not.
 *
 * **AN OPERATOR WITH NO CALLBACK IS NOT AN ERROR.** Content streams contain operators a caller does not care
 * about (`q`, `Q` in a text extraction, say); the scanner consumes it and clears its operands. The table answers
 * "is there a caller for this", not "is this valid PDF".
 *
 * THE TABLE IS A SMALL LINEAR LIST. A content stream has a few dozen distinct operators and a table is built
 * once; a hash would be more machinery than the data has ever justified.
 */
#ifndef CORE_GRAPHICS_CGPDFOPERATORTABLE_H
#define CORE_GRAPHICS_CGPDFOPERATORTABLE_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGPDFScanner.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFOperatorTable *CGPDFOperatorTableRef;

typedef void (*CGPDFOperatorCallback)(CGPDFScannerRef scanner, void *info);

CG_EXTERN CGPDFOperatorTableRef CGPDFOperatorTableCreate(void) CF_RETURNS_RETAINED;

CG_EXTERN CGPDFOperatorTableRef CGPDFOperatorTableRetain(CGPDFOperatorTableRef table);
CG_EXTERN void CGPDFOperatorTableRelease(CGPDFOperatorTableRef table);

/* A LATER SET FOR THE SAME NAME REPLACES THE EARLIER ONE, and a NULL callback removes it. `name` is copied. */
CG_EXTERN void CGPDFOperatorTableSetCallback(CGPDFOperatorTableRef table, const char *name,
					     CGPDFOperatorCallback callback);

/* THE SCANNER'S LOOKUP, NOT APPLE'S — declared here because the two types are built together. NULL means the
 * table has no caller for that operator. */
CGPDFOperatorCallback cg_pdf_operator_table_lookup(CGPDFOperatorTableRef table, const char *name);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFOPERATORTABLE_H */
