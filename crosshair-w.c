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

/* -------------------------------------------------------------------------
 * Data Structures
 * ---------------------------------------------------------------------- */

// Stores a specific crosshair look (used to remember settings per style)
typedef struct {
    double red, green, blue, alpha, size, thickness, gap, offset_x, offset_y;
} StylePreset;

// Main widget struct, holds every pointer and variable
typedef struct {
    GtkWidget *style_combo;
    GtkWidget *color_button;
    
    GtkWidget *size_scale, *size_spin;
    GtkWidget *alpha_scale, *alpha_spin;
    GtkWidget *offset_x_scale, *offset_x_spin;
    GtkWidget *offset_y_scale, *offset_y_spin;

    GtkWidget *thickness_scale, *thickness_spin;
    GtkWidget *thickness_label;
    GtkWidget *gap_scale, *gap_spin;
    GtkWidget *gap_label;

    GtkWidget *enable_check;
    GtkWidget *preview_area;
    
    double red, green, blue, alpha, size, thickness, gap, offset_x, offset_y;
    int style, enabled;
    
    StylePreset style_presets[4];

    GtkWidget *gamma_check;
    GtkWidget *gamma_spin;
    int use_gamma;
    double gamma;

    char preview_bg[32];
    char theme[32];
    GtkWidget *notebook;
    GtkWidget *window;

    // Set while widgets are being populated from disk (or from hardcoded
    // fallback defaults) at startup. save_config() checks this and no-ops
    // while it's set, so that setting a widget's value programmatically
    // during load doesn't fire its "changed"/"value-changed" signal and
    // write a half-loaded (or default-only) state to disk, clobbering the
    // real saved config before it's even been read.
    int loading;

    // Debounce id for the pending disk write (0 = none scheduled). Every
    // slider/spinbutton pair fires its callback twice for a single edit
    // (the slider's handler updates the spin button, which fires its own
    // "value-changed" right back), and a couple of code paths can fire an
    // extra signal on top of that (e.g. a style switch clamping the size
    // range). Writing to disk and SIGHUP-ing the daemon synchronously on
    // every one of those meant the daemon could reload several times in a
    // row for a single edit, using whatever partial widget state existed
    // at each intermediate instant - occasionally including a version of
    // the config where a checkbox hadn't visually "caught up" yet. Instead
    // we coalesce a burst of these into a single write of the final,
    // settled state a little while after the last signal arrives.
    guint save_source_id;
} Widgets;

// FORWARD DECLARATION (Fix for implicit declaration error)
void save_config(Widgets *w);

/* -------------------------------------------------------------------------
 * Preview Drawing
 * ---------------------------------------------------------------------- */

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
    const char *bg_str = (w->preview_bg[0] != '\0') ? w->preview_bg : "#000000";
    if (!gdk_rgba_parse(&bg_color, bg_str)) {
        // Malformed/empty color string - fall back to black instead of
        // leaving bg_color uninitialized (which made the preview look
        // "see-through" on compositors with window transparency enabled).
        gdk_rgba_parse(&bg_color, "#000000");
    }
    cairo_set_source_rgba(cr, bg_color.red, bg_color.green, bg_color.blue, bg_color.alpha);
    cairo_paint(cr);

    // Preview intentionally ignores "enabled" - it always shows the
    // crosshair so you can see/adjust its appearance even while the
    // real overlay is toggled off.
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, w->red, w->green, w->blue, w->alpha);

    double scale = 1.0;
    double max_dim = fmin(alloc.width, alloc.height) / 2.0 - 5;
    if (s > max_dim) scale = max_dim / s;

    double cx = alloc.width / 2.0 + (w->offset_x * scale);
    double cy = alloc.height / 2.0 + (w->offset_y * scale);

    switch (w->style) {
        case 0: cairo_arc(cr, cx, cy, s * scale, 0, 2 * M_PI); cairo_fill(cr); break;
        case 1: draw_cross_lines(cr, cx, cy, s, t, w->gap, scale); break;
        case 2: cairo_set_line_width(cr, t * scale); cairo_arc(cr, cx, cy, s * scale, 0, 2 * M_PI); cairo_stroke(cr); break;
        case 3: cairo_arc(cr, cx, cy, s * 0.4 * scale, 0, 2 * M_PI); cairo_fill(cr); draw_cross_lines(cr, cx, cy, s, t, w->gap, scale); break;
        default: break;
    }
    return FALSE;
}

