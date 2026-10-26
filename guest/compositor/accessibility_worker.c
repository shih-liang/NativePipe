#define _GNU_SOURCE
#include "accessibility_worker.h"
#include "windowwire.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define ACCESSIBLE "org.a11y.atspi.Accessible"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define REGISTRY "org.a11y.atspi.Registry"
#define ROOT_PATH "/org/a11y/atspi/accessible/root"
#define NULL_PATH "/org/a11y/atspi/null"
#define CALL_TIMEOUT_MS 2000
/* The host gives up after 25 seconds; a partial tree is more useful than none. */
#define REQUEST_BUDGET_US (10 * G_USEC_PER_SEC)
#define MAX_NODES 2000u
#define MAX_CHILDREN 256
#define MAX_DEPTH 64u
#define TREE_TEXT 1024
#define FULL_TEXT 65536
#define COORD_TYPE_WINDOW 1u

enum {
    STATE_ACTIVE = 1, STATE_DEFUNCT = 6, STATE_EDITABLE = 7, STATE_SHOWING = 25,
};
/* AtspiStateType order. Bits that never help an agent choose are omitted. */
static const char *const state_names[] = {
    NULL, "active", "armed", "busy", "checked", "collapsed", "defunct", "editable",
    "enabled", "expandable", "expanded", "focusable", "focused", NULL, NULL,
    "iconified", "modal", "multi-line", "multiselectable", NULL, "pressed",
    "resizable", "selectable", "selected", NULL, "showing", NULL, NULL,
    "transient", NULL, "visible", "manages-descendants", "indeterminate",
    "required", "truncated", "animated", "invalid-entry",
    "supports-autocompletion", NULL, "default", "visited", "checkable",
    "has-popup", "read-only",
};

struct np_a11y {
    GThread *thread;
    GAsyncQueue *requests, *replies;
    GMutex lock;
    gboolean stopping;
    int notify[2];
    char *address;
    GCancellable *cancel;
    /* Worker thread only. */
    GDBusConnection *bus;
    gboolean announced;
};

/* The window one request addresses, and the request's output. */
struct job {
    struct np_a11y *a;
    const struct np_a11y_request *r;
    gint64 deadline;
    GString *json;
    unsigned nodes;
    gboolean truncated;
    char *bus, *window, *application, *toolkit;
    int32_t pid, offset_x, offset_y;
};

static struct np_a11y_request stop_request;

static void free_request(struct np_a11y_request *r)
{
    if (!r || r == &stop_request) return;
    g_free(r->title); g_free(r->app_id); g_free(r->element); g_free(r->argument);
    g_free(r);
}

static void wake(int fd)
{
    char byte = 1;
    while (write(fd, &byte, 1) < 0 && errno == EINTR) {}
}

static GVariant *call(struct job *j, const char *bus, const char *path, const char *interface,
                      const char *method, GVariant *args, const char *type, GError **error)
{
    gint64 left = (j->deadline - g_get_monotonic_time()) / 1000;
    if (left <= 0) {
        if (args) g_variant_unref(g_variant_ref_sink(args));
        j->truncated = TRUE;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                            "The application took too long to answer accessibility requests.");
        return NULL;
    }
    /* The registry daemon may be activated by this first call. */
    return g_dbus_connection_call_sync(j->a->bus, bus, path, interface, method, args,
        type ? G_VARIANT_TYPE(type) : NULL, G_DBUS_CALL_FLAGS_NONE,
        (int)MIN(left, CALL_TIMEOUT_MS), j->a->cancel, error);
}

static GVariant *property(struct job *j, const char *bus, const char *path,
                          const char *interface, const char *name)
{
    GVariant *reply = call(j, bus, path, PROPERTIES, "Get",
                           g_variant_new("(ss)", interface, name), "(v)", NULL);
    if (!reply) return NULL;
    GVariant *value = NULL;
    g_variant_get(reply, "(v)", &value);
    g_variant_unref(reply);
    return value;
}

