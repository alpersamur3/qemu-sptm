/*
 * GTK UI
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Portions from gtk-vnc (originally licensed under the LGPL v2+):
 *
 * GTK VNC Widget
 *
 * Copyright (C) 2006  Anthony Liguori <anthony@codemonkey.ws>
 * Copyright (C) 2009-2010 Daniel P. Berrange <dan@berrange.com>
 */

#define GETTEXT_PACKAGE "qemu"
#define LOCALEDIR "po"

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu-main.h"

#include "ui/console.h"
#include "ui/gtk.h"
#ifdef G_OS_WIN32
#include <gdk/gdkwin32.h>
#endif
#include "ui/win32-kbd-hook.h"

#include <glib/gi18n.h>
#include <locale.h>
#if defined(CONFIG_VTE)
#include <vte/vte.h>
#endif
#include <math.h>

#include "trace.h"
#include "standard-headers/linux/input-event-codes.h"
#include "ui/input.h"
#include "system/runstate.h"
#include "system/system.h"
#include "migration/snapshot.h"
#include "migration/migration.h"
#include "block/snapshot.h"
#include "block/graph-lock.h"
#include "keymaps.h"
#include "chardev/char.h"
#include "qom/object.h"

#define VC_WINDOW_X_MIN  320
#define VC_WINDOW_Y_MIN  240
#define VC_TERM_X_MIN     80
#define VC_TERM_Y_MIN     25
#define VC_SCALE_MIN    0.25
#define VC_SCALE_MAX       4
#define VC_SCALE_STEP   0.25

#ifdef GDK_WINDOWING_X11
#include "x_keymap.h"

/* Gtk2 compat */
#ifndef GDK_IS_X11_DISPLAY
#define GDK_IS_X11_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_WAYLAND
/* Gtk2 compat */
#ifndef GDK_IS_WAYLAND_DISPLAY
#define GDK_IS_WAYLAND_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_WIN32
/* Gtk2 compat */
#ifndef GDK_IS_WIN32_DISPLAY
#define GDK_IS_WIN32_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_BROADWAY
/* Gtk2 compat */
#ifndef GDK_IS_BROADWAY_DISPLAY
#define GDK_IS_BROADWAY_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#ifdef GDK_WINDOWING_QUARTZ
/* Gtk2 compat */
#ifndef GDK_IS_QUARTZ_DISPLAY
#define GDK_IS_QUARTZ_DISPLAY(dpy) (dpy != NULL)
#endif
#endif


#if !defined(CONFIG_VTE)
# define VTE_CHECK_VERSION(a, b, c) 0
#endif

#define HOTKEY_MODIFIERS        (GDK_CONTROL_MASK | GDK_MOD1_MASK)

static const guint16 *keycode_map;
static size_t keycode_maplen;
static bool keycode_xorgevdev;

struct VCChardev {
    Chardev parent;
    VirtualConsole *console;
    bool echo;
};
typedef struct VCChardev VCChardev;

#define TYPE_CHARDEV_VC "chardev-vc"
DECLARE_INSTANCE_CHECKER(VCChardev, VC_CHARDEV,
                         TYPE_CHARDEV_VC)

static struct touch_slot touch_slots[INPUT_EVENT_SLOTS_MAX];

bool gtk_use_gl_area;

static void gd_grab_pointer(VirtualConsole *vc, const char *reason);
static void gd_ungrab_pointer(GtkDisplayState *s);
static void gd_grab_keyboard(VirtualConsole *vc, const char *reason);
static void gd_ungrab_keyboard(GtkDisplayState *s);
static void gd_rebuild_vc_menu(GtkDisplayState *s);

/** Utility Functions **/

static VirtualConsole *gd_vc_find_by_menu(GtkDisplayState *s)
{
    VirtualConsole *vc;
    gint i;

    for (i = 0; i < s->vcs->len; i++) {
        vc = g_ptr_array_index(s->vcs, i);
        if (gtk_check_menu_item_get_active
            (GTK_CHECK_MENU_ITEM(vc->menu_item))) {
            return vc;
        }
    }
    return NULL;
}

static VirtualConsole *gd_vc_find_by_page(GtkDisplayState *s, gint page)
{
    VirtualConsole *vc;
    gint i, p;

    if (!s->vcs) {
        return NULL;
    }
    for (i = 0; i < s->vcs->len; i++) {
        vc = g_ptr_array_index(s->vcs, i);
        p = gtk_notebook_page_num(GTK_NOTEBOOK(s->notebook), vc->tab_item);
        if (p == page) {
            return vc;
        }
    }
    return NULL;
}

static VirtualConsole *gd_vc_find_current(GtkDisplayState *s)
{
    gint page;

    page = gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook));
    return gd_vc_find_by_page(s, page);
}

static bool gd_is_grab_active(GtkDisplayState *s)
{
    return gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->grab_item));
}

static bool gd_grab_on_hover(GtkDisplayState *s)
{
    return gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->grab_on_hover_item));
}

static void gd_update_cursor(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    GdkWindow *window;

    if (vc->type != GD_VC_GFX ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    window = gtk_widget_get_window(GTK_WIDGET(vc->gfx.drawing_area));
    if (s->full_screen || qemu_input_is_absolute(vc->gfx.dcl.con) || s->ptr_owner == vc) {
        gdk_window_set_cursor(window, s->null_cursor);
    } else {
        gdk_window_set_cursor(window, NULL);
    }
}

static void gd_update_caption(GtkDisplayState *s)
{
    const char *status = "";
    gchar *prefix;
    gchar *title;
    const char *grab = "";
    bool is_paused = !runstate_is_running();
    int i;

    if (qemu_name) {
        /* -name: the window is the guest's (the ASoftware iOS emulator: "ASoftware iOS") */
        prefix = g_strdup(qemu_name);
    } else {
        prefix = g_strdup_printf("QEMU");
    }

    if (s->ptr_owner != NULL &&
        s->ptr_owner->window == NULL) {
        grab = _(" - Press Ctrl+Alt+G to release grab");
    }

    if (is_paused) {
        status = _(" [Paused]");
    }
    s->external_pause_update = true;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->pause_item),
                                   is_paused);
    s->external_pause_update = false;

    title = g_strdup_printf("%s%s%s", prefix, status, grab);
    gtk_window_set_title(GTK_WINDOW(s->window), title);
    g_free(title);

    for (i = 0; i < s->vcs->len; i++) {
        VirtualConsole *vc = g_ptr_array_index(s->vcs, i);

        if (!vc->window) {
            continue;
        }
        title = g_strdup_printf("%s: %s%s%s", prefix, vc->label,
                                vc == s->kbd_owner ? " +kbd" : "",
                                vc == s->ptr_owner ? " +ptr" : "");
        gtk_window_set_title(GTK_WINDOW(vc->window), title);
        g_free(title);
    }

    g_free(prefix);
}

static void gd_update_geometry_hints(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    GdkWindowHints mask = 0;
    GdkGeometry geo = {};
    GtkWidget *geo_widget = NULL;
    GtkWindow *geo_window;

    if (vc->type == GD_VC_GFX) {
        if (!vc->gfx.ds) {
            return;
        }
        double scale_x = s->free_scale ? VC_SCALE_MIN : vc->gfx.scale_x;
        double scale_y = s->free_scale ? VC_SCALE_MIN : vc->gfx.scale_y;
        geo.min_width  = surface_width(vc->gfx.ds) * scale_x;
        geo.min_height = surface_height(vc->gfx.ds) * scale_y;
        mask |= GDK_HINT_MIN_SIZE;
        geo_widget = vc->gfx.drawing_area;
        gtk_widget_set_size_request(geo_widget, geo.min_width, geo.min_height);

#if defined(CONFIG_VTE)
    } else if (vc->type == GD_VC_VTE) {
        VteTerminal *term = VTE_TERMINAL(vc->vte.terminal);
        GtkBorder padding = { 0 };

#if VTE_CHECK_VERSION(0, 37, 0)
        gtk_style_context_get_padding(
                gtk_widget_get_style_context(vc->vte.terminal),
                gtk_widget_get_state_flags(vc->vte.terminal),
                &padding);
#else
        {
            GtkBorder *ib = NULL;
            gtk_widget_style_get(vc->vte.terminal, "inner-border", &ib, NULL);
            if (ib) {
                padding = *ib;
                gtk_border_free(ib);
            }
        }
#endif

        geo.width_inc  = vte_terminal_get_char_width(term);
        geo.height_inc = vte_terminal_get_char_height(term);
        mask |= GDK_HINT_RESIZE_INC;
        geo.base_width  = geo.width_inc;
        geo.base_height = geo.height_inc;
        mask |= GDK_HINT_BASE_SIZE;
        geo.min_width  = geo.width_inc * VC_TERM_X_MIN;
        geo.min_height = geo.height_inc * VC_TERM_Y_MIN;
        mask |= GDK_HINT_MIN_SIZE;

        geo.base_width  += padding.left + padding.right;
        geo.base_height += padding.top + padding.bottom;
        geo.min_width   += padding.left + padding.right;
        geo.min_height  += padding.top + padding.bottom;
        geo_widget = vc->vte.terminal;
#endif
    }

    geo_window = GTK_WINDOW(vc->window ? vc->window : s->window);
    gtk_window_set_geometry_hints(geo_window, geo_widget, &geo, mask);
}

/*
 * Zoom-to-fit: size the window to the guest display's shape, as large as fits
 * WINDOW_FIT_SHARE of the monitor's work area, so the guest fills it without
 * bars (a phone's 1080x2400 screen gets a tall, narrow window). Done whenever
 * the guest display changes size, and by View > Best Fit.
 */
#define WINDOW_FIT_SHARE 0.92

static void gd_fit_window(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;
    GtkWindow *window = GTK_WINDOW(vc->window ? vc->window : s->window);
    GdkWindow *gdk_window = gtk_widget_get_window(GTK_WIDGET(window));
    GdkMonitor *monitor;
    GdkRectangle area, frame;
    int width, height, menu = 0, bar = 0, x, y;
    double scale;

    if (!gdk_window || !vc->gfx.ds) {
        return;
    }
    width = surface_width(vc->gfx.ds);
    height = surface_height(vc->gfx.ds);
    monitor = gdk_display_get_monitor_at_window(
        gtk_widget_get_display(GTK_WIDGET(window)), gdk_window);
    if (!monitor || width <= 0 || height <= 0) {
        return;
    }
    gdk_monitor_get_workarea(monitor, &area);
    /* title bar and borders */
    gdk_window_get_frame_extents(gdk_window, &frame);
    frame.width -= gdk_window_get_width(gdk_window);
    frame.height -= gdk_window_get_height(gdk_window);
    if (!vc->window && gtk_widget_get_visible(s->menu_bar)) {
        menu = gtk_widget_get_allocated_height(s->menu_bar);
    }
    if (!vc->window && s->sidebar && gtk_widget_get_visible(s->sidebar)) {
        gtk_widget_get_preferred_width(s->sidebar, &bar, NULL);
    }
    scale = MIN((area.height * WINDOW_FIT_SHARE - frame.height - menu) / height,
                (area.width * WINDOW_FIT_SHARE - frame.width - bar) / width);
    if (scale <= VC_SCALE_MIN) {
        return;
    }
    width = width * scale + bar;
    height = height * scale + menu;
    gtk_window_resize(window, width, height);

    /* and keep the whole window inside that work area */
    gtk_window_get_position(window, &x, &y);
    x = MAX(area.x, MIN(x, area.x + area.width - width - frame.width));
    y = MAX(area.y, MIN(y, area.y + area.height - height - frame.height));
    gtk_window_move(window, x, y);
}

void gd_update_windowsize(VirtualConsole *vc)
{
    GtkDisplayState *s = vc->s;

    gd_update_geometry_hints(vc);

    if (vc->type == GD_VC_GFX && !s->full_screen && !s->free_scale) {
        gtk_window_resize(GTK_WINDOW(vc->window ? vc->window : s->window),
                          VC_WINDOW_X_MIN, VC_WINDOW_Y_MIN);
    } else if (vc->type == GD_VC_GFX && !s->full_screen) {
        gd_fit_window(vc);
    }
}

/*
 * Zoom-to-fit, once the window is up: a guest display whose size is set before
 * the window exists and never changes (pvg=reims) gets no resize to fit on.
 */
static gboolean gd_fit_window_idle(void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);
    int i;

    if (!vc || vc->type != GD_VC_GFX || !s->free_scale || s->full_screen) {
        return G_SOURCE_REMOVE;
    }
    /*
     * The other graphic consoles' pages (vresearch's boot framebuffer) still
     * ask for their full size from before zoom-to-fit was on, and the window
     * cannot get smaller than its largest page: renew their size requests,
     * the current console's last, as its hints are the window's.
     */
    for (i = 0; i < s->vcs->len; i++) {
        VirtualConsole *v = g_ptr_array_index(s->vcs, i);

        if (v != vc && v->type == GD_VC_GFX && !v->window) {
            gd_update_geometry_hints(v);
        }
    }
    gd_update_geometry_hints(vc);
    gd_fit_window(vc);
    return G_SOURCE_REMOVE;
}

static void gd_update_full_redraw(VirtualConsole *vc)
{
    GtkWidget *area = vc->gfx.drawing_area;
    int ww, wh;
    ww = gdk_window_get_width(gtk_widget_get_window(area));
    wh = gdk_window_get_height(gtk_widget_get_window(area));
#if defined(CONFIG_OPENGL)
    if (vc->gfx.gls && gtk_use_gl_area) {
        gtk_gl_area_queue_render(GTK_GL_AREA(vc->gfx.drawing_area));
        return;
    }
#endif
    gtk_widget_queue_draw_area(area, 0, 0, ww, wh);
}

static void gtk_release_modifiers(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);

    if (!vc || vc->type != GD_VC_GFX ||
        !qemu_console_is_graphic(vc->gfx.dcl.con)) {
        return;
    }
    qkbd_state_lift_all_keys(vc->gfx.kbd);
}

static void gd_widget_reparent(GtkWidget *from, GtkWidget *to,
                               GtkWidget *widget)
{
    g_object_ref(G_OBJECT(widget));
    gtk_container_remove(GTK_CONTAINER(from), widget);
    gtk_container_add(GTK_CONTAINER(to), widget);
    g_object_unref(G_OBJECT(widget));
}

static void *gd_win32_get_hwnd(VirtualConsole *vc)
{
#ifdef G_OS_WIN32
    return gdk_win32_window_get_impl_hwnd(
        gtk_widget_get_window(vc->window ? vc->window : vc->s->window));
#else
    return NULL;
#endif
}

/** DisplayState Callbacks **/

static void gd_update(DisplayChangeListener *dcl,
                      int fbx, int fby, int fbw, int fbh)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkWindow *win;
    int wx1, wx2, wy1, wy2;
    int wx_offset, wy_offset;
    int ww_surface, wh_surface;
    int ww_widget, wh_widget;

    trace_gd_update(vc->label, fbx, fby, fbw, fbh);

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    if (vc->gfx.convert) {
        pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image,
                               NULL, vc->gfx.convert,
                               fbx, fby, 0, 0, fbx, fby, fbw, fbh);
    }

    wx1 = floor(fbx * vc->gfx.scale_x);
    wy1 = floor(fby * vc->gfx.scale_y);

    wx2 = ceil(fbx * vc->gfx.scale_x + fbw * vc->gfx.scale_x);
    wy2 = ceil(fby * vc->gfx.scale_y + fbh * vc->gfx.scale_y);

    ww_surface = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    wh_surface = surface_height(vc->gfx.ds) * vc->gfx.scale_y;

    win = gtk_widget_get_window(vc->gfx.drawing_area);
    if (!win) {
        return;
    }
    ww_widget = gdk_window_get_width(win);
    wh_widget = gdk_window_get_height(win);

    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    gtk_widget_queue_draw_area(vc->gfx.drawing_area,
                               wx_offset + wx1, wy_offset + wy1,
                               (wx2 - wx1), (wy2 - wy1));
}

static void gd_refresh(DisplayChangeListener *dcl)
{
    qemu_console_hw_update(dcl->con);
}

static GdkDevice *gd_get_pointer(GdkDisplay *dpy)
{
    return gdk_seat_get_pointer(gdk_display_get_default_seat(dpy));
}

static void gd_mouse_set(DisplayChangeListener *dcl,
                         int x, int y, bool visible)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkDisplay *dpy;
    gint x_root, y_root;

    if (!gtk_widget_get_realized(vc->gfx.drawing_area) ||
        qemu_input_is_absolute(dcl->con)) {
        return;
    }

    dpy = gtk_widget_get_display(vc->gfx.drawing_area);
    gdk_window_get_root_coords(gtk_widget_get_window(vc->gfx.drawing_area),
                               x, y, &x_root, &y_root);
    gdk_device_warp(gd_get_pointer(dpy),
                    gtk_widget_get_screen(vc->gfx.drawing_area),
                    x_root, y_root);
    vc->s->last_x = x;
    vc->s->last_y = y;
}

static void gd_cursor_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkPixbuf *pixbuf;
    GdkCursor *cursor;

    if (!gtk_widget_get_realized(vc->gfx.drawing_area)) {
        return;
    }

    pixbuf = gdk_pixbuf_new_from_data((guchar *)(c->data),
                                      GDK_COLORSPACE_RGB, true, 8,
                                      c->width, c->height, c->width * 4,
                                      NULL, NULL);
    cursor = gdk_cursor_new_from_pixbuf
        (gtk_widget_get_display(vc->gfx.drawing_area),
         pixbuf, c->hot_x, c->hot_y);
    gdk_window_set_cursor(gtk_widget_get_window(vc->gfx.drawing_area), cursor);
    g_object_unref(pixbuf);
    g_object_unref(cursor);
}

