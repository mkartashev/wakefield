// Based on https://wayland-book.com/xdg-shell-basics/example-code.html

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>

#include <wayland-client.h>
#include <linux/input-event-codes.h>

#include "xdg-shell-client-protocol.h"
#include "wakefield-client-protocol.h"

/* Shared memory support code */
static void randname(char *buf) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

static int create_shm_file() {
    int retries = 100;
    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static int allocate_shm_file(size_t size) {
    int fd = create_shm_file();
    if (fd < 0)
        return -1;
    int ret;
    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

enum pointer_event_mask {
    POINTER_EVENT_ENTER = 1 << 0,
    POINTER_EVENT_LEAVE = 1 << 1,
    POINTER_EVENT_MOTION = 1 << 2,
    POINTER_EVENT_BUTTON = 1 << 3,
    POINTER_EVENT_AXIS = 1 << 4,
    POINTER_EVENT_AXIS_SOURCE = 1 << 5,
    POINTER_EVENT_AXIS_STOP = 1 << 6,
    POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
};

struct pointer_event {
    uint32_t event_mask;
    wl_fixed_t surface_x;
    wl_fixed_t surface_y;
    uint32_t button;
    uint32_t state;
    uint32_t time;
    uint32_t serial;
};

struct client_state {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wakefield *wakefield;
    struct wl_seat *wl_seat;

    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_pointer *wl_pointer;

    struct pointer_event pointer_event; // evens collected here until wl_pointer_frame

    int32_t surface_x; // absolute x coordinate of wl_surface from above
    int32_t surface_y; // absolute y coordinate of wl_surface from above

    int32_t mouse_x;
    int32_t mouse_y;
};

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
        .release = wl_buffer_release,
};

static void paint_to(uint32_t *data, int width, int height) {
    const uint32_t GREEN = 0xFF00FF00;
    const uint32_t BLUE  = 0xFF0000FF;
    const uint32_t RED   = 0xFFFF0000;
    const uint32_t GREY  = 0xFF606060;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (x < width / 2 && y < height / 2) {
                data[y * width + x] = GREEN;
            } else if (x >= width / 2 && y < height / 2) {
                data[y * width + x] = BLUE;
            } else if (x < width / 2) {
                data[y * width + x] = RED;
            } else {
                data[y * width + x] = GREY;
            }
        }
    }
}

static struct wl_buffer *draw_frame(struct client_state *state) {
    const int width = 640, height = 480;
    int stride = width * 4;
    int size = stride * height;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
                                                         width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    paint_to(data, width, height);
    munmap(data, size);
    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}

static void xdg_surface_configure(void *data,
                                  struct xdg_surface *xdg_surface, uint32_t serial) {
    struct client_state *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(state);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
        .configure = xdg_surface_configure
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
        .ping = xdg_wm_base_ping,
};

static void wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
    printf("INFO: seat name: %s\n", name);
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
                              wl_fixed_t surface_x, wl_fixed_t surface_y) {
    struct client_state * const client_state = data;
    client_state->pointer_event.event_mask |= POINTER_EVENT_MOTION;
    client_state->pointer_event.time = time;
    client_state->pointer_event.surface_x = surface_x;
    client_state->pointer_event.surface_y = surface_y;
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                              uint32_t time, uint32_t button, uint32_t state) {
    struct client_state * const client_state = data;
    client_state->pointer_event.event_mask |= POINTER_EVENT_BUTTON;
    client_state->pointer_event.time = time;
    client_state->pointer_event.serial = serial;
    client_state->pointer_event.button = button;
    client_state->pointer_event.state = state;
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
    struct client_state *client_state = data;
    struct pointer_event * const event = &client_state->pointer_event;

    if (event->event_mask & POINTER_EVENT_MOTION) {
        client_state->mouse_x = wl_fixed_to_int(event->surface_x);
        client_state->mouse_y = wl_fixed_to_int(event->surface_y);
        printf("move local coords (%d, %d)\n", client_state->mouse_x, client_state->mouse_y);
    }

    int32_t abs_x = client_state->surface_x + client_state->mouse_x;
    int32_t abs_y = client_state->surface_y + client_state->mouse_y;
    printf("abs (%d, %d)\n", abs_x, abs_y);

    if ((event->event_mask & POINTER_EVENT_BUTTON)
        && event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_LEFT) {
        wakefield_get_pixel_color(client_state->wakefield, abs_x, abs_y);
    }

    if ((event->event_mask & POINTER_EVENT_BUTTON)
        && event->state == WL_POINTER_BUTTON_STATE_PRESSED && event->button == BTN_RIGHT) {
        wakefield_move_surface(client_state->wakefield, client_state->wl_surface, abs_x, abs_y);
    }

    memset(event, 0, sizeof(*event));
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                                     uint32_t axis, int32_t discrete) {
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                                 uint32_t time, uint32_t axis) {
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                                   uint32_t axis_source) {
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
                uint32_t axis, wl_fixed_t value) {
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y) {
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                             uint32_t serial, struct wl_surface *surface) {
}
static const struct wl_pointer_listener wl_pointer_listener = {
        .enter = wl_pointer_enter,
        .leave = wl_pointer_leave,
        .motion = wl_pointer_motion,
        .button = wl_pointer_button,
        .axis = wl_pointer_axis,
        .frame = wl_pointer_frame,
        .axis_source = wl_pointer_axis_source,
        .axis_stop = wl_pointer_axis_stop,
        .axis_discrete = wl_pointer_axis_discrete
};

