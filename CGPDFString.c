/*
 * CGPDFString.c — PDF strings, and the two readings Apple's API puts on them.
 *
 * Copyright © 2026 Kyle J. Cardoza.
 * SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
 * SPDX-License-Identifier: LGPL-3.0-or-later
 * Portions of this file may derive from GNUstep; see THIRD-PARTY.md.
 *
 * **A STRING IS BYTES AND ITS TEXT READING IS AN ENCODING GUESS WITH A RULE.** PDF 1.5 made the guess an
 * explicit one: a leading `FE FF` means UTF-16BE, and the absence of it means PDFDocEncoding — a variant of
 * Latin-1 with its own assignments in 0x18..0x1F, 0x80..0x9F and 0xA0. That table is written out below rather
 * than approximated, because the slots it differs in are exactly the ones a document's `/Title` uses for
 * typographic quotes and dashes, and a wrong one is visible.
 *
 * **THE UNDEFINED SLOTS ANSWER U+FFFD, NOT A REFUSAL.** A title with one undefined byte is still a title; a door
 * that returned NULL would cost the caller the other forty characters. The bytes remain available through
 * `GetBytePtr` for a caller that wants to know exactly what was there.
 *
 * **A DATE IS THE PDF's `D:` SPELLING.** It is not ISO 8601, the timezone is spelled `+HH'mm'` with apostrophes,
 * and every field after the year is optional. A date with no timezone is read as UTC: a PDF stores no local
 * time, and treating it as local would make the same file mean different instants on different machines.
 */
#include <CoreGraphics/CGPDF_internal.h>
#include <CoreGraphics/CGPDFString.h>

#include <CoreFoundation/CFString.h>

#include <math.h>
#include <string.h>
#include <time.h>

size_t CGPDFStringGetLength(CGPDFStringRef string)
{
	cg_pdf_obj *s = (cg_pdf_obj *)string;

	return (s != NULL && s->type == kCGPDFObjectTypeString) ? s->u.string.len : 0;
}

const unsigned char *CGPDFStringGetBytePtr(CGPDFStringRef string)
{
	cg_pdf_obj *s = (cg_pdf_obj *)string;

	return (s != NULL && s->type == kCGPDFObjectTypeString) ? s->u.string.bytes : NULL;
}

/* THE SLOTS PDFDOCENCODING ASSIGNS DIFFERENTLY, and only those: everything else is its byte value. The two
 * ranges are the specification's table (Annex D), typed out once. */
static unsigned int fn_pdfdoc_unicode(unsigned char c)
{
	switch (c) {
	case 0x18: return 0x02D8;	/* breve */
	case 0x19: return 0x02C7;	/* caron */
	case 0x1A: return 0x02C6;	/* circumflex */
	case 0x1B: return 0x02D9;	/* dot above */
	case 0x1C: return 0x02DD;	/* double acute */
	case 0x1D: return 0x02DB;	/* ogonek */
	case 0x1E: return 0x02DA;	/* ring above */
	case 0x1F: return 0x02DC;	/* small tilde */
	case 0x7F: return 0xFFFD;
	case 0x80: return 0x2022;	/* bullet */
	case 0x81: return 0x2020;	/* dagger */
	case 0x82: return 0x2021;	/* double dagger */
	case 0x83: return 0x2026;	/* ellipsis */
	case 0x84: return 0x2014;	/* em dash */
	case 0x85: return 0x2013;	/* en dash */
	case 0x86: return 0x0192;	/* florin */
	case 0x87: return 0x2044;	/* fraction slash */
	case 0x88: return 0x2039;	/* single left angle quote */
	case 0x89: return 0x203A;	/* single right angle quote */
	case 0x8A: return 0x2212;	/* minus */
	case 0x8B: return 0x2030;	/* per mille */
	case 0x8C: return 0x201E;	/* double low quote */
	case 0x8D: return 0x201C;	/* left double quote */
	case 0x8E: return 0x201D;	/* right double quote */
	case 0x8F: return 0x2018;	/* left single quote */
	case 0x90: return 0x2019;	/* right single quote */
	case 0x91: return 0x201A;	/* single low quote */
	case 0x92: return 0x2122;	/* trade mark */
	case 0x93: return 0xFB01;	/* fi */
	case 0x94: return 0xFB02;	/* fl */
	case 0x95: return 0x0141;	/* L with stroke */
	case 0x96: return 0x0152;	/* OE */
	case 0x97: return 0x0160;	/* S with caron */
	case 0x98: return 0x0178;	/* Y with diaeresis */
	case 0x99: return 0x017D;	/* Z with caron */
	case 0x9A: return 0x0131;	/* dotless i */
	case 0x9B: return 0x0142;	/* l with stroke */
	case 0x9C: return 0x0153;	/* oe */
	case 0x9D: return 0x0161;	/* s with caron */
	case 0x9E: return 0x017E;	/* z with caron */
	case 0x9F: return 0xFFFD;	/* undefined in the table */
	case 0xA0: return 0x20AC;	/* euro */
	default: return c;
	}
}

