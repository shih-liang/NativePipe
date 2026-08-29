// Rootless Xwayland socket activation and the narrow XWM bridge NativePipe needs.

#define _GNU_SOURCE

#include "xwayland.h"

#include "compositor_internal.h"
#include "hostlink.h"
#include "window_events.h"
#include "xdg_shell.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

enum property_query_kind {
	QUERY_NET_WM_NAME,
	QUERY_WM_NAME,
	QUERY_WM_CLASS,
	QUERY_TRANSIENT_FOR,
	QUERY_NORMAL_HINTS,
};

struct np_xwindow {
	struct wl_list link;
	xcb_window_t id;
	uint32_t surface_id;
	struct np_surface *surface;
	int16_t x, y;
	uint16_t width, height;
	bool override_redirect;
};

struct np_xquery {
	struct wl_list link;
	unsigned int sequence;
	xcb_window_t window;
	enum property_query_kind kind;
};

struct np_xwayland {
	struct np_server *server;
	char program[256];
	int display;
	int listen_fd[2];
	char socket_path[108];
	char auth_path[256];
	struct wl_event_source *listen_source[2];
	struct wl_event_source *ready_source;
	struct wl_event_source *xcb_source;
	struct wl_event_source *sigchld_source;
	int ready_fd;
	int wm_fd;
	pid_t pid;
	struct wl_client *wayland_client;
	struct wl_listener wayland_client_destroy;
	xcb_connection_t *xcb;
	xcb_screen_t *screen;
	xcb_atom_t wl_surface_id;
	xcb_atom_t net_wm_name;
	xcb_atom_t utf8_string;
	xcb_atom_t wm_protocols;
	xcb_atom_t wm_delete_window;
	xcb_window_t focused;
	struct wl_list windows;
	struct wl_list queries;
};

static bool write_all(int fd, const void *bytes, size_t length)
{
	const unsigned char *cursor = bytes;
	while (length) {
		ssize_t written = write(fd, cursor, length);
		if (written < 0 && errno == EINTR) continue;
		if (written <= 0) return false;
		cursor += written;
		length -= (size_t)written;
	}
	return true;
}

static bool write_be16(int fd, uint16_t value)
{
	unsigned char bytes[] = {(unsigned char)(value >> 8), (unsigned char)value};
	return write_all(fd, bytes, sizeof(bytes));
}

static bool random_bytes(void *bytes, size_t length)
{
	unsigned char *cursor = bytes;
	while (length) {
		ssize_t count = getrandom(cursor, length, 0);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) return false;
		cursor += count;
		length -= (size_t)count;
	}
	return true;
}

/* Xauthority's wire format is big-endian counted fields. FamilyWild keeps the
 * cookie valid for both the filesystem and abstract Unix listeners without
 * publishing the guest hostname. The display number still has to match. */
static bool create_auth_file(struct np_xwayland *xw)
{
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	static const char name[] = "MIT-MAGIC-COOKIE-1";
	unsigned char cookie[16];
	char display[16];
	if (!runtime || runtime[0] != '/' ||
	    snprintf(display, sizeof(display), "%d", xw->display) >=
	        (int)sizeof(display) ||
	    snprintf(xw->auth_path, sizeof(xw->auth_path),
	             "%s/.nativepipe-Xauthority-XXXXXX", runtime) >=
	        (int)sizeof(xw->auth_path) ||
	    !random_bytes(cookie, sizeof(cookie)))
		return false;
	int fd = mkstemp(xw->auth_path);
	if (fd < 0) return false;
	bool ok = fchmod(fd, 0600) == 0 &&
	          write_be16(fd, UINT16_MAX) && /* FamilyWild */
	          write_be16(fd, 0) &&          /* address */
	          write_be16(fd, (uint16_t)strlen(display)) &&
	          write_all(fd, display, strlen(display)) &&
	          write_be16(fd, (uint16_t)(sizeof(name) - 1)) &&
	          write_all(fd, name, sizeof(name) - 1) &&
	          write_be16(fd, sizeof(cookie)) &&
	          write_all(fd, cookie, sizeof(cookie)) && fsync(fd) == 0;
	if (close(fd) < 0) ok = false;
	if (!ok) {
		unlink(xw->auth_path);
		xw->auth_path[0] = '\0';
	}
	return ok;
}

static void xwayland_runtime_stopped(struct np_xwayland *xw);
static void add_listen_sources(struct np_xwayland *xw);

