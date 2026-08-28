#include <gtk/gtk.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <json-c/json.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CONFIG_PATH "/.config/crosshair/config.json"

// --------------------------------------------------------------------------
// Global State
// --------------------------------------------------------------------------
static int verbose = 0;
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

// Current crosshair state (globals so load_config and draw can access them)
static int current_style = 0;
static double current_red = 0.0;
static double current_green = 1.0;
static double current_blue = 0.0;
static double current_alpha = 1.0;
static double current_size = 3.0;
static double current_thickness = 1.0;
static double current_gap = 0.0;
static double current_offset_x = 0.0;
static double current_offset_y = 0.0;
static int is_enabled = 0;

// GTK window and drawing area
static GtkWidget *window = NULL;
static GtkWidget *drawing_area = NULL;

// --------------------------------------------------------------------------
// Config Loading (MUST match the new GUI JSON format)
// --------------------------------------------------------------------------
void load_config() {
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);

    FILE *f = fopen(path, "r");
    if (!f) {
        // Default settings
        current_style = 0;
        current_red = 0.0; current_green = 1.0; current_blue = 0.0;
        current_alpha = 1.0; current_size = 3.0; current_thickness = 1.0;
        current_gap = 0.0; current_offset_x = 0.0; current_offset_y = 0.0;
        is_enabled = 0;
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(len + 1);
    fread(data, 1, len, f);
    data[len] = '\0';
    fclose(f);

    json_object *root = json_tokener_parse(data);
    free(data);
    if (!root) return;

    json_object *tmp;
    // Load style
    if (json_object_object_get_ex(root, "style", &tmp))
        current_style = json_object_get_int(tmp);

    // Load color from the new object
    json_object *color_obj;
    if (json_object_object_get_ex(root, "color", &color_obj)) {
        json_object *r, *g, *b;
        if (json_object_object_get_ex(color_obj, "red", &r)) current_red = json_object_get_double(r);
        if (json_object_object_get_ex(color_obj, "green", &g)) current_green = json_object_get_double(g);
        if (json_object_object_get_ex(color_obj, "blue", &b)) current_blue = json_object_get_double(b);
    }

    if (json_object_object_get_ex(root, "alpha", &tmp)) current_alpha = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "size", &tmp)) current_size = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "thickness", &tmp)) current_thickness = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "gap", &tmp)) current_gap = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "offset_x", &tmp)) current_offset_x = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "offset_y", &tmp)) current_offset_y = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "enabled", &tmp)) is_enabled = json_object_get_boolean(tmp);

    json_object_put(root);
}

// --------------------------------------------------------------------------
// Drawing Logic
// --------------------------------------------------------------------------
static void draw_cross_lines(cairo_t *cr, double cx, double cy, double s, double t, double g) {
    double ss = s, ts = t, gs = g;
    if (gs < 0) gs = 0;
    if (gs > ss) gs = ss;
    cairo_set_line_width(cr, ts);
    cairo_move_to(cr, cx - ss, cy);
    cairo_line_to(cr, cx - gs, cy);
    cairo_move_to(cr, cx + gs, cy);
    cairo_line_to(cr, cx + ss, cy);
    cairo_move_to(cr, cx, cy - ss);
    cairo_line_to(cr, cx, cy - gs);
    cairo_move_to(cr, cx, cy + gs);
    cairo_line_to(cr, cx, cy + ss);
    cairo_stroke(cr);
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)widget; (void)data;
    debug("on_draw: allocation %dx%d\n", gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
    
    // Center of the screen
    double cx = gtk_widget_get_allocated_width(widget) / 2.0 + current_offset_x;
    double cy = gtk_widget_get_allocated_height(widget) / 2.0 + current_offset_y;
    
    double s = current_size;
    double t = current_thickness;

    debug("render_crosshair: %dx%d\n", (int)cx, (int)cy);

    // Clear the surface
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    
    // If disabled, return leaving it transparent
    if (!is_enabled) {
        debug("  crosshair disabled, surface stays transparent\n");
        return FALSE;
    }

    // Draw crosshair
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, current_red, current_green, current_blue, current_alpha);
    
    switch (current_style) {
        case 0: // Dot
            cairo_arc(cr, cx, cy, s, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case 1: // Cross
            draw_cross_lines(cr, cx, cy, s, t, current_gap);
            break;
        case 2: // Circle
            cairo_set_line_width(cr, t);
            cairo_arc(cr, cx, cy, s, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case 3: // Dot + Cross
            cairo_arc(cr, cx, cy, s * 0.4, 0, 2 * M_PI);
            cairo_fill(cr);
            draw_cross_lines(cr, cx, cy, s, t, current_gap);
            break;
        default:
            break;
    }
    
    debug("  surface blitted\n");
    return FALSE;
}

// --------------------------------------------------------------------------
// Initialization & Signals
// --------------------------------------------------------------------------
static void reload_daemon() {
    debug("reload_daemon: sending SIGHUP\n");
    // This will be handled by the main loop, so just queue a redraw
    if (drawing_area) {
        gtk_widget_queue_draw(drawing_area);
    }
}

static void sig_handler(int sig) {
    (void)sig;
    debug("Received signal %d, cleaning up...\n", sig);
    gtk_main_quit();
}

// --------------------------------------------------------------------------
// Main
// --------------------------------------------------------------------------
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
    }

    debug("crosshaird starting (pid %d)\n", getpid());
    load_config();

    gtk_init(&argc, &argv);
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGHUP, (void (*)(int))reload_daemon); // Handle reload

    // Create the overlay window
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(window), 1920, 1080); // Default full-size, will resize
    gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
    gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);

    // Configure Layer Shell
    gtk_layer_init_for_window(window);
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(window, GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);

    // Drawing area
    drawing_area = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(window), drawing_area);
    g_signal_connect(drawing_area, "draw", G_CALLBACK(on_draw), NULL);

    gtk_widget_show_all(window);
    debug("Entering GTK main loop\n");
    gtk_main();
    return 0;
}