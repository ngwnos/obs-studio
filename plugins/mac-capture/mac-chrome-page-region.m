#include "mac-chrome-page-region.h"

#import <ApplicationServices/ApplicationServices.h>
#import <Cocoa/Cocoa.h>
#include <limits.h>
#include <math.h>

/* A browser that stops responding must not stall the frontend's polling timer. */
static const CFTimeInterval query_budget = 0.25;
static const float messaging_timeout = 0.04f;
static const CFIndex max_windows = 64;
static const CFIndex max_nodes = 512;
static const NSUInteger max_depth = 12;

static bool usable_rect(CGRect rect)
{
    return isfinite(rect.origin.x) && isfinite(rect.origin.y) && isfinite(rect.size.width) &&
           isfinite(rect.size.height) && rect.size.width > 0 && rect.size.height > 0 && isfinite(CGRectGetMaxX(rect)) &&
           isfinite(CGRectGetMaxY(rect));
}

bool chrome_page_window_frames_match(CGRect window, CGRect candidate)
{
    return usable_rect(window) && usable_rect(candidate) && fabs(window.origin.x - candidate.origin.x) <= 1.0 &&
           fabs(window.origin.y - candidate.origin.y) <= 1.0 && fabs(window.size.width - candidate.size.width) <= 1.0 &&
           fabs(window.size.height - candidate.size.height) <= 1.0;
}

bool chrome_page_crop_from_rects(CGRect window, CGRect page, uint32_t width, uint32_t height,
                                 struct chrome_page_crop *crop)
{
    if (!crop || !usable_rect(window) || !usable_rect(page) || !width || !height || width > INT_MAX || height > INT_MAX)
        return false;

    CGRect visible = CGRectIntersection(window, page);
    if (!usable_rect(visible))
        return false;

    double scale_x = width / window.size.width;
    double scale_y = height / window.size.height;
    /* Round inward so a fractional boundary cannot include browser chrome. */
    int left = (int) ceil(fmin(width, fmax(0, (CGRectGetMinX(visible) - CGRectGetMinX(window)) * scale_x)));
    int top = (int) ceil(fmin(height, fmax(0, (CGRectGetMinY(visible) - CGRectGetMinY(window)) * scale_y)));
    int end_x = (int) floor(fmin(width, (CGRectGetMaxX(visible) - CGRectGetMinX(window)) * scale_x));
    int end_y = (int) floor(fmin(height, (CGRectGetMaxY(visible) - CGRectGetMinY(window)) * scale_y));
    if (end_x <= left || end_y <= top)
        return false;

    *crop = (struct chrome_page_crop) {left, top, (int) width - end_x, (int) height - end_y};
    return true;
}

static bool query_expired(CFAbsoluteTime deadline)
{
    return CFAbsoluteTimeGetCurrent() >= deadline;
}

static CFTypeRef copy_attribute(AXUIElementRef element, CFStringRef name, CFAbsoluteTime deadline)
{
    if (query_expired(deadline))
        return NULL;

    /* Timeouts apply to this exact AX object, not its descendants. */
    AXUIElementSetMessagingTimeout(element, messaging_timeout);
    CFTypeRef value = NULL;
    if (AXUIElementCopyAttributeValue(element, name, &value) != kAXErrorSuccess)
        return NULL;
    return value;
}

static bool bool_attribute(AXUIElementRef element, CFStringRef name, CFAbsoluteTime deadline)
{
    CFTypeRef value = copy_attribute(element, name, deadline);
    bool result = value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value);
    if (value)
        CFRelease(value);
    return result;
}

static bool element_frame(AXUIElementRef element, CGRect *frame, CFAbsoluteTime deadline)
{
    CFTypeRef position = copy_attribute(element, kAXPositionAttribute, deadline);
    CFTypeRef size = copy_attribute(element, kAXSizeAttribute, deadline);
    CGPoint point = CGPointZero;
    CGSize dimensions = CGSizeZero;
    bool valid = position && size && CFGetTypeID(position) == AXValueGetTypeID() &&
                 CFGetTypeID(size) == AXValueGetTypeID() && AXValueGetType(position) == kAXValueCGPointType &&
                 AXValueGetType(size) == kAXValueCGSizeType && AXValueGetValue(position, kAXValueCGPointType, &point) &&
                 AXValueGetValue(size, kAXValueCGSizeType, &dimensions);
    if (position)
        CFRelease(position);
    if (size)
        CFRelease(size);
    *frame = (CGRect) {point, dimensions};
    return valid && usable_rect(*frame);
}