static struct np_xwindow *xwindow_for_id(struct np_xwayland *xw, xcb_window_t id)
{
	struct np_xwindow *window;
	wl_list_for_each(window, &xw->windows, link) {
		if (window->id == id) return window;
	}
	return NULL;
}

static struct np_xwindow *xwindow_for_surface(
	struct np_xwayland *xw, const struct np_surface *surface)
{
	if (!surface || !surface->xwayland_window) return NULL;
	return xwindow_for_id(xw, surface->xwayland_window);
}

static struct np_xwindow *ensure_xwindow(struct np_xwayland *xw, xcb_window_t id)
{
	struct np_xwindow *window = xwindow_for_id(xw, id);
	if (window) return window;
	window = calloc(1, sizeof(*window));
	if (!window) return NULL;
	window->id = id;
	window->width = window->height = 1;
	wl_list_insert(xw->windows.prev, &window->link);
	return window;
}

static xcb_atom_t intern_atom(struct np_xwayland *xw, const char *name)
{
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(
		xw->xcb, 0, (uint16_t)strlen(name), name);
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(xw->xcb, cookie, NULL);
	if (!reply) return XCB_ATOM_NONE;
	xcb_atom_t atom = reply->atom;
	free(reply);
	return atom;
}

static void send_parent(struct np_surface *surface, struct np_surface *parent)
{
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	if (parent) cJSON_AddNumberToObject(body, "parent", parent->window_id);
	else cJSON_AddNullToObject(body, "parent");
	np_host_send(&surface->server->host, "parentChanged", body);
}

static void send_server_decoration(struct np_surface *surface)
{
	surface->decoration_negotiated = true;
	surface->decoration_server_side = true;
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "window", surface->window_id);
	cJSON_AddBoolToObject(body, "serverSide", true);
	np_host_send(&surface->server->host, "decorationModeChanged", body);
}

static void queue_property(
	struct np_xwayland *xw, struct np_xwindow *window,
	enum property_query_kind kind, xcb_atom_t property, xcb_atom_t type)
{
	if (!xw->xcb || !window || property == XCB_ATOM_NONE) return;
	struct np_xquery *existing;
	wl_list_for_each(existing, &xw->queries, link) {
		if (existing->window == window->id && existing->kind == kind) return;
	}
	xcb_get_property_cookie_t cookie = xcb_get_property_unchecked(
		xw->xcb, 0, window->id, property, type, 0, 1024);
	struct np_xquery *query = calloc(1, sizeof(*query));
	if (!query) return;
	query->sequence = cookie.sequence;
	query->window = window->id;
	query->kind = kind;
	wl_list_insert(xw->queries.prev, &query->link);
}

