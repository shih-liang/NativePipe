// wl_seat devices and translation of host input/control messages.

#define _GNU_SOURCE

#include "compositor_internal.h"
#include "data_device.h"
#include "scale.h"
#include "scene.h"
#include "text_input.h"
#include "window_events.h"
#include "xdg_shell.h"
#include "xdg-shell-server-protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

static bool same_client(struct wl_resource *a, struct wl_resource *b);

static uint32_t input_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static wl_fixed_t input_fixed_from(double value)
{
	return wl_fixed_from_double(value);
}

// ---------------------------------------------------------------------------
// wl_seat and wl_output
// ---------------------------------------------------------------------------

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112

/// Compiles the keymap clients will use to turn evdev codes into characters.
///
/// The host sends physical key codes and nothing else — no text, no composed
/// characters — so *something* has to define the layout, and xkbcommon is what
/// every Wayland client already links against to read it.
bool np_input_create_keymap(struct np_server *server) {
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!context) return false;

	struct xkb_rule_names names = {
		.rules = NULL, .model = "pc105", .layout = "us",
		.variant = NULL, .options = NULL,
	};
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
	                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		xkb_context_unref(context);
		return false;
	}

	char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	if (!text) return false;

	size_t size = strlen(text) + 1;
	int fd = memfd_create("nativepipe-keymap", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		free(text);
		if (fd >= 0) close(fd);
		return false;
	}
	void *mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		free(text);
		close(fd);
		return false;
	}
	memcpy(mapped, text, size);
	munmap(mapped, size);
	free(text);

	server->keymap_fd = fd;
	server->keymap_size = size;
	return true;
}

static void input_resource_destroy(struct wl_resource *resource) {
	struct np_input *input = wl_resource_get_user_data(resource);
	if (!input) return;
	wl_list_remove(&input->link);
	free(input);
}

/// The pointer image belongs to the macOS window system. Clients still issue
/// wl_pointer.set_cursor after enter, and the request must be consumed even
/// when NativePipe does not use the supplied cursor surface. Registering a NULL
/// implementation makes libwayland abort the client connection on that first
/// perfectly valid request.
static void pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
                               uint32_t serial, struct wl_resource *surface,
                               int32_t hotspot_x, int32_t hotspot_y) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry || serial != entry->last_enter_serial) return;
	struct np_server *server = entry->server;
	uint32_t surface_id = 0;
	if (surface) {
		struct np_surface *cursor = wl_resource_get_user_data(surface);
		if (!cursor || !np_surface_assign_role(cursor, NP_SURFACE_ROLE_CURSOR)) {
			wl_resource_post_error(resource, WL_POINTER_ERROR_ROLE,
			                       "cursor surface has another role");
			return;
		}
		struct np_surface *pointer = np_surface_by_id(
			server, server->pointer_surface);
		struct np_surface *root = np_scene_root(pointer);
		if (root) np_scale_changed(cursor, root->preferred_scale);
		surface_id = cursor->id;
	}
	server->cursor_surface = surface;
	server->cursor_hotspot_x = hotspot_x;
	server->cursor_hotspot_y = hotspot_y;
	uint32_t fields[] = {
		surface_id, (uint32_t)hotspot_x, (uint32_t)hotspot_y,
	};
	np_window_event_send(server, NP_GUEST_CURSOR_CHANGED, fields, 3);
}

static void pointer_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_pointer_interface pointer_implementation = {
	.set_cursor = pointer_set_cursor,
	.release = pointer_release,
};

static void keyboard_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_keyboard_interface keyboard_implementation = {
	.release = keyboard_release,
};

static void seat_get_pointer(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *pointer =
		wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(resource), id);
	if (!pointer) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_input *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_resource_destroy(pointer);
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = pointer;
	entry->server = server;
	wl_list_insert(&server->pointers, &entry->link);
	wl_resource_set_implementation(
		pointer, &pointer_implementation, entry, input_resource_destroy);
}