static AXUIElementRef copy_selected_window(AXUIElementRef application, CGRect bounds, NSString *title,
                                           CFAbsoluteTime deadline, const char **error)
{
    CFIndex count = 0;
    AXUIElementSetMessagingTimeout(application, messaging_timeout);
    if (AXUIElementGetAttributeValueCount(application, kAXWindowsAttribute, &count) != kAXErrorSuccess || count < 1 ||
        count > max_windows) {
        *error = "window_unavailable";
        return NULL;
    }
    CFArrayRef windows = NULL;
    if (query_expired(deadline) ||
        AXUIElementCopyAttributeValues(application, kAXWindowsAttribute, 0, count, &windows) != kAXErrorSuccess) {
        *error = "window_unavailable";
        return NULL;
    }

    AXUIElementRef match = NULL;
    AXUIElementRef title_match = NULL;
    unsigned matches = 0;
    unsigned title_matches = 0;
    bool incomplete = false;
    for (CFIndex index = 0; index < CFArrayGetCount(windows); index++) {
        AXUIElementRef candidate = (AXUIElementRef) CFArrayGetValueAtIndex(windows, index);
        if (CFGetTypeID(candidate) != AXUIElementGetTypeID()) {
            incomplete = true;
            break;
        }
        if (bool_attribute(candidate, kAXMinimizedAttribute, deadline))
            continue;

        CGRect frame;
        if (!element_frame(candidate, &frame, deadline)) {
            incomplete = true;
            break;
        }
        if (!chrome_page_window_frames_match(bounds, frame))
            continue;

        matches++;
        match = candidate;
        CFTypeRef candidate_title = copy_attribute(candidate, kAXTitleAttribute, deadline);
        if (title.length && candidate_title && CFGetTypeID(candidate_title) == CFStringGetTypeID() &&
            [title isEqualToString:(NSString *) candidate_title]) {
            title_matches++;
            title_match = candidate;
        }
        if (candidate_title)
            CFRelease(candidate_title);
    }

    /* Public AX APIs do not expose the CGWindowID. Never pick the focused/first window. */
    if (query_expired(deadline)) {
        match = NULL;
        *error = "query_timeout";
    } else if (incomplete) {
        match = NULL;
        *error = "window_unavailable";
    } else if (matches > 1) {
        match = title_matches == 1 ? title_match : NULL;
        *error = "window_ambiguous";
    }
    if (match)
        CFRetain(match);
    CFRelease(windows);
    return match;
}

static bool is_devtools(AXUIElementRef element, CFAbsoluteTime deadline)
{
    CFTypeRef url = copy_attribute(element, kAXURLAttribute, deadline);
    CFStringRef string = NULL;
    if (url && CFGetTypeID(url) == CFURLGetTypeID())
        string = CFURLGetString(url);
    else if (url && CFGetTypeID(url) == CFStringGetTypeID())
        string = url;
    bool result = string && CFStringHasPrefix(string, CFSTR("devtools://"));
    if (url)
        CFRelease(url);
    return result;
}

static bool find_page_frame(AXUIElementRef window, CGRect bounds, CGRect *page, CFAbsoluteTime deadline,
                            const char **error)
{
    NSMutableArray *elements = [NSMutableArray arrayWithObject:(id) window];
    NSMutableArray *depths = [NSMutableArray arrayWithObject:@0];
    unsigned matches = 0;
    for (NSUInteger index = 0; index < elements.count; index++) {
        if (query_expired(deadline)) {
            *error = "query_timeout";
            return false;
        }
        AXUIElementRef element = (AXUIElementRef) elements[index];
        if (bool_attribute(element, kAXHiddenAttribute, deadline))
            continue;

        CFTypeRef role = copy_attribute(element, kAXRoleAttribute, deadline);
        if (!role || CFGetTypeID(role) != CFStringGetTypeID()) {
            if (role)
                CFRelease(role);
            *error = query_expired(deadline) ? "query_timeout" : "page_unavailable";
            return false;
        }
        bool web_area = role && CFGetTypeID(role) == CFStringGetTypeID() && CFEqual(role, CFSTR("AXWebArea"));
        if (role)
            CFRelease(role);
        if (web_area) {
            CGRect frame;
            if (!is_devtools(element, deadline) && element_frame(element, &frame, deadline) &&
                usable_rect(CGRectIntersection(bounds, frame))) {
                *page = frame;
                matches++;
            }
            /* The WebArea frame is the viewport. Do not read the document's descendants. */
            continue;
        }

        CFIndex child_count = 0;
        CFStringRef children_attribute = kAXVisibleChildrenAttribute;
        AXUIElementSetMessagingTimeout(element, messaging_timeout);
        AXError status = AXUIElementGetAttributeValueCount(element, children_attribute, &child_count);
        if (status == kAXErrorAttributeUnsupported && !query_expired(deadline)) {
            children_attribute = kAXChildrenAttribute;
            status = AXUIElementGetAttributeValueCount(element, children_attribute, &child_count);
        }
        if (status == kAXErrorAttributeUnsupported || status == kAXErrorNoValue ||
            (status == kAXErrorSuccess && child_count == 0))
            continue;
        if (status != kAXErrorSuccess || child_count < 0 || elements.count + child_count > (NSUInteger) max_nodes ||
            [depths[index] unsignedIntegerValue] >= max_depth) {
            *error = "page_unavailable";
            return false;
        }
        CFArrayRef children = NULL;
        if (query_expired(deadline) ||
            AXUIElementCopyAttributeValues(element, children_attribute, 0, child_count, &children) != kAXErrorSuccess) {
            *error = query_expired(deadline) ? "query_timeout" : "page_unavailable";
            return false;
        }
        for (id child in (NSArray *) children) {
            if (CFGetTypeID((CFTypeRef) child) != AXUIElementGetTypeID())
                continue;
            [elements addObject:child];
            [depths addObject:@([depths[index] unsignedIntegerValue] + 1)];
        }
        CFRelease(children);
    }
    if (query_expired(deadline)) {
        *error = "query_timeout";
        return false;
    }
    if (matches != 1) {
        *error = matches ? "page_ambiguous" : "page_unavailable";
        return false;
    }
    return true;
}