static char *string_property(struct job *j, const char *bus, const char *path,
                             const char *interface, const char *name)
{
    GVariant *value = property(j, bus, path, interface, name);
    char *text = value && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)
        ? g_variant_dup_string(value, NULL) : g_strdup("");
    if (value) g_variant_unref(value);
    return text;
}

static int32_t int_property(struct job *j, const char *bus, const char *path,
                            const char *interface, const char *name)
{
    GVariant *value = property(j, bus, path, interface, name);
    int32_t result = value && g_variant_is_of_type(value, G_VARIANT_TYPE_INT32)
        ? g_variant_get_int32(value) : -1;
    if (value) g_variant_unref(value);
    return result;
}

static char *string_call(struct job *j, const char *bus, const char *path,
                         const char *interface, const char *method, GVariant *args)
{
    GVariant *reply = call(j, bus, path, interface, method, args, "(s)", NULL);
    char *text = NULL;
    if (reply) { g_variant_get(reply, "(s)", &text); g_variant_unref(reply); }
    return text ? text : g_strdup("");
}

static gboolean bool_call(struct job *j, const char *bus, const char *path, const char *interface,
                          const char *method, GVariant *args, GError **error)
{
    GVariant *reply = call(j, bus, path, interface, method, args, "(b)", error);
    gboolean result = FALSE;
    if (reply) { g_variant_get(reply, "(b)", &result); g_variant_unref(reply); }
    return result;
}

static gboolean state(struct job *j, const char *bus, const char *path, guint64 *states)
{
    GVariant *reply = call(j, bus, path, ACCESSIBLE, "GetState", NULL, "(au)", NULL);
    if (!reply) return FALSE;
    GVariantIter *words;
    guint32 word;
    unsigned index = 0;
    *states = 0;
    g_variant_get(reply, "(au)", &words);
    while (g_variant_iter_next(words, "u", &word))
        if (index < 2) *states |= (guint64)word << (32 * index++);
    g_variant_iter_free(words);
    g_variant_unref(reply);
    return TRUE;
}

#define HAS(states, bit) (((states) >> (bit)) & 1u)

/* Object references are (bus name, path) pairs; NULL_PATH means none. */
static GPtrArray *children(struct job *j, const char *bus, const char *path, int *total)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
    *total = 0;
    GVariant *reply = call(j, bus, path, ACCESSIBLE, "GetChildren", NULL, "(a(so))", NULL);
    if (reply) {
        GVariantIter *items;
        const char *child_bus, *child_path;
        g_variant_get(reply, "(a(so))", &items);
        while (g_variant_iter_next(items, "(&s&o)", &child_bus, &child_path)) {
            if (!strcmp(child_path, NULL_PATH)) continue;
            if ((*total)++ < MAX_CHILDREN) {
                g_ptr_array_add(list, g_strdup(child_bus));
                g_ptr_array_add(list, g_strdup(child_path));
            }
        }
        g_variant_iter_free(items);
        g_variant_unref(reply);
        return list;
    }
    /* Older bridges answer only by index. */
    int count = int_property(j, bus, path, ACCESSIBLE, "ChildCount");
    *total = MAX(count, 0);
    for (int i = 0; i < MIN(count, MAX_CHILDREN); ++i) {
        GVariant *child = call(j, bus, path, ACCESSIBLE, "GetChildAtIndex",
                               g_variant_new("(i)", i), "((so))", NULL);
        if (!child) break;
        const char *child_bus, *child_path;
        g_variant_get(child, "((&s&o))", &child_bus, &child_path);
        if (strcmp(child_path, NULL_PATH)) {
            g_ptr_array_add(list, g_strdup(child_bus));
            g_ptr_array_add(list, g_strdup(child_path));
        }
        g_variant_unref(child);
    }
    return list;
}

