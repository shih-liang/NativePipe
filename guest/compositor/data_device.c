#include "data_device.h"

#include "compositor_internal.h"
#include "hostlink.h"
#include "scene.h"
#include "window_events.h"
#include "windowwire.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

static wl_fixed_t fixed_from(double value) {
	return wl_fixed_from_double(value);
}


// ---------------------------------------------------------------------------
// wl_data_device_manager
//
// Real clients treat this as mandatory — foot refuses to start without it — so
// a compositor that omits it is not usable, whatever else works. This carries
// the selection between guest clients; bridging it to NSPasteboard is a separate
// job on the host side.
// ---------------------------------------------------------------------------

#define NP_MAX_MIME_TYPES 24

static void send_drag_icon(struct np_server *server, struct wl_resource *icon) {
	struct np_surface *surface = icon ? wl_resource_get_user_data(icon) : NULL;
	uint32_t fields[] = {surface ? surface->id : 0};
	np_window_event_send(server, NP_GUEST_DRAG_ICON_CHANGED, fields, 1);
}

struct np_data_source {
	struct wl_resource *resource;
	struct np_server *server;
	char *mime_types[NP_MAX_MIME_TYPES];
	int mime_count;
	uint32_t actions;
};

struct np_data_offer {
	struct wl_list link;
	struct np_server *server;
	struct wl_resource *resource;
	struct wl_resource *device;
	struct wl_resource *source;
	/// True when the bytes live on the macOS pasteboard, in which case `source`
	/// is NULL and a receive has to make a round trip to the host.
	bool from_host;
	bool dnd;
	bool active;
	bool dropped;
	bool finished;
	char *accepted_mime;
	uint32_t actions;
	uint32_t preferred_action;
	uint32_t chosen_action;
};

static void detach_offers_for_source(struct np_server *server,
	                                  struct wl_resource *source,
	                                  bool finished) {
	if (!server || !source) return;
	struct np_data_offer *offer;
	wl_list_for_each(offer, &server->data_offers, link) {
		if (offer->source != source) continue;
		offer->source = NULL;
		offer->active = false;
		if (finished) offer->finished = true;
	}
}

// Defined with the clipboard transport further down; the selection can change
// from here, when a source is destroyed.
static void announce_selection_to_host(struct np_server *server);

static void data_source_free(struct np_data_source *source) {
	for (int i = 0; i < source->mime_count; i++) free(source->mime_types[i]);
	free(source);
}

static void data_source_offer(struct wl_client *client, struct wl_resource *resource,
                              const char *mime_type) {
	struct np_data_source *source = wl_resource_get_user_data(resource);
	if (!source || source->mime_count >= NP_MAX_MIME_TYPES || !mime_type) return;
	source->mime_types[source->mime_count++] = strdup(mime_type);
}

static void data_source_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void data_source_set_actions(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t actions) {
	struct np_data_source *source = wl_resource_get_user_data(resource);
	if (source) source->actions = actions;
}

static const struct wl_data_source_interface data_source_implementation = {
	.offer = data_source_offer,
	.destroy = data_source_destroy_handler,
	.set_actions = data_source_set_actions,
};

static void data_source_resource_destroy(struct wl_resource *resource) {
	struct np_data_source *source = wl_resource_get_user_data(resource);
	struct np_server *server = source ? source->server : NULL;
	if (server) {
		/* Offers outlive their source resource in the protocol. Detach the weak
		 * resource pointer before libwayland frees it so late destroy/finish
		 * requests cannot address a reused wl_resource. */
		detach_offers_for_source(server, resource, false);
	}
	if (server && server->selection_source == resource) {
		server->selection_source = NULL;
		announce_selection_to_host(server);
	}
	if (server && server->drag_source == resource) {
		if (server->drag_icon) send_drag_icon(server, NULL);
		server->drag_source = NULL;
		server->drag_origin = NULL;
		server->drag_icon = NULL;
		server->drag_focus_window = 0;
		server->drag_dropped = false;
	}
	if (source) data_source_free(source);
}

/// The receiving half: a client asked for the selection in a particular type,
/// and the pipe it handed over goes straight to the owner.
static void clip_request_from_host(struct np_server *server, const char *mime_type, int fd);

