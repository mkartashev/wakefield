#include <weston/weston.h>
#include <libweston/weston-log.h>

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

void wakefield_get_pixel_color(struct wl_client *client,
                               struct wl_resource *resource,
                               int32_t x,
                               int32_t y)
{
    struct wakefield *wakefield = wl_resource_get_user_data(resource);

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: get_pixel_color at (%d, %d)\n", x, y);

    uint32_t rgb = 42;

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: color is 0x%08x\n", rgb);

    wakefield_send_pixel_color(resource, x, y, rgb);
}

void wakefield_get_surface_location(struct wl_client *client,
                                    struct wl_resource *resource,
                                    struct wl_resource *surface_resource)
{
    // See also weston-test.c`move_surface() and the corresponding protocol

    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct weston_surface *surface = wl_resource_get_user_data(surface_resource);
    struct weston_view *view = container_of(surface->views.next, struct weston_view, surface_link);

    if (!view)
        return;

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: get_location\n");

    wakefield_send_surface_location(resource, surface_resource, view->geometry.x, view->geometry.y);
}

void wakefield_move_surface(struct wl_client *client,
                            struct wl_resource *resource,
                            struct wl_resource *surface_resource,
                            int32_t x,
                            int32_t y)
{
    struct wakefield *wakefield = wl_resource_get_user_data(resource);
    struct weston_surface *surface = wl_resource_get_user_data(surface_resource);
    struct weston_view *view = container_of(surface->views.next, struct weston_view, surface_link);

    if (!view)
        return;

    weston_view_set_position(view, x, y);
    weston_view_update_transform(view);

    weston_log_scope_printf(wakefield->log, "WAKEFIELD: move_surface to (%d, %d)\n", x, y);
}

static const struct wakefield_interface wakefield_implementation = {
        .get_surface_location = wakefield_get_surface_location,
        .move_surface = wakefield_move_surface,
        .get_pixel_color = wakefield_get_pixel_color
};

static void
wakefield_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct wakefield *wakefield = data;

    struct wl_resource *resource = wl_resource_create(client, &wakefield_interface, 1, id);
    if (resource)
        wl_resource_set_implementation(resource, &wakefield_implementation, wakefield, NULL);

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