static void query_window_properties(struct np_xwayland *xw, struct np_xwindow *window)
{
	queue_property(xw, window, QUERY_NET_WM_NAME,
	               xw->net_wm_name, xw->utf8_string);
	queue_property(xw, window, QUERY_WM_NAME,
	               XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
	queue_property(xw, window, QUERY_WM_CLASS,
	               XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
	queue_property(xw, window, QUERY_TRANSIENT_FOR,
	               XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW);
	queue_property(xw, window, QUERY_NORMAL_HINTS,
	               XCB_ATOM_WM_NORMAL_HINTS, XCB_GET_PROPERTY_TYPE_ANY);
	xcb_flush(xw->xcb);
}

static void associate_surface(struct np_xwayland *xw, struct np_xwindow *window)
{
	if (!xw->wayland_client || !window || !window->surface_id || window->surface)
		return;
	struct wl_resource *resource = wl_client_get_object(
		xw->wayland_client, window->surface_id);
	if (!resource || strcmp(wl_resource_get_class(resource), "wl_surface") != 0)
		return;
	struct np_surface *surface = wl_resource_get_user_data(resource);
	if (!surface || surface->resource != resource || surface->xwayland_window ||
	    !np_surface_assign_role(surface, NP_SURFACE_ROLE_XWAYLAND))
		return;

	window->surface = surface;
	surface->xwayland_window = window->id;
	surface->xwayland_popup = window->override_redirect;
	surface->window_id = surface->server->next_id++;
	surface->mapped = false;

	if (surface->xwayland_popup) {
		struct np_surface *parent = surface->server->focused_window
			? np_surface_by_window(surface->server, surface->server->focused_window)
			: NULL;
		surface->popup_parent_window = parent ? parent->window_id : 0;
		int32_t px = window->x;
		int32_t py = window->y;
		if (parent && parent->xwayland_window) {
			struct np_xwindow *parent_window = xwindow_for_surface(xw, parent);
			if (parent_window) {
				px -= parent_window->x;
				py -= parent_window->y;
			}
		}
		surface->popup_x = px;
		surface->popup_y = py;
		surface->popup_width = window->width;
		surface->popup_height = window->height;
		uint32_t fields[] = {
			surface->window_id, surface->id, surface->popup_parent_window,
			(uint32_t)surface->popup_x, (uint32_t)surface->popup_y,
			(uint32_t)surface->popup_width, (uint32_t)surface->popup_height,
		};
		np_window_event_send(surface->server, NP_GUEST_POPUP_CREATED, fields, 7);
	} else {
		uint32_t fields[] = {surface->window_id, surface->id};
		np_window_event_send(surface->server, NP_GUEST_TOPLEVEL_CREATED, fields, 2);
		send_server_decoration(surface);
	}
	np_window_set_title(surface, "X11 Application");
	query_window_properties(xw, window);
}

static void publish_window_destroyed(struct np_xwindow *window)
{
	struct np_surface *surface = window->surface;
	if (!surface || !surface->xwayland_window) return;
	uint32_t fields[] = {surface->window_id};
	np_window_event_send(
		surface->server,
		surface->xwayland_popup ? NP_GUEST_POPUP_DESTROYED
		                        : NP_GUEST_TOPLEVEL_DESTROYED,
		fields, 1);
	surface->xwayland_window = 0;
	window->surface = NULL;
}

static void remove_xwindow(struct np_xwindow *window)
{
	if (!window) return;
	publish_window_destroyed(window);
	wl_list_remove(&window->link);
	free(window);
}

static char *property_string(xcb_get_property_reply_t *reply)
{
	int length = xcb_get_property_value_length(reply);
	if (length <= 0) return NULL;
	if (length > 4096) length = 4096;
	const unsigned char *source = xcb_get_property_value(reply);
	char *text = calloc(1, (size_t)length + 1);
	if (!text) return NULL;
	for (int i = 0; i < length && source[i]; i++)
		text[i] = source[i] < 0x20 && source[i] != '\t' ? ' ' : (char)source[i];
	return text;
}

static void process_property_reply(
	struct np_xwayland *xw, struct np_xquery *query,
	xcb_get_property_reply_t *reply)
{
	struct np_xwindow *window = xwindow_for_id(xw, query->window);
	if (!window || !window->surface || !reply) return;
	struct np_surface *surface = window->surface;
	if (query->kind == QUERY_NET_WM_NAME || query->kind == QUERY_WM_NAME) {
		char *title = property_string(reply);
		if (title && title[0] &&
		    (query->kind == QUERY_NET_WM_NAME || !surface->title ||
		     strcmp(surface->title, "X11 Application") == 0))
			np_window_set_title(surface, title);
		free(title);
		return;
	}
	if (query->kind == QUERY_WM_CLASS) {
		int length = xcb_get_property_value_length(reply);
		const char *value = xcb_get_property_value(reply);
		if (length <= 0 || !value) return;
		size_t instance_len = strnlen(value, (size_t)length);
		const char *source = value;
		size_t source_len = instance_len;
		if (instance_len + 1 < (size_t)length) {
			source = value + instance_len + 1;
			source_len = strnlen(source, (size_t)length - instance_len - 1);
		}
		if (source_len) {
			char *app_id = strndup(source, source_len);
			if (app_id) {
				np_window_set_app_id(surface, app_id);
				free(app_id);
			}
		}
		return;
	}
	if (query->kind == QUERY_TRANSIENT_FOR && reply->format == 32 &&
	    xcb_get_property_value_length(reply) >= 4) {
		xcb_window_t parent_id = *(xcb_window_t *)xcb_get_property_value(reply);
		struct np_xwindow *parent = xwindow_for_id(xw, parent_id);
		send_parent(surface, parent ? parent->surface : NULL);
		return;
	}
	if (query->kind == QUERY_NORMAL_HINTS && reply->format == 32) {
		int bytes = xcb_get_property_value_length(reply);
		if (bytes < 9 * 4) return;
		const uint32_t *hints = xcb_get_property_value(reply);
		const uint32_t p_min_size = 1u << 4;
		const uint32_t p_max_size = 1u << 5;
		np_xdg_apply_size_constraints(
			surface,
			(hints[0] & p_min_size) ? (int32_t)hints[5] : 0,
			(hints[0] & p_min_size) ? (int32_t)hints[6] : 0,
			(hints[0] & p_max_size) ? (int32_t)hints[7] : 0,
			(hints[0] & p_max_size) ? (int32_t)hints[8] : 0);
	}
}

static void process_property_replies(struct np_xwayland *xw)
{
	struct np_xquery *query, *next;
	wl_list_for_each_safe(query, next, &xw->queries, link) {
		void *reply = NULL;
		xcb_generic_error_t *error = NULL;
		if (!xcb_poll_for_reply(xw->xcb, query->sequence, &reply, &error))
			continue;
		if (!error)
			process_property_reply(xw, query, reply);
		free(error);
		free(reply);
		wl_list_remove(&query->link);
		free(query);
	}
}

static void handle_configure_request(
	struct np_xwayland *xw, xcb_configure_request_event_t *event)
{
	uint16_t mask = event->value_mask;
	uint32_t values[7];
	int count = 0;
	if (mask & XCB_CONFIG_WINDOW_X) values[count++] = (uint32_t)event->x;
	if (mask & XCB_CONFIG_WINDOW_Y) values[count++] = (uint32_t)event->y;
	if (mask & XCB_CONFIG_WINDOW_WIDTH) values[count++] = event->width;
	if (mask & XCB_CONFIG_WINDOW_HEIGHT) values[count++] = event->height;
	if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[count++] = event->border_width;
	if (mask & XCB_CONFIG_WINDOW_SIBLING) values[count++] = event->sibling;
	if (mask & XCB_CONFIG_WINDOW_STACK_MODE) values[count++] = event->stack_mode;
	xcb_configure_window(xw->xcb, event->window, mask, values);
	struct np_xwindow *window = ensure_xwindow(xw, event->window);
	if (!window) return;
	if (mask & XCB_CONFIG_WINDOW_X) window->x = event->x;
	if (mask & XCB_CONFIG_WINDOW_Y) window->y = event->y;
	if (mask & XCB_CONFIG_WINDOW_WIDTH) window->width = event->width;
	if (mask & XCB_CONFIG_WINDOW_HEIGHT) window->height = event->height;
}

static void handle_xevent(struct np_xwayland *xw, xcb_generic_event_t *generic)
{
	switch (generic->response_type & 0x7f) {
	case XCB_CREATE_NOTIFY: {
		xcb_create_notify_event_t *event = (void *)generic;
		struct np_xwindow *window = ensure_xwindow(xw, event->window);
		if (!window) break;
		window->x = event->x;
		window->y = event->y;
		window->width = event->width;
		window->height = event->height;
		window->override_redirect = event->override_redirect;
		break;
	}
	case XCB_MAP_REQUEST: {
		xcb_map_request_event_t *event = (void *)generic;
		ensure_xwindow(xw, event->window);
		xcb_map_window(xw->xcb, event->window);
		break;
	}
	case XCB_MAP_NOTIFY: {
		xcb_map_notify_event_t *event = (void *)generic;
		struct np_xwindow *window = ensure_xwindow(xw, event->window);
		if (window) window->override_redirect = event->override_redirect;
		break;
	}
	case XCB_CONFIGURE_REQUEST:
		handle_configure_request(xw, (void *)generic);
		break;
	case XCB_CONFIGURE_NOTIFY: {
		xcb_configure_notify_event_t *event = (void *)generic;
		struct np_xwindow *window = ensure_xwindow(xw, event->window);
		if (!window) break;
		window->x = event->x;
		window->y = event->y;
		window->width = event->width;
		window->height = event->height;
		break;
	}
	case XCB_CLIENT_MESSAGE: {
		xcb_client_message_event_t *event = (void *)generic;
		if (event->type != xw->wl_surface_id || event->format != 32) break;
		struct np_xwindow *window = ensure_xwindow(xw, event->window);
		if (!window) break;
		window->surface_id = event->data.data32[0];
		associate_surface(xw, window);
		break;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *event = (void *)generic;
		struct np_xwindow *window = ensure_xwindow(xw, event->window);
		if (!window || event->state == XCB_PROPERTY_DELETE) break;
		if (event->atom == xw->net_wm_name)
			queue_property(xw, window, QUERY_NET_WM_NAME,
			               xw->net_wm_name, xw->utf8_string);
		else if (event->atom == XCB_ATOM_WM_NAME)
			queue_property(xw, window, QUERY_WM_NAME,
			               XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
		else if (event->atom == XCB_ATOM_WM_CLASS)
			queue_property(xw, window, QUERY_WM_CLASS,
			               XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
		else if (event->atom == XCB_ATOM_WM_TRANSIENT_FOR)
			queue_property(xw, window, QUERY_TRANSIENT_FOR,
			               XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW);
		else if (event->atom == XCB_ATOM_WM_NORMAL_HINTS)
			queue_property(xw, window, QUERY_NORMAL_HINTS,
			               XCB_ATOM_WM_NORMAL_HINTS, XCB_GET_PROPERTY_TYPE_ANY);
		break;
	}
	case XCB_DESTROY_NOTIFY: {
		xcb_destroy_notify_event_t *event = (void *)generic;
		remove_xwindow(xwindow_for_id(xw, event->window));
		break;
	}
	default:
		break;
	}
}

static int xcb_readable(int fd, uint32_t mask, void *data)
{
	(void)fd;
	struct np_xwayland *xw = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) return 0;
	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event(xw->xcb))) {
		handle_xevent(xw, event);
		free(event);
	}
	process_property_replies(xw);
	xcb_flush(xw->xcb);
	return 0;
}

static void wayland_client_destroyed(struct wl_listener *listener, void *data)
{
	(void)data;
	struct np_xwayland *xw = wl_container_of(
		listener, xw, wayland_client_destroy);
	xw->wayland_client = NULL;
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

static bool setup_xwm(struct np_xwayland *xw)
{
	xw->xcb = xcb_connect_to_fd(xw->wm_fd, NULL);
	xw->wm_fd = -1;
	if (!xw->xcb || xcb_connection_has_error(xw->xcb)) return false;
	xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(xw->xcb));
	if (!screens.rem) return false;
	xw->screen = screens.data;
	xw->wl_surface_id = intern_atom(xw, "WL_SURFACE_ID");
	xw->net_wm_name = intern_atom(xw, "_NET_WM_NAME");
	xw->utf8_string = intern_atom(xw, "UTF8_STRING");
	xw->wm_protocols = intern_atom(xw, "WM_PROTOCOLS");
	xw->wm_delete_window = intern_atom(xw, "WM_DELETE_WINDOW");
	if (xw->wl_surface_id == XCB_ATOM_NONE) return false;
	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
	                XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
	                XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(
		xw->xcb, xw->screen->root, XCB_CW_EVENT_MASK, &mask);
	xcb_flush(xw->xcb);
	struct wl_event_loop *loop = wl_display_get_event_loop(xw->server->display);
	xw->xcb_source = wl_event_loop_add_fd(
		loop, xcb_get_file_descriptor(xw->xcb),
		WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		xcb_readable, xw);
	return xw->xcb_source != NULL;
}

static int xwayland_ready(int fd, uint32_t mask, void *data)
{
	struct np_xwayland *xw = data;
	char buffer[32];
	ssize_t count = read(fd, buffer, sizeof(buffer));
	if (count <= 0 && !(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))) return 0;
	if (xw->ready_source) wl_event_source_remove(xw->ready_source);
	xw->ready_source = NULL;
	close(xw->ready_fd);
	xw->ready_fd = -1;
	if (count <= 0 || !setup_xwm(xw)) {
		fprintf(stderr, "[xwayland] could not establish XWM connection\n");
		if (xw->pid > 0) kill(xw->pid, SIGTERM);
	} else {
		fprintf(stderr, "[xwayland] ready on %s\n", xw->server->xwayland_display);
	}
	return 0;
}