static void data_offer_receive(struct wl_client *client, struct wl_resource *resource,
                               const char *mime_type, int32_t fd) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (offer && offer->from_host) {
		clip_request_from_host(offer->server, mime_type, fd);
		return;
	}
	if (offer && offer->source) {
		wl_data_source_send_send(offer->source, mime_type, fd);
	}
	close(fd);
}
static void data_offer_destroy_handler(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}
static void data_offer_resource_destroy(struct wl_resource *resource) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer) return;
	if (offer->dnd && offer->source && !offer->finished &&
	    (offer->active || offer->dropped) &&
	    offer->server->drag_source == offer->source) {
		/* Destroying the current offer before finish is the destination's
		 * cancellation path (notably for the ASK action). Never leave the source
		 * waiting forever for dnd_finished. */
		struct wl_resource *source = offer->source;
		wl_data_source_send_cancelled(source);
		detach_offers_for_source(offer->server, source, false);
		offer->server->drag_source = NULL;
		offer->server->drag_origin = NULL;
		offer->server->drag_focus_window = 0;
		offer->server->drag_dropped = false;
		if (offer->server->drag_icon) {
			send_drag_icon(offer->server, NULL);
			offer->server->drag_icon = NULL;
		}
	}
	wl_list_remove(&offer->link);
	free(offer->accepted_mime);
	free(offer);
}

static bool nullable_string_equal(const char *a, const char *b) {
	if (!a || !b) return a == b;
	return strcmp(a, b) == 0;
}

static void data_offer_accept(struct wl_client *client, struct wl_resource *resource,
                              uint32_t serial, const char *mime_type) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || nullable_string_equal(offer->accepted_mime, mime_type)) return;
	char *accepted = mime_type ? strdup(mime_type) : NULL;
	if (mime_type && !accepted) {
		wl_client_post_no_memory(client);
		return;
	}
	free(offer->accepted_mime);
	offer->accepted_mime = accepted;
	if (offer->source) wl_data_source_send_target(offer->source, mime_type);
}
static void data_offer_finish(struct wl_client *client, struct wl_resource *resource) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || !offer->dnd || !offer->source || !offer->dropped ||
	    !offer->accepted_mime || !offer->chosen_action || offer->finished)
		return;
	struct wl_resource *source = offer->source;
	if (wl_resource_get_version(source) >= 3) {
		wl_data_source_send_dnd_finished(source);
	}
	detach_offers_for_source(offer->server, source, true);
	if (offer->server->drag_source == source) {
		offer->server->drag_source = NULL;
		offer->server->drag_origin = NULL;
		offer->server->drag_icon = NULL;
		offer->server->drag_focus_window = 0;
		offer->server->drag_dropped = false;
	}
}
static void data_offer_set_actions(struct wl_client *client, struct wl_resource *resource,
                                   uint32_t actions, uint32_t preferred) {
	struct np_data_offer *offer = wl_resource_get_user_data(resource);
	if (!offer || !offer->dnd || !offer->source || offer->dropped) return;
	struct np_data_source *source = wl_resource_get_user_data(offer->source);
	if (!source) return;
	offer->actions = actions;
	offer->preferred_action = preferred;
	uint32_t available = actions & source->actions;
	uint32_t chosen = (preferred && (available & preferred)) ? preferred : 0;
	if (!chosen && (available & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY))
		chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	if (!chosen && (available & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE))
		chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	if (!chosen && (available & WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK))
		chosen = WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	/* GTK updates set_actions from every drag motion. Emitting the unchanged
	 * result makes its action callback send set_actions again, creating an
	 * unbounded client/server feedback loop. The protocol describes an action
	 * change notification, so silence is required when negotiation is stable. */
	if (chosen == offer->chosen_action) return;
	offer->chosen_action = chosen;
	wl_data_offer_send_action(resource, chosen);
	if (wl_resource_get_version(offer->source) >= 3) {
		wl_data_source_send_action(offer->source, chosen);
	}
}

static const struct wl_data_offer_interface data_offer_implementation = {
	.accept = data_offer_accept,
	.receive = data_offer_receive,
	.destroy = data_offer_destroy_handler,
	.finish = data_offer_finish,
	.set_actions = data_offer_set_actions,
};

