#pragma once

#include "kmsvnc.h"

#define DRM_IOCTL_MUST(...) do{ int e; if ((e = drmIoctl(__VA_ARGS__))) KMSVNC_FATAL("DRM ioctl error %d on line %d\n", e, __LINE__); } while(0)
#define DRM_IOCTL_MAY(...) do{ int e; if ((e = drmIoctl(__VA_ARGS__))) fprintf(stderr, "DRM ioctl error %d on line %d\n", e, __LINE__); } while(0)
#define DRM_R_IOCTL_MAY(...) do{ int e; if ((e = ioctl(__VA_ARGS__))) fprintf(stderr, "DRM ioctl error %d on line %d\n", e, __LINE__); } while(0)


void drm_cleanup();
int drm_open();
int drm_vendors();
int drm_dump_cursor_plane(char **data, int *width, int *height);
int drm_get_cursor_position(int *x, int *y);
void drm_composite_cursor_into_fb(char *fb, int fb_w, int fb_h, char *cursor, int c_w, int c_h, int c_x, int c_y);
