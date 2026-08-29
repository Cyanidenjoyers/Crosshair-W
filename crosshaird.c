#define _USE_MATH_DEFINES

#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <json-c/json.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#define CONFIG_PATH "/.config/crosshair/config.json"

int verbose = 0;
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

typedef struct {
    int style;
    double red, green, blue, alpha;
    double size;
    double thickness;
    double gap;
    double offset_x, offset_y;
    int enabled;
    int use_gamma;
    double gamma;
} CrosshairConfig;

static CrosshairConfig config = {
    .style = 0,
    .red = 0.0,
    .green = 1.0,
    .blue = 0.0,
    .alpha = 1.0,
    .size = 3.0,
    .thickness = 1.0,
    .gap = 0.0,
    .offset_x = 0.0,
    .offset_y = 0.0,
    .enabled = 1,
    .use_gamma = 0,
    .gamma = 1.2
};

static pid_t wlsunset_pid = -1;
static GtkWidget *overlay_window = NULL;
static GtkWidget *drawing_area = NULL;
static cairo_surface_t *crosshair_surface = NULL;
static int surface_width = 0, surface_height = 0;

static int prev_use_gamma = -1;
static double prev_gamma = -1.0;

// --- Helper: set transparent background using CSS ---
static void set_transparent_background(GtkWidget *widget) {
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = " * { background-color: transparent; } ";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// --- Config loading ---
void load_config() {
    const char *home = getenv("HOME");
    if (!home) {
        debug("HOME not set, using built-in defaults.\n");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);
    
    debug("load_config: reading %s\n", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        debug("Config file not found, creating default.\n");
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.config/crosshair", home);
        mkdir(dir, 0755);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "{\n"
                       "  \"style\": 0,\n"
                       "  \"red\": 0.0,\n"
                       "  \"green\": 1.0,\n"
                       "  \"blue\": 0.0,\n"
                       "  \"alpha\": 1.0,\n"
                       "  \"size\": 3.0,\n"
                       "  \"thickness\": 1.0,\n"
                       "  \"gap\": 0.0,\n"
                       "  \"offset_x\": 0.0,\n"
                       "  \"offset_y\": 0.0,\n"
                       "  \"enabled\": 1,\n"
                       "  \"use_gamma\": 0,\n"
                       "  \"gamma\": 1.2,\n"
                       "  \"preview_bg\": \"#000000\",\n"
                       "  \"theme\": \"default\"\n"
                       "}\n");
            fclose(f);
        }
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
    if (!root) {
        debug("Failed to parse JSON config.\n");
        return;
    }
    
    json_object *tmp;
    if (json_object_object_get_ex(root, "style", &tmp))
        config.style = json_object_get_int(tmp);
    if (json_object_object_get_ex(root, "red", &tmp))
        config.red = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "green", &tmp))
        config.green = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "blue", &tmp))
        config.blue = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "alpha", &tmp))
        config.alpha = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "size", &tmp))
        config.size = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "thickness", &tmp))
        config.thickness = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "gap", &tmp))
        config.gap = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "offset_x", &tmp))
        config.offset_x = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "offset_y", &tmp))
        config.offset_y = json_object_get_double(tmp);
    if (json_object_object_get_ex(root, "enabled", &tmp))
        config.enabled = json_object_get_boolean(tmp);
    if (json_object_object_get_ex(root, "use_gamma", &tmp))
        config.use_gamma = json_object_get_boolean(tmp);
    if (json_object_object_get_ex(root, "gamma", &tmp))
        config.gamma = json_object_get_double(tmp);
    
    json_object_put(root);
    
    debug("Config loaded: style=%d, color=(%.2f,%.2f,%.2f), alpha=%.2f, size=%.1f, thick=%.1f, gap=%.1f, offset=(%.1f,%.1f), enabled=%d, use_gamma=%d, gamma=%.2f\n",
          config.style, config.red, config.green, config.blue, config.alpha,
          config.size, config.thickness, config.gap, config.offset_x, config.offset_y,
          config.enabled, config.use_gamma, config.gamma);
}

// --- Gamma control ---
static void wlsunset_exited_cb(GPid pid, gint status, gpointer data) {
    (void)status;
    (void)data;
    debug("wlsunset (pid %d) reaped\n", pid);
    g_spawn_close_pid(pid);
}

void kill_all_wlsunset() {
    debug("Killing all wlsunset processes\n");
    system("pkill -f wlsunset 2>/dev/null");
}

void kill_wlsunset() {
    if (wlsunset_pid > 0) {
        debug("Killing tracked wlsunset (pid %d)\n", wlsunset_pid);
        kill(wlsunset_pid, SIGTERM);
        wlsunset_pid = -1;
    }
}