static void seat_get_keyboard(struct wl_client *client, struct wl_resource *resource, uint32_t id) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *keyboard =
		wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(resource), id);
	if (!keyboard) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_input *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_resource_destroy(keyboard);
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = keyboard;
	entry->server = server;
	wl_list_insert(&server->keyboards, &entry->link);
	wl_resource_set_implementation(
		keyboard, &keyboard_implementation, entry, input_resource_destroy);

	if (server->keymap_fd >= 0) {
		wl_keyboard_send_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
		                        server->keymap_fd, (uint32_t)server->keymap_size);
	}
	if (wl_resource_get_version(keyboard) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
		// Wayland clients own key repeat. The host filters AppKit repeat events,
		// leaving one physical press/release pair for this policy to act on.
		wl_keyboard_send_repeat_info(keyboard, 25, 600);
	}
}

static void seat_get_touch(struct wl_client *client, struct wl_resource *resource, uint32_t id) {}
static void seat_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wl_seat_interface seat_implementation = {
	.get_pointer = seat_get_pointer,
	.get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch,
	.release = seat_release,
};

/// Advertised so clients bind and proceed; events are delivered only to the
/// client owning the surface the host says is focused or under the pointer.
void np_seat_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource =
		wl_resource_create(client, &wl_seat_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &seat_implementation, data, NULL);
	wl_seat_send_capabilities(resource, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	if (version >= WL_SEAT_NAME_SINCE_VERSION) wl_seat_send_name(resource, "NativePipe");
}

static void pointer_frame(struct wl_resource *pointer) {
	if (wl_resource_get_version(pointer) >= WL_POINTER_FRAME_SINCE_VERSION) {
		wl_pointer_send_frame(pointer);
	}
}

static void clear_pointer_focus(struct np_server *server) {
	if (!server || !server->pointer_surface) {
		if (server) server->pointer_window = 0;
		return;
	}
	struct np_surface *surface = np_surface_by_id(
		server, server->pointer_surface);
	if (surface) {
		uint32_t serial = wl_display_next_serial(server->display);
		struct np_input *entry;
		wl_list_for_each(entry, &server->pointers, link) {
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_pointer_send_leave(
				entry->resource, serial, surface->resource);
			pointer_frame(entry->resource);
		}
	}
	server->pointer_window = 0;
	server->pointer_surface = 0;
}

/// A data-device drag replaces the default pointer grab. Clear ordinary
/// wl_pointer focus before sending wl_data_device.enter, matching the ordering
/// used by full compositors. Without this leave, GDK carries widget hover state
/// into the DnD grab and the first post-drag click merely repairs that state.
void np_input_clear_pointer_focus_for_drag(struct np_server *server) {
	clear_pointer_focus(server);
}

/// Ending the DnD grab and finishing its data transfer are separate moments.
/// Re-establish ordinary pointer focus immediately at button release; waiting
/// for wl_data_offer.finish makes the window ignore input while the target is
/// still reading the dragged bytes.
void np_input_restore_pointer_focus_after_drag(struct np_server *server,
	                                         struct np_surface *surface) {
	if (!server || !surface) return;
	double local_x = server->pointer_x, local_y = server->pointer_y;
	struct np_surface *root = np_scene_root(surface);
	struct np_surface *target = np_scene_hit_test(
		root ? root : surface, server->pointer_x, server->pointer_y,
		&local_x, &local_y);
	if (!target) {
		server->pointer_window = 0;
		server->pointer_surface = 0;
		return;
	}
	uint32_t serial = wl_display_next_serial(server->display);
	struct np_input *entry;

	/* np_input_clear_pointer_focus_for_drag() already sent the leave at grab start. At
	 * grab end the default pointer focus is empty, so only enter is valid here. */
	wl_list_for_each(entry, &server->pointers, link) {
		if (!same_client(entry->resource, target->resource)) continue;
		wl_pointer_send_enter(entry->resource, serial, target->resource,
		                      input_fixed_from(local_x), input_fixed_from(local_y));
		entry->last_enter_serial = serial;
		pointer_frame(entry->resource);
	}
	server->pointer_window = surface->window_id;
	server->pointer_surface = target->id;
}

// ---------------------------------------------------------------------------
// host commands
// ---------------------------------------------------------------------------

