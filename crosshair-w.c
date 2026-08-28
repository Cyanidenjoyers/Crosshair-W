#include <gtk/gtk.h>
#include <json-c/json.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>

#define CONFIG_PATH "/.config/crosshair/config.json"

int verbose = 0;
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

typedef struct {
    double red, green, blue, alpha, size, thickness, gap;
} StylePreset;

typedef struct {
    GtkWidget *style_combo;
    GtkWidget *color_button;
    GtkWidget *alpha_scale;
    GtkWidget *size_scale;
    GtkWidget *thickness_scale;
    GtkWidget *thickness_label;
    GtkWidget *gap_scale;
    GtkWidget *gap_label;
    GtkWidget *enable_check;
    GtkWidget *preview_area;
    double red, green, blue, alpha, size, thickness, gap;
    int style, enabled;
    StylePreset style_presets[4]; // remembered settings per crosshair style

    GtkWidget *gamma_check;
    GtkWidget *gamma_spin;
    int use_gamma;
    double gamma;

    char preview_bg[32];
    char theme[32];
    GtkWidget *notebook;
    GtkWidget *window;
} Widgets;

// --- Preview drawing function ---
static void draw_cross_lines(cairo_t *cr, double cx, double cy, double s, double t, double g, double scale) {
    double ss = s * scale, ts = t * scale, gs = g * scale;
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

static gboolean draw_preview(GtkWidget *widget, cairo_t *cr, gpointer data) {
    Widgets *w = (Widgets*)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double cx = alloc.width / 2.0;
    double cy = alloc.height / 2.0;
    double s = w->size;
    double t = w->thickness;

    double radius = 8.0;
    double x = 0, y = 0, width = alloc.width, height = alloc.height;
    cairo_new_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3*M_PI/2);
    cairo_arc(cr, x + width - radius, y + radius, radius, 3*M_PI/2, 2*M_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI/2);
    cairo_arc(cr, x + radius, y + height - radius, radius, M_PI/2, M_PI);
    cairo_close_path(cr);
    cairo_clip(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    GdkRGBA bg_color;
    gdk_rgba_parse(&bg_color, w->preview_bg);
    cairo_set_source_rgba(cr, bg_color.red, bg_color.green, bg_color.blue, bg_color.alpha);
    cairo_paint(cr);

    // Always draw the crosshair – ignore "enable" checkbox
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, w->red, w->green, w->blue, w->alpha);

    double scale = 1.0;
    double max_dim = fmin(alloc.width, alloc.height) / 2.0 - 5;
    if (s > max_dim) scale = max_dim / s;

    switch (w->style) {
        case 0: // Dot
            cairo_arc(cr, cx, cy, s * scale, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case 1: // Cross
            draw_cross_lines(cr, cx, cy, s, t, w->gap, scale);
            break;
        case 2: // Circle
            cairo_set_line_width(cr, t * scale);
            cairo_arc(cr, cx, cy, s * scale, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case 3: // Dot + Cross
            cairo_arc(cr, cx, cy, s * 0.4 * scale, 0, 2 * M_PI);
            cairo_fill(cr);
            draw_cross_lines(cr, cx, cy, s, t, w->gap, scale);
            break;
        default:
            break;
    }
    return FALSE;
}

// --- Reload daemon ---
void reload_daemon() {
    debug("reload_daemon: trying to reload daemon\n");
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1) {
            fclose(f);
            debug("reload_daemon: sending SIGHUP to pid %d\n", pid);
            if (kill(pid, SIGHUP) == 0) {
                debug("reload_daemon: signal sent successfully\n");
                return;
            } else {
                if (verbose) perror("kill failed");
            }
        } else {
            fclose(f);
        }
    } else {
        debug("reload_daemon: no PID file, falling back to pkill\n");
    }
    int ret = system("pkill -SIGHUP crosshaird 2>/dev/null");
    if (ret == 0) {
        debug("reload_daemon: pkill succeeded\n");
    } else {
        debug("reload_daemon: pkill failed (exit %d)\n", ret);
    }
}

// --- Kill daemon and wlsunset on exit ---
void kill_daemon() {
    debug("kill_daemon: killing crosshaird and wlsunset\n");
    system("pkill -f wlsunset 2>/dev/null");
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1) {
            debug("kill_daemon: killing crosshaird (pid %d)\n", pid);
            kill(pid, SIGTERM);
        }
        fclose(f);
    }
}