static void gd_switch(DisplayChangeListener *dcl,
                      DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.surface) {
        cairo_surface_destroy(vc->gfx.surface);
        vc->gfx.surface = NULL;
    }
    if (vc->gfx.convert) {
        pixman_image_unref(vc->gfx.convert);
        vc->gfx.convert = NULL;
    }

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }
    vc->gfx.ds = surface;

    if (surface_format(surface) == PIXMAN_x8r8g8b8) {
        /*
         * PIXMAN_x8r8g8b8 == CAIRO_FORMAT_RGB24
         *
         * No need to convert, use surface directly.  Should be the
         * common case as this is qemu_default_pixelformat(32) too.
         */
        vc->gfx.surface = cairo_image_surface_create_for_data
            (surface_data(surface),
             CAIRO_FORMAT_RGB24,
             surface_width(surface),
             surface_height(surface),
             surface_stride(surface));
    } else {
        /* Must convert surface, use pixman to do it. */
        vc->gfx.convert = pixman_image_create_bits(PIXMAN_x8r8g8b8,
                                                   surface_width(surface),
                                                   surface_height(surface),
                                                   NULL, 0);
        vc->gfx.surface = cairo_image_surface_create_for_data
            ((void *)pixman_image_get_data(vc->gfx.convert),
             CAIRO_FORMAT_RGB24,
             pixman_image_get_width(vc->gfx.convert),
             pixman_image_get_height(vc->gfx.convert),
             pixman_image_get_stride(vc->gfx.convert));
        pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image,
                               NULL, vc->gfx.convert,
                               0, 0, 0, 0, 0, 0,
                               pixman_image_get_width(vc->gfx.convert),
                               pixman_image_get_height(vc->gfx.convert));
    }

    if (resized) {
        gd_update_windowsize(vc);
    } else {
        gd_update_full_redraw(vc);
    }
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name             = "gtk",
    .dpy_gfx_update       = gd_update,
    .dpy_gfx_switch       = gd_switch,
    .dpy_gfx_check_format = qemu_pixman_check_format,
    .dpy_refresh          = gd_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,
};


#if defined(CONFIG_OPENGL)

static bool gd_has_dmabuf(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (gtk_use_gl_area && !gtk_widget_get_realized(vc->gfx.drawing_area)) {
        /* FIXME: Assume it will work, actual check done after realize */
        /* fixing this would require delaying listener registration */
        return true;
    }

    return vc->gfx.has_dmabuf;
}

static void gd_gl_release_dmabuf(DisplayChangeListener *dcl,
                                 QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    egl_dmabuf_release_texture(dmabuf);
    if (vc->gfx.guest_fb.dmabuf == dmabuf) {
        vc->gfx.guest_fb.dmabuf = NULL;
    }
#endif
}

void gd_hw_gl_flushed(void *vcon)
{
    VirtualConsole *vc = vcon;
    QemuDmaBuf *dmabuf = vc->gfx.guest_fb.dmabuf;
    int fence_fd;

    fence_fd = qemu_dmabuf_get_fence_fd(dmabuf);
    if (fence_fd >= 0) {
        qemu_set_fd_handler(fence_fd, NULL, NULL, NULL);
        close(fence_fd);
        qemu_dmabuf_set_fence_fd(dmabuf, -1);
        qemu_console_hw_gl_block(vc->gfx.dcl.con, false);
    }
}

/** DisplayState Callbacks (opengl version) **/

static const DisplayChangeListenerOps dcl_gl_area_ops = {
    .dpy_name             = "gtk-egl",
    .dpy_gfx_update       = gd_gl_area_update,
    .dpy_gfx_switch       = gd_gl_area_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh          = gd_gl_area_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_scanout_texture  = gd_gl_area_scanout_texture,
    .dpy_gl_scanout_disable  = gd_gl_area_scanout_disable,
    .dpy_gl_update           = gd_gl_area_scanout_flush,
    .dpy_gl_scanout_dmabuf   = gd_gl_area_scanout_dmabuf,
    .dpy_gl_release_dmabuf   = gd_gl_release_dmabuf,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

static bool
gd_gl_area_is_compatible_dcl(DisplayGLCtx *dgc,
                             DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_area_ops;
}

static const DisplayGLCtxOps gl_area_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gd_gl_area_is_compatible_dcl,
    .dpy_gl_ctx_create       = gd_gl_area_create_context,
    .dpy_gl_ctx_destroy      = gd_gl_area_destroy_context,
    .dpy_gl_ctx_make_current = gd_gl_area_make_current,
};

#ifdef CONFIG_X11
static const DisplayChangeListenerOps dcl_egl_ops = {
    .dpy_name             = "gtk-egl",
    .dpy_gfx_update       = gd_egl_update,
    .dpy_gfx_switch       = gd_egl_switch,
    .dpy_gfx_check_format = console_gl_check_format,
    .dpy_refresh          = gd_egl_refresh,
    .dpy_mouse_set        = gd_mouse_set,
    .dpy_cursor_define    = gd_cursor_define,

    .dpy_gl_scanout_disable  = gd_egl_scanout_disable,
    .dpy_gl_scanout_texture  = gd_egl_scanout_texture,
    .dpy_gl_scanout_dmabuf   = gd_egl_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = gd_egl_cursor_dmabuf,
    .dpy_gl_cursor_position  = gd_egl_cursor_position,
    .dpy_gl_update           = gd_egl_flush,
    .dpy_gl_release_dmabuf   = gd_gl_release_dmabuf,
    .dpy_has_dmabuf          = gd_has_dmabuf,
};

static bool
gd_egl_is_compatible_dcl(DisplayGLCtx *dgc,
                         DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_egl_ops;
}

static const DisplayGLCtxOps egl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = gd_egl_is_compatible_dcl,
    .dpy_gl_ctx_create       = gd_egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = gd_egl_make_current,
};
#endif

#endif /* CONFIG_OPENGL */

/** QEMU Events **/

static gboolean gd_quick_clear(void *opaque);
static void gd_sidebar_set_label(GtkWidget *button, const char *text);
static void gd_busy_idle(GtkDisplayState *s);

static void gd_change_runstate(void *opaque, bool running, RunState state)
{
    GtkDisplayState *s = opaque;

    gd_update_caption(s);
    if (s->sidebar_pause && (running || state == RUN_STATE_PAUSED)) {
        gd_sidebar_set_label(s->sidebar_pause, running ? "Pause" : "Resume");
        gd_busy_idle(s);            /* "Paused" on the screen, or nothing */
    }
    if (running && s->quick_boot && !s->quick_cleared) {
        s->quick_cleared = true;
        g_idle_add(gd_quick_clear, s);
    }
}

static void gd_mouse_mode_change(Notifier *notify, void *data)
{
    GtkDisplayState *s;
    int i;

    s = container_of(notify, GtkDisplayState, mouse_mode_notifier);
    /* release the grab at switching to absolute mode */
    if (s->ptr_owner && qemu_input_is_absolute(s->ptr_owner->gfx.dcl.con)) {
        if (!s->ptr_owner->window) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                           FALSE);
        } else {
            gd_ungrab_pointer(s);
        }
    }
    for (i = 0; i < s->vcs->len; i++) {
        VirtualConsole *vc = g_ptr_array_index(s->vcs, i);
        gd_update_cursor(vc);
    }
}

/** Side bar **/

/*
 * sidebar=on: the phone's buttons next to the guest display, for a guest
 * that takes F1-F4 as Home, Lock (side button), Volume down and Volume up
 * (ASoftware iOS does). Screenshots go to $ASOFT_SHOTS_DIR, default
 * Pictures/iOS Screenshots. Install .ipa copies the chosen files into
 * $ASOFT_IPA_DIR, where the guest fetches them, and shows the result the
 * guest leaves in Installed/<name>.log or Failed/<name>.log there.
 */
#define SIDEBAR_KEY_HOLD_MS 150

static void gd_sidebar_say(GtkDisplayState *s, const char *text)
{
    if (s->sidebar_status) {
        gtk_label_set_text(GTK_LABEL(s->sidebar_status), text);
    }
}

/* A side bar row's name (gd_sidebar_button keeps its label) */
static void gd_sidebar_set_label(GtkWidget *button, const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(button), "name")),
                       text);
}

/* Ask the guest to power down; the next request (button, close) quits. */
static void gd_powerdown(GtkDisplayState *s)
{
    s->powerdown_requested = true;
    qemu_system_powerdown_request();
    if (s->sidebar_power) {
        gd_sidebar_set_label(s->sidebar_power, "Force quit");
    }
    gd_sidebar_say(s, "Shutting down... the guest turns QEMU off in "
                   "about 10 s.");
}

/*
 * A guest whose machine has a usb-consumer (the iOS 26 research VM) takes the
 * phone's buttons from it as Consumer usages: F1 Home, F2 the side button
 * (Power), F3/F4 Volume down/up. The device releases the button itself.
 */
static const struct {
    unsigned int key;
    const char *button;
} gd_consumer_buttons[] = {
    { KEY_F1, "home" },
    { KEY_F2, "power" },
    { KEY_F3, "voldown" },
    { KEY_F4, "volup" },
};

static int gd_consumer_button(unsigned int key)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(gd_consumer_buttons); i++) {
        if (gd_consumer_buttons[i].key == key) {
            return i;
        }
    }
    return -1;
}

static Object *gd_consumer(void)
{
    return object_resolve_path_type("", "usb-consumer", NULL);
}

/* Press and release a key, as QMP send-key does, or the usb-consumer button. */
static void gd_sidebar_press(unsigned int key)
{
    Object *consumer = gd_consumer();
    int button = gd_consumer_button(key);
    Error *err = NULL;

    if (consumer && button >= 0) {
        if (!object_property_set_str(consumer, "press",
                                     gd_consumer_buttons[button].button, &err)) {
            error_report_err(err);
        }
        return;
    }
    qemu_input_event_send_key_linux(NULL, key, true);
    qemu_input_event_send_key_delay(SIDEBAR_KEY_HOLD_MS);
    qemu_input_event_send_key_linux(NULL, key, false);
    qemu_input_event_send_key_delay(SIDEBAR_KEY_HOLD_MS);
}

static void gd_sidebar_key(GtkButton *button, void *opaque)
{
    gd_sidebar_press(GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),
                                                        "key")));
}

/*
 * Screen off: ASoftware's kernel lights the guest keyboard's Scroll Lock LED
 * while iOS has the display off (xnu 0102), and keeps showing the last frame.
 * A cover over the guest display says so instead, and Lock becomes Wake. iOS
 * also turns the display off once while it boots, under the Apple logo: the
 * cover comes only once the display was on again after that.
 */
static gboolean gd_guest_display_update(void *opaque)
{
    GtkDisplayState *s = opaque;
    bool off = qemu_input_get_leds_mask(NULL) & QEMU_SCROLL_LOCK_LED;
    bool cover;

    if (off == s->guest_display_off) {
        return G_SOURCE_REMOVE;
    }
    s->guest_display_off = off;
    if (off) {
        s->guest_display_was_off = true;
    } else if (s->guest_display_was_off) {
        s->guest_display_armed = true;
    }
    cover = off && s->guest_display_armed;
    gtk_widget_set_visible(s->screen_off, cover);
    gd_sidebar_set_label(s->sidebar_lock, cover ? "Wake" : "Lock");
    gtk_widget_set_tooltip_text(s->sidebar_lock,
                                cover ? "Side button: wake (F2)"
                                      : "Side button: lock (F2)");
    return G_SOURCE_REMOVE;
}

/* from the vCPU thread that ran the guest's LED request: GTK on the main loop */
static void gd_guest_leds(Notifier *notifier, void *data)
{
    GtkDisplayState *s = container_of(notifier, GtkDisplayState, led_notifier);

    g_idle_add(gd_guest_display_update, s);
}

static gboolean gd_screen_off_clicked(GtkWidget *widget, GdkEventButton *event,
                                      void *opaque)
{
    gd_sidebar_press(KEY_F2);
    return TRUE;
}

static GdkPixbuf *gd_asoft_logo(int size);

static void gd_create_screen_off(GtkDisplayState *s)
{
    GtkCssProvider *css = gtk_css_provider_new();
    GtkWidget *label = gtk_label_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    g_autoptr(GdkPixbuf) logo = gd_asoft_logo(72);

    s->screen_off = gtk_event_box_new();
    gtk_widget_set_name(s->screen_off, "screen-off");
    gtk_css_provider_load_from_data(css, "#screen-off { background-color: "
                                    "#000000; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(s->screen_off),
                                   GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    if (logo) {
        GtkWidget *image = gtk_image_new_from_pixbuf(logo);

        gtk_widget_set_opacity(image, 0.5);
        gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    }
    gtk_label_set_markup(GTK_LABEL(label),
                         "<span foreground='#ffffff' size='x-large'>"
                         "Screen off</span>\n\n"
                         "<span foreground='#8e8e93'>Click here, or press "
                         "Wake or Home</span>");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(s->screen_off), box);
    gtk_widget_show_all(box);
    /* shown by gd_guest_display_update only, not by gtk_widget_show_all */
    gtk_widget_set_no_show_all(s->screen_off, TRUE);
    g_signal_connect(s->screen_off, "button-press-event",
                     G_CALLBACK(gd_screen_off_clicked), s);

    s->led_notifier.notify = gd_guest_leds;
    qemu_input_led_notifier_add(&s->led_notifier);
}

static void gd_sidebar_screenshot(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *name = NULL, *path = NULL, *said = NULL;
    g_autoptr(GError) err = NULL;
    pixman_image_t *rgba;
    GdkPixbuf *pixbuf;
    int width, height;
    gboolean ok;

    if (!vc || vc->type != GD_VC_GFX || !vc->gfx.ds) {
        gd_sidebar_say(s, "No screen to save yet.");
        return;
    }
    width = surface_width(vc->gfx.ds);
    height = surface_height(vc->gfx.ds);
    /* R, G, B, A bytes, as GdkPixbuf wants them */
    rgba = pixman_image_create_bits(PIXMAN_a8b8g8r8, width, height, NULL,
                                    width * 4);
    pixman_image_composite(PIXMAN_OP_SRC, vc->gfx.ds->image, NULL, rgba,
                           0, 0, 0, 0, 0, 0, width, height);
    pixbuf = gdk_pixbuf_new_from_data(
        (const guchar *)pixman_image_get_data(rgba), GDK_COLORSPACE_RGB,
        TRUE, 8, width, height, pixman_image_get_stride(rgba), NULL, NULL);

    name = g_date_time_format(now, "iOS %Y-%m-%d %H.%M.%S.png");
    path = g_build_filename(s->shots_dir, name, NULL);
    g_mkdir_with_parents(s->shots_dir, 0755);
    ok = gdk_pixbuf_save(pixbuf, path, "png", &err,
                         "tEXt::Software", "ASoftware iOS Emulator", NULL);
    g_object_unref(pixbuf);
    pixman_image_unref(rgba);

    said = ok ? g_strdup_printf("Saved %s", name)
              : g_strdup_printf("Could not save %s: %s", path, err->message);
    gd_sidebar_say(s, said);
}

static void gd_sidebar_open_shots(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;
    g_autofree char *uri = NULL, *said = NULL;
    g_autoptr(GError) err = NULL;

    g_mkdir_with_parents(s->shots_dir, 0755);
    uri = g_filename_to_uri(s->shots_dir, NULL, &err);
    if (!uri || !gtk_show_uri_on_window(GTK_WINDOW(s->window), uri,
                                        GDK_CURRENT_TIME, &err)) {
        said = g_strdup_printf("Could not open %s: %s", s->shots_dir,
                               err ? err->message : "?");
        gd_sidebar_say(s, said);
    }
}

/* Every 2 s while .ipa files wait: has the guest left a result? */
static gboolean gd_sidebar_poll_ipas(void *opaque)
{
    static const char *const results[] = { "Installed", "Failed" };
    GtkDisplayState *s = opaque;
    GHashTableIter iter;
    gpointer name, since;
    size_t i;

    g_hash_table_iter_init(&iter, s->ipas_pending);
    while (g_hash_table_iter_next(&iter, &name, &since)) {
        for (i = 0; i < ARRAY_SIZE(results); i++) {
            g_autofree char *log = g_strdup_printf("%s.log", (char *)name);
            g_autofree char *path = g_build_filename(s->ipa_dir, results[i],
                                                     log, NULL);
            g_autofree char *text = NULL, *said = NULL;
            GStatBuf st;

            if (g_stat(path, &st) == 0 &&
                st.st_mtime + 1 >= (time_t)GPOINTER_TO_SIZE(since) &&
                g_file_get_contents(path, &text, NULL, NULL)) {
                said = g_strdup_printf("%s: %.300s", (char *)name,
                                       g_strstrip(text));
                gd_sidebar_say(s, said);
                g_hash_table_iter_remove(&iter);
                break;
            }
        }
    }
    if (g_hash_table_size(s->ipas_pending) == 0) {
        s->ipas_poll = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

typedef struct SidebarCopy {
    GtkDisplayState *s;
    char *name;
    time_t started;
} SidebarCopy;

static void gd_sidebar_copied(GObject *source, GAsyncResult *res,
                              void *opaque)
{
    SidebarCopy *copy = opaque;
    GtkDisplayState *s = copy->s;
    g_autoptr(GError) err = NULL;
    g_autofree char *said = NULL;

    if (!g_file_copy_finish(G_FILE(source), res, &err)) {
        said = g_strdup_printf("Could not copy %s: %s", copy->name,
                               err->message);
    } else {
        g_hash_table_insert(s->ipas_pending, g_strdup(copy->name),
                            GSIZE_TO_POINTER((gsize)copy->started));
        if (!s->ipas_poll) {
            s->ipas_poll = g_timeout_add_seconds(2, gd_sidebar_poll_ipas, s);
        }
        said = g_strdup_printf("Installing %s... the guest picks it up "
                               "within about 10 s.", copy->name);
    }
    gd_sidebar_say(s, said);
    g_free(copy->name);
    g_free(copy);
}

static void gd_sidebar_ipa_chosen(GtkNativeDialog *dialog, gint response,
                                  void *opaque)
{
    GtkDisplayState *s = opaque;
    GSList *files, *f;

    if (response != GTK_RESPONSE_ACCEPT) {
        g_object_unref(dialog);
        return;
    }
    g_mkdir_with_parents(s->ipa_dir, 0755);
    files = gtk_file_chooser_get_files(GTK_FILE_CHOOSER(dialog));
    for (f = files; f; f = f->next) {
        SidebarCopy *copy = g_new0(SidebarCopy, 1);
        g_autoptr(GFile) dest = NULL;

        copy->s = s;
        copy->name = g_file_get_basename(f->data);
        copy->started = time(NULL);
        dest = g_file_new_build_filename(s->ipa_dir, copy->name, NULL);
        /* off the main loop: an .ipa can be hundreds of MB */
        g_file_copy_async(f->data, dest, G_FILE_COPY_OVERWRITE,
                          G_PRIORITY_DEFAULT, NULL, NULL, NULL,
                          gd_sidebar_copied, copy);
    }
    g_slist_free_full(files, g_object_unref);
    g_object_unref(dialog);
}

static void gd_sidebar_install(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;
    GtkFileChooserNative *dialog;
    GtkFileFilter *filter;

    /* not gtk_dialog_run(): its nested loop would hold the BQL */
    dialog = gtk_file_chooser_native_new("Install .ipa",
                                         GTK_WINDOW(s->window),
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Install", "_Cancel");
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), TRUE);
    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "iOS apps (*.ipa)");
    gtk_file_filter_add_pattern(filter, "*.ipa");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    g_signal_connect(dialog, "response", G_CALLBACK(gd_sidebar_ipa_chosen), s);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

static void gd_sidebar_quit_answered(GtkDialog *dialog, gint response,
                                     void *opaque)
{
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response == GTK_RESPONSE_YES) {
        qmp_quit(NULL);
    }
}

