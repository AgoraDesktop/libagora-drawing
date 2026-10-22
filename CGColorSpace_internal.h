/*
 * CGColorSpace_internal.h — the seam between the colour space and the colour.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * NOT INSTALLED, NOT INCLUDED BY ANY PUBLIC HEADER, and named the way libconfig_internal.h is
 * for the same reason: one translation unit knows how a colour space is built, and one other
 * needs to ask it for the engine's handle. The alternative was to publish the struct, which
 * would make every field part of the ABI.
 *
 * THE HANDLE IS `void *` AND NOT `cmsHPROFILE`, DELIBERATELY: so that this header — and
 * therefore any file that includes it only for the accessor — does not have to pull in
 * `<lcms2.h>`. lcms2's own `cmsHPROFILE` is a `void *`, so the value passes through both ways
 * without a cast at the seam.
 */
#ifndef CORE_GRAPHICS_CGCOLORSPACE_INTERNAL_H
#define CORE_GRAPHICS_CGCOLORSPACE_INTERNAL_H

#include <CoreGraphics/CGColorSpace.h>

/* THE ENGINE'S PROFILE FOR A SPACE, OR NULL. NULL IS THE ANSWER FOR EVERY DEVICE SPACE, and
 * that is not a gap: a device space means "these numbers are what you blend", so there is
 * nothing to interpret and nothing to convert FROM. It is also the answer for a space with no
 * colour model the engine can build a profile for — device CMYK, today. A caller that gets
 * NULL must either copy the components as they stand (which is right for device RGB and device
 * grayscale) or refuse. */
void *cg_colorspace_engine_profile(CGColorSpaceRef space);

/* THE ONLY SPACE WHOSE NUMBERS THIS LIBRARY INTERPRETS ITSELF, WHICH IS WHY IT NEEDS A PREDICATE RATHER
 * THAN A MODEL TEST. Device CMYK has no engine profile (above), so a CMYK colour's four inks become
 * light through `cg_device_cmyk_to_rgb` below — this tree's own rule, stated there. An ICC profile whose
 * data space is CMYK reports the same MODEL and must NOT come here: its numbers belong to the profile.
 * `space == the device singleton` is the whole test, which is what makes it exact. */
int cg_color_space_is_device_cmyk(CGColorSpaceRef space);

/* DEVICE CMYK (four inks plus an alpha) AS DEVICE RGB (three channels plus the alpha), by
 * `r = (1-c)(1-k)`, `g = (1-m)(1-k)`, `b = (1-y)(1-k)`. See the definition for why this is a stated
 * choice rather than a duplicated contract. */
void cg_device_cmyk_to_rgb(const CGFloat cmyk[5], CGFloat rgb[4]);

/* A SPACE FROM AN ENGINE PROFILE THAT IS ALREADY OPEN, WHICH IS THE HALF OF THE CONSTRUCTION
 * THAT LIVES WITH THE STRUCT. The named spaces are built from an Objective-C translation unit —
 * the only place a name can be compared — but a `CGColorSpace` is private to CGColorSpace.c, so
 * the profile comes here to be wrapped. ON SUCCESS THE SPACE OWNS THE PROFILE and closes it when
 * it is released; on failure the caller still owns it and must close it, which is why the two
 * sides of this seam both mention the same rule. */
CGColorSpaceRef cg_colorspace_from_profile(void *profile, CGColorSpaceModel model,
					   size_t components);

/* THE PARSED PROFILE HANDLE, for the one door that has to serialise it back to bytes. `void *` rather than
 * `cmsHPROFILE` so that a caller of this header does not need the engine's header to name the type: the
 * translation unit that uses it knows what it is, and the rest do not have to. NULL for a space that has no
 * profile, which is every device space. */
void *cg_colorspace_profile_handle(CGColorSpaceRef space);

#endif /* CORE_GRAPHICS_CGCOLORSPACE_INTERNAL_H */