static void json_string(GString *json, const char *text)
{
    g_string_append_c(json, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
        switch (*p) {
        case '"': g_string_append(json, "\\\""); break;
        case '\\': g_string_append(json, "\\\\"); break;
        case '\n': g_string_append(json, "\\n"); break;
        case '\r': g_string_append(json, "\\r"); break;
        case '\t': g_string_append(json, "\\t"); break;
        default:
            if (*p < 0x20) g_string_append_printf(json, "\\u%04x", *p);
            else g_string_append_c(json, (char)*p);
        }
    }
    g_string_append_c(json, '"');
}

static void json_number(GString *json, double value)
{
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    if (isfinite(value)) g_string_append(json, g_ascii_formatd(buffer, sizeof(buffer), "%.10g", value));
    else g_string_append(json, "null");
}

static void json_ref(GString *json, const char *bus, const char *path)
{
    char *ref = g_strconcat(bus, path, NULL);
    json_string(json, ref);
    g_free(ref);
}

static void append_value(struct job *j, const char *bus, const char *path)
{
    const char *names[] = {"CurrentValue", "MinimumValue", "MaximumValue"};
    const char *keys[] = {"value", "minimum", "maximum"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); ++i) {
        GVariant *value = property(j, bus, path, "org.a11y.atspi.Value", names[i]);
        if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_DOUBLE)) {
            g_string_append_printf(j->json, ",\"%s\":", keys[i]);
            json_number(j->json, g_variant_get_double(value));
        }
        if (value) g_variant_unref(value);
    }
}