/* -------------------------------------------------------------------------
 * Daemon Control
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Styling / Theming
 * ---------------------------------------------------------------------- */

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
            "colorbutton { border-radius: 8px; } "
            ".rounded-color { border-radius: 8px; } "
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
            "colorbutton { border-radius: 8px; } "
            ".rounded-color { border-radius: 8px; } "
            "button { background-color: #2a2c3a; color: #e4e4ec; border-radius: 8px; border: 1px solid #3a3d52; padding: 6px 10px; } "
            "button:hover { background-color: #35374a; } "
            ".page-title { font-size: 15pt; font-weight: bold; color: #e4e4ec; }";
    }

    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* -------------------------------------------------------------------------
 * Helper: Update UI state based on selected style
 * ---------------------------------------------------------------------- */
static void update_style_dependent_widgets(Widgets *w) {
    int style = gtk_combo_box_get_active(GTK_COMBO_BOX(w->style_combo));
    gboolean is_cross = (style == 1 || style == 3); // Cross, Dot + Cross
    gboolean uses_thickness = (style != 0); // everything except Dot

    gtk_range_set_range(GTK_RANGE(w->size_scale), is_cross ? 2.5 : 1.0, 50);
    gtk_spin_button_set_range(GTK_SPIN_BUTTON(w->size_spin), is_cross ? 2.5 : 1.0, 50);
    
    gtk_widget_set_sensitive(w->gap_scale, is_cross);
    gtk_widget_set_sensitive(w->gap_label, is_cross);
    gtk_widget_set_sensitive(w->gap_spin, is_cross);
    
    gtk_widget_set_sensitive(w->thickness_scale, uses_thickness);
    gtk_widget_set_sensitive(w->thickness_label, uses_thickness);
    gtk_widget_set_sensitive(w->thickness_spin, uses_thickness);
}

/* -------------------------------------------------------------------------
 * Sync Callbacks (Sliders <-> Spinbuttons)
 * ---------------------------------------------------------------------- */
void on_size_spin_changed(GtkSpinButton *spin, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_range_set_value(GTK_RANGE(w->size_scale), gtk_spin_button_get_value(spin));
    save_config(w);
}
void on_size_slider_changed(GtkRange *range, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->size_spin), gtk_range_get_value(range));
    save_config(w);
}
void on_alpha_spin_changed(GtkSpinButton *spin, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_range_set_value(GTK_RANGE(w->alpha_scale), gtk_spin_button_get_value(spin));
    save_config(w);
}
void on_alpha_slider_changed(GtkRange *range, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->alpha_spin), gtk_range_get_value(range));
    save_config(w);
}
void on_offset_x_spin_changed(GtkSpinButton *spin, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_range_set_value(GTK_RANGE(w->offset_x_scale), gtk_spin_button_get_value(spin));
    save_config(w);
}
void on_offset_x_slider_changed(GtkRange *range, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->offset_x_spin), gtk_range_get_value(range));
    save_config(w);
}
void on_offset_y_spin_changed(GtkSpinButton *spin, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_range_set_value(GTK_RANGE(w->offset_y_scale), gtk_spin_button_get_value(spin));
    save_config(w);
}
void on_offset_y_slider_changed(GtkRange *range, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->offset_y_spin), gtk_range_get_value(range));
    save_config(w);
}
void on_thickness_spin_changed(GtkSpinButton *spin, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_range_set_value(GTK_RANGE(w->thickness_scale), gtk_spin_button_get_value(spin));
    save_config(w);
}
void on_thickness_slider_changed(GtkRange *range, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->thickness_spin), gtk_range_get_value(range));
    save_config(w);
}
void on_gap_spin_changed(GtkSpinButton *spin, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_range_set_value(GTK_RANGE(w->gap_scale), gtk_spin_button_get_value(spin));
    save_config(w);
}
void on_gap_slider_changed(GtkRange *range, gpointer data) {
    Widgets *w = (Widgets*)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->gap_spin), gtk_range_get_value(range));
    save_config(w);
}

