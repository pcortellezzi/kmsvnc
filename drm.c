#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <libdrm/drm_fourcc.h>

#include "drm.h"
#include "va.h"

#ifndef DISABLE_KMSVNC_SCREEN_BLANK
    #include "drm_master.h"
#endif

#ifndef fourcc_mod_is_vendor
    #define fourcc_mod_is_vendor(modifier, vendor) \
            (fourcc_mod_get_vendor(modifier) == DRM_FORMAT_MOD_VENDOR_## vendor)
#endif
#ifdef DISABLE_KMSVNC_drmGetFormatName
    static char* drmGetFormatName(uint32_t data) {
        char *name = "missing drmGetFormatName";
        char *out = malloc(strlen(name)+1);
        if (out) {
            memcpy(out, name, strlen(name)+1);
        }
        return out;
    }
#endif

extern struct kmsvnc_data *kmsvnc;

static int check_pixfmt_non_vaapi() {
    if (
        kmsvnc->drm->mfb->pixel_format != KMSVNC_FOURCC_TO_INT('X', 'R', '2', '4') &&
        kmsvnc->drm->mfb->pixel_format != KMSVNC_FOURCC_TO_INT('A', 'R', '2', '4')
    )
    {
        KMSVNC_FATAL("Unsupported pixfmt %s, please create an issue with your pixfmt.\n", kmsvnc->drm->pixfmt_name);
    }
    return 0;
}

static void convert_copy(const char *in, int width, int height, char *buff)
{
    if (likely(in != buff)) {
        memcpy(buff, in, width * height * BYTES_PER_PIXEL);
    }
}

static void convert_bgra_to_rgba(const char *in, int width, int height, char *buff)
{
    if (likely(in != buff)) {
        memcpy(buff, in, width * height * BYTES_PER_PIXEL);
    }
    for (int i = 0; i < width * height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
        uint32_t pixdata = htonl(*((uint32_t*)(buff + i)));
        buff[i+0] = (pixdata & 0x0000ff00) >> 8;
        buff[i+2] = (pixdata & 0xff000000) >> 24;
    }
}

static inline char convert_buf_allocate(size_t len) {
    if (kmsvnc->drm->kms_convert_buf_len < len)
    {
        if (kmsvnc->drm->kms_convert_buf)
            free(kmsvnc->drm->kms_convert_buf);
        kmsvnc->drm->kms_convert_buf = malloc(len);
        if (!kmsvnc->drm->kms_convert_buf) return 1;
        kmsvnc->drm->kms_convert_buf_len = len;
    }
    return 0;
}
static inline void convert_x_tiled(const int tilex, const int tiley, const char *in, int width, int height, char *buff)
{
    if (width % tilex)
    {
        return;
    }
    if (height % tiley)
    {
        int sno = (width / tilex) + (height / tiley) * (width / tilex);
        int ord = (width % tilex) + (height % tiley) * tilex;
        int max_offset = sno * tilex * tiley + ord;
        if (kmsvnc->drm->kms_cpy_tmp_buf_len < max_offset * 4 + 4)
        {
            if (kmsvnc->drm->kms_cpy_tmp_buf)
                free(kmsvnc->drm->kms_convert_buf);
            kmsvnc->drm->kms_cpy_tmp_buf = malloc(max_offset * 4 + 4);
            if (!kmsvnc->drm->kms_cpy_tmp_buf) return;
            kmsvnc->drm->kms_cpy_tmp_buf_len = max_offset * 4 + 4;
        }
        memcpy(kmsvnc->drm->kms_cpy_tmp_buf, in, max_offset * 4 + 4);
        in = (const char *)kmsvnc->drm->kms_cpy_tmp_buf;
    }
    if (convert_buf_allocate(width * height * 4)) return;
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int sno = (x / tilex) + (y / tiley) * (width / tilex);
            int ord = (x % tilex) + (y % tiley) * tilex;
            int offset = sno * tilex * tiley + ord;
            memcpy(kmsvnc->drm->kms_convert_buf + (x + y * width) * 4, in + offset * 4, 4);
        }
    }
    convert_bgra_to_rgba(kmsvnc->drm->kms_convert_buf, width, height, buff);
}

void convert_nvidia_x_tiled_kmsbuf(const char *in, int width, int height, char *buff)
{
    convert_x_tiled(16, 128, in, width, height, buff);
}
void convert_intel_x_tiled_kmsbuf(const char *in, int width, int height, char *buff)
{
    convert_x_tiled(128, 8, in, width, height, buff);
}

static void convert_vaapi(const char *in, int width, int height, char *buff) {
    va_hwframe_to_vaapi(buff);
    if (
        (KMSVNC_FOURCC_TO_INT('R','G','B',0) & kmsvnc->va->selected_fmt->fourcc) == KMSVNC_FOURCC_TO_INT('R','G','B',0)
    ) {}
    else {
        // is 30 depth?
        if (kmsvnc->va->selected_fmt->depth == 30) {
            for (int i = 0; i < width * height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
                // ensure little endianess
                uint32_t pixdata = __builtin_bswap32(htonl(*((uint32_t*)(buff + i))));
                buff[i] = (pixdata & 0x3ff00000) >> 20 >> 2;
                buff[i+1] = (pixdata & 0xffc00) >> 10 >> 2;
                buff[i+2] = (pixdata & 0x3ff) >> 2;
            }
        }
        else {
            // actually, does anyone use this?
            if (!kmsvnc->va->selected_fmt->byte_order) {
                for (int i = 0; i < width * height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
                    uint32_t *pixdata = (uint32_t*)(buff + i);
                    *pixdata = __builtin_bswap32(*pixdata);
                }
            }
        }
        // is xrgb?
        if ((kmsvnc->va->selected_fmt->blue_mask | kmsvnc->va->selected_fmt->red_mask) < 0x1000000) {
            for (int i = 0; i < width * height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
                uint32_t *pixdata = (uint32_t*)(buff + i);
                *pixdata = ntohl(htonl(*pixdata) << 8);
            }
        }
        // is bgrx?
        if (kmsvnc->va->selected_fmt->blue_mask > kmsvnc->va->selected_fmt->red_mask) {
            for (int i = 0; i < width * height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
                uint32_t pixdata = htonl(*((uint32_t*)(buff + i)));
                buff[i+0] = (pixdata & 0x0000ff00) >> 8;
                buff[i+2] = (pixdata & 0xff000000) >> 24;
            }
        }
    }
}