static gboolean node(struct job *j, const char *bus, const char *path, unsigned depth, unsigned max_depth)
{
    if (j->nodes >= MAX_NODES || j->json->len > NP_A11Y_MAX_REPLY - 64 * 1024 ||
        g_get_monotonic_time() >= j->deadline) {
        j->truncated = TRUE;
        return FALSE;
    }
    guint64 states;
    if (!state(j, bus, path, &states) || HAS(states, STATE_DEFUNCT)) return FALSE;
    if (depth && !(j->r->flags & NP_A11Y_INCLUDE_HIDDEN) && !HAS(states, STATE_SHOWING)) return FALSE;
    ++j->nodes;

    char *role = string_call(j, bus, path, ACCESSIBLE, "GetRoleName", NULL);
    char *name = string_property(j, bus, path, ACCESSIBLE, "Name");
    char *description = string_property(j, bus, path, ACCESSIBLE, "Description");
    GVariant *reply = call(j, bus, path, ACCESSIBLE, "GetInterfaces", NULL, "(as)", NULL);
    char **interfaces = NULL;
    if (reply) { g_variant_get(reply, "(^as)", &interfaces); g_variant_unref(reply); }
    #define IMPLEMENTS(x) (interfaces && g_strv_contains((const char *const *)interfaces, "org.a11y.atspi." x))

    GString *json = j->json;
    g_string_append(json, "{\"ref\":");
    json_ref(json, bus, path);
    g_string_append(json, ",\"role\":");
    json_string(json, role);
    if (*name) { g_string_append(json, ",\"name\":"); json_string(json, name); }
    if (*description) { g_string_append(json, ",\"description\":"); json_string(json, description); }
    g_string_append(json, ",\"states\":[");
    gboolean first = TRUE;
    for (unsigned bit = 0; bit < G_N_ELEMENTS(state_names); ++bit) {
        if (!state_names[bit] || !HAS(states, bit)) continue;
        if (!first) g_string_append_c(json, ',');
        json_string(json, state_names[bit]);
        first = FALSE;
    }
    g_string_append_c(json, ']');

    if (IMPLEMENTS("Component")) {
        reply = call(j, bus, path, "org.a11y.atspi.Component", "GetExtents",
                     g_variant_new("(u)", COORD_TYPE_WINDOW), "((iiii))", NULL);
        if (reply) {
            int32_t x, y, width, height;
            g_variant_get(reply, "((iiii))", &x, &y, &width, &height);
            if (width > 0 && height > 0)
                g_string_append_printf(json, ",\"bounds\":[%d,%d,%d,%d]",
                                       x - j->offset_x, y - j->offset_y, width, height);
            g_variant_unref(reply);
        }
    }
    if (IMPLEMENTS("Action")) {
        reply = call(j, bus, path, "org.a11y.atspi.Action", "GetActions", NULL, "(a(sss))", NULL);
        if (reply) {
            GVariantIter *actions;
            const char *action, *ignored, *keys;
            g_variant_get(reply, "(a(sss))", &actions);
            g_string_append(json, ",\"actions\":[");
            first = TRUE;
            while (g_variant_iter_next(actions, "(&s&s&s)", &action, &ignored, &keys)) {
                if (!first) g_string_append_c(json, ',');
                json_string(json, action);
                first = FALSE;
            }
            g_string_append_c(json, ']');
            g_variant_iter_free(actions);
            g_variant_unref(reply);
        }
    }
    /* Never read back a password field, even for an agent the user started. */
    gboolean full = (j->r->flags & NP_A11Y_FULL_TEXT) != 0;
    if (IMPLEMENTS("Text") && strcmp(role, "password text") &&
        (full || HAS(states, STATE_EDITABLE) || !*name)) {
        int32_t length = int_property(j, bus, path, "org.a11y.atspi.Text", "CharacterCount");
        int32_t limit = full ? FULL_TEXT : TREE_TEXT;
        if (length > 0) {
            char *text = string_call(j, bus, path, "org.a11y.atspi.Text", "GetText",
                                     g_variant_new("(ii)", 0, MIN(length, limit)));
            g_string_append(json, ",\"text\":");
            json_string(json, text);
            if (length > limit) g_string_append_printf(json, ",\"textLength\":%d", length);
            g_free(text);
        }
    }
    if (IMPLEMENTS("Value")) append_value(j, bus, path);
    #undef IMPLEMENTS

    int total = 0;
    if (depth < max_depth) {
        GPtrArray *list = children(j, bus, path, &total);
        if (list->len) {
            g_string_append(json, ",\"children\":[");
            gboolean wrote = FALSE;
            for (guint i = 0; i + 1 < list->len; i += 2) {
                gsize mark = json->len;
                if (wrote) g_string_append_c(json, ',');
                if (node(j, list->pdata[i], list->pdata[i + 1], depth + 1, max_depth)) wrote = TRUE;
                else g_string_truncate(json, mark);
            }
            g_string_append_c(json, ']');
        }
        if (total > MAX_CHILDREN) g_string_append_printf(json, ",\"childCount\":%d", total);
        g_ptr_array_unref(list);
    } else {
        total = int_property(j, bus, path, ACCESSIBLE, "ChildCount");
        if (total > 0) g_string_append_printf(json, ",\"childCount\":%d", total);
    }
    g_string_append_c(json, '}');
    g_strfreev(interfaces);
    g_free(role); g_free(name); g_free(description);
    return TRUE;
}

static gboolean connect_bus(struct np_a11y *a, GError **error)
{
    if (a->bus && !g_dbus_connection_is_closed(a->bus)) return TRUE;
    g_clear_object(&a->bus);
    char *address = g_strdup(a->address);
    if (!address) {
        GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, a->cancel, NULL);
        GVariant *reply = NULL;
        if (session) {
            if (!a->announced) {
                /* Qt, Chromium and others publish their tree only while an
                 * assistive technology is announced; GTK always does. */
                GVariant *set = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus",
                    PROPERTIES, "Set", g_variant_new("(ssv)", "org.a11y.Status", "IsEnabled",
                    g_variant_new_boolean(TRUE)), NULL, G_DBUS_CALL_FLAGS_NONE,
                    CALL_TIMEOUT_MS, a->cancel, NULL);
                a->announced = set != NULL;
                if (set) g_variant_unref(set);
            }
            reply = g_dbus_connection_call_sync(session, "org.a11y.Bus", "/org/a11y/bus",
                "org.a11y.Bus", "GetAddress", NULL, G_VARIANT_TYPE("(s)"),
                G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, a->cancel, NULL);
            g_object_unref(session);
        }
        if (reply) { g_variant_get(reply, "(s)", &address); g_variant_unref(reply); }
    }
    if (address && *address)
        a->bus = g_dbus_connection_new_for_address_sync(address,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, a->cancel, NULL);
    g_free(address);
    if (!a->bus)
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "The Linux session has no accessibility bus. Install at-spi2-core, then restart the application.");
    return a->bus != NULL;
}

