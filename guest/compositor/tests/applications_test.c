#define _GNU_SOURCE
#include "applications_worker.h"
#include "application_icons.h"
#include "windowwire.h"
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *root;
#ifndef NP_ICONS_ONLY
static void activate(GDBusConnection *bus, const char *sender, const char *path,
                     const char *interface, const char *method, GVariant *parameters,
                     GDBusMethodInvocation *call, gpointer data)
{
    (void)bus; (void)sender; (void)path; (void)interface; (void)method; (void)parameters;
    assert(g_file_set_contents(data, getenv("WAYLAND_DISPLAY"), -1, NULL));
    g_dbus_method_invocation_return_value(call, NULL);
}
static int activation_service(const char *output)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    assert(bus);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(
        "<node><interface name='org.freedesktop.Application'><method name='Activate'>"
        "<arg type='a{sv}' direction='in'/></method></interface></node>", NULL);
    static const GDBusInterfaceVTable vtable = { .method_call = activate };
    assert(g_dbus_connection_register_object(bus, "/org/nativepipe/Test", node->interfaces[0],
                                           &vtable, (void *)output, NULL, NULL));
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", "org.nativepipe.Test", 0),
        NULL, 0, 5000, NULL, NULL);
    assert(reply); g_variant_unref(reply);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
