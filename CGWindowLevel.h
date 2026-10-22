/*
 * CGWindowLevel.h — the window levels, keyed the way Core Graphics keys them.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * WRITTEN FROM APPLE'S 10.6 HEADER, and the SHAPE is why this file exists at all: Apple does not publish the
 * NUMBERS, it publishes a KEY and a FUNCTION — `CGWindowLevelForKey` — so that "levels may be changed or
 * adjusted in future releases without breaking binary compatibility" (Apple's own words, in the header this is
 * transcribed from). The names below are therefore MACROS over that function, which is Apple's own spelling and
 * is why `NSWindow.h` could not simply use them in an enum: a function call is not a compile-time constant, and
 * a caller's `switch` over a level needs one.
 *
 * **THE VALUES ARE OURS, AND THEY ARE STATED HERE AS THE TREE STATES THEM ELSEWHERE.** Apple declares the
 * function and no numbers, so `CGWindowLevel.c` chooses a set, and it is the SAME set `NSWindow.h` already
 * chose and wrote down — `NSNormalWindowLevel` is 0, the dock 20, the main menu 24, the status level 25, the
 * pop-up menu 101 — so the two frameworks cannot disagree. THE DESKTOP LEVELS ARE NEGATIVE and belong below
 * every window, which is what makes a desktop window the thing everything else is drawn over.
 */
#ifndef CORE_GRAPHICS_CGWINDOWLEVEL_H
#define CORE_GRAPHICS_CGWINDOWLEVEL_H

#include <CoreGraphics/CGBase.h>
#include <stdint.h>

typedef int32_t CGWindowLevel;
typedef int32_t CGWindowLevelKey;

enum {
	kCGBaseWindowLevelKey = 0,
	kCGMinimumWindowLevelKey,
	kCGDesktopWindowLevelKey,
	kCGBackstopMenuLevelKey,
	kCGNormalWindowLevelKey,
	kCGFloatingWindowLevelKey,
	kCGTornOffMenuWindowLevelKey,
	kCGDockWindowLevelKey,
	kCGMainMenuWindowLevelKey,
	kCGStatusWindowLevelKey,
	kCGModalPanelWindowLevelKey,
	kCGPopUpMenuWindowLevelKey,
	kCGDraggingWindowLevelKey,
	kCGScreenSaverWindowLevelKey,
	kCGMaximumWindowLevelKey,
	kCGOverlayWindowLevelKey,
	kCGHelpWindowLevelKey,
	kCGUtilityWindowLevelKey,
	kCGDesktopIconWindowLevelKey,
	kCGCursorWindowLevelKey,
	kCGAssistiveTechHighWindowLevelKey,
	kCGNumberOfWindowLevelKeys	/* Must be last, as in Apple's header. */
};

/* THE ONE FUNCTIONAL DOOR. `CG_AVAILABLE_STARTING` is not spelled here because this tree's `CGBase.h` does not
 * declare it; the door is the same door. */
CG_EXTERN CGWindowLevel CGWindowLevelForKey(CGWindowLevelKey key);

/* The number of levels Apple reserves for its own use. Carried because Apple's header carries it. */
#define kCGNumReservedWindowLevels	(16)

/* THE NAMES, EACH A MACRO OVER THE FUNCTION — Apple's own definitions of the older constant values. */
#define kCGBaseWindowLevel		CGWindowLevelForKey(kCGBaseWindowLevelKey)
#define kCGMinimumWindowLevel		CGWindowLevelForKey(kCGMinimumWindowLevelKey)
#define kCGDesktopWindowLevel		CGWindowLevelForKey(kCGDesktopWindowLevelKey)
#define kCGDesktopIconWindowLevel	CGWindowLevelForKey(kCGDesktopIconWindowLevelKey)
#define kCGBackstopMenuLevel		CGWindowLevelForKey(kCGBackstopMenuLevelKey)
#define kCGNormalWindowLevel		CGWindowLevelForKey(kCGNormalWindowLevelKey)
#define kCGFloatingWindowLevel		CGWindowLevelForKey(kCGFloatingWindowLevelKey)
#define kCGTornOffMenuWindowLevel	CGWindowLevelForKey(kCGTornOffMenuWindowLevelKey)
#define kCGDockWindowLevel		CGWindowLevelForKey(kCGDockWindowLevelKey)
#define kCGMainMenuWindowLevel		CGWindowLevelForKey(kCGMainMenuWindowLevelKey)
#define kCGStatusWindowLevel		CGWindowLevelForKey(kCGStatusWindowLevelKey)
#define kCGModalPanelWindowLevel	CGWindowLevelForKey(kCGModalPanelWindowLevelKey)
#define kCGPopUpMenuWindowLevel		CGWindowLevelForKey(kCGPopUpMenuWindowLevelKey)
#define kCGDraggingWindowLevel		CGWindowLevelForKey(kCGDraggingWindowLevelKey)
#define kCGScreenSaverWindowLevel	CGWindowLevelForKey(kCGScreenSaverWindowLevelKey)
#define kCGCursorWindowLevel		CGWindowLevelForKey(kCGCursorWindowLevelKey)
#define kCGOverlayWindowLevel		CGWindowLevelForKey(kCGOverlayWindowLevelKey)
#define kCGHelpWindowLevel		CGWindowLevelForKey(kCGHelpWindowLevelKey)
#define kCGUtilityWindowLevel		CGWindowLevelForKey(kCGUtilityWindowLevelKey)
#define kCGAssistiveTechHighWindowLevel	CGWindowLevelForKey(kCGAssistiveTechHighWindowLevelKey)
#define kCGMaximumWindowLevel		CGWindowLevelForKey(kCGMaximumWindowLevelKey)

#endif /* CORE_GRAPHICS_CGWINDOWLEVEL_H */