static inline void drm_sync(int drmfd, uint64_t flags)
{
    struct dma_buf_sync sync = {
        .flags = flags,
    };
    DRM_R_IOCTL_MAY(drmfd, DMA_BUF_IOCTL_SYNC, &sync);
}

void drm_sync_start(int drmfd)
{
    drm_sync(drmfd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
}
void drm_sync_end(int drmfd)
{
    drm_sync(drmfd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
}
void drm_sync_noop(int drmfd)
{
}

void drm_cleanup() {
    if (kmsvnc->drm) {
#ifndef DISABLE_KMSVNC_SCREEN_BLANK
        if (kmsvnc->drm->gamma && kmsvnc->drm->gamma->size && kmsvnc->drm->gamma->red && kmsvnc->drm->gamma->green && kmsvnc->drm->gamma->blue) {
            if (drmModeCrtcSetGamma(kmsvnc->drm->drm_master_fd ?: kmsvnc->drm->drm_fd, kmsvnc->drm->plane->crtc_id, kmsvnc->drm->gamma->size, kmsvnc->drm->gamma->red, kmsvnc->drm->gamma->green, kmsvnc->drm->gamma->blue)) perror("Failed to restore gamma");
        }
        if (kmsvnc->drm->gamma && kmsvnc->drm->gamma->red) {
            free(kmsvnc->drm->gamma->red);
            kmsvnc->drm->gamma->red = kmsvnc->drm->gamma->green = kmsvnc->drm->gamma->blue = NULL;
        }
        if (kmsvnc->drm->gamma) {
            free(kmsvnc->drm->gamma);
            kmsvnc->drm->gamma = NULL;
        }
#endif
        if (kmsvnc->drm->drm_ver) {
            drmFreeVersion(kmsvnc->drm->drm_ver);
            kmsvnc->drm->drm_ver = NULL;
        }
        if (kmsvnc->drm->pixfmt_name) {
            free(kmsvnc->drm->pixfmt_name);
            kmsvnc->drm->pixfmt_name = NULL;
        }
        if (kmsvnc->drm->mod_vendor) {
            free(kmsvnc->drm->mod_vendor);
            kmsvnc->drm->mod_vendor = NULL;
        }
        if (kmsvnc->drm->mod_name) {
            free(kmsvnc->drm->mod_name);
            kmsvnc->drm->mod_name = NULL;
        }
        if (kmsvnc->drm->plane) {
            drmModeFreePlane(kmsvnc->drm->plane);
            kmsvnc->drm->plane = NULL;
        }
        if (kmsvnc->drm->cursor_plane) {
            drmModeFreePlane(kmsvnc->drm->cursor_plane);
            kmsvnc->drm->cursor_plane = NULL;
        }
        if (kmsvnc->drm->mfb) {
            drmModeFreeFB2(kmsvnc->drm->mfb);
            kmsvnc->drm->mfb = NULL;
        }
        if (kmsvnc->drm->cursor_mfb) {
            drmModeFreeFB2(kmsvnc->drm->cursor_mfb);
            kmsvnc->drm->cursor_mfb = NULL;
        }
        if (kmsvnc->drm->mapped && kmsvnc->drm->mapped != MAP_FAILED) {
            munmap(kmsvnc->drm->mapped, kmsvnc->drm->mmap_size);
            kmsvnc->drm->mapped = NULL;
        }
        if (kmsvnc->drm->cursor_mapped && kmsvnc->drm->cursor_mapped != MAP_FAILED) {
            munmap(kmsvnc->drm->cursor_mapped, kmsvnc->drm->cursor_mmap_size);
            kmsvnc->drm->cursor_mapped = NULL;
        }
        if (kmsvnc->drm->prime_fd > 0) {
            close(kmsvnc->drm->prime_fd);
            kmsvnc->drm->prime_fd = 0;
        }
        if (kmsvnc->drm->drm_fd > 0) {
            close(kmsvnc->drm->drm_fd);
            kmsvnc->drm->drm_fd = 0;
        }
        if (kmsvnc->drm->drm_master_fd > 0) {
            close(kmsvnc->drm->drm_master_fd);
            kmsvnc->drm->drm_master_fd = 0;
        }
        if (kmsvnc->drm->plane_res) {
            drmModeFreePlaneResources(kmsvnc->drm->plane_res);
            kmsvnc->drm->plane_res = NULL;
        }
        if (kmsvnc->drm->kms_convert_buf) {
            free(kmsvnc->drm->kms_convert_buf);
            kmsvnc->drm->kms_convert_buf = NULL;
        }
        kmsvnc->drm->kms_convert_buf_len = 0;
        if (kmsvnc->drm->kms_cpy_tmp_buf) {
            free(kmsvnc->drm->kms_cpy_tmp_buf);
            kmsvnc->drm->kms_cpy_tmp_buf = NULL;
        }
        kmsvnc->drm->kms_cpy_tmp_buf_len = 0;
        if (kmsvnc->drm->kms_cursor_buf) {
            free(kmsvnc->drm->kms_cursor_buf);
            kmsvnc->drm->kms_cursor_buf = NULL;
        }
        kmsvnc->drm->kms_cursor_buf_len = 0;
        free(kmsvnc->drm);
        kmsvnc->drm = NULL;
    }
}

static const char* drm_get_plane_type_name(uint64_t plane_type) {
    switch (plane_type) {
        case DRM_PLANE_TYPE_OVERLAY:
            return "overlay";
        case DRM_PLANE_TYPE_PRIMARY:
            return "primary";
        case DRM_PLANE_TYPE_CURSOR:
            return "cursor";
        default:
            return "unknown";
    }
};

static int drm_refresh_planes(char first_time) {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;
    if (!drm->plane && kmsvnc->source_plane > 0)
    {
        drm->plane = drmModeGetPlane(drm->drm_fd, kmsvnc->source_plane);
        if (!drm->plane)
            KMSVNC_FATAL("Failed to get plane %d: %s\n", kmsvnc->source_plane, strerror(errno));
        if (drm->plane->fb_id == 0)
            fprintf(stderr, "Place %d does not have an attached framebuffer\n", kmsvnc->source_plane);
    }
    if (!drm->plane || (kmsvnc->capture_cursor && !drm->cursor_plane)) {
        drmModePlane *current_plane = NULL;
        if (drm->plane_res) {
            drmModeFreePlaneResources(kmsvnc->drm->plane_res);
            drm->plane_res = NULL;
        }
        drm->plane_res = drmModeGetPlaneResources(drm->drm_fd);
        if (!drm->plane_res)
            KMSVNC_FATAL("Failed to get plane resources: %s\n", strerror(errno));
        int i;
        for (i = 0; i < drm->plane_res->count_planes; i++)
        {
            current_plane = drmModeGetPlane(drm->drm_fd, drm->plane_res->planes[i]);
            if (!current_plane)
            {
                fprintf(stderr, "Failed to get plane %u: %s\n", drm->plane_res->planes[i], strerror(errno));
                continue;
            }
            // get plane type
            uint64_t plane_type = 114514;
            drmModeObjectPropertiesPtr plane_props = drmModeObjectGetProperties(drm->drm_fd, current_plane->plane_id, DRM_MODE_OBJECT_PLANE);
            if (!plane_props) {
                fprintf(stderr, "Failed to get plane prop %u: %s\n", drm->plane_res->planes[i], strerror(errno));
            }
            else {
                for (int i = 0; i < plane_props->count_props; i++) {
                    drmModePropertyPtr plane_prop = drmModeGetProperty(drm->drm_fd, plane_props->props[i]);
                    if (strcmp(plane_prop->name, "type") == 0) {
                        plane_type = plane_props->prop_values[i];
                    }
                    drmModeFreeProperty(plane_prop);
                }
                drmModeFreeObjectProperties(plane_props);
            }
            assert(drm->plane_res->planes[i] == current_plane->plane_id);
            if (first_time) {
                printf("Plane %u CRTC %u FB %u Type %s\n", current_plane->plane_id, current_plane->crtc_id, current_plane->fb_id, drm_get_plane_type_name(plane_type));
            }
            // populate drm->plane and drm->cursor_plane
            char nofree = 0;
            if (current_plane->fb_id != 0) {
                if (!drm->plane) {
                    if (kmsvnc->source_crtc == 0 || current_plane->crtc_id == kmsvnc->source_crtc) {
                        nofree = 1;
                        drm->plane = current_plane;
                    }
                }
                // assume cursor plane is always after primary plane
                if (!drm->cursor_plane) {
                    if (drm->plane && drm->plane->crtc_id == current_plane->crtc_id && plane_type == DRM_PLANE_TYPE_CURSOR) {
                        nofree = 1;
                        drm->cursor_plane = current_plane;
                    }
                }
            }
            if ((!kmsvnc->capture_cursor || drm->cursor_plane) && drm->plane) {
                break;
            }
            if (!nofree) {
                drmModeFreePlane(current_plane);
            }
            current_plane = NULL;
        }
        if (!first_time) return 0;
        if (i == drm->plane_res->count_planes)
        {
            if (!drm->plane) {
                if (kmsvnc->source_crtc != 0)
                {
                    KMSVNC_FATAL("No usable planes found on CRTC %d\n", kmsvnc->source_crtc);
                }
                else
                {
                    KMSVNC_FATAL("No usable planes found\n");
                }
            }
            else if (!drm->cursor_plane) {
                fprintf(stderr, "No usable cursor plane found, cursor capture currently unavailable\n");
            }
        }
        printf("Using plane %u to locate framebuffers\n", drm->plane->plane_id);
        if (drm->cursor_plane) {
            printf("Using cursor plane %u\n", drm->cursor_plane->plane_id);
        }
    }
    return 0;
}

int drm_dump_cursor_plane(char **data, int *width, int *height) {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;

    if (!drm->cursor_plane) {
        drm_refresh_planes(0); // ignore error
        if (drm->cursor_plane) {
            printf("Using cursor plane %u\n", drm->cursor_plane->plane_id);
        }
    }
    else {
        uint32_t plane_id = drm->cursor_plane->plane_id;
        drmModeFreePlane(drm->cursor_plane);
        drm->cursor_plane = NULL;
        drm->cursor_plane = drmModeGetPlane(drm->drm_fd, plane_id);
    }
    if (!drm->cursor_plane) {
        data = NULL;
        return 1;
    }
    if (drm->cursor_mfb) drmModeFreeFB2(drm->cursor_mfb);
    drm->cursor_mfb = drmModeGetFB2(drm->drm_fd, drm->cursor_plane->fb_id);
    if (!drm->cursor_mfb) {
        KMSVNC_DEBUG("Cursor framebuffer missing\n");
        return 1;
    }

    if (drm->cursor_mfb->modifier != DRM_FORMAT_MOD_NONE && drm->cursor_mfb->modifier != DRM_FORMAT_MOD_LINEAR) {
        //kmsvnc->capture_cursor = 0;
        KMSVNC_DEBUG("Cursor plane modifier is not linear: %lu\n", drm->cursor_mfb->modifier);
        return 1;
    }

    if (
        drm->cursor_mfb->pixel_format != KMSVNC_FOURCC_TO_INT('A', 'R', '2', '4') &&
        drm->cursor_mfb->pixel_format != KMSVNC_FOURCC_TO_INT('A', 'R', '3', '0')
    )
    {
        //kmsvnc->capture_cursor = 0;
        char *fmtname = drmGetFormatName(drm->cursor_mfb->pixel_format);
        KMSVNC_DEBUG("Cursor plane pixel format unsupported (%u, %s)\n", drm->cursor_mfb->pixel_format, fmtname);
        free(fmtname);
        return 1;
    }

    struct drm_gem_flink flink;
    flink.handle = drm->cursor_mfb->handles[0];
    DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_GEM_FLINK, &flink);

    struct drm_gem_open open_arg;
    open_arg.name = flink.name;
    DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_GEM_OPEN, &open_arg);

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = open_arg.handle;
    DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

    size_t mmap_size = open_arg.size;
    if (mmap_size != drm->cursor_mfb->width * drm->cursor_mfb->height * BYTES_PER_PIXEL) {
        KMSVNC_DEBUG("Cursor plane mmap_size != calculated size (%ld, %d)\n", mmap_size, drm->cursor_mfb->width * drm->cursor_mfb->height * BYTES_PER_PIXEL);
        return 1;
    }

    off_t mmap_offset = mreq.offset;
    if (drm->cursor_mapped && drm->cursor_mapped != MAP_FAILED) munmap(drm->cursor_mapped, drm->cursor_mmap_size);
    drm->cursor_mapped = mmap(NULL, mmap_size, PROT_READ, MAP_SHARED, drm->drm_fd, mmap_offset);
    if (drm->cursor_mapped == MAP_FAILED)
    {
        KMSVNC_DEBUG("Failed to mmap cursor: %s\n", strerror(errno));
        return 1;
    }
    else
    {
        if (kmsvnc->drm->kms_cursor_buf_len < mmap_size)
        {
            if (kmsvnc->drm->kms_cursor_buf)
                free(kmsvnc->drm->kms_cursor_buf);
            kmsvnc->drm->kms_cursor_buf = malloc(mmap_size);
            if (!kmsvnc->drm->kms_cursor_buf) return 1;
            kmsvnc->drm->kms_cursor_buf_len = mmap_size;
        }
        memcpy(drm->kms_cursor_buf, drm->cursor_mapped, mmap_size);
        if (drm->cursor_mfb->pixel_format == KMSVNC_FOURCC_TO_INT('X', 'R', '3', '0') ||
            drm->cursor_mfb->pixel_format == KMSVNC_FOURCC_TO_INT('A', 'R', '3', '0'))
        {
            for (int i = 0; i < drm->cursor_mfb->width * drm->cursor_mfb->height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
                uint32_t pixdata = __builtin_bswap32(htonl(*((uint32_t*)(kmsvnc->drm->kms_cursor_buf + i))));
                kmsvnc->drm->kms_cursor_buf[i] = (pixdata & 0x3ff00000) >> 20 >> 2;
                kmsvnc->drm->kms_cursor_buf[i+1] = (pixdata & 0xffc00) >> 10 >> 2;
                kmsvnc->drm->kms_cursor_buf[i+2] = (pixdata & 0x3ff) >> 2;
                kmsvnc->drm->kms_cursor_buf[i+3] = (pixdata & 0xc0000000) >> 30 << 6;
            }
        }
        if (drm->cursor_mfb->pixel_format == KMSVNC_FOURCC_TO_INT('X', 'R', '2', '4') || 
            drm->cursor_mfb->pixel_format == KMSVNC_FOURCC_TO_INT('A', 'R', '2', '4'))
        {
            // bgra to rgba
            for (int i = 0; i < drm->cursor_mfb->width * drm->cursor_mfb->height * BYTES_PER_PIXEL; i += BYTES_PER_PIXEL) {
                uint32_t pixdata = htonl(*((uint32_t*)(kmsvnc->drm->kms_cursor_buf + i)));
                kmsvnc->drm->kms_cursor_buf[i+0] = (pixdata & 0x0000ff00) >> 8;
                kmsvnc->drm->kms_cursor_buf[i+2] = (pixdata & 0xff000000) >> 24;
            }
        }
        *width = drm->cursor_mfb->width;
        *height = drm->cursor_mfb->height;
        *data = drm->kms_cursor_buf;
    }
    return 0;
}