/* -------------------------------------------------------------------------
 * Config Save / Load
 * ---------------------------------------------------------------------- */

void save_config(Widgets *w) {
    if (w->loading) {
        // We're in the middle of populating widgets from a config file (or
        // from fallback defaults) at startup. Every gtk_range_set_value /
        // gtk_combo_box_set_active call during that process fires the same
        // "changed" signals a real user edit would, which would otherwise
        // call save_config() with a partially-loaded state and overwrite
        // the on-disk config before we've finished reading it.
        debug("save_config: skipped, still loading config into UI\n");
        return;
    }
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
    w->offset_x = gtk_range_get_value(GTK_RANGE(w->offset_x_scale));
    w->offset_y = gtk_range_get_value(GTK_RANGE(w->offset_y_scale));
    w->enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->enable_check));
    w->use_gamma = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->gamma_check));
    w->gamma = gtk_spin_button_get_value(GTK_SPIN_BUTTON(w->gamma_spin));

    if (w->style >= 0 && w->style < 4) {
        StylePreset *p = &w->style_presets[w->style];
        p->red = w->red;
        p->green = w->green;
        p->blue = w->blue;
        p->alpha = w->alpha;
        p->size = w->size;
        p->thickness = w->thickness;
        p->gap = w->gap;
        p->offset_x = w->offset_x;
        p->offset_y = w->offset_y;
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
    json_object_object_add(root, "offset_x", json_object_new_double(w->offset_x));
    json_object_object_add(root, "offset_y", json_object_new_double(w->offset_y));
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
        json_object_object_add(s, "offset_x", json_object_new_double(p->offset_x));
        json_object_object_add(s, "offset_y", json_object_new_double(p->offset_y));
        json_object_array_add(styles_arr, s);
    }
    json_object_object_add(root, "styles", styles_arr);

    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s", json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY));
        fclose(f);
    }
    json_object_put(root);

    reload_daemon();
    gtk_widget_queue_draw(w->preview_area);
}

