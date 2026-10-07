#include "notify_dbus.h"

#include <stdio.h>
#include <string.h>

#ifndef NATIVEPIPE_PRODUCT_VERSION
#error "NATIVEPIPE_PRODUCT_VERSION must come from the product VERSION file"
#endif

#define NOTIFICATIONS_NAME "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"

static const char introspection_xml[] =
	"<node>"
	" <interface name='org.freedesktop.Notifications'>"
	"  <method name='GetCapabilities'>"
	"   <arg type='as' name='capabilities' direction='out'/>"
	"  </method>"
	"  <method name='Notify'>"
	"   <arg type='s' name='app_name' direction='in'/>"
	"   <arg type='u' name='replaces_id' direction='in'/>"
	"   <arg type='s' name='app_icon' direction='in'/>"
	"   <arg type='s' name='summary' direction='in'/>"
	"   <arg type='s' name='body' direction='in'/>"
	"   <arg type='as' name='actions' direction='in'/>"
	"   <arg type='a{sv}' name='hints' direction='in'/>"
	"   <arg type='i' name='expire_timeout' direction='in'/>"
	"   <arg type='u' name='id' direction='out'/>"
	"  </method>"
	"  <method name='CloseNotification'>"
	"   <arg type='u' name='id' direction='in'/>"
	"  </method>"
	"  <method name='GetServerInformation'>"
	"   <arg type='s' name='name' direction='out'/>"
	"   <arg type='s' name='vendor' direction='out'/>"
	"   <arg type='s' name='version' direction='out'/>"
	"   <arg type='s' name='spec_version' direction='out'/>"
	"  </method>"
	"  <signal name='NotificationClosed'>"
	"   <arg type='u' name='id'/>"
	"   <arg type='u' name='reason'/>"
	"  </signal>"
	"  <signal name='ActionInvoked'>"
	"   <arg type='u' name='id'/>"
	"   <arg type='s' name='action_key'/>"
	"  </signal>"
	" </interface>"
	"</node>";

struct np_notify_service {
	np_notify_event_fn deliver;
	void *user;
	GMutex lock;
	GCond ready;
	bool ready_done, ready_ok, stopping;
	bool connecting, closing, ownership_pending;
	GCancellable *cancellable;
	GMainLoop *loop; /* borrowed until stop drains all callbacks */
	GDBusConnection *connection; /* set once the object is registered */
	guint registration;
	guint owner;
	uint32_t next_id;
	uint64_t next_revision;
	GHashTable *active;
};

struct active_notification {
	uint64_t revision;
	unsigned action_count;
	char *keys[NP_NOTIFY_MAX_ACTIONS];
};

static void active_free(gpointer data)
{
	struct active_notification *active = data;
	for (unsigned i = 0; i < active->action_count; i++) g_free(active->keys[i]);
	g_free(active);
}

char *np_notify_bounded_text(const char *text, size_t max_bytes)
{
	if (!text) text = "";
	/* Validate only a bounded prefix; repairing the entire input first retains
	 * its full allocation even after an in-place truncation. */
	char *valid = g_utf8_make_valid(text, (gssize)strnlen(text, max_bytes));
	if (strlen(valid) <= max_bytes) return valid;
	/* Step back to the start of a character so the cut never splits one. */
	char *end = valid + max_bytes;
	while (end > valid && (((unsigned char)*end) & 0xC0) == 0x80) end--;
	char *bounded = g_strndup(valid, (gsize)(end - valid));
	g_free(valid);
	return bounded;
}

void np_notify_event_free(struct np_notify_event *event)
{
	if (!event) return;
	g_free(event->app_name);
	g_free(event->desktop_entry);
	g_free(event->summary);
	g_free(event->body);
	for (unsigned i = 0; i < event->action_count; i++) {
		g_free(event->actions[i].key);
		g_free(event->actions[i].label);
	}
	g_free(event);
}