int drm_open() {
    struct kmsvnc_drm_data *drm = malloc(sizeof(struct kmsvnc_drm_data));
    if (!drm) KMSVNC_FATAL("memory allocation error at %s:%d\n", __FILE__, __LINE__);
    memset(drm, 0, sizeof(struct kmsvnc_drm_data));
    kmsvnc->drm = drm;

    drm->drm_fd = open(kmsvnc->card, O_RDONLY);
    if (drm->drm_fd < 0)
    {
        KMSVNC_FATAL("card %s open failed: %s\n", kmsvnc->card, strerror(errno));
    }
    if (!kmsvnc->screen_blank && drmIsMaster(drm->drm_fd)) {
        if (drmDropMaster(drm->drm_fd)) fprintf(stderr, "Failed to drop master");
    }
#ifndef DISABLE_KMSVNC_SCREEN_BLANK
    if (kmsvnc->screen_blank && !drmIsMaster(drm->drm_fd)) {
        drm->drm_master_fd = drm_get_master_fd();
        drm->drm_master_fd = drm->drm_master_fd > 0 ? drm->drm_master_fd : 0;
        if (kmsvnc->debug_enabled) {
            fprintf(stderr, "not master client, master fd %d\n", drm->drm_master_fd);
        }
    }
#endif

    drm->drm_ver = drmGetVersion(drm->drm_fd);
    printf("drm driver is %s\n", drm->drm_ver->name);

    int err = drmSetClientCap(drm->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (err < 0)
    {
        perror("Failed to set universal planes capability: primary planes will not be usable");
    }

    if (drm_refresh_planes(1)) return 1;

#ifndef DISABLE_KMSVNC_SCREEN_BLANK
    if (kmsvnc->screen_blank) {
        drm->gamma = malloc(sizeof(struct kmsvnc_drm_gamma_data));
        if (!drm->gamma) KMSVNC_FATAL("memory allocation error at %s:%d\n", __FILE__, __LINE__);
        memset(drm->gamma, 0, sizeof(struct kmsvnc_drm_gamma_data));
        drmModeCrtc *target_crtc = drmModeGetCrtc(drm->drm_fd, drm->plane->crtc_id);
        if (target_crtc) {
            drm->gamma->size = (uint32_t)target_crtc->gamma_size;
            drm->gamma->red = malloc(drm->gamma->size*sizeof(uint16_t)*3);
            if (!drm->gamma->size) {
                fprintf(stderr, "drm->gamma->size = %u, not setting gamma.\n", drm->gamma->size);
            }
            else if (!drm->gamma->red) {
                fprintf(stderr, "memory allocation error at %s:%d\n", __FILE__, __LINE__);
                fprintf(stderr, "not setting gamma.\n");
            }
            else {
                memset(drm->gamma->red, 0, drm->gamma->size*sizeof(uint16_t)*3);
                drm->gamma->green = drm->gamma->red + drm->gamma->size;
                drm->gamma->blue = drm->gamma->red + drm->gamma->size*2;
                if (kmsvnc->screen_blank_restore) {
                    int step = 0x10000 / drm->gamma->size;
                    for (int i = 0; i < drm->gamma->size; i++) {
                        drm->gamma->red[i] = drm->gamma->green[i] = drm->gamma->blue[i] = step * i;
                    }
                }
                else {
                    // legacy api, but weston also uses this, so whatever
                    drmModeCrtcGetGamma(drm->drm_fd, drm->plane->crtc_id, drm->gamma->size, drm->gamma->red, drm->gamma->green, drm->gamma->blue);
                }
                if (kmsvnc->debug_enabled) {
                    for (int i = 0; i < drm->gamma->size; i++) {
                        fprintf(stderr, "gamma: %05d %05hu %05hu %05hu\n", i, drm->gamma->red[i], drm->gamma->green[i], drm->gamma->blue[i]);
                    }
                }
                uint16_t *new_gamma_red = malloc(drm->gamma->size*sizeof(uint16_t)*3);
                if (!new_gamma_red) {
                    fprintf(stderr, "memory allocation error at %s:%d\n", __FILE__, __LINE__);
                    fprintf(stderr, "not setting gamma.\n");
                }
                else {
                    memset(new_gamma_red, 0, drm->gamma->size*sizeof(uint16_t)*3);
                    uint16_t *new_gamma_green = new_gamma_red + drm->gamma->size;
                    uint16_t *new_gamma_blue = new_gamma_red + drm->gamma->size*2;
                    if (drmModeCrtcSetGamma(drm->drm_master_fd ?: drm->drm_fd, drm->plane->crtc_id, drm->gamma->size, new_gamma_red, new_gamma_green, new_gamma_blue)) perror("Failed to set gamma");
                }
                if (new_gamma_red) {
                    free(new_gamma_red);
                    new_gamma_red = NULL;
                }
            }
        }
        else {
            fprintf(stderr, "Did not get a crtc structure, not setting gamma.\n");
        }
        if (target_crtc) {
            drmModeFreeCrtc(target_crtc);
            target_crtc = NULL;
        }
    }
#endif

    drm->mfb = drmModeGetFB2(drm->drm_fd, drm->plane->fb_id);
    if (!drm->mfb) {
        KMSVNC_FATAL("Failed to get framebuffer %u: %s\n", drm->plane->fb_id, strerror(errno));
    }
    drm->pixfmt_name = drmGetFormatName(drm->mfb->pixel_format);
    drm->mod_vendor = drmGetFormatModifierVendor(drm->mfb->modifier);
    drm->mod_name = drmGetFormatModifierName(drm->mfb->modifier);
    printf("Template framebuffer is %u: %ux%u fourcc:%u mod:%lu flags:%u\n", drm->mfb->fb_id, drm->mfb->width, drm->mfb->height, drm->mfb->pixel_format, drm->mfb->modifier, drm->mfb->flags);
    printf("handles %u %u %u %u\n", drm->mfb->handles[0], drm->mfb->handles[1], drm->mfb->handles[2], drm->mfb->handles[3]);
    printf("offsets %u %u %u %u\n", drm->mfb->offsets[0], drm->mfb->offsets[1], drm->mfb->offsets[2], drm->mfb->offsets[3]);
    printf("pitches %u %u %u %u\n", drm->mfb->pitches[0], drm->mfb->pitches[1], drm->mfb->pitches[2], drm->mfb->pitches[3]);
    printf("format %s, modifier %s:%s\n", drm->pixfmt_name, drm->mod_vendor, drm->mod_name);

    if (!drm->mfb->handles[0])
    {
        KMSVNC_FATAL("No handle set on framebuffer: maybe you need some additional capabilities?\n");
    }

    drm->mmap_fd = drm->drm_fd;
    drm->mmap_size = drm->mfb->width * drm->mfb->height * BYTES_PER_PIXEL;
    drm->funcs = malloc(sizeof(struct kmsvnc_drm_funcs));
    if (!drm->funcs) KMSVNC_FATAL("memory allocation error at %s:%d\n", __FILE__, __LINE__);
    drm->funcs->convert = convert_bgra_to_rgba;
    drm->funcs->sync_start = drm_sync_noop;
    drm->funcs->sync_end = drm_sync_noop;

    if (drm_vendors()) return 1;

    return 0;
}


static int drm_kmsbuf_prime() {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;

    int err = drmPrimeHandleToFD(drm->drm_fd, drm->mfb->handles[0], O_RDWR, &drm->prime_fd);
    if (err < 0 || drm->prime_fd < 0)
    {
        KMSVNC_FATAL("Failed to get PRIME fd from framebuffer handle\n");
    }
    drm->funcs->sync_start = &drm_sync_start;
    drm->funcs->sync_end = &drm_sync_end;
    drm->mmap_fd = drm->prime_fd;
    return 0;
}

static int drm_kmsbuf_prime_vaapi() {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;

    int err = drmPrimeHandleToFD(drm->drm_fd, drm->mfb->handles[0], O_RDWR, &drm->prime_fd);
    if (err < 0 || drm->prime_fd < 0)
    {
        KMSVNC_FATAL("Failed to get PRIME fd from framebuffer handle\n");
    }

    if (va_init()) return 1;

    drm->mmap_fd = drm->prime_fd;
    drm->skip_map = 1;
    return 0;
}

static int drm_kmsbuf_dumb() {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;

    struct drm_gem_flink flink;
    flink.handle = drm->mfb->handles[0];
    DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_GEM_FLINK, &flink);

    struct drm_gem_open open_arg;
    open_arg.name = flink.name;
    DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_GEM_OPEN, &open_arg);

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = open_arg.handle;
    DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);

    drm->mmap_size = open_arg.size;
    drm->mmap_offset = mreq.offset;
    return 0;
}