static void gd_sidebar_power(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;
    GtkWidget *dialog;

    if (!s->powerdown_requested) {
        gd_powerdown(s);
        return;
    }
    dialog = gtk_message_dialog_new(GTK_WINDOW(s->window),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                    "Quit QEMU now, without letting the "
                                    "guest shut down?");
    g_signal_connect(dialog, "response",
                     G_CALLBACK(gd_sidebar_quit_answered), s);
    gtk_widget_show(dialog);
}

/** Side bar: gestures, pasted text, pause, saved states (qemu/patches/0009) **/

/*
 * Touch gestures made on the guest's tablet, for what a mouse cannot do: a
 * queue of steps (where the finger is, as fractions of the guest display,
 * whether it touches, how long until the next step) played from a timer.
 */
typedef struct GdGestureStep {
    double x, y;
    bool down;
    guint ms;
} GdGestureStep;

static void gd_gesture_add(GtkDisplayState *s, double x, double y, bool down,
                           guint ms)
{
    GdGestureStep *step = g_new(GdGestureStep, 1);

    *step = (GdGestureStep){ CLAMP(x, 0, 0.999), CLAMP(y, 0, 0.999), down, ms };
    if (!s->gesture) {
        s->gesture = g_queue_new();
    }
    g_queue_push_tail(s->gesture, step);
}

static void gd_gesture_send(GtkDisplayState *s, double x, double y, bool down)
{
    VirtualConsole *vc = gd_vc_find_current(s);
    int width, height;

    if (!vc || vc->type != GD_VC_GFX || !vc->gfx.ds) {
        return;
    }
    width = surface_width(vc->gfx.ds);
    height = surface_height(vc->gfx.ds);
    qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_X, x * width, 0, width);
    qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_Y, y * height, 0, height);
    if (down != s->gesture_down) {
        qemu_input_queue_btn(vc->gfx.dcl.con, INPUT_BUTTON_LEFT, down);
        s->gesture_down = down;
    }
    qemu_input_event_sync();
}

static void gd_wheel_swipe(GtkDisplayState *s);

static gboolean gd_gesture_tick(void *opaque)
{
    GtkDisplayState *s = opaque;
    GdGestureStep *step = g_queue_pop_head(s->gesture);

    s->gesture_timer = 0;
    if (!step) {
        gd_wheel_swipe(s);          /* wheel steps that came in meanwhile */
        return G_SOURCE_REMOVE;
    }
    gd_gesture_send(s, step->x, step->y, step->down);
    s->gesture_timer = g_timeout_add(MAX(step->ms, 1), gd_gesture_tick, s);
    g_free(step);
    return G_SOURCE_REMOVE;
}

static void gd_gesture_run(GtkDisplayState *s)
{
    if (!s->gesture_timer && s->gesture && !g_queue_is_empty(s->gesture)) {
        s->gesture_timer = g_timeout_add(1, gd_gesture_tick, s);
    }
}

/* The real mouse button goes down: the gesture under way stops, lifted. */
static void gd_gesture_abort(GtkDisplayState *s)
{
    if (s->gesture) {
        g_queue_clear_full(s->gesture, g_free);
    }
    if (s->gesture_timer) {
        g_source_remove(s->gesture_timer);
        s->gesture_timer = 0;
    }
    if (s->gesture_down) {
        VirtualConsole *vc = gd_vc_find_current(s);

        if (vc && vc->type == GD_VC_GFX) {
            qemu_input_queue_btn(vc->gfx.dcl.con, INPUT_BUTTON_LEFT, false);
            qemu_input_event_sync();
        }
        s->gesture_down = false;
    }
    s->wheel_pending = 0;
}

/*
 * The mouse wheel as a touch swipe where the pointer is: iOS has no wheel.
 * A wheel step moves the content WHEEL_SWIPE_STEP of the display; the swipe
 * starts away from the edges, where a touch would be an edge gesture.
 */
#define WHEEL_SWIPE_STEP 0.06

static void gd_wheel_swipe(GtkDisplayState *s)
{
    VirtualConsole *vc = gd_vc_find_current(s);
    double x, y, y1, dist;
    int i;

    if (s->gesture_timer || s->mouse_down || s->wheel_pending == 0 ||
        !vc || vc->type != GD_VC_GFX || !vc->gfx.ds) {
        return;
    }
    dist = CLAMP(s->wheel_pending * WHEEL_SWIPE_STEP, -0.4, 0.4);
    s->wheel_pending = 0;
    x = CLAMP((double)s->last_x / surface_width(vc->gfx.ds), 0.08, 0.92);
    y = CLAMP((double)s->last_y / surface_height(vc->gfx.ds), 0.1, 0.9);
    /* wheel down shows what is below: the finger moves up */
    y1 = y - dist;
    if (y1 < 0.1) {
        y += 0.1 - y1;
        y1 = 0.1;
    } else if (y1 > 0.9) {
        y -= y1 - 0.9;
        y1 = 0.9;
    }
    gd_gesture_add(s, x, y, true, 16);
    for (i = 1; i <= 8; i++) {
        gd_gesture_add(s, x, y + (y1 - y) * i / 8, true, 16);
    }
    gd_gesture_add(s, x, y1, false, 40);
    gd_gesture_run(s);
}

/*
 * iOS's app switcher: up from the bottom edge, then a pause before the lift.
 * The pause is reported as touches that stay put: with no reports at all
 * SpringBoard kept the last speed and went home instead.
 */
static void gd_sidebar_switcher(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;
    int i;

    if (s->gesture_timer) {
        return;
    }
    /* measured under TCG: 35 % of the height in 0.8 s, then 1.5 s still */
    gd_gesture_add(s, 0.5, 0.995, true, 40);
    for (i = 1; i <= 20; i++) {
        gd_gesture_add(s, 0.5, 0.995 - 0.35 * i / 20, true, 40);
    }
    for (i = 0; i < 75; i++) {
        gd_gesture_add(s, 0.5 + (i & 1) * 0.0005, 0.645, true, 20);
    }
    gd_gesture_add(s, 0.5, 0.645, false, 30);
    gd_gesture_run(s);
}

/*
 * Text from the PC's clipboard, typed on the guest's keyboard one character
 * every PASTE_INTERVAL_MS. A US layout: printable ASCII, tabs and new lines;
 * anything else is left out.
 */
#define PASTE_INTERVAL_MS 30
#define PASTE_MAX 4000

static bool gd_ascii_key(char c, unsigned int *key, bool *shift)
{
    static const char lower[] = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
    static const char upper[] = "~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";
    static const unsigned char keys[] = {
        KEY_GRAVE, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8,
        KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL,
        KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
        KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH,
        KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
        KEY_SEMICOLON, KEY_APOSTROPHE,
        KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
        KEY_COMMA, KEY_DOT, KEY_SLASH,
    };
    const char *p;

    QEMU_BUILD_BUG_ON(sizeof(lower) - 1 != ARRAY_SIZE(keys));
    QEMU_BUILD_BUG_ON(sizeof(upper) - 1 != ARRAY_SIZE(keys));
    *shift = false;
    switch (c) {
    case ' ':
        *key = KEY_SPACE;
        return true;
    case '\n':
        *key = KEY_ENTER;
        return true;
    case '\t':
        *key = KEY_TAB;
        return true;
    case '\0':
        return false;
    }
    if ((p = strchr(lower, c))) {
        *key = keys[p - lower];
        return true;
    }
    if ((p = strchr(upper, c))) {
        *key = keys[p - upper];
        *shift = true;
        return true;
    }
    return false;
}

static gboolean gd_paste_tick(void *opaque)
{
    GtkDisplayState *s = opaque;
    unsigned int key;
    bool shift;

    while (s->paste[s->paste_pos]) {
        if (!gd_ascii_key(s->paste[s->paste_pos++], &key, &shift)) {
            continue;
        }
        if (shift) {
            qemu_input_event_send_key_linux(NULL, KEY_LEFTSHIFT, true);
        }
        qemu_input_event_send_key_linux(NULL, key, true);
        qemu_input_event_send_key_delay(10);
        qemu_input_event_send_key_linux(NULL, key, false);
        if (shift) {
            qemu_input_event_send_key_linux(NULL, KEY_LEFTSHIFT, false);
        }
        qemu_input_event_send_key_delay(10);
        return G_SOURCE_CONTINUE;
    }
    g_clear_pointer(&s->paste, g_free);
    s->paste_timer = 0;
    return G_SOURCE_REMOVE;
}

static void gd_paste_received(GtkClipboard *clipboard, const char *text,
                              void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);
    g_autofree char *said = NULL;
    unsigned int key;
    bool shift;
    size_t i, typed = 0, left_out = 0;

    if (!text || !*text) {
        gd_sidebar_say(s, "The PC's clipboard has no text.");
        return;
    }
    if (s->paste_timer) {
        g_source_remove(s->paste_timer);
        g_free(s->paste);
    }
    s->paste = g_strndup(text, PASTE_MAX);
    s->paste_pos = 0;
    for (i = 0; s->paste[i]; i++) {
        if (gd_ascii_key(s->paste[i], &key, &shift)) {
            typed++;
        } else if (s->paste[i] != '\r' && ((unsigned char)s->paste[i] & 0xc0) != 0x80) {
            left_out++;     /* a character, counted once (not a UTF-8 tail) */
        }
    }
    /* Ctrl, still held for Ctrl+V, would make every character a shortcut */
    if (vc && vc->type == GD_VC_GFX) {
        qkbd_state_lift_all_keys(vc->gfx.kbd);
    }
    s->paste_timer = g_timeout_add(PASTE_INTERVAL_MS, gd_paste_tick, s);
    said = left_out ? g_strdup_printf("Typing %zu characters; %zu left out "
                                      "(only English letters, digits and "
                                      "symbols).", typed, left_out)
                    : g_strdup_printf("Typing %zu characters.", typed);
    gd_sidebar_say(s, said);
}

static void gd_sidebar_paste(GtkButton *button, void *opaque)
{
    gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
                               gd_paste_received, opaque);
}

static void gd_sidebar_full_screen(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;

    gtk_menu_item_activate(GTK_MENU_ITEM(s->full_screen_item));
}

static void gd_sidebar_pause(GtkButton *button, void *opaque)
{
    Error *err = NULL;

    if (runstate_is_running()) {
        qmp_stop(NULL);
    } else if (runstate_check(RUN_STATE_PAUSED)) {
        qmp_cont(&err);
        error_free(err);
    }
}

/*
 * A cover over the guest display that says what the machine is doing: paused
 * (a click goes on), or saving or restoring a state (a spinner).
 */
static gboolean gd_busy_clicked(GtkWidget *widget, GdkEventButton *event,
                                void *opaque)
{
    if (runstate_check(RUN_STATE_PAUSED)) {
        gd_sidebar_pause(NULL, opaque);
    }
    return TRUE;
}

static void gd_create_busy(GtkDisplayState *s)
{
    GtkCssProvider *css = gtk_css_provider_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);

    s->busy = gtk_event_box_new();
    gtk_widget_set_name(s->busy, "asoft-busy");
    gtk_css_provider_load_from_data(css,
        "#asoft-busy { background-color: rgba(0, 0, 0, 0.72); }"
        "#asoft-busy .title { color: #ffffff; font-size: 20px; font-weight: 600; }"
        "#asoft-busy .text { color: #8e8e93; font-size: 13px; }"
        "#asoft-busy spinner { color: #ffffff; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    s->busy_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(s->busy_spinner, 36, 36);
    s->busy_title = gtk_label_new(NULL);
    s->busy_text = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(s->busy_text), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap(GTK_LABEL(s->busy_text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(s->busy_text), 30);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->busy_title), "title");
    gtk_style_context_add_class(gtk_widget_get_style_context(s->busy_text), "text");
    gtk_box_pack_start(GTK_BOX(box), s->busy_spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->busy_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), s->busy_text, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(s->busy), box);
    gtk_widget_show_all(box);
    gtk_widget_set_no_show_all(s->busy, TRUE);
    g_signal_connect(s->busy, "button-press-event",
                     G_CALLBACK(gd_busy_clicked), s);
}

static void gd_busy_show(GtkDisplayState *s, const char *title,
                         const char *text, bool spinning)
{
    if (!s->busy) {
        return;
    }
    gtk_label_set_text(GTK_LABEL(s->busy_title), title);
    gtk_label_set_text(GTK_LABEL(s->busy_text), text);
    gtk_widget_set_visible(s->busy_spinner, spinning);
    if (spinning) {
        gtk_spinner_start(GTK_SPINNER(s->busy_spinner));
    } else {
        gtk_spinner_stop(GTK_SPINNER(s->busy_spinner));
    }
    gtk_widget_show(s->busy);
}

static void gd_busy_hide(GtkDisplayState *s)
{
    if (s->busy) {
        gtk_spinner_stop(GTK_SPINNER(s->busy_spinner));
        gtk_widget_hide(s->busy);
    }
}

/* What the cover shows when nothing is being saved: Paused, or nothing. */
static void gd_busy_idle(GtkDisplayState *s)
{
    if (s->snapshot_busy) {
        return;
    }
    if (runstate_check(RUN_STATE_PAUSED)) {
        gd_busy_show(s, "Paused", "Click here or press Resume to go on.", false);
    } else {
        gd_busy_hide(s);
    }
}

/*
 * Saved states: internal snapshots of the machine (devices, memory, disks) in
 * its qcow2 disk, named "state-<time>". QEMU saves and loads them in its main
 * loop, so the window stands still meanwhile: the cover says so first.
 */
#define QUICK_BOOT_SNAPSHOT "asoft-quick"

typedef struct GdSnapshotJob {
    GtkDisplayState *s;
    char *name;
    int what;           /* 0 save, 1 load, 2 delete, 3 save and quit */
} GdSnapshotJob;

