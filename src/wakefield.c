#include <weston/weston.h>
#include <libweston/weston-log.h>

#include <pixman.h>
#include <assert.h>
#include <string.h>

#include "wakefield-server-protocol.h"

#ifndef container_of
#define container_of(ptr, type, member) ({                              \
        const __typeof__( ((type *)0)->member ) *__mptr = (ptr);        \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#endif

struct wakefield {
    struct weston_compositor *compositor;
    struct wl_listener destroy_listener;

    struct weston_log_scope *log;
};

static void
wakefield_get_pixel_color(struct wl_client *client,
                          struct wl_resource *resource,
                          int32_t x,
                          int32_t y)
{
    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct weston_compositor *compositor = wakefield->compositor;

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: get_pixel_color at (%d, %d)\n", x, y);

    const unsigned int byte_per_pixel = (PIXMAN_FORMAT_BPP(compositor->read_format) / 8);
    uint32_t pixel = 0;
    if (byte_per_pixel > sizeof(pixel)) {
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: compositor pixel format (%d) exceeds allocated storage (%d > %ld)\n",
                                compositor->read_format,
                                byte_per_pixel,
                                sizeof(pixel));
        wakefield_send_pixel_color(resource, x, y, 0, WAKEFIELD_ERROR_FORMAT);
        return;
    }

    const wl_fixed_t xf = wl_fixed_from_int(x);
    const wl_fixed_t yf = wl_fixed_from_int(y);
    wl_fixed_t view_xf;
    wl_fixed_t view_yf;
    struct weston_view *view = weston_compositor_pick_view(compositor, xf, yf, &view_xf, &view_yf);
    if (view == NULL) {
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: pixel location (%d, %d) doesn't map to any view\n", x, y);
        wakefield_send_pixel_color(resource, x, y, 0, WAKEFIELD_ERROR_INVALID_COORDINATES);
        return;
    }

    struct weston_output *output = view->output;
    compositor->renderer->read_pixels(output,
                                      compositor->read_format, &pixel,
                                      x, y, 1, 1);

    uint32_t rgb = 0;
    switch (compositor->read_format) {
        case PIXMAN_a8r8g8b8:
        case PIXMAN_x8r8g8b8:
        case PIXMAN_r8g8b8:
            rgb = pixel & 0x00ffffffu;
            break;

        default:
            weston_log_scope_printf(wakefield->log,
                                    "WAKEFIELD: compositor pixel format %d (see pixman.h) not supported\n",
                                    compositor->read_format);
            wakefield_send_pixel_color(resource, x, y, 0, WAKEFIELD_ERROR_FORMAT);
            return;
    }
    weston_log_scope_printf(wakefield->log, "WAKEFIELD: color is 0x%08x\n", rgb);

    wakefield_send_pixel_color(resource, x, y, rgb, WAKEFIELD_ERROR_NO_ERROR);
}

static void
wakefield_get_surface_location(struct wl_client *client,
                               struct wl_resource *resource,
                               struct wl_resource *surface_resource)
{
    // See also weston-test.c`move_surface() and the corresponding protocol

    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct weston_surface *surface = wl_resource_get_user_data(surface_resource);
    struct weston_view *view = container_of(surface->views.next, struct weston_view, surface_link);

    if (!view) {
        weston_log_scope_printf(wakefield->log, "WAKEFIELD: get_location error\n");
        wakefield_send_surface_location(resource, surface_resource, 0, 0,
                                        WAKEFIELD_ERROR_INTERNAL);
        return;
    }

    float fx;
    float fy;
    weston_view_to_global_float(view, 0, 0, &fx, &fy);
    const int32_t x = (int32_t)fx;
    const int32_t y = (int32_t)fy;
    weston_log_scope_printf(wakefield->log, "WAKEFIELD: get_location: %d, %d\n", x, y);

    wakefield_send_surface_location(resource, surface_resource, x, y,
                                    WAKEFIELD_ERROR_NO_ERROR);
}

static void
wakefield_move_surface(struct wl_client *client,
                       struct wl_resource *resource,
                       struct wl_resource *surface_resource,
                       int32_t x,
                       int32_t y)
{
    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct weston_surface *surface = wl_resource_get_user_data(surface_resource);
    struct weston_view *view = container_of(surface->views.next, struct weston_view, surface_link);

    if (!view) {
        weston_log_scope_printf(wakefield->log, "WAKEFIELD: move_surface error\n");
        return;
    }

    weston_view_set_position(view, (float)x, (float)y);
    weston_view_update_transform(view);

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: move_surface to (%d, %d)\n", x, y);
}