static void remove_listen_sources(struct np_xwayland *xw)
{
	for (int i = 0; i < 2; i++) {
		if (xw->listen_source[i]) wl_event_source_remove(xw->listen_source[i]);
		xw->listen_source[i] = NULL;
	}
}

static int start_xwayland(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct np_xwayland *xw = data;
	if (xw->pid > 0) return 0;
	remove_listen_sources(xw);

	int wayland_pair[2] = {-1, -1};
	int wm_pair[2] = {-1, -1};
	int ready_pipe[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wayland_pair) < 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm_pair) < 0 ||
	    pipe2(ready_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
		fprintf(stderr, "[xwayland] socket setup failed: %s\n", strerror(errno));
		for (int i = 0; i < 2; i++) {
			if (wayland_pair[i] >= 0) close(wayland_pair[i]);
			if (wm_pair[i] >= 0) close(wm_pair[i]);
			if (ready_pipe[i] >= 0) close(ready_pipe[i]);
		}
		add_listen_sources(xw);
		return 0;
	}
	xw->wayland_client = wl_client_create(xw->server->display, wayland_pair[0]);
	if (!xw->wayland_client) {
		close(wayland_pair[0]); close(wayland_pair[1]);
		close(wm_pair[0]); close(wm_pair[1]);
		close(ready_pipe[0]); close(ready_pipe[1]);
		add_listen_sources(xw);
		return 0;
	}
	wl_list_init(&xw->wayland_client_destroy.link);
	xw->wayland_client_destroy.notify = wayland_client_destroyed;
	wl_client_add_destroy_listener(xw->wayland_client, &xw->wayland_client_destroy);

	pid_t pid = fork();
	if (pid == 0) {
		close(wayland_pair[0]);
		close(wm_pair[0]);
		close(ready_pipe[0]);
		for (int i = 0; i < 2; i++) fcntl(xw->listen_fd[i], F_SETFD, 0);
		fcntl(wayland_pair[1], F_SETFD, 0);
		fcntl(wm_pair[1], F_SETFD, 0);
		fcntl(ready_pipe[1], F_SETFD, 0);
		char wayland_fd[24], wm_fd[24], ready_fd[24], listen0[24], listen1[24];
		snprintf(wayland_fd, sizeof(wayland_fd), "%d", wayland_pair[1]);
		snprintf(wm_fd, sizeof(wm_fd), "%d", wm_pair[1]);
		snprintf(ready_fd, sizeof(ready_fd), "%d", ready_pipe[1]);
		snprintf(listen0, sizeof(listen0), "%d", xw->listen_fd[0]);
		snprintf(listen1, sizeof(listen1), "%d", xw->listen_fd[1]);
		setenv("WAYLAND_SOCKET", wayland_fd, 1);
		execl(xw->program, "Xwayland", xw->server->xwayland_display,
		      "-rootless", "-terminate", "-nolisten", "tcp",
		      "-auth", xw->auth_path,
		      "-listenfd", listen0, "-listenfd", listen1,
		      "-displayfd", ready_fd, "-wm", wm_fd, (char *)NULL);
		_exit(127);
	}
	close(wayland_pair[1]);
	close(wm_pair[1]);
	close(ready_pipe[1]);
	if (pid < 0) {
		close(wm_pair[0]);
		close(ready_pipe[0]);
		wl_client_destroy(xw->wayland_client);
		add_listen_sources(xw);
		return 0;
	}
	xw->pid = pid;
	xw->wm_fd = wm_pair[0];
	xw->ready_fd = ready_pipe[0];
	struct wl_event_loop *loop = wl_display_get_event_loop(xw->server->display);
	xw->ready_source = wl_event_loop_add_fd(
		loop, xw->ready_fd,
		WL_EVENT_READABLE | WL_EVENT_HANGUP | WL_EVENT_ERROR,
		xwayland_ready, xw);
	return 0;
}