static struct wl_resource *create_data_offer(struct np_server *server,
                                              struct wl_resource *device,
                                              struct wl_resource *source_resource,
                                              bool dnd) {
	struct wl_resource *offer = wl_resource_create(
		wl_resource_get_client(device), &wl_data_offer_interface,
		wl_resource_get_version(device), 0);
	if (!offer) return NULL;
	struct np_data_offer *state = calloc(1, sizeof(*state));
	if (!state) {
		wl_resource_destroy(offer);
		return NULL;
	}
	state->server = server;
	state->resource = offer;
	state->device = device;
	state->source = source_resource;
	state->dnd = dnd;
	wl_list_insert(&server->data_offers, &state->link);
	wl_resource_set_implementation(offer, &data_offer_implementation, state,
	                               data_offer_resource_destroy);
	return offer;
}

static void announce_offer(struct wl_resource *device, struct wl_resource *offer,
                           struct wl_resource *source_resource, bool dnd) {
	struct np_data_source *source = wl_resource_get_user_data(source_resource);
	if (!source) return;
	wl_data_device_send_data_offer(device, offer);
	for (int i = 0; i < source->mime_count; i++) {
		wl_data_offer_send_offer(offer, source->mime_types[i]);
	}
	if (dnd && wl_resource_get_version(offer) >= 3) {
		wl_data_offer_send_source_actions(offer, source->actions);
		wl_data_offer_send_action(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
	}
}


// ---------------------------------------------------------------------------
// Clipboard transport
//
// Wayland moves selection data over a pipe the receiver supplies, and the host
// channel carries JSON. Base64 is the bridge. It costs a third in size, which
// for clipboard payloads is worth not having to add a second binary framing and
// keep the two in step — the same reasoning that kept only pointer motion and
// scroll on the fast path.
// ---------------------------------------------------------------------------

/// A clipboard payload larger than this is refused rather than buffered. The
/// point is to bound what one paste can make the compositor hold, not to
/// support moving disk images through the selection.
#define NP_CLIP_MAX (16u * 1024u * 1024u)

static const char NP_B64_ALPHABET[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *np_base64_encode(const unsigned char *data, size_t len) {
	char *out = malloc(((len + 2) / 3) * 4 + 1);
	if (!out) return NULL;
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len) v |= data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		out[o++] = NP_B64_ALPHABET[(v >> 18) & 0x3f];
		out[o++] = NP_B64_ALPHABET[(v >> 12) & 0x3f];
		out[o++] = i + 1 < len ? NP_B64_ALPHABET[(v >> 6) & 0x3f] : '=';
		out[o++] = i + 2 < len ? NP_B64_ALPHABET[v & 0x3f] : '=';
	}
	out[o] = '\0';
	return out;
}

static unsigned char *np_base64_decode(const char *text, size_t *out_len) {
	static signed char reverse[256];
	static bool built = false;
	if (!built) {
		memset(reverse, -1, sizeof(reverse));
		for (int i = 0; i < 64; i++) reverse[(unsigned char)NP_B64_ALPHABET[i]] = (signed char)i;
		built = true;
	}
	size_t len = strlen(text);
	unsigned char *out = malloc(len / 4 * 3 + 3);
	if (!out) return NULL;
	size_t o = 0;
	unsigned accumulator = 0;
	int bits = 0;
	for (size_t i = 0; i < len; i++) {
		signed char value = reverse[(unsigned char)text[i]];
		if (value < 0) continue;               // '=' and any stray whitespace
		accumulator = (accumulator << 6) | (unsigned)value;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (unsigned char)((accumulator >> bits) & 0xff);
		}
	}
	*out_len = o;
	return out;
}

/// Draining a guest client's selection into a buffer, on its way to the host.
struct np_clip_read {
	struct wl_list link;
	struct np_server *server;
	uint32_t token;
	char *mime;
	int fd;
	struct wl_event_source *source;
	unsigned char *data;
	size_t len, cap;
};

/// Pushing the macOS pasteboard's bytes into a guest client's pipe.
struct np_clip_write {
	struct wl_list link;
	int fd;
	struct wl_event_source *source;
	unsigned char *data;
	size_t len, sent;
};

static void clip_read_finish(struct np_clip_read *read_state, bool ok) {
	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "token", read_state->token);
	cJSON_AddStringToObject(body, "mimeType", read_state->mime);
	char *encoded = ok ? np_base64_encode(read_state->data, read_state->len) : NULL;
	if (encoded) {
		cJSON_AddStringToObject(body, "base64", encoded);
		free(encoded);
	} else {
		cJSON_AddNullToObject(body, "base64");
	}
	np_host_send(&read_state->server->host, "selectionData", body);

	if (read_state->source) wl_event_source_remove(read_state->source);
	close(read_state->fd);
	wl_list_remove(&read_state->link);
	free(read_state->mime);
	free(read_state->data);
	free(read_state);
}