void spawn_gamma_control() {
    // Only respawn if state or value changed
    if (config.use_gamma != prev_use_gamma || config.gamma != prev_gamma) {
        debug("Gamma state/value changed, respawning...\n");
        // Kill all instances to ensure the compositor releases the gamma control
        kill_wlsunset();
        kill_all_wlsunset(); // nuke any strays
        usleep(120000);      // 120ms delay to let compositor release gamma (It caused some issues...)
        prev_use_gamma = config.use_gamma;
        prev_gamma = config.gamma;
    } else {
        debug("Gamma state unchanged, skipping respawn.\n");
        return;
    }

    if (!config.use_gamma) {
        debug("Gamma disabled, no wlsunset started.\n");
        return;
    }

    if (system("which wlsunset >/dev/null 2>&1") != 0) {
        if (verbose) fprintf(stderr, "wlsunset not installed. Gamma disabled.\n");
        return;
    }

    debug("Spawning wlsunset with gamma %.2f\n", config.gamma);
    
    // Set environment variables (proven working)
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (wayland_display) {
        setenv("WAYLAND_DISPLAY", wayland_display, 1);
        debug("WAYLAND_DISPLAY set to: %s\n", wayland_display);
    } else {
        debug("WAYLAND_DISPLAY not set in environment\n");
    }
    if (xdg_runtime) {
        setenv("XDG_RUNTIME_DIR", xdg_runtime, 1);
        debug("XDG_RUNTIME_DIR set to: %s\n", xdg_runtime);
    } else {
        debug("XDG_RUNTIME_DIR not set in environment\n");
    }

    wlsunset_pid = fork();
    if (wlsunset_pid == 0) {
        char gamma_str[16];
        snprintf(gamma_str, sizeof(gamma_str), "%.2f", config.gamma);
        
        if (!verbose) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd != -1) dup2(fd, STDERR_FILENO), close(fd);
        }
        
        execlp("wlsunset", "wlsunset", "-T", "6501", "-t", "6500", "-g", gamma_str, NULL);
        _exit(1);
    }
    if (wlsunset_pid > 0) {
        debug("wlsunset started with pid %d\n", wlsunset_pid);
        g_child_watch_add(wlsunset_pid, wlsunset_exited_cb, NULL);
    } else {
        debug("Failed to fork wlsunset\n");
    }
}

// --- Signal handlers for clean exit ---
static gboolean cleanup_and_exit(gpointer data) {
    int sig = GPOINTER_TO_INT(data);
    debug("Received signal %d, cleaning up...\n", sig);
    kill_wlsunset();
    kill_all_wlsunset();
    unlink("/tmp/crosshaird.pid");
    exit(0);
    return G_SOURCE_REMOVE;
}

// --- Flicker-free rendering ---
static void draw_cross_lines(cairo_t *cr, double cx, double cy, double s, double t, double g) {
    if (g < 0) g = 0;
    if (g > s) g = s;
    cairo_set_line_width(cr, t);
    cairo_move_to(cr, cx - s, cy);
    cairo_line_to(cr, cx - g, cy);
    cairo_move_to(cr, cx + g, cy);
    cairo_line_to(cr, cx + s, cy);
    cairo_move_to(cr, cx, cy - s);
    cairo_line_to(cr, cx, cy - g);
    cairo_move_to(cr, cx, cy + g);
    cairo_line_to(cr, cx, cy + s);
    cairo_stroke(cr);
}

void render_crosshair(int width, int height) {
    debug("render_crosshair: %dx%d\n", width, height);
    if (width <= 0 || height <= 0) {
        debug("  invalid size, skipping render\n");
        return;
    }

    if (crosshair_surface &&
        cairo_image_surface_get_width(crosshair_surface) == width &&
        cairo_image_surface_get_height(crosshair_surface) == height) {
        debug("  reusing existing surface\n");
    } else {
        if (crosshair_surface) {
            cairo_surface_destroy(crosshair_surface);
            crosshair_surface = NULL;
        }
        crosshair_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        debug("  created new surface\n");
    }

    cairo_t *cr = cairo_create(crosshair_surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    
    if (config.enabled) {
        // Apply the same offset_x/offset_y the GUI preview uses. These
        // were previously never read from the config at all here, so
        // dragging the Offset X/Y sliders moved the preview but had no
        // effect whatsoever on the real on-screen crosshair.
        double cx = width / 2.0 + config.offset_x;
        double cy = height / 2.0 + config.offset_y;
        double s = config.size;
        double t = config.thickness;
        debug("  drawing at (%.1f, %.1f), size=%.1f, thick=%.1f\n", cx, cy, s, t);
        
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_set_source_rgba(cr, config.red, config.green, config.blue, config.alpha);
        
        switch (config.style) {
            case 0: // Dot
                cairo_arc(cr, cx, cy, s, 0, 2 * M_PI);
                cairo_fill(cr);
                break;
            case 1: // Cross
                draw_cross_lines(cr, cx, cy, s, t, config.gap);
                break;
            case 2: // Circle
                cairo_set_line_width(cr, t);
                cairo_arc(cr, cx, cy, s, 0, 2 * M_PI);
                cairo_stroke(cr);
                break;
            case 3: // Dot + Cross
                cairo_arc(cr, cx, cy, s * 0.4, 0, 2 * M_PI);
                cairo_fill(cr);
                draw_cross_lines(cr, cx, cy, s, t, config.gap);
                break;
            default:
                debug("  unknown style %d\n", config.style);
                break;
        }
    } else {
        debug("  crosshair disabled, surface stays transparent\n");
    }
    cairo_destroy(cr);
    surface_width = width;
    surface_height = height;
    debug("render_crosshair done\n");
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    (void)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    debug("on_draw: allocation %dx%d\n", alloc.width, alloc.height);
    
    if (!crosshair_surface ||
        alloc.width != surface_width ||
        alloc.height != surface_height) {
        debug("  surface missing or resized, rendering...\n");
        render_crosshair(alloc.width, alloc.height);
    }
    
    if (crosshair_surface) {
        cairo_set_source_surface(cr, crosshair_surface, 0, 0);
        cairo_paint(cr);
        debug("  surface blitted\n");
    } else {
        debug("  no surface, clearing to transparent\n");
        cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cr, 0, 0, 0, 0);
        cairo_paint(cr);
    }
    return FALSE;
}