int drm_vendors() {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;

    char *driver_name;
    if (kmsvnc->force_driver) {
        printf("using %s instead of %s\n", kmsvnc->force_driver, drm->drm_ver->name);
        driver_name = kmsvnc->force_driver;
    }
    else {
        driver_name = drm->drm_ver->name;
    }

    if (strcmp(driver_name, "i915") == 0 || strcmp(driver_name, "amdgpu") == 0)
    {
        if (fourcc_mod_is_vendor(drm->mfb->modifier, INTEL)) {
            if (strstr(drm->mod_name, "CCS")) {
                printf("warn: intel with CCS modifier detected, please set INTEL_DEBUG=noccs\n");
            }
        };
        drm->funcs->convert = &convert_vaapi;
        if (drm_kmsbuf_prime_vaapi()) return 1;
    }
    else if (strcmp(driver_name, "nvidia-drm") == 0)
    {
        if (check_pixfmt_non_vaapi()) return 1;
        printf("warn: nvidia card detected. Currently only x-tiled framebuffer is supported. Performance may suffer.\n");
        if (drm->mfb->modifier != DRM_FORMAT_MOD_NONE && drm->mfb->modifier != DRM_FORMAT_MOD_LINEAR) {
            drm->funcs->convert = &convert_nvidia_x_tiled_kmsbuf;
        }
        if (drm_kmsbuf_dumb()) return 1;
    }
    else if (strcmp(driver_name, "vmwgfx") == 0 ||
             strcmp(driver_name, "vboxvideo") == 0 ||
             strcmp(driver_name, "virtio_gpu") == 0
    )
    {
        if (check_pixfmt_non_vaapi()) return 1;
        if (drm->mfb->modifier != DRM_FORMAT_MOD_NONE && drm->mfb->modifier != DRM_FORMAT_MOD_LINEAR) {
            printf("warn: modifier is not LINEAR, please create an issue with your modifier.\n");
        }
        // virgl does not work
        if (drm_kmsbuf_dumb()) return 1;
    }
    else if (strcmp(driver_name, "test-prime") == 0)
    {
        if (check_pixfmt_non_vaapi()) return 1;
        if (drm_kmsbuf_prime()) return 1;
    }
    else if (strcmp(driver_name, "test-map-dumb") == 0)
    {
        if (check_pixfmt_non_vaapi()) return 1;
        if (drm_kmsbuf_dumb()) return 1;
    }
    else if (strcmp(driver_name, "test-i915-gem") == 0)
    {
        if (check_pixfmt_non_vaapi()) return 1;
        struct drm_gem_flink flink;
        flink.handle = drm->mfb->handles[0];
        DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_GEM_FLINK, &flink);

        struct drm_gem_open open_arg;
        open_arg.name = flink.name;
        DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_GEM_OPEN, &open_arg);

        struct drm_i915_gem_mmap_gtt mmap_arg;
        mmap_arg.handle = open_arg.handle;
        DRM_IOCTL_MUST(drm->drm_fd, DRM_IOCTL_I915_GEM_MMAP_GTT, &mmap_arg);
        drm->mmap_size = open_arg.size;
        drm->mmap_offset = mmap_arg.offset;
    }
    else if (strcmp(driver_name, "test-i915-prime-xtiled") == 0)
    {
        if (check_pixfmt_non_vaapi()) return 1;
        drm->funcs->convert = &convert_intel_x_tiled_kmsbuf;
        if (drm_kmsbuf_prime()) return 1;
    }
    else
    {
        if (check_pixfmt_non_vaapi()) return 1;
        fprintf(stderr, "Untested drm driver, use at your own risk!\n");
        if (drm->mfb->modifier != DRM_FORMAT_MOD_NONE && drm->mfb->modifier != DRM_FORMAT_MOD_LINEAR) {
            printf("warn: modifier is not LINEAR, please create an issue with your driver and modifier.\n");
        }
        if (drm_kmsbuf_dumb()) return 1;
    }

    if (!drm->skip_map && !drm->mapped)
    {
        printf("mapping with size = %lu, offset = %ld, fd = %d\n", drm->mmap_size, drm->mmap_offset, drm->mmap_fd);
        drm->mapped = mmap(NULL, drm->mmap_size, PROT_READ, MAP_SHARED, drm->mmap_fd, drm->mmap_offset);
        if (drm->mapped == MAP_FAILED)
        {
            KMSVNC_FATAL("Failed to mmap: %s\n", strerror(errno));
        }
    }

    return 0;
}