void load_config_into_ui(Widgets *w) {
    // Block save_config() for the duration of this function. Every
    // gtk_*_set_* call below fires the same signal a live user edit would,
    // and previously each one called save_config() immediately - meaning
    // the act of *loading* the config would itself overwrite the file on
    // disk with whatever partial state existed at that instant, before the
    // rest of the real saved values had even been read. That's what made
    // settings (like your selected style) appear to reset on every launch.
    w->loading = 1;

    // Sane fallback defaults, used for anything not found in the config
    // file below (missing file, corrupt file, or a config that's simply
    // missing a key, e.g. after a code change added a new field).
    gtk_combo_box_set_active(GTK_COMBO_BOX(w->style_combo), 0); // Dot
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->enable_check), TRUE);
    strcpy(w->preview_bg, "#000000");
    strcpy(w->theme, "default");

    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof(path), "%s%s", home, CONFIG_PATH);

    FILE *f = fopen(path, "r");
    if (!f) {
        w->use_gamma = 0;
        w->gamma = 1.2;
        w->loading = 0;
        save_config(w); // no existing config - persist these defaults

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
        w->use_gamma = 0;
        w->gamma = 1.2;
        w->loading = 0;
        save_config(w); // corrupt config - persist sane defaults
        return;
    }

    json_object *tmp;
    if (json_object_object_get_ex(root, "style", &tmp))
        gtk_combo_box_set_active(GTK_COMBO_BOX(w->style_combo), json_object_get_int(tmp));
    {
        GdkRGBA rgba = { 0.0, 0.0, 0.0, 1.0 };
        if (json_object_object_get_ex(root, "red", &tmp)) rgba.red = json_object_get_double(tmp);
        if (json_object_object_get_ex(root, "green", &tmp)) rgba.green = json_object_get_double(tmp);
        if (json_object_object_get_ex(root, "blue", &tmp)) rgba.blue = json_object_get_double(tmp);
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w->color_button), &rgba);
    }
    if (json_object_object_get_ex(root, "alpha", &tmp)) {
        double val = json_object_get_double(tmp);
        gtk_range_set_value(GTK_RANGE(w->alpha_scale), val);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->alpha_spin), val);
    }
    if (json_object_object_get_ex(root, "size", &tmp)) {
        double val = json_object_get_double(tmp);
        gtk_range_set_value(GTK_RANGE(w->size_scale), val);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->size_spin), val);
    }
    if (json_object_object_get_ex(root, "thickness", &tmp)) {
        double val = json_object_get_double(tmp);
        gtk_range_set_value(GTK_RANGE(w->thickness_scale), val);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->thickness_spin), val);
    }
    if (json_object_object_get_ex(root, "gap", &tmp)) {
        double val = json_object_get_double(tmp);
        gtk_range_set_value(GTK_RANGE(w->gap_scale), val);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->gap_spin), val);
    }
    if (json_object_object_get_ex(root, "offset_x", &tmp)) {
        double val = json_object_get_double(tmp);
        gtk_range_set_value(GTK_RANGE(w->offset_x_scale), val);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->offset_x_spin), val);
    }
    if (json_object_object_get_ex(root, "offset_y", &tmp)) {
        double val = json_object_get_double(tmp);
        gtk_range_set_value(GTK_RANGE(w->offset_y_scale), val);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->offset_y_spin), val);
    }

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
            if (json_object_object_get_ex(sp, "offset_x", &f)) p->offset_x = json_object_get_double(f);
            if (json_object_object_get_ex(sp, "offset_y", &f)) p->offset_y = json_object_get_double(f);
        }
    }

    if (json_object_object_get_ex(root, "enabled", &tmp))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->enable_check), json_object_get_boolean(tmp));
    if (json_object_object_get_ex(root, "use_gamma", &tmp))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->gamma_check), json_object_get_boolean(tmp));
    if (json_object_object_get_ex(root, "gamma", &tmp))
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w->gamma_spin), json_object_get_double(tmp));
    if (json_object_object_get_ex(root, "preview_bg", &tmp) &&
        json_object_get_string(tmp)[0] != '\0') {
        const char *bg = json_object_get_string(tmp);
        strncpy(w->preview_bg, bg, sizeof(w->preview_bg)-1);
        w->preview_bg[sizeof(w->preview_bg)-1] = '\0';
    }
    // else: leave the "#000000" default set at the top of this function -
    // an empty string in the config (which is what a fresh save produces
    // if this bug is hit) used to be copied verbatim and made the preview
    // background parse to garbage/transparent.
    if (json_object_object_get_ex(root, "theme", &tmp)) {
        const char *theme = json_object_get_string(tmp);
        strncpy(w->theme, theme, sizeof(w->theme)-1);
        w->theme[sizeof(w->theme)-1] = '\0';
    } else {
        strcpy(w->theme, "default");
    }

    json_object_put(root);
    w->loading = 0; // done populating widgets - save_config() works normally again
    apply_theme(w);
    update_style_dependent_widgets(w);
    gtk_widget_queue_draw(w->preview_area);
}

