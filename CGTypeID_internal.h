/*
 * CGTypeID_internal — the table of type identities, and why the values are ours.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * ONE REGISTRY, ONE FILE, FIXED NUMBERS. Every `CG*GetTypeID` door in this library answers from the table
 * below, so the two properties the API exists for hold by construction rather than by luck: THE SAME CLASS
 * ALWAYS ANSWERS THE SAME ID (a constant, not an allocation), and TWO CLASSES NEVER SHARE ONE (the numbers
 * are distinct and assigned once, here).
 *
 * WHAT IS DUPLICATED AND WHAT IS NOT: the CONTRACT is Apple's — a type identity for Core Graphics objects, which
 * is what `CFGetTypeID` would compare against — and the VALUES are not, because Apple's are assigned by its
 * runtime at load time and appear in no header or documentation. A caller can therefore compare ids (ours
 * against ours) exactly as Apple's documentation describes, and cannot port a constant, which no caller
 * does. **THE TREE NOW HAS A COREFOUNDATION (2026-10-10), AND THE AGREEMENT IT ASKED FOR IS NOT ACHIEVABLE
 * IN THE DIRECTION IT NAMED.** This file was written when there was no `CFGetTypeID` to agree with, and it
 * said that when one existed, the agreement to make would be that `CFGetTypeID` answers THESE numbers.
 * Core Foundation assigns its type ids at registration, from 256 up, and a class cannot be given one of the
 * numbers below -- 1 through 255 belong to Core Foundation's own classes. SO THE AGREEMENT IS MADE THE OTHER
 * WAY ROUND, ONE CLASS AT A TIME: A CLASS THAT IS A CORE FOUNDATION TYPE ANSWERS CORE FOUNDATION'S ID, and
 * its constant below is superseded. `CGColor` is the first, because it is the first class that IS one (see
 * CGColor.c); the rest still answer their constants, because they are still opaque C structs. A caller
 * comparing `CGColorGetTypeID()` against `CFGetTypeID()` gets the same answer either way, which is the
 * property the table exists for.
 *
 * ONE IS NOT ZERO, AND ZERO IS THE INVALID ID: the numbering starts at 1 so that a zeroed structure, an
 * uninitialised field or a refusal can never be mistaken for a class identity.
 */
#ifndef CORE_GRAPHICS_CGTYPEID_INTERNAL_H
#define CORE_GRAPHICS_CGTYPEID_INTERNAL_H

#include <CoreGraphics/CGBase.h>

#include <CoreFoundation/CFBase.h>

/* The colour's identity, assigned by Core Foundation when its class is registered (CGColor.c).
 * CGTypeID.c's `CGColorGetTypeID` is the public door; this is what it answers with. */
CFTypeID cg_color_type_id(void);

/* THE TABLE. Adding a class means adding a line HERE and a door in CGTypeID.c — which is the whole point of
 * one file owning it: no id is ever picked at a call site. */
#define CG_TYPE_ID_CGCOLOR		1u
#define CG_TYPE_ID_CGCOLORSPACE		2u
#define CG_TYPE_ID_CGCONTEXT		3u
#define CG_TYPE_ID_CGDATAPROVIDER	4u
#define CG_TYPE_ID_CGFONT		5u
#define CG_TYPE_ID_CGFUNCTION		6u
#define CG_TYPE_ID_CGGRADIENT		7u
#define CG_TYPE_ID_CGIMAGE		8u
#define CG_TYPE_ID_CGPATH		9u
#define CG_TYPE_ID_CGPATTERN		10u
#define CG_TYPE_ID_CGSHADING		11u
#define CG_TYPE_ID_CGDATACONSUMER	12u
#define CG_TYPE_ID_CGLAYER		13u

#endif /* CORE_GRAPHICS_CGTYPEID_INTERNAL_H */