static int json_int(cJSON *object, const char *key, int fallback) {
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

static double json_double(cJSON *object, const char *key, double fallback) {
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
	return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool same_client(struct wl_resource *a, struct wl_resource *b) {
	return wl_resource_get_client(a) == wl_resource_get_client(b);
}

/// Delivers keyboard focus, which in Wayland is leave-then-enter rather than a
/// single "focus is now X" event.

// ---------------------------------------------------------------------------
// zwp_text_input_v3
//
// This is the IME seam, and it is a narrow one on purpose. The client says "a
// text field is focused, and the caret is here"; macOS composes; the client is
// handed finished text and a preedit string. Nothing in between crosses the
// boundary — no candidate list, no conversion state, no key interception —
// because all of that is the input method's business and macOS already has one.
//
// Keys that are not text keep going down the raw evdev path. Return, Escape and
// the arrow keys are editing commands rather than characters, and a client that
// stopped receiving them while a text field was focused would be unusable.
// ---------------------------------------------------------------------------

/// recently created one — the innermost menu level.
static struct np_surface *grabbing_popup(struct np_server *server) {
	struct np_surface *surface;
	wl_list_for_each(surface, &server->surfaces, link) {
		if (surface->has_grab) return surface;
	}
	return NULL;
}

void np_set_keyboard_focus(struct np_server *server, uint32_t window_id) {
	if (server->focused_window == window_id) return;

	struct np_surface *previous = server->focused_window
		? np_surface_by_window(server, server->focused_window) : NULL;
	struct np_surface *next = window_id ? np_surface_by_window(server, window_id) : NULL;
	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);

	if (previous) {
		wl_list_for_each(entry, &server->keyboards, link) {
			if (!same_client(entry->resource, previous->resource)) continue;
			wl_keyboard_send_leave(entry->resource, serial, previous->resource);
		}
	}
	if (next) {
		struct wl_array keys;
		wl_array_init(&keys);
		// The selection belongs to the keyboard-focused client. The core
		// protocol requires this event immediately before keyboard.enter, not
		// while an unfocused client is merely constructing its data device.
		wl_list_for_each(entry, &server->data_devices, link) {
			if (!same_client(entry->resource, next->resource)) continue;
			np_data_send_selection(server, entry->resource);
		}
		int delivered = 0;
		wl_list_for_each(entry, &server->keyboards, link) {
			if (!same_client(entry->resource, next->resource)) continue;
			wl_keyboard_send_enter(entry->resource, serial, next->resource, &keys);
			delivered++;
		}
		wl_array_release(&keys);
		if (np_trace_enabled()) {
			fprintf(stderr, "[wayland] focus -> window %u surface %u, %d keyboards\n",
			        window_id, next->id, delivered);
		}
	} else if (np_trace_enabled()) {
		fprintf(stderr, "[wayland] focus -> window %u: no such surface\n", window_id);
	}
	server->focused_window = window_id;
	np_text_input_focus_changed(server, previous, next);
}

/// Wayland has no "modifiers changed" input event separate from the key that
/// changed them, so the current set is pushed alongside every key.
static void send_modifiers(struct np_server *server, struct np_surface *surface,
                           uint32_t modifiers) {
	uint32_t depressed = 0;
	if (modifiers & (1u << 0)) depressed |= 1u << 0;  // shift
	if (modifiers & (1u << 4)) depressed |= 1u << 1;  // caps lock
	if (modifiers & (1u << 1)) depressed |= 1u << 2;  // control
	if (modifiers & (1u << 2)) depressed |= 1u << 3;  // alt
	if (modifiers & (1u << 3)) depressed |= 1u << 6;  // super

	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);
	wl_list_for_each(entry, &server->keyboards, link) {
		if (!same_client(entry->resource, surface->resource)) continue;
		wl_keyboard_send_modifiers(entry->resource, serial, depressed, 0, 0, 0);
	}
}

static bool surface_local_position(struct np_surface *surface,
                                   double root_x, double root_y,
                                   double *local_x, double *local_y) {
	if (!surface) return false;
	struct np_surface *root = np_scene_root(surface);
	if (!root) return false;
	double x = root_x + (root->geometry_set ? root->geometry_x : 0);
	double y = root_y + (root->geometry_set ? root->geometry_y : 0);
	struct np_surface *current = surface;
	while (current && current != root) {
		x -= current->sub_x;
		y -= current->sub_y;
		current = current->parent;
	}
	if (current != root) return false;
	if (local_x) *local_x = x;
	if (local_y) *local_y = y;
	return true;
}