// --- Apply theme ---
void apply_theme(Widgets *w) {
    GtkStyleContext *context = gtk_widget_get_style_context(w->window);
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = NULL;

    if (strcmp(w->theme, "blue") == 0) {
        css =
            "window, notebook { background-color: #161a24; } "
            "grid, box { background-color: transparent; } "
            "label { color: #d6e0f5; } "
            "checkbutton label { color: #d6e0f5; } "
            "frame { border: 1px solid #2a3a5c; border-radius: 10px; background-color: #1c2436; } "
            "scale trough { background-color: #0f1420; border-radius: 4px; min-height: 6px; } "
            "scale highlight { background-color: #4a90e2; border-radius: 4px; } "
            "scale slider { background-color: #ffffff; border-radius: 50%; min-width: 14px; min-height: 14px; margin: -5px; } "
            "combobox, combobox button, combobox entry, spinbutton, entry "
            "  { background-color: #1c2436; color: #d6e0f5; border-radius: 6px; border: 1px solid #2a3a5c; } "
            "colorbutton { border-radius: 6px; } "
            "button { background-color: #1c2436; color: #d6e0f5; border-radius: 8px; border: 1px solid #2a3a5c; padding: 6px 10px; } "
            "button:hover { background-color: #26314a; } "
            ".page-title { font-size: 15pt; font-weight: bold; color: #d6e0f5; }";
    } else {
        css =
            "window, notebook { background-color: #1c1d26; } "
            "grid, box { background-color: transparent; } "
            "label { color: #e4e4ec; } "
            "checkbutton label { color: #e4e4ec; } "
            "frame { border: 1px solid #33364a; border-radius: 10px; background-color: #23242f; } "
            "scale trough { background-color: #14151b; border-radius: 4px; min-height: 6px; } "
            "scale highlight { background-color: #7c5cff; border-radius: 4px; } "
            "scale slider { background-color: #ffffff; border-radius: 50%; min-width: 14px; min-height: 14px; margin: -5px; } "
            "combobox, combobox button, combobox entry, spinbutton, entry "
            "  { background-color: #2a2c3a; color: #e4e4ec; border-radius: 6px; border: 1px solid #3a3d52; } "
            "colorbutton { border-radius: 6px; } "
            "button { background-color: #2a2c3a; color: #e4e4ec; border-radius: 8px; border: 1px solid #3a3d52; padding: 6px 10px; } "
            "button:hover { background-color: #35374a; } "
            ".page-title { font-size: 15pt; font-weight: bold; color: #e4e4ec; }";
    }

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// --- Style-dependent widget state ---
static void update_style_dependent_widgets(Widgets *w) {
    int style = gtk_combo_box_get_active(GTK_COMBO_BOX(w->style_combo));
    gboolean is_cross = (style == 1 || style == 3); // Cross, Dot + Cross
    gboolean uses_thickness = (style != 0); // everything except Dot

    gtk_range_set_range(GTK_RANGE(w->size_scale), is_cross ? 2.5 : 1.0, 50);
    gtk_widget_set_sensitive(w->gap_scale, is_cross);
    gtk_widget_set_sensitive(w->gap_label, is_cross);
    gtk_widget_set_sensitive(w->thickness_scale, uses_thickness);
    gtk_widget_set_sensitive(w->thickness_label, uses_thickness);
}

// --- Save config ---
void save_config(Widgets *w) {
    debug("save_config: saving config\n");
    w->style = gtk_combo_box_get_active(GTK_COMBO_BOX(w->style_combo));
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(w->color_button), &rgba);
    w->red = rgba.red;
    w->green = rgba.green;
    w->blue = rgba.blue;
    w->alpha = gtk_range_get_value(GTK_RANGE(w->alpha_scale));
    w->size = gtk_range_get_value(GTK_RANGE(w->size_scale));
    w->thickness = gtk_range_get_value(GTK_RANGE(w->thickness_scale));
    w->gap = gtk_range_get_value(GTK_RANGE(w->gap_scale));
    w->enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->enable_check));
    w->use_gamma = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->gamma_check));
    w->gamma = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->gamma_spin));

    // Remember this style's look so switching styles and back restores it
    if (w->style >= 0 && w->style < 4) {
        StylePreset *p = &w->style_presets[w->style];
        p->red = w->red;
        p->green = w->green;
        p->blue = w->blue;
        p->alpha = w->alpha;
        p->size = w->size;
        p->thickness = w->thickness;
        p->gap = w->gap;
    }

    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/crosshair", home);
    mkdir(dir, 0755);

    json_object *root = json_object_new_object();
    json_object_object_add(root, "style", json_object_new_int(w->style));
    json_object_object_add(root, "red", json_object_new_double(w->red));
    json_object_object_add(root, "green", json_object_new_double(w->green));
    json_object_object_add(root, "blue", json_object_new_double(w->blue));
    json_object_object_add(root, "alpha", json_object_new_double(w->alpha));
    json_object_object_add(root, "size", json_object_new_double(w->size));
    json_object_object_add(root, "thickness", json_object_new_double(w->thickness));
    json_object_object_add(root, "gap", json_object_new_double(w->gap));
    json_object_object_add(root, "enabled", json_object_new_boolean(w->enabled));
    json_object_object_add(root, "use_gamma", json_object_new_boolean(w->use_gamma));
    json_object_object_add(root, "gamma", json_object_new_double(w->gamma));
    json_object_object_add(root, "preview_bg", json_object_new_string(w->preview_bg));
    json_object_object_add(root, "theme", json_object_new_string(w->theme));

    json_object *styles_arr = json_object_new_array();
    for (int i = 0; i < 4; i++) {
        StylePreset *p = &w->style_presets[i];
        json_object *s = json_object_new_object();
        json_object_object_add(s, "red", json_object_new_double(p->red));
        json_object_object_add(s, "green", json_object_new_double(p->green));
        json_object_object_add(s, "blue", json_object_new_double(p->blue));
        json_object_object_add(s, "alpha", json_object_new_double(p->alpha));
        json_object_object_add(s, "size", json_object_new_double(p->size));
        json_object_object_add(s, "thickness", json_object_new_double(p->thickness));
        json_object_object_add(s, "gap", json_object_new_double(p->gap));
        json_object_array_add(styles_arr, s);
    }
    json_object_object_add(root, "styles", styles_arr);

    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
        fclose(f);
        debug("save_config: config written to %s\n", path);
    } else {
        debug("save_config: failed to write config\n");
    }
    json_object_put(root);

    reload_daemon();
    gtk_widget_queue_draw(w->preview_area);
}

// --- Load config ---
void load_config_into_ui(Widgets *w) {
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);

    FILE *f = fopen(path, "r");
    if (!f) {
        strcpy(w->preview_bg, "#ffffff");
        strcpy(w->theme, "default");
        w->use_gamma = 0;
        w->gamma = 1.2;
        save_config(w);
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
        strcpy(w->preview_bg, "#ffffff");
        strcpy(w->theme, "default");
        w->use_gamma = 0;
        w->gamma = 1.2;
        save_config(w);
        return;
    }

    json_object *tmp;
    if (json_object_object_get_ex(root, "style", &tmp))
        gtk_combo_box_set_active(GTK_COMBO_BOX(w->style_combo), json_object_get_int(tmp));
    {
        GdkRGBA rgba = { 0.0, 0.0, 0.0, 1.0 };
        if (json_object_object_get_ex(root, "red", &tmp))
            rgba.red = json_object_get_double(tmp);
        if (json_object_object_get_ex(root, "green", &tmp))
            rgba.green = json_object_get_double(tmp);
        if (json_object_object_get_ex(root, "blue", &tmp))
            rgba.blue = json_object_get_double(tmp);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w->color_button), &rgba);
    }
    if (json_object_object_get_ex(root, "alpha", &tmp))
        gtk_range_set_value(GTK_RANGE(w->alpha_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "size", &tmp))
        gtk_range_set_value(GTK_RANGE(w->size_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "thickness", &tmp))
        gtk_range_set_value(GTK_RANGE(w->thickness_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "gap", &tmp))
        gtk_range_set_value(GTK_RANGE(w->gap_scale), json_object_get_double(tmp));

    json_object *styles_arr;
    if (json_object_object_get_ex(root, "styles", &styles_arr) &&
        json_object_get_type(styles_arr) == json_type_array) {
        int n = json_object_array_length(styles_arr);
        for (int i = 0; i < n && i < 4; i++) {
            json_object *sp = json_object_array_get_idx(styles_arr, i);
            json_object *f;
            StylePreset *p = &w->style_presets[i];
            if (json_object_object_get_ex(sp, "red", &f)) p->red = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "green", &f)) p->green = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "blue", &f)) p->blue = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "alpha", &f)) p->alpha = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "size", &f)) p->size = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "thickness", &f)) p->thickness = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "gap", &f)) p->gap = json_object_get_double(f);
        }
    }

    if (json_object_object_get_ex(root, "enabled", &tmp))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->enable_check), json_object_get_boolean(tmp));
    if (json_object_object_get_ex(root, "use_gamma", &tmp))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->gamma_check), json_object_get_boolean(tmp));
    if (json_object_object_get_ex(root, "gamma", &tmp))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->gamma_spin), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "preview_bg", &tmp)) {
        const char *bg = json_object_get_string(tmp);
        strncpy(w->preview_bg, bg, sizeof(w->preview_bg)-1);
        w->preview_bg[sizeof(w->preview_bg)-1] = '\0';
    } else {
        strcpy(w->preview_bg, "#ffffff");
    }
    if (json_object_object_get_ex(root, "theme", &tmp)) {
        const char *theme = json_object_get_string(tmp);
        strncpy(w->theme, theme, sizeof(w->theme)-1);
        w->theme[sizeof(w->theme)-1] = '\0';
    } else {
        strcpy(w->theme, "default");
    }

    json_object_put(root);
    apply_theme(w);
    update_style_dependent_widgets(w);
    gtk_widget_queue_draw(w->preview_area);
}

