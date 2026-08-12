// shmclient — a minimal wl_shm client, for testing the window path end to end.
//
// Alpine ships no weston-simple-shm, and a purpose-built client is more useful
// anyway: what it draws is chosen so that the failure modes are visible rather
// than merely detectable. Orientation, channel order and stride each have a
// marker, the same way the host-side demo window does.

#define _GNU_SOURCE

#include "text-input-v3-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_output *output;
static struct wl_surface *surface;
static struct wl_seat *seat;
static struct wl_data_device_manager *data_manager;
static struct zwp_text_input_manager_v3 *text_input_manager;
static struct zwp_text_input_v3 *text_input;
static struct wl_data_device *data_device;
static struct wl_data_offer *incoming_offer;
static bool incoming_has_text;

/// What this client puts on the clipboard, so a `pbpaste` on the Mac has
/// something recognisable to show.
static const char CLIP_TEXT[] = "NativePipe clipboard from Linux";
#define CLIP_MIME "text/plain;charset=utf-8"

/// Last pointer position in surface-local logical units, and whether it is
/// inside. Drawn as a crosshair so input can be checked by moving the mouse.
static double pointer_x = -1, pointer_y = -1;
static bool pointer_inside = false;
static bool button_down = false;
static uint32_t last_key = 0;

// A popup, opened on right-click, to exercise xdg_positioner and xdg_popup.
static struct xdg_surface *popup_xdg;
static struct xdg_popup *popup;
static struct wl_surface *popup_surface;
static struct wl_buffer *popup_buffer;
static struct xdg_surface *toplevel_xdg;
static struct xdg_toplevel *toplevel_role;
#define POPUP_W 160
#define POPUP_H 120

static void popup_close(void);
static void popup_open(double x, double y);

/// Logical size, in surface-local coordinates — what xdg_toplevel.configure
/// speaks. The buffer is this multiplied by `scale`.
static int width = 480;
static int height = 320;
/// Output scale, straight from macOS's backing scale factor. Rendering at 1 on a
/// Retina display would look soft, and the compositor would have to upscale.
static int scale = 1;
static bool running = true;
static bool configured = false;
static int frame_counter = 0;
static bool full_damage = true;

static void frames_release(void);

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
		text_input_manager = wl_registry_bind(registry, name,
		                                      &zwp_text_input_manager_v3_interface, 1);
	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
		                                version < 3 ? version : 3);
	} else if (strcmp(interface, wl_seat_interface.name) == 0 && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
	} else if (strcmp(interface, wl_output_interface.name) == 0 && !output) {
		output = wl_registry_bind(registry, name, &wl_output_interface,
		                          version < 2 ? version : 2);
	}
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model, int32_t transform) {}
static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh) {}

static void output_scale(void *data, struct wl_output *o, int32_t factor) {
	if (factor <= 0 || factor == scale) return;
	scale = factor;
	printf("[client] output scale %d\n", scale);
	fflush(stdout);
	if (surface) wl_surface_set_buffer_scale(surface, scale);
	// The buffer has to be reallocated at the new density.
	frames_release();
}

static void output_done(void *data, struct wl_output *o) {}