static gboolean gd_snapshot_run(void *opaque)
{
    GdSnapshotJob *job = opaque;
    GtkDisplayState *s = job->s;
    Error *err = NULL;
    g_autofree char *said = NULL;
    RunState state;
    bool ok;

    switch (job->what) {
    case 1:
        state = runstate_get();
        vm_stop(RUN_STATE_RESTORE_VM);
        ok = load_snapshot(job->name, NULL, false, NULL, &err);
        if (ok) {
            load_snapshot_resume(state);
        }
        break;
    case 2:
        ok = delete_snapshot(job->name, false, NULL, &err);
        break;
    case 3:
        /* stopped first, so the guest does not run on after the snapshot */
        state = runstate_get();
        vm_stop(RUN_STATE_PAUSED);
        ok = save_snapshot(job->name, true, NULL, false, NULL, &err);
        if (ok) {
            qmp_quit(NULL);         /* the cover stays until the end */
            g_free(job->name);
            g_free(job);
            return G_SOURCE_REMOVE;
        }
        if (state == RUN_STATE_RUNNING) {
            vm_start();
        }
        break;
    default:
        ok = save_snapshot(job->name, true, NULL, false, NULL, &err);
        break;
    }
    said = ok ? g_strdup(job->what == 0 ? "State saved." :
                         job->what == 1 ? "State restored." : "State deleted.")
              : g_strdup_printf(job->what == 3 ? "Could not save the state (%s);"
                                " shutting down instead." : "Failed: %s",
                                error_get_pretty(err));
    gd_sidebar_say(s, said);
    s->snapshot_busy = false;
    gd_busy_idle(s);
    if (!ok && job->what == 3) {
        gd_powerdown(s);
    }
    error_free(err);
    g_free(job->name);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* After 150 ms, so that the cover is drawn before the window stands still. */
static void gd_snapshot_job(GtkDisplayState *s, const char *name, int what,
                            const char *title)
{
    GdSnapshotJob *job;

    if (s->snapshot_busy) {
        return;
    }
    job = g_new0(GdSnapshotJob, 1);
    job->s = s;
    job->name = g_strdup(name);
    job->what = what;
    s->snapshot_busy = true;
    gd_sidebar_say(s, title);
    gd_busy_show(s, title, what == 3 ? "Closing when it is saved." :
                 "This takes a moment; the window stands still meanwhile.",
                 true);
    g_timeout_add(150, gd_snapshot_run, job);
}

static void gd_sidebar_save_state(GtkButton *button, void *opaque)
{
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *name = g_date_time_format(now, "state-%Y%m%d-%H%M%S");

    gd_snapshot_job(opaque, name, 0, "Saving the state…");
}

static void gd_states_answered(GtkDialog *dialog, gint response, void *opaque)
{
    GtkDisplayState *s = opaque;
    GtkListBox *list = g_object_get_data(G_OBJECT(dialog), "list");
    GtkListBoxRow *row = gtk_list_box_get_selected_row(list);
    const char *name = row ? g_object_get_data(G_OBJECT(row), "name") : NULL;

    if (name && response == 1) {
        gd_snapshot_job(s, name, 1, "Restoring the state…");
    } else if (name && response == 2) {
        gd_snapshot_job(s, name, 2, "Deleting the state…");
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void gd_sidebar_states(GtkButton *button, void *opaque)
{
    GtkDisplayState *s = opaque;
    QEMUSnapshotInfo *sn_tab = NULL;
    BlockDriverState *bs;
    GtkWidget *dialog, *list, *content;
    Error *err = NULL;
    int i, n = 0, shown = 0;

    {
        GRAPH_RDLOCK_GUARD_MAINLOOP();

        bs = bdrv_all_find_vmstate_bs(NULL, false, NULL, &err);
        if (bs) {
            n = bdrv_snapshot_list(bs, &sn_tab);
        }
    }
    if (!bs) {
        g_autofree char *said = g_strdup_printf("No saved states here: %s",
                                                error_get_pretty(err));
        gd_sidebar_say(s, said);
        error_free(err);
        return;
    }
    /* the quick boot's own state is not one of them */
    for (i = 0; i < n; i++) {
        shown += strcmp(sn_tab[i].name, QUICK_BOOT_SNAPSHOT) != 0;
    }
    if (!shown) {
        g_free(sn_tab);
        gd_sidebar_say(s, "No saved states yet: Save state makes one.");
        return;
    }

    dialog = gtk_dialog_new_with_buttons("Saved states", GTK_WINDOW(s->window),
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "_Delete", 2, "_Cancel",
                                         GTK_RESPONSE_CANCEL, "_Restore", 1,
                                         NULL);
    list = gtk_list_box_new();
    for (i = n - 1; i >= 0; i--) {     /* the newest first */
        g_autoptr(GDateTime) when = NULL;
        g_autofree char *date = NULL, *text = NULL;
        GtkWidget *label;

        if (!strcmp(sn_tab[i].name, QUICK_BOOT_SNAPSHOT)) {
            continue;
        }
        when = g_date_time_new_from_unix_local(sn_tab[i].date_sec);
        date = g_date_time_format(when, "%d %b %Y, %H:%M:%S");
        text = g_strdup_printf("%s  ·  %.1f GB", date,
                               sn_tab[i].vm_state_size / (double)(1 << 30));
        label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_margin_start(label, 10);
        gtk_widget_set_margin_end(label, 10);
        gtk_widget_set_margin_top(label, 6);
        gtk_widget_set_margin_bottom(label, 6);
        gtk_list_box_insert(GTK_LIST_BOX(list), label, -1);
        g_object_set_data_full(G_OBJECT(gtk_widget_get_parent(label)), "name",
                               g_strdup(sn_tab[i].name), g_free);
    }
    g_free(sn_tab);
    gtk_list_box_select_row(GTK_LIST_BOX(list),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 8);
    gtk_box_pack_start(GTK_BOX(content), list, TRUE, TRUE, 0);
    g_object_set_data(G_OBJECT(dialog), "list", list);
    g_signal_connect(dialog, "response", G_CALLBACK(gd_states_answered), s);
    gtk_widget_set_size_request(dialog, 320, -1);
    gtk_widget_show_all(dialog);
}

/*
 * Quick boot: closing the window saves the machine as the internal snapshot
 * QUICK_BOOT_SNAPSHOT and quits (gd_snapshot_run, what 3); the launcher starts
 * it next time with -loadvm of it. Once that machine runs, the snapshot goes:
 * the disk goes on from there, so a state is never resumed twice.
 */
static gboolean gd_quick_clear(void *opaque)
{
    GtkDisplayState *s = opaque;
    Error *err = NULL;

    if (bdrv_all_has_snapshot(QUICK_BOOT_SNAPSHOT, false, NULL, &err) == 1 &&
        delete_snapshot(QUICK_BOOT_SNAPSHOT, false, NULL, &err)) {
        gd_sidebar_say(s, "Resumed where it was closed.");
    }
    if (err) {
        g_autofree char *said = g_strdup_printf("The quick boot state stays: "
                                                "%s", error_get_pretty(err));
        gd_sidebar_say(s, said);
        error_free(err);
    }
    return G_SOURCE_REMOVE;
}

/*
 * ASoftware's look: its logo (asoftware.png next to the executable) as the window
 * icon and atop the side bar, which follows the dark style of the iOS it shows:
 * black, rows grouped as in Settings, an action in blue and power in red.
 */
static GdkPixbuf *gd_asoft_logo(int size)
{
#ifdef G_OS_WIN32
    /* the executable's folder (CONFIG_BINDIR is no path there) */
    g_autofree char *dir = g_win32_get_package_installation_directory_of_module(NULL);
#else
    g_autofree char *dir = get_relocated_path(CONFIG_BINDIR);
#endif
    g_autofree char *path = g_build_filename(dir, "asoftware.png", NULL);

    return gdk_pixbuf_new_from_file_at_size(path, size, size, NULL);
}

/* iOS's dark system colors: grouped background, separator, labels, blue, red */
static const char asoft_sidebar_css[] =
    "window.background, #asoft-sidebar { background-color: #000000; }"
    "#asoft-sidebar { padding: 14px 12px; }"
    "#asoft-sidebar .brand { color: #ffffff; font-weight: 600; font-size: 14px; }"
    "#asoft-sidebar .subtitle { color: #8e8e93; font-size: 11px; }"
    "#asoft-sidebar .caption { color: #8e8e93; font-size: 10px; margin: 14px 12px 5px 12px; }"
    "#asoft-sidebar .group { background-color: #1c1c1e; border-radius: 10px; }"
    "#asoft-sidebar .group button { background-image: none; background-color: transparent;"
    "  border: none; border-radius: 0; box-shadow: none; text-shadow: none;"
    "  -gtk-icon-shadow: none; padding: 8px 12px; min-height: 0; }"
    "#asoft-sidebar .group button + button { border-top: 1px solid #38383a; }"
    "#asoft-sidebar .group button:first-child { border-radius: 10px 10px 0 0; }"
    "#asoft-sidebar .group button:last-child { border-radius: 0 0 10px 10px; }"
    "#asoft-sidebar .group button:only-child { border-radius: 10px; }"
    "#asoft-sidebar .group button:hover { background-color: #2c2c2e; }"
    "#asoft-sidebar .group button:active { background-color: #3a3a3c; }"
    "#asoft-sidebar .group .name { color: #ffffff; font-size: 13px; }"
    "#asoft-sidebar .group .hint { color: #8e8e93; font-size: 12px; }"
    "#asoft-sidebar .group button.action .name { color: #0a84ff; }"
    "#asoft-sidebar .group button.destructive .name { color: #ff453a; }"
    "#asoft-sidebar .group button:disabled .name,"
    "#asoft-sidebar .group button:disabled .hint { color: #48484a; }"
    "#asoft-sidebar .status { color: #8e8e93; font-size: 11px; margin: 12px 6px 0 6px; }";

/* A captioned group of rows, as in iOS Settings */
static GtkWidget *gd_sidebar_group(GtkDisplayState *s, const char *caption)
{
    GtkWidget *label = gtk_label_new(caption);
    GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "caption");
    gtk_style_context_add_class(gtk_widget_get_style_context(group), "group");
    gtk_box_pack_start(GTK_BOX(s->sidebar), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->sidebar), group, FALSE, FALSE, 0);
    return group;
}

/* A row: its name on the left, the key that does the same (if any) on the right */
static GtkWidget *gd_sidebar_button(GtkDisplayState *s, GtkWidget *group,
                                    const char *label, const char *hint,
                                    const char *tooltip, GCallback clicked,
                                    unsigned int key)
{
    GtkWidget *button = gtk_button_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name = gtk_label_new(label);

    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(name), "name");
    gtk_box_pack_start(GTK_BOX(row), name, TRUE, TRUE, 0);
    if (hint) {
        GtkWidget *keys = gtk_label_new(hint);

        gtk_style_context_add_class(gtk_widget_get_style_context(keys), "hint");
        gtk_box_pack_end(GTK_BOX(row), keys, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(button), row);
    g_object_set_data(G_OBJECT(button), "name", name);

    /* keep the keyboard focus on the guest display */
    gtk_widget_set_can_focus(button, FALSE);
    gtk_widget_set_tooltip_text(button, tooltip);
    g_object_set_data(G_OBJECT(button), "key", GUINT_TO_POINTER(key));
    g_signal_connect(button, "clicked", clicked, s);
    gtk_box_pack_start(GTK_BOX(group), button, FALSE, FALSE, 0);
    return button;
}

static void gd_create_sidebar(GtkDisplayState *s)
{
    const char *shots = g_getenv("ASOFT_SHOTS_DIR");
    const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    GtkWidget *group, *install, *header, *names, *brand, *subtitle, *save, *states;
    GtkCssProvider *css = gtk_css_provider_new();
    Error *blocked = NULL;
    g_autoptr(GdkPixbuf) logo = gd_asoft_logo(38);

    s->shots_dir = shots ? g_strdup(shots)
        : g_build_filename(pictures ? pictures : g_get_home_dir(),
                           "iOS Screenshots", NULL);
    s->ipa_dir = g_strdup(g_getenv("ASOFT_IPA_DIR"));
    s->ipas_pending = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);

    /* dark menus and dialogs to go with the side bar */
    g_object_set(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", TRUE, NULL);
    gtk_css_provider_load_from_data(css, asoft_sidebar_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    s->sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(s->sidebar, "asoft-sidebar");    /* padding in its CSS */
    gtk_widget_set_size_request(s->sidebar, 184, -1);

    /* logo and name */
    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    if (logo) {
        gtk_box_pack_start(GTK_BOX(header), gtk_image_new_from_pixbuf(logo),
                           FALSE, FALSE, 0);
    }
    names = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_valign(names, GTK_ALIGN_CENTER);
    brand = gtk_label_new("ASoftware");
    subtitle = gtk_label_new(g_getenv("ASOFT_SUBTITLE") ?: "iOS 16.7.16 · iPhone X");
    gtk_label_set_xalign(GTK_LABEL(brand), 0);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(brand), "brand");
    gtk_style_context_add_class(gtk_widget_get_style_context(subtitle),
                                "subtitle");
    gtk_box_pack_start(GTK_BOX(names), brand, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(names), subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), names, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(s->sidebar), header, FALSE, FALSE, 0);

    group = gd_sidebar_group(s, "BUTTONS");
    gd_sidebar_button(s, group, "Home", "F1", "Home (F1)",
                      G_CALLBACK(gd_sidebar_key), KEY_F1);
    s->sidebar_lock = gd_sidebar_button(s, group, "Lock", "F2",
                                        "Side button: lock (F2)",
                                        G_CALLBACK(gd_sidebar_key), KEY_F2);
    gd_sidebar_button(s, group, "App switcher", NULL,
                      "Swipe up from the bottom edge and hold",
                      G_CALLBACK(gd_sidebar_switcher), 0);
    gd_sidebar_button(s, group, "Volume up", "F4", "Volume up (F4)",
                      G_CALLBACK(gd_sidebar_key), KEY_F4);
    gd_sidebar_button(s, group, "Volume down", "F3", "Volume down (F3)",
                      G_CALLBACK(gd_sidebar_key), KEY_F3);

    group = gd_sidebar_group(s, "SCREEN");
    gd_sidebar_button(s, group, "Screenshot", "Ctrl+S",
                      "Save the screen as a PNG (Ctrl+S)",
                      G_CALLBACK(gd_sidebar_screenshot), 0);
    gd_sidebar_button(s, group, "Full screen", "F11", "Full screen (F11)",
                      G_CALLBACK(gd_sidebar_full_screen), 0);
    gd_sidebar_button(s, group, "Screenshots folder", NULL,
                      "Open the folder the screenshots go to",
                      G_CALLBACK(gd_sidebar_open_shots), 0);

    group = gd_sidebar_group(s, "APPS AND TEXT");
    install = gd_sidebar_button(s, group, "Install .ipa…", NULL,
                                "Copy .ipa files to the guest, which "
                                "installs them",
                                G_CALLBACK(gd_sidebar_install), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(install), "action");
    if (!s->ipa_dir) {
        gtk_widget_set_sensitive(install, FALSE);
        gtk_widget_set_tooltip_text(install, "ASOFT_IPA_DIR is not set");
    }
    gd_sidebar_button(s, group, "Paste text", "Ctrl+V",
                      "Type the text on the PC's clipboard (Ctrl+V)",
                      G_CALLBACK(gd_sidebar_paste), 0);

    group = gd_sidebar_group(s, "STATE");
    s->sidebar_pause = gd_sidebar_button(s, group, "Pause", NULL,
                                         "Stop the guest where it is, or go on",
                                         G_CALLBACK(gd_sidebar_pause), 0);
    save = gd_sidebar_button(s, group, "Save state", NULL,
                             "Save the whole machine as it is now",
                             G_CALLBACK(gd_sidebar_save_state), 0);
    states = gd_sidebar_button(s, group, "Saved states…", NULL,
                               "Go back to a saved state, or delete one",
                               G_CALLBACK(gd_sidebar_states), 0);
    if (migration_is_blocked(&blocked)) {
        /* a device that cannot be saved (pvg=reims: the host GPU's state) */
        gtk_widget_set_sensitive(save, FALSE);
        gtk_widget_set_sensitive(states, FALSE);
        gtk_widget_set_tooltip_text(save, error_get_pretty(blocked));
        gtk_widget_set_tooltip_text(states, error_get_pretty(blocked));
        error_free(blocked);
    }

    group = gd_sidebar_group(s, "POWER");
    s->sidebar_power = gd_sidebar_button(s, group, "Shut down", NULL,
                                         s->quick_boot ?
                                         "Shut the guest down (closing the "
                                         "window saves its state instead); "
                                         "pressed again, quit QEMU" :
                                         "Power the guest down; pressed "
                                         "again, quit QEMU",
                                         G_CALLBACK(gd_sidebar_power), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->sidebar_power),
                                "destructive");

    s->sidebar_status = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(s->sidebar_status), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(s->sidebar_status),
                                 PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(s->sidebar_status), 18);
    gtk_label_set_xalign(GTK_LABEL(s->sidebar_status), 0);
    gtk_label_set_selectable(GTK_LABEL(s->sidebar_status), TRUE);
    gtk_widget_set_can_focus(s->sidebar_status, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(s->sidebar_status),
                                "status");
    gtk_box_pack_start(GTK_BOX(s->sidebar), s->sidebar_status,
                       FALSE, FALSE, 0);
}

#ifdef GDK_WINDOWING_WIN32
/*
 * A dark title bar to go with the side bar: Windows 10 1809 and later take
 * DWMWA_USE_IMMERSIVE_DARK_MODE (20), Windows 11 also a caption color (35) and
 * its text color (36). Looked up at run time: dwmapi is not linked.
 */
static void gd_win32_dark_title(GtkWidget *window)
{
    HRESULT (WINAPI *set_attribute)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
    GdkWindow *gdk_window = gtk_widget_get_window(window);
    BOOL dark = TRUE;
    COLORREF caption = RGB(0, 0, 0), text = RGB(255, 255, 255);
    HWND hwnd;

    if (!dwmapi || !gdk_window) {
        return;
    }
    set_attribute = (void *)GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    if (!set_attribute) {
        return;
    }
    hwnd = gdk_win32_window_get_handle(gdk_window);
    set_attribute(hwnd, 20, &dark, sizeof(dark));
    set_attribute(hwnd, 35, &caption, sizeof(caption));
    set_attribute(hwnd, 36, &text, sizeof(text));
    /* redraw the frame with them */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
#endif

/** GTK Events **/

/*
 * window-close=off: closing the window asks the guest to power down (as
 * system_powerdown does: on the virt machine its power key is pressed), so
 * it can sync and unmount its disks and then turn itself off. Closing the
 * window again quits right away.
 */
static gboolean gd_window_close(GtkWidget *widget, GdkEvent *event,
                                void *opaque)
{
    GtkDisplayState *s = opaque;

    /* quick-boot=on: save the machine, then quit; the next start resumes it */
    if (s->quick_boot && !s->powerdown_requested && !s->snapshot_busy &&
        (runstate_is_running() || runstate_check(RUN_STATE_PAUSED))) {
        gd_snapshot_job(s, QUICK_BOOT_SNAPSHOT, 3, "Saving the state…");
        return TRUE;
    }
    if (s->snapshot_busy) {
        return TRUE;                /* QEMU stands still until it is done */
    }
    if (s->opts->has_window_close && !s->opts->window_close &&
        !s->powerdown_requested) {
        gd_powerdown(s);
        return TRUE;
    }

    qmp_quit(NULL);
    return TRUE;
}

static void gd_set_ui_refresh_rate(VirtualConsole *vc, int refresh_rate)
{
    QemuUIInfo info;

    if (!qemu_console_ui_info_supported(vc->gfx.dcl.con)) {
        return;
    }

    info = *qemu_console_get_ui_info(vc->gfx.dcl.con);
    info.refresh_rate = refresh_rate;
    qemu_console_set_ui_info(vc->gfx.dcl.con, &info, true);
}

static void gd_set_ui_size(VirtualConsole *vc, gint width, gint height)
{
    QemuUIInfo info;

    if (!qemu_console_ui_info_supported(vc->gfx.dcl.con)) {
        return;
    }

    info = *qemu_console_get_ui_info(vc->gfx.dcl.con);
    info.width = width;
    info.height = height;
    qemu_console_set_ui_info(vc->gfx.dcl.con, &info, true);
}

#if defined(CONFIG_OPENGL)

static gboolean gd_render_event(GtkGLArea *area, GdkGLContext *context,
                                void *opaque)
{
    VirtualConsole *vc = opaque;

    if (vc->gfx.gls) {
        gd_gl_area_draw(vc);
    }
    return TRUE;
}

static void gd_resize_event(GtkGLArea *area,
                            gint width, gint height, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    double pw = width, ph = height;
    double sx = vc->gfx.scale_x, sy = vc->gfx.scale_y;
    GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(area));
    const int gs = gdk_window_get_scale_factor(window);

    if (!vc->s->free_scale && !vc->s->full_screen) {
        pw /= sx;
        ph /= sy;
    }

    /**
     * width and height here are in pixel coordinate, so we must divide it
     * by global window scale (gs)
     */
    gd_set_ui_size(vc, pw / gs, ph / gs);
}

#endif