static int make_unix_listener(const char *path, bool abstract)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return -1;
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	socklen_t length;
	if (abstract) {
		size_t size = strlen(path);
		if (size + 1 >= sizeof(address.sun_path)) { close(fd); return -1; }
		memcpy(address.sun_path + 1, path, size);
		length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + size);
	} else {
		if (strlen(path) >= sizeof(address.sun_path)) { close(fd); return -1; }
		strcpy(address.sun_path, path);
		length = sizeof(address);
	}
	if (bind(fd, (struct sockaddr *)&address, length) < 0 || listen(fd, 32) < 0) {
		close(fd);
		return -1;
	}
	if (!abstract) chmod(path, 0777);
	return fd;
}

static bool reserve_display(struct np_xwayland *xw)
{
	if (mkdir("/tmp/.X11-unix", 01777) < 0 && errno != EEXIST) return false;
	chmod("/tmp/.X11-unix", 01777);
	for (int display = 0; display < 64; display++) {
		char path[108];
		snprintf(path, sizeof(path), "/tmp/.X11-unix/X%d", display);
		if (access(path, F_OK) == 0) continue;
		int filesystem = make_unix_listener(path, false);
		if (filesystem < 0) continue;
		int abstract = make_unix_listener(path, true);
		if (abstract < 0) {
			close(filesystem);
			unlink(path);
			continue;
		}
		xw->display = display;
		xw->listen_fd[0] = filesystem;
		xw->listen_fd[1] = abstract;
		snprintf(xw->socket_path, sizeof(xw->socket_path), "%s", path);
		snprintf(xw->server->xwayland_display,
		         sizeof(xw->server->xwayland_display), ":%d", display);
		return true;
	}
	return false;
}