struct np_notify_service *np_notify_service_new(np_notify_event_fn deliver, void *user)
{
	if (!deliver) return NULL;
	struct np_notify_service *service = g_new0(struct np_notify_service, 1);
	service->deliver = deliver;
	service->user = user;
	service->next_id = 1;
	service->next_revision = 1;
	service->active = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, active_free);
	g_mutex_init(&service->lock);
	g_cond_init(&service->ready);
	return service;
}

/* The caller owns service->lock. At most 64 IDs can be in use. */
static uint32_t allocate_id(struct np_notify_service *service)
{
	for (;;) {
		uint32_t id = service->next_id++;
		if (service->next_id == 0) service->next_id = 1;
		if (!g_hash_table_contains(service->active, GUINT_TO_POINTER(id))) return id;
	}
}

static void emit(struct np_notify_service *service, const char *signal, GVariant *parameters)
{
	g_variant_ref_sink(parameters);
	g_mutex_lock(&service->lock);
	GDBusConnection *connection = service->connection ? g_object_ref(service->connection) : NULL;
	g_mutex_unlock(&service->lock);
	if (connection) {
		g_dbus_connection_emit_signal(connection, NULL, NOTIFICATIONS_PATH,
		                              NOTIFICATIONS_NAME, signal, parameters, NULL);
		g_object_unref(connection);
	}
	g_variant_unref(parameters);
}

bool np_notify_service_is_active(struct np_notify_service *service, uint32_t id)
{
	if (!service || !id) return false;
	g_mutex_lock(&service->lock);
	bool active = g_hash_table_contains(service->active, GUINT_TO_POINTER(id));
	g_mutex_unlock(&service->lock);
	return active;
}

bool np_notify_service_is_current(struct np_notify_service *service, uint32_t id, uint64_t revision)
{
	if (!service || !id || !revision) return false;
	g_mutex_lock(&service->lock);
	struct active_notification *active = g_hash_table_lookup(service->active, GUINT_TO_POINTER(id));
	bool current = active && active->revision == revision;
	g_mutex_unlock(&service->lock);
	return current;
}

void np_notify_service_reset(struct np_notify_service *service)
{
	if (!service) return;
	uint32_t ids[NP_NOTIFY_MAX_ACTIVE];
	unsigned count = 0;
	g_mutex_lock(&service->lock);
	GHashTableIter iterator;
	gpointer key;
	g_hash_table_iter_init(&iterator, service->active);
	while (g_hash_table_iter_next(&iterator, &key, NULL) && count < NP_NOTIFY_MAX_ACTIVE)
		ids[count++] = GPOINTER_TO_UINT(key);
	/* Use the same lock order as delivery: service, then consumer queue. New
	 * Notify calls cannot slip between clearing IDs and clearing pending state. */
	struct np_notify_event *event = g_new0(struct np_notify_event, 1);
	event->type = NP_NOTIFY_EVENT_RESET;
	(void)service->deliver(event, service->user);
	g_hash_table_remove_all(service->active);
	g_mutex_unlock(&service->lock);
	for (unsigned i = 0; i < count; i++)
		emit(service, "NotificationClosed", g_variant_new("(uu)", ids[i], NP_NOTIFY_CLOSED_UNDEFINED));
}

void np_notify_service_emit_closed(struct np_notify_service *service, uint32_t id, uint64_t revision, uint32_t reason)
{
	if (!service || !id || !revision || reason < NP_NOTIFY_CLOSED_EXPIRED || reason > NP_NOTIFY_CLOSED_UNDEFINED) return;
	g_mutex_lock(&service->lock);
	struct active_notification *active = g_hash_table_lookup(service->active, GUINT_TO_POINTER(id));
	bool removed = active && active->revision == revision && g_hash_table_remove(service->active, GUINT_TO_POINTER(id));
	g_mutex_unlock(&service->lock);
	if (removed) emit(service, "NotificationClosed", g_variant_new("(uu)", id, reason));
}

