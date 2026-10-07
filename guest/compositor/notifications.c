#include "notifications.h"

#include "compositor_internal.h"
#include "notify_dbus.h"
#include "window_events.h"
#include "windowwire.h"

#include <glib-unix.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>

/* A program that posts without pause must not grow memory while the host
 * session is down or slow. The host rate-limits what it shows. */
#define NP_NOTIFICATIONS_MAX_PENDING 64

struct np_notifications {
	struct np_server *server;
	struct np_notify_service *service;
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	GMutex lock;
	GQueue queue; /* struct np_notify_event *, produced on the service thread */
	int wake[2];
	struct wl_event_source *source;
	bool stopping;
};

static bool deliver(struct np_notify_event *event, void *user)
{
	struct np_notifications *n = user;
	g_mutex_lock(&n->lock);
	if (event->type == NP_NOTIFY_EVENT_RESET) {
		struct np_notify_event *pending;
		while ((pending = g_queue_pop_head(&n->queue))) np_notify_event_free(pending);
		g_mutex_unlock(&n->lock);
		np_notify_event_free(event);
		return true;
	}
	if (n->stopping) {
		g_mutex_unlock(&n->lock);
		np_notify_event_free(event);
		return false;
	}
	/* A replacement supersedes pending state for this ID. This also makes a
	 * pending close replace its post instead of being lost to queue pressure. */
	for (GList *item = n->queue.head; item; item = item->next) {
		struct np_notify_event *pending = item->data;
		if (pending->id == event->id) {
			g_queue_delete_link(&n->queue, item);
			np_notify_event_free(pending);
			break;
		}
	}
	bool full = g_queue_get_length(&n->queue) >= NP_NOTIFICATIONS_MAX_PENDING;
	if (!full) g_queue_push_tail(&n->queue, event);
	g_mutex_unlock(&n->lock);
	if (full) {
		np_notify_event_free(event);
		return false;
	}
	/* The pipe is non-blocking; a full pipe already means a wake is pending. */
	char byte = 1;
	ssize_t ignored = write(n->wake[1], &byte, 1);
	(void)ignored;
	return true;
}

static bool send_event(struct np_server *server, const struct np_notify_event *event)
{
	if (!server->host_session_ready || !event->id || !event->revision ||
	    (event->type != NP_NOTIFY_EVENT_POSTED && event->type != NP_NOTIFY_EVENT_CLOSED)) return false;
	struct np_window_message message;
	if (event->type == NP_NOTIFY_EVENT_POSTED) {
		np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST, NP_GUEST_NOTIFICATION_POSTED);
		np_window_put_u32(&message, event->id);
		np_window_put_u64(&message, event->revision);
		np_window_put_u8(&message, event->urgency);
		np_window_put_i32(&message, event->timeout_ms);
		np_window_put_string(&message, event->app_name);
		np_window_put_string(&message, event->desktop_entry);
		np_window_put_string(&message, event->summary);
		np_window_put_string(&message, event->body);
		np_window_put_u32(&message, event->action_count);
		for (unsigned i = 0; i < event->action_count; i++) {
			np_window_put_string(&message, event->actions[i].key);
			np_window_put_string(&message, event->actions[i].label);
		}
	} else {
		np_window_message_init(&message, NP_WINDOW_GUEST_TO_HOST, NP_GUEST_NOTIFICATION_CLOSED);
		np_window_put_u32(&message, event->id);
		np_window_put_u64(&message, event->revision);
	}
	bool sent = message.ok && np_window_event_send_message(server, &message);
	np_window_message_clear(&message);
	return sent;
}

static void send_backlog_reset(struct np_server *server)
{
	(void)np_window_event_send(server, NP_GUEST_NOTIFICATION_BACKLOG_RESET, NULL, 0);
}

static int wake_ready(int fd, uint32_t mask, void *data)
{
	(void)mask;
	struct np_notifications *n = data;
	char buffer[64];
	while (read(fd, buffer, sizeof(buffer)) > 0) {}
	for (;;) {
		g_mutex_lock(&n->lock);
		struct np_notify_event *event = g_queue_pop_head(&n->queue);
		g_mutex_unlock(&n->lock);
		if (!event) break;
		if (event->type != NP_NOTIFY_EVENT_POSTED || np_notify_service_is_current(n->service, event->id, event->revision)) {
			if (!send_event(n->server, event)) {
				if (event->type == NP_NOTIFY_EVENT_POSTED) {
					np_notify_service_emit_closed(n->service, event->id, event->revision, NP_NOTIFY_CLOSED_UNDEFINED);
					/* A refused replacement also withdraws the host's older banner.
					 * The transport retains bounded close tombstones or a reset. */
					event->type = NP_NOTIFY_EVENT_CLOSED;
					if (!send_event(n->server, event)) send_backlog_reset(n->server);
				} else send_backlog_reset(n->server);
			}
		}
		np_notify_event_free(event);
	}
	return 0;
}

