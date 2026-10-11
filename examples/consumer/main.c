/*
 * A client of libagora-drawing. It links AgoraDrawing::Drawing AND NOTHING ELSE.
 *
 * TWO FAILURES WOULD SHOW HERE RATHER THAN IN A TEST. IF CORE FOUNDATION'S HEADERS WERE NOT REACHABLE
 * THROUGH THE PACKAGE this file would not preprocess, because every public Core Graphics door is spelled
 * with CF types. AND IF THE LIBRARY WERE NOT LINKED the pixels below would not be there: the gradient is
 * drawn into the client's own buffer through the public context API and read back out of it.
 *
 * It also reaches Core Foundation DIRECTLY -- CFArrayCreate for the gradient's stops, CFGetTypeID to say so
 * -- which is what a real client does and what made the missing public dependency visible.
 */
#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGColor.h>
#include <CoreGraphics/CGColorSpace.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGGradient.h>
#include <CoreFoundation/CoreFoundation.h>

#include <stdio.h>

int main(void)
{
	unsigned char pixels[8 * 4];	/* eight pixels, B G R A */
	CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(pixels, 8, 1, 8, 8 * 4, rgb,
						 kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	CGColorRef red = CGColorCreateGenericRGB(1, 0, 0, 1);
	CGColorRef blue = CGColorCreateGenericRGB(0, 0, 1, 1);
	const void *stops[2];
	CFArrayRef ramp;
	CGGradientRef gradient;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (ctx == NULL || red == NULL || blue == NULL) {
		fprintf(stderr, "the client could not set up a context or a colour\n");
		return 1;
	}
	stops[0] = red;
	stops[1] = blue;
	ramp = CFArrayCreate(NULL, stops, 2, &kCFTypeArrayCallBacks);
	gradient = CGGradientCreateWithColors(rgb, ramp, NULL);
	if (gradient == NULL) {
		fprintf(stderr, "the client could not make a gradient\n");
		return 1;
	}
	CGContextDrawLinearGradient(ctx, gradient, CGPointMake(0, 0), CGPointMake(8, 0), 0);

	printf("the client drew a gradient into its own buffer:\n");
	printf("  first pixel B=%u G=%u R=%u A=%u\n", pixels[0], pixels[1], pixels[2], pixels[3]);
	printf("  last  pixel B=%u G=%u R=%u A=%u\n", pixels[28], pixels[29], pixels[30], pixels[31]);
	printf("  and Core Foundation came through the package: CFString type id %lu\n",
	       (unsigned long)CFGetTypeID(CFSTR("reached through find_package")));
	return 0;
}
