#include "../mac-chrome-page-region.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

/* Standalone macOS test:
 * clang -Wall -Wextra -Werror -framework Cocoa -framework ApplicationServices \
 *   plugins/mac-capture/mac-chrome-page-region.m \
 *   plugins/mac-capture/tests/chrome-page-region-test.c -o /tmp/chrome-page-region-test
 * /tmp/chrome-page-region-test
 */

static void expect_crop(CGRect window, CGRect page, uint32_t width, uint32_t height, int left, int top, int right,
			int bottom)
{
	struct chrome_page_crop crop = {0};
	assert(chrome_page_crop_from_rects(window, page, width, height, &crop));
	assert(crop.left == left);
	assert(crop.top == top);
	assert(crop.right == right);
	assert(crop.bottom == bottom);
}

int main(void)
{
	CGRect window = CGRectMake(100, 80, 1000, 800);
	CGRect page = CGRectMake(100, 200, 1000, 680);
	expect_crop(window, page, 1000, 800, 0, 120, 0, 0);
	expect_crop(window, page, 2000, 1600, 0, 240, 0, 0);
	expect_crop(window, page, 500, 400, 0, 60, 0, 0);

	/* Windows on monitors to the left/above the main display retain the same crop. */
	expect_crop(CGRectMake(-1200, -900, 1000, 800), CGRectMake(-1200, -780, 1000, 680), 2000, 1600, 0, 240, 0, 0);
	/* A sidebar, bottom bar, or partially clipped AX frame can change every edge. */
	expect_crop(window, CGRectMake(120, 200, 700, 600), 2000, 1600, 40, 240, 560, 160);
	expect_crop(window, CGRectMake(80, 200, 1040, 900), 2000, 1600, 0, 240, 0, 0);
	/* Fractional point bounds round inward at the source's actual pixel scale. */
	expect_crop(CGRectMake(0, 0, 100, 100), CGRectMake(0.25, 10.25, 99.5, 89.5), 200, 200, 1, 21, 1, 1);
	expect_crop(window, window, 2000, 1600, 0, 0, 0, 0);

	struct chrome_page_crop crop;
	assert(!chrome_page_crop_from_rects(window, page, 0, 800, &crop));
	assert(!chrome_page_crop_from_rects(window, page, (uint32_t)INT_MAX + 1, 800, &crop));
	assert(!chrome_page_crop_from_rects(CGRectZero, page, 1000, 800, &crop));
	assert(!chrome_page_crop_from_rects(window, CGRectMake(2000, 0, 100, 100), 1000, 800, &crop));
	assert(!chrome_page_crop_from_rects(window, CGRectMake(NAN, 0, 100, 100), 1000, 800, &crop));
	assert(!chrome_page_crop_from_rects(window, CGRectMake(0, 0, INFINITY, 100), 1000, 800, &crop));
	assert(!chrome_page_crop_from_rects(window, page, 1000, 800, NULL));
	assert(!chrome_page_crop_from_rects(CGRectMake(0, 0, 100, 100), CGRectMake(0.1, 0.1, 0.1, 0.1), 100, 100,
					    &crop));

	assert(chrome_page_window_frames_match(window, window));
	assert(chrome_page_window_frames_match(window, CGRectMake(100.5, 80.5, 1000.5, 800.5)));
	assert(!chrome_page_window_frames_match(window, CGRectMake(102, 80, 1000, 800)));
	assert(!chrome_page_window_frames_match(window, CGRectMake(100, 80, 900, 800)));
	assert(!chrome_page_window_frames_match(window, CGRectNull));
	assert(!chrome_page_window_frames_match(window, CGRectMake(NAN, 80, 1000, 800)));
	puts("Chrome page geometry tests passed.");
	return 0;
}
