/*
 * Exercises org.freedesktop.Notifications over a point-to-point D-Bus
 * connection: real method calls and signals, no bus daemon and no Wayland.
 */
#include "../notify_dbus.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <malloc.h>
#endif
#ifdef NP_NOTIFICATIONS_LIFECYCLE_TEST
#include <sys/socket.h>
#include <sys/un.h>
#include "../notifications.c"
#include "../host_session_common.c"
#endif

static GMutex lock;
static GPtrArray *events;
static GMainContext *server_context;
static GMainLoop *server_loop;
static GDBusServer *server;
static struct np_notify_service *service;
static gchar *client_address;
static GCond ready;
static gboolean server_ready;
static gboolean reject_next;
static uint32_t rejected_id;

static bool record_event(struct np_notify_event *event, void *user)
{
	(void)user;
	g_mutex_lock(&lock);
	if (reject_next) {
		reject_next = FALSE;
		rejected_id = event->id;
		g_mutex_unlock(&lock);
		np_notify_event_free(event);
		return false;
	}
	g_ptr_array_add(events, event);
	g_mutex_unlock(&lock);
	return true;
}

static struct np_notify_event *take(enum np_notify_event_type type)
{
	g_mutex_lock(&lock);
	assert(events->len > 0);
	struct np_notify_event *event = g_ptr_array_index(events, 0);
	g_ptr_array_remove_index(events, 0);
	g_mutex_unlock(&lock);
	assert(event->type == type);
	return event;
}

static gboolean on_connection(GDBusServer *s, GDBusConnection *connection, gpointer user)
{
	(void)s; (void)user;
	GError *error = NULL;
	if (!np_notify_service_register(service, connection, &error)) {
		fprintf(stderr, "register failed: %s\n", error->message);
		assert(false);
	}
	return TRUE;
}

static gpointer server_thread(gpointer data)
{
	(void)data;
	g_main_context_push_thread_default(server_context);
	GError *error = NULL;
	gchar *guid = g_dbus_generate_guid();
	server = g_dbus_server_new_sync("unix:tmpdir=/tmp", G_DBUS_SERVER_FLAGS_AUTHENTICATION_ALLOW_ANONYMOUS,
	                                guid, NULL, NULL, &error);
	g_free(guid);
	assert(server);
	g_signal_connect(server, "new-connection", G_CALLBACK(on_connection), NULL);
	g_dbus_server_start(server);
	g_mutex_lock(&lock);
	client_address = g_strdup(g_dbus_server_get_client_address(server));
	server_ready = TRUE;
	g_cond_signal(&ready);
	g_mutex_unlock(&lock);
	g_main_loop_run(server_loop);
	g_main_context_pop_thread_default(server_context);
	return NULL;
}

static GVariant *call(GDBusConnection *client, const char *method, GVariant *parameters,
                      const char *reply, GError **error)
{
	return g_dbus_connection_call_sync(client, NULL, "/org/freedesktop/Notifications",
	                                   "org.freedesktop.Notifications", method, parameters,
	                                   reply ? G_VARIANT_TYPE(reply) : NULL,
	                                   G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
}

struct signal_log {
	guint closed_id, closed_reason, action_id;
	guint closed_count;
	gchar action_key[64];
	gboolean closed, action;
};

static void on_signal(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                      const gchar *name, GVariant *parameters, gpointer data)
{
	(void)c; (void)sender; (void)path; (void)iface;
	struct signal_log *log = data;
	if (g_strcmp0(name, "NotificationClosed") == 0) {
		g_variant_get(parameters, "(uu)", &log->closed_id, &log->closed_reason);
		log->closed = TRUE;
		log->closed_count++;
	} else if (g_strcmp0(name, "ActionInvoked") == 0) {
		const gchar *key;
		g_variant_get(parameters, "(u&s)", &log->action_id, &key);
		g_strlcpy(log->action_key, key, sizeof(log->action_key));
		log->action = TRUE;
	}
}

static void wait_for(gboolean *flag)
{
	for (int i = 0; i < 500 && !*flag; i++) {
		while (g_main_context_iteration(NULL, FALSE)) {}
		g_usleep(10000);
	}
	assert(*flag);
}

static GVariant *notify_args(const char *app, guint replaces, const char *summary, const char *body,
                             const gchar *const *actions, GVariant *hints, gint timeout)
{
	static const gchar *const none[] = {NULL};
	return g_variant_new("(susss^as@a{sv}i)", app, replaces, "icon", summary, body,
	                     actions ? actions : none, hints, timeout);
}

static GVariant *no_hints(void)
{
	return g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0);
}