static const struct wl_output_listener output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
	pointer_inside = true;
	pointer_x = wl_fixed_to_double(x);
	pointer_y = wl_fixed_to_double(y);
	printf("[client] pointer enter %.1f,%.1f\n", pointer_x, pointer_y);
	fflush(stdout);
}
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	pointer_inside = false;
	printf("[client] pointer leave\n");
	fflush(stdout);
}
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y) {
	pointer_inside = true;
	pointer_x = wl_fixed_to_double(x);
	pointer_y = wl_fixed_to_double(y);
}
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) {
	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (button == 0x110) button_down = pressed;
	printf("[client] button 0x%x %s at %.1f,%.1f\n", button, pressed ? "down" : "up",
	       pointer_x, pointer_y);
	fflush(stdout);
	if (button == 0x111 && pressed) popup_open(pointer_x, pointer_y);
}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {
	printf("[client] axis %u %.2f\n", axis, wl_fixed_to_double(value));
	fflush(stdout);
}
static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t steps) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                            int32_t fd, uint32_t size) {
	printf("[client] keymap format=%u size=%u\n", format, size);
	fflush(stdout);
	close(fd);
}
static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *s, struct wl_array *keys) {
	printf("[client] keyboard focus in: %s\n",
	       s == popup_surface ? "POPUP" : s == surface ? "toplevel" : "other");
	fflush(stdout);
}
static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *s) {
	printf("[client] keyboard focus out: %s\n",
	       s == popup_surface ? "POPUP" : s == surface ? "toplevel" : "other");
	fflush(stdout);
}
static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state) {
	last_key = key;
	printf("[client] key %u %s\n", key, state ? "down" : "up");
	fflush(stdout);
}
static void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group) {}
static void keyboard_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay) {}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	xdg_surface_ack_configure(surface, serial);
	configured = true;
}
static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                               int32_t new_width, int32_t new_height, struct wl_array *states) {
	// A zero size means "you choose"; anything else is the host's decision and
	// the client redraws to match.
	if (new_width > 0 && new_height > 0 && (new_width != width || new_height != height)) {
		width = new_width;
		height = new_height;
		printf("[client] configure %dx%d\n", width, height);
		fflush(stdout);
		// Redraw at the size the host asked for, which means new buffers — and
		// everything in them is new, so the next frame's damage is the lot.
		frames_release();
		full_damage = true;
	}
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	printf("[client] close requested\n");
	fflush(stdout);
	running = false;
}
static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

/// Every element here exists to make one specific defect visible. Coordinates
/// are in pixels; features are multiplied by `scale` so they keep the same
/// physical size whatever the density.
static void paint(uint32_t *pixels, int w, int h, int stride_words) {
	const int u = scale;
	const uint32_t background = 0xFF101418;
	for (int y = 0; y < h; y++) {
		uint32_t *row = pixels + (size_t)y * stride_words;
		for (int x = 0; x < w; x++) row[x] = background;
	}

	// Border: a mishandled stride shears it off the right edge.
	for (int x = 0; x < w; x++) {
		pixels[x] = 0xFF808080;
		pixels[(size_t)(h - 1) * stride_words + x] = 0xFF808080;
	}
	for (int y = 0; y < h; y++) {
		pixels[(size_t)y * stride_words] = 0xFF808080;
		pixels[(size_t)y * stride_words + w - 1] = 0xFF808080;
	}

	// Diagonal from the top-left corner. Vertical flips send it the other way.
	int diagonal = w < h ? w : h;
	for (int i = 0; i < diagonal; i++) {
		pixels[(size_t)i * stride_words + i] = 0xFFFFD400;
	}

	// A solid block in the top-left quadrant only, so mirroring is obvious.
	for (int y = 12 * u; y < 48 * u; y++) {
		for (int x = 12 * u; x < 120 * u; x++) pixels[(size_t)y * stride_words + x] = 0xFFFFD400;
	}

	// Channel order: red, green, blue, white in that order left to right. BGRA
	// read as RGBA swaps the first and third.
	const uint32_t swatches[4] = { 0xFFFF3B30, 0xFF34C759, 0xFF0A84FF, 0xFFFFFFFF };
	for (int s = 0; s < 4; s++) {
		for (int y = 64 * u; y < 112 * u && y < h - 1; y++) {
			for (int x = (12 + s * 56) * u; x < (12 + s * 56 + 48) * u && x < w - 1; x++) {
				pixels[(size_t)y * stride_words + x] = swatches[s];
			}
		}
	}

	// A crosshair where the pointer is, so mouse input can be checked by moving
	// it, and a filled square while a button is held.
	if (pointer_inside) {
		int px = (int)(pointer_x * u), py = (int)(pointer_y * u);
		uint32_t ink = button_down ? 0xFF34C759 : 0xFFFF9F0A;
		int arm = 10 * u;
		for (int d = -arm; d <= arm; d++) {
			int x = px + d, y = py;
			if (x > 0 && x < w - 1 && y > 0 && y < h - 1) pixels[(size_t)y * stride_words + x] = ink;
			x = px; y = py + d;
			if (x > 0 && x < w - 1 && y > 0 && y < h - 1) pixels[(size_t)y * stride_words + x] = ink;
		}
	}

	// The last key received, as a bar whose length is its evdev code.
	if (last_key > 0) {
		int len = (int)(last_key % 120) * u;
		for (int y = 120 * u; y < 132 * u && y < h - 1; y++) {
			for (int x = 12 * u; x < 12 * u + len && x < w - 1; x++) {
				pixels[(size_t)y * stride_words + x] = 0xFF5AC8FA;
			}
		}
	}

	// Something moving, so a frozen window is obvious.
	int sweep = 12 * u + (frame_counter % 60) * ((w - 60 * u) / 60);
	for (int y = h - 40 * u; y < h - 20 * u && y < h - 1; y++) {
		for (int x = sweep; x < sweep + 40 * u && x < w - 1; x++) {
			pixels[(size_t)y * stride_words + x] = 0xFFFF9F0A;
		}
	}
}

