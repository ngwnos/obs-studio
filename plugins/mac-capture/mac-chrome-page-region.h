#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

struct chrome_page_crop {
    int left;
    int top;
    int right;
    int bottom;
};

struct chrome_page_region {
    bool supported;
    bool valid;
    const char *error;
    struct chrome_page_crop crop;
};

/* AX and CGWindow coordinates both use screen points with a top-left origin. */
bool chrome_page_crop_from_rects(CGRect window, CGRect page, uint32_t width, uint32_t height,
                                 struct chrome_page_crop *crop);
bool chrome_page_window_frames_match(CGRect window, CGRect candidate);

/* Does not display a permission prompt. Error values are stable, unlocalized codes. */
struct chrome_page_region chrome_page_region_get(CGWindowID window, uint32_t width, uint32_t height);
void chrome_page_region_request_access(void);
