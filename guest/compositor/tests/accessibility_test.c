/* Exercises the AT-SPI2 worker against a fake registry and application on a
 * private session bus. Run with dbus-run-session. */
#include "../accessibility_worker.h"
#include <gio/gio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char introspection[] =
    "<node>"
    "<interface name='org.a11y.atspi.Accessible'>"
    " <method name='GetChildren'><arg type='a(so)' direction='out'/></method>"
    " <method name='GetChildAtIndex'><arg type='i' direction='in'/><arg type='(so)' direction='out'/></method>"
    " <method name='GetState'><arg type='au' direction='out'/></method>"
    " <method name='GetRoleName'><arg type='s' direction='out'/></method>"
    " <method name='GetInterfaces'><arg type='as' direction='out'/></method>"
    " <property name='Name' type='s' access='read'/>"
    " <property name='Description' type='s' access='read'/>"
    " <property name='ChildCount' type='i' access='read'/>"
    "</interface>"
    "<interface name='org.a11y.atspi.Application'>"
    " <property name='ToolkitName' type='s' access='read'/>"
    " <property name='Version' type='s' access='read'/>"
    "</interface>"
    "<interface name='org.a11y.atspi.Component'>"
    " <method name='GetExtents'><arg type='u' direction='in'/><arg type='(iiii)' direction='out'/></method>"
    " <method name='GrabFocus'><arg type='b' direction='out'/></method>"
    "</interface>"
    "<interface name='org.a11y.atspi.Action'>"
    " <method name='GetActions'><arg type='a(sss)' direction='out'/></method>"
    " <method name='DoAction'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "</interface>"
    "<interface name='org.a11y.atspi.Text'>"
    " <method name='GetText'><arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='s' direction='out'/></method>"
    " <property name='CharacterCount' type='i' access='read'/>"
    "</interface>"
    "<interface name='org.a11y.atspi.EditableText'>"
    " <method name='SetTextContents'><arg type='s' direction='in'/><arg type='b' direction='out'/></method>"
    "</interface>"
    "<interface name='org.a11y.atspi.Value'>"
    " <property name='CurrentValue' type='d' access='readwrite'/>"
    " <property name='MinimumValue' type='d' access='read'/>"
    " <property name='MaximumValue' type='d' access='read'/>"
    "</interface>"
    "</node>";

#define SHOWING (1u << 25)
#define ACTIVE (1u << 1)
#define EDITABLE (1u << 7)
#define FOCUSABLE (1u << 11)

struct fake {
    const char *path, *role, *name, *children[6];
    guint32 states;
    const char *interfaces[6];
};
static char text[64] = "hello";
static double value = 3;
static int clicks, focused;
static const struct fake nodes[] = {
    {"/org/a11y/atspi/accessible/root", "desktop frame", "main", {"/app"}, SHOWING, {"Accessible"}},
    {"/app", "application", "editor", {"/window", "/other"}, 0, {"Accessible", "Application"}},
    {"/other", "frame", "Preferences", {NULL}, SHOWING, {"Accessible"}},
    {"/window", "frame", "Notes", {"/save", "/entry", "/hidden", "/password", "/volume"}, SHOWING | ACTIVE,
     {"Accessible", "Component"}},
    {"/save", "push button", "Save \"now\"", {NULL}, SHOWING | FOCUSABLE, {"Accessible", "Component", "Action"}},
    {"/entry", "text", "", {NULL}, SHOWING | EDITABLE | FOCUSABLE, {"Accessible", "Component", "Text", "EditableText"}},
    {"/hidden", "push button", "Hidden", {NULL}, 0, {"Accessible", "Action"}},
    {"/password", "password text", "Password", {NULL}, SHOWING | EDITABLE, {"Accessible", "Text"}},
    {"/volume", "slider", "Volume", {NULL}, SHOWING, {"Accessible", "Value"}},
};

static const struct fake *find(const char *path)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(nodes); ++i)
        if (!strcmp(nodes[i].path, path)) return &nodes[i];
    return NULL;
}

