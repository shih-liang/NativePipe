#include "application_icons.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gunixinputstream.h>
#include <librsvg/rsvg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <glib/gstdio.h>

void np_application_icons_init(void)
{
    /* Only release bundles have private image loader modules. Native distro
     * builds use their installed cache. No GDK environment variable escapes into
     * applications or D-Bus activation. */
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length <= 0) return;
    executable[length] = 0;
    char *bin = g_path_get_dirname(executable);
    char *root = g_path_get_dirname(bin);
    char *query = g_build_filename(bin, "gdk-pixbuf-query-loaders", NULL);
    char *directory = g_build_filename(root, "lib", "pixbuf", NULL);
    GDir *dir = g_file_test(query, G_FILE_TEST_IS_EXECUTABLE) ? g_dir_open(directory, 0, NULL) : NULL;
    if (dir) {
        /* Initialize the default list first. The explicitly bundled loaders
         * are then prepended, rather than displaced by lazy system scanning. */
        GSList *formats = gdk_pixbuf_get_formats();
        g_slist_free(formats);
        GPtrArray *args = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(args, g_strdup(query));
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (g_str_has_suffix(name, ".so")) g_ptr_array_add(args, g_build_filename(directory, name, NULL));
        }
        g_dir_close(dir);
        if (args->len > 1) {
            g_ptr_array_add(args, NULL);
            char *cache = NULL; int status = -1;
            if (g_spawn_sync(NULL, (char **)args->pdata, NULL, G_SPAWN_DEFAULT, NULL, NULL, &cache, NULL, &status, NULL) && status == 0) {
                char *temp = g_dir_make_tmp("nativepipe-icons-XXXXXX", NULL);
                if (temp) {
                    char *file = g_build_filename(temp, "loaders.cache", NULL);
                    if (g_file_set_contents(file, cache, -1, NULL)) gdk_pixbuf_init_modules(temp, NULL);
                    g_unlink(file); g_rmdir(temp); g_free(file); g_free(temp);
                }
            }
            g_free(cache);
        }
        g_ptr_array_unref(args);
    }
    g_free(directory); g_free(query); g_free(root); g_free(bin);
}

static char *current_theme(void)
{
    /* Read the session's GTK preference without initializing GTK/a display. */
    GKeyFile *key = g_key_file_new();
    char *theme = NULL;
    const char *configs[] = {"gtk-4.0/settings.ini", "gtk-3.0/settings.ini", "kdeglobals"};
    for (unsigned i = 0; i < G_N_ELEMENTS(configs) && !theme; ++i) {
        char *path = g_build_filename(g_get_user_config_dir(), configs[i], NULL);
        if (g_key_file_load_from_file(key, path, G_KEY_FILE_NONE, NULL))
            theme = g_key_file_get_string(key, i == 2 ? "Icons" : "Settings",
                                         i == 2 ? "Theme" : "gtk-icon-theme-name", NULL);
        g_free(path);
    }
    g_key_file_unref(key);
    if (!theme) {
        GSettingsSchemaSource *source = g_settings_schema_source_get_default();
        GSettingsSchema *schema = source ? g_settings_schema_source_lookup(
            source, "org.gnome.desktop.interface", TRUE) : NULL;
        if (schema && g_settings_schema_has_key(schema, "icon-theme")) {
            GSettings *settings = g_settings_new_full(schema, NULL, NULL);
            theme = g_settings_get_string(settings, "icon-theme");
            g_object_unref(settings);
        }
        if (schema) g_settings_schema_unref(schema);
    }
    if (!theme || !*theme) { g_free(theme); theme = g_strdup("hicolor"); }
    return theme;
}

static gboolean component(const char *name)
{
    return name && *name && !strchr(name, '/') && strcmp(name, ".") && strcmp(name, "..");
}

