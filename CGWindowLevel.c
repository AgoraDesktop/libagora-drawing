/*
 * CGWindowLevel.c — the one function behind the level names.
 *
 * Copyright © 2026 Kyle J. Cardoza. MIT licensed — see LICENSE.
 * SPDX-License-Identifier: MIT
 *
 * **THE NUMBERS ARE OURS AND THEY ARE CHOSEN TO AGREE WITH `NSWindow.h`, WHICH ALREADY WROTE ITS OWN DOWN.**
 * Apple publishes the key and the function and no values, so this is the tree's own set: the SAME set
 * `NSWindow.h` states for `NSNormalWindowLevel` (0), the floating level (3), the modal panel (8), the dock (20),
 * the main menu (24), the status level (25) and the pop-up menu (101), plus the values `NSWindow.m` gives
 * `NSScreenSaverWindowLevel` (1000) — so a Core Graphics caller and an AppKit caller cannot come to differ.
 *
 * **THE DESKTOP IS NEGATIVE AND THAT IS THE WHOLE POINT OF THE LEVEL.** The desktop is not "a window that
 * happens to be at the back": it is at a level BELOW every normal window, so the compositor draws it first and
 * everything else over it. `kCGDesktopIconWindowLevel` is one above it (the icons drawn on the wallpaper), and
 * `kCGBackstopMenuLevel` one above that and still below normal.
 */
#include <CoreGraphics/CGWindowLevel.h>

#include <stdint.h>

/* INT32_MIN + the offsets, so the desktop block sits under any level a caller could reach by counting down. */
#define AG_BASE		INT32_MIN
#define AG_DESKTOP	(AG_BASE + 20)
#define AG_DESKTOP_ICON	(AG_BASE + 21)
#define AG_BACKSTOP	(AG_BASE + 22)

CGWindowLevel CGWindowLevelForKey(CGWindowLevelKey key)
{
	switch (key) {
	case kCGBaseWindowLevelKey:		return AG_BASE;
	case kCGMinimumWindowLevelKey:		return (AG_BASE + 1);
	case kCGDesktopWindowLevelKey:		return AG_DESKTOP;
	case kCGDesktopIconWindowLevelKey:	return AG_DESKTOP_ICON;
	case kCGBackstopMenuLevelKey:		return AG_BACKSTOP;

	case kCGNormalWindowLevelKey:		return 0;
	case kCGFloatingWindowLevelKey:		return 3;
	case kCGTornOffMenuWindowLevelKey:	return 3;
	case kCGModalPanelWindowLevelKey:	return 8;
	case kCGUtilityWindowLevelKey:		return 9;
	case kCGHelpWindowLevelKey:		return 10;
	case kCGDockWindowLevelKey:		return 20;
	case kCGMainMenuWindowLevelKey:		return 24;
	case kCGStatusWindowLevelKey:		return 25;
	case kCGPopUpMenuWindowLevelKey:	return 101;
	case kCGOverlayWindowLevelKey:		return 102;
	case kCGDraggingWindowLevelKey:		return 500;
	case kCGScreenSaverWindowLevelKey:	return 1000;
	case kCGCursorWindowLevelKey:		return 1500;
	case kCGAssistiveTechHighWindowLevelKey: return 2000;
	case kCGMaximumWindowLevelKey:		return INT32_MAX;

	default:				return 0;
	}
}