CFStringRef CGPDFStringCopyTextString(CGPDFStringRef string)
{
	cg_pdf_obj *s = (cg_pdf_obj *)string;
	UniChar *chars;
	size_t count;
	size_t i;

	if (s == NULL || s->type != kCGPDFObjectTypeString) {
		return NULL;
	}
	if (s->u.string.len >= 2 && s->u.string.bytes[0] == 0xFE && s->u.string.bytes[1] == 0xFF) {
		size_t n = (s->u.string.len - 2) / 2;

		chars = CFAllocatorAllocate(kCFAllocatorDefault, (CFIndex)(n ? n : 1) * sizeof(UniChar), 0);
		if (chars == NULL) {
			return NULL;
		}
		for (i = 0; i < n; i++) {
			chars[i] = (UniChar)(((unsigned)s->u.string.bytes[2 + i * 2] << 8) |
					     s->u.string.bytes[3 + i * 2]);
		}
		{
			CFStringRef result = CFStringCreateWithCharacters(kCFAllocatorDefault, chars, (CFIndex)n);

			CFAllocatorDeallocate(kCFAllocatorDefault, chars);
			return result;
		}
	}
	count = s->u.string.len;
	chars = CFAllocatorAllocate(kCFAllocatorDefault, (CFIndex)(count ? count : 1) * sizeof(UniChar), 0);
	if (chars == NULL) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		chars[i] = (UniChar)fn_pdfdoc_unicode(s->u.string.bytes[i]);
	}
	{
		CFStringRef result = CFStringCreateWithCharacters(kCFAllocatorDefault, chars, (CFIndex)count);

		CFAllocatorDeallocate(kCFAllocatorDefault, chars);
		return result;
	}
}

/* `n' DIGITS AT `at', AND HOW MANY WERE THERE. The caller decides whether the absence is legal. */
static int fn_digits(const unsigned char *bytes, size_t len, size_t at, int width)
{
	int value = 0;
	int n = 0;

	while (n < width && at + (size_t)n < len && bytes[at + n] >= '0' && bytes[at + n] <= '9') {
		value = value * 10 + (bytes[at + n] - '0');
		n++;
	}
	return n == width ? value : -1;
}

CFDateRef CGPDFStringCopyDate(CGPDFStringRef string)
{
	cg_pdf_obj *s = (cg_pdf_obj *)string;
	const unsigned char *b;
	size_t len;
	size_t at;
	int year, month = 1, day = 1, hour = 0, minute = 0, second = 0;
	int zone_sign = 0, zone_hour = 0, zone_minute = 0;
	struct tm tm;
	time_t seconds;
	double absolute;

	if (s == NULL || s->type != kCGPDFObjectTypeString) {
		return NULL;
	}
	b = s->u.string.bytes;
	len = s->u.string.len;
	at = 0;
	if (len >= 2 && b[0] == 'D' && b[1] == ':') {
		at = 2;
	}
	year = fn_digits(b, len, at, 4);
	if (year < 0) {
		return NULL;
	}
	at += 4;
	/* AFTER THE YEAR EVERY FIELD IS OPTIONAL AND ITS ABSENCE MEANS ZERO — EXCEPT THE MONTH AND DAY, WHOSE
	 * ZERO WOULD BE AN INVALID DATE AND ARE 1 INSTEAD. */
	if (at < len && b[at] == '-') {
		return NULL;
	}
	{
		int m = fn_digits(b, len, at, 2);

		if (m >= 0) {
			month = m;
			at += 2;
			{
				int d = fn_digits(b, len, at, 2);

				if (d >= 0) {
					day = d;
					at += 2;
					{
						int h = fn_digits(b, len, at, 2);

						if (h >= 0) {
							hour = h;
							at += 2;
							{
								int mi = fn_digits(b, len, at, 2);

								if (mi >= 0) {
									minute = mi;
									at += 2;
									{
										int se = fn_digits(b, len, at, 2);

										if (se >= 0) {
											second = se;
											at += 2;
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	if (at < len) {
		if (b[at] == 'Z') {
			at++;
		} else if (b[at] == '+' || b[at] == '-') {
			zone_sign = b[at] == '+' ? 1 : -1;
			at++;
			zone_hour = fn_digits(b, len, at, 2);
			if (zone_hour < 0) {
				zone_hour = 0;
			} else {
				at += 2;
				if (at < len && b[at] == '\'') {
					at++;
				}
				{
					int zm = fn_digits(b, len, at, 2);

					if (zm >= 0) {
						zone_minute = zm;
						at += 2;
					}
				}
				if (at < len && b[at] == '\'') {
					at++;
				}
			}
		}
	}

	memset(&tm, 0, sizeof tm);
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = hour;
	tm.tm_min = minute;
	tm.tm_sec = second;
	tm.tm_isdst = 0;
	seconds = timegm(&tm);
	/* A `+HH` ZONE MEANS THE LOCAL READING IS AHEAD OF UTC, so UTC is the local reading minus the offset. */
	seconds -= (time_t)(zone_sign * (zone_hour * 3600 + zone_minute * 60));
	absolute = (double)seconds - 978307200.0;	/* 2001-01-01T00:00:00Z in Unix seconds */
	return CFDateCreate(kCFAllocatorDefault, (CFAbsoluteTime)absolute);
}
