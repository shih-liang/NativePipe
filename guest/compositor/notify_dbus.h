#ifndef NATIVEPIPE_NOTIFY_DBUS_H
#define NATIVEPIPE_NOTIFY_DBUS_H

/*
 * org.freedesktop.Notifications for the guest session.
 *
 * Linux applications (GTK, Qt, Chromium, Firefox) post desktop notifications on
 * the session bus. Nothing in the guest owns that name, so they were lost. This
 * module is the server: it turns each call into a bounded np_notify_event for
 * the host, and reports what the user did back to the application as the
 * standard NotificationClosed and ActionInvoked signals.
 *
 * It depends on GIO only, never on Wayland, so it is testable on its own.
 */

#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NP_NOTIFY_MAX_ACTIONS 8
#define NP_NOTIFY_MAX_NAME_BYTES 128
#define NP_NOTIFY_MAX_SUMMARY_BYTES 512
#define NP_NOTIFY_MAX_BODY_BYTES 4096
#define NP_NOTIFY_MAX_ACTION_BYTES 128
#define NP_NOTIFY_MAX_ACTIVE 64

/* Values of the NotificationClosed signal. */
#define NP_NOTIFY_CLOSED_EXPIRED 1u
#define NP_NOTIFY_CLOSED_DISMISSED 2u
#define NP_NOTIFY_CLOSED_BY_CALL 3u
#define NP_NOTIFY_CLOSED_UNDEFINED 4u

enum np_notify_event_type {
	NP_NOTIFY_EVENT_POSTED = 1,
	NP_NOTIFY_EVENT_CLOSED = 2,
	/* Internal only: discard pending state at the host-session boundary. */
	NP_NOTIFY_EVENT_RESET = 3,
};

struct np_notify_action {
	char *key;
	char *label;
};

/* Every string is owned, valid UTF-8 and bounded by the limits above. */
struct np_notify_event {
	enum np_notify_event_type type;
	uint32_t id;
	uint64_t revision;
	uint8_t urgency;     /* 0 low, 1 normal, 2 critical */
	int32_t timeout_ms;  /* -1 server default, 0 never expires */
	char *app_name;
	char *desktop_entry;
	char *summary;
	char *body;
	struct np_notify_action actions[NP_NOTIFY_MAX_ACTIONS];
	unsigned action_count;
};

/* Called on the service thread. The callee always owns and frees the event.
 * Returning false rejects the request without adding/changing an active ID. */
typedef bool (*np_notify_event_fn)(struct np_notify_event *event, void *user);

struct np_notify_service;

struct np_notify_service *np_notify_service_new(np_notify_event_fn deliver, void *user);
void np_notify_service_free(struct np_notify_service *service);

/* Serves the interface on an existing connection. The session bus path below
 * uses it, and so does the point-to-point connection in the test. */
bool np_notify_service_register(struct np_notify_service *service,
                                GDBusConnection *connection, GError **error);

/* Claims org.freedesktop.Notifications on the session bus, without queueing: a
 * machine whose own desktop already runs a notification daemon keeps it. Uses a private connection in the calling
 * thread's thread-default main context; stop drains all ownership callbacks. */
void np_notify_service_start(struct np_notify_service *service, GMainLoop *loop);
/* The compositor waits at most one second before launching applications, so
 * their first Notify cannot race another daemon's D-Bus activation. */
bool np_notify_service_wait_ready(struct np_notify_service *service, gint64 deadline);
/* On the owner context: close only our private bus connection and drain GIO's
 * ownership callbacks before quitting the loop. */
void np_notify_service_stop(struct np_notify_service *service);

/* Safe from any thread. */
bool np_notify_service_is_active(struct np_notify_service *service, uint32_t id);
bool np_notify_service_is_current(struct np_notify_service *service, uint32_t id, uint64_t revision);
void np_notify_service_reset(struct np_notify_service *service);
void np_notify_service_emit_closed(struct np_notify_service *service, uint32_t id, uint64_t revision, uint32_t reason);
void np_notify_service_emit_action(struct np_notify_service *service, uint32_t id, uint64_t revision, const char *key);

void np_notify_event_free(struct np_notify_event *event);

/* Valid UTF-8 of at most max_bytes, cut on a character boundary. Never NULL. */
char *np_notify_bounded_text(const char *text, size_t max_bytes);

#endif