static pixman_format_code_t
wl_shm_format_to_pixman(uint32_t wl_shm_format)
{
    pixman_format_code_t rc;

    switch (wl_shm_format) {
        case WL_SHM_FORMAT_ARGB8888:
            rc = PIXMAN_a8r8g8b8;
            break;
        case WL_SHM_FORMAT_XRGB8888:
            rc = PIXMAN_x8r8g8b8;
            break;
        default:
            assert(false);
    }

    return rc;
}

static uint64_t
area_in_pixels(pixman_region32_t *region)
{
    pixman_box32_t *e = pixman_region32_extents(region);
    assert (e->x2 >= e->x1);
    assert (e->y2 >= e->y1);

    return ((uint64_t)(e->x2 - e->x1))*(e->y2 - e->y1);
}

static uint64_t
get_largest_area_in_one_output(struct weston_compositor *compositor, pixman_region32_t *region)
{
    uint64_t area = 0; // in pixels

    pixman_region32_t region_in_output;
    pixman_region32_init(&region_in_output);

    struct weston_output *output;
    wl_list_for_each(output, &compositor->output_list, link) {
        if (output->destroying)
            continue;

        pixman_region32_intersect(&region_in_output, region, &output->region);
        if (pixman_region32_not_empty(&region_in_output)) {
            const uint64_t this_area = area_in_pixels(&region_in_output);
            if (this_area > area) {
                area = this_area;
            }
        }
    }

    pixman_region32_fini(&region_in_output);

    return area;
}

static void
clear_buffer(struct wl_shm_buffer *buffer)
{
    const int32_t  width            = wl_shm_buffer_get_width(buffer);
    const int32_t  height           = wl_shm_buffer_get_height(buffer);
    const size_t   bpp              = 4;
    const size_t   buffer_byte_size = width*height*bpp;

    wl_shm_buffer_begin_access(buffer);
    {
        uint32_t *data = wl_shm_buffer_get_data(buffer);
        memset(data, 0, buffer_byte_size);
    }
    wl_shm_buffer_end_access(buffer);
}

static void
copy_pixels_to_shm_buffer(struct wl_shm_buffer *buffer, uint32_t *data, int32_t target_x, int32_t target_y, int32_t width, int32_t height)
{
    assert (target_x >= 0 && target_y >= 0);
    assert (data);

    const int32_t buffer_width = wl_shm_buffer_get_width(buffer);

    wl_shm_buffer_begin_access(buffer);
    {
        uint32_t * const buffer_data = wl_shm_buffer_get_data(buffer);
        assert (buffer_data);

        for (int32_t y = 0; y < height; y++) {
            uint32_t * const src_line = &data[y*width];
            uint32_t * const dst_line = &buffer_data[(target_y + y)*buffer_width];
            for (int32_t x = 0; x < width; x++) {
                dst_line[target_x + x] = src_line[x];
            }
        }
    }
    wl_shm_buffer_end_access(buffer);
}