/* -------------------------------------------------------------------------
 * General Callbacks
 * ---------------------------------------------------------------------- */

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
        g_signal_handlers_block_by_func(w->offset_x_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_block_by_func(w->offset_y_scale, G_CALLBACK(on_crosshair_changed), w);

        GdkRGBA rgba = { p->red, p->green, p->blue, 1.0 };
        gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(w->color_button), &rgba);
        gtk_range_set_value(GTK_RANGE(w->alpha_scale), p->alpha);
        gtk_range_set_value(GTK_RANGE(w->size_scale), p->size);
        gtk_range_set_value(GTK_RANGE(w->thickness_scale), p->thickness);
        gtk_range_set_value(GTK_RANGE(w->gap_scale), p->gap);
        gtk_range_set_value(GTK_RANGE(w->offset_x_scale), p->offset_x);
        gtk_range_set_value(GTK_RANGE(w->offset_y_scale), p->offset_y);

        g_signal_handlers_unblock_by_func(w->color_button, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->alpha_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->size_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->thickness_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->gap_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->offset_x_scale, G_CALLBACK(on_crosshair_changed), w);
        g_signal_handlers_unblock_by_func(w->offset_y_scale, G_CALLBACK(on_crosshair_changed), w);
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
    (void)data;
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

/* -------------------------------------------------------------------------
 * Daemon Startup / Signal Handling
 * ---------------------------------------------------------------------- */

void ensure_daemon_running() {
    FILE *f = fopen("/tmp/crosshaird.pid", "r");
    if (f) {
        pid_t pid;
        if (fscanf(f, "%d", &pid) == 1 && kill(pid, 0) == 0) {
            fclose(f);
            return;
        }
        fclose(f);
    }
    const char *args = verbose ? "crosshaird -v &" : "crosshaird &";
    int ret = system(args);
    if (ret == -1) {
        debug("Failed to start daemon\n");
    }
}

static void sigint_handler(int sig) {
    (void)sig;
    kill_daemon();
    gtk_main_quit();
}