int drm_get_cursor_position(int *x, int *y) {
    struct kmsvnc_drm_data *drm = kmsvnc->drm;
    if (drm->cursor_plane) {
        uint32_t plane_id = drm->cursor_plane->plane_id;
        drmModePlane *updated = drmModeGetPlane(drm->drm_fd, plane_id);
        if (updated && updated->fb_id && updated->crtc_id) {
            *x = (int)updated->crtc_x;
            *y = (int)updated->crtc_y;
            drmModeFreePlane(updated);
            return 0;
            drmModeFreePlane(updated);
        }
        else if (updated) {
            drmModeFreePlane(updated);
        }
    }
    // Fallback: scan other cards for cursor position
    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (!strcmp(path, kmsvnc->card)) continue;
        if (access(path, F_OK)) continue;
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        drmModePlaneRes *res = drmModeGetPlaneResources(fd);
        if (!res) { close(fd); continue; }
        for (int j = 0; j < res->count_planes; j++) {
            drmModePlane *p = drmModeGetPlane(fd, res->planes[j]);
            if (!p || p->fb_id == 0) { if(p) drmModeFreePlane(p); continue; }
            uint64_t t = 114514, crtc_x_val = 0, crtc_y_val = 0;
            drmModeObjectPropertiesPtr pr = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            if (pr) {
                for (int k = 0; k < pr->count_props; k++) {
                    drmModePropertyPtr prop = drmModeGetProperty(fd, pr->props[k]);
                    if (!prop) continue;
                    if (!strcmp(prop->name, "type")) t = pr->prop_values[k];
                    if (!strcmp(prop->name, "CRTC_X")) crtc_x_val = pr->prop_values[k];
                    if (!strcmp(prop->name, "CRTC_Y")) crtc_y_val = pr->prop_values[k];
                    drmModeFreeProperty(prop);
                }
                drmModeFreeObjectProperties(pr);
            }
            if (t == DRM_PLANE_TYPE_CURSOR) {
                *x = crtc_x_val ? (int)crtc_x_val : (int)p->crtc_x;
                *y = crtc_y_val ? (int)crtc_y_val : (int)p->crtc_y;
                drmModeFreePlane(p);
                drmModeFreePlaneResources(res);
                close(fd);
                KMSVNC_DEBUG("cursor position from fallback %s: %d,%d\n", path, *x, *y);
                return 0;
            }
            drmModeFreePlane(p);
        }
        drmModeFreePlaneResources(res);
        close(fd);
    }
    *x = 0; *y = 0;
    return 1;
}