// One pool holding two buffers, alternated. Real toolkits work this way: a pool
// per frame is legal but churns file descriptors for no reason.
static struct {
	struct wl_shm_pool *pool;
	struct wl_buffer *buffers[2];
	void *data;
	size_t size;
	int stride;
	int pixel_width;
	int pixel_height;
	int back;
} frames;

static void frames_release(void) {
	for (int i = 0; i < 2; i++) {
		if (frames.buffers[i]) wl_buffer_destroy(frames.buffers[i]);
		frames.buffers[i] = NULL;
	}
	if (frames.pool) {
		wl_shm_pool_destroy(frames.pool);
		frames.pool = NULL;
	}
	if (frames.data) {
		munmap(frames.data, frames.size);
		frames.data = NULL;
	}
}

static bool frames_allocate(void) {
	frames_release();
	frames.pixel_width = width * scale;
	frames.pixel_height = height * scale;
	frames.stride = frames.pixel_width * 4;
	size_t plane = (size_t)frames.stride * frames.pixel_height;
	frames.size = plane * 2;

	int fd = memfd_create("shmclient", MFD_CLOEXEC);
	if (fd < 0) {
		perror("memfd_create");
		return false;
	}
	if (ftruncate(fd, (off_t)frames.size) < 0) {
		perror("ftruncate");
		close(fd);
		return false;
	}
	frames.data = mmap(NULL, frames.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (frames.data == MAP_FAILED) {
		perror("mmap");
		frames.data = NULL;
		close(fd);
		return false;
	}

	frames.pool = wl_shm_create_pool(shm, fd, (int32_t)frames.size);
	for (int i = 0; i < 2; i++) {
		frames.buffers[i] = wl_shm_pool_create_buffer(
			frames.pool, (int32_t)(plane * i), frames.pixel_width, frames.pixel_height,
			frames.stride, WL_SHM_FORMAT_XRGB8888);
	}
	close(fd);
	return true;
}

static struct wl_buffer *make_frame(void) {
	if (!frames.pool && !frames_allocate()) return NULL;
	size_t plane = (size_t)frames.stride * frames.pixel_height;
	uint32_t *pixels = (uint32_t *)((unsigned char *)frames.data + plane * frames.back);
	paint(pixels, frames.pixel_width, frames.pixel_height, frames.stride / 4);
	struct wl_buffer *buffer = frames.buffers[frames.back];
	frames.back = 1 - frames.back;
	return buffer;
}

/// A one-off buffer, filled with a flat colour and a border so the popup is
/// unmistakable on screen.
static struct wl_buffer *make_solid_buffer(int w, int h, uint32_t fill, uint32_t edge) {
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("popup", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0) close(fd);
		return NULL;
	}
	uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) { close(fd); return NULL; }
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			bool border = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
			bool stripe = ((x + y) / 8) % 2 == 0;
			pixels[(size_t)y * w + x] = border ? edge : (stripe ? fill : fill - 0x00101010);
		}
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
	                                                     WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	munmap(pixels, size);
	close(fd);
	return buffer;
}

static void popup_xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
	xdg_surface_ack_configure(s, serial);
	if (!popup_buffer) {
		popup_buffer = make_solid_buffer(POPUP_W * scale, POPUP_H * scale,
		                                 0xFF2C3E50, 0xFF5AC8FA);
	}
	if (popup_buffer && popup_surface) {
		if (scale > 1) wl_surface_set_buffer_scale(popup_surface, scale);
		wl_surface_attach(popup_surface, popup_buffer, 0, 0);
		wl_surface_damage_buffer(popup_surface, 0, 0, POPUP_W * scale, POPUP_H * scale);
		wl_surface_commit(popup_surface);
	}
}
static const struct xdg_surface_listener popup_xdg_listener = {
	.configure = popup_xdg_configure,
};