static void handle_pointer_position(struct np_server *server, uint32_t window_id,
                                    wl_fixed_t x, wl_fixed_t y) {
	struct np_surface *root = np_surface_by_window(server, window_id);
	if (!root) return;
	server->pointer_x = wl_fixed_to_double(x);
	server->pointer_y = wl_fixed_to_double(y);
	double local_x = server->pointer_x, local_y = server->pointer_y;
	bool implicit_grab = server->pointer_buttons != 0 &&
	                     !server->drag_source && server->pointer_surface;
	struct np_surface *surface = implicit_grab
		? np_surface_by_id(server, server->pointer_surface)
		: np_scene_hit_test(
			root, server->pointer_x, server->pointer_y, &local_x, &local_y);
	if (implicit_grab && surface &&
	    !surface_local_position(
		    surface, server->pointer_x, server->pointer_y,
		    &local_x, &local_y))
		surface = NULL;
	if (!surface) {
		if (server->drag_source && !server->drag_dropped &&
		    server->drag_focus_window) {
			np_data_drag_leave(server, server->drag_focus_window);
			server->drag_focus_window = 0;
		}
		if (!server->drag_source) clear_pointer_focus(server);
		wl_display_flush_clients(server->display);
		return;
	}
	wl_fixed_t sx = input_fixed_from(local_x), sy = input_fixed_from(local_y);
	bool entering = server->pointer_surface != surface->id;

	struct np_input *entry;
	uint32_t serial = wl_display_next_serial(server->display);
	uint32_t time = input_now_ms();
	if (server->drag_source && !server->drag_dropped) {
		struct np_surface *icon = server->drag_icon
			? wl_resource_get_user_data(server->drag_icon) : NULL;
		struct np_surface *root = np_scene_root(surface);
		if (icon && root) np_scale_changed(icon, root->preferred_scale);
		if (server->drag_focus_surface != surface->id) {
			if (server->drag_focus_window) np_data_drag_leave(server, server->drag_focus_window);
			np_data_drag_enter(server, surface, sx, sy);
		} else {
			np_data_drag_motion(server, surface, time, sx, sy);
		}
		wl_display_flush_clients(server->display);
		return;
	}

	// Tracking areas belonging to two NSWindows can overlap briefly while a
	// child window is ordered. Repair that host ordering to Wayland's single
	// pointer-focus, leave-before-enter model.
	if (entering && server->pointer_surface) {
		struct np_surface *previous = np_surface_by_id(server, server->pointer_surface);
		if (previous) {
			wl_list_for_each(entry, &server->pointers, link) {
				if (!same_client(entry->resource, previous->resource)) continue;
				wl_pointer_send_leave(entry->resource, serial, previous->resource);
				pointer_frame(entry->resource);
			}
		}
	}
	wl_list_for_each(entry, &server->pointers, link) {
		if (!same_client(entry->resource, surface->resource)) continue;
		if (entering) {
			wl_pointer_send_enter(entry->resource, serial, surface->resource, sx, sy);
			entry->last_enter_serial = serial;
		}
		else wl_pointer_send_motion(entry->resource, time, sx, sy);
		pointer_frame(entry->resource);
	}
	if (!implicit_grab) server->pointer_window = window_id;
	server->pointer_surface = surface->id;
	wl_display_flush_clients(server->display);
}

static uint32_t read_le32(const unsigned char *bytes) {
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const unsigned char *bytes) {
	return (uint64_t)read_le32(bytes) | ((uint64_t)read_le32(bytes + 4) << 32);
}

static void handle_pointer_scroll(struct np_server *server, uint32_t window_id,
                                  double dx, double dy) {
	struct np_surface *surface = np_surface_by_id(server, server->pointer_surface);
	if (!surface || server->pointer_window != window_id) return;
	struct np_input *entry;
	uint32_t time = input_now_ms();
	wl_list_for_each(entry, &server->pointers, link) {
		if (!same_client(entry->resource, surface->resource)) continue;
		if (dy != 0) {
			wl_pointer_send_axis(entry->resource, time,
			                     WL_POINTER_AXIS_VERTICAL_SCROLL, input_fixed_from(dy));
		}
		if (dx != 0) {
			wl_pointer_send_axis(entry->resource, time,
			                     WL_POINTER_AXIS_HORIZONTAL_SCROLL, input_fixed_from(dx));
		}
		pointer_frame(entry->resource);
	}
	wl_display_flush_clients(server->display);
}