static GPtrArray *icon_roots(void)
{
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(roots, g_build_filename(g_get_user_data_dir(), "icons", NULL));
    g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".icons", NULL));
    const char *const *dirs = g_get_system_data_dirs();
    for (int i = 0; dirs[i]; ++i)
        g_ptr_array_add(roots, g_build_filename(dirs[i], "icons", NULL));
    return roots;
}

static char *find_image(const char *base, const char *theme, const char *dir, const char *name)
{
    const char *extensions[] = {"png", "svg", "xpm"};
    for (unsigned i = 0; i < G_N_ELEMENTS(extensions); ++i) {
        char *file = g_strdup_printf("%s.%s", name, extensions[i]);
        char *path = g_build_filename(base, theme, dir, file, NULL);
        g_free(file);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) return path;
        g_free(path);
    }
    return NULL;
}

static int distance(GKeyFile *key, const char *dir)
{
    int size = g_key_file_get_integer(key, dir, "Size", NULL);
    int scale = g_key_file_get_integer(key, dir, "Scale", NULL);
    if (scale <= 0) scale = 1;
    if (size <= 0 || size > 16384 || scale > 16) return INT_MAX;
    int low = size, high = size;
    char *type = g_key_file_get_string(key, dir, "Type", NULL);
    if (g_strcmp0(type, "Scalable") == 0) {
        if (g_key_file_has_key(key, dir, "MinSize", NULL))
            low = g_key_file_get_integer(key, dir, "MinSize", NULL);
        if (g_key_file_has_key(key, dir, "MaxSize", NULL))
            high = g_key_file_get_integer(key, dir, "MaxSize", NULL);
    } else if (!type || !strcmp(type, "Threshold")) {
        int threshold = g_key_file_has_key(key, dir, "Threshold", NULL)
            ? g_key_file_get_integer(key, dir, "Threshold", NULL) : 2;
        if (threshold < 0 || threshold > 16384) threshold = 2;
        low -= threshold; high += threshold;
    }
    g_free(type);
    if (low < -16384 || high > 32768 || low > high) return INT_MAX;
    low *= scale; high *= scale;
    return 64 < low ? low - 64 : 64 > high ? 64 - high : 0;
}

static char *in_theme(GPtrArray *roots, const char *theme, const char *name, GHashTable *visited)
{
    if (!component(theme) || g_hash_table_contains(visited, theme) || g_hash_table_size(visited) >= 64) return NULL;
    g_hash_table_add(visited, g_strdup(theme));
    GKeyFile *index = g_key_file_new();
    gboolean found = FALSE;
    for (unsigned i = 0; i < roots->len && !found; ++i) {
        char *path = g_build_filename(roots->pdata[i], theme, "index.theme", NULL);
        found = g_key_file_load_from_file(index, path, G_KEY_FILE_NONE, NULL);
        g_free(path);
    }
    char *best = NULL;
    int best_distance = INT_MAX;
    if (found) {
        const char *keys[] = {"Directories", "ScaledDirectories"};
        /* Icon theme lists use commas, unlike desktop-entry lists. */
        g_key_file_set_list_separator(index, ',');
        for (unsigned k = 0; k < G_N_ELEMENTS(keys); ++k) {
            char **dirs = g_key_file_get_string_list(index, "Icon Theme", keys[k], NULL, NULL);
            for (int d = 0; dirs && dirs[d]; ++d) {
                int score = distance(index, dirs[d]);
                if (score >= best_distance || g_path_is_absolute(dirs[d]) || strstr(dirs[d], "..")) continue;
                for (unsigned i = 0; i < roots->len; ++i) {
                    char *path = find_image(roots->pdata[i], theme, dirs[d], name);
                    if (path) { g_free(best); best = path; best_distance = score; break; }
                }
            }
            g_strfreev(dirs);
        }
        if (!best) {
            char **parents = g_key_file_get_string_list(index, "Icon Theme", "Inherits", NULL, NULL);
            for (int i = 0; parents && parents[i] && !best; ++i)
                best = in_theme(roots, parents[i], name, visited);
            g_strfreev(parents);
        }
    }
    g_key_file_unref(index);
    return best;
}

