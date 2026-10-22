/*
 * window_levels — F4's measurement: the Core Graphics window-level vocabulary.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * WHAT THIS HOLDS UP, in WORKSPACE-PLAN.md's words for F4: `kCGDesktopWindowLevel` is BELOW every window, and
 * the vocabulary is ORDERED — the thing a compositor and a caller both rely on and neither can derive. It also
 * holds the agreement that matters more than any single number: THE VALUES ARE THE ONES `NSWindow.h` STATES,
 * checked here rather than asserted in a comment, so Core Graphics and the AppKit cannot drift apart.
 */
#include <CoreGraphics/CGWindowLevel.h>

#include <stdint.h>
#include <stdio.h>

static int failed;

static void check(int ok, const char *name)
{
	printf("  [%s] %s\n", ok ? "ok" : "FAIL", name);
	if (!ok) failed++;
}

int main(void)
{
	/* THE DESKTOP, WHICH IS THE POINT OF F4. */
	check(kCGDesktopWindowLevel < kCGNormalWindowLevel,
	      "the DESKTOP level is BELOW the normal level, so everything is drawn over it");
	check(kCGDesktopWindowLevel < 0, "and it is negative, not merely lower by a small amount");
	check(kCGDesktopIconWindowLevel > kCGDesktopWindowLevel,
	      "the desktop-ICON level is above the desktop level");
	check(kCGDesktopIconWindowLevel < kCGNormalWindowLevel, "and still below every window");
	check(kCGBackstopMenuLevel > kCGDesktopIconWindowLevel && kCGBackstopMenuLevel < kCGNormalWindowLevel,
	      "the backstop sits between the desktop icons and the normal level");

	/* THE ORDER THE WINDOW SERVER'S COMPOSITING LOOP RELIES ON. */
	check(kCGNormalWindowLevel < kCGFloatingWindowLevel, "normal < floating");
	check(kCGFloatingWindowLevel == kCGTornOffMenuWindowLevel, "a torn-off menu IS the floating level, as Apple's aliases say");
	check(kCGFloatingWindowLevel < kCGModalPanelWindowLevel, "floating < modal panel");
	check(kCGModalPanelWindowLevel < kCGDockWindowLevel, "modal panel < dock");
	check(kCGDockWindowLevel < kCGMainMenuWindowLevel, "dock < main menu");
	check(kCGMainMenuWindowLevel < kCGStatusWindowLevel, "main menu < status");
	check(kCGStatusWindowLevel < kCGPopUpMenuWindowLevel, "status < pop-up menu");
	check(kCGPopUpMenuWindowLevel < kCGScreenSaverWindowLevel, "pop-up menu < screen saver");
	check(kCGScreenSaverWindowLevel < kCGMaximumWindowLevel, "screen saver < maximum");

	/* THE AGREEMENT WITH `NSWindow.h`, WHICH WROTE ITS VALUES DOWN FIRST. */
	check(kCGNormalWindowLevel == 0, "the normal level is 0, as NSNormalWindowLevel is");
	check(kCGFloatingWindowLevel == 3, "floating is 3, as NSFloatingWindowLevel is");
	check(kCGModalPanelWindowLevel == 8, "the modal panel is 8, as NSModalPanelWindowLevel is");
	check(kCGDockWindowLevel == 20, "the dock is 20, as NSDockWindowLevel is");
	check(kCGMainMenuWindowLevel == 24, "the main menu is 24, as NSMainMenuWindowLevel is");
	check(kCGStatusWindowLevel == 25, "the status level is 25, as NSStatusWindowLevel is");
	check(kCGPopUpMenuWindowLevel == 101, "the pop-up menu is 101, as NSPopUpMenuWindowLevel is");

	/* THE KEY AND THE FUNCTION AGREE, which is what makes the macros safe to use anywhere. */
	check(CGWindowLevelForKey(kCGDesktopWindowLevelKey) == kCGDesktopWindowLevel,
	      "kCGDesktopWindowLevel is CGWindowLevelForKey(kCGDesktopWindowLevelKey)");
	check(CGWindowLevelForKey(kCGNormalWindowLevelKey) == kCGNormalWindowLevel, "and so is the normal level");
	check(CGWindowLevelForKey((CGWindowLevelKey)9999) == 0, "an unknown key answers 0 rather than a level nobody asked for");

	printf("window_levels: %d case(s) failed\n", failed);
	return failed == 0 ? 0 : 1;
}
