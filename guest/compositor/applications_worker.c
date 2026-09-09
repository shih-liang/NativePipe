#define _GNU_SOURCE
#include "applications_worker.h"
#include "application_icons.h"
#include "windowwire.h"
#include <gio/gdesktopappinfo.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

struct request { uint64_t generation; uint32_t token, action; char *id; };
struct np_apps {
    GThread *thread;
    GMainContext *context;
    GAsyncQueue *requests, *replies;
    GMutex lock;
    GCond available;
    gboolean stopping;
    int wake[2], notify[2];
    uint64_t generation;
    gboolean activation_ready;
    GHashTable *icon_monitors;
    GPtrArray *children;
    GCancellable *cancel;
};

static void wake(int fd)
{
    char byte = 1;
    while (write(fd, &byte, 1) < 0 && errno == EINTR) {}
    /* EAGAIN means the nonblocking pipe already contains a wakeup. */
}

static void free_request(gpointer p)
{
    struct request *r = p;
    g_free(r->id); g_free(r);
}
static void free_reply(gpointer p)
{
    struct np_app_reply *r = p;
    free(r->data); g_free(r);
}
static void init_reply(struct np_window_message *m, unsigned type, uint32_t token)
{
    np_window_message_init(m, NP_WINDOW_GUEST_TO_HOST, type);
    if (m->ok) memcpy(m->data, "NPAP", 4);
    np_window_put_u32(m, token);
}
static gboolean emit(struct np_apps *a, uint64_t generation, struct np_window_message *m)
{
    if (!m->ok) { np_window_message_clear(m); return FALSE; }
    g_mutex_lock(&a->lock);
    /* Bounded worker -> Wayland handoff. Stop wakes a stalled producer. */
    while (!a->stopping && (!generation || generation == a->generation) && g_async_queue_length(a->replies) >= 4)
        g_cond_wait(&a->available, &a->lock);
    gboolean ok = !a->stopping && (!generation || generation == a->generation);
    if (ok) {
        struct np_app_reply *r = g_new0(struct np_app_reply, 1);
        *r = (struct np_app_reply){ generation, m->data, m->len };
        g_async_queue_push(a->replies, r);
        m->data = NULL;
        wake(a->notify[1]);
    }
    g_mutex_unlock(&a->lock);
    np_window_message_clear(m);
    return ok;
}
static void changed(GAppInfoMonitor *monitor, gpointer data)
{
    (void)monitor;
    struct np_apps *a = data;
    struct np_window_message m;
    init_reply(&m, 3, 0);
    emit(a, 0, &m); /* Invalidation is valid for any connected host. */
}
static void icon_changed(GFileMonitor *monitor, GFile *file, GFile *other,
                         GFileMonitorEvent event, gpointer data) {
    (void)monitor; (void)file; (void)other; (void)event;
    changed(NULL, data);
}
static void theme_changed(GSettings *settings, const char *key, gpointer data) {
    (void)settings; (void)key;
    changed(NULL, data);
}
static void watch_icon(struct np_apps *a, const char *path) {
    if (!path || g_hash_table_contains(a->icon_monitors, path)) return;
    GFile *file = g_file_new_for_path(path);
    GFileMonitor *monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, a->cancel, NULL);
    g_object_unref(file);
    if (monitor) {
        g_signal_connect(monitor, "changed", G_CALLBACK(icon_changed), a);
        g_hash_table_insert(a->icon_monitors, g_strdup(path), monitor);
    }
}
static gboolean visible(GAppInfo *app)
{
    return G_IS_DESKTOP_APP_INFO(app) && g_app_info_get_id(app) && g_app_info_should_show(app);
}
static void list(struct np_apps *a, struct request *r)
{
    GList *catalog = g_app_info_get_all(), *item = catalog;
    const char *error = NULL;
    while (item) {
        struct np_window_message m;
        init_reply(&m, 1, r->token);
        size_t count_offset = m.len;
        np_window_put_u32(&m, 0);
        uint32_t count = 0;
        for (; item && count < 32; item = item->next) {
            GAppInfo *app = item->data;
            if (!visible(app)) continue;
            char *icon = g_desktop_app_info_get_string(G_DESKTOP_APP_INFO(app), "Icon");
            const char *fields[] = {g_app_info_get_id(app), g_app_info_get_display_name(app),
                g_app_info_get_description(app), g_app_info_get_executable(app),
                g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(app)), icon};
            size_t bytes = 4 * G_N_ELEMENTS(fields);
            for (unsigned i = 0; i < G_N_ELEMENTS(fields); ++i) {
                if (!fields[i]) fields[i] = "";
                if (!g_utf8_validate(fields[i], -1, NULL)) error = "Application metadata is not valid UTF-8.";
                bytes += strlen(fields[i]);
            }
            if (bytes > 65536 - 16 || strlen(fields[0]) > 4096)
                error = "Application metadata exceeds the per-entry size limit.";
            if (error || (count && m.len + bytes > 65536)) { g_free(icon); break; }
            for (unsigned i = 0; i < G_N_ELEMENTS(fields); ++i) np_window_put_string(&m, fields[i]);
            g_free(icon);
            ++count;
        }
        if (!m.ok) error = "Could not encode the application catalog.";
        if (error) { np_window_message_clear(&m); break; }
        if (m.ok) m.data[count_offset] = count; /* count <= 32, other bytes zero */
        if (!emit(a, r->generation, &m)) { g_list_free_full(catalog, g_object_unref); return; }
    }
    g_list_free_full(catalog, g_object_unref);
    struct np_window_message m;
    init_reply(&m, 4, r->token);
    np_window_put_string(&m, error ? error : "");
    emit(a, r->generation, &m);
}
static void child_setup(gpointer unused)
{
    (void)unused;
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
    /* The compositor blocks SIGCHLD for its own event sources, clients must not. */
}
struct child { struct np_apps *apps; uint64_t generation; GSource *source; GPid pid; };
static void child_exited(GPid pid, gint status, gpointer data)
{
    struct child *c = data;
    struct np_window_message m;
    init_reply(&m, 6, 0);
    np_window_put_i32(&m, pid);
    np_window_put_i32(&m, WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    emit(c->apps, c->generation, &m);
    g_spawn_close_pid(pid);
    g_ptr_array_remove_fast(c->apps->children, c);
    g_source_unref(c->source);
    g_free(c);
}
struct launch { struct np_apps *apps; uint64_t generation; GPid pid; };
struct activation { gboolean done, ok; GError *error; };
static void activated(GObject *object, GAsyncResult *result, gpointer data)
{
    struct activation *a = data;
    a->ok = g_app_info_launch_uris_finish(G_APP_INFO(object), result, &a->error);
    a->done = TRUE;
}
static gboolean uses_activation(GDesktopAppInfo *entry)
{
    if (!entry || !g_desktop_app_info_get_boolean(entry, "DBusActivatable")) return FALSE;
    const char *filename = g_desktop_app_info_get_filename(entry);
    char *name = filename ? g_path_get_basename(filename) : NULL;
    gboolean valid = name && g_str_has_suffix(name, ".desktop");
    if (valid) {
        name[strlen(name) - strlen(".desktop")] = 0;
        valid = g_dbus_is_name(name) && name[0] != ':';
    }
    g_free(name);
    return valid;
}
static void launched(GDesktopAppInfo *info, GPid pid, gpointer data)
{
    (void)info;
    struct launch *l = data;
    l->pid = pid;
    struct child *c = g_new0(struct child, 1);
    *c = (struct child){ l->apps, l->generation, g_child_watch_source_new(pid), pid };
    g_source_set_callback(c->source, G_SOURCE_FUNC(child_exited), c, NULL);
    g_source_attach(c->source, l->apps->context);
    g_ptr_array_add(l->apps->children, c);
}
static void dispatch(struct np_apps *a, struct request *r)
{
    if (r->action == NP_APP_LIST) { list(a, r); return; }
    if (r->action == NP_APP_APPEARANCE) {
        /* Same settings as VMHost/guestd, applied by the unprivileged session
         * worker. Neither the SSH reader nor Wayland event loop waits on dconf. */
        GSettingsSchemaSource *source = g_settings_schema_source_get_default();
        GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source,
            "org.gnome.desktop.interface", TRUE) : NULL;
        gboolean ok = FALSE;
        if (schema && g_settings_schema_has_key(schema, "color-scheme") &&
            g_settings_schema_has_key(schema, "gtk-theme")) {
            GSettings *settings = g_settings_new_full(schema, NULL, NULL);
            gboolean dark = !strcmp(r->id, "dark");
            ok = g_settings_set_string(settings, "color-scheme", dark ? "prefer-dark" : "prefer-light");
            ok = g_settings_set_string(settings, "gtk-theme", dark ? "Adwaita-dark" : "Adwaita") && ok;
            g_settings_sync();
            g_object_unref(settings);
        }
        if (schema) g_settings_schema_unref(schema);
        struct np_window_message m;
        init_reply(&m, 2, r->token); np_window_put_i32(&m, 0);
        np_window_put_string(&m, ok ? "" : "Install gsettings-desktop-schemas and dconf to synchronize desktop appearance.");
        emit(a, r->generation, &m);
        return;
    }
    GDesktopAppInfo *entry = strchr(r->id, '/') ? NULL : g_desktop_app_info_new(r->id);
    struct np_window_message m;
    if (r->action == NP_APP_ICON) {
        GBytes *icon = NULL;
        if (entry) {
            GIcon *source = g_app_info_get_icon(G_APP_INFO(entry));
            char *path = np_application_icon_path(source);
            watch_icon(a, path); g_free(path);
            icon = np_application_icon(source);
        }
        gsize size = 0;
        const unsigned char *bytes = icon ? g_bytes_get_data(icon, &size) : NULL;
        init_reply(&m, 5, r->token);
        np_window_put_bytes(&m, bytes, size);
        if (icon) g_bytes_unref(icon);
    } else {
        GError *error = NULL;
        struct launch l = { a, r->generation, 0 };
        /* Each backend owns a private D-Bus session. Preserve DBusActivatable,
         * Path, %k, empty/quoted args and terminal semantics in the original entry. */
        gboolean activation = uses_activation(entry);
        gboolean isolated = entry && (!activation || a->activation_ready);
        gboolean ok = FALSE;
        if (isolated && visible(G_APP_INFO(entry))) {
            if (activation) {
                /* The synchronous manager API only queues a D-Bus call; its
                 * return value does not report a failed service activation. */
                struct activation result = {0};
                g_app_info_launch_uris_async(G_APP_INFO(entry), NULL, NULL, a->cancel, activated, &result);
                while (!result.done) g_main_context_iteration(a->context, TRUE);
                ok = result.ok; error = result.error;
            } else {
                ok = g_desktop_app_info_launch_uris_as_manager(entry, NULL, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                    child_setup, NULL, launched, &l, &error);
            }
        }
        init_reply(&m, 2, r->token);
        np_window_put_i32(&m, l.pid); /* D-Bus activation has no child PID. */
        np_window_put_string(&m, ok ? "" : error ? error->message : entry && !isolated
            ? "The private application activation service is unavailable." : "Application is no longer available.");
        g_clear_error(&error);
    }
    g_clear_object(&entry);
    emit(a, r->generation, &m);
}
static gboolean requests_ready(gint fd, GIOCondition condition, gpointer data)
{
    (void)condition;
    struct np_apps *a = data;
    char bytes[64];
    (void)!read(fd, bytes, sizeof(bytes));
    struct request *r;
    while ((r = g_async_queue_try_pop(a->requests))) {
        g_mutex_lock(&a->lock);
        gboolean stopping = a->stopping || r->generation != a->generation;
        g_mutex_unlock(&a->lock);
        if (!stopping) dispatch(a, r);
        free_request(r);
    }
    return G_SOURCE_CONTINUE;
}
static gpointer run(gpointer data)
{
    struct np_apps *a = data;
    g_main_context_push_thread_default(a->context);
    np_application_icons_init();
    /* D-Bus services inherit the daemon environment, not the caller's launch
     * context. Publish the actual display after socket/Xwayland setup. */
    if (getenv("NP_PRIVATE_APPLICATION_BUS") && getenv("DBUS_SESSION_BUS_ADDRESS")) {
        GError *error = NULL;
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, a->cancel, &error);
        if (bus) {
            GVariantBuilder environment;
            g_variant_builder_init(&environment, G_VARIANT_TYPE("a{ss}"));
            const char *names[] = {"WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
                "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "GDK_BACKEND", "QT_QPA_PLATFORM",
                "MOZ_ENABLE_WAYLAND", "LD_LIBRARY_PATH", "LD_PRELOAD"};
            for (unsigned i = 0; i < G_N_ELEMENTS(names); ++i)
                g_variant_builder_add(&environment, "{ss}", names[i], getenv(names[i]) ? getenv(names[i]) : "");
            GVariant *result = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus",
                "/org/freedesktop/DBus", "org.freedesktop.DBus", "UpdateActivationEnvironment",
                g_variant_new("(a{ss})", &environment), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, a->cancel, &error);
            a->activation_ready = result != NULL;
            if (result) g_variant_unref(result);
            g_object_unref(bus);
        }
        if (error) { g_printerr("[applications] session activation environment: %s\n", error->message); g_error_free(error); }
    }
    GAppInfoMonitor *monitor = g_app_info_monitor_get();
    GSettingsSchemaSource *schemas = g_settings_schema_source_get_default();
    GSettingsSchema *schema = schemas ? g_settings_schema_source_lookup(schemas,
        "org.gnome.desktop.interface", TRUE) : NULL;
    GSettings *theme = NULL;
    if (schema && g_settings_schema_has_key(schema, "icon-theme")) {
        theme = g_settings_new_full(schema, NULL, NULL);
        g_signal_connect(theme, "changed::icon-theme", G_CALLBACK(theme_changed), a);
    }
    if (schema) g_settings_schema_unref(schema);
    const char *configs[] = {"gtk-4.0/settings.ini", "gtk-3.0/settings.ini", "kdeglobals"};
    for (unsigned i = 0; i < G_N_ELEMENTS(configs); ++i) {
        char *path = g_build_filename(g_get_user_config_dir(), configs[i], NULL);
        watch_icon(a, path); g_free(path);
    }
    g_signal_connect(monitor, "changed", G_CALLBACK(changed), a);
    GSource *source = g_unix_fd_source_new(a->wake[0], G_IO_IN);
    g_source_set_callback(source, G_SOURCE_FUNC(requests_ready), a, NULL);
    g_source_attach(source, a->context);
    for (;;) {
        g_mutex_lock(&a->lock);
        gboolean stop = a->stopping;
        g_mutex_unlock(&a->lock);
        if (stop) break;
        g_main_context_iteration(a->context, TRUE);
    }
    g_source_destroy(source); g_source_unref(source);
    for (unsigned i = 0; i < a->children->len; ++i) {
        struct child *c = a->children->pdata[i];
        g_source_destroy(c->source); g_source_unref(c->source);
        g_spawn_close_pid(c->pid); g_free(c);
    }
    g_signal_handlers_disconnect_by_data(monitor, a);
    g_object_unref(monitor);
    g_clear_object(&theme);
    g_main_context_pop_thread_default(a->context);
    return NULL;
}
struct np_apps *np_apps_start(void)
{
    /* A root desktop catalog would expose files inaccessible to its users. */
    if (geteuid() == 0) { errno = EPERM; return NULL; }
    struct np_apps *a = g_new0(struct np_apps, 1);
    a->wake[0] = a->wake[1] = a->notify[0] = a->notify[1] = -1;
    if (!g_unix_open_pipe(a->wake, FD_CLOEXEC, NULL) ||
        !g_unix_open_pipe(a->notify, FD_CLOEXEC, NULL)) {
        for (unsigned i = 0; i < 2; ++i) { if (a->wake[i] >= 0) close(a->wake[i]); if (a->notify[i] >= 0) close(a->notify[i]); }
        g_free(a); return NULL;
    }
    for (unsigned i = 0; i < 2; ++i) {
        g_unix_set_fd_nonblocking(a->wake[i], TRUE, NULL);
        g_unix_set_fd_nonblocking(a->notify[i], TRUE, NULL);
    }
    g_mutex_init(&a->lock);
    g_cond_init(&a->available);
    a->context = g_main_context_new();
    a->requests = g_async_queue_new_full(free_request);
    a->replies = g_async_queue_new_full(free_reply);
    a->icon_monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    a->children = g_ptr_array_new();
    a->cancel = g_cancellable_new();
    a->generation = 1;
    a->thread = g_thread_new("np-apps", run, a);
    return a;
}
int np_apps_fd(struct np_apps *a) { return a->notify[0]; }
void np_apps_set_generation(struct np_apps *a, uint64_t generation)
{
    if (!a) return;
    g_mutex_lock(&a->lock);
    a->generation = generation;
    g_cond_signal(&a->available);
    g_mutex_unlock(&a->lock);
}
bool np_apps_request(struct np_apps *a, uint64_t generation, uint32_t token,
                     uint32_t action, const char *id)
{
    if (!a || !token || action > NP_APP_APPEARANCE || !id || strlen(id) > 4096 ||
        (action == NP_APP_APPEARANCE && strcmp(id, "dark") && strcmp(id, "light")) ||
        (action == NP_APP_LIST && *id) ||
        (action != NP_APP_LIST && (!*id || strchr(id, '/')))) { errno = EINVAL; return false; }
    if (g_async_queue_length(a->requests) >= 32) { errno = EAGAIN; return false; }
    struct request *r = g_new0(struct request, 1);
    *r = (struct request){ generation, token, action, g_strdup(id) };
    g_async_queue_push(a->requests, r);
    wake(a->wake[1]);
    return true;
}
bool np_apps_take(struct np_apps *a, struct np_app_reply *reply)
{
    char byte;
    (void)!read(a->notify[0], &byte, 1);
    g_mutex_lock(&a->lock);
    struct np_app_reply *r = g_async_queue_try_pop(a->replies);
    g_cond_signal(&a->available);
    g_mutex_unlock(&a->lock);
    if (!r) return false;
    *reply = *r; g_free(r);
    return true;
}
void np_apps_stop(struct np_apps *a)
{
    if (!a) return;
    g_mutex_lock(&a->lock);
    a->stopping = TRUE;
    g_cond_signal(&a->available);
    g_mutex_unlock(&a->lock);
    g_main_context_wakeup(a->context);
    g_cancellable_cancel(a->cancel);
    g_thread_join(a->thread);
    g_hash_table_unref(a->icon_monitors); g_ptr_array_unref(a->children);
    g_object_unref(a->cancel);
    g_async_queue_unref(a->requests); g_async_queue_unref(a->replies);
    g_main_context_unref(a->context);
    for (unsigned i = 0; i < 2; ++i) { close(a->wake[i]); close(a->notify[i]); }
    g_mutex_clear(&a->lock); g_cond_clear(&a->available); g_free(a);
}