void np_notify_service_emit_action(struct np_notify_service *service, uint32_t id, uint64_t revision, const char *key)
{
	if (!service || !id || !revision || !key || strnlen(key, NP_NOTIFY_MAX_ACTION_BYTES + 1) > NP_NOTIFY_MAX_ACTION_BYTES ||
	    !g_utf8_validate(key, -1, NULL)) return;
	g_mutex_lock(&service->lock);
	struct active_notification *active = g_hash_table_lookup(service->active, GUINT_TO_POINTER(id));
	bool found = false;
	for (unsigned i = 0; active && active->revision == revision && i < active->action_count; i++)
		if (strcmp(active->keys[i], key) == 0) found = true;
	g_mutex_unlock(&service->lock);
	if (found) emit(service, "ActionInvoked", g_variant_new("(us)", id, key));
}

static void handle_notify(struct np_notify_service *service, GVariant *parameters,
                          GDBusMethodInvocation *invocation)
{
	const gchar *app_name = NULL, *icon = NULL, *summary = NULL, *body = NULL;
	guint32 replaces_id = 0;
	gint32 timeout = -1;
	GVariant *actions = NULL;
	GVariant *hints = NULL;
	g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app_name, &replaces_id, &icon,
	              &summary, &body, &actions, &hints, &timeout);

	struct np_notify_event *event = g_new0(struct np_notify_event, 1);
	event->type = NP_NOTIFY_EVENT_POSTED;
	event->timeout_ms = timeout < -1 ? -1 : timeout;
	event->urgency = 1;
	guint8 urgency = 1;
	if (g_variant_lookup(hints, "urgency", "y", &urgency) && urgency <= 2) event->urgency = urgency;
	const gchar *entry = NULL;
	if (!g_variant_lookup(hints, "desktop-entry", "&s", &entry)) entry = NULL;
	event->app_name = np_notify_bounded_text(app_name, NP_NOTIFY_MAX_NAME_BYTES);
	event->desktop_entry = np_notify_bounded_text(entry, NP_NOTIFY_MAX_NAME_BYTES);
	event->summary = np_notify_bounded_text(summary, NP_NOTIFY_MAX_SUMMARY_BYTES);
	event->body = np_notify_bounded_text(body, NP_NOTIFY_MAX_BODY_BYTES);
	/* The spec sends actions as a flat list of key, label pairs. */
	for (gsize i = 0; i + 1 < g_variant_n_children(actions) && i < NP_NOTIFY_MAX_ACTIONS * 2 &&
	                    event->action_count < NP_NOTIFY_MAX_ACTIONS; i += 2) {
		const gchar *key = NULL, *label = NULL;
		g_variant_get_child(actions, i, "&s", &key);
		g_variant_get_child(actions, i + 1, "&s", &label);
		/* Action keys are identifiers, not display text. Truncating one would
		 * send ActionInvoked with a different key than the application posted. */
		if (!key[0] || strnlen(key, NP_NOTIFY_MAX_ACTION_BYTES + 1) > NP_NOTIFY_MAX_ACTION_BYTES) continue;
		struct np_notify_action *action = &event->actions[event->action_count++];
		action->key = g_strdup(key);
		action->label = np_notify_bounded_text(label, NP_NOTIFY_MAX_ACTION_BYTES);
	}
	g_variant_unref(hints);
	g_variant_unref(actions);
	struct active_notification *active = g_new0(struct active_notification, 1);
	active->action_count = event->action_count;
	for (unsigned i = 0; i < active->action_count; i++) active->keys[i] = g_strdup(event->actions[i].key);
	g_mutex_lock(&service->lock);
	bool replacing = replaces_id && g_hash_table_contains(service->active, GUINT_TO_POINTER(replaces_id));
	if (!replacing && g_hash_table_size(service->active) >= NP_NOTIFY_MAX_ACTIVE) {
		g_mutex_unlock(&service->lock);
		active_free(active);
		np_notify_event_free(event);
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED,
		                                      "Too many active notifications");
		return;
	}
	uint32_t id = event->id = replacing ? replaces_id : allocate_id(service);
	event->revision = active->revision = service->next_revision++;
	if (!service->next_revision) service->next_revision = 1;
	bool accepted = service->deliver(event, service->user);
	if (accepted) g_hash_table_replace(service->active, GUINT_TO_POINTER(id), active);
	g_mutex_unlock(&service->lock);
	if (accepted) g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", id));
	else {
		active_free(active);
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED,
		                                      "The notification queue is full");
	}
}