// --- Callbacks ---
void on_crosshair_changed(GtkWidget *widget, gpointer data) {
    (void)widget;
    Widgets *w = (Widgets*)data;
    update_style_dependent_widgets(w);
    save_config(w);
}

void on_style_changed(GtkWidget *widget, gpointer data) {
    (void)widget;
    Widgets *w = (Widgets*)data;
    int new_style = gtk_combo_box_get_active(GTK_COMBO_BOX(w->style_combo));
    update_style_dependent_widgets(w);

    if (new_style >= 0 && new_style < 4) {
        StylePreset *p = &w->style_presets[new_style];

        g_signal_handlers_block_by_func(w->color_button, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_block_by_func(w->alpha_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_block_by_func(w->size_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_block_by_func(w->thickness_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_block_by_func(w->gap_scale, G_CALLBACK(on_crosshair_changed), w);

        GdkRGBA rgba = { p->red, p->green, p->blue, 1.0 };
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w->color_button), &rgba);
        gtk_range_set_value(GTK_RANGE(w->alpha_scale), p->alpha);
        gtk_range_set_value(GTK_RANGE(w->size_scale), p->size);
        gtk_range_set_value(GTK_RANGE(w->thickness_scale), p->thickness);
        gtk_range_set_value(GTK_RANGE(w->gap_scale), p->gap);

        g_signal_handlers_unblock_by_func(w->color_button, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->alpha_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->size_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->thickness_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->gap_scale, G_CALLBACK(on_crosshair_changed), w);
    }

    save_config(w);
}

void on_preview_bg_clicked(GtkButton *button, gpointer data) {
    Widgets *w = (Widgets*)data;
    const char *color = (const char*)g_object_get_data(G_OBJECT(button), "color");
    if (color) {
        strncpy(w->preview_bg, color, sizeof(w->preview_bg)-1);
        save_config(w);
        gtk_widget_queue_draw(w->preview_area);
    }
}

void on_theme_blue_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    strcpy(w->theme, "blue");
    apply_theme(w);
    save_config(w);
}

void on_theme_default_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    strcpy(w->theme, "default");
    apply_theme(w);
    save_config(w);
}

void on_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    kill_daemon();
    gtk_main_quit();
}

void on_settings_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), 1);
}

void on_back_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), 0);
}

// --- Daemon auto-start ---
void ensure_daemon_running() {
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1 && kill(pid, 0) == 0) {
            fclose(f);
            debug("Daemon already running (pid %d)\n", pid);
            return;
        }
        fclose(f);
    }
    debug("Daemon not running, starting crosshaird...\n");
    const char *args = verbose ? "crosshaird -v &" : "crosshaird &";
    int ret = system(args);
    if (ret == -1) {
        debug("Failed to start daemon\n");
    } else {
        debug("Daemon started (ret=%d)\n", ret);
        sleep(1);
    }
}

// --- SIGINT handler for GUI ---
static void sigint_handler(int sig) {
    (void)sig;
    debug("Received SIGINT, cleaning up daemon and exiting...\n");
    kill_daemon();
    gtk_main_quit();
}

