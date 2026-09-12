/* GTK animation for end-to-end display/resize tests when gtk4-demo is absent.
 * Draw callbacks report producer FPS; the host separately counts presentation.
 * Build: cc remote_animation.c $(pkg-config --cflags --libs gtk4) -o remote-animation
 */
#include <gtk/gtk.h>
#include <stdio.h>

static unsigned frame, interval_frames, clicks, window_count = 1;
static gint64 interval_start;

static void draw(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
    (void)area; (void)data;
    frame++; interval_frames++;
    cairo_set_source_rgb(cr, .04, .07, .12); cairo_paint(cr);
    for (int i = 0; i < 12; i++) {
        cairo_set_source_rgb(cr, .15 + i * .05, .7, .9 - i * .05);
        cairo_rectangle(cr, (frame * 7 + i * 71) % (w + 80) - 80,
                        80 + i * (h - 100) / 12, 80, 12);
        cairo_fill(cr);
    }
    double marker = (clicks % 8 + 1) / 10.0;
    cairo_set_source_rgb(cr, marker, marker, marker);
    cairo_rectangle(cr, w / 2 - 60, h / 2 - 40, 120, 80); cairo_fill(cr);
    char text[96]; snprintf(text, sizeof(text), "Frame %u  |  %d x %d  |  Click %u", frame, w, h, clicks);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 28); cairo_move_to(cr, 20, 48); cairo_show_text(cr, text);
    gint64 now = g_get_monotonic_time();
    if (!interval_start) interval_start = now;
    if (now - interval_start >= 2000000) {
        fprintf(stderr, "ANIMATION draw_fps=%.2f frame=%u\n",
                interval_frames * 1e6 / (now - interval_start), frame);
        interval_start = now; interval_frames = 0;
    }
}

static gboolean tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{ (void)clock; (void)data; gtk_widget_queue_draw(widget); return G_SOURCE_CONTINUE; }

static void clicked(GtkGestureClick *gesture, int count, double x, double y, gpointer data)
{ (void)gesture; (void)count; (void)x; (void)y; clicks++; gtk_widget_queue_draw(data); }

static void activate(GtkApplication *app, gpointer data)
{
    (void)data;
    for (unsigned i = 0; i < window_count; i++) {
        GtkWidget *window = gtk_application_window_new(app);
        char title[64]; snprintf(title, sizeof(title), "NativePipe Animation %u", i + 1);
        gtk_window_set_title(GTK_WINDOW(window), title);
        gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
        GtkWidget *area = gtk_drawing_area_new();
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
        gtk_widget_add_tick_callback(area, tick, NULL, NULL);
        GtkGesture *click = gtk_gesture_click_new();
        g_signal_connect(click, "pressed", G_CALLBACK(clicked), area);
        gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));
        gtk_window_set_child(GTK_WINDOW(window), area);
        gtk_window_present(GTK_WINDOW(window));
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--two-windows")) { window_count = 2; argc = 1; }
    GtkApplication *app = gtk_application_new("org.nativepipe.Animation", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