static char *named_icon(GPtrArray *roots, const char *theme, const char *name)
{
    if (!component(name)) return NULL;
    GHashTable *visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *path = in_theme(roots, theme, name, visited);
    if (!path) path = in_theme(roots, "hicolor", name, visited);
    g_hash_table_unref(visited);
    for (unsigned i = 0; i < roots->len && !path; ++i)
        path = find_image(roots->pdata[i], "", "", name);
    const char *const *dirs = g_get_system_data_dirs();
    for (int i = 0; dirs[i] && !path; ++i)
        path = find_image(dirs[i], "pixmaps", "", name);
    return path;
}
char *np_application_icon_path(GIcon *icon)
{
    if (G_IS_FILE_ICON(icon)) return g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));
    if (!G_IS_THEMED_ICON(icon)) return NULL;
    const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
    GPtrArray *roots = icon_roots();
    char *theme = current_theme(), *path = NULL;
    for (int n = 0; names && names[n] && !path; ++n)
        path = named_icon(roots, theme, names[n]);
    /* Some GTK applications install only their symbolic application icon. */
    for (int n = 0; names && names[n] && !path; ++n) {
        if (!component(names[n]) || g_str_has_suffix(names[n], "-symbolic")) continue;
        char *name = g_strconcat(names[n], "-symbolic", NULL);
        path = named_icon(roots, theme, name);
        g_free(name);
    }
    g_free(theme); g_ptr_array_unref(roots);
    return path;
}

static cairo_status_t png_bytes(void *data, const unsigned char *bytes, unsigned length)
{
    g_byte_array_append(data, bytes, length);
    return CAIRO_STATUS_SUCCESS;
}

GBytes *np_application_icon(GIcon *icon)
{
    char *path = np_application_icon_path(icon);
    if (!path) return g_bytes_new(NULL, 0);
    gboolean svg = g_str_has_suffix(path, ".svg");
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    g_free(path);
    struct stat st;
    if (fd < 0) return g_bytes_new(NULL, 0);
    /* Inspect the opened inode, not a path that can be replaced by a FIFO. */
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > 16 * 1024 * 1024) {
        close(fd); return g_bytes_new(NULL, 0);
    }
    GInputStream *input = g_unix_input_stream_new(fd, TRUE);
    /* Link the SVG decoder explicitly: a relocated remote bundle must not
     * depend on the builder's absolute GdkPixbuf loaders.cache paths. */
    if (svg) {
        RsvgHandle *handle = rsvg_handle_new_from_stream_sync(input, NULL, RSVG_HANDLE_FLAGS_NONE, NULL, NULL);
        GByteArray *bytes = g_byte_array_new();
        if (handle) {
            cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
            cairo_t *cr = cairo_create(surface);
            RsvgRectangle viewport = {0, 0, 64, 64};
            if (rsvg_handle_render_document(handle, cr, &viewport, NULL))
                cairo_surface_write_to_png_stream(surface, png_bytes, bytes);
            cairo_destroy(cr); cairo_surface_destroy(surface); g_object_unref(handle);
        }
        g_object_unref(input);
        return g_byte_array_free_to_bytes(bytes);
    }
    GdkPixbuf *pixbuf = input ? gdk_pixbuf_new_from_stream_at_scale(
        G_INPUT_STREAM(input), 64, 64, TRUE, NULL, NULL) : NULL;
    char *png = NULL;
    gsize size = 0;
    if (pixbuf) gdk_pixbuf_save_to_buffer(pixbuf, &png, &size, "png", NULL, NULL);
    g_clear_object(&pixbuf); g_clear_object(&input);
    return png ? g_bytes_new_take(png, size) : g_bytes_new(NULL, 0);
}