// --- Main ---
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
    }

    debug("crosshair-w starting\n");
    gtk_init(&argc, &argv);
    ensure_daemon_running();

    signal(SIGINT, sigint_handler);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Crosshair Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 700);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_container_add(GTK_CONTAINER(window), notebook);

    Widgets widgets = {0};
    widgets.notebook = notebook;
    widgets.window = window;
    for (int i = 0; i < 4; i++) {
        widgets.style_presets[i].red = 0.0;
        widgets.style_presets[i].green = 1.0;
        widgets.style_presets[i].blue = 0.0;
        widgets.style_presets[i].alpha = 1.0;
        widgets.style_presets[i].size = 3.0;
        widgets.style_presets[i].thickness = 1.0;
        widgets.style_presets[i].gap = 0.0;
    }

    // ---------- Crosshair tab ----------
    GtkWidget *crosshair_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(crosshair_page), 18);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), crosshair_page, gtk_label_new("Crosshair"));

    GtkWidget *page_title = gtk_label_new("Crosshair");
    gtk_widget_set_halign(page_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(page_title), "page-title");
    gtk_box_pack_start(GTK_BOX(crosshair_page), page_title, FALSE, FALSE, 0);

    // Preview card
    GtkWidget *preview_frame = gtk_frame_new("Preview");
    gtk_box_pack_start(GTK_BOX(crosshair_page), preview_frame, FALSE, FALSE, 0);
    GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(preview_box, GTK_ALIGN_CENTER);
    gtk_container_set_border_width(GTK_CONTAINER(preview_box), 14);
    gtk_container_add(GTK_CONTAINER(preview_frame), preview_box);
    widgets.preview_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(widgets.preview_area, 220, 160);
    gtk_widget_set_hexpand(widgets.preview_area, FALSE);
    gtk_widget_set_vexpand(widgets.preview_area, FALSE);
    g_signal_connect(widgets.preview_area, "draw", G_CALLBACK(draw_preview), &widgets);
    gtk_box_pack_start(GTK_BOX(preview_box), widgets.preview_area, FALSE, FALSE, 0);

    // Appearance card: style, color, alpha
    GtkWidget *appearance_frame = gtk_frame_new("Appearance");
    gtk_box_pack_start(GTK_BOX(crosshair_page), appearance_frame, FALSE, FALSE, 0);
    GtkWidget *appearance_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(appearance_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(appearance_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(appearance_grid), 14);
    gtk_container_add(GTK_CONTAINER(appearance_frame), appearance_grid);

    int arow = 0;
    GtkWidget *style_label = gtk_label_new("Style:");
    gtk_widget_set_halign(style_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(appearance_grid), style_label, 0, arow, 1, 1);
    widgets.style_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Dot");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Cross");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Circle");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Dot + Cross");
    gtk_widget_set_hexpand(widgets.style_combo, TRUE);
    gtk_grid_attach(GTK_GRID(appearance_grid), widgets.style_combo, 1, arow++, 1, 1);
    g_signal_connect(widgets.style_combo, "changed", G_CALLBACK(on_style_changed), &widgets);

    GtkWidget *color_label = gtk_label_new("Color:");
    gtk_widget_set_halign(color_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(appearance_grid), color_label, 0, arow, 1, 1);
    widgets.color_button = gtk_color_button_new();
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(widgets.color_button), FALSE);
    // Make the color picker expand to fill the row
    gtk_widget_set_halign(widgets.color_button, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widgets.color_button, TRUE);
    gtk_grid_attach(GTK_GRID(appearance_grid), widgets.color_button, 1, arow++, 1, 1);
    g_signal_connect(widgets.color_button, "color-set", G_CALLBACK(on_crosshair_changed), &widgets);

    GtkWidget *alpha_label = gtk_label_new("Opacity:");
    gtk_widget_set_halign(alpha_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(appearance_grid), alpha_label, 0, arow, 1, 1);
    widgets.alpha_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_widget_set_hexpand(widgets.alpha_scale, TRUE);
    gtk_grid_attach(GTK_GRID(appearance_grid), widgets.alpha_scale, 1, arow++, 1, 1);
    g_signal_connect(widgets.alpha_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Size, Gap & Thickness card
    GtkWidget *size_frame = gtk_frame_new("Size, Gap & Thickness");
    gtk_box_pack_start(GTK_BOX(crosshair_page), size_frame, FALSE, FALSE, 0);
    GtkWidget *size_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(size_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(size_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(size_grid), 14);
    gtk_container_add(GTK_CONTAINER(size_frame), size_grid);

    int srow2 = 0;
    GtkWidget *size_label = gtk_label_new("Size (px):");
    gtk_widget_set_halign(size_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), size_label, 0, srow2, 1, 1);
    widgets.size_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 50, 1);
    gtk_widget_set_hexpand(widgets.size_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.size_scale, 1, srow2++, 1, 1);
    g_signal_connect(widgets.size_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    widgets.gap_label = gtk_label_new("Gap (px):");
    gtk_widget_set_halign(widgets.gap_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.gap_label, 0, srow2, 1, 1);
    widgets.gap_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 30, 1);
    gtk_widget_set_hexpand(widgets.gap_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.gap_scale, 1, srow2++, 1, 1);
    g_signal_connect(widgets.gap_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    widgets.thickness_label = gtk_label_new("Thickness (px):");
    gtk_widget_set_halign(widgets.thickness_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.thickness_label, 0, srow2, 1, 1);
    widgets.thickness_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 10, 1);
    gtk_widget_set_hexpand(widgets.thickness_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.thickness_scale, 1, srow2++, 1, 1);
    g_signal_connect(widgets.thickness_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Enable Crosshair checkbox
    widgets.enable_check = gtk_check_button_new_with_label("Enable Crosshair");
    gtk_box_pack_start(GTK_BOX(crosshair_page), widgets.enable_check, FALSE, FALSE, 0);
    g_signal_connect(widgets.enable_check, "toggled", G_CALLBACK(on_crosshair_changed), &widgets);

    GtkWidget *settings_button = gtk_button_new_with_label("⚙ Misc");
    gtk_box_pack_start(GTK_BOX(crosshair_page), settings_button, FALSE, FALSE, 0);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_clicked), &widgets);

    // ---------- Misc tab ----------
    GtkWidget *settings_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(settings_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(settings_grid), 20);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), settings_grid, gtk_label_new("Misc"));

    int srow = 0;
    GtkWidget *back_button = gtk_button_new_with_label("← Back");
    gtk_widget_set_halign(back_button, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(settings_grid), back_button, 0, srow, 2, 1);
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), &widgets);
    srow++;

    // Preview Background
    GtkWidget *bg_frame = gtk_frame_new("Preview Background");
    gtk_grid_attach(GTK_GRID(settings_grid), bg_frame, 0, srow, 2, 1);
    GtkWidget *bg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(bg_frame), bg_box);
    gtk_container_set_border_width(GTK_CONTAINER(bg_box), 10);
    // Black button (re-added)
    GtkWidget *black_btn = gtk_button_new_with_label("Black");
    g_object_set_data(G_OBJECT(black_btn), "color", "#000000");
    g_signal_connect(black_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), black_btn, FALSE, FALSE, 0);
    // White button
    GtkWidget *white_btn = gtk_button_new_with_label("White");
    g_object_set_data(G_OBJECT(white_btn), "color", "#ffffff");
    g_signal_connect(white_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), white_btn, FALSE, FALSE, 0);
    // Blue button
    GtkWidget *blue_btn = gtk_button_new_with_label("Blue");
    g_object_set_data(G_OBJECT(blue_btn), "color", "#0000ff");
    g_signal_connect(blue_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), blue_btn, FALSE, FALSE, 0);
    // Red button
    GtkWidget *red_btn = gtk_button_new_with_label("Red");
    g_object_set_data(G_OBJECT(red_btn), "color", "#ff0000");
    g_signal_connect(red_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), red_btn, FALSE, FALSE, 0);
    srow++;

    // Themes
    GtkWidget *theme_frame = gtk_frame_new("Themes");
    gtk_grid_attach(GTK_GRID(settings_grid), theme_frame, 0, srow, 2, 1);
    GtkWidget *theme_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(theme_frame), theme_box);
    gtk_container_set_border_width(GTK_CONTAINER(theme_box), 10);
    GtkWidget *theme_blue_btn = gtk_button_new_with_label("Blue Theme");
    g_signal_connect(theme_blue_btn, "clicked", G_CALLBACK(on_theme_blue_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_blue_btn, FALSE, FALSE, 0);
    GtkWidget *theme_default_btn = gtk_button_new_with_label("Default Theme");
    g_signal_connect(theme_default_btn, "clicked", G_CALLBACK(on_theme_default_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_default_btn, FALSE, FALSE, 0);
    srow++;

    // Gamma Control
    GtkWidget *gamma_frame = gtk_frame_new("Gamma Control (wlsunset)");
    gtk_grid_attach(GTK_GRID(settings_grid), gamma_frame, 0, srow, 2, 1);
    GtkWidget *gamma_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(gamma_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(gamma_grid), 8);
    gtk_container_add(GTK_CONTAINER(gamma_frame), gamma_grid);
    gtk_container_set_border_width(GTK_CONTAINER(gamma_grid), 10);

    widgets.gamma_check = gtk_check_button_new_with_label("Enable Gamma");
    gtk_grid_attach(GTK_GRID(gamma_grid), widgets.gamma_check, 0, 0, 2, 1);
    g_signal_connect(widgets.gamma_check, "toggled", G_CALLBACK(on_crosshair_changed), &widgets);

    GtkWidget *gamma_val_label = gtk_label_new("Gamma value:");
    gtk_widget_set_halign(gamma_val_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(gamma_grid), gamma_val_label, 0, 1, 1, 1);
    widgets.gamma_spin = gtk_spin_button_new_with_range(0.5, 2.0, 0.01);
    gtk_widget_set_hexpand(widgets.gamma_spin, TRUE);
    gtk_grid_attach(GTK_GRID(gamma_grid), widgets.gamma_spin, 1, 1, 1, 1);
    g_signal_connect(widgets.gamma_spin, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);
    srow++;

    // Load config
    load_config_into_ui(&widgets);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}#include <gtk/gtk.h>
#include <json-c/json.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>

#define CONFIG_PATH "/.config/crosshair/config.json"

int verbose = 0;
#define debug(...) do { if (verbose) fprintf(stderr, __VA_ARGS__); } while(0)

typedef struct {
    GtkWidget *style_combo;
    GtkWidget *red_scale;
    GtkWidget *green_scale;
    GtkWidget *blue_scale;
    GtkWidget *alpha_scale;
    GtkWidget *size_scale;
    GtkWidget *thickness_scale;
    GtkWidget *enable_check;
    GtkWidget *preview_area;
    double red, green, blue, alpha, size, thickness;
    int style, enabled;

    GtkWidget *gamma_check;
    GtkWidget *gamma_spin;
    int use_gamma;
    double gamma;

    char preview_bg[32];
    char theme[32];
    GtkWidget *notebook;
    GtkWidget *window;
} Widgets;

// --- Preview drawing function (always draws the crosshair) ---
static gboolean draw_preview(GtkWidget *widget, cairo_t *cr, gpointer data) {
    Widgets *w = (Widgets*)data;
    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double cx = alloc.width / 2.0;
    double cy = alloc.height / 2.0;
    double s = w->size;
    double t = w->thickness;

    double radius = 8.0;
    double x = 0, y = 0, width = alloc.width, height = alloc.height;
    cairo_new_path(cr);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3*M_PI/2);
    cairo_arc(cr, x + width - radius, y + radius, radius, 3*M_PI/2, 2*M_PI);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI/2);
    cairo_arc(cr, x + radius, y + height - radius, radius, M_PI/2, M_PI);
    cairo_close_path(cr);
    cairo_clip(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    GdkRGBA bg_color;
    gdk_rgba_parse(&bg_color, w->preview_bg);
    cairo_set_source_rgba(cr, bg_color.red, bg_color.green, bg_color.blue, bg_color.alpha);
    cairo_paint(cr);

    // Always draw the crosshair – ignore "enabled" checkbox
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, w->red, w->green, w->blue, w->alpha);

    double scale = 1.0;
    double max_dim = fmin(alloc.width, alloc.height) / 2.0 - 5;
    if (s > max_dim) scale = max_dim / s;

    switch (w->style) {
        case 0: // Dot
            cairo_arc(cr, cx, cy, s * scale, 0, 2 * M_PI);
            cairo_fill(cr);
            break;
        case 1: // Cross
            cairo_set_line_width(cr, t * scale);
            cairo_move_to(cr, cx - s*scale, cy);
            cairo_line_to(cr, cx + s*scale, cy);
            cairo_move_to(cr, cx, cy - s*scale);
            cairo_line_to(cr, cx, cy + s*scale);
            cairo_stroke(cr);
            break;
        case 2: // Circle
            cairo_set_line_width(cr, t * scale);
            cairo_arc(cr, cx, cy, s * scale, 0, 2 * M_PI);
            cairo_stroke(cr);
            break;
        case 3: // Dot + Cross
            cairo_arc(cr, cx, cy, s * 0.4 * scale, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_set_line_width(cr, t * scale);
            cairo_move_to(cr, cx - s*scale, cy);
            cairo_line_to(cr, cx + s*scale, cy);
            cairo_move_to(cr, cx, cy - s*scale);
            cairo_line_to(cr, cx, cy + s*scale);
            cairo_stroke(cr);
            break;
        default:
            break;
    }
    return FALSE;
}