/* -------------------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
    }

    gtk_init(&argc, &argv);
    ensure_daemon_running();
    signal(SIGINT, sigint_handler);

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Crosshair");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 800); 
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), NULL);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header), "Crosshair");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
    GtkWidget *icon = gtk_image_new_from_file("icon.svg");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 16);
    gtk_widget_set_size_request(icon, 16, 16);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), icon);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook), FALSE);
    gtk_container_add(GTK_CONTAINER(window), notebook);

    Widgets widgets = {0};
    // Guard save_config() against firing while the UI is still being built
    // below (widget creation and the default-value calls before
    // load_config_into_ui() runs can themselves trigger "changed" signals).
    // load_config_into_ui() takes ownership of this flag and clears it once
    // real values are in place.
    widgets.loading = 1;
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
        widgets.style_presets[i].offset_x = 0.0;
        widgets.style_presets[i].offset_y = 0.0;
    }

    /* ------------------------- Crosshair Tab ------------------------- */
    GtkWidget *crosshair_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(crosshair_page), 18);
    GtkWidget *crosshair_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(crosshair_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(crosshair_scroll), crosshair_page);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), crosshair_scroll, gtk_label_new("Crosshair"));

    GtkWidget *page_title = gtk_label_new("Crosshair");
    gtk_widget_set_halign(page_title, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(page_title), "page-title");
    gtk_box_pack_start(GTK_BOX(crosshair_page), page_title, FALSE, FALSE, 0);

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
    gtk_widget_set_halign(widgets.color_button, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widgets.color_button, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(widgets.color_button), "rounded-color");
    gtk_grid_attach(GTK_GRID(appearance_grid), widgets.color_button, 1, arow++, 1, 1);
    g_signal_connect(widgets.color_button, "color-set", G_CALLBACK(on_crosshair_changed), &widgets);

    GtkWidget *size_frame = gtk_frame_new("Size, position & opacity");
    gtk_box_pack_start(GTK_BOX(crosshair_page), size_frame, FALSE, FALSE, 0);
    GtkWidget *size_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(size_grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(size_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(size_grid), 14);
    gtk_container_add(GTK_CONTAINER(size_frame), size_grid);

    int srow = 0;
    
    GtkWidget *size_label = gtk_label_new("Size (px)");
    gtk_widget_set_halign(size_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), size_label, 0, srow, 1, 1);
    widgets.size_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 50, 1);
    gtk_widget_set_hexpand(widgets.size_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.size_scale, 1, srow, 1, 1);
    g_signal_connect(widgets.size_scale, "value-changed", G_CALLBACK(on_size_slider_changed), &widgets);
    widgets.size_spin = gtk_spin_button_new_with_range(1, 50, 1);
    gtk_widget_set_size_request(widgets.size_spin, 80, -1);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.size_spin, 2, srow++, 1, 1);
    g_signal_connect(widgets.size_spin, "value-changed", G_CALLBACK(on_size_spin_changed), &widgets);

    GtkWidget *alpha_label = gtk_label_new("Opacity");
    gtk_widget_set_halign(alpha_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), alpha_label, 0, srow, 1, 1);
    widgets.alpha_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 0.01);
    gtk_widget_set_hexpand(widgets.alpha_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.alpha_scale, 1, srow, 1, 1);
    g_signal_connect(widgets.alpha_scale, "value-changed", G_CALLBACK(on_alpha_slider_changed), &widgets);
    widgets.alpha_spin = gtk_spin_button_new_with_range(0, 1, 0.01);
    gtk_widget_set_size_request(widgets.alpha_spin, 80, -1);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.alpha_spin, 2, srow++, 1, 1);
    g_signal_connect(widgets.alpha_spin, "value-changed", G_CALLBACK(on_alpha_spin_changed), &widgets);

    GtkWidget *offset_x_label = gtk_label_new("Offset X\n(relative to center)");
    gtk_widget_set_halign(offset_x_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), offset_x_label, 0, srow, 1, 1);
    widgets.offset_x_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -50, 50, 1);
    gtk_widget_set_hexpand(widgets.offset_x_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.offset_x_scale, 1, srow, 1, 1);
    g_signal_connect(widgets.offset_x_scale, "value-changed", G_CALLBACK(on_offset_x_slider_changed), &widgets);
    widgets.offset_x_spin = gtk_spin_button_new_with_range(-50, 50, 1);
    gtk_widget_set_size_request(widgets.offset_x_spin, 80, -1);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.offset_x_spin, 2, srow++, 1, 1);
    g_signal_connect(widgets.offset_x_spin, "value-changed", G_CALLBACK(on_offset_x_spin_changed), &widgets);

    GtkWidget *offset_y_label = gtk_label_new("Offset Y\n(relative to center)");
    gtk_widget_set_halign(offset_y_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), offset_y_label, 0, srow, 1, 1);
    widgets.offset_y_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -50, 50, 1);
    gtk_widget_set_hexpand(widgets.offset_y_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.offset_y_scale, 1, srow, 1, 1);
    g_signal_connect(widgets.offset_y_scale, "value-changed", G_CALLBACK(on_offset_y_slider_changed), &widgets);
    widgets.offset_y_spin = gtk_spin_button_new_with_range(-50, 50, 1);
    gtk_widget_set_size_request(widgets.offset_y_spin, 80, -1);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.offset_y_spin, 2, srow++, 1, 1);
    g_signal_connect(widgets.offset_y_spin, "value-changed", G_CALLBACK(on_offset_y_spin_changed), &widgets);

    widgets.thickness_label = gtk_label_new("Thickness (px)");
    gtk_widget_set_halign(widgets.thickness_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.thickness_label, 0, srow, 1, 1);
    widgets.thickness_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 10, 1);
    gtk_widget_set_hexpand(widgets.thickness_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.thickness_scale, 1, srow, 1, 1);
    g_signal_connect(widgets.thickness_scale, "value-changed", G_CALLBACK(on_thickness_slider_changed), &widgets);
    widgets.thickness_spin = gtk_spin_button_new_with_range(1, 10, 1);
    gtk_widget_set_size_request(widgets.thickness_spin, 80, -1);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.thickness_spin, 2, srow++, 1, 1);
    g_signal_connect(widgets.thickness_spin, "value-changed", G_CALLBACK(on_thickness_spin_changed), &widgets);

    widgets.gap_label = gtk_label_new("Gap (px)");
    gtk_widget_set_halign(widgets.gap_label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.gap_label, 0, srow, 1, 1);
    widgets.gap_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 30, 1);
    gtk_widget_set_hexpand(widgets.gap_scale, TRUE);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.gap_scale, 1, srow, 1, 1);
    g_signal_connect(widgets.gap_scale, "value-changed", G_CALLBACK(on_gap_slider_changed), &widgets);
    widgets.gap_spin = gtk_spin_button_new_with_range(0, 30, 1);
    gtk_widget_set_size_request(widgets.gap_spin, 80, -1);
    gtk_grid_attach(GTK_GRID(size_grid), widgets.gap_spin, 2, srow++, 1, 1);
    g_signal_connect(widgets.gap_spin, "value-changed", G_CALLBACK(on_gap_spin_changed), &widgets);

    widgets.enable_check = gtk_check_button_new_with_label("Enable Crosshair");
    gtk_box_pack_start(GTK_BOX(crosshair_page), widgets.enable_check, FALSE, FALSE, 0);
    g_signal_connect(widgets.enable_check, "toggled", G_CALLBACK(on_crosshair_changed), &widgets);

    GtkWidget *settings_button = gtk_button_new_with_label("⚙ Misc");
    gtk_box_pack_start(GTK_BOX(crosshair_page), settings_button, FALSE, FALSE, 0);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_clicked), &widgets);

    /* ------------------------- Misc Tab ------------------------- */
    GtkWidget *settings_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(settings_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(settings_grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(settings_grid), 20);
    GtkWidget *settings_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(settings_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(settings_scroll), settings_grid);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), settings_scroll, gtk_label_new("Misc"));

    int mrow = 0;
    GtkWidget *back_button = gtk_button_new_with_label("← Back");
    gtk_widget_set_halign(back_button, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(settings_grid), back_button, 0, mrow, 2, 1);
    g_signal_connect(back_button, "clicked", G_CALLBACK(on_back_clicked), &widgets);
    mrow++;

    GtkWidget *bg_frame = gtk_frame_new("Preview Background");
    gtk_grid_attach(GTK_GRID(settings_grid), bg_frame, 0, mrow, 2, 1);
    GtkWidget *bg_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(bg_frame), bg_box);
    gtk_container_set_border_width(GTK_CONTAINER(bg_box), 10);
    GtkWidget *black_btn = gtk_button_new_with_label("Black");
    g_object_set_data(G_OBJECT(black_btn), "color", "#000000");
    g_signal_connect(black_btn, "clicked", G_CALLBACK(on_preview_bg_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(bg_box), black_btn, FALSE, FALSE, 0);
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
    mrow++;

    GtkWidget *theme_frame = gtk_frame_new("Themes");
    gtk_grid_attach(GTK_GRID(settings_grid), theme_frame, 0, mrow, 2, 1);
    GtkWidget *theme_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(theme_frame), theme_box);
    gtk_container_set_border_width(GTK_CONTAINER(theme_box), 10);
    GtkWidget *theme_blue_btn = gtk_button_new_with_label("Blue Theme");
    g_signal_connect(theme_blue_btn, "clicked", G_CALLBACK(on_theme_blue_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_blue_btn, FALSE, FALSE, 0);
    GtkWidget *theme_default_btn = gtk_button_new_with_label("Default Theme");
    g_signal_connect(theme_default_btn, "clicked", G_CALLBACK(on_theme_default_clicked), &widgets);
    gtk_box_pack_start(GTK_BOX(theme_box), theme_default_btn, FALSE, FALSE, 0);
    mrow++;

    GtkWidget *gamma_frame = gtk_frame_new("Gamma Control (wlsunset)");
    gtk_grid_attach(GTK_GRID(settings_grid), gamma_frame, 0, mrow, 2, 1);
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
    mrow++;

    gtk_range_set_value(GTK_RANGE(widgets.offset_x_scale), 0.0);
    gtk_range_set_value(GTK_RANGE(widgets.offset_y_scale), 0.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.offset_x_spin), 0.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widgets.offset_y_spin), 0.0);

    load_config_into_ui(&widgets);

    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}