static void method(GDBusConnection *connection, const char *sender, const char *path,
                   const char *interface, const char *name, GVariant *parameters,
                   GDBusMethodInvocation *invocation, gpointer data)
{
    (void)sender; (void)data;
    const struct fake *node = find(path);
    const char *bus = g_dbus_connection_get_unique_name(connection);
    if (!strcmp(name, "GetChildren")) {
        GVariantBuilder list;
        g_variant_builder_init(&list, G_VARIANT_TYPE("a(so)"));
        for (unsigned i = 0; node->children[i]; ++i) g_variant_builder_add(&list, "(so)", bus, node->children[i]);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(so))", &list));
    } else if (!strcmp(name, "GetState")) {
        GVariantBuilder words;
        g_variant_builder_init(&words, G_VARIANT_TYPE("au"));
        g_variant_builder_add(&words, "u", node->states);
        g_variant_builder_add(&words, "u", 0u);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(au)", &words));
    } else if (!strcmp(name, "GetRoleName")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", node->role));
    } else if (!strcmp(name, "GetInterfaces")) {
        GVariantBuilder list;
        g_variant_builder_init(&list, G_VARIANT_TYPE("as"));
        for (unsigned i = 0; node->interfaces[i]; ++i) {
            char *full = g_strconcat("org.a11y.atspi.", node->interfaces[i], NULL);
            g_variant_builder_add(&list, "s", full);
            g_free(full);
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(as)", &list));
    } else if (!strcmp(name, "GetExtents")) {
        guint32 type;
        g_variant_get(parameters, "(u)", &type);
        assert(type == 1);
        int y = !strcmp(path, "/save") ? 30 : !strcmp(path, "/entry") ? 60 : 0;
        g_dbus_method_invocation_return_value(invocation, !strcmp(path, "/window")
            ? g_variant_new("((iiii))", 0, 0, 420, 300) : g_variant_new("((iiii))", 20, y, 80, 24));
    } else if (!strcmp(name, "GrabFocus")) {
        focused++;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
    } else if (!strcmp(name, "GetActions")) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new_parsed("([('click', 'Clicks', '<Alt>s')],)"));
    } else if (!strcmp(name, "DoAction")) {
        clicks++;
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
    } else if (!strcmp(name, "GetText")) {
        int start, end;
        g_variant_get(parameters, "(ii)", &start, &end);
        char *slice = g_utf8_substring(text, start, end);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", slice));
        g_free(slice);
    } else if (!strcmp(name, "SetTextContents")) {
        const char *replacement;
        g_variant_get(parameters, "(&s)", &replacement);
        g_strlcpy(text, replacement, sizeof(text));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", TRUE));
    } else {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", interface);
    }
}

static GVariant *get_property(GDBusConnection *connection, const char *sender, const char *path,
                              const char *interface, const char *name, GError **error, gpointer data)
{
    (void)connection; (void)sender; (void)interface; (void)error; (void)data;
    const struct fake *node = find(path);
    if (!strcmp(name, "Name")) return g_variant_new_string(node->name);
    if (!strcmp(name, "Description")) return g_variant_new_string("");
    if (!strcmp(name, "ChildCount")) {
        int count = 0;
        while (node->children[count]) ++count;
        return g_variant_new_int32(count);
    }
    if (!strcmp(name, "ToolkitName")) return g_variant_new_string("GTK");
    if (!strcmp(name, "Version")) return g_variant_new_string("3.24.41");
    if (!strcmp(name, "CharacterCount")) return g_variant_new_int32(g_utf8_strlen(text, -1));
    if (!strcmp(name, "CurrentValue")) return g_variant_new_double(value);
    if (!strcmp(name, "MinimumValue")) return g_variant_new_double(0);
    if (!strcmp(name, "MaximumValue")) return g_variant_new_double(10);
    return NULL;
}

static gboolean set_property(GDBusConnection *connection, const char *sender, const char *path,
                             const char *interface, const char *name, GVariant *new_value,
                             GError **error, gpointer data)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)error; (void)data;
    assert(!strcmp(name, "CurrentValue"));
    value = g_variant_get_double(new_value);
    return TRUE;
}

static const GDBusInterfaceVTable vtable = { method, get_property, set_property, {0} };

struct result { char *error, *json; };

static struct result request(struct np_a11y *worker, uint32_t token, uint32_t operation, uint32_t flags,
                             double number, int32_t pid, const char *title, const char *element,
                             const char *argument)
{
    struct np_a11y_request r = {
        .token = token, .operation = operation, .flags = flags, .number = number, .pid = pid,
        .geometry_x = 5, .geometry_y = 6, .title = g_strdup(title), .app_id = g_strdup("org.example.Notes"),
        .element = g_strdup(element), .argument = g_strdup(argument),
    };
    assert(np_a11y_request(worker, &r));
    struct np_a11y_reply reply;
    gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!np_a11y_take(worker, &reply)) {
        assert(g_get_monotonic_time() < deadline);
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
    /* "NPAP", direction 1, type 7, two zero bytes, then the fields. */
    assert(reply.length >= 20 && !memcmp(reply.data, "NPAP", 4) && reply.data[4] == 1 && reply.data[5] == 7);
    size_t offset = 8;
    uint32_t got;
    memcpy(&got, reply.data + offset, 4); offset += 4;
    assert(got == token);
    struct result result;
    char **fields[] = { &result.error, &result.json };
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t length;
        memcpy(&length, reply.data + offset, 4); offset += 4;
        *fields[i] = g_strndup((const char *)reply.data + offset, length);
        offset += length;
    }
    assert(offset == reply.length);
    free(reply.data);
    return result;
}

static void clear(struct result *result) { g_free(result->error); g_free(result->json); }