// --- Reload daemon ---
void reload_daemon() {
    debug("reload_daemon: trying to reload daemon\n");
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1) {
            fclose(f);
            debug("reload_daemon: sending SIGHUP to pid %d\n", pid);
            if (kill(pid, SIGHUP) == 0) {
                debug("reload_daemon: signal sent successfully\n");
                return;
            } else {
                if (verbose) perror("kill failed");
            }
        } else {
            fclose(f);
        }
    } else {
        debug("reload_daemon: no PID file, falling back to pkill\n");
    }
    int ret = system("pkill -SIGHUP crosshaird 2>/dev/null");
    if (ret == 0) {
        debug("reload_daemon: pkill succeeded\n");
    } else {
        debug("reload_daemon: pkill failed (exit %d)\n", ret);
    }
}

// --- Kill daemon and wlsunset on exit ---
void kill_daemon() {
    debug("kill_daemon: killing crosshaird and wlsunset\n");
    system("pkill -f wlsunset 2>/dev/null");
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1) {
            debug("kill_daemon: killing crosshaird (pid %d)\n", pid);
            kill(pid, SIGTERM);
        }
        fclose(f);
    }
}

// --- Apply theme ---
void apply_theme(Widgets *w) {
    GtkStyleContext *context = gtk_widget_get_style_context(w->window);
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = NULL;

    if (strcmp(w->theme, "blue") == 0) {
        css =
            "window { background-color: #1a2a4a; } "
            "grid { background-color: #1a2a4a; } "
            "label { color: #c0d0f0; } "
            "checkbutton label { color: #c0d0f0; } "
            "frame { border-color: #3a5a8a; } "
            "button { background-color: #2a4a6a; color: #ffffff; border-radius: 4px; } "
            "button:hover { background-color: #3a6a8a; } "
            "scale slider { background-color: #4a7a9a; } "
            "scale trough { background-color: #1a2a3a; } "
            "combobox { background-color: #2a4a6a; color: #ffffff; } "
            "combobox button { background-color: #2a4a6a; } "
            "combobox entry { background-color: #1a2a3a; color: #ffffff; } "
            "headerbar { background-color: #2a4a6a; color: #ffffff; } "
            "headerbar button { background-color: transparent; color: #ffffff; } "
            "headerbar button:hover { background-color: #3a6a8a; } ";
    } else {
        css = "";
    }

    if (css && strlen(css) > 0) {
        gtk_css_provider_load_from_data(provider, css, -1, NULL);
        gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    } else {
        gtk_css_provider_load_from_data(provider, "", -1, NULL);
        gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

// --- Save config ---
void save_config(Widgets *w) {
    debug("save_config: saving config\n");
    w->style = gtk_combo_box_get_active(GTK_COMBO_BOX(w->style_combo));
    w->red = gtk_range_get_value(GTK_RANGE(w->red_scale));
    w->green = gtk_range_get_value(GTK_RANGE(w->green_scale));
    w->blue = gtk_range_get_value(GTK_RANGE(w->blue_scale));
    w->alpha = gtk_range_get_value(GTK_RANGE(w->alpha_scale));
    w->size = gtk_range_get_value(GTK_RANGE(w->size_scale));
    w->thickness = gtk_range_get_value(GTK_RANGE(w->thickness_scale));
    w->enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->enable_check));
    w->use_gamma = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->gamma_check));
    w->gamma = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->gamma_spin));

    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/crosshair", home);
    mkdir(dir, 0755);

    json_object *root = json_object_new_object();
    json_object_object_add(root, "style", json_object_new_int(w->style));
    json_object_object_add(root, "red", json_object_new_double(w->red));
    json_object_object_add(root, "green", json_object_new_double(w->green));
    json_object_object_add(root, "blue", json_object_new_double(w->blue));
    json_object_object_add(root, "alpha", json_object_new_double(w->alpha));
    json_object_object_add(root, "size", json_object_new_double(w->size));
    json_object_object_add(root, "thickness", json_object_new_double(w->thickness));
    json_object_object_add(root, "enabled", json_object_new_boolean(w->enabled));
    json_object_object_add(root, "use_gamma", json_object_new_boolean(w->use_gamma));
    json_object_object_add(root, "gamma", json_object_new_double(w->gamma));
    json_object_object_add(root, "preview_bg", json_object_new_string(w->preview_bg));
    json_object_object_add(root, "theme", json_object_new_string(w->theme));

    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
        fclose(f);
        debug("save_config: config written to %s\n", path);
    } else {
        debug("save_config: failed to write config\n");
    }
    json_object_put(root);

    reload_daemon();
    gtk_widget_queue_draw(w->preview_area);
}