struct chrome_page_region chrome_page_region_get(CGWindowID window, uint32_t width, uint32_t height)
{
    struct chrome_page_region result = {.error = "window_unavailable"};
    if (!window)
        return result;
    @autoreleasepool {
        NSArray *windows = (NSArray *) CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, window);
        NSDictionary *selected = windows.count == 1 ? windows[0] : nil;
        if (!selected || [selected[(id) kCGWindowNumber] unsignedIntValue] != window) {
            [windows release];
            return result;
        }
        pid_t pid = [selected[(id) kCGWindowOwnerPID] intValue];
        NSString *bundle = [NSRunningApplication runningApplicationWithProcessIdentifier:pid].bundleIdentifier;
        result.supported =
            [bundle isEqualToString:@"com.google.Chrome"] || [bundle isEqualToString:@"com.google.Chrome.beta"] ||
            [bundle isEqualToString:@"com.google.Chrome.dev"] || [bundle isEqualToString:@"com.google.Chrome.canary"];
        if (!result.supported) {
            result.error = "unsupported";
            [windows release];
            return result;
        }
        CGRect bounds = CGRectZero;
        bool visible = [selected[(id) kCGWindowIsOnscreen] boolValue];
        CFDictionaryRef bounds_dictionary = (CFDictionaryRef) selected[(id) kCGWindowBounds];
        bool has_bounds = bounds_dictionary && CFGetTypeID(bounds_dictionary) == CFDictionaryGetTypeID() &&
                          CGRectMakeWithDictionaryRepresentation(bounds_dictionary, &bounds);
        NSString *title = [[selected[(id) kCGWindowName] copy] autorelease];
        [windows release];
        if (!visible || !has_bounds || !usable_rect(bounds))
            return result;
        if (!AXIsProcessTrusted()) {
            result.error = "permission_required";
            return result;
        }
        if (!width || !height) {
            result.error = "source_unavailable";
            return result;
        }

        CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + query_budget;
        AXUIElementRef application = AXUIElementCreateApplication(pid);
        AXUIElementSetMessagingTimeout(application, messaging_timeout);
        /* Chromium enables its web tree for accessibility clients. Avoid rebuilding it each poll. */
        if (!bool_attribute(application, CFSTR("AXEnhancedUserInterface"), deadline) && !query_expired(deadline))
            AXUIElementSetAttributeValue(application, CFSTR("AXEnhancedUserInterface"), kCFBooleanTrue);
        AXUIElementRef selected_window = copy_selected_window(application, bounds, title, deadline, &result.error);
        CFRelease(application);
        if (!selected_window)
            return result;
        CGRect page = CGRectZero;
        result.error = "page_unavailable";
        result.valid = find_page_frame(selected_window, bounds, &page, deadline, &result.error) &&
                       chrome_page_crop_from_rects(bounds, page, width, height, &result.crop);
        CFRelease(selected_window);
        if (result.valid)
            result.error = "";
    }
    return result;
}

void chrome_page_region_request_access(void)
{
    const void *keys[] = {kAXTrustedCheckOptionPrompt};
    const void *values[] = {kCFBooleanTrue};
    CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    AXIsProcessTrustedWithOptions(options);
    CFRelease(options);
}