struct candidate { char *bus, *path; int score; };

/* Applications are registry children; their children are top-level windows.
 * The compositor's client PID selects the application when the bus reports
 * one. The window title disambiguates and is the only key for Xwayland. */
static gboolean resolve(struct job *j, GError **error)
{
    int count;
    GPtrArray *apps = children(j, REGISTRY, ROOT_PATH, &count);
    /* The best score wins; a tie keeps the first window found. */
    struct candidate best = {0};
    for (int pass = j->r->pid > 0 ? 0 : 1; pass < 2 && !best.bus; ++pass) {
        for (guint i = 0; i + 1 < apps->len; i += 2) {
            const char *bus = apps->pdata[i], *app = apps->pdata[i + 1];
            if (pass == 0) {
                GVariant *pid = call(j, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                    "GetConnectionUnixProcessID", g_variant_new("(s)", bus), "(u)", NULL);
                guint32 value = 0;
                if (pid) { g_variant_get(pid, "(u)", &value); g_variant_unref(pid); }
                if ((int32_t)value != j->r->pid) continue;
            }
            int windows;
            GPtrArray *list = children(j, bus, app, &windows);
            for (guint w = 0; w + 1 < list->len; w += 2) {
                guint64 states;
                if (!state(j, list->pdata[w], list->pdata[w + 1], &states)) continue;
                char *name = string_property(j, list->pdata[w], list->pdata[w + 1], ACCESSIBLE, "Name");
                gboolean titled = *j->r->title && !strcmp(name, j->r->title);
                g_free(name);
                /* Without a PID match, only the title identifies the window. */
                if (pass == 1 && !titled) continue;
                int score = (titled ? 4 : 0) + (HAS(states, STATE_ACTIVE) ? 2 : 0) +
                            (HAS(states, STATE_SHOWING) ? 1 : 0);
                if (!score) continue;
                if (score > best.score) {
                    g_free(best.bus); g_free(best.path);
                    best = (struct candidate){ g_strdup(list->pdata[w]), g_strdup(list->pdata[w + 1]), score };
                }
            }
            g_ptr_array_unref(list);
        }
    }
    if (best.bus) {
        /* Report the application owning the window, not an embedded plug. */
        for (guint i = 0; i + 1 < apps->len; i += 2)
            if (!strcmp(apps->pdata[i], best.bus)) {
                j->application = string_property(j, best.bus, apps->pdata[i + 1], ACCESSIBLE, "Name");
                char *toolkit = string_property(j, best.bus, apps->pdata[i + 1], "org.a11y.atspi.Application", "ToolkitName");
                char *version = string_property(j, best.bus, apps->pdata[i + 1], "org.a11y.atspi.Application", "Version");
                /* GTK 4 reports window coordinates from the window's content
                 * origin; GTK 3 and others from the surface, which includes
                 * client-side shadows outside the xdg window geometry. */
                gboolean content = g_ascii_strncasecmp(toolkit, "gtk", 3) == 0 && g_str_has_prefix(version, "4.");
                j->offset_x = content ? 0 : j->r->geometry_x;
                j->offset_y = content ? 0 : j->r->geometry_y;
                j->toolkit = *toolkit ? g_strconcat(toolkit, *version ? " " : "", version, NULL) : g_strdup("");
                g_free(toolkit); g_free(version);
                break;
            }
        GVariant *pid = call(j, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "GetConnectionUnixProcessID", g_variant_new("(s)", best.bus), "(u)", NULL);
        if (pid) { guint32 value; g_variant_get(pid, "(u)", &value); j->pid = (int32_t)value; g_variant_unref(pid); }
    }
    g_ptr_array_unref(apps);
    if (!best.bus) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
            "This window's application doesn't expose an accessibility tree. Use a screenshot with text recognition instead.");
        return FALSE;
    }
    j->bus = best.bus;
    j->window = best.path;
    if (!j->application) j->application = g_strdup("");
    if (!j->toolkit) j->toolkit = g_strdup("");
    return TRUE;
}