static void popup_configure(void *data, struct xdg_popup *p, int32_t x, int32_t y,
                            int32_t w, int32_t h) {
	printf("[client] popup configure %d,%d %dx%d\n", x, y, w, h);
	fflush(stdout);
}
static void popup_done(void *data, struct xdg_popup *p) {
	printf("[client] popup dismissed\n");
	fflush(stdout);
	popup_close();
}
static const struct xdg_popup_listener popup_listener = {
	.configure = popup_configure,
	.popup_done = popup_done,
};

static void popup_close(void) {
	if (popup) { xdg_popup_destroy(popup); popup = NULL; }
	if (popup_xdg) { xdg_surface_destroy(popup_xdg); popup_xdg = NULL; }
	if (popup_surface) { wl_surface_destroy(popup_surface); popup_surface = NULL; }
	if (popup_buffer) { wl_buffer_destroy(popup_buffer); popup_buffer = NULL; }
}


// ---------------------------------------------------------------------------
// Clipboard
//
// Both directions are exercised without any input: this client takes the
// selection at startup, and prints whatever the selection becomes afterwards.
// So `pbpaste` on the Mac shows the guest's copy, and a `pbcopy` on the Mac
// shows up in this client's log.
// ---------------------------------------------------------------------------

static void data_source_target(void *data, struct wl_data_source *source, const char *mime) {}

static void data_source_send(void *data, struct wl_data_source *source,
                             const char *mime, int32_t fd) {
	printf("[client] clipboard: asked for %s\n", mime);
	fflush(stdout);
	size_t len = strlen(CLIP_TEXT), sent = 0;
	while (sent < len) {
		ssize_t put = write(fd, CLIP_TEXT + sent, len - sent);
		if (put > 0) { sent += (size_t)put; continue; }
		if (put < 0 && errno == EINTR) continue;
		break;
	}
	close(fd);
}

static void data_source_cancelled(void *data, struct wl_data_source *source) {
	printf("[client] clipboard: our selection was replaced\n");
	fflush(stdout);
	wl_data_source_destroy(source);
}

static void data_source_dnd_drop(void *data, struct wl_data_source *source) {}
static void data_source_dnd_finished(void *data, struct wl_data_source *source) {}
static void data_source_action(void *data, struct wl_data_source *source, uint32_t action) {}

static const struct wl_data_source_listener data_source_listener = {
	.target = data_source_target,
	.send = data_source_send,
	.cancelled = data_source_cancelled,
	.dnd_drop_performed = data_source_dnd_drop,
	.dnd_finished = data_source_dnd_finished,
	.action = data_source_action,
};

static void data_offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
	if (offer != incoming_offer) return;
	printf("[client] clipboard: offer advertises %s\n", mime);
	fflush(stdout);
	if (strcmp(mime, CLIP_MIME) == 0 || strcmp(mime, "text/plain") == 0) {
		incoming_has_text = true;
	}
}
static void data_offer_source_actions(void *data, struct wl_data_offer *o, uint32_t a) {}
static void data_offer_action(void *data, struct wl_data_offer *o, uint32_t a) {}

static const struct wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer,
	.source_actions = data_offer_source_actions,
	.action = data_offer_action,
};

static void data_device_data_offer(void *data, struct wl_data_device *device,
                                   struct wl_data_offer *offer) {
	incoming_offer = offer;
	incoming_has_text = false;
	wl_data_offer_add_listener(offer, &data_offer_listener, NULL);
}

/// Pulls the selection through a pipe. The compositor may take a round trip to
/// the host to answer, so this waits rather than assuming the bytes are ready,
/// but it gives up instead of stalling the client for good.
static void drain_selection(struct wl_display *display, struct wl_data_offer *offer) {
	int fds[2];
	if (pipe(fds) < 0) return;
	wl_data_offer_receive(offer, CLIP_MIME, fds[1]);
	close(fds[1]);
	wl_display_flush(display);

	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	char buffer[4096];
	size_t total = 0;
	for (int attempt = 0; attempt < 400 && total < sizeof(buffer) - 1; attempt++) {
		// The answer arrives as compositor events, so the dispatch is what makes
		// progress possible at all.
		wl_display_roundtrip(display);
		ssize_t got = read(fds[0], buffer + total, sizeof(buffer) - 1 - total);
		if (got > 0) { total += (size_t)got; continue; }
		if (got == 0) break;
		if (errno != EAGAIN && errno != EWOULDBLOCK) break;
		usleep(5000);
	}
	close(fds[0]);
	buffer[total] = '\0';
	printf("[client] clipboard: read back %zu bytes: \"%s\"\n", total, buffer);
	fflush(stdout);
}