// --- Load config ---
void load_config_into_ui(Widgets *w) {
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);

    FILE *f = fopen(path, "r");
    if (!f) {
        strcpy(w->preview_bg, "#ffffff");
        strcpy(w->theme, "default");
        w->use_gamma = 0;
        w->gamma = 1.2;
        save_config(w);
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
        strcpy(w->preview_bg, "#ffffff");
        strcpy(w->theme, "default");
        w->use_gamma = 0;
        w->gamma = 1.2;
        save_config(w);
        return;
    }

    json_object *tmp;
    if (json_object_object_get_ex(root, "style", &tmp))
        gtk_combo_box_set_active(GTK_COMBO_BOX(w->style_combo), json_object_get_int(tmp));
    if (json_object_object_get_ex(root, "red", &tmp))
        gtk_range_set_value(GTK_RANGE(w->red_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "green", &tmp))
        gtk_range_set_value(GTK_RANGE(w->green_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "blue", &tmp))
        gtk_range_set_value(GTK_RANGE(w->blue_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "alpha", &tmp))
        gtk_range_set_value(GTK_RANGE(w->alpha_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "size", &tmp))
        gtk_range_set_value(GTK_RANGE(w->size_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "thickness", &tmp))
        gtk_range_set_value(GTK_RANGE(w->thickness_scale), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "enabled", &tmp))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->enable_check), json_object_get_boolean(tmp));
    if (json_object_object_get_ex(root, "use_gamma", &tmp))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->gamma_check), json_object_get_boolean(tmp));
    if (json_object_object_get_ex(root, "gamma", &tmp))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->gamma_spin), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "preview_bg", &tmp)) {
        const char *bg = json_object_get_string(tmp);
        strncpy(w->preview_bg, bg, sizeof(w->preview_bg)-1);
        w->preview_bg[sizeof(w->preview_bg)-1] = '\0';
    } else {
        strcpy(w->preview_bg, "#ffffff");
    }
    if (json_object_object_get_ex(root, "theme", &tmp)) {
        const char *theme = json_object_get_string(tmp);
        strncpy(w->theme, theme, sizeof(w->theme)-1);
        w->theme[sizeof(w->theme)-1] = '\0';
    } else {
        strcpy(w->theme, "default");
    }

    json_object_put(root);
    apply_theme(w);
    gtk_widget_queue_draw(w->preview_area);
}

// --- Callbacks ---
void on_crosshair_changed(GtkWidget *widget, gpointer data) {
    (void)widget;
    save_config((Widgets*)data);
}

void on_preview_bg_clicked(GtkButton *button, gpointer data) {
    Widgets *w = (Widgets*)data;
    const char *color = (const char*)g_object_get_data(G_OBJECT(button), "color");
    if (color) {
        strncpy(w->preview_bg, color, sizeof(w->preview_bg)-1);
        save_config(w);
        gtk_widget_queue_draw(w->preview_area);
    }
}

void on_theme_blue_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    strcpy(w->theme, "blue");
    apply_theme(w);
    save_config(w);
}