/* Elements are addressed as the bus name immediately followed by the object
 * path, as the tree reports them. They must belong to the addressed window's
 * application. */
static gboolean element(struct job *j, char **path, GError **error)
{
    const char *ref = j->r->element, *slash = ref ? strchr(ref, '/') : NULL;
    if (!slash || slash == ref) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Choose an element reference from the window's accessibility tree.");
        return FALSE;
    }
    char *bus = g_strndup(ref, slash - ref);
    gboolean ok = !strcmp(bus, j->bus) && g_variant_is_object_path(slash);
    g_free(bus);
    if (!ok) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "The element doesn't belong to this window. Read the accessibility tree again.");
        return FALSE;
    }
    *path = g_strdup(slash);
    return TRUE;
}

static gboolean tree(struct job *j, GError **error)
{
    char *path = NULL;
    if (j->r->element && *j->r->element && !element(j, &path, error)) return FALSE;
    unsigned depth = j->r->number >= 0 && j->r->number < MAX_DEPTH ? (unsigned)j->r->number : MAX_DEPTH;
    GString *json = j->json;
    g_string_append(json, "{\"application\":");
    json_string(json, j->application);
    g_string_append(json, ",\"toolkit\":");
    json_string(json, j->toolkit);
    g_string_append_printf(json, ",\"pid\":%d,\"window\":", j->pid);
    json_ref(json, j->bus, j->window);
    g_string_append(json, ",\"root\":");
    gsize mark = json->len;
    if (!node(j, j->bus, path ? path : j->window, 0, depth)) {
        g_string_truncate(json, mark);
        g_string_append(json, "null");
    }
    g_string_append_printf(json, ",\"truncated\":%s}", j->truncated ? "true" : "false");
    g_free(path);
    return TRUE;
}

static gboolean action(struct job *j, const char *path, GError **error)
{
    GVariant *reply = call(j, j->bus, path, "org.a11y.atspi.Action", "GetActions", NULL, "(a(sss))", error);
    if (!reply) return FALSE;
    GVariantIter *actions;
    const char *name, *ignored, *keys;
    char *chosen = NULL;
    int index = -1, i = 0;
    const char *wanted = j->r->argument && *j->r->argument ? j->r->argument : NULL;
    int wanted_index = j->r->number >= 0 && j->r->number < INT32_MAX ? (int)j->r->number : 0;
    g_variant_get(reply, "(a(sss))", &actions);
    while (g_variant_iter_next(actions, "(&s&s&s)", &name, &ignored, &keys)) {
        if (index < 0 && (wanted ? !g_ascii_strcasecmp(name, wanted) : i == wanted_index)) {
            index = i;
            chosen = g_strdup(name);
        }
        ++i;
    }
    g_variant_iter_free(actions);
    g_variant_unref(reply);
    if (index < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            i ? "The element has no action with that name." : "The element has no actions. Click its bounds instead.");
        return FALSE;
    }
    gboolean done = bool_call(j, j->bus, path, "org.a11y.atspi.Action", "DoAction",
                              g_variant_new("(i)", index), error);
    if (done) {
        g_string_append(j->json, "{\"ok\":true,\"action\":");
        json_string(j->json, chosen);
        g_string_append_c(j->json, '}');
    } else if (error && !*error) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The application declined the action.");
    }
    g_free(chosen);
    return done;
}