void gd_update_monitor_refresh_rate(VirtualConsole *vc, GtkWidget *widget)
{
#ifdef GDK_VERSION_3_22
    GdkWindow *win = gtk_widget_get_window(widget);
    int refresh_rate;

    if (win) {
        GdkDisplay *dpy = gtk_widget_get_display(widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_window(dpy, win);
        refresh_rate = gdk_monitor_get_refresh_rate(monitor); /* [mHz] */
    } else {
        refresh_rate = 0;
    }

    gd_set_ui_refresh_rate(vc, refresh_rate);

    /*
     * T = 1 / f = 1 [s*Hz] / f = 1000*1000 [ms*mHz] / f, but never shorter
     * than GUI_REFRESH_INTERVAL_DEFAULT: a refresh redraws the whole surface
     * of a display without dirty tracking (ramfb) with the BQL held, and at
     * the 144 Hz of a laptop panel that kept the main loop, and so every vCPU
     * waiting for the BQL, busy two thirds of the time.
     */
    vc->gfx.dcl.update_interval = refresh_rate ?
        MAX(1000 * 1000 / refresh_rate, GUI_REFRESH_INTERVAL_DEFAULT) :
        GUI_REFRESH_INTERVAL_DEFAULT;
#endif
}

void gd_update_scale(VirtualConsole *vc, int ww, int wh, int fbw, int fbh)
{
    if (!vc) {
        return;
    }

    if (vc->s->full_screen) {
        vc->gfx.scale_x = (double)ww / fbw;
        vc->gfx.scale_y = (double)wh / fbh;
    } else if (vc->s->free_scale) {
        double sx, sy;

        sx = (double)ww / fbw;
        sy = (double)wh / fbh;
        if (vc->s->keep_aspect_ratio) {
            vc->gfx.scale_x = vc->gfx.scale_y = MIN(sx, sy);
        } else {
            vc->gfx.scale_x = sx;
            vc->gfx.scale_y = sy;
        }
    }
}
/**
 * DOC: Coordinate handling.
 *
 * We are coping with sizes and positions in various coordinates and the
 * handling of these coordinates is somewhat confusing. It would benefit us
 * all if we define these coordinates explicitly and clearly. Besides, it's
 * also helpful to follow the same naming convention for variables
 * representing values in different coordinates.
 *
 * I. Definitions
 *
 * - (guest) buffer coordinate: this is the coordinates that the guest will
 *   see. The x/y offsets and width/height specified in commands sent by
 *   guest is basically in buffer coordinate.
 *
 * - (host) pixel coordinate: this is the coordinate in pixel level on the
 *   host destop. A window/widget of width 300 in pixel coordinate means it
 *   occupies 300 pixels horizontally.
 *
 * - (host) logical window coordinate: the existence of global scaling
 *   factor in desktop level makes this kind of coordinate play a role. It
 *   always holds that (logical window size) * (global scale factor) =
 *   (pixel size).
 *
 * - global scale factor: this is specified in desktop level and is
 *   typically invariant during the life cycle of the process. Users with
 *   high-DPI monitors might set this scale, for example, to 2, in order to
 *   make the UI look larger.
 *
 * - zooming scale: this can be freely controlled by the QEMU user to zoom
 *   in/out the guest content.
 *
 * II. Representation
 *
 * We'd like to use consistent representation for variables in different
 * coordinates:
 * - buffer coordinate: prefix fb
 * - pixel coordinate: prefix p
 * - logical window coordinate: prefix w
 *
 * For scales:
 * - global scale factor: prefix gs
 * - zooming scale: prefix scale/s
 *
 * Example: fbw, pw, ww for width in different coordinates
 *
 * III. Equation
 *
 * - fbw * gs * scale_x = pw
 * - pw = gs * ww
 *
 * Consequently we have
 *
 * - fbw * scale_x = ww
 *
 * Example: assuming we are running QEMU on a 3840x2160 screen and have set
 * global scaling factor to 2, if the guest buffer size is 1920x1080 and the
 * zooming scale is 0.5, then we have:
 * - fbw = 1920, fbh = 1080
 * - pw  = 1920, ph  = 1080
 * - ww  = 960,  wh  = 540
 * A bonus of this configuration is that we can achieve pixel to pixel
 * presentation of the guest content.
 */

static gboolean gd_draw_event(GtkWidget *widget, cairo_t *cr, void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int wx_offset, wy_offset;
    int ww_widget, wh_widget, ww_surface, wh_surface;
    int fbw, fbh;

#if defined(CONFIG_OPENGL)
    if (vc->gfx.gls) {
        if (gtk_use_gl_area) {
            /* invoke render callback please */
            return FALSE;
        } else {
#ifdef CONFIG_X11
            gd_egl_draw(vc);
            return TRUE;
#else
            abort();
#endif
        }
    }
#endif

    if (!gtk_widget_get_realized(widget)) {
        return FALSE;
    }
    if (!vc->gfx.ds) {
        return FALSE;
    }
    if (!vc->gfx.surface) {
        return FALSE;
    }

    gd_update_monitor_refresh_rate(vc, vc->window ? vc->window : s->window);

    fbw = surface_width(vc->gfx.ds);
    fbh = surface_height(vc->gfx.ds);

    ww_widget = gdk_window_get_width(gtk_widget_get_window(widget));
    wh_widget = gdk_window_get_height(gtk_widget_get_window(widget));

    gd_update_scale(vc, ww_widget, wh_widget, fbw, fbh);

    ww_surface = fbw * vc->gfx.scale_x;
    wh_surface = fbh * vc->gfx.scale_y;

    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    cairo_rectangle(cr, 0, 0, ww_widget, wh_widget);

    /* Optionally cut out the inner area where the pixmap
       will be drawn. This avoids 'flashing' since we're
       not double-buffering. Note we're using the undocumented
       behaviour of drawing the rectangle from right to left
       to cut out the whole */
    cairo_rectangle(cr, wx_offset + ww_surface, wy_offset,
                    -1 * ww_surface, wh_surface);
    cairo_fill(cr);

    cairo_scale(cr, vc->gfx.scale_x, vc->gfx.scale_y);
    cairo_set_source_surface(cr, vc->gfx.surface,
                             wx_offset / vc->gfx.scale_x,
                             wy_offset / vc->gfx.scale_y);
    cairo_paint(cr);

    return TRUE;
}

static gboolean gd_motion_event(GtkWidget *widget, GdkEventMotion *motion,
                                void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int fbx, fby;
    int wx_offset, wy_offset;
    int wh_surface, ww_surface;
    int ww_widget, wh_widget;

    if (!vc->gfx.ds) {
        return TRUE;
    }

    ww_surface = surface_width(vc->gfx.ds) * vc->gfx.scale_x;
    wh_surface = surface_height(vc->gfx.ds) * vc->gfx.scale_y;
    ww_widget = gtk_widget_get_allocated_width(widget);
    wh_widget = gtk_widget_get_allocated_height(widget);

    /*
     * `widget` may not have the same size with the frame buffer.
     * In such cases, some paddings are needed around the `vc`.
     * To achieve that, `vc` will be displayed at (mx, my)
     * so that it is displayed at the center of the widget.
     */
    wx_offset = wy_offset = 0;
    if (ww_widget > ww_surface) {
        wx_offset = (ww_widget - ww_surface) / 2;
    }
    if (wh_widget > wh_surface) {
        wy_offset = (wh_widget - wh_surface) / 2;
    }

    /*
     * `motion` is reported in `widget` coordinates
     * so translating it to the coordinates in `vc`.
     */
    fbx = (motion->x - wx_offset) / vc->gfx.scale_x;
    fby = (motion->y - wy_offset) / vc->gfx.scale_y;

    trace_gd_motion_event(ww_widget, wh_widget,
                          gtk_widget_get_scale_factor(widget), fbx, fby);

    if (qemu_input_is_absolute(vc->gfx.dcl.con)) {
        if (fbx < 0 || fby < 0 ||
            fbx >= surface_width(vc->gfx.ds) ||
            fby >= surface_height(vc->gfx.ds)) {
            return TRUE;
        }
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_X, fbx,
                             0, surface_width(vc->gfx.ds));
        qemu_input_queue_abs(vc->gfx.dcl.con, INPUT_AXIS_Y, fby,
                             0, surface_height(vc->gfx.ds));
        qemu_input_event_sync();
    } else if (s->last_set && s->ptr_owner == vc) {
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_X, fbx - s->last_x);
        qemu_input_queue_rel(vc->gfx.dcl.con, INPUT_AXIS_Y, fby - s->last_y);
        qemu_input_event_sync();
    }
    s->last_x = fbx;
    s->last_y = fby;
    s->last_set = TRUE;

    if (!qemu_input_is_absolute(vc->gfx.dcl.con) && s->ptr_owner == vc) {
        GdkScreen *screen = gtk_widget_get_screen(vc->gfx.drawing_area);
        GdkDisplay *dpy = gtk_widget_get_display(widget);
        GdkWindow *win = gtk_widget_get_window(widget);
        GdkMonitor *monitor = gdk_display_get_monitor_at_window(dpy, win);
        GdkRectangle geometry;

        int xr = (int)motion->x_root;
        int yr = (int)motion->y_root;

        gdk_monitor_get_geometry(monitor, &geometry);

        /* In relative mode check to see if client pointer hit
         * one of the monitor edges, and if so move it back to the
         * center of the monitor. This is important because the pointer
         * in the server doesn't correspond 1-for-1, and so
         * may still be only half way across the screen. Without
         * this warp, the server pointer would thus appear to hit
         * an invisible wall */
        if (xr <= geometry.x || xr - geometry.x >= geometry.width - 1 ||
            yr <= geometry.y || yr - geometry.y >= geometry.height - 1) {
            GdkDevice *dev = gdk_event_get_device((GdkEvent *)motion);
            xr = geometry.x + geometry.width / 2;
            yr = geometry.y + geometry.height / 2;

            gdk_device_warp(dev, screen, xr, yr);
            s->last_set = FALSE;
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean gd_button_event(GtkWidget *widget, GdkEventButton *button,
                                void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    InputButton btn;

    /* implicitly grab the input at the first click in the relative mode */
    if (button->button == 1 && button->type == GDK_BUTTON_PRESS &&
        !qemu_input_is_absolute(vc->gfx.dcl.con) && s->ptr_owner != vc) {
        if (!vc->window) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                           TRUE);
        } else {
            gd_grab_pointer(vc, "relative-mode-click");
        }
        return TRUE;
    }

    if (button->button == 1) {
        btn = INPUT_BUTTON_LEFT;
    } else if (button->button == 2) {
        btn = INPUT_BUTTON_MIDDLE;
    } else if (button->button == 3) {
        btn = INPUT_BUTTON_RIGHT;
    } else if (button->button == 8) {
        btn = INPUT_BUTTON_SIDE;
    } else if (button->button == 9) {
        btn = INPUT_BUTTON_EXTRA;
    } else {
        return TRUE;
    }

    if (button->type == GDK_2BUTTON_PRESS || button->type == GDK_3BUTTON_PRESS) {
        return TRUE;
    }

    if (btn == INPUT_BUTTON_LEFT) {
        /* a real touch ends any gesture the side bar is making */
        if (button->type == GDK_BUTTON_PRESS && (s->gesture_timer || s->gesture_down)) {
            gd_gesture_abort(s);
        }
        s->mouse_down = button->type == GDK_BUTTON_PRESS;
    }
    qemu_input_queue_btn(vc->gfx.dcl.con, btn,
                         button->type == GDK_BUTTON_PRESS);
    qemu_input_event_sync();
    return TRUE;
}

static gboolean gd_scroll_event(GtkWidget *widget, GdkEventScroll *scroll,
                                void *opaque)
{
    VirtualConsole *vc = opaque;
    InputButton btn_vertical;
    InputButton btn_horizontal;
    bool has_vertical = false;
    bool has_horizontal = false;

    /* with the side bar the guest is a phone: the wheel swipes */
    if (vc->s->sidebar && qemu_input_is_absolute(vc->gfx.dcl.con)) {
        gdouble delta_x, delta_y;

        if (scroll->direction == GDK_SCROLL_UP) {
            vc->s->wheel_pending -= 1;
        } else if (scroll->direction == GDK_SCROLL_DOWN) {
            vc->s->wheel_pending += 1;
        } else if (scroll->direction == GDK_SCROLL_SMOOTH &&
                   gdk_event_get_scroll_deltas((GdkEvent *)scroll,
                                               &delta_x, &delta_y)) {
            vc->s->wheel_pending += delta_y;
        }
        gd_wheel_swipe(vc->s);
        return TRUE;
    }

    if (scroll->direction == GDK_SCROLL_UP) {
        btn_vertical = INPUT_BUTTON_WHEEL_UP;
        has_vertical = true;
    } else if (scroll->direction == GDK_SCROLL_DOWN) {
        btn_vertical = INPUT_BUTTON_WHEEL_DOWN;
        has_vertical = true;
    } else if (scroll->direction == GDK_SCROLL_LEFT) {
        btn_horizontal = INPUT_BUTTON_WHEEL_LEFT;
        has_horizontal = true;
    } else if (scroll->direction == GDK_SCROLL_RIGHT) {
        btn_horizontal = INPUT_BUTTON_WHEEL_RIGHT;
        has_horizontal = true;
    } else if (scroll->direction == GDK_SCROLL_SMOOTH) {
        gdouble delta_x, delta_y;
        if (!gdk_event_get_scroll_deltas((GdkEvent *)scroll,
                                         &delta_x, &delta_y)) {
            return TRUE;
        }

        if (delta_y > 0) {
            btn_vertical = INPUT_BUTTON_WHEEL_DOWN;
            has_vertical = true;
        } else if (delta_y < 0) {
            btn_vertical = INPUT_BUTTON_WHEEL_UP;
            has_vertical = true;
        } else if (delta_x > 0) {
            btn_horizontal = INPUT_BUTTON_WHEEL_RIGHT;
            has_horizontal = true;
        } else if (delta_x < 0) {
            btn_horizontal = INPUT_BUTTON_WHEEL_LEFT;
            has_horizontal = true;
        } else {
            return TRUE;
        }
    } else {
        return TRUE;
    }

    if (has_vertical) {
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_vertical, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_vertical, false);
        qemu_input_event_sync();
    }

    if (has_horizontal) {
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_horizontal, true);
        qemu_input_event_sync();
        qemu_input_queue_btn(vc->gfx.dcl.con, btn_horizontal, false);
        qemu_input_event_sync();
    }

    return TRUE;
}


static gboolean gd_touch_event(GtkWidget *widget, GdkEventTouch *touch,
                               void *opaque)
{
    VirtualConsole *vc = opaque;
    Error *err = NULL;
    uint64_t num_slot = GPOINTER_TO_UINT(touch->sequence);
    int type = -1;

    switch (touch->type) {
    case GDK_TOUCH_BEGIN:
        type = INPUT_MULTI_TOUCH_TYPE_BEGIN;
        break;
    case GDK_TOUCH_UPDATE:
        type = INPUT_MULTI_TOUCH_TYPE_UPDATE;
        break;
    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
        type = INPUT_MULTI_TOUCH_TYPE_END;
        break;
    default:
        warn_report("gtk: unexpected touch event type");
        return FALSE;
    }

    qemu_input_touch_event(vc->gfx.dcl.con, touch_slots,
                           num_slot, surface_width(vc->gfx.ds),
                           surface_height(vc->gfx.ds), touch->x,
                           touch->y, type, &err);
    if (err) {
        warn_report_err(err);
    }
    return TRUE;
}

static const guint16 *gd_get_keymap(size_t *maplen, bool *xorgevdev)
{
    GdkDisplay *dpy = gdk_display_get_default();

    *maplen = 0;
    *xorgevdev = false;

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("x11");
        return qemu_xkeymap_mapping_table(
            gdk_x11_display_get_xdisplay(dpy), maplen, xorgevdev);
    }
#endif

#ifdef GDK_WINDOWING_WAYLAND
    if (GDK_IS_WAYLAND_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("wayland");
        *xorgevdev = true;
        return NULL;
    }
#endif

#ifdef GDK_WINDOWING_WIN32
    if (GDK_IS_WIN32_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("win32");
        *maplen = qemu_input_map_atset1_to_linux_len;
        return qemu_input_map_atset1_to_linux;
    }
#endif

#ifdef GDK_WINDOWING_QUARTZ
    if (GDK_IS_QUARTZ_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("quartz");
        *maplen = qemu_input_map_osx_to_linux_len;
        return qemu_input_map_osx_to_linux;
    }
#endif

#ifdef GDK_WINDOWING_BROADWAY
    if (GDK_IS_BROADWAY_DISPLAY(dpy)) {
        trace_gd_keymap_windowing("broadway");
        g_warning("experimental: using broadway, x11 virtual keysym\n"
                  "mapping - with very limited support. See also\n"
                  "https://bugzilla.gnome.org/show_bug.cgi?id=700105");
        *maplen = qemu_input_map_x11_to_linux_len;
        return qemu_input_map_x11_to_linux;
    }
#endif

    g_warning("Unsupported GDK Windowing platform.\n"
              "Disabling extended keycode tables.\n"
              "Please report to qemu-devel@nongnu.org\n"
              "including the following information:\n"
              "\n"
              "  - Operating system\n"
              "  - GDK Windowing system build\n");
    return NULL;
}


static unsigned int gd_map_keycode(int scancode)
{
    if (keycode_xorgevdev) {
        return scancode < 8 ? KEY_RESERVED : scancode - 8;
    }
    if (!keycode_map) {
        return 0;
    }
    if (scancode > keycode_maplen) {
        return 0;
    }

    return keycode_map[scancode];
}

static int gd_get_keycode(GdkEventKey *key)
{
#ifdef G_OS_WIN32
    int scancode = gdk_event_get_scancode((GdkEvent *)key);

    /* translate Windows native scancodes to atset1 keycodes */
    switch (scancode & (KF_EXTENDED | 0xff)) {
    case 0x145:     /* NUMLOCK */
        return scancode & 0xff;
    }

    return scancode & KF_EXTENDED ?
        0xe000 | (scancode & 0xff) : scancode & 0xff;

#else
    return key->hardware_keycode;
#endif
}