static void
wakefield_capture_create(struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *buffer_resource,
                         int32_t x,
                         int32_t y)
{
    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct wl_shm_buffer *buffer = wl_shm_buffer_get(buffer_resource);

    if (!buffer) {
        weston_log_scope_printf(wakefield->log, "WAKEFIELD: buffer for image capture not from wl_shm\n");
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_INTERNAL);
        return;
    }

    const uint32_t buffer_format = wl_shm_buffer_get_format(buffer);
    if (buffer_format != WL_SHM_FORMAT_ARGB8888
        && buffer_format != WL_SHM_FORMAT_XRGB8888) {
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: buffer for image capture has unsupported format %d, "
                                "check codes in enum 'format' in wayland.xml\n",
                                buffer_format);
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_FORMAT);
        return;
    }

    clear_buffer(buffer); // in case some outputs disappear mid-flight

    const int32_t width  = wl_shm_buffer_get_width(buffer);
    const int32_t height = wl_shm_buffer_get_height(buffer);

    pixman_region32_t region_global;
    pixman_region32_t region_in_output;

    pixman_region32_init_rect(&region_global, x, y, width, height);
    pixman_region32_init(&region_in_output);

    const uint64_t largest_capture_area = get_largest_area_in_one_output(wakefield->compositor, &region_global);
    if (largest_capture_area == 0) {
        // All outputs might've just disappeared
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: captured area size on all outputs is zero.\n");
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_NO_ERROR);
        return;
    }

    // TODO: bypass this if fits on one output entirely
    const size_t bpp = 4; // byte-per-pixel
    void *per_output_buffer = malloc(largest_capture_area*bpp);
    if (per_output_buffer == NULL) {
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: failed to allocate %ld bytes for temporary capture buffer.\n", largest_capture_area);
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_OUT_OF_MEMORY);
        return;
    }

    const pixman_format_code_t buffer_format_pixman = wl_shm_format_to_pixman(buffer_format);
    struct weston_output *output;
    wl_list_for_each(output, &wakefield->compositor->output_list, link) {
        if (output->destroying)
            continue;

        pixman_region32_intersect(&region_in_output, &region_global, &output->region);
        if (pixman_region32_not_empty(&region_in_output)) {
            pixman_box32_t *e = pixman_region32_extents(&region_in_output);

            const int32_t region_x_in_global = e->x1;
            const int32_t region_y_in_global = e->y1;
            const int32_t width_in_output    = e->x2 - e->x1;
            const int32_t height_in_output   = e->y2 - e->y1;
            weston_log_scope_printf(wakefield->log, "WAKEFIELD: output '%s' has a chunk of the image at (%d, %d) sized (%d, %d)\n",
                                    output->name,
                                    e->x1, e->y1,
                                    width_in_output, height_in_output);

            // Better, but not available in the current libweston:
            // weston_output_region_from_global(output, &region_in_output);

            // Now convert region_in_output from global to output-local coordinates.
            pixman_region32_translate(&region_in_output, -output->x, -output->y);

            pixman_box32_t *e_in_output = pixman_region32_extents(&region_in_output);
            const int32_t x_in_output = e_in_output->x1;
            const int32_t y_in_output = e_in_output->y1;
            weston_log_scope_printf(wakefield->log, "WAKEFIELD: ... and in output-local coordinates: (%d, %d)\n",
                                    x_in_output, y_in_output);

            weston_log_scope_printf(wakefield->log,
                                    "WAKEFIELD: grabbing pixels at (%d, %d) of size %dx%d, format %s\n",
                                    x_in_output, y_in_output,
                                    width_in_output, height_in_output,
                                    buffer_format_pixman == PIXMAN_a8r8g8b8 ? "ARGB8888" : "XRGB8888");

            wakefield->compositor->renderer->read_pixels(output,
                                                         buffer_format_pixman, // TODO: may not work with all renderers, check screenshooter_frame_notify() in libweston
                                                         per_output_buffer,
                                                         x_in_output, y_in_output,
                                                         width_in_output, height_in_output);

            copy_pixels_to_shm_buffer(buffer, per_output_buffer,
                                      region_x_in_global - x, region_y_in_global - y,
                                      width_in_output, height_in_output); // TODO: coordinates
        }

        pixman_region32_fini(&region_in_output);
        pixman_region32_fini(&region_global);
    }

    free(per_output_buffer);

    wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_NO_ERROR);
}

static const struct wakefield_interface wakefield_implementation = {
        .get_surface_location = wakefield_get_surface_location,
        .move_surface = wakefield_move_surface,
        .get_pixel_color = wakefield_get_pixel_color,
        .capture_create = wakefield_capture_create
};

static void
wakefield_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wakefield *wakefield = data;

    struct wl_resource *resource = wl_resource_create(client, &wakefield_interface, 1, id);
    if (resource) {
        wl_resource_set_implementation(resource, &wakefield_implementation, wakefield, NULL);
    }

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: bind\n");
}

static void
wakefield_destroy(struct wl_listener *listener, void *data)
{
    struct wakefield *wakefield = container_of(listener, struct wakefield, destroy_listener);

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: destroy\n");

    wl_list_remove(&wakefield->destroy_listener.link);

    weston_log_scope_destroy(wakefield->log);
    free(wakefield);
}

WL_EXPORT int
wet_module_init(struct weston_compositor *wc, int *argc, char *argv[])
{
    struct wakefield *wakefield = zalloc(sizeof(struct wakefield));
    if (wakefield == NULL)
        return -1;

    if (!weston_compositor_add_destroy_listener_once(wc, &wakefield->destroy_listener,
                                                     wakefield_destroy)) {
        free(wakefield);
        return 0;
    }


    wakefield->compositor = wc;
    // Log scope; add this to weston option list to subscribe: `--logger-scopes=wakefield`
    // See https://wayland.pages.freedesktop.org/weston/toc/libweston/log.html for more info.
    wakefield->log = weston_compositor_add_log_scope(wc, "wakefield",
                                                     "wakefield plugin own actions",
                                                     NULL, NULL, NULL);

    if (wl_global_create(wc->wl_display, &wakefield_interface,
                         1, wakefield, wakefield_bind) == NULL) {
        wl_list_remove(&wakefield->destroy_listener.link);
        return -1;
    }

    return 0;
}