static gboolean set_text(struct job *j, const char *path, GError **error)
{
    const char *text = j->r->argument ? j->r->argument : "";
    if (bool_call(j, j->bus, path, "org.a11y.atspi.EditableText", "SetTextContents",
                  g_variant_new("(s)", text), NULL))
        return TRUE;
    /* Some bridges implement only incremental edits. */
    int32_t length = int_property(j, j->bus, path, "org.a11y.atspi.Text", "CharacterCount");
    gboolean cleared = length <= 0 || bool_call(j, j->bus, path, "org.a11y.atspi.EditableText",
        "DeleteText", g_variant_new("(ii)", 0, length), NULL);
    if (cleared && (!*text || bool_call(j, j->bus, path, "org.a11y.atspi.EditableText", "InsertText",
            g_variant_new("(isi)", 0, text, (int32_t)g_utf8_strlen(text, -1)), NULL)))
        return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
        "This element doesn't accept text. Focus it and type instead.");
    return FALSE;
}

static gboolean perform(struct job *j, GError **error)
{
    if (j->r->operation == NP_A11Y_TREE) return tree(j, error);
    char *path = NULL;
    if (!element(j, &path, error)) return FALSE;
    gboolean ok = FALSE;
    switch (j->r->operation) {
    case NP_A11Y_ACTION:
        ok = action(j, path, error);
        break;
    case NP_A11Y_FOCUS:
        ok = bool_call(j, j->bus, path, "org.a11y.atspi.Component", "GrabFocus", NULL, error);
        if (!ok && error && !*error)
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The element can't take keyboard focus.");
        break;
    case NP_A11Y_SET_TEXT:
        ok = set_text(j, path, error);
        break;
    case NP_A11Y_SET_VALUE: {
        GVariant *reply = call(j, j->bus, path, PROPERTIES, "Set", g_variant_new("(ssv)",
            "org.a11y.atspi.Value", "CurrentValue", g_variant_new_double(j->r->number)), NULL, error);
        ok = reply != NULL;
        if (reply) g_variant_unref(reply);
        break;
    }
    }
    if (ok && !j->json->len) g_string_append(j->json, "{\"ok\":true}");
    g_free(path);
    return ok;
}

bool np_a11y_encode_reply(uint32_t token, const char *error, const char *json,
                          unsigned char **data, size_t *length)
{
    struct np_window_message m;
    np_window_message_init(&m, NP_WINDOW_GUEST_TO_HOST, 7);
    if (m.ok) memcpy(m.data, "NPAP", 4);
    np_window_put_u32(&m, token);
    np_window_put_string(&m, error);
    np_window_put_string(&m, json);
    if (!m.ok) { np_window_message_clear(&m); return false; }
    *data = m.data;
    *length = m.len;
    return true;
}

static void dispatch(struct np_a11y *a, struct np_a11y_request *r)
{
    struct job j = { .a = a, .r = r, .deadline = g_get_monotonic_time() + REQUEST_BUDGET_US,
                     .json = g_string_new(NULL) };
    GError *error = NULL;
    gboolean ok = connect_bus(a, &error) && resolve(&j, &error) && perform(&j, &error);
    char *message = NULL;
    if (!ok) {
        if (error && g_dbus_error_is_remote_error(error)) g_dbus_error_strip_remote_error(error);
        message = g_strdup(error ? error->message : "The accessibility request failed.");
        g_string_truncate(j.json, 0);
    }
    g_clear_error(&error);
    struct np_a11y_reply *reply = g_new0(struct np_a11y_reply, 1);
    reply->generation = r->generation;
    if (!np_a11y_encode_reply(r->token, message ? message : "", j.json->str, &reply->data, &reply->length) &&
        !np_a11y_encode_reply(r->token, "The accessibility reply was too large.", "", &reply->data, &reply->length)) {
        g_free(reply);
        reply = NULL;
    }
    if (reply) {
        g_async_queue_push(a->replies, reply);
        wake(a->notify[1]);
    }
    g_free(message);
    g_string_free(j.json, TRUE);
    g_free(j.bus); g_free(j.window); g_free(j.application); g_free(j.toolkit);
}