static int clip_read_ready(int fd, uint32_t mask, void *data) {
	struct np_clip_read *read_state = data;
	for (;;) {
		if (read_state->len == read_state->cap) {
			size_t cap = read_state->cap ? read_state->cap * 2 : 8192;
			if (cap > NP_CLIP_MAX) {
				fprintf(stderr, "[wayland] selection exceeds %u bytes; refusing\n",
				        NP_CLIP_MAX);
				clip_read_finish(read_state, false);
				return 0;
			}
			unsigned char *grown = realloc(read_state->data, cap);
			if (!grown) {
				clip_read_finish(read_state, false);
				return 0;
			}
			read_state->data = grown;
			read_state->cap = cap;
		}
		ssize_t got = read(fd, read_state->data + read_state->len,
		                   read_state->cap - read_state->len);
		if (got > 0) {
			read_state->len += (size_t)got;
			continue;
		}
		if (got < 0 && errno == EINTR) continue;
		if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		// Zero is the writer closing its end, which is how a source signals that
		// it has handed over everything.
		clip_read_finish(read_state, got == 0);
		return 0;
	}
}

/// Asks the guest's current selection owner for `mime_type` and reports the
/// bytes back to the host under `token`.
void np_data_serve_host_request(struct np_server *server, uint32_t token,
                                    const char *mime_type) {
	int pipe_fds[2];
	bool failed = !server->selection_source || pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) < 0;

	struct np_clip_read *read_state = failed ? NULL : calloc(1, sizeof(*read_state));
	if (failed || !read_state) {
		if (!failed) { close(pipe_fds[0]); close(pipe_fds[1]); }
		cJSON *body = cJSON_CreateObject();
		cJSON_AddNumberToObject(body, "token", token);
		cJSON_AddStringToObject(body, "mimeType", mime_type);
		cJSON_AddNullToObject(body, "base64");
		np_host_send(&server->host, "selectionData", body);
		free(read_state);
		return;
	}

	read_state->server = server;
	read_state->token = token;
	read_state->mime = strdup(mime_type);
	read_state->fd = pipe_fds[0];
	wl_list_insert(&server->clip_reads, &read_state->link);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	read_state->source = wl_event_loop_add_fd(loop, pipe_fds[0],
	                                          WL_EVENT_READABLE | WL_EVENT_HANGUP,
	                                          clip_read_ready, read_state);

	// The source writes into the far end and closes it; the compositor must let
	// go of its copy or the read would never see EOF.
	wl_data_source_send_send(server->selection_source, mime_type, pipe_fds[1]);
	close(pipe_fds[1]);
	wl_display_flush_clients(server->display);
}

static void clip_write_finish(struct np_clip_write *write_state) {
	if (write_state->source) wl_event_source_remove(write_state->source);
	close(write_state->fd);
	wl_list_remove(&write_state->link);
	free(write_state->data);
	free(write_state);
}

static int clip_write_ready(int fd, uint32_t mask, void *data) {
	struct np_clip_write *write_state = data;
	while (write_state->sent < write_state->len) {
		ssize_t put = write(fd, write_state->data + write_state->sent,
		                    write_state->len - write_state->sent);
		if (put > 0) {
			write_state->sent += (size_t)put;
			continue;
		}
		if (put < 0 && errno == EINTR) continue;
		if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		break;   // the client gave up on reading; nothing left to do for it
	}
	clip_write_finish(write_state);
	return 0;
}

/// Pending pastes waiting on the host, keyed by token. A client hands over a
/// pipe and expects bytes; the answer arrives one round trip later.
struct np_clip_pending {
	struct wl_list link;
	uint32_t token;
	int fd;
};

static void clip_request_from_host(struct np_server *server, const char *mime_type, int fd) {
	struct np_clip_pending *pending = calloc(1, sizeof(*pending));
	if (!pending) {
		close(fd);
		return;
	}
	pending->token = ++server->next_clip_token;
	pending->fd = fd;
	wl_list_insert(&server->clip_writes, &pending->link);

	cJSON *body = cJSON_CreateObject();
	cJSON_AddNumberToObject(body, "token", pending->token);
	cJSON_AddStringToObject(body, "mimeType", mime_type);
	np_host_send(&server->host, "hostSelectionRequest", body);
}