static gpointer service_thread(gpointer data)
{
	struct np_notifications *n = data;
	g_main_context_push_thread_default(n->context);
	np_notify_service_start(n->service, n->loop);
	g_main_loop_run(n->loop);
	g_main_context_pop_thread_default(n->context);
	return NULL;
}

bool np_notifications_init(struct np_server *server)
{
	if (!server) return false;
	const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (!bus || !bus[0]) return false; /* No session bus: applications cannot reach us. */

	struct np_notifications *n = g_new0(struct np_notifications, 1);
	n->server = server;
	n->wake[0] = n->wake[1] = -1;
	g_mutex_init(&n->lock);
	g_queue_init(&n->queue);
	if (!g_unix_open_pipe(n->wake, FD_CLOEXEC, NULL)) goto fail;
	if (!g_unix_set_fd_nonblocking(n->wake[0], TRUE, NULL) ||
	    !g_unix_set_fd_nonblocking(n->wake[1], TRUE, NULL)) goto fail;
	n->source = wl_event_loop_add_fd(wl_display_get_event_loop(server->display),
	                                 n->wake[0], WL_EVENT_READABLE, wake_ready, n);
	if (!n->source) goto fail;
	n->service = np_notify_service_new(deliver, n);
	n->context = g_main_context_new();
	n->loop = g_main_loop_new(n->context, FALSE);
	if (!n->service || !n->context || !n->loop) goto fail;
	n->thread = g_thread_try_new("np-notifications", service_thread, n, NULL);
	if (!n->thread) goto fail;
	server->notifications = n;
	if (!np_notify_service_wait_ready(n->service, g_get_monotonic_time() + G_TIME_SPAN_SECOND)) {
		fprintf(stderr, "[notifications] session notification service unavailable; forwarding disabled\n");
		np_notifications_finish(server);
		return false;
	}
	return true;

fail:
	if (n->source) wl_event_source_remove(n->source);
	if (n->loop) g_main_loop_unref(n->loop);
	if (n->context) g_main_context_unref(n->context);
	np_notify_service_free(n->service);
	for (int i = 0; i < 2; i++) if (n->wake[i] >= 0) close(n->wake[i]);
	g_mutex_clear(&n->lock);
	g_free(n);
	return false;
}

static gboolean quit_loop(gpointer data)
{
	struct np_notifications *n = data;
	np_notify_service_stop(n->service);
	return G_SOURCE_REMOVE;
}

void np_notifications_finish(struct np_server *server)
{
	struct np_notifications *n = server ? server->notifications : NULL;
	if (!n) return;
	g_mutex_lock(&n->lock);
	n->stopping = true;
	g_mutex_unlock(&n->lock);
	np_notifications_reset(server);
	server->notifications = NULL;
	if (n->source) wl_event_source_remove(n->source);
	/* quit() before run() is forgotten when run() starts. An attached source
	 * executes after the worker enters its context, even when finish wins the
	 * startup race. invoke() is insufficient: it may execute synchronously. */
	GSource *stop = g_idle_source_new();
	g_source_set_priority(stop, G_PRIORITY_HIGH);
	g_source_set_callback(stop, quit_loop, n, NULL);
	g_source_attach(stop, n->context);
	g_source_unref(stop);
	g_thread_join(n->thread);
	np_notify_service_free(n->service);
	struct np_notify_event *event;
	while ((event = g_queue_pop_head(&n->queue))) np_notify_event_free(event);
	g_main_loop_unref(n->loop);
	g_main_context_unref(n->context);
	for (int i = 0; i < 2; i++) close(n->wake[i]);
	g_mutex_clear(&n->lock);
	g_free(n);
}

void np_notifications_reset(struct np_server *server)
{
	struct np_notifications *n = server ? server->notifications : NULL;
	if (n) np_notify_service_reset(n->service);
}

bool np_notifications_handle_command(struct np_server *server, struct np_window_reader *reader)
{
	struct np_notifications *n = server ? server->notifications : NULL;
	uint32_t id = np_window_read_u32(reader);
	uint64_t revision = np_window_read_u64(reader);
	if (reader->opcode == NP_HOST_NOTIFICATION_CLOSED) {
		uint32_t reason = np_window_read_u32(reader);
		if (n && np_window_reader_finished(reader))
			np_notify_service_emit_closed(n->service, id, revision, reason);
		return true;
	}
	if (reader->opcode == NP_HOST_NOTIFICATION_ACTION) {
		const unsigned char *bytes = NULL;
		size_t size = 0;
		if (np_window_read_bytes(reader, &bytes, &size, false, NULL) &&
		    size > 0 && size <= NP_NOTIFY_MAX_ACTION_BYTES && !memchr(bytes, 0, size) &&
		    g_utf8_validate((const char *)bytes, (gssize)size, NULL) &&
		    np_window_reader_finished(reader) && n) {
			char key[NP_NOTIFY_MAX_ACTION_BYTES + 1];
			memcpy(key, bytes, size); key[size] = 0;
			np_notify_service_emit_action(n->service, id, revision, key);
		}
		return true;
	}
	return false;
}