static gpointer run(gpointer data)
{
    struct np_a11y *a = data;
    for (;;) {
        struct np_a11y_request *r = g_async_queue_pop(a->requests);
        if (r == &stop_request) break;
        g_mutex_lock(&a->lock);
        gboolean stopping = a->stopping;
        g_mutex_unlock(&a->lock);
        if (!stopping) dispatch(a, r);
        free_request(r);
    }
    g_clear_object(&a->bus);
    return NULL;
}

struct np_a11y *np_a11y_start_with_address(const char *address)
{
    struct np_a11y *a = g_new0(struct np_a11y, 1);
    if (!g_unix_open_pipe(a->notify, FD_CLOEXEC, NULL)) { g_free(a); return NULL; }
    g_unix_set_fd_nonblocking(a->notify[0], TRUE, NULL);
    g_unix_set_fd_nonblocking(a->notify[1], TRUE, NULL);
    g_mutex_init(&a->lock);
    a->address = g_strdup(address);
    a->requests = g_async_queue_new();
    a->replies = g_async_queue_new();
    a->cancel = g_cancellable_new();
    a->thread = g_thread_new("np-a11y", run, a);
    return a;
}

struct np_a11y *np_a11y_start(void)
{
    /* A root compositor would read every user's applications. */
    if (geteuid() == 0) { errno = EPERM; return NULL; }
    return np_a11y_start_with_address(NULL);
}

int np_a11y_fd(struct np_a11y *a) { return a->notify[0]; }

bool np_a11y_request(struct np_a11y *a, struct np_a11y_request *r)
{
    if (!a || !r || !r->token || r->operation < NP_A11Y_TREE || r->operation > NP_A11Y_SET_VALUE ||
        (r->flags & ~(uint32_t)(NP_A11Y_INCLUDE_HIDDEN | NP_A11Y_FULL_TEXT)) || !isfinite(r->number) ||
        !r->title || !r->app_id || !r->element || !r->argument ||
        strlen(r->element) > NP_A11Y_MAX_ELEMENT || strlen(r->argument) > NP_A11Y_MAX_ARGUMENT ||
        !g_utf8_validate(r->argument, -1, NULL) || !g_utf8_validate(r->element, -1, NULL)) {
        errno = EINVAL;
        return false;
    }
    if (g_async_queue_length(a->requests) >= 8) { errno = EAGAIN; return false; }
    struct np_a11y_request *copy = g_new(struct np_a11y_request, 1);
    *copy = *r;
    *r = (struct np_a11y_request){0};
    g_async_queue_push(a->requests, copy);
    return true;
}

bool np_a11y_take(struct np_a11y *a, struct np_a11y_reply *reply)
{
    char byte;
    (void)!read(a->notify[0], &byte, 1);
    struct np_a11y_reply *r = g_async_queue_try_pop(a->replies);
    if (!r) return false;
    *reply = *r;
    g_free(r);
    return true;
}

void np_a11y_stop(struct np_a11y *a)
{
    if (!a) return;
    g_mutex_lock(&a->lock);
    a->stopping = TRUE;
    g_mutex_unlock(&a->lock);
    g_cancellable_cancel(a->cancel);
    g_async_queue_push(a->requests, &stop_request);
    g_thread_join(a->thread);
    struct np_a11y_request *r;
    while ((r = g_async_queue_try_pop(a->requests))) free_request(r);
    struct np_a11y_reply *reply;
    while ((reply = g_async_queue_try_pop(a->replies))) { free(reply->data); g_free(reply); }
    g_async_queue_unref(a->requests);
    g_async_queue_unref(a->replies);
    g_object_unref(a->cancel);
    close(a->notify[0]);
    close(a->notify[1]);
    g_mutex_clear(&a->lock);
    g_free(a->address);
    g_free(a);
}