static void wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
    struct client_state * const state = data;

    const bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    if (have_pointer && state->wl_pointer == NULL) {
        state->wl_pointer = wl_seat_get_pointer(state->wl_seat);
        wl_pointer_add_listener(state->wl_pointer,
                                &wl_pointer_listener, state);
    } else if (!have_pointer && state->wl_pointer != NULL) {
        wl_pointer_release(state->wl_pointer);
        state->wl_pointer = NULL;
    }
}

static const struct wl_seat_listener wl_seat_listener = {
        .capabilities = wl_seat_capabilities,
        .name = wl_seat_name
};

static void wakefield_surface_location(void *data,
                         struct wakefield *wakefield,
                         struct wl_surface *surface,
                         int32_t x,
                         int32_t y,
                         int32_t error_code) {
    struct client_state *client_state = data;
    client_state->surface_x = x;
    client_state->surface_y = y;

    if (error_code) {
        printf("surface location: ERROR, code %d\n", error_code);
    } else {
        printf("surface location: %d, %d\n", x, y);
    }
}

static void wakefield_pixel_color(void *data,
                    struct wakefield *wakefield,
                    int32_t x,
                    int32_t y,
                    uint32_t rgb,
                    int32_t error_code) {
    if (error_code) {
        printf("pixel at (%d, %d): ERROR, code %d\n", x, y, error_code);
    } else {
        printf("pixel at (%d, %d) has color 0x%08x\n", x, y, rgb);
    }
}

static const struct wakefield_listener wakefield_listener = {
        .surface_location = wakefield_surface_location,
        .pixel_color = wakefield_pixel_color
};

static void registry_global(void *data, struct wl_registry *wl_registry,
                            uint32_t name, const char *interface, uint32_t version) {
    struct client_state *state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = wl_registry_bind(
                wl_registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = wl_registry_bind(
                wl_registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(
                wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base,
                                 &xdg_wm_base_listener, state);
    } else if (strcmp(interface, wakefield_interface.name) == 0) {
        state->wakefield = wl_registry_bind(wl_registry, name, &wakefield_interface, 1);
        wakefield_add_listener(state->wakefield, &wakefield_listener, state);
    } else if (strcmp(interface, wl_seat_interface.name) ==0) {
        state->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(state->wl_seat, &wl_seat_listener, state);
    }
}

static void registry_global_remove(void *data,
                                   struct wl_registry *wl_registry, uint32_t name) {
    /* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
        .global = registry_global,
        .global_remove = registry_global_remove,
};

int main(int argc, char *argv[]) {
    struct client_state state = {0};

    state.wl_display = wl_display_connect(NULL);
    if (!state.wl_display) {
        fprintf(stderr, "Can't open WAYLAND_DISPLAY. Shutting down.\n");
        return 1;
    }

    state.wl_registry = wl_display_get_registry(state.wl_display);
    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    if (state.wakefield == NULL) {
        fprintf(stderr, "No wakefield interface ('%s') available. Shutting down.\n", wakefield_interface.name);
        return 1;
    }

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_set_title(state.xdg_toplevel, "Example client");
    wl_surface_commit(state.wl_surface);

    wl_display_roundtrip(state.wl_display);

    wakefield_get_surface_location(state.wakefield, state.wl_surface);
    wl_display_dispatch(state.wl_display);

    while (wl_display_dispatch(state.wl_display)) {
    }

    return 0;
}