void on_theme_default_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    strcpy(w->theme, "default");
    apply_theme(w);
    save_config(w);
}

void on_exit_clicked(GtkButton *button, gpointer data) {
    (void)button;
    (void)data;
    kill_daemon();
    gtk_main_quit();
}

void on_destroy(GtkWidget *widget, gpointer data) {
    (void)widget;
    kill_daemon();
    gtk_main_quit();
}

void on_settings_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), 1);
}

void on_back_clicked(GtkButton *button, gpointer data) {
    (void)button;
    Widgets *w = (Widgets*)data;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(w->notebook), 0);
}

// --- Daemon auto-start ---
void ensure_daemon_running() {
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1 && kill(pid, 0) == 0) {
            fclose(f);
            debug("Daemon already running (pid %d)\n", pid);
            return;
        }
        fclose(f);
    }
    debug("Daemon not running, starting crosshaird...\n");
    const char *args = verbose ? "crosshaird -v &" : "crosshaird &";
    int ret = system(args);
    if (ret == -1) {
        debug("Failed to start daemon\n");
    } else {
        debug("Daemon started (ret=%d)\n", ret);
        sleep(1);
    }
}

// --- SIGINT handler for GUI ---
static void sigint_handler(int sig) {
    (void)sig;
    debug("Received SIGINT, cleaning up daemon and exiting...\n");
    kill_daemon();
    gtk_main_quit();
}