static void handle_method_call(GDBusConnection *connection, const gchar *sender,
                               const gchar *object_path, const gchar *interface_name,
                               const gchar *method_name, GVariant *parameters,
                               GDBusMethodInvocation *invocation, gpointer user_data)
{
	(void)connection; (void)sender; (void)object_path; (void)interface_name;
	struct np_notify_service *service = user_data;
	if (g_strcmp0(method_name, "GetCapabilities") == 0) {
		/* Text and buttons are shown; markup is stripped by the host and no
		 * images or sounds are forwarded, so those are not advertised. */
		static const gchar *const capabilities[] = {"body", "actions", NULL};
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", capabilities));
	} else if (g_strcmp0(method_name, "Notify") == 0) {
		handle_notify(service, parameters, invocation);
	} else if (g_strcmp0(method_name, "CloseNotification") == 0) {
		guint32 id = 0;
		g_variant_get(parameters, "(u)", &id);
		g_mutex_lock(&service->lock);
		if (!id || !g_hash_table_contains(service->active, GUINT_TO_POINTER(id))) {
			g_mutex_unlock(&service->lock);
			g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			                                      "No active notification with ID %u", id);
			return;
		}
		struct np_notify_event *event = g_new0(struct np_notify_event, 1);
		event->type = NP_NOTIFY_EVENT_CLOSED;
		event->id = id;
		event->revision = ((struct active_notification *)g_hash_table_lookup(service->active, GUINT_TO_POINTER(id)))->revision;
		bool accepted = service->deliver(event, service->user);
		if (accepted) g_hash_table_remove(service->active, GUINT_TO_POINTER(id));
		g_mutex_unlock(&service->lock);
		if (accepted) {
			g_dbus_method_invocation_return_value(invocation, NULL);
			emit(service, "NotificationClosed", g_variant_new("(uu)", id, NP_NOTIFY_CLOSED_BY_CALL));
		} else g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED,
		                                           "The notification queue is full");
	} else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
		g_dbus_method_invocation_return_value(
			invocation, g_variant_new("(ssss)", "NativePipe", "NativePipe", NATIVEPIPE_PRODUCT_VERSION, "1.2"));
	} else {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
		                                      "Unknown method %s", method_name);
	}
}

bool np_notify_service_register(struct np_notify_service *service,
                                GDBusConnection *connection, GError **error)
{
	if (!service || !connection) return false;
	g_mutex_lock(&service->lock);
	bool already_registered = service->connection == connection && service->registration != 0;
	g_mutex_unlock(&service->lock);
	if (already_registered) return true;
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(introspection_xml, error);
	if (!node) return false;
	static const GDBusInterfaceVTable vtable = {handle_method_call, NULL, NULL, {0}};
	guint registration = g_dbus_connection_register_object(
		connection, NOTIFICATIONS_PATH, node->interfaces[0], &vtable, service, NULL, error);
	g_dbus_node_info_unref(node);
	if (!registration) return false;
	g_mutex_lock(&service->lock);
	GDBusConnection *previous = service->connection;
	guint previous_registration = service->registration;
	service->connection = g_object_ref(connection);
	service->registration = registration;
	g_mutex_unlock(&service->lock);
	if (previous) {
		if (previous_registration) g_dbus_connection_unregister_object(previous, previous_registration);
		g_object_unref(previous);
	}
	return true;
}

static void set_ready(struct np_notify_service *service, bool ok)
{
	g_mutex_lock(&service->lock);
	if (!service->ready_done) {
		service->ready_done = true;
		service->ready_ok = ok && !service->stopping;
		g_cond_broadcast(&service->ready);
	}
	g_mutex_unlock(&service->lock);
}

static void stopped_if_drained(struct np_notify_service *service)
{
	/* The private context must run until GIO's ownership destroy callback,
	 * including cancelled connection/name-request callbacks, has completed. */
	if (service->stopping && !service->connecting && !service->closing &&
	    !service->ownership_pending) g_main_loop_quit(service->loop);
}