/// Binary host-control path. High-rate state and frame feedback never allocate
/// a cJSON object; the outer NPIP framing still supplies message boundaries.
void np_input_handle_host_binary(const unsigned char *payload, size_t length, void *user_data) {
	struct np_server *server = user_data;
	if (length == 16 && memcmp(payload, "NPMO", 4) == 0) {
		uint32_t window_id = read_le32(payload + 4);
		wl_fixed_t x = (wl_fixed_t)(int32_t)read_le32(payload + 8);
		wl_fixed_t y = (wl_fixed_t)(int32_t)read_le32(payload + 12);
		handle_pointer_position(server, window_id, x, y);
		return;
	}
	if (length == 28 && memcmp(payload, "NPSC", 4) == 0) {
		uint64_t dx_bits = read_le64(payload + 8);
		uint64_t dy_bits = read_le64(payload + 16);
		double dx, dy;
		memcpy(&dx, &dx_bits, sizeof(dx));
		memcpy(&dy, &dy_bits, sizeof(dy));
		handle_pointer_scroll(server, read_le32(payload + 4), dx, dy);
		return;
	}
	if (length == 24 && memcmp(payload, "NPCF", 4) == 0) {
		uint32_t window_id = read_le32(payload + 4);
		struct np_surface *surface = np_surface_by_window(server, window_id);
		if (!surface || !surface->toplevel) return;
		int32_t width = (int32_t)read_le32(payload + 8);
		int32_t height = (int32_t)read_le32(payload + 12);
		uint32_t state_bits = read_le32(payload + 16);
		/* payload + 20 is the host-side diagnostic serial. Wayland owns the
		 * configure serial and flow control is tied to the output latch. */
		np_xdg_configure_toplevel_from_host(
			surface, width, height, state_bits);
		wl_display_flush_clients(server->display);
		return;
	}
	if (length == 28 && memcmp(payload, "NPPF", 4) == 0) {
		struct np_surface *surface = np_surface_by_window(
			server, read_le32(payload + 4));
		if (!surface || !surface->popup) return;
		np_xdg_configure_popup(
			surface, (int32_t)read_le32(payload + 8),
			(int32_t)read_le32(payload + 12),
			(int32_t)read_le32(payload + 16),
			(int32_t)read_le32(payload + 20),
			read_le32(payload + 24));
		wl_display_flush_clients(server->display);
		return;
	}
	if (length >= 8 && memcmp(payload, "NPFT", 4) == 0) {
		uint32_t count = read_le32(payload + 4);
		if ((length - 8) % 12 != 0 || (size_t)count != (length - 8) / 12)
			return;
		for (uint32_t i = 0; i < count; i++) {
			const unsigned char *record = payload + 8 + (size_t)i * 12;
			uint32_t kind = read_le32(record);
			uint32_t surface_id = read_le32(record + 4);
			uint32_t presentation_id = read_le32(record + 8);
			if (np_trace_enabled())
				fprintf(stderr,
				        "[feedback] %s surface=%u present=%u\n",
				        kind == 1 ? "presented" :
				        kind == 2 ? "released" : "unknown",
				        surface_id, presentation_id);
			if (kind == 1) {
				/* A configure record can precede this feedback in the same host
				 * transport drain. Emit that newest configure before waking the
				 * client's frame callback; otherwise the idle coalescer reverses
				 * their observable order. */
				np_xdg_flush_pending_toplevel_configure(
					np_surface_by_id(server, surface_id));
				np_presentation_process_presented(server, surface_id, presentation_id);
			} else if (kind == 2) {
				np_presentation_process_released(server, surface_id, presentation_id);
			}
		}
		np_presentation_finish_feedback(server);
	}
}