// --- Main ---
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
    }

    debug("crosshair-w starting\n");
    gtk_init(&argc, &argv);
    ensure_daemon_running();

    signal(SIGINT, sigint_handler);

    // Create the main window
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Crosshair Overlay Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 550, 750);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    // --- Create a header bar (top bar) ---
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Crosshair Overlay Settings");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);

    // Optionally add some buttons to the header (like a menu) – we'll keep it simple.
    // You could add a "Settings" gear button here if you like, but we already have a "Settings" button in the tab.

    // Set the header bar as the title bar of the window
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    // Create the notebook (tabs) – this will be the main content
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_container_add(GTK_CONTAINER(window), notebook);

    Widgets widgets = {0};
    widgets.notebook = notebook;
    widgets.window = window;

    // ---------- Crosshair tab ----------
    GtkWidget *crosshair_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(crosshair_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(crosshair_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(crosshair_grid), 20);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), crosshair_grid, gtk_label_new("Crosshair"));

    int row = 0;
    // Style
    GtkWidget *style_label = gtk_label_new("Style:");
    gtk_widget_set_halign(style_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(crosshair_grid), style_label, 0, row, 1, 1);
    widgets.style_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Dot");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Cross");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Circle");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widgets.style_combo), "Dot + Cross");
    gtk_widget_set_hexpand(widgets.style_combo, TRUE);
    gtk_grid_attach(GTK_GRID(crosshair_grid), widgets.style_combo, 1, row++, 1, 1);
    g_signal_connect(widgets.style_combo, "changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Preview area (now expands)
    GtkWidget *preview_label = gtk_label_new("Preview:");
    gtk_widget_set_halign(preview_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(crosshair_grid), preview_label, 0, row, 1, 1);
    widgets.preview_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(widgets.preview_area, TRUE);
    gtk_widget_set_vexpand(widgets.preview_area, TRUE);
    gtk_widget_set_size_request(widgets.preview_area, 220, 160);
    g_signal_connect(widgets.preview_area, "draw", G_CALLBACK(draw_preview), &widgets);
    GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(preview_box, TRUE);
    gtk_widget_set_vexpand(preview_box, TRUE);
    gtk_box_pack_start(GTK_BOX(preview_box), widgets.preview_area, TRUE, TRUE, 0);
    gtk_grid_attach(GTK_GRID(crosshair_grid), preview_box, 1, row, 1, 1);
    gtk_grid_set_row_vexpand(GTK_GRID(crosshair_grid), row, TRUE);
    row++;

    // Color
    GtkWidget *color_label = gtk_label_new("Color:");
    gtk_widget_set_halign(color_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(crosshair_grid), color_label, 0, row, 1, 1);
    GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_grid_attach(GTK_GRID(crosshair_grid), color_box, 1, row++, 1, 1);

    // R
    GtkWidget *red_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *red_label = gtk_label_new("R:");
    gtk_widget_set_halign(red_label, GTK_ALIGN_START);
    gtk_widget_set_size_request(red_label, 20, -1);
    gtk_box_pack_start(GTK_BOX(red_row), red_label, FALSE, FALSE, 0);
    widgets.red_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_widget_set_hexpand(widgets.red_scale, TRUE);
    gtk_box_pack_start(GTK_BOX(red_row), widgets.red_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(color_box), red_row, FALSE, FALSE, 0);
    g_signal_connect(widgets.red_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);
    // G
    GtkWidget *green_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *green_label = gtk_label_new("G:");
    gtk_widget_set_halign(green_label, GTK_ALIGN_START);
    gtk_widget_set_size_request(green_label, 20, -1);
    gtk_box_pack_start(GTK_BOX(green_row), green_label, FALSE, FALSE, 0);
    widgets.green_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_widget_set_hexpand(widgets.green_scale, TRUE);
    gtk_box_pack_start(GTK_BOX(green_row), widgets.green_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(color_box), green_row, FALSE, FALSE, 0);
    g_signal_connect(widgets.green_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);
    // B
    GtkWidget *blue_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *blue_label = gtk_label_new("B:");
    gtk_widget_set_halign(blue_label, GTK_ALIGN_START);
    gtk_widget_set_size_request(blue_label, 20, -1);
    gtk_box_pack_start(GTK_BOX(blue_row), blue_label, FALSE, FALSE, 0);
    widgets.blue_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_widget_set_hexpand(widgets.blue_scale, TRUE);
    gtk_box_pack_start(GTK_BOX(blue_row), widgets.blue_scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(color_box), blue_row, FALSE, FALSE, 0);
    g_signal_connect(widgets.blue_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Alpha
    GtkWidget *alpha_label = gtk_label_new("Alpha:");
    gtk_widget_set_halign(alpha_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(crosshair_grid), alpha_label, 0, row, 1, 1);
    widgets.alpha_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_widget_set_hexpand(widgets.alpha_scale, TRUE);
    gtk_grid_attach(GTK_GRID(crosshair_grid), widgets.alpha_scale, 1, row++, 1, 1);
    g_signal_connect(widgets.alpha_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Size
    GtkWidget *size_label = gtk_label_new("Size:");
    gtk_widget_set_halign(size_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(crosshair_grid), size_label, 0, row, 1, 1);
    widgets.size_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 50, 0.5);
    gtk_widget_set_hexpand(widgets.size_scale, TRUE);
    gtk_grid_attach(GTK_GRID(crosshair_grid), widgets.size_scale, 1, row++, 1, 1);
    g_signal_connect(widgets.size_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Thickness
    GtkWidget *thick_label = gtk_label_new("Thickness:");
    gtk_widget_set_halign(thick_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(crosshair_grid), thick_label, 0, row, 1, 1);
    widgets.thickness_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 10, 0.5);
    gtk_widget_set_hexpand(widgets.thickness_scale, TRUE);
    gtk_grid_attach(GTK_GRID(crosshair_grid), widgets.thickness_scale, 1, row++, 1, 1);
    g_signal_connect(widgets.thickness_scale, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);

    // Enabled
    widgets.enable_check = gtk_check_button_new_with_label("Enabled");
    gtk_grid_attach(GTK_GRID(crosshair_grid), widgets.enable_check, 0, row, 2, 1);
    g_signal_connect(widgets.enable_check, "toggled", G_CALLBACK(on_crosshair_changed), &widgets);
    row++;

    GtkWidget *settings_button = gtk_button_new_with_label("⚙ Settings");
    gtk_grid_attach(GTK_GRID(crosshair_grid), settings_button, 0, row, 2, 1);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_clicked), &widgets);

    // ---------- Settings tab ----------
    GtkWidget *settings_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(settings_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(settings_grid), 20);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), settings_grid, gtk_label_new("Settings"));

    int srow = 0;
    GtkWidget *back_button = gtk_button_new_with_label("← Back");
    gtk_widget_set_halign(back_button, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(settings_grid), back_button, 0, srow, 2, 1);
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), &widgets);
    srow++;

    // Preview Background
    GtkWidget *bg_frame = gtk_frame_new("Preview Background");
    gtk_grid_attach(GTK_GRID(settings_grid), bg_frame, 0, srow, 2, 1);
    GtkWidget *bg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(bg_frame), bg_box);
    gtk_container_set_border_width(GTK_CONTAINER(bg_box), 10);
    GtkWidget *white_btn = gtk_button_new_with_label("White");
    g_object_set_data(G_OBJECT(white_btn), "color", "#ffffff");
    g_signal_connect(white_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), white_btn, FALSE, FALSE, 0);
    GtkWidget *blue_btn = gtk_button_new_with_label("Blue");
    g_object_set_data(G_OBJECT(blue_btn), "color", "#0000ff");
    g_signal_connect(blue_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), blue_btn, FALSE, FALSE, 0);
    GtkWidget *red_btn = gtk_button_new_with_label("Red");
    g_object_set_data(G_OBJECT(red_btn), "color", "#ff0000");
    g_signal_connect(red_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), red_btn, FALSE, FALSE, 0);
    srow++;

    // Themes
    GtkWidget *theme_frame = gtk_frame_new("Themes");
    gtk_grid_attach(GTK_GRID(settings_grid), theme_frame, 0, srow, 2, 1);
    GtkWidget *theme_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(theme_frame), theme_box);
    gtk_container_set_border_width(GTK_CONTAINER(theme_box), 10);
    GtkWidget *theme_blue_btn = gtk_button_new_with_label("Blue Theme");
    g_signal_connect(theme_blue_btn, "clicked", G_CALLBACK(on_theme_blue_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_blue_btn, FALSE, FALSE, 0);
    GtkWidget *theme_default_btn = gtk_button_new_with_label("Default Theme");
    g_signal_connect(theme_default_btn, "clicked", G_CALLBACK(on_theme_default_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_default_btn, FALSE, FALSE, 0);
    srow++;

    // Gamma Control
    GtkWidget *gamma_frame = gtk_frame_new("Gamma Control (wlsunset)");
    gtk_grid_attach(GTK_GRID(settings_grid), gamma_frame, 0, srow, 2, 1);
    GtkWidget *gamma_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(gamma_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(gamma_grid), 8);
    gtk_container_add(GTK_CONTAINER(gamma_frame), gamma_grid);
    gtk_container_set_border_width(GTK_CONTAINER(gamma_grid), 10);

    widgets.gamma_check = gtk_check_button_new_with_label("Enable Gamma");
    gtk_grid_attach(GTK_GRID(gamma_grid), widgets.gamma_check, 0, 0, 2, 1);
    g_signal_connect(widgets.gamma_check, "toggled", G_CALLBACK(on_crosshair_changed), &widgets);

    GtkWidget *gamma_val_label = gtk_label_new("Gamma value:");
    gtk_widget_set_halign(gamma_val_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(gamma_grid), gamma_val_label, 0, 1, 1, 1);
    widgets.gamma_spin = gtk_spin_button_new_with_range(0.5, 2.0, 0.01);
    gtk_widget_set_hexpand(widgets.gamma_spin, TRUE);
    gtk_grid_attach(GTK_GRID(gamma_grid), widgets.gamma_spin, 1, 1, 1, 1);
    g_signal_connect(widgets.gamma_spin, "value-changed", G_CALLBACK(on_crosshair_changed), &widgets);
    srow++;

    // Spacer to push exit down
    GtkWidget *spacer2 = gtk_label_new("");
    gtk_widget_set_vexpand(spacer2, TRUE);
    gtk_grid_attach(GTK_GRID(settings_grid), spacer2, 0, srow, 2, 1);
    srow++;

    // Exit button
    GtkWidget *exit_button = gtk_button_new_with_label("✕ Exit");
    gtk_widget_set_size_request(exit_button, 120, 50);
    GtkStyleContext *exit_ctx = gtk_widget_get_style_context(exit_button);
    GtkCssProvider *exit_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(exit_provider,
        "button { background-color: #cc3333; color: white; font-weight: bold; font-size: 16px; border-radius: 8px; } "
        "button:hover { background-color: #dd4444; }", -1, NULL);
    gtk_style_context_add_provider(exit_ctx, GTK_STYLE_PROVIDER(exit_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(exit_provider);
    g_signal_connect(exit_button, "clicked", G_CALLBACK(on_exit_clicked), NULL);
    gtk_grid_attach(GTK_GRID(settings_grid), exit_button, 0, srow, 2, 1);
    gtk_widget_set_halign(exit_button, GTK_ALIGN_CENTER);

    // Load config
    load_config_into_ui(&widgets);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