static void add_listen_sources(struct np_xwayland *xw)
{
	struct wl_event_loop *loop = wl_display_get_event_loop(xw->server->display);
	for (int i = 0; i < 2; i++) {
		if (xw->listen_fd[i] >= 0 && !xw->listen_source[i])
			xw->listen_source[i] = wl_event_loop_add_fd(
				loop, xw->listen_fd[i], WL_EVENT_READABLE,
				start_xwayland, xw);
	}
}

static void clear_runtime_objects(struct np_xwayland *xw)
{
	if (xw->ready_source) wl_event_source_remove(xw->ready_source);
	xw->ready_source = NULL;
	if (xw->ready_fd >= 0) close(xw->ready_fd);
	xw->ready_fd = -1;
	if (xw->xcb_source) wl_event_source_remove(xw->xcb_source);
	xw->xcb_source = NULL;
	if (xw->xcb) xcb_disconnect(xw->xcb);
	xw->xcb = NULL;
	xw->screen = NULL;
	if (xw->wm_fd >= 0) close(xw->wm_fd);
	xw->wm_fd = -1;
	if (xw->wayland_client) wl_client_destroy(xw->wayland_client);
	xw->wayland_client = NULL;
	struct np_xquery *query, *query_next;
	wl_list_for_each_safe(query, query_next, &xw->queries, link) {
		wl_list_remove(&query->link);
		free(query);
	}
	struct np_xwindow *window, *window_next;
	wl_list_for_each_safe(window, window_next, &xw->windows, link)
		remove_xwindow(window);
	xw->focused = XCB_WINDOW_NONE;
}