/// Completes a paste: the host answered, so the bytes go into the pipe the
/// client supplied. Nothing here blocks — an unread pipe just leaves the write
/// pending until the loop says it will take more.
void np_data_deliver_host_data(struct np_server *server, uint32_t token,
                                   const char *base64) {
	struct np_clip_pending *pending, *tmp;
	int fd = -1;
	wl_list_for_each_safe(pending, tmp, &server->clip_writes, link) {
		if (pending->token != token) continue;
		fd = pending->fd;
		wl_list_remove(&pending->link);
		free(pending);
		break;
	}
	if (fd < 0) return;

	size_t len = 0;
	unsigned char *data = base64 ? np_base64_decode(base64, &len) : NULL;
	if (!data || len == 0) {
		// Closing an empty pipe is a valid answer: the client sees EOF and
		// concludes the selection had nothing in that type.
		free(data);
		close(fd);
		return;
	}

	struct np_clip_write *write_state = calloc(1, sizeof(*write_state));
	if (!write_state) {
		free(data);
		close(fd);
		return;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	write_state->fd = fd;
	write_state->data = data;
	write_state->len = len;
	wl_list_insert(&server->clip_writes, &write_state->link);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	write_state->source = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE,
	                                           clip_write_ready, write_state);
	clip_write_ready(fd, WL_EVENT_WRITABLE, write_state);
}

/// Tells the host what the guest's selection now offers, so it can put matching
/// types on NSPasteboard. An empty list means the guest gave the selection up.
static void announce_selection_to_host(struct np_server *server) {
	cJSON *types = cJSON_CreateArray();
	if (server->selection_source) {
		struct np_data_source *source = wl_resource_get_user_data(server->selection_source);
		if (source) {
			for (int i = 0; i < source->mime_count; i++) {
				cJSON_AddItemToArray(types, cJSON_CreateString(source->mime_types[i]));
			}
		}
	}
	cJSON *body = cJSON_CreateObject();
	cJSON_AddItemToObject(body, "mimeTypes", types);
	np_host_send(&server->host, "selectionOffered", body);
}

/// Announces the current selection to one data device: a fresh offer, its types,
/// and then the selection itself. Order matters — a client must see the offer
/// object before it is told that object is the selection.
void np_data_send_selection(struct np_server *server, struct wl_resource *device) {
	if (!server->selection_source && server->host_mime_count > 0) {
		// The Mac owns the clipboard. The offer is real as far as the client is
		// concerned; only the fetch is different.
		struct wl_resource *offer = create_data_offer(server, device, NULL, false);
		if (!offer) return;
		struct np_data_offer *state = wl_resource_get_user_data(offer);
		state->from_host = true;
		wl_data_device_send_data_offer(device, offer);
		for (int i = 0; i < server->host_mime_count; i++) {
			wl_data_offer_send_offer(offer, server->host_mime[i]);
		}
		wl_data_device_send_selection(device, offer);
		return;
	}
	if (!server->selection_source) {
		wl_data_device_send_selection(device, NULL);
		return;
	}
	struct np_data_source *source = wl_resource_get_user_data(server->selection_source);
	if (!source) return;

	struct wl_resource *offer = create_data_offer(server, device,
	                                             server->selection_source, false);
	if (!offer) return;
	announce_offer(device, offer, server->selection_source, false);
	wl_data_device_send_selection(device, offer);
}