static void expect_close_error(GDBusConnection *client, uint32_t id)
{
	GError *error = NULL;
	GVariant *reply = call(client, "CloseNotification", g_variant_new("(u)", id), NULL, &error);
	assert(!reply && error);
	char *name = g_dbus_error_get_remote_error(error);
	assert(name && strcmp(name, "org.freedesktop.DBus.Error.InvalidArgs") == 0);
	g_free(name);
	g_clear_error(&error);
	g_mutex_lock(&lock);
	assert(events->len == 0);
	g_mutex_unlock(&lock);
}

#ifdef NP_NOTIFICATIONS_LIFECYCLE_TEST
/* The real notifications module, with only the transport replaced. */
static unsigned refuse_posts, refuse_closes, backlog_resets;
bool np_window_event_send_message(struct np_server *s, struct np_window_message *message)
{
	(void)s;
	assert(message->ok);
	if (message->data[5] == NP_GUEST_NOTIFICATION_POSTED && refuse_posts) { refuse_posts--; return false; }
	if (message->data[5] == NP_GUEST_NOTIFICATION_CLOSED && refuse_closes) { refuse_closes--; return false; }
	return true;
}

bool np_xwayland_owns_client(struct np_server *s, struct wl_client *client)
{ (void)s; (void)client; return false; }
bool np_surface_is_toplevel(const struct np_surface *surface) { (void)surface; return false; }
bool np_surface_is_popup(const struct np_surface *surface) { (void)surface; return false; }
bool np_window_event_send(struct np_server *s, uint8_t opcode, const uint32_t *values, size_t count)
{ (void)s; (void)values;
  if (opcode == NP_GUEST_NOTIFICATION_BACKLOG_RESET) { assert(count == 0); backlog_resets++; }
  return true; }
bool np_window_event_send_force_quit_capability(struct np_server *s, uint32_t window, bool supported)
{ (void)s; (void)window; (void)supported; return true; }

static void test_optional_sender_refusal(struct np_notify_event *event)
{
	struct np_notifications n = {.service = service};
	struct np_server s = {.notifications = &n, .host_session_ready = true}; n.server = &s;
	g_mutex_init(&n.lock); g_queue_init(&n.queue);
	assert(g_unix_open_pipe(n.wake, FD_CLOEXEC, NULL));
	assert(g_unix_set_fd_nonblocking(n.wake[0], TRUE, NULL));
	uint32_t id = event->id; uint64_t revision = event->revision;
	g_queue_push_tail(&n.queue, event);
	refuse_posts = refuse_closes = 1;
	wake_ready(n.wake[0], WL_EVENT_READABLE, &n);
	assert(refuse_posts == 0 && refuse_closes == 0 && backlog_resets == 1);
	assert(!np_notify_service_is_current(service, id, revision) && g_queue_is_empty(&n.queue));
	close(n.wake[0]); close(n.wake[1]); g_mutex_clear(&n.lock);
}

static void test_optional_feedback(struct np_notify_service *active_service, uint32_t id, uint64_t revision)
{
	struct np_notifications n = {.service = active_service};
	struct np_server s = {.notifications = &n};
	for (unsigned kind = 0; kind < 8; kind++) {
		struct np_window_message message;
		np_window_message_init(&message, NP_WINDOW_HOST_TO_GUEST,
			kind < 3 ? NP_HOST_NOTIFICATION_CLOSED : NP_HOST_NOTIFICATION_ACTION);
		np_window_put_u32(&message, id);
		np_window_put_u64(&message, kind == 0 ? revision - 1 : revision);
		if (kind < 3) {
			np_window_put_u32(&message, kind == 1 ? 99 : NP_NOTIFY_CLOSED_DISMISSED);
			if (kind == 2) np_window_put_u8(&message, 0);
		} else if (kind == 3) np_window_put_string(&message, "");
		else if (kind == 4) {
			unsigned char oversized[NP_NOTIFY_MAX_ACTION_BYTES + 1]; memset(oversized, 'x', sizeof(oversized));
			np_window_put_bytes(&message, oversized, sizeof(oversized));
		} else if (kind == 5) np_window_put_bytes(&message, (const unsigned char *)"a\0b", 3);
		else if (kind == 6) np_window_put_bytes(&message, (const unsigned char *)"\xFF", 1);
		else { np_window_put_string(&message, "reply"); message.len = 14; }
		struct np_window_reader reader;
		assert(np_window_reader_init(&reader, message.data, message.len, NP_WINDOW_HOST_TO_GUEST));
		assert(np_notifications_handle_command(&s, &reader));
		assert(np_notify_service_is_current(active_service, id, revision));
		np_window_message_clear(&message);
	}
}

