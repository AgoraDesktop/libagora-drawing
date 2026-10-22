/*
 * CGPDFScanner.h — the content stream's interpreter: operands, operators, and a stack between them.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **POSTScript'S MODEL, AND THAT IS NOT AN ANALOGY: PDF CONTENT IS A POSTFIX LANGUAGE.** A content stream is a
 * sequence of operands and operators; operands accumulate on a stack, and an operator consumes what it needs and
 * then the stack is empty again. `10 20 m` pushes two numbers and runs `m`. The scanner is the tokeniser and the
 * stack; the MEANING of `m` is the operator table's callback, which is exactly why this type is useful without
 * any drawing in it.
 *
 * **THE SCANNER DOES NOT DRAW.** It hands callbacks the operands and lets them decide. A caller can therefore
 * use it to render (behind `CGContextDrawPDFPage`), to inspect (`cgpdfbox`-style tools), or to rewrite a stream,
 * with the same tokeniser. That separation is the reason it exists as a public type rather than as a private
 * loop inside the renderer.
 *
 * **THE STACK IS CLEARED AFTER EVERY OPERATOR**, which is the specification's own model and also what protects a
 * damaged stream: operands left over are not carried into the next operator where they would silently change the
 * meaning of the next one.
 *
 * AN OPERAND IS A PARSED OBJECT. Numbers, names, strings, arrays and dictionaries are the same object type the
 * document's parser produces, so a callback can hand a `/Matrix` array straight to the same reader that read the
 * page tree. Indirect references do not occur in a valid content stream and are resolved if a damaged one has
 * them.
 *
 * INLINE IMAGES (`BI ... ID <bytes> EI`) ARE THE ONE PLACE WHERE THE TOKEN MODEL BREAKS: the bytes between `ID`
 * and `EI` are not tokens and may contain anything. The scanner skips them wholesale rather than letting a stray
 * `EI` inside the data end the image early, and states that the image is not offered to a callback.
 */
#ifndef CORE_GRAPHICS_CGPDFSCANNER_H
#define CORE_GRAPHICS_CGPDFSCANNER_H

#include <CoreGraphics/CGBase.h>
#include <CoreGraphics/CGPDFContentStream.h>
#include <CoreGraphics/CGPDFObject.h>

CF_IMPLICIT_BRIDGING_ENABLED

typedef struct CGPDFScanner *CGPDFScannerRef;
struct CGPDFOperatorTable;

CG_EXTERN CGPDFScannerRef CGPDFScannerCreate(CGPDFContentStreamRef contentStream,
					     struct CGPDFOperatorTable *table, void *info) CF_RETURNS_RETAINED;

CG_EXTERN CGPDFScannerRef CGPDFScannerRetain(CGPDFScannerRef scanner);
CG_EXTERN void CGPDFScannerRelease(CGPDFScannerRef scanner);

/* **RUN THE WHOLE STREAM.** True when every stream was tokenised to its end; false when the tokeniser met
 * something that is neither an operand nor an operator, which is a damaged stream and is reported rather than
 * silently stopping. */
CG_EXTERN bool CGPDFScannerScan(CGPDFScannerRef scanner);

CG_EXTERN CGPDFContentStreamRef CGPDFScannerGetContentStream(CGPDFScannerRef scanner);

/* THE STACK, POPPED. Each of these takes the top operand, checks its type and consumes it. A wrong type consumes
 * nothing and answers false, so a callback that pops twice in the wrong order sees the first answer and not a
 * corrupted stack. */
CG_EXTERN bool CGPDFScannerPopObject(CGPDFScannerRef scanner, CGPDFObjectRef *value);
CG_EXTERN bool CGPDFScannerPopBoolean(CGPDFScannerRef scanner, CGPDFBoolean *value);
CG_EXTERN bool CGPDFScannerPopInteger(CGPDFScannerRef scanner, CGPDFInteger *value);
CG_EXTERN bool CGPDFScannerPopNumber(CGPDFScannerRef scanner, CGPDFReal *value);
CG_EXTERN bool CGPDFScannerPopName(CGPDFScannerRef scanner, const char **value);
CG_EXTERN bool CGPDFScannerPopString(CGPDFScannerRef scanner, CGPDFStringRef *value);
CG_EXTERN bool CGPDFScannerPopArray(CGPDFScannerRef scanner, CGPDFArrayRef *value);
CG_EXTERN bool CGPDFScannerPopDictionary(CGPDFScannerRef scanner, CGPDFDictionaryRef *value);
CG_EXTERN bool CGPDFScannerPopStream(CGPDFScannerRef scanner, CGPDFStreamRef *value);

/* HOW MANY OPERANDS ARE WAITING — this library's own door (Apple has none), and the only way a callback can ask
 * whether an optional operand is present without consuming the required ones first. */
CG_EXTERN size_t CGPDFScannerGetOperandCount(CGPDFScannerRef scanner);

CF_IMPLICIT_BRIDGING_DISABLED

#endif /* CORE_GRAPHICS_CGPDFSCANNER_H */