static gboolean gd_text_key_down(GtkWidget *widget,
                                 GdkEventKey *key, void *opaque)
{
    VirtualConsole *vc = opaque;
    QemuTextConsole *con = QEMU_TEXT_CONSOLE(vc->gfx.dcl.con);

    if (key->keyval == GDK_KEY_Delete) {
        qemu_text_console_put_linux(con, KEY_DELETE, false);
    } else if (key->length) {
        qemu_text_console_put_string(con, key->string, key->length);
    } else {
        unsigned int lnx = gd_map_keycode(gd_get_keycode(key));
        qemu_text_console_put_linux(con, lnx, false);
    }
    return TRUE;
}

static gboolean gd_key_event(GtkWidget *widget, GdkEventKey *key, void *opaque)
{
    VirtualConsole *vc = opaque;
    int keycode, button;
    unsigned int lnx;

#ifdef G_OS_WIN32
    /* on windows, we ought to ignore the reserved key event? */
    if (key->hardware_keycode == 0xff)
        return false;

    if (!vc->s->kbd_owner) {
        if (key->hardware_keycode == VK_LWIN ||
            key->hardware_keycode == VK_RWIN) {
            return FALSE;
        }
    }
#endif

    if (key->keyval == GDK_KEY_Pause
#ifdef G_OS_WIN32
        /* for some reason GDK does not fill keyval for VK_PAUSE
         * See https://bugzilla.gnome.org/show_bug.cgi?id=769214
         */
        || key->hardware_keycode == VK_PAUSE
#endif
        ) {
        qkbd_state_key_event(vc->gfx.kbd, KEY_PAUSE,
                             key->type == GDK_KEY_PRESS);
        return TRUE;
    }

    /* the side bar's keys: F11 full screen, Ctrl+S screenshot, Ctrl+V paste */
    if (vc->s->sidebar && key->type == GDK_KEY_PRESS) {
        guint lower = gdk_keyval_to_lower(key->keyval);
        bool ctrl = (key->state & GDK_CONTROL_MASK) &&
                    !(key->state & (GDK_MOD1_MASK | GDK_SHIFT_MASK));

        if (key->keyval == GDK_KEY_F11) {
            gd_sidebar_full_screen(NULL, vc->s);
            return TRUE;
        }
        if (ctrl && lower == GDK_KEY_s) {
            gd_sidebar_screenshot(NULL, vc->s);
            return TRUE;
        }
        if (ctrl && lower == GDK_KEY_v) {
            gd_sidebar_paste(NULL, vc->s);
            return TRUE;
        }
    }

    keycode = gd_get_keycode(key);
    lnx = gd_map_keycode(keycode);

    trace_gd_key_event(vc->label, keycode, lnx,
                       (key->type == GDK_KEY_PRESS) ? "down" : "up");

    /* F1-F4 are the phone's buttons on a guest with a usb-consumer: one press each */
    button = gd_consumer_button(lnx);
    if (button >= 0 && gd_consumer()) {
        if (key->type == GDK_KEY_PRESS &&
            !(vc->s->consumer_keys_down & BIT(button))) {
            vc->s->consumer_keys_down |= BIT(button);
            gd_sidebar_press(lnx);
        } else if (key->type == GDK_KEY_RELEASE) {
            vc->s->consumer_keys_down &= ~BIT(button);
        }
        return TRUE;
    }

    qkbd_state_key_event(vc->gfx.kbd, lnx, key->type == GDK_KEY_PRESS);

    return TRUE;
}

static gboolean gd_grab_broken_event(GtkWidget *widget,
                                     GdkEventGrabBroken *event, void *opaque)
{
#ifdef CONFIG_WIN32
    /*
     * On Windows the Ctrl-Alt-Del key combination can't be grabbed. This
     * key combination leaves all three keys in a stuck condition. We use
     * the grab-broken-event to release all keys.
     */
    if (event->keyboard) {
        VirtualConsole *vc = opaque;
        GtkDisplayState *s = vc->s;

        gtk_release_modifiers(s);
    }
#endif
    return TRUE;
}

static gboolean gd_event(GtkWidget *widget, GdkEvent *event, void *opaque)
{
    if (event->type == GDK_MOTION_NOTIFY) {
        return gd_motion_event(widget, &event->motion, opaque);
    }
    return FALSE;
}

/** Window Menu Actions **/

static void gd_menu_pause(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;

    if (s->external_pause_update) {
        return;
    }
    if (runstate_is_running()) {
        qmp_stop(NULL);
    } else {
        qmp_cont(NULL);
    }
}

static void gd_menu_reset(GtkMenuItem *item, void *opaque)
{
    qmp_system_reset(NULL);
}

static void gd_menu_powerdown(GtkMenuItem *item, void *opaque)
{
    qmp_system_powerdown(NULL);
}

static void gd_menu_quit(GtkMenuItem *item, void *opaque)
{
    qmp_quit(NULL);
}

static void gd_menu_switch_vc(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_by_menu(s);
    GtkNotebook *nb = GTK_NOTEBOOK(s->notebook);
    gint page;

    gtk_release_modifiers(s);
    if (vc) {
        page = gtk_notebook_page_num(nb, vc->tab_item);
        gtk_notebook_set_current_page(nb, page);
        gtk_widget_grab_focus(vc->focus);
    }
}

static void gd_accel_switch_vc(void *opaque)
{
    VirtualConsole *vc = opaque;

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(vc->menu_item), TRUE);
}

static void gd_menu_show_tabs(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->show_tabs_item))) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), TRUE);
    } else {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    }
    gd_update_windowsize(vc);
}

static int gd_vc_notebook_pos(GtkDisplayState *s, VirtualConsole *target)
{
    int pos = 0;
    guint i;

    for (i = 0; i < s->vcs->len; i++) {
        VirtualConsole *vc = g_ptr_array_index(s->vcs, i);
        if (vc == target) {
            return pos;
        }
        if (!vc->window) {
            pos++;
        }
    }
    g_assert_not_reached();
}

static gboolean gd_tab_window_close(GtkWidget *widget, GdkEvent *event,
                                    void *opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;
    int page;

    g_object_ref(vc->tab_item);
    gtk_container_remove(GTK_CONTAINER(vc->window), vc->tab_item);
    page = gd_vc_notebook_pos(s, vc);
    gtk_notebook_insert_page(GTK_NOTEBOOK(s->notebook),
                             vc->tab_item, gtk_label_new(vc->label), page);
    g_object_unref(vc->tab_item);
    gtk_widget_destroy(vc->window);
    vc->window = NULL;
#if defined(CONFIG_OPENGL)
    if (vc->gfx.esurface) {
        eglDestroySurface(qemu_egl_display, vc->gfx.esurface);
        vc->gfx.esurface = NULL;
    }
    if (vc->gfx.ectx) {
        eglDestroyContext(qemu_egl_display, vc->gfx.ectx);
        vc->gfx.ectx = NULL;
    }
#endif

    gd_rebuild_vc_menu(s);

    if (vc == gd_vc_find_by_menu(s)) {
        gtk_widget_grab_focus(vc->focus);
    }

    return TRUE;
}

static gboolean gd_win_grab(void *opaque)
{
    VirtualConsole *vc = opaque;

    fprintf(stderr, "%s: %s\n", __func__, vc->label);
    if (vc->s->ptr_owner) {
        gd_ungrab_pointer(vc->s);
    } else {
        gd_grab_pointer(vc, "user-request-detached-tab");
    }
    return TRUE;
}

static void gd_menu_untabify(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (vc->type == GD_VC_GFX &&
        qemu_console_is_graphic(vc->gfx.dcl.con)) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                       FALSE);
    }
    if (!vc->window) {
        vc->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#if defined(CONFIG_OPENGL)
        if (vc->gfx.esurface) {
            eglDestroySurface(qemu_egl_display, vc->gfx.esurface);
            vc->gfx.esurface = NULL;
        }
        if (vc->gfx.ectx) {
            eglDestroyContext(qemu_egl_display, vc->gfx.ectx);
            vc->gfx.ectx = NULL;
        }
#endif
        gd_widget_reparent(s->notebook, vc->window, vc->tab_item);

        g_signal_connect(vc->window, "delete-event",
                         G_CALLBACK(gd_tab_window_close), vc);
        gtk_widget_show_all(vc->window);

        if (qemu_console_is_graphic(vc->gfx.dcl.con)) {
            GtkAccelGroup *ag = gtk_accel_group_new();
            gtk_window_add_accel_group(GTK_WINDOW(vc->window), ag);

            GClosure *cb = g_cclosure_new_swap(G_CALLBACK(gd_win_grab),
                                               vc, NULL);
            gtk_accel_group_connect(ag, GDK_KEY_g, HOTKEY_MODIFIERS, 0, cb);
        }

        gd_rebuild_vc_menu(s);
        gd_update_geometry_hints(vc);
        gd_update_caption(s);
    }
}

static void gd_menu_show_menubar(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (s->full_screen) {
        return;
    }

    if (gtk_check_menu_item_get_active(
                GTK_CHECK_MENU_ITEM(s->show_menubar_item))) {
        gtk_widget_show(s->menu_bar);
    } else {
        gtk_widget_hide(s->menu_bar);
    }
    gd_update_windowsize(vc);
}

static void gd_accel_show_menubar(void *opaque)
{
    GtkDisplayState *s = opaque;
    gtk_menu_item_activate(GTK_MENU_ITEM(s->show_menubar_item));
}

static void gd_menu_full_screen(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (!s->full_screen) {
        gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
        gtk_widget_hide(s->menu_bar);
        if (s->sidebar) {
            gtk_widget_hide(s->sidebar);
        }
        if (vc->type == GD_VC_GFX) {
            gtk_widget_set_size_request(vc->gfx.drawing_area, -1, -1);
        }
        gtk_window_fullscreen(GTK_WINDOW(s->window));
        s->full_screen = TRUE;
    } else {
        gtk_window_unfullscreen(GTK_WINDOW(s->window));
        gd_menu_show_tabs(GTK_MENU_ITEM(s->show_tabs_item), s);
        if (gtk_check_menu_item_get_active(
                    GTK_CHECK_MENU_ITEM(s->show_menubar_item))) {
            gtk_widget_show(s->menu_bar);
        }
        if (s->sidebar) {
            gtk_widget_show(s->sidebar);
        }
        s->full_screen = FALSE;
        if (vc->type == GD_VC_GFX) {
            vc->gfx.scale_x = vc->gfx.preferred_scale;
            vc->gfx.scale_y = vc->gfx.preferred_scale;
            gd_update_windowsize(vc);
        }
    }

    gd_update_cursor(vc);
}

static void gd_accel_full_screen(void *opaque)
{
    GtkDisplayState *s = opaque;
    gtk_menu_item_activate(GTK_MENU_ITEM(s->full_screen_item));
}

static void gd_menu_zoom_in(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->zoom_fit_item),
                                   FALSE);

    vc->gfx.scale_x += VC_SCALE_STEP;
    vc->gfx.scale_y += VC_SCALE_STEP;

    gd_update_windowsize(vc);
}

static void gd_accel_zoom_in(void *opaque)
{
    GtkDisplayState *s = opaque;
    gtk_menu_item_activate(GTK_MENU_ITEM(s->zoom_in_item));
}

static void gd_menu_zoom_out(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->zoom_fit_item),
                                   FALSE);

    vc->gfx.scale_x -= VC_SCALE_STEP;
    vc->gfx.scale_y -= VC_SCALE_STEP;

    vc->gfx.scale_x = MAX(vc->gfx.scale_x, VC_SCALE_MIN);
    vc->gfx.scale_y = MAX(vc->gfx.scale_y, VC_SCALE_MIN);

    gd_update_windowsize(vc);
}

static void gd_menu_zoom_fixed(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    vc->gfx.scale_x = vc->gfx.preferred_scale;
    vc->gfx.scale_y = vc->gfx.preferred_scale;

    gd_update_windowsize(vc);
}

static void gd_menu_zoom_fit(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(s->zoom_fit_item))) {
        s->free_scale = TRUE;
    } else {
        s->free_scale = FALSE;
        vc->gfx.scale_x = vc->gfx.preferred_scale;
        vc->gfx.scale_y = vc->gfx.preferred_scale;
    }

    gd_update_windowsize(vc);
    gd_update_full_redraw(vc);
}

static void gd_grab_update(VirtualConsole *vc, bool kbd, bool ptr)
{
    GdkDisplay *display = gtk_widget_get_display(vc->gfx.drawing_area);
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkWindow *window = gtk_widget_get_window(vc->gfx.drawing_area);
    GdkSeatCapabilities caps = 0;
    GdkCursor *cursor = NULL;

    if (kbd) {
        caps |= GDK_SEAT_CAPABILITY_KEYBOARD;
    }
    if (ptr) {
        caps |= GDK_SEAT_CAPABILITY_ALL_POINTING;
        cursor = vc->s->null_cursor;
    }

    if (caps) {
        gdk_seat_grab(seat, window, caps, false, cursor,
                      NULL, NULL, NULL);
    } else {
        gdk_seat_ungrab(seat);
    }
}

static void gd_grab_keyboard(VirtualConsole *vc, const char *reason)
{
    if (vc->s->kbd_owner) {
        if (vc->s->kbd_owner == vc) {
            return;
        } else {
            gd_ungrab_keyboard(vc->s);
        }
    }

    win32_kbd_set_grab(true);
    gd_grab_update(vc, true, vc->s->ptr_owner == vc);
    vc->s->kbd_owner = vc;
    gd_update_caption(vc->s);
    trace_gd_grab(vc->label, "kbd", reason);
}

static void gd_ungrab_keyboard(GtkDisplayState *s)
{
    VirtualConsole *vc = s->kbd_owner;

    if (vc == NULL) {
        return;
    }
    s->kbd_owner = NULL;

    win32_kbd_set_grab(false);
    gd_grab_update(vc, false, vc->s->ptr_owner == vc);
    gd_update_caption(s);
    trace_gd_ungrab(vc->label, "kbd");
}

static void gd_grab_pointer(VirtualConsole *vc, const char *reason)
{
    GdkDisplay *display = gtk_widget_get_display(vc->gfx.drawing_area);

    if (vc->s->ptr_owner) {
        if (vc->s->ptr_owner == vc) {
            return;
        } else {
            gd_ungrab_pointer(vc->s);
        }
    }

    gd_grab_update(vc, vc->s->kbd_owner == vc, true);
    gdk_device_get_position(gd_get_pointer(display),
                            NULL, &vc->s->grab_x_root, &vc->s->grab_y_root);
    vc->s->ptr_owner = vc;
    gd_update_caption(vc->s);
    trace_gd_grab(vc->label, "ptr", reason);
}

static void gd_ungrab_pointer(GtkDisplayState *s)
{
    VirtualConsole *vc = s->ptr_owner;
    GdkDisplay *display;

    if (vc == NULL) {
        return;
    }
    s->ptr_owner = NULL;

    display = gtk_widget_get_display(vc->gfx.drawing_area);
    gd_grab_update(vc, vc->s->kbd_owner == vc, false);
    gdk_device_warp(gd_get_pointer(display),
                    gtk_widget_get_screen(vc->gfx.drawing_area),
                    vc->s->grab_x_root, vc->s->grab_y_root);
    gd_update_caption(s);
    trace_gd_ungrab(vc->label, "ptr");
}

static void gd_menu_grab_input(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

    if (gd_is_grab_active(s)) {
        gd_grab_keyboard(vc, "user-request-main-window");
        gd_grab_pointer(vc, "user-request-main-window");
    } else {
        gd_ungrab_keyboard(s);
        gd_ungrab_pointer(s);
    }

    gd_update_cursor(vc);
}

static void gd_change_page(GtkNotebook *nb, gpointer arg1, guint arg2,
                           gpointer data)
{
    GtkDisplayState *s = data;
    VirtualConsole *vc;
    gboolean on_vga;

    if (!gtk_widget_get_realized(s->notebook)) {
        return;
    }

    vc = gd_vc_find_by_page(s, arg2);
    if (!vc) {
        return;
    }
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(vc->menu_item),
                                   TRUE);
    on_vga = (vc->type == GD_VC_GFX &&
              qemu_console_is_graphic(vc->gfx.dcl.con));
    if (!on_vga) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                       FALSE);
    } else if (s->full_screen) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->grab_item),
                                       TRUE);
    }
    gtk_widget_set_sensitive(s->grab_item, on_vga);
#ifdef CONFIG_VTE
    gtk_widget_set_sensitive(s->copy_item, vc->type == GD_VC_VTE);
#endif

    gd_update_windowsize(vc);
    gd_update_cursor(vc);
}

static gboolean gd_enter_event(GtkWidget *widget, GdkEventCrossing *crossing,
                               gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    if (gd_grab_on_hover(s)) {
        gd_grab_keyboard(vc, "grab-on-hover");
    }
    return TRUE;
}

static gboolean gd_leave_event(GtkWidget *widget, GdkEventCrossing *crossing,
                               gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    if (gd_grab_on_hover(s)) {
        gd_ungrab_keyboard(s);
    }
    return TRUE;
}

static gboolean gd_focus_in_event(GtkWidget *widget,
                                  GdkEventFocus *event, gpointer opaque)
{
    VirtualConsole *vc = opaque;

    win32_kbd_set_window(gd_win32_get_hwnd(vc));
    return TRUE;
}

static gboolean gd_focus_out_event(GtkWidget *widget,
                                   GdkEventFocus *event, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    GtkDisplayState *s = vc->s;

    win32_kbd_set_window(NULL);
    gtk_release_modifiers(s);
    return TRUE;
}

static gboolean gd_configure(GtkWidget *widget,
                             GdkEventConfigure *cfg, gpointer opaque)
{
    VirtualConsole *vc = opaque;
    const double sx = vc->gfx.scale_x, sy = vc->gfx.scale_y;
    double width = cfg->width, height = cfg->height;

    if (!vc->s->free_scale && !vc->s->full_screen) {
        width /= sx;
        height /= sy;
    }

    gd_set_ui_size(vc, width, height);

    return FALSE;
}

/** Virtual Console Callbacks **/