// --- Window creation ---
void create_overlay() {
    debug("create_overlay: starting\n");
    if (overlay_window) {
        gtk_widget_destroy(overlay_window);
        overlay_window = NULL;
        drawing_area = NULL;
        if (crosshair_surface) {
            cairo_surface_destroy(crosshair_surface);
            crosshair_surface = NULL;
            surface_width = 0;
            surface_height = 0;
        }
    }
    
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(win), 1, 1);
    
    GdkScreen *screen = gtk_window_get_screen(GTK_WINDOW(win));
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) {
        gtk_widget_set_visual(win, visual);
        debug("  rgba visual set\n");
    } else {
        debug("  no rgba visual available\n");
    }
    gtk_widget_set_app_paintable(win, TRUE);
    
    set_transparent_background(win);
    
    gtk_layer_init_for_window(GTK_WINDOW(win));
    gtk_layer_set_layer(GTK_WINDOW(win), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_LEFT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_RIGHT, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_anchor(GTK_WINDOW(win), GTK_LAYER_SHELL_EDGE_BOTTOM, TRUE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(win), -1);
    debug("  layer shell configured\n");
    
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_hexpand(da, TRUE);
    gtk_widget_set_vexpand(da, TRUE);
    set_transparent_background(da);
    drawing_area = da;
    g_signal_connect(da, "draw", G_CALLBACK(on_draw), NULL);
    gtk_container_add(GTK_CONTAINER(win), da);
    debug("  drawing area added\n");
    
    gtk_widget_show_all(win);
    debug("  window shown\n");
    
    cairo_region_t *empty = cairo_region_create();
    gdk_window_input_shape_combine_region(gtk_widget_get_window(win), empty, 0, 0);
    cairo_region_destroy(empty);
    debug("  input region set to empty\n");
    
    overlay_window = win;
    
    if (drawing_area) {
        gtk_widget_queue_draw(drawing_area);
        debug("  initial draw queued\n");
    }
}

// --- SIGHUP handler ---
static gboolean reload_config(gpointer data) {
    (void)data;
    debug("Received SIGHUP, reloading config...\n");
    load_config();
    spawn_gamma_control();

    if (drawing_area) {
        GtkAllocation alloc;
        gtk_widget_get_allocation(drawing_area, &alloc);
        if (alloc.width > 0 && alloc.height > 0) {
            render_crosshair(alloc.width, alloc.height);
            gtk_widget_queue_draw(drawing_area);
            debug("  redraw queued\n");
        }
    }
    return G_SOURCE_CONTINUE;
}

// --- PID file ---
void write_pid_file() {
    FILE *f = fopen("/tmp/crosshaird.pid", "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
        debug("PID written to /tmp/crosshaird.pid\n");
    } else {
        if (verbose) perror("Failed to write PID file");
        debug("Failed to write PID file\n");
    }
}

// --- Main ---
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
    }

    debug("crosshaird starting (pid %d)\n", getpid());
    kill_all_wlsunset(); // clean slate at startup
    load_config();
    // Force initial spawn
    prev_use_gamma = !config.use_gamma;
    prev_gamma = config.gamma + 0.01;
    spawn_gamma_control();
    g_unix_signal_add(SIGHUP, reload_config, NULL);
    g_unix_signal_add(SIGTERM, cleanup_and_exit, GINT_TO_POINTER(SIGTERM));
    g_unix_signal_add(SIGINT, cleanup_and_exit, GINT_TO_POINTER(SIGINT));
    write_pid_file();

    gtk_init(&argc, &argv);
    debug("GTK initialized\n");
    create_overlay();
    debug("Entering GTK main loop\n");
    gtk_main();
    return 0;
}