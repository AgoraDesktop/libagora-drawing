/*
 * CGTypeID — every `CG*GetTypeID` door in this library, in one file.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * THE TYPE IS `CGTypeID`, THIS LIBRARY'S OWN NAME (CGBase.h says why, and keeps Apple's spelling as an
 * alias), and THE ANSWER IS A CONSTANT, and the two properties the API promises — the same class always
 * answers the same id, and two classes never share one — are properties of the TABLE, not of any call site.
 * A door defined beside its class would put the numbers in eleven places and make them agree by review.
 *
 * AND NO DOOR IS MISSING FROM THE TABLE. THIS NOTE USED TO SAY THAT `CGDataConsumerGetTypeID` AND
 * `CGLayerGetTypeID` WERE OWED "because their CLASSES are -- this library has no `CGDataConsumer` and no
 * `CGLayer`", AND IT WAS STALE: both classes are here (`CGDataConsumer.c`, `CGLayer.c`), both doors are
 * defined, and MEASURED they answer 12 and 13.
 *
 * THE WHOLE TABLE WAS MEASURED, NOT JUST THE TWO, AND IT KEEPS BOTH OF THE PROMISES THIS FILE MAKES: the
 * same class answers the same id on a second call, and no two classes share one. The ids are 2..13 for the
 * types this library mints, and `CGColor` answers 256 because it is a registered CORE FOUNDATION type
 * rather than a CoreGraphics-minted one -- a difference in kind that the table makes visible rather than
 * hiding behind a contiguous range.
 */
#include <CoreGraphics/CGTypeID_internal.h>
#include <CoreGraphics/CGDataConsumer.h>
#include <CoreGraphics/CGLayer.h>

/* THE COLOUR IS A REAL CORE FOUNDATION TYPE, so its identity is the one Core Foundation assigned it
 * at registration rather than a constant from the table below (CGTypeID_internal.h records the
 * agreement this makes; CGColor.c does the registering). The other doors still answer table
 * constants because their classes are still opaque C structs. */
CGTypeID CGColorGetTypeID(void)
{
	return (CGTypeID)cg_color_type_id();
}

CGTypeID CGColorSpaceGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGCOLORSPACE;
}

CGTypeID CGContextGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGCONTEXT;
}

CGTypeID CGLayerGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGLAYER;
}

CGTypeID CGDataConsumerGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGDATACONSUMER;
}

CGTypeID CGDataProviderGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGDATAPROVIDER;
}

CGTypeID CGFontGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGFONT;
}

CGTypeID CGFunctionGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGFUNCTION;
}

CGTypeID CGGradientGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGGRADIENT;
}

CGTypeID CGImageGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGIMAGE;
}

CGTypeID CGPathGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGPATH;
}

CGTypeID CGPatternGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGPATTERN;
}

CGTypeID CGShadingGetTypeID(void)
{
	return (CGTypeID)CG_TYPE_ID_CGSHADING;
}