void np_input_handle_host_command(const char *name, cJSON *body, void *user_data) {
	struct np_server *server = user_data;
	if (np_trace_enabled() && strcmp(name, "pointerMoved") != 0 &&
	    strcmp(name, "pointerScroll") != 0 && strcmp(name, "configure") != 0) {
		char *text = cJSON_PrintUnformatted(body);
		fprintf(stderr, "[wayland] <- %s %s\n", name, text ? text : "");
		free(text);
	}

	if (strcmp(name, "framePresented") == 0) {
		uint32_t surface_id = (uint32_t)json_int(body, "surface", 0);
		uint32_t presentation_id = (uint32_t)json_int(body, "presentationID", 0);
		np_xdg_flush_pending_toplevel_configure(
			np_surface_by_id(server, surface_id));
		np_presentation_process_presented(server, surface_id, presentation_id);
		np_presentation_finish_feedback(server);
		return;
	}

	if (strcmp(name, "frameReleased") == 0) {
		uint32_t surface_id = (uint32_t)json_int(body, "surface", 0);
		uint32_t presentation_id = (uint32_t)json_int(body, "presentationID", 0);
		np_presentation_process_released(server, surface_id, presentation_id);
		np_presentation_finish_feedback(server);
		return;
	}

	if (strcmp(name, "configure") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		struct np_surface *surface = np_surface_by_window(server, window_id);
		if (!surface || !surface->toplevel) return;

		cJSON *size = cJSON_GetObjectItemCaseSensitive(body, "size");
		int width = size ? json_int(size, "width", 0) : 0;
		int height = size ? json_int(size, "height", 0) : 0;

		uint32_t state_bits = 0;
		cJSON *state_list = cJSON_GetObjectItemCaseSensitive(body, "states");
		cJSON *state;
		cJSON_ArrayForEach(state, state_list) {
			if (!cJSON_IsString(state)) continue;
			if (strcmp(state->valuestring, "maximized") == 0)
				state_bits |= NP_CONFIGURE_MAXIMIZED;
			else if (strcmp(state->valuestring, "fullscreen") == 0)
				state_bits |= NP_CONFIGURE_FULLSCREEN;
			else if (strcmp(state->valuestring, "resizing") == 0)
				state_bits |= NP_CONFIGURE_RESIZING;
			else if (strcmp(state->valuestring, "activated") == 0)
				state_bits |= NP_CONFIGURE_ACTIVATED;
		}
		np_xdg_configure_toplevel_from_host(
			surface, width, height, state_bits);
		// Flushed before allocating: the client starts repainting now, and the
		// map round trip overlaps that instead of following it.
		wl_display_flush_clients(server->display);

		return;
	}

	if (strcmp(name, "close") == 0) {
		struct np_surface *surface = np_surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (surface && surface->toplevel) {
			// A request, not an order: the client decides whether to exit.
			xdg_toplevel_send_close(surface->toplevel);
			wl_display_flush_clients(server->display);
		}
		return;
	}

	if (strcmp(name, "dismissPopup") == 0) {
		struct np_surface *surface = np_surface_by_window(server, (uint32_t)json_int(body, "window", 0));
		if (surface && surface->popup) {
			xdg_popup_send_popup_done(surface->popup);
			wl_display_flush_clients(server->display);
		}
		return;
	}

	if (strcmp(name, "textCommit") == 0) {
		cJSON *text = cJSON_GetObjectItemCaseSensitive(body, "text");
		np_text_input_deliver(server, cJSON_IsString(text) ? text->valuestring : "",
		                   // An empty preedit alongside the commit is what ends
		                   // the composition the commit came out of.
		                   "", 0, 0, 0, 0);
		return;
	}

	if (strcmp(name, "textPreedit") == 0) {
		cJSON *text = cJSON_GetObjectItemCaseSensitive(body, "text");
		np_text_input_deliver(server, NULL, cJSON_IsString(text) ? text->valuestring : "",
		                   (int32_t)json_int(body, "cursorBegin", 0),
		                   (int32_t)json_int(body, "cursorEnd", 0), 0, 0);
		return;
	}

	if (strcmp(name, "textDeleteSurrounding") == 0) {
		np_text_input_deliver(server, NULL, NULL, 0, 0,
		                   (int32_t)json_int(body, "beforeLength", 0),
		                   (int32_t)json_int(body, "afterLength", 0));
		return;
	}

	if (strcmp(name, "selectionRequest") == 0) {
		cJSON *mime = cJSON_GetObjectItemCaseSensitive(body, "mimeType");
		np_data_serve_host_request(server, (uint32_t)json_int(body, "token", 0),
		                        cJSON_IsString(mime) ? mime->valuestring : "");
		return;
	}

	if (strcmp(name, "hostSelectionOffered") == 0) {
		// The Mac now owns the clipboard, which retires whatever guest client
		// held it. Telling that client it was cancelled is what lets it drop the
		// data it was holding for a paste that will never come.
		for (int i = 0; i < server->host_mime_count; i++) free(server->host_mime[i]);
		server->host_mime_count = 0;
		cJSON *types = cJSON_GetObjectItemCaseSensitive(body, "mimeTypes");
		cJSON *type;
		cJSON_ArrayForEach(type, types) {
			if (!cJSON_IsString(type)) continue;
			if (server->host_mime_count >= (int)(sizeof(server->host_mime) /
			                                     sizeof(server->host_mime[0]))) break;
			server->host_mime[server->host_mime_count++] = strdup(type->valuestring);
		}
		if (server->host_mime_count > 0 && server->selection_source) {
			wl_data_source_send_cancelled(server->selection_source);
			server->selection_source = NULL;
		}
		struct np_surface *focused = server->focused_window
			? np_surface_by_window(server, server->focused_window) : NULL;
		if (focused) {
			struct np_input *device;
			wl_list_for_each(device, &server->data_devices, link) {
				if (wl_resource_get_client(device->resource) !=
				    wl_resource_get_client(focused->resource)) continue;
				np_data_send_selection(server, device->resource);
			}
		}
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "hostSelectionData") == 0) {
		cJSON *encoded = cJSON_GetObjectItemCaseSensitive(body, "base64");
		np_data_deliver_host_data(server, (uint32_t)json_int(body, "token", 0),
		                       cJSON_IsString(encoded) ? encoded->valuestring : NULL);
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "keyboardFocus") == 0) {
		uint32_t window = (uint32_t)json_int(body, "window", 0);
		/* A non-activating popup panel never becomes AppKit's key window, but its
		 * Wayland grab owns keyboard focus while the application is active.  An
		 * application deactivation still clears the seat; if it becomes active
		 * before the popup is destroyed, restore focus to the grabbing popup. */
		struct np_surface *popup = grabbing_popup(server);
		np_set_keyboard_focus(
			server, popup && window != 0 ? popup->window_id : window);
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "key") == 0) {
		/* AppKit reports the key parent NSWindow even for a non-activating menu
		 * panel.  Key events belong to the Wayland keyboard focus established by
		 * wl_keyboard.enter, not to that host window identifier. */
		struct np_surface *surface = np_surface_by_window(
			server, server->focused_window);
		if (!surface) return;
		uint32_t keycode = (uint32_t)json_int(body, "keycode", 0);
		bool pressed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "pressed"));
		send_modifiers(server, surface, (uint32_t)json_int(body, "modifiers", 0));

		struct np_input *entry;
		uint32_t serial = wl_display_next_serial(server->display);
		if (pressed) {
			server->last_input_serial = serial;
			server->last_input_client = wl_resource_get_client(surface->resource);
		}
		uint32_t time = input_now_ms();
		int delivered = 0, total = 0;
		wl_list_for_each(entry, &server->keyboards, link) {
			total++;
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_keyboard_send_key(entry->resource, serial, time, keycode,
			                     pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
			                             : WL_KEYBOARD_KEY_STATE_RELEASED);
			delivered++;
		}
		if (np_trace_enabled()) {
			fprintf(stderr, "[wayland] key %u -> %d of %d keyboards (surface %u)\n",
			        keycode, delivered, total, surface->id);
		}
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerEntered") == 0 || strcmp(name, "pointerMoved") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		wl_fixed_t x = input_fixed_from(json_double(body, "x", 0));
		wl_fixed_t y = input_fixed_from(json_double(body, "y", 0));
		handle_pointer_position(server, window_id, x, y);
		return;
	}

	if (strcmp(name, "pointerLeft") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		if (server->drag_source && !server->drag_dropped &&
		    server->drag_focus_window == window_id) {
			np_data_drag_leave(server, window_id);
			server->drag_focus_window = 0;
		}
		/* A core pointer implicit grab retains focus until the button release,
		 * even when AppKit reports that the pointer left the view mid-drag. */
		if (server->pointer_buttons != 0 && !server->drag_source) return;
		// Ignore a delayed exit from the window that lost focus during the
		// leave-before-enter repair above.
		if (server->pointer_window != window_id) return;
		clear_pointer_focus(server);
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerButton") == 0) {
		uint32_t window_id = (uint32_t)json_int(body, "window", 0);
		cJSON *which = cJSON_GetObjectItemCaseSensitive(body, "button");
		uint32_t code = BTN_LEFT;
		uint32_t button = 1u;
		if (cJSON_IsString(which)) {
			if (strcmp(which->valuestring, "right") == 0) {
				code = BTN_RIGHT;
				button = 2u;
			} else if (strcmp(which->valuestring, "middle") == 0) {
				code = BTN_MIDDLE;
				button = 4u;
			}
		}
		bool pressed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "pressed"));
		struct np_surface *surface = np_surface_by_id(
			server, server->pointer_surface);
		if (pressed && !surface) return;
		if (pressed)
			server->pointer_buttons |= button;
		else
			server->pointer_buttons &= ~button;
		if (!pressed && server->pointer_buttons == 0)
			server->pointer_grab_client = NULL;

		/* A DnD grab has no ordinary wl_pointer focus.  End it after the
		 * final physical button is released without inventing a core pointer
		 * release for the drop target. */
		if (!pressed && server->drag_source &&
		    server->pointer_buttons == 0 && !server->drag_dropped) {
			np_data_drag_finish(server);
			wl_display_flush_clients(server->display);
			return;
		}
		if (!surface) return;

		struct np_input *entry;
		uint32_t serial = wl_display_next_serial(server->display);
		if (pressed) {
			server->pointer_grab_serial = serial;
			server->pointer_grab_client = wl_resource_get_client(surface->resource);
			server->last_input_serial = serial;
			server->last_input_client = wl_resource_get_client(surface->resource);
		}
		uint32_t time = input_now_ms();
		wl_list_for_each(entry, &server->pointers, link) {
			if (!same_client(entry->resource, surface->resource)) continue;
			wl_pointer_send_button(entry->resource, serial, time, code,
			                       pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			                               : WL_POINTER_BUTTON_STATE_RELEASED);
			pointer_frame(entry->resource);
		}
		if (!pressed && server->pointer_buttons == 0) {
			handle_pointer_position(
				server, window_id, input_fixed_from(server->pointer_x),
				input_fixed_from(server->pointer_y));
			return;
		}
		wl_display_flush_clients(server->display);
		return;
	}

	if (strcmp(name, "pointerScroll") == 0) {
		handle_pointer_scroll(
			server, (uint32_t)json_int(body, "window", 0),
			json_double(body, "dx", 0), json_double(body, "dy", 0));
		return;
	}

	if (strcmp(name, "scaleChanged") == 0) {
		int scale = json_int(body, "scale", 0);
		if (scale <= 0) return;

		// Per surface, because that is what the host is telling us: this window
		// is now on a display of this density. Applying it to wl_output would
		// change every window at once, which is wrong the moment two of them sit
		// on screens that differ.
		struct np_surface *root = np_surface_by_window(
			server, (uint32_t)json_int(body, "window", 0));
		if (root) {
			struct np_surface *surface;
			wl_list_for_each(surface, &server->surfaces, link) {
				if (np_scene_root(surface) == root)
					np_scale_changed(surface, scale);
			}
			if (server->pointer_window == root->window_id &&
			    server->cursor_surface) {
				struct np_surface *cursor = wl_resource_get_user_data(
					server->cursor_surface);
				np_scale_changed(cursor, scale);
			}
			if (server->drag_focus_window == root->window_id &&
			    server->drag_icon) {
				struct np_surface *icon = wl_resource_get_user_data(
					server->drag_icon);
				np_scale_changed(icon, scale);
			}
		}

		wl_display_flush_clients(server->display);
		return;
	}
}