static void owner_destroyed(gpointer data)
{
	struct np_notify_service *service = data;
	service->ownership_pending = false;
	stopped_if_drained(service);
}

static void name_acquired(GDBusConnection *connection, const gchar *name, gpointer data)
{
	(void)connection; (void)name;
	set_ready(data, true);
}

static void name_lost(GDBusConnection *connection, const gchar *name, gpointer data)
{
	(void)connection; (void)name;
	set_ready(data, false);
}

static void connection_closed(GObject *source, GAsyncResult *result, gpointer data)
{
	struct np_notify_service *service = data;
	GError *error = NULL;
	g_dbus_connection_close_finish(G_DBUS_CONNECTION(source), result, &error);
	g_clear_error(&error);
	service->closing = false;
	/* Closing the dedicated connection first aborts any pending RequestName
	 * and makes unown_name's ReleaseName incapable of waiting on a dead bus. */
	guint owner = service->owner;
	service->owner = 0;
	if (owner) g_bus_unown_name(owner);
	stopped_if_drained(service);
}

static void close_connection(struct np_notify_service *service)
{
	if (service->connection && !service->closing) {
		service->closing = true;
		g_dbus_connection_close(service->connection, NULL, connection_closed, service);
	}
}

static void connected(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)source;
	struct np_notify_service *service = data;
	GError *error = NULL;
	GDBusConnection *connection = g_dbus_connection_new_for_address_finish(result, &error);
	service->connecting = false;
	if (!connection) {
		g_clear_error(&error);
		set_ready(service, false);
		stopped_if_drained(service);
		return;
	}
	g_dbus_connection_set_exit_on_close(connection, FALSE);
	if (!np_notify_service_register(service, connection, &error)) {
		g_clear_error(&error);
		/* Retain the private connection so shutdown drains its close callback. */
		service->connection = g_object_ref(connection);
		set_ready(service, false);
	} else if (!service->stopping) {
		service->ownership_pending = true;
		service->owner = g_bus_own_name_on_connection(connection, NOTIFICATIONS_NAME,
			G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, name_acquired, name_lost, service, owner_destroyed);
	}
	g_object_unref(connection);
	if (service->stopping) close_connection(service);
}

void np_notify_service_start(struct np_notify_service *service, GMainLoop *loop)
{
	service->loop = loop;
	service->cancellable = g_cancellable_new();
	service->connecting = true;
	g_dbus_connection_new_for_address(getenv("DBUS_SESSION_BUS_ADDRESS"),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
		NULL, service->cancellable, connected, service);
}

bool np_notify_service_wait_ready(struct np_notify_service *service, gint64 deadline)
{
	g_mutex_lock(&service->lock);
	while (!service->ready_done && g_cond_wait_until(&service->ready, &service->lock, deadline)) {}
	bool ready = service->ready_done && service->ready_ok;
	g_mutex_unlock(&service->lock);
	return ready;
}

void np_notify_service_stop(struct np_notify_service *service)
{
	/* Runs on the same context as GIO, including finish-before-start. */
	g_mutex_lock(&service->lock);
	service->stopping = true;
	g_mutex_unlock(&service->lock);
	set_ready(service, false);
	if (service->cancellable) g_cancellable_cancel(service->cancellable);
	close_connection(service);
	stopped_if_drained(service);
}

void np_notify_service_free(struct np_notify_service *service)
{
	if (!service) return;
	g_assert(!service->owner && !service->ownership_pending && !service->connecting && !service->closing);
	g_clear_object(&service->cancellable);
	g_mutex_lock(&service->lock);
	GDBusConnection *connection = service->connection;
	guint registration = service->registration;
	service->connection = NULL;
	service->registration = 0;
	g_mutex_unlock(&service->lock);
	if (connection) {
		if (registration) g_dbus_connection_unregister_object(connection, registration);
		g_object_unref(connection);
	}
	g_cond_clear(&service->ready);
	g_mutex_clear(&service->lock);
	g_hash_table_unref(service->active);
	g_free(service);
}