void drm_composite_cursor_into_fb(char *fb, int fb_w, int fb_h, char *cursor, int c_w, int c_h, int c_x, int c_y) {
    // Clip cursor to screen bounds
    int start_x = c_x > 0 ? c_x : 0;
    int start_y = c_y > 0 ? c_y : 0;
    int end_x = (c_x + c_w) < fb_w ? (c_x + c_w) : fb_w;
    int end_y = (c_y + c_h) < fb_h ? (c_y + c_h) : fb_h;
    if (start_x >= end_x || start_y >= end_y) return;
    for (int y = start_y; y < end_y; y++) {
        for (int x = start_x; x < end_x; x++) {
            int ci = ((x - c_x) + (y - c_y) * c_w) * 4;
            int fi = (x + y * fb_w) * 4;
            uint8_t ca = (uint8_t)cursor[ci + 3];
            if (ca == 0) continue;
            if (ca == 255) {
                fb[fi + 0] = cursor[ci + 0];
                fb[fi + 1] = cursor[ci + 1];
                fb[fi + 2] = cursor[ci + 2];
            } else {
                fb[fi + 0] = (uint8_t)(((uint16_t)cursor[ci + 0] * ca + (uint16_t)fb[fi + 0] * (255 - ca)) / 255);
                fb[fi + 1] = (uint8_t)(((uint16_t)cursor[ci + 1] * ca + (uint16_t)fb[fi + 1] * (255 - ca)) / 255);
                fb[fi + 2] = (uint8_t)(((uint16_t)cursor[ci + 2] * ca + (uint16_t)fb[fi + 2] * (255 - ca)) / 255);
            }
        }
    }
}