static struct wl_display *client_display;
/// Set from the selection callback and acted on by the main loop. Draining
/// inside the callback would mean dispatching the display from within a
/// dispatch, which libwayland does not support.
static bool selection_pending;

static void data_device_selection(void *data, struct wl_data_device *device,
                                  struct wl_data_offer *offer) {
	if (!offer) {
		printf("[client] clipboard: selection cleared\n");
		fflush(stdout);
		incoming_offer = NULL;
		return;
	}
	printf("[client] clipboard: selection changed (text=%s)\n",
	       incoming_has_text ? "yes" : "no");
	fflush(stdout);
	incoming_offer = offer;
	selection_pending = incoming_has_text;
}

static void data_device_enter(void *data, struct wl_data_device *d, uint32_t serial,
                              struct wl_surface *s, wl_fixed_t x, wl_fixed_t y,
                              struct wl_data_offer *offer) {}
static void data_device_leave(void *data, struct wl_data_device *d) {}
static void data_device_motion(void *data, struct wl_data_device *d, uint32_t time,
                               wl_fixed_t x, wl_fixed_t y) {}
static void data_device_drop(void *data, struct wl_data_device *d) {}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = data_device_data_offer,
	.enter = data_device_enter,
	.leave = data_device_leave,
	.motion = data_device_motion,
	.drop = data_device_drop,
	.selection = data_device_selection,
};


// ---------------------------------------------------------------------------
// text-input-v3
//
// Standing in for a real text field: the client says it is focused and where
// the caret is, then prints whatever the Mac's input method produces. Typing
// CJK into the window should show up here as commit_string, with the
// intermediate romaji or pinyin arriving as preedit_string first.
// ---------------------------------------------------------------------------

static void text_input_enter(void *data, struct zwp_text_input_v3 *ti,
                             struct wl_surface *s) {
	printf("[client] text-input: enter\n");
	fflush(stdout);
	zwp_text_input_v3_enable(ti);
	// A caret near the bottom-left, so the candidate window has somewhere
	// sensible to appear.
	zwp_text_input_v3_set_cursor_rectangle(ti, 24, height - 60, 2, 20);
	zwp_text_input_v3_commit(ti);
}

static void text_input_leave(void *data, struct zwp_text_input_v3 *ti,
                             struct wl_surface *s) {
	printf("[client] text-input: leave\n");
	fflush(stdout);
}

static void text_input_preedit(void *data, struct zwp_text_input_v3 *ti,
                               const char *text, int32_t begin, int32_t end) {
	printf("[client] text-input: preedit \"%s\" (%d..%d)\n", text ? text : "", begin, end);
	fflush(stdout);
}

static void text_input_commit_string(void *data, struct zwp_text_input_v3 *ti,
                                     const char *text) {
	printf("[client] text-input: COMMIT \"%s\"\n", text ? text : "");
	fflush(stdout);
}

static void text_input_delete_surrounding(void *data, struct zwp_text_input_v3 *ti,
                                          uint32_t before, uint32_t after) {
	printf("[client] text-input: delete %u before, %u after\n", before, after);
	fflush(stdout);
}

static void text_input_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial) {
	printf("[client] text-input: done serial=%u\n", serial);
	fflush(stdout);
}

static const struct zwp_text_input_v3_listener text_input_listener = {
	.enter = text_input_enter,
	.leave = text_input_leave,
	.preedit_string = text_input_preedit,
	.commit_string = text_input_commit_string,
	.delete_surrounding_text = text_input_delete_surrounding,
	.done = text_input_done,
};