void np_data_drag_leave(struct np_server *server, uint32_t window_id) {
	struct np_surface *surface = np_surface_by_id(server, server->drag_focus_surface);
	if (!surface) surface = np_surface_by_window(server, window_id);
	if (!surface) return;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		struct np_data_offer *offer;
		wl_list_for_each(offer, &server->data_offers, link) {
			if (!offer->active || offer->device != device->resource ||
			    offer->source != server->drag_source) continue;
			offer->active = false;
			if (offer->accepted_mime) {
				free(offer->accepted_mime);
				offer->accepted_mime = NULL;
				wl_data_source_send_target(offer->source, NULL);
			}
			if (offer->chosen_action) {
				offer->chosen_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
				wl_data_offer_send_action(offer->resource,
				                          WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
				if (wl_resource_get_version(offer->source) >= 3)
					wl_data_source_send_action(offer->source,
					                           WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
			}
		}
		wl_data_device_send_leave(device->resource);
	}
	server->drag_focus_surface = 0;
}

void np_data_drag_enter(struct np_server *server, struct np_surface *surface,
                            wl_fixed_t x, wl_fixed_t y) {
	if (!server->drag_source || !surface) return;
	uint32_t serial = wl_display_next_serial(server->display);
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		struct wl_resource *offer = create_data_offer(
			server, device->resource, server->drag_source, true);
		if (!offer) continue;
		struct np_data_offer *state = wl_resource_get_user_data(offer);
		if (state) state->active = true;
		announce_offer(device->resource, offer, server->drag_source, true);
		wl_data_device_send_enter(device->resource, serial, surface->resource, x, y, offer);
	}
	struct np_surface *root = np_scene_root(surface);
	server->drag_focus_window = root ? root->window_id : surface->window_id;
	server->drag_focus_surface = surface->id;
}

void np_data_drag_motion(struct np_server *server, struct np_surface *surface,
                             uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		bool active = false;
		struct np_data_offer *offer;
		wl_list_for_each(offer, &server->data_offers, link) {
			if (offer->active && offer->device == device->resource &&
			    offer->source == server->drag_source) {
				active = true;
				break;
			}
		}
		if (!active) continue;
		wl_data_device_send_motion(device->resource, time, x, y);
	}
}

void np_data_drag_finish(struct np_server *server) {
	if (!server->drag_source) return;
	struct np_surface *surface = np_surface_by_id(server, server->drag_focus_surface);
	if (!surface) surface = np_surface_by_window(server, server->drag_focus_window);
	if (!surface) {
		struct wl_resource *source = server->drag_source;
		wl_data_source_send_cancelled(source);
		detach_offers_for_source(server, source, false);
		server->drag_source = NULL;
		server->drag_origin = NULL;
		if (server->drag_icon) {
			send_drag_icon(server, NULL);
		}
		server->drag_icon = NULL;
		server->drag_focus_window = 0;
		server->drag_focus_surface = 0;
		return;
	}
	bool dropped = false;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(surface->resource)) continue;
		struct np_data_offer *match = NULL;
		struct np_data_offer *offer;
		wl_list_for_each(offer, &server->data_offers, link) {
			if (offer->active && offer->device == device->resource &&
			    offer->source == server->drag_source) {
				match = offer;
				break;
			}
		}
		if (!match) continue;
		match->active = false;
		bool accepted = wl_resource_get_version(match->resource) < 3 ||
		                (match->accepted_mime && match->chosen_action !=
		                 WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
		if (!accepted) {
			wl_data_device_send_leave(device->resource);
			continue;
		}
		match->dropped = true;
		wl_data_device_send_drop(device->resource);
		dropped = true;
	}
	if (dropped && wl_resource_get_version(server->drag_source) >= 3) {
		wl_data_source_send_dnd_drop_performed(server->drag_source);
	}
	server->drag_dropped = dropped;
	if (!dropped) {
		struct wl_resource *source = server->drag_source;
		wl_data_source_send_cancelled(source);
		detach_offers_for_source(server, source, false);
		server->drag_source = NULL;
		server->drag_origin = NULL;
	}
	if (server->drag_icon) {
		send_drag_icon(server, NULL);
		server->drag_icon = NULL;
	}
	server->drag_focus_window = 0;
	server->drag_focus_surface = 0;
	np_input_restore_pointer_focus_after_drag(server, surface);
}

static void data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
                                   struct wl_resource *source, struct wl_resource *origin,
                                   struct wl_resource *icon, uint32_t serial) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry || !source || !origin) return;
	struct np_server *server = entry->server;
	struct np_surface *origin_surface = wl_resource_get_user_data(origin);
	if (!server->pointer_button_down || serial != server->pointer_grab_serial ||
	    server->pointer_grab_client != client || !origin_surface ||
	    origin_surface->id != server->pointer_surface)
		return;
	struct np_surface *icon_surface = icon ? wl_resource_get_user_data(icon) : NULL;
	if (icon && (!icon_surface ||
	             !np_surface_assign_role(icon_surface, NP_SURFACE_ROLE_DRAG_ICON))) {
		wl_resource_post_error(resource, WL_DATA_DEVICE_ERROR_ROLE,
		                       "drag icon surface has another role");
		return;
	}
	if (server->drag_source && server->drag_source != source) {
		struct wl_resource *previous = server->drag_source;
		wl_data_source_send_cancelled(previous);
		detach_offers_for_source(server, previous, false);
	}
	server->drag_source = source;
	server->drag_origin = origin;
	server->drag_icon = icon;
	server->drag_dropped = false;
	/* A data-device drag replaces the default pointer grab. The ordinary
	 * wl_pointer focus must leave before wl_data_device.enter takes ownership of
	 * the same physical pointer. GDK relies on this ordering: its DnD enter
	 * installs the drop focus without first clearing the normal pointer focus. */
	struct np_surface *surface = np_surface_by_window(server, server->pointer_window);
	if (!surface) surface = origin_surface;
	np_input_clear_pointer_focus_for_drag(server);
	send_drag_icon(server, icon);
	np_data_drag_enter(server, surface, fixed_from(server->pointer_x), fixed_from(server->pointer_y));
}