#endif
static void put(const char *relative, const char *text)
{
    char *path = g_build_filename(root, relative, NULL), *parent = g_path_get_dirname(path);
    assert(g_mkdir_with_parents(parent, 0700) == 0);
    assert(g_file_set_contents(path, text, -1, NULL));
    g_free(path); g_free(parent);
}
#ifndef NP_ICONS_ONLY
static uint32_t number(const unsigned char *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static struct np_app_reply next(struct np_apps *a)
{
    struct pollfd fd = { np_apps_fd(a), POLLIN, 0 };
    assert(poll(&fd, 1, 10000) == 1);
    struct np_app_reply r;
    assert(np_apps_take(a, &r));
    assert(r.length >= 12 && !memcmp(r.data, "NPAP", 4));
    return r;
}
static GHashTable *catalog(struct np_apps *a, unsigned token)
{
    assert(np_apps_request(a, 1, token, NP_APP_LIST, ""));
    GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (;;) {
        struct np_app_reply r = next(a);
        if (r.data[5] == 3) { free(r.data); continue; }
        assert(number(r.data + 8) == token);
        if (r.data[5] == 4) { assert(r.length == 16 && number(r.data + 12) == 0); free(r.data); break; }
        assert(r.data[5] == 1 && number(r.data + 12) <= 32);
        memcpy(r.data, "NPW2", 4);
        struct np_window_reader rd;
        assert(np_window_reader_init(&rd, r.data, r.length, NP_WINDOW_GUEST_TO_HOST));
        assert(np_window_read_u32(&rd) == token);
        unsigned count = np_window_read_u32(&rd);
        for (unsigned i = 0; i < count; ++i) {
            char *id = np_window_read_string(&rd), *name = np_window_read_string(&rd);
            assert(id && name && !g_hash_table_contains(ids, id));
            g_hash_table_insert(ids, id, name);
            for (int k = 0; k < 4; ++k) { char *field = np_window_read_string(&rd); assert(field); free(field); }
        }
        assert(np_window_reader_finished(&rd));
        free(r.data);
    }
    return ids;
}
static void await_change(struct np_apps *a)
{
    for (;;) {
        struct np_app_reply r = next(a);
        unsigned type = r.data[5]; free(r.data);
        if (type == 3) break;
    }
}
#endif
static void test_icons(void)
{
    put("config/gtk-3.0/settings.ini", "[Settings]\ngtk-icon-theme-name=Child\n");
    put("data/icons/Child/index.theme", "[Icon Theme]\nName=Child\nDirectories=64x64/apps\nInherits=Parent\n[64x64/apps]\nSize=64\nType=Fixed\n");
    put("system/icons/Parent/index.theme", "[Icon Theme]\nName=Parent\nDirectories=scalable/apps\n[scalable/apps]\nSize=64\nType=Scalable\nMinSize=16\nMaxSize=512\n");
    put("system/icons/Parent/scalable/apps/test-vector.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"512\" height=\"512\"><rect width=\"512\" height=\"512\" fill=\"red\"/></svg>");
    put("data/icons/Child/64x64/apps/test-xpm.xpm", "/* XPM */\nstatic char *i[]={\"2 2 1 1\",\"x c #0000ff\",\"xx\",\"xx\"};\n");
    put("system/icons/Parent/scalable/apps/test-symbolic.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\"><rect width=\"64\" height=\"64\" fill=\"blue\"/></svg>");
    const char *names[] = {"test-vector", "test-xpm", "test"};
    for (unsigned i = 0; i < G_N_ELEMENTS(names); ++i) {
        GIcon *icon = g_themed_icon_new(names[i]);
        GBytes *png = np_application_icon(icon);
        if (g_bytes_get_size(png) <= 50) {
            char *path = np_application_icon_path(icon);
            fprintf(stderr, "Could not decode icon %s (%s)\n", names[i], path ? path : "not found");
            if (path) {
                GError *error = NULL;
                GdkPixbuf *probe = gdk_pixbuf_new_from_file(path, &error);
                if (error) { fprintf(stderr, "Decoder: %s\n", error->message); g_error_free(error); }
                g_clear_object(&probe);
            }
            g_free(path);
        }
        assert(g_bytes_get_size(png) > 50 && g_bytes_get_size(png) < 65536);
        g_bytes_unref(png); g_object_unref(icon);
    }
    char *fifo = g_build_filename(root, "fifo", NULL);
    GIcon *missing = g_themed_icon_new("no-such-icon-symbolic");
    GBytes *empty = np_application_icon(missing);
    assert(g_bytes_get_size(empty) == 0);
    g_bytes_unref(empty); g_object_unref(missing);
    char *large = g_build_filename(root, "large.png", NULL);
    GdkPixbuf *image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 256, 256);
    unsigned char *pixels = gdk_pixbuf_get_pixels(image);
    GRand *random = g_rand_new_with_seed(42);
    for (int i = 0; i < 256 * 256 * 4; ++i) pixels[i] = g_rand_int(random);
    assert(gdk_pixbuf_save(image, large, "png", NULL, NULL));
    struct stat st; assert(stat(large, &st) == 0 && st.st_size > 65536);
    GFile *large_file = g_file_new_for_path(large);
    GIcon *large_icon = g_file_icon_new(large_file);
    GBytes *thumbnail = np_application_icon(large_icon);
    assert(g_bytes_get_size(thumbnail) > 50 && g_bytes_get_size(thumbnail) < 65536);
    g_bytes_unref(thumbnail);
    assert(chmod(large, 0000) == 0);
    thumbnail = np_application_icon(large_icon);
    assert(g_bytes_get_size(thumbnail) == 0);
    assert(chmod(large, 0600) == 0);
    g_bytes_unref(thumbnail); g_object_unref(large_icon); g_object_unref(large_file);
    g_object_unref(image); g_rand_free(random); g_free(large);
    assert(mkfifo(fifo, 0600) == 0);
    GFile *file = g_file_new_for_path(fifo);
    GIcon *icon = g_file_icon_new(file);
    GBytes *png = np_application_icon(icon);
    assert(g_bytes_get_size(png) == 0);
    g_bytes_unref(png); g_object_unref(icon); g_object_unref(file); g_free(fifo);
}
int main(int argc, char **argv)
{
#ifndef NP_ICONS_ONLY
    if (argc == 3 && !strcmp(argv[1], "--activate")) return activation_service(argv[2]);
#endif
    if (argc > 2 && !strcmp(argv[1], "--record")) {
        GString *out = g_string_new("");
        char *cwd = g_get_current_dir();
        g_string_append_printf(out, "cwd=%s\nwayland=%s\n", cwd, getenv("WAYLAND_DISPLAY"));
        for (int i = 3; i < argc; ++i) g_string_append_printf(out, "arg=[%s]\n", argv[i]);
        assert(g_file_set_contents(argv[2], out->str, out->len, NULL));
        return 7;
    }
    char *binary = realpath(argv[0], NULL);
    assert(binary);
    /* CI containers run as root; the service itself must never do so. */
    if (geteuid() == 0) { assert(setgid(65534) == 0); assert(setuid(65534) == 0); }
    root = g_dir_make_tmp("nativepipe-applications-test-XXXXXX", NULL);
    assert(root);
    setenv("HOME", root, 1);
    char *data = g_build_filename(root, "data", NULL), *system = g_build_filename(root, "system", NULL);
    char *config = g_build_filename(root, "config", NULL), *run = g_build_filename(root, "runtime", NULL);
    assert(g_mkdir_with_parents(run, 0700) == 0);
    /* Isolate desktop entries/themes, not the distro's image decoder config.
     * GdkPixbuf 2.44 resolves its Glycin loaders through XDG_DATA_DIRS too. */
    const char *original_dirs = getenv("XDG_DATA_DIRS");
    char **dirs = g_strsplit(original_dirs ? original_dirs : "/usr/local/share:/usr/share", ":", -1);
    assert(g_mkdir_with_parents(system, 0700) == 0);
    char **mime_dirs = g_new0(char *, g_strv_length(dirs) + 1);
    for (int i = 0; dirs[i]; ++i) mime_dirs[i] = g_build_filename(dirs[i], "mime", NULL);
    g_content_type_set_mime_dirs((const char *const *)mime_dirs);
    g_strfreev(mime_dirs);
    const char *runtime_data[] = {"glycin-loaders"};
    for (unsigned k = 0; k < G_N_ELEMENTS(runtime_data); ++k) {
        for (int i = 0; dirs[i]; ++i) {
            char *decoder = g_build_filename(dirs[i], runtime_data[k], NULL);
            char *link = g_build_filename(system, runtime_data[k], NULL);
            if (g_file_test(decoder, G_FILE_TEST_IS_DIR)) {
                assert(symlink(decoder, link) == 0);
                g_free(decoder); g_free(link); break;
            }
            g_free(decoder); g_free(link);
        }
    }
    g_strfreev(dirs);
    setenv("XDG_DATA_HOME", data, 1); setenv("XDG_DATA_DIRS", system, 1);
    setenv("XDG_CONFIG_HOME", config, 1); setenv("XDG_CURRENT_DESKTOP", "NativePipe", 1);
    setenv("XDG_RUNTIME_DIR", run, 1); setenv("WAYLAND_DISPLAY", "nativepipe-test", 1);
#ifndef NP_ICONS_ONLY
    GTestDBus *bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    char *services = g_build_filename(root, "services", NULL);
    char *activation_output = g_build_filename(root, "activation.txt", NULL);
    char *service = g_strdup_printf("[D-BUS Service]\nName=org.nativepipe.Test\nExec=%s --activate %s\n", binary, activation_output);
    put("services/org.nativepipe.Test.service", service);
    g_test_dbus_add_service_dir(bus, services);
    g_test_dbus_up(bus);
    setenv("NP_PRIVATE_APPLICATION_BUS", "1", 1);
#endif
    put("system/applications/masked.desktop", "[Desktop Entry]\nType=Application\nName=Masked\nExec=true\n");
    put("data/applications/masked.desktop", "[Desktop Entry]\nHidden=true\n");
    put("data/applications/nested/tool.desktop", "[Desktop Entry]\nType=Application\nName=Nested\nExec=true\n");
    put("data/applications/hidden.desktop", "[Desktop Entry]\nType=Application\nName=Hidden\nExec=true\nNoDisplay=true\n");
    put("data/applications/wrong-desktop.desktop", "[Desktop Entry]\nType=Application\nName=Wrong\nExec=true\nOnlyShowIn=Other;\n");
    put("data/applications/missing.desktop", "[Desktop Entry]\nType=Application\nName=Missing\nExec=true\nTryExec=/no/such/binary\n");
    for (int i = 0; i < 600; ++i) {
        char *path = g_strdup_printf("system/applications/app-%03d.desktop", i);
        put(path, "[Desktop Entry]\nType=Application\nName=Many\nExec=true\n"); g_free(path);
    }
    char *output = g_build_filename(root, "record.txt", NULL);
    char *entry = g_strdup_printf("[Desktop Entry]\nType=Application\nName=Launch Test\nPath=%s\nIcon=test-icon\nExec=%s --record %s \"\" \"two words\" %%c %%k %%i", root, binary, output);
    GString *args = g_string_new(entry);
    for (int i = 0; i < 40; ++i) g_string_append(args, " extra");
    g_string_append(args, "\n"); put("data/applications/launch.desktop", args->str);
    test_icons();
#ifdef NP_ICONS_ONLY
    printf("PASS: inherited SVG, XPM, bounded PNG conversion and FIFO rejection (%s)\n", root);
    return 0;
#else
    struct np_apps *a = np_apps_start(); assert(a);
    GHashTable *ids = catalog(a, 1);
    assert(g_hash_table_size(ids) == 602);
    assert(g_hash_table_contains(ids, "nested-tool.desktop"));
    assert(!g_hash_table_contains(ids, "masked.desktop"));
    g_hash_table_unref(ids);
    put("data/applications/new.desktop", "[Desktop Entry]\nType=Application\nName=New\nExec=true\n");
    await_change(a);
    ids = catalog(a, 2); assert(g_hash_table_size(ids) == 603); g_hash_table_unref(ids);
    assert(np_apps_request(a, 1, 3, NP_APP_LAUNCH, "launch.desktop"));
    gboolean launched = FALSE;
    for (;;) {
        struct np_app_reply r = next(a);
        if (r.data[5] == 2) { assert(number(r.data + 12) > 0 && number(r.data + 16) == 0); launched = TRUE; }
        gboolean exited = r.data[5] == 6;
        if (exited) assert(launched && number(r.data + 16) == 7);
        free(r.data); if (exited) break;
    }
    char *contents = NULL; assert(g_file_get_contents(output, &contents, NULL, NULL));
    assert(strstr(contents, "arg=[]\narg=[two words]\narg=[Launch Test]\n"));
    assert(strstr(contents, "data/applications/launch.desktop]"));
    assert(strstr(contents, "arg=[--icon]\narg=[test-icon]"));
    char **lines = g_strsplit(contents, "arg=[extra]", -1); assert(g_strv_length(lines) == 41); g_strfreev(lines);
    assert(strstr(contents, "wayland=nativepipe-test")); g_free(contents);
    put("data/applications/org.nativepipe.Test.desktop", "[Desktop Entry]\nType=Application\nName=Activation\nDBusActivatable=true\n");
    await_change(a);
    ids = catalog(a, 4); assert(g_hash_table_contains(ids, "org.nativepipe.Test.desktop")); g_hash_table_unref(ids);
    assert(np_apps_request(a, 1, 5, NP_APP_LAUNCH, "org.nativepipe.Test.desktop"));
    for (;;) {
        struct np_app_reply r = next(a);
        if (r.data[5] == 2) {
            assert(number(r.data + 8) == 5 && number(r.data + 12) == 0 && number(r.data + 16) == 0);
            free(r.data); break;
        }
        free(r.data);
    }
    assert(g_file_get_contents(activation_output, &contents, NULL, NULL));
    assert(!strcmp(contents, "nativepipe-test")); g_free(contents);
    put("data/applications/org.nativepipe.Missing.desktop", "[Desktop Entry]\nType=Application\nName=Missing service\nDBusActivatable=true\n");
    await_change(a);
    assert(np_apps_request(a, 1, 6, NP_APP_LAUNCH, "org.nativepipe.Missing.desktop"));
    for (;;) {
        struct np_app_reply r = next(a);
        if (r.data[5] == 2) {
            assert(number(r.data + 8) == 6 && number(r.data + 16) > 0);
            free(r.data); break;
        }
        free(r.data);
    }
    GString *large_entry = g_string_new("[Desktop Entry]\nType=Application\nExec=true\nName=");
    for (unsigned i = 0; i < 70000; ++i) g_string_append_c(large_entry, 'x');
    g_string_append_c(large_entry, '\n');
    put("data/applications/oversized.desktop", large_entry->str);
    g_string_free(large_entry, TRUE);
    await_change(a);
    assert(np_apps_request(a, 1, 7, NP_APP_LIST, ""));
    for (;;) {
        struct np_app_reply r = next(a);
        assert(r.length <= 65536);
        gboolean ended = r.data[5] == 4;
        if (ended) assert(number(r.data + 8) == 7 && number(r.data + 12) > 0);
        free(r.data); if (ended) break;
    }
    char *oversized = g_build_filename(root, "data/applications/oversized.desktop", NULL);
    assert(unlink(oversized) == 0); g_free(oversized);
    await_change(a);
    ids = catalog(a, 8); assert(g_hash_table_size(ids) == 605); g_hash_table_unref(ids);
    /* Closing while the bounded reply handoff is full must join promptly. */
    assert(np_apps_request(a, 1, 9, NP_APP_LIST, ""));
    struct pollfd pending = { np_apps_fd(a), POLLIN, 0 };
    assert(poll(&pending, 1, 10000) == 1); /* Started, but deliberately do not drain it. */
    for (unsigned i = 0; i < 32; ++i) assert(np_apps_request(a, 1, 10 + i, NP_APP_ICON, "launch.desktop"));
    assert(!np_apps_request(a, 1, 42, NP_APP_ICON, "launch.desktop") && errno == EAGAIN);
    np_apps_stop(a);
    g_test_dbus_down(bus); g_object_unref(bus);
    printf("PASS: 600+ entries, XDG precedence, monitor, icons, launch arguments, D-Bus, limits and shutdown (%s)\n", root);
    return 0;
#endif
}