struct delayed_start {
	struct np_server server;
	struct np_notifications *notifications;
	GMutex lock;
	GCond ready;
	bool start;
};

static gpointer delayed_service_thread(gpointer data)
{
	struct delayed_start *fixture = data;
	g_mutex_lock(&fixture->lock);
	while (!fixture->start) g_cond_wait(&fixture->ready, &fixture->lock);
	g_mutex_unlock(&fixture->lock);
	return service_thread(fixture->notifications);
}

static gpointer finish_before_start(gpointer data)
{
	struct delayed_start *fixture = data;
	np_notifications_finish(&fixture->server);
	return NULL;
}

static void test_notifications_session_bus(void)
{
	struct np_server s = {.display = wl_display_create()};
	assert(s.display);
	GError *error = NULL;
	GDBusConnection *client = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	assert(client && !error);
	for (int i = 0; i < 50; i++) {
		assert(np_notifications_init(&s));
		/* No sleeps/polls before the application's first Notify. */
		GVariant *reply = g_dbus_connection_call_sync(client, "org.freedesktop.Notifications",
			"/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
			notify_args("Immediate", 0, "First notification", "", NULL, no_hints(), -1),
			G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
		assert(reply && !error); g_variant_unref(reply);
		np_notifications_finish(&s);
		assert(s.notifications == NULL);
		reply = g_dbus_connection_call_sync(client, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.freedesktop.Notifications"),
			G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
		gboolean owned = TRUE;
		assert(reply && !error); g_variant_get(reply, "(b)", &owned); g_variant_unref(reply);
		assert(!owned);
	}
	/* An existing desktop daemon keeps its name and its connection. */
	GVariant *reply = g_dbus_connection_call_sync(client, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.freedesktop.Notifications", 4u),
		G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
	guint result = 0;
	assert(reply && !error); g_variant_get(reply, "(u)", &result); g_variant_unref(reply); assert(result == 1);
	assert(!np_notifications_init(&s) && !s.notifications);
	reply = g_dbus_connection_call_sync(client, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.freedesktop.Notifications"),
		G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &error);
	gboolean owned = FALSE;
	assert(reply && !error); g_variant_get(reply, "(b)", &owned); g_variant_unref(reply); assert(owned);
	g_object_unref(client); wl_display_destroy(s.display);
	puts("notifications session startup/stop/name isolation: ok");
}

struct stalled_bus { int listener; bool eof; };
static gpointer stall_authentication(gpointer data)
{
	struct stalled_bus *bus = data;
	int fd = accept(bus->listener, NULL, NULL); assert(fd >= 0);
	char bytes[64]; ssize_t count;
	while ((count = read(fd, bytes, sizeof(bytes))) > 0) {}
	bus->eof = count == 0;
	close(fd); return NULL;
}

static void test_stalled_bus_deadline(struct np_server *s)
{
	char *directory = g_dir_make_tmp("nativepipe-notify-stalled-XXXXXX", NULL); assert(directory);
	char *path = g_build_filename(directory, "bus", NULL);
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	assert(strlen(path) < sizeof(address.sun_path)); strcpy(address.sun_path, path);
	struct stalled_bus bus = {.listener = socket(AF_UNIX, SOCK_STREAM, 0)}; assert(bus.listener >= 0);
	assert(bind(bus.listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	assert(listen(bus.listener, 1) == 0);
	GThread *worker = g_thread_new("stall-authentication", stall_authentication, &bus);
	char *bus_address = g_strdup_printf("unix:path=%s", path);
	assert(setenv("DBUS_SESSION_BUS_ADDRESS", bus_address, 1) == 0);
	gint64 start = g_get_monotonic_time();
	assert(!np_notifications_init(s) && !s->notifications);
	gint64 elapsed = g_get_monotonic_time() - start;
	assert(elapsed < 1500 * G_TIME_SPAN_MILLISECOND);
	g_thread_join(worker); assert(bus.eof);
	printf("stalled authentication cancelled and worker drained in %.1f ms\n", elapsed / 1000.0);
	close(bus.listener); unlink(path); rmdir(directory);
	g_free(bus_address); g_free(path); g_free(directory);
}

static void test_notifications_lifecycle(void)
{
	assert(setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/nativepipe-no-notification-bus", 1) == 0);
	/* Delay the worker until finish has queued its stop. quit-before-run
	 * alone cannot terminate this fixture. */
	struct delayed_start fixture = {0};
	g_mutex_init(&fixture.lock); g_cond_init(&fixture.ready);
	struct np_notifications *n = g_new0(struct np_notifications, 1);
	n->server = &fixture.server;
	g_mutex_init(&n->lock); g_queue_init(&n->queue);
	assert(g_unix_open_pipe(n->wake, FD_CLOEXEC, NULL));
	n->service = np_notify_service_new(deliver, n);
	n->context = g_main_context_new();
	n->loop = g_main_loop_new(n->context, FALSE);
	fixture.server.notifications = n;
	fixture.notifications = n;
	GMainContext *context = g_main_context_ref(n->context);
	n->thread = g_thread_new("delayed-notifications", delayed_service_thread, &fixture);
	GThread *finisher = g_thread_new("early-finish", finish_before_start, &fixture);
	for (int i = 0; i < 500 && !g_main_context_pending(context); i++) g_usleep(1000);
	assert(g_main_context_pending(context));
	g_mutex_lock(&fixture.lock);
	fixture.start = true; g_cond_signal(&fixture.ready);
	g_mutex_unlock(&fixture.lock);
	g_thread_join(finisher);
	assert(fixture.server.notifications == NULL);
	g_main_context_unref(context);
	g_cond_clear(&fixture.ready); g_mutex_clear(&fixture.lock);

	struct np_server s = {.display = wl_display_create()};
	assert(s.display && setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/nativepipe-no-notification-bus", 1) == 0);
	for (int i = 0; i < 200; i++) {
		assert(!np_notifications_init(&s));
		np_notifications_finish(&s);
		assert(s.notifications == NULL);
	}
	test_stalled_bus_deadline(&s);
	wl_display_destroy(s.display);

	/* Exercise the actual bounded queue; replacements and closes reuse their
	 * ID's slot rather than growing it or losing the close at capacity. */
	struct np_notifications queue = {0};
	g_mutex_init(&queue.lock); g_queue_init(&queue.queue);
	assert(g_unix_open_pipe(queue.wake, FD_CLOEXEC, NULL));
	assert(g_unix_set_fd_nonblocking(queue.wake[1], TRUE, NULL));
	for (unsigned id = 1; id <= NP_NOTIFICATIONS_MAX_PENDING; id++) {
		struct np_notify_event *event = g_new0(struct np_notify_event, 1);
		event->type = NP_NOTIFY_EVENT_POSTED; event->id = id;
		assert(deliver(event, &queue));
	}
	struct np_notify_event *event = g_new0(struct np_notify_event, 1);
	event->type = NP_NOTIFY_EVENT_POSTED; event->id = 1; event->summary = g_strdup("Latest");
	assert(deliver(event, &queue));
	assert(g_queue_get_length(&queue.queue) == NP_NOTIFICATIONS_MAX_PENDING);
	assert(strcmp(((struct np_notify_event *)g_queue_peek_tail(&queue.queue))->summary, "Latest") == 0);
	event = g_new0(struct np_notify_event, 1);
	event->type = NP_NOTIFY_EVENT_CLOSED; event->id = 1;
	assert(deliver(event, &queue));
	assert(g_queue_get_length(&queue.queue) == NP_NOTIFICATIONS_MAX_PENDING);
	assert(((struct np_notify_event *)g_queue_peek_tail(&queue.queue))->type == NP_NOTIFY_EVENT_CLOSED);
	event = g_new0(struct np_notify_event, 1);
	event->type = NP_NOTIFY_EVENT_POSTED; event->id = NP_NOTIFICATIONS_MAX_PENDING + 1;
	assert(!deliver(event, &queue));
	struct np_server queue_server = {.notifications = &queue};
	queue.service = np_notify_service_new(deliver, &queue);
	/* Execute the real shared old-stream/new-session boundaries. */
	struct np_host disconnected = {.conn_fd = -1};
	struct wl_event_source *source = NULL;
	int watched = 123;
	uint32_t mask = 0;
	np_host_session_watch(&queue_server, &disconnected, &source, &watched, &mask, NULL);
	assert(g_queue_is_empty(&queue.queue) && watched == -1);
	event = g_new0(struct np_notify_event, 1);
	event->type = NP_NOTIFY_EVENT_POSTED; event->id = 1;
	assert(deliver(event, &queue));
	wl_list_init(&queue_server.surfaces);
	np_host_session_replay_metadata(&queue_server);
	assert(g_queue_is_empty(&queue.queue));
	np_notify_service_free(queue.service);
	close(queue.wake[0]); close(queue.wake[1]); g_mutex_clear(&queue.lock);
}
#endif

int main(int argc, char **argv)
{
#ifndef NP_NOTIFICATIONS_LIFECYCLE_TEST
	(void)argc; (void)argv;
#endif
#ifdef NP_NOTIFICATIONS_LIFECYCLE_TEST
	alarm(30);
	if (argc == 2 && strcmp(argv[1], "--session-bus") == 0) { test_notifications_session_bus(); return 0; }
	test_notifications_lifecycle();
#endif
	events = g_ptr_array_new();
	server_context = g_main_context_new();
	server_loop = g_main_loop_new(server_context, FALSE);
	service = np_notify_service_new(record_event, NULL);
	GThread *thread = g_thread_new("server", server_thread, NULL);
	g_mutex_lock(&lock);
	while (!server_ready) g_cond_wait(&ready, &lock);
	g_mutex_unlock(&lock);

	GError *error = NULL;
	GDBusConnection *client = g_dbus_connection_new_for_address_sync(
		client_address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, NULL, NULL, &error);
	assert(client);
	expect_close_error(client, 0);
	expect_close_error(client, UINT32_MAX);
	struct signal_log log = {0};
	g_dbus_connection_signal_subscribe(client, NULL, "org.freedesktop.Notifications", NULL,
	                                   "/org/freedesktop/Notifications", NULL,
	                                   G_DBUS_SIGNAL_FLAGS_NONE, on_signal, &log, NULL);

	/* Capabilities and identity. */
	GVariant *reply = call(client, "GetCapabilities", NULL, "(as)", &error);
	assert(reply);
	gchar **capabilities = NULL;
	g_variant_get(reply, "(^as)", &capabilities);
	assert(g_strv_contains((const gchar *const *)capabilities, "actions"));
	assert(g_strv_contains((const gchar *const *)capabilities, "body"));
	assert(!g_strv_contains((const gchar *const *)capabilities, "body-markup"));
	g_strfreev(capabilities);
	g_variant_unref(reply);
	reply = call(client, "GetServerInformation", NULL, "(ssss)", &error);
	assert(reply);
	const gchar *name, *version;
	g_variant_get(reply, "(&s&s&s&s)", &name, NULL, &version, NULL);
	assert(strcmp(name, "NativePipe") == 0 && strcmp(version, NATIVEPIPE_PRODUCT_VERSION) == 0);
	g_variant_unref(reply);

	/* A full notification: fields, urgency, desktop entry and action pairs. */
	const gchar *const actions[] = {"default", "Open", "reply", "Reply", NULL};
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&builder, "{sv}", "urgency", g_variant_new_byte(2));
	g_variant_builder_add(&builder, "{sv}", "desktop-entry", g_variant_new_string("org.example.mail"));
	reply = call(client, "Notify",
	             notify_args("Mail", 0, "New message", "Hello \xF0\x9F\x91\x8B", actions,
	                         g_variant_builder_end(&builder), 5000), "(u)", &error);
	assert(reply);
	guint first;
	g_variant_get(reply, "(u)", &first);
	g_variant_unref(reply);
	assert(first == 1);
	struct np_notify_event *event = take(NP_NOTIFY_EVENT_POSTED);
	assert(event->id == first && event->urgency == 2 && event->timeout_ms == 5000);
	assert(strcmp(event->app_name, "Mail") == 0);
	assert(strcmp(event->desktop_entry, "org.example.mail") == 0);
	assert(strcmp(event->summary, "New message") == 0);
	assert(strcmp(event->body, "Hello \xF0\x9F\x91\x8B") == 0);
	assert(event->action_count == 2);
	assert(strcmp(event->actions[0].key, "default") == 0 && strcmp(event->actions[0].label, "Open") == 0);
	assert(strcmp(event->actions[1].key, "reply") == 0 && strcmp(event->actions[1].label, "Reply") == 0);
	uint64_t first_revision = event->revision;
	assert(first_revision != 0);
	np_notify_event_free(event);
	/* Actions are valid only while the ID is active and for a posted key. */
	np_notify_service_emit_action(service, first, first_revision, "reply");
	wait_for(&log.action);
	assert(log.action_id == first && strcmp(log.action_key, "reply") == 0);
	log.action = FALSE;
#ifdef NP_NOTIFICATIONS_LIFECYCLE_TEST
	test_optional_feedback(service, first, first_revision);
#endif

	/* replaces_id keeps the identifier; a new notification gets the next one. */
	reply = call(client, "Notify", notify_args("Mail", first, "Updated", "", NULL, no_hints(), -1), "(u)", &error);
	guint replaced;
	g_variant_get(reply, "(u)", &replaced);
	g_variant_unref(reply);
	assert(replaced == first);
	event = take(NP_NOTIFY_EVENT_POSTED);
	assert(event->id == first && event->urgency == 1 && event->timeout_ms == -1);
	assert(event->desktop_entry[0] == '\0' && event->action_count == 0);
	uint64_t replacement_revision = event->revision;
	assert(replacement_revision > first_revision);
	np_notify_event_free(event);
	np_notify_service_emit_action(service, first, first_revision, "reply");
	np_notify_service_emit_closed(service, first, first_revision, NP_NOTIFY_CLOSED_DISMISSED);
	for (int i = 0; i < 30; i++) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
	assert(!log.action && !log.closed);
	assert(np_notify_service_is_current(service, first, replacement_revision));
	reply = call(client, "Notify", notify_args("Mail", 0, "Other", "", NULL, no_hints(), 0), "(u)", &error);
	guint second;
	g_variant_get(reply, "(u)", &second);
	g_variant_unref(reply);
	assert(second == first + 1);
	np_notify_event_free(take(NP_NOTIFY_EVENT_POSTED));

	/* Oversize text is cut on a character boundary and stays valid UTF-8. */
	GString *big = g_string_new(NULL);
	for (int i = 0; i < 400; i++) g_string_append(big, "\xC3\xA9"); /* 800 bytes */
	GString *huge = g_string_new(NULL);
	for (int i = 0; i < 3000; i++) g_string_append(huge, "\xE4\xBD\xA0"); /* 9000 bytes */
	reply = call(client, "Notify", notify_args("A", 0, big->str, huge->str, NULL, no_hints(), -1), "(u)", &error);
	assert(reply);
	g_variant_unref(reply);
	event = take(NP_NOTIFY_EVENT_POSTED);
	assert(strlen(event->summary) <= NP_NOTIFY_MAX_SUMMARY_BYTES && g_utf8_validate(event->summary, -1, NULL));
	assert(strlen(event->body) <= NP_NOTIFY_MAX_BODY_BYTES && g_utf8_validate(event->body, -1, NULL));
	assert(strlen(event->summary) == 512 && strlen(event->body) == 4095 /* 1365 three-byte characters */);
	np_notify_event_free(event);
	g_string_free(big, TRUE);
	g_string_free(huge, TRUE);

	/* At most eight actions and a sane urgency. */
	const gchar *terminated[41];
	gchar labels[40][8];
	for (int i = 0; i < 40; i++) {
		snprintf(labels[i], sizeof(labels[0]), "%c%d", i % 2 ? 'L' : 'k', i / 2);
		terminated[i] = labels[i]; /* 20 key, label pairs */
	}
	terminated[40] = NULL;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&builder, "{sv}", "urgency", g_variant_new_byte(7));
	reply = call(client, "Notify", notify_args("A", 0, "x", "", terminated, g_variant_builder_end(&builder), -9), "(u)", &error);
	assert(reply);
	g_variant_unref(reply);
	event = take(NP_NOTIFY_EVENT_POSTED);
	assert(event->action_count == NP_NOTIFY_MAX_ACTIONS);
	assert(event->urgency == 1 && event->timeout_ms == -1);
	np_notify_event_free(event);

	char oversized_key[NP_NOTIFY_MAX_ACTION_BYTES + 2];
	memset(oversized_key, 'k', sizeof(oversized_key) - 1); oversized_key[sizeof(oversized_key) - 1] = 0;
	const gchar *key_boundaries[] = {oversized_key, "Skipped", " original ", "Original", NULL};
	reply = call(client, "Notify", notify_args("A", 0, "Keys", "", key_boundaries, no_hints(), -1), "(u)", &error);
	assert(reply); g_variant_unref(reply);
	event = take(NP_NOTIFY_EVENT_POSTED);
	assert(event->action_count == 1 && strcmp(event->actions[0].key, " original ") == 0);
	np_notify_event_free(event);

	/* An odd trailing action entry (a key without a label) is ignored. */
	const gchar *odd[] = {"only-key", NULL};
	reply = call(client, "Notify", notify_args("A", 0, "x", "", odd, no_hints(), -1), "(u)", &error);
	g_variant_unref(reply);
	event = take(NP_NOTIFY_EVENT_POSTED);
	assert(event->action_count == 0);
	np_notify_event_free(event);

	/* CloseNotification reaches the host and is echoed as a signal. */
	reply = call(client, "CloseNotification", g_variant_new("(u)", second), NULL, &error);
	assert(reply);
	g_variant_unref(reply);
	event = take(NP_NOTIFY_EVENT_CLOSED);
	assert(event->id == second);
	np_notify_event_free(event);
	wait_for(&log.closed);
	assert(log.closed_id == second && log.closed_reason == NP_NOTIFY_CLOSED_BY_CALL);
	assert(!np_notify_service_is_active(service, second));
	expect_close_error(client, second);

	/* What the user did on the Mac arrives as the standard signals. */
	log.closed = FALSE;
	np_notify_service_emit_closed(service, first, replacement_revision, NP_NOTIFY_CLOSED_DISMISSED);
	wait_for(&log.closed);
	assert(log.closed_id == first && log.closed_reason == NP_NOTIFY_CLOSED_DISMISSED);
	assert(!np_notify_service_is_active(service, first));
	np_notify_service_emit_action(service, first, first_revision, "reply");
	for (int i = 0; i < 30; i++) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
	assert(!log.action);
	expect_close_error(client, first);

	/* A nonexistent replacement allocates a fresh ID, never the arbitrary
	 * caller-provided one. A rejected delivery leaves no active ID. */
	reply = call(client, "Notify", notify_args("A", UINT32_MAX, "Fresh", "", NULL, no_hints(), -1), "(u)", &error);
	assert(reply);
	guint fresh; g_variant_get(reply, "(u)", &fresh); g_variant_unref(reply);
	assert(fresh != 0 && fresh != UINT32_MAX && np_notify_service_is_active(service, fresh));
	np_notify_event_free(take(NP_NOTIFY_EVENT_POSTED));
	g_mutex_lock(&lock); reject_next = TRUE; g_mutex_unlock(&lock);
	reply = call(client, "Notify", notify_args("A", 0, "Rejected", "", NULL, no_hints(), -1), "(u)", &error);
	assert(!reply && error && !np_notify_service_is_active(service, rejected_id));
	g_clear_error(&error);
	g_mutex_lock(&lock); reject_next = TRUE; g_mutex_unlock(&lock);
	reply = call(client, "CloseNotification", g_variant_new("(u)", fresh), NULL, &error);
	assert(!reply && error && np_notify_service_is_active(service, fresh));
	g_clear_error(&error);

	/* IDs, not just queued events, are bounded. Replacing at capacity still
	 * works; host closure releases one slot for the next application. */
	unsigned accepted = 0;
	for (unsigned i = 0; i <= NP_NOTIFY_MAX_ACTIVE; i++) {
		reply = call(client, "Notify", notify_args("A", 0, "Fill", "", NULL, no_hints(), -1), "(u)", &error);
		if (!reply) { assert(error); g_clear_error(&error); break; }
		g_variant_unref(reply); accepted++;
		np_notify_event_free(take(NP_NOTIFY_EVENT_POSTED));
	}
	assert(accepted > 0 && accepted < NP_NOTIFY_MAX_ACTIVE);
	reply = call(client, "Notify", notify_args("A", fresh, "Replace at capacity", "", NULL, no_hints(), -1), "(u)", &error);
	assert(reply); g_variant_unref(reply);
	event = take(NP_NOTIFY_EVENT_POSTED);
	uint64_t fresh_revision = event->revision; np_notify_event_free(event);
	log.closed = FALSE;
	np_notify_service_emit_closed(service, fresh, fresh_revision, NP_NOTIFY_CLOSED_EXPIRED);
	wait_for(&log.closed);
	assert(log.closed_id == fresh && log.closed_reason == NP_NOTIFY_CLOSED_EXPIRED);
	reply = call(client, "Notify", notify_args("A", 0, "After closure", "", NULL, no_hints(), -1), "(u)", &error);
	assert(reply); g_variant_unref(reply);
	np_notify_event_free(take(NP_NOTIFY_EVENT_POSTED));

	/* A host-session reset closes every active ID on D-Bus and clears old
	 * pending state atomically. IDs are not reused in the next host session. */
	unsigned closed_before = log.closed_count;
	np_notify_service_reset(service);
	np_notify_event_free(take(NP_NOTIFY_EVENT_RESET));
	for (int i = 0; i < 500 && log.closed_count < closed_before + NP_NOTIFY_MAX_ACTIVE; i++) {
		while (g_main_context_iteration(NULL, FALSE)) {}
		g_usleep(1000);
	}
	assert(log.closed_count == closed_before + NP_NOTIFY_MAX_ACTIVE);
	assert(log.closed_reason == NP_NOTIFY_CLOSED_UNDEFINED);
	expect_close_error(client, log.closed_id);
	reply = call(client, "Notify", notify_args("A", 0, "New session", "", NULL, no_hints(), -1), "(u)", &error);
	assert(reply); g_variant_unref(reply);
#ifdef NP_NOTIFICATIONS_LIFECYCLE_TEST
	test_optional_sender_refusal(take(NP_NOTIFY_EVENT_POSTED));
#else
	np_notify_event_free(take(NP_NOTIFY_EVENT_POSTED));
#endif

	/* Unknown methods fail rather than hang. */
	GVariant *bad = call(client, "NoSuchMethod", NULL, NULL, &error);
	assert(!bad && error);
	g_clear_error(&error);

	/* Bounded text helper. */
	char *cut = np_notify_bounded_text("a\xC3\xA9", 2);
	assert(strcmp(cut, "a") == 0);
	g_free(cut);
	cut = np_notify_bounded_text(NULL, 10);
	assert(strcmp(cut, "") == 0);
	g_free(cut);
	char *oversized = g_malloc0(16 * 1024 * 1024 + 1);
	memset(oversized, 'x', 16 * 1024 * 1024);
	cut = np_notify_bounded_text(oversized, NP_NOTIFY_MAX_BODY_BYTES);
	assert(strlen(cut) == NP_NOTIFY_MAX_BODY_BYTES);
#ifdef __linux__
	assert(malloc_usable_size(cut) <= NP_NOTIFY_MAX_BODY_BYTES * 4);
#endif
	g_free(cut); g_free(oversized);

	g_object_unref(client);
	g_main_loop_quit(server_loop);
	g_thread_join(thread);
	g_object_unref(server);
	np_notify_service_free(service);
	g_free(client_address);
	g_main_loop_unref(server_loop); g_main_context_unref(server_context);
	g_ptr_array_unref(events);
	puts("notify_dbus_test: ok");
	return 0;
}