int main(void)
{
    const char *address = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    if (!address) { fprintf(stderr, "run with dbus-run-session\n"); return 77; }
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    assert(bus);
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(introspection, NULL);
    assert(info);
    for (unsigned n = 0; n < G_N_ELEMENTS(nodes); ++n)
        for (unsigned i = 0; info->interfaces[i]; ++i)
            assert(g_dbus_connection_register_object(bus, nodes[n].path, info->interfaces[i], &vtable, NULL, NULL, NULL));
    GVariant *owned = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.a11y.atspi.Registry", 4u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    assert(owned);
    g_variant_unref(owned);
    const char *me = g_dbus_connection_get_unique_name(bus);

    struct np_a11y *worker = np_a11y_start_with_address(address);
    assert(worker);

    /* The title selects a window even when the bus can't report a PID. */
    struct result tree = request(worker, 1, NP_A11Y_TREE, 0, 8, 0, "Notes", "", "");
    assert(!*tree.error);
    assert(strstr(tree.json, "\"application\":\"editor\""));
    assert(strstr(tree.json, "\"toolkit\":\"GTK 3.24.41\""));
    assert(strstr(tree.json, "\"name\":\"Save \\\"now\\\"\""));
    assert(strstr(tree.json, "\"actions\":[\"click\"]"));
    /* GTK 3 window coordinates include the surface's shadow margin. */
    assert(strstr(tree.json, "\"bounds\":[15,24,80,24]"));
    assert(strstr(tree.json, "\"text\":\"hello\""));
    assert(strstr(tree.json, "\"value\":3,\"minimum\":0,\"maximum\":10"));
    assert(!strstr(tree.json, "Hidden"));
    /* Password text is never read back. */
    assert(strstr(tree.json, "\"name\":\"Password\",\"states\":[\"editable\",\"showing\"]}"));
    assert(strstr(tree.json, "\"truncated\":false"));
    char *window = g_strconcat("\"window\":\"", me, "/window\"", NULL);
    assert(strstr(tree.json, window));
    g_free(window);
    clear(&tree);

    struct result hidden = request(worker, 2, NP_A11Y_TREE, NP_A11Y_INCLUDE_HIDDEN, 8, getpid(), "Notes", "", "");
    assert(!*hidden.error && strstr(hidden.json, "Hidden"));
    clear(&hidden);

    struct result shallow = request(worker, 3, NP_A11Y_TREE, 0, 0, 0, "Notes", "", "");
    assert(!*shallow.error && strstr(shallow.json, "\"childCount\":5") && !strstr(shallow.json, "Save"));
    clear(&shallow);

    char *save = g_strconcat(me, "/save", NULL), *entry = g_strconcat(me, "/entry", NULL);
    char *volume = g_strconcat(me, "/volume", NULL);
    struct result clicked = request(worker, 4, NP_A11Y_ACTION, 0, 0, 0, "Notes", save, "");
    assert(!*clicked.error && clicks == 1 && strstr(clicked.json, "\"action\":\"click\""));
    clear(&clicked);
    struct result named = request(worker, 5, NP_A11Y_ACTION, 0, 0, 0, "Notes", save, "CLICK");
    assert(!*named.error && clicks == 2);
    clear(&named);
    struct result missing = request(worker, 6, NP_A11Y_ACTION, 0, 0, 0, "Notes", save, "press");
    assert(*missing.error && clicks == 2);
    clear(&missing);

    struct result focus = request(worker, 7, NP_A11Y_FOCUS, 0, 0, 0, "Notes", entry, "");
    assert(!*focus.error && focused == 1);
    clear(&focus);
    struct result typed = request(worker, 8, NP_A11Y_SET_TEXT, 0, 0, 0, "Notes", entry, "héllo wörld");
    assert(!*typed.error && !strcmp(text, "héllo wörld"));
    clear(&typed);
    struct result full = request(worker, 9, NP_A11Y_TREE, NP_A11Y_FULL_TEXT, 0, 0, "Notes", entry, "");
    assert(!*full.error && strstr(full.json, "\"text\":\"héllo wörld\"") && strstr(full.json, "\"root\":{\"ref\""));
    clear(&full);
    struct result moved = request(worker, 10, NP_A11Y_SET_VALUE, 0, 7.5, 0, "Notes", volume, "");
    assert(!*moved.error && value == 7.5);
    clear(&moved);

    /* Elements outside the addressed application are refused. */
    struct result foreign = request(worker, 11, NP_A11Y_ACTION, 0, 0, 0, "Notes", ":1.9999/save", "");
    assert(*foreign.error && clicks == 2);
    clear(&foreign);
    struct result unknown = request(worker, 12, NP_A11Y_TREE, 0, 8, 0, "Untitled", "", "");
    assert(*unknown.error && strstr(unknown.error, "screenshot"));
    clear(&unknown);

    struct np_a11y_request invalid = { .token = 13, .operation = 99, .title = "", .app_id = "", .element = "", .argument = "" };
    assert(!np_a11y_request(worker, &invalid));

    g_free(save); g_free(entry); g_free(volume);
    np_a11y_stop(worker);
    g_dbus_node_info_unref(info);
    g_object_unref(bus);
    puts("accessibility worker: ok");
    return 0;
}