static void popup_open(double x, double y) {
	popup_close();
	struct xdg_positioner *positioner = xdg_wm_base_create_positioner(wm_base);
	if (!positioner) return;
	// Anchored on a one-pixel rectangle at the cursor, growing down and right —
	// the shape of every context menu.
	xdg_positioner_set_size(positioner, POPUP_W, POPUP_H);
	xdg_positioner_set_anchor_rect(positioner, (int32_t)x, (int32_t)y, 1, 1);
	xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);

	popup_surface = wl_compositor_create_surface(compositor);
	popup_xdg = xdg_wm_base_get_xdg_surface(wm_base, popup_surface);
	xdg_surface_add_listener(popup_xdg, &popup_xdg_listener, NULL);
	popup = xdg_surface_get_popup(popup_xdg, toplevel_xdg, positioner);
	xdg_popup_add_listener(popup, &popup_listener, NULL);
	xdg_popup_grab(popup, seat, 0);
	wl_surface_commit(popup_surface);
	xdg_positioner_destroy(positioner);
	printf("[client] popup opened at %.0f,%.0f\n", x, y);
	fflush(stdout);
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "[client] could not connect to a Wayland display\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "[client] missing globals: compositor=%p shm=%p xdg_wm_base=%p\n",
		        (void *)compositor, (void *)shm, (void *)wm_base);
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);

	if (output) wl_output_add_listener(output, &output_listener, NULL);
	if (seat) {
		struct wl_pointer *pointer = wl_seat_get_pointer(seat);
		if (pointer) wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
		if (keyboard) wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
		if (text_input_manager) {
			text_input = zwp_text_input_manager_v3_get_text_input(text_input_manager, seat);
			zwp_text_input_v3_add_listener(text_input, &text_input_listener, NULL);
		}
		if (data_manager) {
			client_display = display;
			data_device = wl_data_device_manager_get_data_device(data_manager, seat);
			wl_data_device_add_listener(data_device, &data_device_listener, NULL);
		}
	}

	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(wm_base, surface);
	toplevel_xdg = xdg_surface;
	xdg_surface_add_listener(xdg_surface, &xdg_surface_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);
	toplevel_role = toplevel;
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(toplevel, "Linux wl_shm client");
	xdg_toplevel_set_app_id(toplevel, "org.nativepipe.shmclient");
	xdg_toplevel_set_min_size(toplevel, 240, 160);

	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) != -1) {}
	if (scale > 1) wl_surface_set_buffer_scale(surface, scale);
	printf("[client] configured, drawing %dx%d logical at scale %d\n", width, height, scale);
	fflush(stdout);

	if (data_device && data_manager) {
		struct wl_data_source *source =
			wl_data_device_manager_create_data_source(data_manager);
		wl_data_source_add_listener(source, &data_source_listener, NULL);
		wl_data_source_offer(source, CLIP_MIME);
		wl_data_source_offer(source, "text/plain");
		wl_data_device_set_selection(data_device, source, 0);
		printf("[client] clipboard: offered \"%s\"\n", CLIP_TEXT);
		fflush(stdout);
	}

	while (running) {
		if (selection_pending && incoming_offer) {
			selection_pending = false;
			drain_selection(display, incoming_offer);
		}
		struct wl_buffer *buffer = make_frame();
		if (!buffer) break;
		wl_surface_attach(surface, buffer, 0, 0);
		if (full_damage || frame_counter < 2 || pointer_inside) {
			wl_surface_damage_buffer(surface, 0, 0, frames.pixel_width, frames.pixel_height);
			full_damage = false;
		} else {
			// Only the sweep bar moves. Reporting that honestly is what lets the
			// server copy a few rows instead of the whole surface.
			wl_surface_damage_buffer(surface, 0, frames.pixel_height - 44 * scale,
			                         frames.pixel_width, 28 * scale);
		}
		wl_surface_commit(surface);

		// roundtrip, not dispatch_pending: the latter only drains events already
		// queued in memory and never reads the socket, so wl_buffer.release
		// events pile up until libwayland's 4 KiB connection buffer overflows and
		// the server drops the client ("Data too big for buffer").
		if (wl_display_roundtrip(display) < 0) {
			// Say why. A client that just vanishes turns a protocol bug into a
			// guessing game on the other side of the VM boundary.
			int err = wl_display_get_error(display);
			const struct wl_interface *iface = NULL;
			uint32_t id = 0;
			uint32_t code = wl_display_get_protocol_error(display, &iface, &id);
			fprintf(stderr, "[client] dispatch failed: errno=%d (%s) protocol_error=%u on %s#%u\n",
			        err, strerror(err), code, iface ? iface->name : "?", id);
			break;
		}
		frame_counter++;
		usleep(33000);  // ~30 fps is plenty for a test client
	}

	printf("[client] exiting after %d frames\n", frame_counter);
	fflush(stdout);
	frames_release();
	wl_display_disconnect(display);
	return 0;
}