static void xwayland_runtime_stopped(struct np_xwayland *xw)
{
	clear_runtime_objects(xw);
	xw->pid = -1;
	for (int i = 0; i < 2; i++) {
		if (xw->listen_fd[i] >= 0) close(xw->listen_fd[i]);
		xw->listen_fd[i] = -1;
	}
	unlink(xw->socket_path);
	xw->server->xwayland_display[0] = '\0';
	if (reserve_display(xw)) add_listen_sources(xw);
}

static int sigchld_received(int signal_number, void *data)
{
	(void)signal_number;
	struct np_xwayland *xw = data;
	if (xw->pid <= 0) return 0;
	int status;
	pid_t result = waitpid(xw->pid, &status, WNOHANG);
	if (result == xw->pid) {
		fprintf(stderr, "[xwayland] exited status=%d\n", status);
		xwayland_runtime_stopped(xw);
	}
	return 0;
}

bool np_xwayland_init(struct np_server *server)
{
	static const char *programs[] = {
		"/usr/bin/Xwayland", "/bin/Xwayland", "/usr/local/bin/Xwayland", NULL};
	const char *program = NULL;
	for (size_t i = 0; programs[i]; i++) {
		if (access(programs[i], X_OK) == 0) { program = programs[i]; break; }
	}
	if (!program) return false;
	struct np_xwayland *xw = calloc(1, sizeof(*xw));
	if (!xw) return false;
	xw->server = server;
	snprintf(xw->program, sizeof(xw->program), "%s", program);
	xw->listen_fd[0] = xw->listen_fd[1] = -1;
	xw->ready_fd = xw->wm_fd = -1;
	xw->pid = -1;
	wl_list_init(&xw->windows);
	wl_list_init(&xw->queries);
	server->xwayland = xw;
	if (!reserve_display(xw) || !create_auth_file(xw)) {
		np_xwayland_finish(server);
		return false;
	}
	snprintf(server->xwayland_auth, sizeof(server->xwayland_auth), "%s",
	         xw->auth_path);
	add_listen_sources(xw);
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	xw->sigchld_source = wl_event_loop_add_signal(loop, SIGCHLD, sigchld_received, xw);
	fprintf(stderr, "[xwayland] lazy display reserved at %s\n", server->xwayland_display);
	return true;
}