static void gd_rebuild_vc_menu(GtkDisplayState *s)
{
    GSList *group = NULL;
    VirtualConsole *vc;
    GList *children;
    gint insert_pos;
    int shortcut_idx = 0;
    guint i;

    for (i = 0; i < s->vcs->len; i++) {
        vc = g_ptr_array_index(s->vcs, i);
        if (vc->menu_item) {
            gtk_widget_destroy(vc->menu_item);
            vc->menu_item = NULL;
        }
    }

    for (i = 0; i < 9; i++) {
        gtk_accel_group_disconnect_key(s->accel_group,
                                       GDK_KEY_1 + i, HOTKEY_MODIFIERS);
    }

    /* find insertion position (just before vc_menu_separator) */
    children = gtk_container_get_children(GTK_CONTAINER(s->view_menu));
    insert_pos = g_list_index(children, s->vc_menu_separator);
    g_list_free(children);

    /* create new menu items for each console */
    for (i = 0; i < s->vcs->len; i++) {
        vc = g_ptr_array_index(s->vcs, i);

        vc->menu_item = gtk_radio_menu_item_new_with_mnemonic(group,
                                                               vc->label);
        group = gtk_radio_menu_item_get_group(
            GTK_RADIO_MENU_ITEM(vc->menu_item));

        if (vc->window) {
            gtk_widget_set_sensitive(vc->menu_item, false);
        } else if (shortcut_idx < 9) {
            guint key = GDK_KEY_1 + shortcut_idx;
            gtk_accel_group_connect(s->accel_group, key,
                    HOTKEY_MODIFIERS, 0,
                    g_cclosure_new_swap(G_CALLBACK(gd_accel_switch_vc),
                                        vc, NULL));
            gtk_accel_label_set_accel(
                    GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(vc->menu_item))),
                    key, HOTKEY_MODIFIERS);
            shortcut_idx++;
        }

        g_signal_connect(vc->menu_item, "activate",
                         G_CALLBACK(gd_menu_switch_vc), s);
        gtk_menu_shell_insert(GTK_MENU_SHELL(s->view_menu),
                              vc->menu_item, insert_pos + i);
        gtk_widget_show(vc->menu_item);
    }

    /* sync active menu item with current notebook page */
    vc = gd_vc_find_current(s);
    if (vc && vc->menu_item) {
        g_signal_handlers_block_by_func(vc->menu_item,
                                        gd_menu_switch_vc, s);
        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(vc->menu_item), TRUE);
        g_signal_handlers_unblock_by_func(vc->menu_item,
                                          gd_menu_switch_vc, s);
    }
}

#if defined(CONFIG_VTE)
static void gd_menu_copy(GtkMenuItem *item, void *opaque)
{
    GtkDisplayState *s = opaque;
    VirtualConsole *vc = gd_vc_find_current(s);

#if VTE_CHECK_VERSION(0, 50, 0)
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(vc->vte.terminal),
                                       VTE_FORMAT_TEXT);
#else
    vte_terminal_copy_clipboard(VTE_TERMINAL(vc->vte.terminal));
#endif
}

static void gd_vc_adjustment_changed(GtkAdjustment *adjustment, void *opaque)
{
    VirtualConsole *vc = opaque;

    if (gtk_adjustment_get_upper(adjustment) >
        gtk_adjustment_get_page_size(adjustment)) {
        gtk_widget_show(vc->vte.scrollbar);
    } else {
        gtk_widget_hide(vc->vte.scrollbar);
    }
}

static void gd_vc_send_chars(VirtualConsole *vc)
{
    uint32_t len, avail;

    len = qemu_chr_be_can_write(vc->vte.chr);
    avail = fifo8_num_used(&vc->vte.out_fifo);
    while (len > 0 && avail > 0) {
        const uint8_t *buf;
        uint32_t size;

        buf = fifo8_pop_bufptr(&vc->vte.out_fifo, MIN(len, avail), &size);
        qemu_chr_be_write(vc->vte.chr, buf, size);
        len = qemu_chr_be_can_write(vc->vte.chr);
        avail -= size;
    }
}

static int gd_vc_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    vte_terminal_feed(VTE_TERMINAL(vc->vte.terminal), (const char *)buf, len);
    return len;
}

static void gd_vc_chr_accept_input(Chardev *chr)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    if (vc) {
        gd_vc_send_chars(vc);
    }
}

static void gd_vc_chr_set_echo(Chardev *chr, bool echo)
{
    VCChardev *vcd = VC_CHARDEV(chr);
    VirtualConsole *vc = vcd->console;

    if (vc) {
        vc->vte.echo = echo;
    } else {
        vcd->echo = echo;
    }
}

static int nb_vcs;
static Chardev *vcs[MAX_VCS];
static bool gd_vc_chr_open(Chardev *chr, ChardevBackend *backend, Error **errp)
{
    if (nb_vcs == MAX_VCS) {
        error_setg(errp, "Maximum number of consoles reached");
        return false;
    }

    vcs[nb_vcs++] = chr;

    /*
     * console/chardev init sometimes completes elsewhere in a 2nd
     * stage, so defer OPENED events until they are fully initialized
     */
    return true;
}

static void char_gd_vc_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = gd_vc_chr_open;
    cc->chr_write = gd_vc_chr_write;
    cc->chr_accept_input = gd_vc_chr_accept_input;
    cc->chr_set_echo = gd_vc_chr_set_echo;
}

static const TypeInfo char_gd_vc_type_info = {
    .name = TYPE_CHARDEV_VC,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(VCChardev),
    .class_init = char_gd_vc_class_init,
};

static gboolean gd_vc_in(VteTerminal *terminal, gchar *text, guint size,
                         gpointer user_data)
{
    VirtualConsole *vc = user_data;
    uint32_t free;

    if (vc->vte.echo) {
        VteTerminal *term = VTE_TERMINAL(vc->vte.terminal);
        int i;
        for (i = 0; i < size; i++) {
            uint8_t c = text[i];
            if (c >= 128 || isprint(c)) {
                /* 8-bit characters are considered printable.  */
                vte_terminal_feed(term, &text[i], 1);
            } else if (c == '\r' || c == '\n') {
                vte_terminal_feed(term, "\r\n", 2);
            } else {
                char ctrl[2] = { '^', 0};
                ctrl[1] = text[i] ^ 64;
                vte_terminal_feed(term, ctrl, 2);
            }
        }
    }

    free = fifo8_num_free(&vc->vte.out_fifo);
    fifo8_push_all(&vc->vte.out_fifo, (uint8_t *)text, MIN(free, size));
    gd_vc_send_chars(vc);

    return TRUE;
}

static void gd_vc_vte_init(GtkDisplayState *s, VirtualConsole *vc,
                           Chardev *chr, int idx)
{
    char buffer[32];
    GtkWidget *box;
    GtkWidget *scrollbar;
    GtkAdjustment *vadjustment;
    VCChardev *vcd = VC_CHARDEV(chr);

    vc->s = s;
    vc->vte.echo = vcd->echo;
    vc->vte.chr = chr;
    fifo8_create(&vc->vte.out_fifo, 4096);
    vcd->console = vc;

    snprintf(buffer, sizeof(buffer), "vc%d", idx);
    vc->label = g_strdup(vc->vte.chr->label ? : buffer);

    vc->vte.terminal = vte_terminal_new();
    g_signal_connect(vc->vte.terminal, "commit", G_CALLBACK(gd_vc_in), vc);

    /* The documentation says that the default is UTF-8, but actually it is
     * 7-bit ASCII at least in VTE 0.38. The function is deprecated since
     * VTE 0.54 (only UTF-8 is supported now). */
#if !VTE_CHECK_VERSION(0, 54, 0)
#if VTE_CHECK_VERSION(0, 38, 0)
    vte_terminal_set_encoding(VTE_TERMINAL(vc->vte.terminal), "UTF-8", NULL);
#else
    vte_terminal_set_encoding(VTE_TERMINAL(vc->vte.terminal), "UTF-8");
#endif
#endif

    vte_terminal_set_scrollback_lines(VTE_TERMINAL(vc->vte.terminal), -1);
    vte_terminal_set_size(VTE_TERMINAL(vc->vte.terminal),
                          VC_TERM_X_MIN, VC_TERM_Y_MIN);

#if VTE_CHECK_VERSION(0, 28, 0)
    vadjustment = gtk_scrollable_get_vadjustment
        (GTK_SCROLLABLE(vc->vte.terminal));
#else
    vadjustment = vte_terminal_get_adjustment(VTE_TERMINAL(vc->vte.terminal));
#endif

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, vadjustment);

    gtk_box_pack_end(GTK_BOX(box), scrollbar, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), vc->vte.terminal, TRUE, TRUE, 0);

    vc->vte.box = box;
    vc->vte.scrollbar = scrollbar;

    g_signal_connect(vadjustment, "changed",
                     G_CALLBACK(gd_vc_adjustment_changed), vc);

    vc->type = GD_VC_VTE;
    vc->tab_item = box;
    vc->focus = vc->vte.terminal;
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), vc->tab_item,
                             gtk_label_new(vc->label));

    qemu_chr_be_event(vc->vte.chr, CHR_EVENT_OPENED);
}

static void gd_vcs_init(GtkDisplayState *s)
{
    int i;

    for (i = 0; i < nb_vcs; i++) {
        VirtualConsole *vc = g_new0(VirtualConsole, 1);
        g_ptr_array_add(s->vcs, vc);
        gd_vc_vte_init(s, vc, vcs[i], s->vcs->len - 1);
    }
}
#endif /* CONFIG_VTE */

/** Window Creation **/

static void gd_connect_vc_gfx_signals(VirtualConsole *vc)
{
    g_signal_connect(vc->gfx.drawing_area, "draw",
                     G_CALLBACK(gd_draw_event), vc);
#if defined(CONFIG_OPENGL)
    if (gtk_use_gl_area) {
        /* wire up GtkGlArea events */
        g_signal_connect(vc->gfx.drawing_area, "render",
                         G_CALLBACK(gd_render_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "resize",
                         G_CALLBACK(gd_resize_event), vc);
    }
#endif
    if (qemu_console_is_graphic(vc->gfx.dcl.con)) {
        g_signal_connect(vc->gfx.drawing_area, "event",
                         G_CALLBACK(gd_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "button-press-event",
                         G_CALLBACK(gd_button_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "button-release-event",
                         G_CALLBACK(gd_button_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "scroll-event",
                         G_CALLBACK(gd_scroll_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "key-press-event",
                         G_CALLBACK(gd_key_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "key-release-event",
                         G_CALLBACK(gd_key_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "touch-event",
                         G_CALLBACK(gd_touch_event), vc);

        g_signal_connect(vc->gfx.drawing_area, "enter-notify-event",
                         G_CALLBACK(gd_enter_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "leave-notify-event",
                         G_CALLBACK(gd_leave_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "focus-in-event",
                         G_CALLBACK(gd_focus_in_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "focus-out-event",
                         G_CALLBACK(gd_focus_out_event), vc);
        g_signal_connect(vc->gfx.drawing_area, "configure-event",
                         G_CALLBACK(gd_configure), vc);
        g_signal_connect(vc->gfx.drawing_area, "grab-broken-event",
                         G_CALLBACK(gd_grab_broken_event), vc);
    } else {
        g_signal_connect(vc->gfx.drawing_area, "key-press-event",
                         G_CALLBACK(gd_text_key_down), vc);
    }
}

static void gd_connect_signals(GtkDisplayState *s)
{
    g_signal_connect(s->show_tabs_item, "activate",
                     G_CALLBACK(gd_menu_show_tabs), s);
    g_signal_connect(s->untabify_item, "activate",
                     G_CALLBACK(gd_menu_untabify), s);
    g_signal_connect(s->show_menubar_item, "activate",
                     G_CALLBACK(gd_menu_show_menubar), s);

    g_signal_connect(s->window, "delete-event",
                     G_CALLBACK(gd_window_close), s);

    g_signal_connect(s->pause_item, "activate",
                     G_CALLBACK(gd_menu_pause), s);
    g_signal_connect(s->reset_item, "activate",
                     G_CALLBACK(gd_menu_reset), s);
    g_signal_connect(s->powerdown_item, "activate",
                     G_CALLBACK(gd_menu_powerdown), s);
    g_signal_connect(s->quit_item, "activate",
                     G_CALLBACK(gd_menu_quit), s);
#if defined(CONFIG_VTE)
    g_signal_connect(s->copy_item, "activate",
                     G_CALLBACK(gd_menu_copy), s);
#endif
    g_signal_connect(s->full_screen_item, "activate",
                     G_CALLBACK(gd_menu_full_screen), s);
    g_signal_connect(s->zoom_in_item, "activate",
                     G_CALLBACK(gd_menu_zoom_in), s);
    g_signal_connect(s->zoom_out_item, "activate",
                     G_CALLBACK(gd_menu_zoom_out), s);
    g_signal_connect(s->zoom_fixed_item, "activate",
                     G_CALLBACK(gd_menu_zoom_fixed), s);
    g_signal_connect(s->zoom_fit_item, "activate",
                     G_CALLBACK(gd_menu_zoom_fit), s);
    g_signal_connect(s->grab_item, "activate",
                     G_CALLBACK(gd_menu_grab_input), s);
    g_signal_connect(s->notebook, "switch-page",
                     G_CALLBACK(gd_change_page), s);
}

static GtkWidget *gd_create_menu_machine(GtkDisplayState *s)
{
    GtkWidget *machine_menu;
    GtkWidget *separator;

    machine_menu = gtk_menu_new();
    gtk_menu_set_accel_group(GTK_MENU(machine_menu), s->accel_group);

    s->pause_item = gtk_check_menu_item_new_with_mnemonic(_("_Pause"));
    gtk_menu_shell_append(GTK_MENU_SHELL(machine_menu), s->pause_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(machine_menu), separator);

    s->reset_item = gtk_menu_item_new_with_mnemonic(_("_Reset"));
    gtk_menu_shell_append(GTK_MENU_SHELL(machine_menu), s->reset_item);

    s->powerdown_item = gtk_menu_item_new_with_mnemonic(_("Power _Down"));
    gtk_menu_shell_append(GTK_MENU_SHELL(machine_menu), s->powerdown_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(machine_menu), separator);

    s->quit_item = gtk_menu_item_new_with_mnemonic(_("_Quit"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->quit_item),
                                 "<QEMU>/Machine/Quit");
    gtk_accel_map_add_entry("<QEMU>/Machine/Quit",
                            GDK_KEY_q, HOTKEY_MODIFIERS);
    gtk_menu_shell_append(GTK_MENU_SHELL(machine_menu), s->quit_item);

    return machine_menu;
}

#if defined(CONFIG_OPENGL)
static void gl_area_realize(GtkGLArea *area, VirtualConsole *vc)
{
    gtk_gl_area_make_current(area);
    qemu_egl_display = eglGetCurrentDisplay();
    vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
    if (!vc->gfx.has_dmabuf) {
        error_report("GtkGLArea console lacks DMABUF support.");
    }
}
#endif

static bool gd_scale_valid(double scale)
{
    return scale >= VC_SCALE_MIN && scale <= VC_SCALE_MAX;
}

static VirtualConsole *
add_gfx_console(GtkDisplayState *s, QemuConsole *con)
{
    VirtualConsole *vc = g_new0(VirtualConsole, 1);
    const DisplayChangeListenerOps *ops = &dcl_ops;

    g_ptr_array_add(s->vcs, vc);
    vc->label = qemu_console_get_label(con);
    vc->s = s;
    vc->gfx.preferred_scale = 1.0;
    if (s->opts->u.gtk.has_scale) {
        if (gd_scale_valid(s->opts->u.gtk.scale)) {
            vc->gfx.preferred_scale = s->opts->u.gtk.scale;
        } else {
            error_report("Invalid scale value %lf given, being ignored",
                         s->opts->u.gtk.scale);
            s->opts->u.gtk.has_scale = false;
        }
    }
    vc->gfx.scale_x = vc->gfx.preferred_scale;
    vc->gfx.scale_y = vc->gfx.preferred_scale;

#if defined(CONFIG_OPENGL)
    if (display_opengl) {
        if (gtk_use_gl_area) {
            vc->gfx.drawing_area = gtk_gl_area_new();
            g_signal_connect(vc->gfx.drawing_area, "realize",
                             G_CALLBACK(gl_area_realize), vc);
            ops = &dcl_gl_area_ops;
            vc->gfx.dgc.ops = &gl_area_ctx_ops;
        } else {
#ifdef CONFIG_X11
            vc->gfx.drawing_area = gtk_drawing_area_new();
            /*
             * gtk_widget_set_double_buffered() was deprecated in 3.14.
             * It is required for opengl rendering on X11 though.  A
             * proper replacement (native opengl support) is only
             * available in 3.16+.  Silence the warning if possible.
             */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            gtk_widget_set_double_buffered(vc->gfx.drawing_area, FALSE);
#pragma GCC diagnostic pop
            ops = &dcl_egl_ops;
            vc->gfx.dgc.ops = &egl_ctx_ops;
            vc->gfx.has_dmabuf = qemu_egl_has_dmabuf();
#else
            abort();
#endif
        }
    } else
#endif
    {
        vc->gfx.drawing_area = gtk_drawing_area_new();
    }


    gtk_widget_add_events(vc->gfx.drawing_area,
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON_MOTION_MASK |
                          GDK_TOUCH_MASK |
                          GDK_ENTER_NOTIFY_MASK |
                          GDK_LEAVE_NOTIFY_MASK |
                          GDK_SCROLL_MASK |
                          GDK_SMOOTH_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(vc->gfx.drawing_area, TRUE);

    vc->type = GD_VC_GFX;
    vc->tab_item = vc->gfx.drawing_area;
    vc->focus = vc->gfx.drawing_area;
    gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook),
                             vc->tab_item, gtk_label_new(vc->label));

    vc->gfx.kbd = qkbd_state_init(con);
    if (display_opengl) {
        qemu_console_set_display_gl_ctx(con, &vc->gfx.dgc);
    }
    qemu_console_register_listener(con, &vc->gfx.dcl, ops);

    gd_connect_vc_gfx_signals(vc);
    return vc;
}

static void gd_vc_add_gfx(GtkDisplayState *s, QemuConsole *con)
{
    VirtualConsole *vc;
    int i;

    for (i = 0; i < (int)s->vcs->len; i++) {
        VirtualConsole *v = g_ptr_array_index(s->vcs, i);
        if (v->type == GD_VC_GFX && v->gfx.dcl.con == con) {
            return;
        }
    }

    vc = add_gfx_console(s, con);
    gtk_widget_show_all(vc->tab_item);
    gtk_widget_realize(vc->gfx.drawing_area);

    if (s->free_scale) {
        gd_update_windowsize(vc);
    }

    gd_update_caption(s);
    gd_rebuild_vc_menu(s);
}

static void gd_vc_remove_gfx(GtkDisplayState *s, QemuConsole *con)
{
    VirtualConsole *vc = NULL;
    guint idx;

    for (idx = 0; idx < s->vcs->len; idx++) {
        VirtualConsole *v = g_ptr_array_index(s->vcs, idx);
        if (v->type == GD_VC_GFX && v->gfx.dcl.con == con) {
            vc = v;
            break;
        }
    }
    if (!vc) {
        return;
    }

    if (s->kbd_owner == vc) {
        gd_ungrab_keyboard(s);
    }
    if (s->ptr_owner == vc) {
        gd_ungrab_pointer(s);
    }

    g_ptr_array_remove_index(s->vcs, idx);
    gd_rebuild_vc_menu(s);
    gd_update_caption(s);
}

static void gd_console_notify(Notifier *n, void *data)
{
    GtkDisplayState *s = container_of(n, GtkDisplayState, console_notifier);
    QemuConsoleEvent *event = data;

    switch (event->type) {
    case QEMU_CONSOLE_ADDED:
        gd_vc_add_gfx(s, event->con);
        break;
    case QEMU_CONSOLE_REMOVED:
        gd_vc_remove_gfx(s, event->con);
        break;
    }
}

static void gd_create_menu_view(GtkDisplayState *s, DisplayOptions *opts)
{
    GtkWidget *view_menu;
    GtkWidget *separator;
    QemuConsole *con;
    bool zoom_to_fit = false;
    int vc, i;

    view_menu = gtk_menu_new();
    s->view_menu = view_menu;
    gtk_menu_set_accel_group(GTK_MENU(view_menu), s->accel_group);

    s->full_screen_item = gtk_menu_item_new_with_mnemonic(_("_Fullscreen"));

#if defined(CONFIG_VTE)
    s->copy_item = gtk_menu_item_new_with_mnemonic(_("_Copy"));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->copy_item);
#endif

    gtk_accel_group_connect(s->accel_group, GDK_KEY_f, HOTKEY_MODIFIERS, 0,
            g_cclosure_new_swap(G_CALLBACK(gd_accel_full_screen), s, NULL));
    gtk_accel_label_set_accel(
            GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(s->full_screen_item))),
            GDK_KEY_f, HOTKEY_MODIFIERS);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->full_screen_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), separator);

    s->zoom_in_item = gtk_menu_item_new_with_mnemonic(_("Zoom _In"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->zoom_in_item),
                                 "<QEMU>/View/Zoom In");
    gtk_accel_map_add_entry("<QEMU>/View/Zoom In", GDK_KEY_plus,
                            HOTKEY_MODIFIERS);
    gtk_accel_group_connect(s->accel_group, GDK_KEY_equal, HOTKEY_MODIFIERS, 0,
            g_cclosure_new_swap(G_CALLBACK(gd_accel_zoom_in), s, NULL));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->zoom_in_item);

    s->zoom_out_item = gtk_menu_item_new_with_mnemonic(_("Zoom _Out"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->zoom_out_item),
                                 "<QEMU>/View/Zoom Out");
    gtk_accel_map_add_entry("<QEMU>/View/Zoom Out", GDK_KEY_minus,
                            HOTKEY_MODIFIERS);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->zoom_out_item);

    s->zoom_fixed_item = gtk_menu_item_new_with_mnemonic(_("Best _Fit"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->zoom_fixed_item),
                                 "<QEMU>/View/Zoom Fixed");
    gtk_accel_map_add_entry("<QEMU>/View/Zoom Fixed", GDK_KEY_0,
                            HOTKEY_MODIFIERS);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->zoom_fixed_item);

    s->zoom_fit_item = gtk_check_menu_item_new_with_mnemonic(_("Zoom To _Fit"));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->zoom_fit_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), separator);

    s->grab_on_hover_item = gtk_check_menu_item_new_with_mnemonic(_("Grab On _Hover"));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->grab_on_hover_item);

    s->grab_item = gtk_check_menu_item_new_with_mnemonic(_("_Grab Input"));
    gtk_menu_item_set_accel_path(GTK_MENU_ITEM(s->grab_item),
                                 "<QEMU>/View/Grab Input");
    gtk_accel_map_add_entry("<QEMU>/View/Grab Input", GDK_KEY_g,
                            HOTKEY_MODIFIERS);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->grab_item);

    separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), separator);

    /* gfx */
    for (vc = 0;; vc++) {
        con = qemu_console_lookup_by_index(vc);
        if (!con) {
            break;
        }
        add_gfx_console(s, con);
        if (qemu_console_ui_info_supported(con)) {
            zoom_to_fit = true;
        }
    }

    if (s->opts->u.gtk.has_zoom_to_fit) {
        zoom_to_fit = s->opts->u.gtk.zoom_to_fit;
    }
    if (zoom_to_fit) {
        gtk_menu_item_activate(GTK_MENU_ITEM(s->zoom_fit_item));
        s->free_scale = true;
    }

    s->keep_aspect_ratio = true;
    if (s->opts->u.gtk.has_keep_aspect_ratio) {
        s->keep_aspect_ratio = s->opts->u.gtk.keep_aspect_ratio;
    }

    for (i = 0; i < INPUT_EVENT_SLOTS_MAX; i++) {
        struct touch_slot *slot = &touch_slots[i];
        slot->tracking_id = -1;
    }

