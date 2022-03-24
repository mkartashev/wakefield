#include <weston/weston.h>
#include <libweston/weston-log.h>

#include <pixman.h>
#include <assert.h>

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

    const int32_t x = view->geometry.x;
    const int32_t y = view->geometry.y;
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
static void
wakefield_capture_create(struct wl_client *client,
                         struct wl_resource *resource,
                         struct wl_resource *buffer_resource,
                         int32_t x,
                         int32_t y)
{
    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct wl_shm_buffer *shm_buffer = wl_shm_buffer_get(buffer_resource);

    if (!shm_buffer) {
        weston_log_scope_printf(wakefield->log, "WAKEFIELD: buffer for image capture not from wl_shm\n");
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_INTERNAL);
        return;
    }

    const uint32_t buffer_format = wl_shm_buffer_get_format(shm_buffer);
    if (buffer_format != WL_SHM_FORMAT_ARGB8888
        && buffer_format != WL_SHM_FORMAT_XRGB8888) {
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: buffer for image capture has unsupported format %d, "
                                "check codes in enum 'format' in wayland.xml\n",
                                buffer_format);
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_FORMAT);
        return;
    }

    const wl_fixed_t xf = wl_fixed_from_int(x);
    const wl_fixed_t yf = wl_fixed_from_int(y);
    wl_fixed_t view_xf;
    wl_fixed_t view_yf;
    struct weston_view *view = weston_compositor_pick_view(wakefield->compositor, xf, yf, &view_xf, &view_yf);
    if (view == NULL) {
        weston_log_scope_printf(wakefield->log,
                                "WAKEFIELD: capture location (%d, %d) doesn't map to any view\n", x, y);
        wakefield_send_capture_ready(resource, buffer_resource, WAKEFIELD_ERROR_INVALID_COORDINATES);
        return;
    }

    pixman_format_code_t buffer_format_pixman = wl_shm_format_to_pixman(buffer_format);
    const int32_t width = wl_shm_buffer_get_width(shm_buffer);
    const int32_t height = wl_shm_buffer_get_height(shm_buffer);
    weston_log_scope_printf(wakefield->log,
                            "WAKEFIELD: about to send screen capture at (%d, %d) of size %dx%d, format %s\n",
                            x, y,
                            width, height,
                            buffer_format_pixman == PIXMAN_a8r8g8b8 ? "ARGB8888" : "XRGB8888");

    wl_shm_buffer_begin_access(shm_buffer);
    {
        uint32_t *data = wl_shm_buffer_get_data(shm_buffer);
        wakefield->compositor->renderer->read_pixels(view->output,
                                                     buffer_format_pixman, // TODO: may not work with all renderers, check screenshooter_frame_notify() in libweston
                                                     data,
                                                     x, y, width, height);
    }
    wl_shm_buffer_end_access(shm_buffer);

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