static void data_device_set_selection(struct wl_client *client, struct wl_resource *resource,
                                      struct wl_resource *source, uint32_t serial) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry) return;
	struct np_server *server = entry->server;
	server->selection_source = source;
	// A guest client taking the selection displaces the Mac's, and vice versa.
	// Keeping both would mean deciding which one a paste meant.
	for (int i = 0; i < server->host_mime_count; i++) free(server->host_mime[i]);
	server->host_mime_count = 0;
	announce_selection_to_host(server);

	struct np_surface *focused = server->focused_window
		? np_surface_by_window(server, server->focused_window) : NULL;
	if (!focused) return;
	struct np_input *device;
	wl_list_for_each(device, &server->data_devices, link) {
		if (wl_resource_get_client(device->resource) !=
		    wl_resource_get_client(focused->resource)) continue;
		np_data_send_selection(server, device->resource);
	}
}

static void data_device_release(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void data_device_resource_destroy(struct wl_resource *resource) {
	struct np_input *entry = wl_resource_get_user_data(resource);
	if (!entry) return;
	/* Offers keep a non-owning pointer to their target data device. Detach it
	 * before libwayland is allowed to recycle the wl_resource allocation. */
	struct np_data_offer *offer;
	wl_list_for_each(offer, &entry->server->data_offers, link) {
		if (offer->device != resource) continue;
		offer->device = NULL;
		offer->active = false;
	}
	wl_list_remove(&entry->link);
	free(entry);
}

static const struct wl_data_device_interface data_device_implementation = {
	.start_drag = data_device_start_drag,
	.set_selection = data_device_set_selection,
	.release = data_device_release,
};

static void data_manager_create_source(struct wl_client *client, struct wl_resource *resource,
                                       uint32_t id) {
	struct wl_resource *source = wl_resource_create(
		client, &wl_data_source_interface, wl_resource_get_version(resource), id);
	if (!source) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_data_source *state = calloc(1, sizeof(*state));
	if (!state) {
		wl_resource_destroy(source);
		wl_client_post_no_memory(client);
		return;
	}
	state->resource = source;
	state->server = wl_resource_get_user_data(resource);
	wl_resource_set_implementation(source, &data_source_implementation, state,
	                               data_source_resource_destroy);
}

static void data_manager_get_device(struct wl_client *client, struct wl_resource *resource,
                                    uint32_t id, struct wl_resource *seat) {
	struct np_server *server = wl_resource_get_user_data(resource);
	struct wl_resource *device = wl_resource_create(
		client, &wl_data_device_interface, wl_resource_get_version(resource), id);
	if (!device) {
		wl_client_post_no_memory(client);
		return;
	}
	struct np_input *entry = calloc(1, sizeof(*entry));
	if (!entry) {
		wl_resource_destroy(device);
		wl_client_post_no_memory(client);
		return;
	}
	entry->resource = device;
	entry->server = server;
	wl_list_insert(&server->data_devices, &entry->link);
	// The entry, not the server: input_resource_destroy unlinks whatever it is
	// given, and handing it the server would unlink a field of the server struct.
	wl_resource_set_implementation(device, &data_device_implementation, entry,
	                               data_device_resource_destroy);
}

static const struct wl_data_device_manager_interface data_manager_implementation = {
	.create_data_source = data_manager_create_source,
	.get_data_device = data_manager_get_device,
};

void np_data_device_manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &wl_data_device_manager_interface, (int)version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &data_manager_implementation, data, NULL);
}