static int capture_from_card(const char *card_path, char **data, int *width, int *height, int *crtc_x, int *crtc_y) {
    int fd = open(card_path, O_RDONLY);
    if (fd < 0) return 1;

    int err = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (err < 0) { close(fd); return 1; }

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) { close(fd); return 1; }

    int ret = 1;
    for (int i = 0; i < plane_res->count_planes; i++) {
        drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);
        if (!plane || plane->fb_id == 0 || plane->crtc_id == 0) {
            if (plane) drmModeFreePlane(plane);
            continue;
        }
        uint64_t plane_type = 114514;
        drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (int j = 0; j < props->count_props; j++) {
                drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[j]);
                if (prop && strcmp(prop->name, "type") == 0) {
                    plane_type = props->prop_values[j];
                }
                if (prop) drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        if (plane_type != DRM_PLANE_TYPE_CURSOR) {
            drmModeFreePlane(plane);
            continue;
        }
        drmModeFB2 *mfb = drmModeGetFB2(fd, plane->fb_id);
        if (!mfb) {
            drmModeFreePlane(plane);
            continue;
        }
        if (mfb->pixel_format != KMSVNC_FOURCC_TO_INT('A', 'R', '2', '4') &&
            mfb->pixel_format != KMSVNC_FOURCC_TO_INT('A', 'R', '3', '0') &&
            mfb->pixel_format != KMSVNC_FOURCC_TO_INT('X', 'R', '2', '4')) {
            drmModeFreeFB2(mfb);
            drmModeFreePlane(plane);
            continue;
        }
        // mmap cursor buffer
        struct drm_gem_flink flink = {.handle = mfb->handles[0]};
        if (drmIoctl(fd, DRM_IOCTL_GEM_FLINK, &flink)) {
            drmModeFreeFB2(mfb); drmModeFreePlane(plane); continue;
        }
        struct drm_gem_open open_arg = {.name = flink.name};
        if (drmIoctl(fd, DRM_IOCTL_GEM_OPEN, &open_arg)) {
            drmModeFreeFB2(mfb); drmModeFreePlane(plane); continue;
        }
        struct drm_mode_map_dumb mreq = {.handle = open_arg.handle};
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
            drmModeFreeFB2(mfb); drmModeFreePlane(plane); continue;
        }
        size_t size = mfb->width * mfb->height * BYTES_PER_PIXEL;
        size_t mmap_size = open_arg.size;
        if (mmap_size < size) {
            drmModeFreeFB2(mfb); drmModeFreePlane(plane); continue;
        }
        char *mapped = mmap(NULL, mmap_size, PROT_READ, MAP_SHARED, fd, mreq.offset);
        if (mapped == MAP_FAILED) {
            drmModeFreeFB2(mfb); drmModeFreePlane(plane); continue;
        }
        size_t buf_size = size > mmap_size ? mmap_size : size;
        char *buf = malloc(buf_size);
        if (!buf) { munmap(mapped, mmap_size); drmModeFreeFB2(mfb); drmModeFreePlane(plane); continue; }
        memcpy(buf, mapped, buf_size);
        if (mfb->pixel_format == KMSVNC_FOURCC_TO_INT('X', 'R', '2', '4') ||
            mfb->pixel_format == KMSVNC_FOURCC_TO_INT('A', 'R', '2', '4')) {
            for (int p = 0; p < mfb->width * mfb->height; p++) {
                int pi = p * 4;
                uint32_t pix = htonl(*((uint32_t*)(buf + pi)));
                buf[pi+0] = (pix & 0x0000ff00) >> 8;
                buf[pi+2] = (pix & 0xff000000) >> 24;
            }
        } else if (mfb->pixel_format == KMSVNC_FOURCC_TO_INT('A', 'R', '3', '0')) {
            for (int p = 0; p < mfb->width * mfb->height; p++) {
                int pi = p * 4;
                uint32_t pix = __builtin_bswap32(htonl(*((uint32_t*)(buf + pi))));
                buf[pi+0] = (pix & 0x3ff00000) >> 20 >> 2;
                buf[pi+1] = (pix & 0xffc00) >> 10 >> 2;
                buf[pi+2] = (pix & 0x3ff) >> 2;
                buf[pi+3] = (pix & 0xc0000000) >> 30 << 6;
            }
        }
        *data = buf;
        *width = mfb->width;
        *height = mfb->height;
        *crtc_x = (int)plane->crtc_x;
        *crtc_y = (int)plane->crtc_y;
        ret = 0;
        munmap(mapped, mmap_size);
        drmModeFreeFB2(mfb);
        drmModeFreePlane(plane);
        break;
    }
    drmModeFreePlaneResources(plane_res);
    close(fd);
    return ret;
}

int drm_capture_cursor_any(char **data, int *width, int *height, int *crtc_x, int *crtc_y) {
    // First try the primary card
    if (!drm_dump_cursor_plane(data, width, height) && *data) {
        return drm_get_cursor_position(crtc_x, crtc_y);
    }
    // Fallback: scan other DRM cards
    for (int i = 0; i < 10; i++) {
        char path[32];
        // Skip the primary card
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        if (!strcmp(path, kmsvnc->card)) continue;
        if (access(path, F_OK)) continue;
        if (!capture_from_card(path, data, width, height, crtc_x, crtc_y)) {
            KMSVNC_DEBUG("Cursor captured from fallback card %s\n", path);
            return 0;
        }
    }
    return 1;
}