void np_xwayland_finish(struct np_server *server)
{
	struct np_xwayland *xw = server ? server->xwayland : NULL;
	if (!xw) return;
	remove_listen_sources(xw);
	if (xw->sigchld_source) wl_event_source_remove(xw->sigchld_source);
	if (xw->pid > 0) {
		kill(xw->pid, SIGTERM);
		waitpid(xw->pid, NULL, 0);
	}
	clear_runtime_objects(xw);
	for (int i = 0; i < 2; i++) if (xw->listen_fd[i] >= 0) close(xw->listen_fd[i]);
	unlink(xw->socket_path);
	unlink(xw->auth_path);
	server->xwayland = NULL;
	server->xwayland_display[0] = '\0';
	server->xwayland_auth[0] = '\0';
	free(xw);
}

void np_xwayland_surface_created(struct np_server *server, struct np_surface *surface)
{
	(void)surface;
	struct np_xwayland *xw = server ? server->xwayland : NULL;
	if (!xw || !xw->wayland_client) return;
	struct np_xwindow *window;
	wl_list_for_each(window, &xw->windows, link) associate_surface(xw, window);
}

void np_xwayland_surface_destroyed(struct np_server *server, struct np_surface *surface)
{
	struct np_xwayland *xw = server ? server->xwayland : NULL;
	struct np_xwindow *window = xw ? xwindow_for_surface(xw, surface) : NULL;
	if (window) window->surface = NULL;
}

void np_xwayland_configure(
	struct np_surface *surface, int32_t width, int32_t height, uint32_t state_bits)
{
	(void)state_bits;
	struct np_xwayland *xw = surface && surface->server
		? surface->server->xwayland : NULL;
	struct np_xwindow *window = xw ? xwindow_for_surface(xw, surface) : NULL;
	if (!window || !xw->xcb || width <= 0 || height <= 0) return;
	uint32_t values[] = {(uint32_t)width, (uint32_t)height};
	xcb_configure_window(
		xw->xcb, window->id,
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
	window->width = (uint16_t)(width > 65535 ? 65535 : width);
	window->height = (uint16_t)(height > 65535 ? 65535 : height);
	xcb_flush(xw->xcb);
}

void np_xwayland_set_focus(struct np_server *server, struct np_surface *surface)
{
	struct np_xwayland *xw = server ? server->xwayland : NULL;
	if (!xw || !xw->xcb || !xw->screen) return;
	xcb_window_t target = surface && surface->xwayland_window
		? surface->xwayland_window : xw->screen->root;
	if (xw->focused == target) return;
	xw->focused = target;
	xcb_set_input_focus(
		xw->xcb, XCB_INPUT_FOCUS_POINTER_ROOT, target, XCB_CURRENT_TIME);
	xcb_flush(xw->xcb);
}

void np_xwayland_close(struct np_surface *surface)
{
	struct np_xwayland *xw = surface && surface->server
		? surface->server->xwayland : NULL;
	if (!xw || !xw->xcb || !surface->xwayland_window) return;
	if (xw->wm_protocols == XCB_ATOM_NONE || xw->wm_delete_window == XCB_ATOM_NONE) {
		xcb_kill_client(xw->xcb, surface->xwayland_window);
	} else {
		xcb_client_message_event_t event;
		memset(&event, 0, sizeof(event));
		event.response_type = XCB_CLIENT_MESSAGE;
		event.format = 32;
		event.window = surface->xwayland_window;
		event.type = xw->wm_protocols;
		event.data.data32[0] = xw->wm_delete_window;
		event.data.data32[1] = XCB_CURRENT_TIME;
		xcb_send_event(xw->xcb, 0, surface->xwayland_window,
		               XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
	}
	xcb_flush(xw->xcb);
}

void np_xwayland_force_quit(struct np_surface *surface)
{
	struct np_xwayland *xw = surface && surface->server
		? surface->server->xwayland : NULL;
	if (!xw || !xw->xcb || !surface->xwayland_window) return;
	xcb_kill_client(xw->xcb, surface->xwayland_window);
	xcb_flush(xw->xcb);
}

void np_xwayland_dismiss_popup(struct np_surface *surface)
{
	struct np_xwayland *xw = surface && surface->server
		? surface->server->xwayland : NULL;
	if (!xw || !xw->xcb || !surface->xwayland_window) return;
	xcb_unmap_window(xw->xcb, surface->xwayland_window);
	xcb_flush(xw->xcb);
}