#if defined(CONFIG_VTE)
    /* vte */
    gd_vcs_init(s);
#endif

    s->vc_menu_separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->vc_menu_separator);

    gd_rebuild_vc_menu(s);

    s->show_tabs_item = gtk_check_menu_item_new_with_mnemonic(_("Show _Tabs"));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->show_tabs_item);

    s->untabify_item = gtk_menu_item_new_with_mnemonic(_("Detach Tab"));
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->untabify_item);

    s->show_menubar_item = gtk_check_menu_item_new_with_mnemonic(
            _("Show Menubar"));
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(s->show_menubar_item),
                                   !opts->u.gtk.has_show_menubar ||
                                   opts->u.gtk.show_menubar);
    gtk_accel_group_connect(s->accel_group, GDK_KEY_m, HOTKEY_MODIFIERS, 0,
            g_cclosure_new_swap(G_CALLBACK(gd_accel_show_menubar), s, NULL));
    gtk_accel_label_set_accel(
            GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(s->show_menubar_item))),
            GDK_KEY_m, HOTKEY_MODIFIERS);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), s->show_menubar_item);
}

static void gd_create_menus(GtkDisplayState *s, DisplayOptions *opts)
{
    GtkSettings *settings;

    s->accel_group = gtk_accel_group_new();
    s->machine_menu = gd_create_menu_machine(s);
    gd_create_menu_view(s, opts);

    s->machine_menu_item = gtk_menu_item_new_with_mnemonic(_("_Machine"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(s->machine_menu_item),
                              s->machine_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu_bar), s->machine_menu_item);

    s->view_menu_item = gtk_menu_item_new_with_mnemonic(_("_View"));
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(s->view_menu_item), s->view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(s->menu_bar), s->view_menu_item);

    g_object_set_data(G_OBJECT(s->window), "accel_group", s->accel_group);
    gtk_window_add_accel_group(GTK_WINDOW(s->window), s->accel_group);

    /* Disable the default "F10" menu shortcut. */
    settings = gtk_widget_get_settings(s->window);
    g_object_set(G_OBJECT(settings), "gtk-menu-bar-accel", "", NULL);
}


static void gd_vc_free(void *p)
{
    VirtualConsole *vc = p;

    switch (vc->type) {
    case GD_VC_GFX:
        qemu_console_unregister_listener(&vc->gfx.dcl);
#if defined(CONFIG_OPENGL)
        if (display_opengl) {
            qemu_console_set_display_gl_ctx(vc->gfx.dcl.con, NULL);
        }
        if (vc->gfx.ectx) {
            eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                           vc->gfx.esurface, vc->gfx.ectx);
        } else if (gtk_use_gl_area) {
            gtk_gl_area_make_current(GTK_GL_AREA(vc->gfx.drawing_area));
        }
        if (vc->gfx.gls) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
            qemu_gl_fini_shader(vc->gfx.gls);
        }
        egl_fb_destroy(&vc->gfx.guest_fb);
        egl_fb_destroy(&vc->gfx.win_fb);
        egl_fb_destroy(&vc->gfx.cursor_fb);
        if (vc->gfx.esurface) {
            eglDestroySurface(qemu_egl_display, vc->gfx.esurface);
        }
        if (vc->gfx.ectx) {
            eglDestroyContext(qemu_egl_display, vc->gfx.ectx);
        }
#endif
        qkbd_state_free(vc->gfx.kbd);
        if (vc->gfx.surface) {
            cairo_surface_destroy(vc->gfx.surface);
        }
        if (vc->gfx.convert) {
            pixman_image_unref(vc->gfx.convert);
        }
        break;
    case GD_VC_VTE:
#ifdef CONFIG_VTE
        fifo8_destroy(&vc->vte.out_fifo);
#endif
        break;
    }

    if (vc->window) {
        gtk_widget_destroy(vc->window);
    } else if (vc->tab_item) {
        gtk_widget_destroy(vc->tab_item);
    }
    if (vc->menu_item) {
        gtk_widget_destroy(vc->menu_item);
    }
    g_free(vc->label);
    g_free(vc);
}

static GtkDisplayState *gtk_display_state;
static gboolean gtkinit;

static void gtk_display_init(DisplayState *ds, DisplayOptions *opts)
{
    VirtualConsole *vc;

    GtkDisplayState *s;
    GdkDisplay *window_display;
    GtkIconTheme *theme;
    char *dir;
    int idx;

    if (!gtkinit) {
        fprintf(stderr, "gtk initialization failed\n");
        exit(1);
    }
    assert(opts->type == DISPLAY_TYPE_GTK);
    s = g_malloc0(sizeof(*s));
    gtk_display_state = s;
    s->vcs = g_ptr_array_new_with_free_func(gd_vc_free);
    s->opts = opts;

    theme = gtk_icon_theme_get_default();
    dir = get_relocated_path(CONFIG_QEMU_ICONDIR);
    gtk_icon_theme_prepend_search_path(theme, dir);
    g_free(dir);
    g_set_prgname("qemu");

    s->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    s->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    s->notebook = gtk_notebook_new();
    s->menu_bar = gtk_menu_bar_new();

    s->free_scale = FALSE;

    /* Mostly LC_MESSAGES only. See early_gtk_display_init() for details. For
     * LC_CTYPE, we need to make sure that non-ASCII characters are considered
     * printable, but without changing any of the character classes to make
     * sure that we don't accidentally break implicit assumptions.  */
    setlocale(LC_MESSAGES, "");
    setlocale(LC_CTYPE, "C.UTF-8");
    dir = get_relocated_path(CONFIG_QEMU_LOCALEDIR);
    bindtextdomain("qemu", dir);
    g_free(dir);
    bind_textdomain_codeset("qemu", "UTF-8");
    textdomain("qemu");

    window_display = gtk_widget_get_display(s->window);
    if (s->opts->has_show_cursor && s->opts->show_cursor) {
        s->null_cursor = NULL; /* default pointer */
    } else {
        s->null_cursor = gdk_cursor_new_for_display(window_display,
                                                    GDK_BLANK_CURSOR);
    }

    s->mouse_mode_notifier.notify = gd_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&s->mouse_mode_notifier);
    s->vmse = qemu_add_vm_change_state_handler(gd_change_runstate, s);

    {
        g_autoptr(GdkPixbuf) logo = gd_asoft_logo(256);

        if (logo) {
            gtk_window_set_icon(GTK_WINDOW(s->window), logo);
        } else {
            gtk_window_set_icon_name(GTK_WINDOW(s->window), "qemu");
        }
    }

    gd_create_menus(s, opts);

    s->console_notifier.notify = gd_console_notify;
    qemu_console_add_notifier(&s->console_notifier);

    gd_connect_signals(s);

    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(s->notebook), FALSE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(s->notebook), FALSE);

    gd_update_caption(s);

    s->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    s->quick_boot = opts->u.gtk.has_quick_boot && opts->u.gtk.quick_boot;
    if (s->quick_boot && runstate_is_running()) {
        /* -loadvm resumed the machine before the display was up */
        s->quick_cleared = true;
        g_idle_add(gd_quick_clear, s);
    }
    if (opts->u.gtk.has_sidebar && opts->u.gtk.sidebar) {
        gd_create_sidebar(s);
        gd_create_screen_off(s);
        gd_create_busy(s);
        s->overlay = gtk_overlay_new();
        gtk_container_add(GTK_CONTAINER(s->overlay), s->notebook);
        gtk_overlay_add_overlay(GTK_OVERLAY(s->overlay), s->screen_off);
        gtk_overlay_add_overlay(GTK_OVERLAY(s->overlay), s->busy);
        gtk_box_pack_start(GTK_BOX(s->hbox), s->overlay, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(s->hbox), s->sidebar, FALSE, FALSE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(s->hbox), s->notebook, TRUE, TRUE, 0);
    }

    gtk_box_pack_start(GTK_BOX(s->vbox), s->menu_bar, FALSE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(s->vbox), s->hbox, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(s->window), s->vbox);

    gtk_widget_show_all(s->window);
#ifdef GDK_WINDOWING_WIN32
    if (s->sidebar) {
        gd_win32_dark_title(s->window);
    }
#endif

    for (idx = 0; idx < s->vcs->len; idx++) {
        VirtualConsole *v = g_ptr_array_index(s->vcs, idx);
        if (v->type == GD_VC_GFX) {
            gtk_widget_realize(v->gfx.drawing_area);
        }
    }

    if (opts->u.gtk.has_show_menubar &&
        !opts->u.gtk.show_menubar) {
        gtk_widget_hide(s->menu_bar);
    }

    vc = gd_vc_find_current(s);
    gtk_widget_set_sensitive(s->view_menu, vc != NULL);
    g_idle_add(gd_fit_window_idle, s);
#ifdef CONFIG_VTE
    gtk_widget_set_sensitive(s->copy_item,
                             vc && vc->type == GD_VC_VTE);
#endif

    if (opts->has_full_screen &&
        opts->full_screen) {
        gtk_menu_item_activate(GTK_MENU_ITEM(s->full_screen_item));
    }
    if (opts->u.gtk.has_grab_on_hover &&
        opts->u.gtk.grab_on_hover) {
        gtk_menu_item_activate(GTK_MENU_ITEM(s->grab_on_hover_item));
    }
    if (opts->u.gtk.has_show_tabs &&
        opts->u.gtk.show_tabs) {
        gtk_menu_item_activate(GTK_MENU_ITEM(s->show_tabs_item));
    }

    if (opts->u.gtk.has_clipboard &&
        opts->u.gtk.clipboard) {
        gd_clipboard_init(s);
    }

    /* GTK's event polling must happen on the main thread. */
    qemu_main = NULL;
}

static void early_gtk_display_init(DisplayOptions *opts)
{
    /* The QEMU code relies on the assumption that it's always run in
     * the C locale. Therefore it is not prepared to deal with
     * operations that produce different results depending on the
     * locale, such as printf's formatting of decimal numbers, and
     * possibly others.
     *
     * Since GTK+ calls setlocale() by default -importing the locale
     * settings from the environment- we must prevent it from doing so
     * using gtk_disable_setlocale().
     *
     * QEMU's GTK+ UI, however, _does_ have translations for some of
     * the menu items. As a trade-off between a functionally correct
     * QEMU and a fully internationalized UI we support importing
     * LC_MESSAGES from the environment (see the setlocale() call
     * earlier in this file). This allows us to display translated
     * messages leaving everything else untouched.
     */
    gtk_disable_setlocale();
    gtkinit = gtk_init_check(NULL, NULL);
    if (!gtkinit) {
        /* don't exit yet, that'll break -help */
        return;
    }

    assert(opts->type == DISPLAY_TYPE_GTK);
    if (opts->has_gl && opts->gl != DISPLAY_GL_MODE_OFF) {
#if defined(CONFIG_OPENGL)
#if defined(GDK_WINDOWING_WAYLAND)
        if (GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) {
            gtk_use_gl_area = true;
            gtk_gl_area_init();
        } else
#endif
#if defined(GDK_WINDOWING_WIN32)
        if (GDK_IS_WIN32_DISPLAY(gdk_display_get_default())) {
            gtk_use_gl_area = true;
            gtk_gl_area_init();
        } else
#endif
        {
#ifdef CONFIG_X11
            DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAY_GL_MODE_ON;
            gtk_egl_init(mode);
#endif
        }
#endif
    }

    keycode_map = gd_get_keymap(&keycode_maplen, &keycode_xorgevdev);

#if defined(CONFIG_VTE)
    type_register_static(&char_gd_vc_type_info);
#endif
}

static void gtk_display_cleanup(void)
{
    GtkDisplayState *s = gtk_display_state;

    if (!s) {
        return;
    }
    qemu_del_vm_change_state_handler(s->vmse);
    qemu_console_remove_notifier(&s->console_notifier);
    qemu_remove_mouse_mode_change_notifier(&s->mouse_mode_notifier);
    gd_clipboard_cleanup(s);
    g_signal_handlers_disconnect_by_func(s->notebook,
                                         G_CALLBACK(gd_change_page), s);
    g_clear_pointer(&s->vcs, g_ptr_array_unref);
    g_clear_pointer(&s->window, gtk_widget_destroy);
    g_clear_object(&s->null_cursor);
    g_clear_pointer(&gtk_display_state, g_free);
}

static QemuDisplay qemu_display_gtk = {
    .type       = DISPLAY_TYPE_GTK,
    .early_init = early_gtk_display_init,
    .init       = gtk_display_init,
    .cleanup    = gtk_display_cleanup,
    .vc         = "vc",
};

static void register_gtk(void)
{
    qemu_display_register(&qemu_display_gtk);
}

type_init(register_gtk);

#ifdef CONFIG_OPENGL
module_dep("ui-opengl");
#endif
