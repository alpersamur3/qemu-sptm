/*
 * A headset guest's screen in a drawn Apple Vision Pro (ASoftware visionOS), in cairo; ui/gtk.c includes it.
 *
 * The headset is seen from the front, as in Apple's own picture of it: one piece of dark glass in the shape of
 * ski goggles (round ends, a lobe under each eye, the nose bridge between them), a thin polished aluminium frame
 * around it, the Digital Crown and the top button on the frame's top edge (on the wearer's right and left: the
 * picture is what the wearer sees, so the drawing is the wearer's way round), the cameras under the picture.
 * The guest's picture is seen through the glass: it fills the glass and the glass's outline cuts its corners,
 * as a headset's field of view does; the glass's rim lies over the picture's edge.
 *
 * Everything is in units of a hundredth of the picture's height, so the headset is the same object at every
 * window size. Nothing here knows GTK: visor_layout() places the picture in an area, visor_geometry() derives
 * the headset from the picture, visor_paint() draws what lies under the picture and what lies over it (in the
 * picture's own rectangle), visor_paint_knobs() only what a knob's hover, press or turn changes.
 */

#ifndef VISOR_MARK
#define VISOR_MARK(what)        /* a part is done: the prototype times the parts with it */
#endif

#define VISOR_MT 4.6            /* glass above the picture, in units */
#define VISOR_MB 11.5           /* glass below it: the lobes with the cameras, the nose bridge between them */
#define VISOR_ML 2.4            /* glass beside it */
#define VISOR_FRAME 0.85        /* the aluminium frame's width */
#define VISOR_PAD_TOP 5.0       /* above the frame: the crown, the top button, room for the glow */
#define VISOR_PAD_SIDE 4.0
#define VISOR_PAD_BOTTOM 2.0
#define VISOR_LEGEND 34         /* pixels under the headset: what the keys and the mouse do now */

/* The glass's outline in its own box: x from -1 to 1 about the centre line, y from 0 (top) to 1 (the lobes) */
#define VISOR_TX 0.640          /* the top edge's handle */
#define VISOR_SY 0.105          /* the shoulder's handle */
#define VISOR_MY 0.470          /* where the glass is widest */
#define VISOR_LY 0.800          /* the outer lower curve's handles */
#define VISOR_LX1 0.860
#define VISOR_LX 0.560          /* the lobe's lowest point */
#define VISOR_AX1 0.300         /* the nose bridge's handles */
#define VISOR_AX2 0.170

typedef struct VisorKnob {
    double x, y, angle;         /* its foot's centre on the frame's outer edge, and the edge's tilt there */
    double w, h;
} VisorKnob;

typedef struct VisorGeometry {
    double sx, sy, sw, sh;      /* the picture */
    double u;                   /* the unit */
    double cx, hw;              /* the glass: its centre line, half its width */
    double gt, gh;              /* its top at the centre line, its height down to the lobes */
    double ay;                  /* the nose bridge's top, in the outline's box */
    double frame;
    VisorKnob crown, top;
} VisorGeometry;

typedef struct VisorLook {
    int hover, pressed;         /* 1: the crown, 2: the top button */
    bool walk;                  /* walk mode is on */
    bool hold_key;              /* outside walk mode the keyboard leaves the head alone */
    double crown_phase;         /* the crown's turn, in pixels of knurling */
    bool full_screen;           /* the window is full screen: the legend says how to leave it */
} VisorLook;

/* The room the headset takes around a picture of @sh pixels' height: left+right and top+bottom */
static void visor_room(double sh, double *wide, double *high)
{
    double u = sh / 100;

    *wide = 2 * (VISOR_ML + VISOR_FRAME + VISOR_PAD_SIDE) * u;
    *high = (VISOR_MT + VISOR_MB + 2 * VISOR_FRAME + VISOR_PAD_TOP + VISOR_PAD_BOTTOM) * u + VISOR_LEGEND;
}

/* The picture (aspect @aspect, width over height) as large as a w x h area lets the headset be, centred */
static void visor_layout(double w, double h, double aspect, int *sx, int *sy, int *sw, int *sh)
{
    double kx = 2 * (VISOR_ML + VISOR_FRAME + VISOR_PAD_SIDE) / 100;
    double ky = (VISOR_MT + VISOR_MB + 2 * VISOR_FRAME + VISOR_PAD_TOP + VISOR_PAD_BOTTOM) / 100;
    double ph = MIN((h - VISOR_LEGEND) / (1 + ky), w / (aspect + kx)), u, top, total;

    ph = MAX(ph, 40);
    u = ph / 100;
    *sh = (int)floor(ph);
    *sw = (int)floor(ph * aspect);
    *sx = (int)floor((w - *sw) / 2);
    total = ph * (1 + ky) + VISOR_LEGEND;
    top = (h - total) / 2 + (VISOR_PAD_TOP + VISOR_FRAME + VISOR_MT) * u;
    *sy = (int)floor(MAX(top, 0));
}

static void visor_bez(const double p[8], double t, double *x, double *y, double *dx, double *dy)
{
    double m = 1 - t;

    *x = m * m * m * p[0] + 3 * m * m * t * p[2] + 3 * m * t * t * p[4] + t * t * t * p[6];
    *y = m * m * m * p[1] + 3 * m * m * t * p[3] + 3 * m * t * t * p[5] + t * t * t * p[7];
    if (dx) {
        *dx = 3 * m * m * (p[2] - p[0]) + 6 * m * t * (p[4] - p[2]) + 3 * t * t * (p[6] - p[4]);
        *dy = 3 * m * m * (p[3] - p[1]) + 6 * m * t * (p[5] - p[3]) + 3 * t * t * (p[7] - p[5]);
    }
}

/* A knob standing on the top edge at @t of the way from the centre line to the widest point, on side @side */
static void visor_knob(const VisorGeometry *g, double t, int side, double w, double h, VisorKnob *k)
{
    const double top[8] = { 0, 0, VISOR_TX, 0, 1, VISOR_SY, 1, VISOR_MY };
    double x, y, dx, dy, tx, ty, len;

    visor_bez(top, t, &x, &y, &dx, &dy);
    tx = dx * g->hw;
    ty = dy * g->gh * side;
    len = hypot(tx, ty);
    k->angle = atan2(ty, tx);
    /* out to the frame's outer edge: the outward normal of a left-to-right tangent points up */
    k->x = g->cx + side * x * g->hw + ty / len * g->frame;
    k->y = g->gt + y * g->gh - tx / len * g->frame;
    k->w = w;
    k->h = h;
}

static void visor_geometry(double sx, double sy, double sw, double sh, VisorGeometry *g)
{
    g->sx = sx;
    g->sy = sy;
    g->sw = sw;
    g->sh = sh;
    g->u = sh / 100;
    g->cx = sx + sw / 2;
    g->hw = sw / 2 + VISOR_ML * g->u;
    g->gt = sy - VISOR_MT * g->u;
    g->gh = sh + (VISOR_MT + VISOR_MB) * g->u;
    g->frame = MAX(3, VISOR_FRAME * g->u);
    /* the bridge's top stays clear of the picture */
    g->ay = (VISOR_MT * g->u + sh + 2.4 * g->u) / g->gh;
    visor_knob(g, 0.335, 1, 6.0 * g->u, 2.0 * g->u, &g->crown);
    visor_knob(g, 0.300, -1, 10.5 * g->u, 0.95 * g->u, &g->top);
}

/* The glass's outline, clockwise from the top of the centre line */
static void visor_outline(cairo_t *cr, const VisorGeometry *g)
{
#define VX(x) (g->cx + (x) * g->hw)
#define VY(y) (g->gt + (y) * g->gh)
    cairo_new_sub_path(cr);
    cairo_move_to(cr, VX(0), VY(0));
    cairo_curve_to(cr, VX(VISOR_TX), VY(0), VX(1), VY(VISOR_SY), VX(1), VY(VISOR_MY));
    cairo_curve_to(cr, VX(1), VY(VISOR_LY), VX(VISOR_LX1), VY(1), VX(VISOR_LX), VY(1));
    cairo_curve_to(cr, VX(VISOR_AX1), VY(1), VX(VISOR_AX2), VY(g->ay), VX(0), VY(g->ay));
    cairo_curve_to(cr, VX(-VISOR_AX2), VY(g->ay), VX(-VISOR_AX1), VY(1), VX(-VISOR_LX), VY(1));
    cairo_curve_to(cr, VX(-VISOR_LX1), VY(1), VX(-1), VY(VISOR_LY), VX(-1), VY(VISOR_MY));
    cairo_curve_to(cr, VX(-1), VY(VISOR_SY), VX(-VISOR_TX), VY(0), VX(0), VY(0));
    cairo_close_path(cr);
}

static void visor_round_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = MIN(r, MIN(w, h) / 2);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* The mode's light: Apple's blue through to violet, left to right across the glass */
static cairo_pattern_t *visor_light(const VisorGeometry *g, double alpha)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(g->cx - g->hw, 0, g->cx + g->hw, 0);

    cairo_pattern_add_color_stop_rgba(p, 0.0, 0.16, 0.56, 1.00, alpha);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 0.38, 0.45, 1.00, alpha);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0.66, 0.36, 1.00, alpha);
    return p;
}

/* Polished aluminium: the room's light and dark bands, running across the frame at a slant */
static cairo_pattern_t *visor_aluminium(const VisorGeometry *g, double lit)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(g->cx - g->hw * 0.35, g->gt - g->frame,
                                                     g->cx + g->hw * 0.35, g->gt + g->gh + g->frame);
    static const double stops[][2] = {
        { 0.00, 0.97 }, { 0.10, 0.80 }, { 0.26, 0.56 }, { 0.42, 0.70 }, { 0.58, 0.46 },
        { 0.76, 0.62 }, { 0.90, 0.86 }, { 1.00, 0.52 },
    };
    int i;

    for (i = 0; i < G_N_ELEMENTS(stops); i++) {
        double v = MIN(1, stops[i][1] + lit);

        cairo_pattern_add_color_stop_rgb(p, stops[i][0], v, v, v + 0.02);
    }
    return p;
}

/* The Digital Crown from the front: a knurled wheel standing on the frame, seen side-on */
static void visor_draw_crown(cairo_t *cr, const VisorGeometry *g, const VisorLook *l)
{
    const VisorKnob *k = &g->crown;
    double lit = l->pressed == 1 ? -0.10 : l->hover == 1 ? 0.10 : 0, u = g->u;
    double down = l->pressed == 1 ? 0.45 * u : 0, gap = MAX(2.6, 0.46 * u), x;
    cairo_pattern_t *p;

    cairo_save(cr);
    cairo_translate(cr, k->x, k->y);
    cairo_rotate(cr, k->angle);
    /* the collar it sits in */
    cairo_set_source_rgb(cr, 0.30 + lit, 0.30 + lit, 0.33 + lit);
    visor_round_rect(cr, -k->w / 2 - 0.35 * u, -0.55 * u, k->w + 0.7 * u, 0.9 * u, 0.3 * u);
    cairo_fill(cr);
    /* the wheel: light from the upper left, dark towards both sides as it turns away */
    p = cairo_pattern_create_linear(-k->w / 2, 0, k->w / 2, 0);
    cairo_pattern_add_color_stop_rgb(p, 0.00, 0.24 + lit, 0.24 + lit, 0.27 + lit);
    cairo_pattern_add_color_stop_rgb(p, 0.22, 0.70 + lit, 0.70 + lit, 0.73 + lit);
    cairo_pattern_add_color_stop_rgb(p, 0.40, 0.93 + lit, 0.93 + lit, 0.95 + lit);
    cairo_pattern_add_color_stop_rgb(p, 0.68, 0.62 + lit, 0.62 + lit, 0.65 + lit);
    cairo_pattern_add_color_stop_rgb(p, 1.00, 0.20 + lit, 0.20 + lit, 0.23 + lit);
    cairo_set_source(cr, p);
    visor_round_rect(cr, -k->w / 2, -k->h + down, k->w, k->h, 0.55 * u);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_set_line_width(cr, MAX(1, 0.16 * u));
    for (x = -k->w / 2 + fmod(fmod(l->crown_phase, gap) + gap, gap); x < k->w / 2; x += gap) {
        double edge = 1 - fabs(x) / (k->w / 2);

        cairo_set_source_rgba(cr, 0, 0, 0, 0.10 + 0.34 * MIN(1, edge * 1.6));
        cairo_move_to(cr, x, -k->h + down + 0.35 * u);
        cairo_line_to(cr, x, down);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    /* its cap's edge catches the light */
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
    cairo_move_to(cr, -k->w / 2 + 0.6 * u, -k->h + down + 0.5);
    cairo_line_to(cr, k->w / 2 - 0.6 * u, -k->h + down + 0.5);
    cairo_stroke(cr);
    cairo_restore(cr);
}

/* The top button: a long low key on the other side of the frame's top */
static void visor_draw_top(cairo_t *cr, const VisorGeometry *g, const VisorLook *l)
{
    const VisorKnob *k = &g->top;
    double lit = l->pressed == 2 ? -0.10 : l->hover == 2 ? 0.10 : 0;
    double down = l->pressed == 2 ? 0.5 * g->u : 0;
    cairo_pattern_t *p = cairo_pattern_create_linear(0, -k->h, 0, 0);

    cairo_save(cr);
    cairo_translate(cr, k->x, k->y);
    cairo_rotate(cr, k->angle);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.96 + lit, 0.96 + lit, 0.98 + lit);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.58 + lit, 0.58 + lit, 0.61 + lit);
    cairo_set_source(cr, p);
    visor_round_rect(cr, -k->w / 2, -k->h + down, k->w, k->h + 0.5 * g->u, 0.5 * g->u);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_restore(cr);
}

/* A camera behind the glass: a dark barrel, the lens's blue coating, one glint */
static void visor_draw_lens(cairo_t *cr, double x, double y, double r)
{
    cairo_pattern_t *p = cairo_pattern_create_radial(x - r * 0.3, y - r * 0.4, r * 0.1, x, y, r);

    cairo_pattern_add_color_stop_rgb(p, 0, 0.20, 0.215, 0.26);
    cairo_pattern_add_color_stop_rgb(p, 1, 0.045, 0.05, 0.065);
    cairo_set_source(cr, p);
    cairo_arc(cr, x, y, r, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_set_line_width(cr, MAX(1, r * 0.09));
    cairo_set_source_rgba(cr, 0.66, 0.74, 0.88, 0.34);
    cairo_arc(cr, x, y, r - 0.5, 0, 2 * G_PI);
    cairo_stroke(cr);
    p = cairo_pattern_create_radial(x + r * 0.12, y + r * 0.16, 0, x, y, r * 0.56);
    cairo_pattern_add_color_stop_rgb(p, 0, 0.14, 0.22, 0.46);
    cairo_pattern_add_color_stop_rgb(p, 0.7, 0.025, 0.035, 0.09);
    cairo_pattern_add_color_stop_rgb(p, 1, 0, 0, 0);
    cairo_set_source(cr, p);
    cairo_arc(cr, x, y, r * 0.56, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.42);
    cairo_arc(cr, x - r * 0.24, y - r * 0.26, MAX(0.8, r * 0.10), 0, 2 * G_PI);
    cairo_fill(cr);
}

/* One key of the legend; returns its width (drawn only with @cr) */
static double visor_keycap(cairo_t *cr, PangoLayout *layout, const char *name, double x, double cy, bool lit)
{
    int tw, th;
    double w, h = 19;

    pango_layout_set_text(layout, name, -1);
    pango_layout_get_pixel_size(layout, &tw, &th);
    w = MAX(h, tw + 11);
    if (cr) {
        visor_round_rect(cr, x + 0.5, cy - h / 2 + 0.5, w - 1, h - 1, 4.5);
        cairo_set_source_rgba(cr, 1, 1, 1, lit ? 0.16 : 0.055);
        cairo_fill_preserve(cr);
        cairo_set_line_width(cr, 1);
        cairo_set_source_rgba(cr, 1, 1, 1, lit ? 0.50 : 0.20);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, lit ? 0.95 : 0.74);
        cairo_move_to(cr, x + (w - tw) / 2.0, cy - th / 2.0 - 0.5);
        pango_cairo_show_layout(cr, layout);
    }
    return w;
}

/* A mouse with one of its buttons lit (1 left, 2 middle, 3 right); returns its width */
static double visor_mouse(cairo_t *cr, double x, double cy, int button)
{
    double w = 13, h = 19, y = cy - h / 2;

    if (cr) {
        cairo_set_line_width(cr, 1);
        cairo_save(cr);
        visor_round_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 6);
        cairo_clip(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.055);
        cairo_paint(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.88);
        if (button == 1) {
            cairo_rectangle(cr, x, y, w / 2, h * 0.42);
            cairo_fill(cr);
        } else if (button == 3) {
            cairo_rectangle(cr, x + w / 2, y, w / 2, h * 0.42);
            cairo_fill(cr);
        }
        cairo_restore(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.34);
        visor_round_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 6);
        cairo_stroke(cr);
        cairo_move_to(cr, x + 0.5, y + h * 0.42 + 0.5);
        cairo_line_to(cr, x + w - 0.5, y + h * 0.42 + 0.5);
        cairo_stroke(cr);
        if (button != 2) {
            cairo_move_to(cr, x + w / 2, y + 1);
            cairo_line_to(cr, x + w / 2, y + h * 0.42);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.92);
            visor_round_rect(cr, x + w / 2 - 1.5, y + 2.5, 3, 6, 1.5);
            cairo_fill(cr);
        }
    }
    return w;
}

/*
 * The legend's items: "k:NAME" a key, "m:N" the mouse with button N lit, "t:TEXT" what they do, "s" a space
 * between two groups. Returns the row's width; draws it from @x with @cr.
 */
static double visor_legend_row(cairo_t *cr, PangoLayout *keys, PangoLayout *text, const char *const *items,
                               double x, double cy)
{
    double x0 = x;
    int tw, th, i;

    for (i = 0; items[i]; i++) {
        const char *it = items[i];

        if (it[0] == 'k') {
            x += visor_keycap(cr, keys, it + 2, x, cy, false) + 3;
        } else if (it[0] == 'm') {
            x += visor_mouse(cr, x, cy, it[2] - '0') + 3;
        } else if (it[0] == 't') {
            pango_layout_set_text(text, it + 2, -1);
            pango_layout_get_pixel_size(text, &tw, &th);
            if (cr) {
                cairo_set_source_rgba(cr, 1, 1, 1, 0.52);
                cairo_move_to(cr, x + 4, cy - th / 2.0 - 0.5);
                pango_cairo_show_layout(cr, text);
            }
            x += tw + 4;
        } else {
            x += 20;
        }
    }
    return x - x0;
}

/* Under the headset: the mode in the nose bridge's hollow, and under it what the keys and the mouse do */
static void visor_draw_legend(cairo_t *cr, const VisorGeometry *g, const VisorLook *l, PangoContext *pc)
{
    static const char *const walk[] = { "k:W", "k:A", "k:S", "k:D", "t:walk", "s", "k:R", "k:F", "t:up, down", "s",
                                        "k:Shift", "t:turn", "s", "m:1", "t:tap", "s", "m:2", "t:leave", NULL };
    static const char *const type[] = { "m:3", "t:hold to look around and walk", "s", "m:2", "t:walk mode", NULL };
    static const char *const both[] = { "k:W", "k:A", "k:S", "k:D", "k:R", "k:F", "k:Shift", "t:move the head too",
                                        "s", "m:2", "t:walk mode: the mouse looks", NULL };
    static const char *const brief[] = { "m:2", "t:walk mode", NULL };
    static const char *const leave[] = { "s", "k:F11", "t:leave full screen", "s", "k:F9", "t:hide the headset",
                                         NULL };
    const char *const *row = l->walk ? walk : l->hold_key ? type : both;
    const char *items[32];
    PangoLayout *keys = pango_layout_new(pc), *text = pango_layout_new(pc), *mode = pango_layout_new(pc);
    PangoFontDescription *fd = pango_font_description_copy(pango_context_get_font_description(pc));
    double cy = g->gt + g->gh + g->frame + VISOR_PAD_BOTTOM * g->u + VISOR_LEGEND / 2.0, w;
    double hollow = (1 - g->ay) * g->gh, my;
    cairo_pattern_t *p;
    int tw, th, n, i;

    pango_font_description_set_absolute_size(fd, 10.5 * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(keys, fd);
    pango_font_description_set_absolute_size(fd, 12 * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(text, fd);
    pango_font_description_set_absolute_size(fd, MAX(11, MIN(13.5, hollow * 0.30)) * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(mode, fd);
    pango_font_description_free(fd);

    /* full screen, how to leave it too: the side bar is not there */
    for (n = 0; row[n]; n++) {
        items[n] = row[n];
    }
    for (i = 0; l->full_screen && leave[i]; i++) {
        items[n++] = leave[i];
    }
    items[n] = NULL;
    row = items;
    w = visor_legend_row(NULL, keys, text, row, 0, 0);
    if (w > 2 * g->hw) {
        row = brief;            /* a narrow window */
        w = visor_legend_row(NULL, keys, text, row, 0, 0);
    }
    visor_legend_row(cr, keys, text, row, floor(g->cx - w / 2), floor(cy));

    pango_layout_set_text(mode, l->walk ? "Walk mode" : "Typing", -1);
    pango_layout_get_pixel_size(mode, &tw, &th);
    my = g->gt + g->gh + g->frame - hollow * 0.40 - th / 2.0;
    if (hollow > th * 1.7) {
        if (l->walk) {
            p = visor_light(g, 1);
            cairo_set_source(cr, p);
            cairo_pattern_destroy(p);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.62);
        }
        cairo_move_to(cr, floor(g->cx - tw / 2.0), floor(my));
        pango_cairo_show_layout(cr, mode);
    }
    g_object_unref(keys);
    g_object_unref(text);
    g_object_unref(mode);
}

/* The glass's lower edge at @x (-1 to 1 in the outline's box): its y in the box */
static double visor_lower_edge(const VisorGeometry *g, double x)
{
    const double lobe[8] = { 1, VISOR_MY, 1, VISOR_LY, VISOR_LX1, 1, VISOR_LX, 1 };
    const double bridge[8] = { VISOR_LX, 1, VISOR_AX1, 1, VISOR_AX2, g->ay, 0, g->ay };
    const double *seg = fabs(x) > VISOR_LX ? lobe : bridge;
    double lo = 0, hi = 1, bx, by;
    int i;

    for (i = 0; i < 24; i++) {
        double t = (lo + hi) / 2;

        visor_bez(seg, t, &bx, &by, NULL, NULL);
        if (bx > fabs(x)) {     /* both run from the outside in */
            lo = t;
        } else {
            hi = t;
        }
    }
    visor_bez(seg, (lo + hi) / 2, &bx, &by, NULL, NULL);
    return by;
}

/* The glass's upper edge at @x (-1 to 1 in the outline's box): its y in the box */
static double visor_upper_edge(double x)
{
    const double top[8] = { 0, 0, VISOR_TX, 0, 1, VISOR_SY, 1, VISOR_MY };
    double lo = 0, hi = 1, bx, by;
    int i;

    for (i = 0; i < 24; i++) {
        double t = (lo + hi) / 2;

        visor_bez(top, t, &bx, &by, NULL, NULL);
        if (bx < fabs(x)) {     /* it runs from the centre out */
            lo = t;
        } else {
            hi = t;
        }
    }
    visor_bez(top, (lo + hi) / 2, &bx, &by, NULL, NULL);
    return by;
}

/*
 * The soft bands along the glass's outline: the rim's shade and its light, in walk mode the glow past the frame.
 * A band reaches @span units to each side of the outline, @peak of it on the outline itself, and falls off with
 * the distance to the power @curve: the falloff of VISOR_STEPS strokes of growing width laid over each other,
 * smoothed. The distance to the outline is worked out once, on a grid of VISOR_CELL units and only where the
 * pixels being painted read it; a band's value at a cell is its profile averaged over the cell (as a stroke's
 * coverage of it would be) and the pixels between the cells' centres take it bilinearly. The band's source is
 * laid through it once, into one premultiplied layer (many faint layers of a gradient would show the 8 bits'
 * steps), and only the pixels the band reaches are touched.
 */
#define VISOR_CELL 4
#define VISOR_STEPS 14
#define VISOR_TSTEP 8           /* a band's profile: entries per unit of distance */

/* Where the visor's user space lies on the under layer's pixels: pixel = user * scale + origin, nothing turns */
typedef struct VisorMap {
    double scale, ox, oy;
} VisorMap;

/* A rectangle of the under layer's pixels, x1 and y1 excluded */
typedef struct VisorRect {
    int x0, y0, x1, y1;
} VisorRect;

/* The distance to the outline at the centres of a block of the grid: cell (i, j)'s lies at the grid's corner
 * plus VISOR_CELL * (i + 0.5, j + 0.5) */
typedef struct VisorField {
    double x0, y0;              /* the grid's corner, in user space */
    int i0, j0, w, h;           /* the block */
    float *d;
} VisorField;

/* A band or a line to lay along the outline, and its source */
typedef struct VisorSource {
    float *cell;                /* a band: its value at the field's cells; or NULL, a line: */
    cairo_surface_t *line;      /* its coverage (A8, on the patch's pixels), laid at @alpha (0 to 255) */
    guint32 alpha;
    double end;                 /* how far from the outline it is still there */
    guint32 colour;             /* the source, premultiplied ARGB: one colour, or a gradient's colours along its */
    guint32 *strip;             /* direction, one per pixel row (@by_row) or column of the patch */
    bool by_row;
} VisorSource;

static void visor_map(cairo_t *cr, VisorMap *m)
{
    double x = 0, y = 0, dx = 1, dy = 0, sx, sy, ox, oy;
    cairo_surface_t *target = cairo_get_group_target(cr);

    /* cairo's "device" space is the CTM's; the surface's own device scale and offset come after it */
    cairo_user_to_device(cr, &x, &y);
    cairo_user_to_device_distance(cr, &dx, &dy);
    cairo_surface_get_device_scale(target, &sx, &sy);
    cairo_surface_get_device_offset(target, &ox, &oy);
    m->scale = dx * sx;
    m->ox = x * sx + ox;
    m->oy = y * sy + oy;
}

/* The pixels a box of user space touches, cut down to @within (if any); false when nothing is left */
static bool visor_rect(const VisorMap *m, double x0, double y0, double x1, double y1, const VisorRect *within,
                       VisorRect *r)
{
    r->x0 = (int)floor(x0 * m->scale + m->ox + 1e-6);
    r->y0 = (int)floor(y0 * m->scale + m->oy + 1e-6);
    r->x1 = (int)ceil(x1 * m->scale + m->ox - 1e-6);
    r->y1 = (int)ceil(y1 * m->scale + m->oy - 1e-6);
    if (within) {
        r->x0 = MAX(r->x0, within->x0);
        r->y0 = MAX(r->y0, within->y0);
        r->x1 = MIN(r->x1, within->x1);
        r->y1 = MIN(r->y1, within->y1);
    }
    return r->x0 < r->x1 && r->y0 < r->y1;
}

/*
 * Memory a repaint keeps for the next one, for its patches and grids: in a large window, fresh memory's page
 * faults cost more than the painting in it. visor_scratch_free() gives it back.
 */
typedef enum VisorSlot {
    VISOR_FIELD, VISOR_DARK, VISOR_LIGHT, VISOR_GLOW_BAND, VISOR_INSIDE, VISOR_GLOW, VISOR_GLASS, VISOR_LINE,
    VISOR_SLOTS
} VisorSlot;

typedef struct VisorScratch {
    void *mem[VISOR_SLOTS];
    size_t size[VISOR_SLOTS];
} VisorScratch;

static void visor_scratch_free(VisorScratch *sc)
{
    int i;

    for (i = 0; i < VISOR_SLOTS; i++) {
        g_clear_pointer(&sc->mem[i], g_free);
        sc->size[i] = 0;
    }
}

/* @size bytes of @slot (a window resized a step at a time does not make it grow each time) */
static void *visor_keep(VisorScratch *sc, VisorSlot slot, size_t size)
{
    if (sc->size[slot] < size) {
        g_free(sc->mem[slot]);
        sc->size[slot] = size + size / 4;
        sc->mem[slot] = g_malloc(sc->size[slot]);
    }
    return sc->mem[slot];
}

/*
 * An image surface on @r's pixels, clear, drawn on in the visor's user space and laid back with its origin at
 * 0, 0; in @slot of @sc (when not NULL), so that it is gone when the slot is used again.
 */
static cairo_surface_t *visor_patch(VisorScratch *sc, VisorSlot slot, const VisorMap *m, const VisorRect *r,
                                    cairo_format_t format)
{
    int w = r->x1 - r->x0, h = r->y1 - r->y0, stride = cairo_format_stride_for_width(format, w);
    cairo_surface_t *s;

    if (sc) {
        guint8 *mem = visor_keep(sc, slot, (size_t)stride * h);

        memset(mem, 0, (size_t)stride * h);
        s = cairo_image_surface_create_for_data(mem, format, w, h, stride);
    } else {
        s = cairo_image_surface_create(format, w, h);
    }
    cairo_surface_set_device_scale(s, m->scale, m->scale);
    cairo_surface_set_device_offset(s, m->ox - r->x0, m->oy - r->y0);
    return s;
}

/* A gradient's colours along @r's rows (@by_row) or columns, exactly as cairo paints them on those pixels */
static guint32 *visor_strip(cairo_pattern_t *p, const VisorMap *m, const VisorRect *r, bool by_row)
{
    VisorRect line = { r->x0, r->y0, by_row ? r->x0 + 1 : r->x1, by_row ? r->y1 : r->y0 + 1 };
    int n = by_row ? r->y1 - r->y0 : r->x1 - r->x0, i;
    cairo_surface_t *s = visor_patch(NULL, 0, m, &line, CAIRO_FORMAT_ARGB32);
    cairo_t *cr = cairo_create(s);
    guint32 *strip = g_new(guint32, n);
    guint8 *data;
    int stride;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    data = cairo_image_surface_get_data(s);
    stride = cairo_image_surface_get_stride(s);
    for (i = 0; i < n; i++) {
        strip[i] = by_row ? *(guint32 *)(data + i * stride) : ((guint32 *)data)[i];
    }
    cairo_surface_destroy(s);
    return strip;
}

/* How far from the outline a band is still there, in user space */
static double visor_band_end(const VisorGeometry *g, double span, double curve)
{
    return span * g->u * pow(1 + 0.5 / VISOR_STEPS, curve) + VISOR_CELL / 2.0;
}

static void visor_field_segment(VisorField *f, double ax, double ay, double bx, double by, double reach)
{
    double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy, inv = len > 0 ? 1 / len : 0;
    int i0 = MAX(f->i0, (int)floor((MIN(ax, bx) - reach - f->x0) / VISOR_CELL - 0.5));
    int i1 = MIN(f->i0 + f->w - 1, (int)ceil((MAX(ax, bx) + reach - f->x0) / VISOR_CELL - 0.5));
    int j0 = MAX(f->j0, (int)floor((MIN(ay, by) - reach - f->y0) / VISOR_CELL - 0.5));
    int j1 = MIN(f->j0 + f->h - 1, (int)ceil((MAX(ay, by) + reach - f->y0) / VISOR_CELL - 0.5));
    int i, j;

    for (j = j0; j <= j1; j++) {
        double py = f->y0 + (j + 0.5) * VISOR_CELL - ay;
        float *row = f->d + (j - f->j0) * f->w - f->i0;

        for (i = i0; i <= i1; i++) {
            double px = f->x0 + (i + 0.5) * VISOR_CELL - ax, t = (px * dx + py * dy) * inv, ex, ey, d;

            t = t < 0 ? 0 : t > 1 ? 1 : t;
            ex = px - t * dx;
            ey = py - t * dy;
            d = ex * ex + ey * ey;
            if (d < row[i]) {
                row[i] = d;
            }
        }
    }
}

/* The distance to the outline (as far as @reach) at the cells the pixels of @r read */
static void visor_field(VisorField *f, const VisorGeometry *g, double reach, const VisorMap *m, const VisorRect *r,
                        VisorScratch *sc)
{
    double ux0 = (r->x0 - m->ox) / m->scale, uy0 = (r->y0 - m->oy) / m->scale;
    double ux1 = (r->x1 - m->ox) / m->scale, uy1 = (r->y1 - m->oy) / m->scale, ax = 0, ay = 0, sx = 0, sy = 0;
    cairo_surface_t *one = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *cr = cairo_create(one);
    cairo_path_t *path;
    int k;

    f->x0 = floor(g->cx - g->hw - reach) - VISOR_CELL;
    f->y0 = floor(g->gt - reach) - VISOR_CELL;
    f->i0 = (int)floor((ux0 - f->x0) / VISOR_CELL - 0.5);
    f->j0 = (int)floor((uy0 - f->y0) / VISOR_CELL - 0.5);
    f->w = (int)floor((ux1 - f->x0) / VISOR_CELL - 0.5) + 2 - f->i0;
    f->h = (int)floor((uy1 - f->y0) / VISOR_CELL - 0.5) + 2 - f->j0;
    f->d = visor_keep(sc, VISOR_FIELD, sizeof(float) * f->w * f->h);
    for (k = 0; k < f->w * f->h; k++) {
        f->d[k] = reach * reach;
    }
    /* the outline as lines, within a quarter of a pixel of it */
    cairo_set_tolerance(cr, 0.25);
    visor_outline(cr, g);
    path = cairo_copy_path_flat(cr);
    for (k = 0; k < path->num_data; k += path->data[k].header.length) {
        cairo_path_data_t *p = &path->data[k];

        if (p->header.type == CAIRO_PATH_MOVE_TO) {
            sx = ax = p[1].point.x;
            sy = ay = p[1].point.y;
        } else if (p->header.type == CAIRO_PATH_LINE_TO) {
            visor_field_segment(f, ax, ay, p[1].point.x, p[1].point.y, reach);
            ax = p[1].point.x;
            ay = p[1].point.y;
        } else if (p->header.type == CAIRO_PATH_CLOSE_PATH) {
            visor_field_segment(f, ax, ay, sx, sy, reach);
            ax = sx;
            ay = sy;
        }
    }
    cairo_path_destroy(path);
    cairo_destroy(cr);
    cairo_surface_destroy(one);
    for (k = 0; k < f->w * f->h; k++) {
        f->d[k] = sqrtf(f->d[k]);
    }
}

/* A band's value at each of the field's cells, in @slot of @sc */
static float *visor_band(const VisorField *f, const VisorGeometry *g, double span, double peak, double curve,
                         VisorScratch *sc, VisorSlot slot)
{
    double reach = span * g->u, h = 1.0 / VISOR_TSTEP;
    int win = VISOR_CELL * VISOR_TSTEP, n = (int)ceil(visor_band_end(g, span, curve) * VISOR_TSTEP) + 2, k;
    double *sum = g_new(double, n + win + 1);
    float *table = g_new(float, n), *cell = visor_keep(sc, slot, sizeof(float) * f->w * f->h);

    /* the profile, summed up from half a cell inside the outline so that a cell's average is a difference */
    sum[0] = 0;
    for (k = 0; k < n + win; k++) {
        double x = fabs((k + 0.5) * h - VISOR_CELL / 2.0);
        double e = VISOR_STEPS * (1 - pow(x / reach, 1 / curve)) + 0.5;

        e = MAX(0, MIN(VISOR_STEPS, e)) / VISOR_STEPS;
        sum[k + 1] = sum[k] + 1 - pow(1 - peak, e);
    }
    for (k = 0; k < n; k++) {
        table[k] = (sum[k + win] - sum[k]) / win;
    }
    for (k = 0; k < f->w * f->h; k++) {
        float x = f->d[k] * VISOR_TSTEP;
        int i = (int)x;

        cell[k] = i < n - 1 ? table[i] + (table[i + 1] - table[i]) * (x - i) : 0;
    }
    g_free(sum);
    g_free(table);
    return cell;
}

/* Each byte of @x times @a / 255, rounded as pixman rounds it */
static inline guint32 visor_mul(guint32 x, guint32 a)
{
    guint32 rb = (x & 0xff00ff) * a + 0x800080, ag = ((x >> 8) & 0xff00ff) * a + 0x800080;

    rb = ((rb + ((rb >> 8) & 0xff00ff)) >> 8) & 0xff00ff;
    ag = (ag + ((ag >> 8) & 0xff00ff)) & 0xff00ff00;
    return rb | ag;
}

/* @s over @d, both premultiplied ARGB */
static inline guint32 visor_over(guint32 d, guint32 s)
{
    return s + visor_mul(d, 255 - (s >> 24));
}

/*
 * Lays @n bands and lines, in order, over the ARGB32 patch @dst on @r, as cairo would lay their sources through
 * them. @inside is the outline's coverage (A8, on @ir, which holds @r): with @clip they are cut to it, as a clip
 * to the outline cuts them; without, the pixels it covers whole are left alone (what lies there is painted over
 * later). Only the pixels near enough the outline for one of them are visited.
 */
static void visor_lay(cairo_surface_t *dst, const VisorRect *r, const VisorMap *m, const VisorField *f,
                      const VisorSource *src, int n, cairo_surface_t *inside, const VisorRect *ir, bool clip)
{
    int w = r->x1 - r->x0, ds, cs, ls[4], runs = 0, nclose = 0, last = -1, x, y, k, q;
    /* the columns' runs between two cells' centres (the same on every row), how far each column is along, and
     * the runs near enough the outline on the cells' row of the moment */
    int *run = g_new(int, w + 1), *cell = g_new(int, w), *closer = g_new(int, w);
    float *along = g_new(float, w);
    double reach = 0;
    guint8 *dp, *cp, *lp[4];

    for (x = 0; x < w; x++) {
        double gx = ((r->x0 + x + 0.5 - m->ox) / m->scale - f->x0) / VISOR_CELL - 0.5;

        cell[x] = (int)floor(gx) - f->i0;
        along[x] = gx - floor(gx);
        if (!x || cell[x] != cell[x - 1]) {
            run[runs++] = x;
        }
    }
    run[runs] = w;
    for (k = 0; k < n; k++) {
        reach = MAX(reach, src[k].end);
        if (src[k].line) {
            cairo_surface_flush(src[k].line);
            lp[k] = cairo_image_surface_get_data(src[k].line);
            ls[k] = cairo_image_surface_get_stride(src[k].line);
        }
    }
    cairo_surface_flush(dst);
    cairo_surface_flush(inside);
    dp = cairo_image_surface_get_data(dst);
    ds = cairo_image_surface_get_stride(dst);
    cp = cairo_image_surface_get_data(inside);
    cs = cairo_image_surface_get_stride(inside);
    for (y = r->y0; y < r->y1; y++) {
        double gy = ((y + 0.5 - m->oy) / m->scale - f->y0) / VISOR_CELL - 0.5;
        int j = (int)floor(gy) - f->j0;
        float fy = gy - floor(gy);
        guint32 *row = (guint32 *)(dp + (y - r->y0) * ds), colour[4];
        const guint8 *cov = cp + (y - ir->y0) * cs + (r->x0 - ir->x0), *lrow[4];
        const guint32 *strip[4];

        if (j < 0 || j + 1 >= f->h) {
            continue;
        }
        if (j != last) {
            const float *up = f->d + j * f->w, *down = up + f->w;

            for (nclose = q = 0; q < runs; q++) {
                int c = cell[run[q]];

                if (c >= 0 && c + 1 < f->w && MIN(MIN(up[c], up[c + 1]), MIN(down[c], down[c + 1])) < reach) {
                    closer[nclose++] = q;
                }
            }
            last = j;
        }
        for (k = 0; k < n; k++) {
            lrow[k] = src[k].line ? lp[k] + (y - r->y0) * ls[k] : NULL;
            colour[k] = src[k].strip && src[k].by_row ? src[k].strip[y - r->y0] : src[k].colour;
            strip[k] = src[k].strip && !src[k].by_row ? src[k].strip : NULL;
        }
        for (q = 0; q < nclose; q++) {
            int c = cell[run[closer[q]]];
            float left[4], slope[4];

            for (k = 0; k < n; k++) {
                if (src[k].cell) {
                    const float *a = src[k].cell + j * f->w + c, *b = a + f->w;

                    left[k] = a[0] + (b[0] - a[0]) * fy;
                    slope[k] = a[1] + (b[1] - a[1]) * fy - left[k];
                }
            }
            for (x = run[closer[q]]; x < run[closer[q] + 1]; x++) {
                guint32 in = cov[x], p = row[x];
                float by = clip ? in : 255;

                if (clip ? in == 0 : in == 255) {
                    continue;
                }
                for (k = 0; k < n; k++) {
                    guint32 s = strip[k] ? strip[k][x] : colour[k], a;

                    if (src[k].cell) {
                        a = (guint32)((left[k] + slope[k] * along[x]) * by + 0.5f);
                        if (a) {
                            p = visor_over(p, visor_mul(s, a));
                        }
                    } else if (lrow[k][x]) {
                        /* a line cut to the outline (its coverage), then laid at its alpha under the cut again */
                        a = clip ? visor_mul(src[k].alpha, in) & 0xff : src[k].alpha;
                        p = visor_over(p, visor_mul(visor_mul(s, lrow[k][x]), a));
                    }
                }
                row[x] = p;
            }
        }
    }
    cairo_surface_mark_dirty(dst);
    g_free(run);
    g_free(cell);
    g_free(closer);
    g_free(along);
}

#define VISOR_TILE 64           /* a patch is laid in tiles of this many pixels: only those that hold anything */

/*
 * Lays the patch @p (on @r) over @cr where it holds anything: on its tiles that come within @reach of the outline
 * (by the field, which covers @r) and on @n more boxes of user space (x, y, width, height; whole pixels).
 */
static void visor_lay_patch(cairo_t *cr, cairo_surface_t *p, const VisorRect *r, const VisorMap *m,
                            const VisorField *f, double reach, const double (*more)[4], int n)
{
    int tx, ty, i, j, k, any = n;

    cairo_save(cr);
    cairo_new_path(cr);
    for (ty = r->y0; ty < r->y1; ty += VISOR_TILE) {
        double uy0 = (ty - m->oy) / m->scale, uy1 = (MIN(ty + VISOR_TILE, r->y1) - m->oy) / m->scale;
        int j0 = MAX(0, (int)floor((uy0 - f->y0) / VISOR_CELL - 0.5) - f->j0);
        int j1 = MIN(f->h - 1, (int)floor((uy1 - f->y0) / VISOR_CELL - 0.5) + 1 - f->j0);

        for (tx = r->x0; tx < r->x1; tx += VISOR_TILE) {
            double ux0 = (tx - m->ox) / m->scale, ux1 = (MIN(tx + VISOR_TILE, r->x1) - m->ox) / m->scale;
            int i0 = MAX(0, (int)floor((ux0 - f->x0) / VISOR_CELL - 0.5) - f->i0);
            int i1 = MIN(f->w - 1, (int)floor((ux1 - f->x0) / VISOR_CELL - 0.5) + 1 - f->i0);
            bool hit = false;

            for (j = j0; j <= j1 && !hit; j++) {
                for (i = i0; i <= i1 && !hit; i++) {
                    hit = f->d[j * f->w + i] < reach;
                }
            }
            if (hit) {
                cairo_rectangle(cr, ux0, uy0, ux1 - ux0, uy1 - uy0);
                any++;
            }
        }
    }
    for (k = 0; k < n; k++) {
        cairo_rectangle(cr, more[k][0], more[k][1], more[k][2], more[k][3]);
    }
    if (any) {
        cairo_clip(cr);
        cairo_set_source_surface(cr, p, 0, 0);
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

/* A camera under the picture at @x of the outline's box, as low as the glass's edge lets it sit */
static void visor_camera(cairo_t *cr, const VisorGeometry *g, double x, double r)
{
    double rest = g->gt + g->gh - VISOR_MB * g->u * 0.47;
    double edge = g->gt + visor_lower_edge(g, x) * g->gh - r - 1.25 * g->u;
    double y = MAX(MIN(rest, edge), g->sy + g->sh + r + 0.3 * g->u);

    visor_draw_lens(cr, g->cx + x * g->hw, y, r);
}

/* The light along the glass's top: a pale crescent between the rim and the picture, as in Apple's picture */
static void visor_paint_crescent(cairo_t *cr, const VisorGeometry *g, const VisorLook *l)
{
    const double top[8] = { 0, 0, VISOR_TX, 0, 1, VISOR_SY, 1, VISOR_MY };
    double u = g->u, span = 0.56, x, y, t;
    cairo_pattern_t *p;
    int i, n = 48;

    cairo_save(cr);
    visor_outline(cr, g);
    cairo_clip(cr);
    cairo_new_path(cr);
    /* its upper edge follows the glass's, a rim's width in; its lower edge is the flatter arc of a lens */
    for (i = -n; i <= n; i++) {
        double s = (double)i / n;

        t = fabs(s) * span;
        visor_bez(top, t, &x, &y, NULL, NULL);
        x = g->cx + (s < 0 ? -x : x) * g->hw;
        y = g->gt + y * g->gh + (1.0 + 0.4 * s * s) * u;
        if (i == -n) {
            cairo_move_to(cr, x, y);
        } else {
            cairo_line_to(cr, x, y);
        }
    }
    for (i = n; i >= -n; i--) {
        double s = (double)i / n, thick = 2.7 * u * pow(1 - s * s, 0.8);

        t = fabs(s) * span;
        visor_bez(top, t, &x, &y, NULL, NULL);
        x = g->cx + (s < 0 ? -x : x) * g->hw;
        y = g->gt + y * g->gh + (1.0 + 0.4 * s * s) * u + thick;
        cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
    p = cairo_pattern_create_linear(0, g->gt + u, 0, g->gt + 4.2 * u);
    if (l->walk) {
        cairo_pattern_add_color_stop_rgba(p, 0, 0.62, 0.70, 1.00, 0.62);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.45, 0.50, 1.00, 0.20);
    } else {
        cairo_pattern_add_color_stop_rgba(p, 0, 0.80, 0.87, 0.98, 0.78);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.58, 0.67, 0.84, 0.34);
    }
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    cairo_restore(cr);
}

/* The room's light is painted at one value per VISOR_ROOM units and stretched: it changes far too slowly to show */
#define VISOR_ROOM 8

/*
 * The room behind the headset, on x0..x1, y0..y1 of user space: its dark, and a breath of light behind the
 * headset so that it floats; in walk mode the glass's own light. Not in a box well inside the glass's outline,
 * which the glass covers whole: around 90% of its width, from its upper edge there down to the nose bridge.
 */
static void visor_paint_room(cairo_t *cr, const VisorGeometry *g, const VisorLook *l, double x0, double y0,
                             double x1, double y1)
{
    double in_x0 = ceil(g->cx - 0.9 * g->hw) + 2, in_x1 = floor(g->cx + 0.9 * g->hw) - 2;
    double in_y0 = ceil(g->gt + visor_upper_edge(0.9) * g->gh) + 2;
    double in_y1 = floor(g->gt + MIN(g->ay, visor_lower_edge(g, 0.9)) * g->gh) - 2;
    int i0 = (int)floor(x0 / VISOR_ROOM - 0.5) - 1, j0 = (int)floor(y0 / VISOR_ROOM - 0.5) - 1;
    int i1 = (int)floor(x1 / VISOR_ROOM - 0.5) + 3, j1 = (int)floor(y1 / VISOR_ROOM - 0.5) + 3;
    cairo_surface_t *low = cairo_image_surface_create(CAIRO_FORMAT_RGB24, i1 - i0, j1 - j0);
    cairo_t *lc = cairo_create(low);
    cairo_pattern_t *p;
    cairo_matrix_t place;

    cairo_scale(lc, 1.0 / VISOR_ROOM, 1.0 / VISOR_ROOM);
    cairo_translate(lc, -i0 * VISOR_ROOM, -j0 * VISOR_ROOM);
    cairo_set_source_rgb(lc, 0.047, 0.047, 0.055);
    cairo_paint(lc);
    p = cairo_pattern_create_radial(g->cx, g->gt + g->gh * 0.5, g->gh * 0.3, g->cx, g->gt + g->gh * 0.5, g->hw * 1.25);
    if (l->walk) {
        cairo_pattern_add_color_stop_rgba(p, 0, 0.30, 0.45, 1.0, 0.15);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.30, 0.45, 1.0, 0);
    } else {
        cairo_pattern_add_color_stop_rgba(p, 0, 0.62, 0.70, 0.88, 0.085);
        cairo_pattern_add_color_stop_rgba(p, 1, 0.62, 0.70, 0.88, 0);
    }
    cairo_set_source(lc, p);
    cairo_paint(lc);
    cairo_pattern_destroy(p);
    cairo_destroy(lc);
    p = cairo_pattern_create_for_surface(low);
    cairo_matrix_init_scale(&place, 1.0 / VISOR_ROOM, 1.0 / VISOR_ROOM);
    cairo_matrix_translate(&place, -i0 * VISOR_ROOM, -j0 * VISOR_ROOM);
    cairo_pattern_set_matrix(p, &place);
    cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
    cairo_save(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, floor(x0), floor(y0), ceil(x1) - floor(x0), ceil(y1) - floor(y0));
    if (in_x0 < in_x1 && in_y0 < in_y1) {
        cairo_rectangle(cr, in_x0, in_y0, in_x1 - in_x0, in_y1 - in_y0);
    }
    cairo_clip(cr);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(p);
    cairo_surface_destroy(low);
}

/*
 * The glass's own light and shade, in a patch on @r, clear where there is none: the picture's edges going dark
 * into the glass, the glass's thick curved rim, the light it catches along its top; all of it cut to the
 * outline. It lies over whatever is behind the glass, the picture or the lobes, and is laid on both layers.
 */
static cairo_surface_t *visor_glass(const VisorGeometry *g, const VisorLook *l, const VisorMap *m,
                                    const VisorRect *r, const VisorField *f, cairo_surface_t *inside,
                                    const VisorRect *ir, VisorScratch *sc)
{
    double u = g->u, fade = 2.2 * u, top = g->gt, by = g->gt + g->gh;
    cairo_surface_t *glass = visor_patch(sc, VISOR_GLASS, m, r, CAIRO_FORMAT_ARGB32);
    cairo_t *cr = cairo_create(glass);
    VisorSource lay[3];
    cairo_pattern_t *p;
    cairo_t *lc;
    int i;

    cairo_save(cr);
    visor_outline(cr, g);
    cairo_clip(cr);
    /* the picture ends in the glass, not at a ruled line: its lower edge and its sides go dark */
    p = cairo_pattern_create_linear(0, g->sy + g->sh - fade, 0, g->sy + g->sh);
    cairo_pattern_add_color_stop_rgba(p, 0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(p, 1, 0.004, 0.005, 0.008, 1);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, g->sx, g->sy + g->sh - fade, g->sw, fade);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    for (i = 0; i < 2; i++) {
        double x0 = i ? g->sx + g->sw : g->sx, x1 = i ? x0 - fade : x0 + fade;

        p = cairo_pattern_create_linear(x0, 0, x1, 0);
        cairo_pattern_add_color_stop_rgba(p, 0, 0.004, 0.005, 0.008, 1);
        cairo_pattern_add_color_stop_rgba(p, 1, 0, 0, 0, 0);
        cairo_set_source(cr, p);
        cairo_rectangle(cr, MIN(x0, x1), g->sy, fade, g->sh);
        cairo_fill(cr);
        cairo_pattern_destroy(p);
    }
    p = cairo_pattern_create_linear(0, g->sy, 0, g->sy + fade * 0.7);
    cairo_pattern_add_color_stop_rgba(p, 0, 0.004, 0.005, 0.008, 1);
    cairo_pattern_add_color_stop_rgba(p, 1, 0, 0, 0, 0);
    cairo_set_source(cr, p);
    cairo_rectangle(cr, g->sx, g->sy, g->sw, fade * 0.7);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    VISOR_MARK("glass: fades");
    /* the rim: the glass curves away at its edge, so the edge is darker (rgb 0, 0, 0.008) ... */
    lay[0] = (VisorSource) { .cell = visor_band(f, g, 9.5, 0.46, 1.25, sc, VISOR_DARK),
                             .end = visor_band_end(g, 9.5, 1.25),
                             .colour = 0xff000002 };
    /* ... and mirrors the room: a narrow cool band, brightest low on the lobes and along the top, and the
     * edge's own thin line of light. In walk mode it is the glass's light instead, and wider. */
    p = l->walk ? visor_light(g, 1) : cairo_pattern_create_linear(0, top, 0, by);
    if (!l->walk) {
        cairo_pattern_add_color_stop_rgba(p, 0.00, 0.66, 0.75, 0.92, 1.00);
        cairo_pattern_add_color_stop_rgba(p, 0.30, 0.52, 0.62, 0.82, 0.50);
        cairo_pattern_add_color_stop_rgba(p, 0.62, 0.52, 0.62, 0.82, 0.55);
        cairo_pattern_add_color_stop_rgba(p, 1.00, 0.66, 0.76, 0.94, 1.00);
    }
    lay[1] = (VisorSource) { .cell = visor_band(f, g, l->walk ? 6.0 : 3.2, l->walk ? 0.58 : 0.30, 1.6, sc,
                                                VISOR_LIGHT),
                             .end = visor_band_end(g, l->walk ? 6.0 : 3.2, 1.6),
                             .strip = visor_strip(p, m, r, !l->walk), .by_row = !l->walk };
    /* the line: stroked under the cut to the outline and laid at its alpha under the cut again, a group's way */
    lay[2] = lay[1];
    lay[2].cell = NULL;
    lay[2].line = visor_patch(sc, VISOR_LINE, m, r, CAIRO_FORMAT_A8);
    lay[2].alpha = (guint32)((l->walk ? 0.85 : 0.46) * 65535 + 0.5) >> 8;
    lay[2].end = 0.30 * u + VISOR_CELL + 2;
    lc = cairo_create(lay[2].line);
    visor_outline(lc, g);
    cairo_clip(lc);
    cairo_set_line_join(lc, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(lc, 2 * 0.30 * u);
    visor_outline(lc, g);
    cairo_stroke(lc);
    cairo_destroy(lc);
    cairo_pattern_destroy(p);
    VISOR_MARK("glass: bands and line made");
    visor_lay(glass, r, m, f, lay, 3, inside, ir, true);
    g_free(lay[1].strip);
    cairo_surface_destroy(lay[2].line);
    cairo_restore(cr);
    VISOR_MARK("glass: bands and line laid");
    visor_paint_crescent(cr, g, l);
    cairo_destroy(cr);
    VISOR_MARK("glass: crescent");
    return glass;
}

/*
 * Both layers. @under is the whole area, opaque: the room, the crown, the top button, the frame, the glass. @over
 * (or NULL) is the picture's rectangle in the same user space, clear where the picture shows: outside the glass's
 * outline the headset and the room again, inside it the glass, and a dark line along the outline that closes the
 * seam between the two. Only what lies in their clips is painted (visor_paint_knobs() paints a knob's rows so);
 * @over must be clear there, and as it takes the headset from @under's surface, its clip must lie in @under's.
 * Either may have a device scale (HiDPI): the patches made here are on the under layer's own pixels; @sc keeps
 * their memory.
 */
static void visor_paint(cairo_t *under, cairo_t *over, const VisorGeometry *g, const VisorLook *l, PangoContext *pc,
                        VisorScratch *sc)
{
    double u = g->u, x0, y0, x1, y1, reach, cy;
    VisorMap m;
    VisorRect roi, around, r;
    VisorField f = { 0 };
    cairo_surface_t *inside = NULL;
    cairo_pattern_t *p;
    cairo_t *cr = under;
    bool banded;
    int i;

    visor_map(under, &m);
    cairo_clip_extents(under, &x0, &y0, &x1, &y1);
    visor_rect(&m, x0, y0, x1, y1, NULL, &roi);
    /* the soft bands' neighbourhood of the outline: the distance to it and the outline's own coverage */
    reach = MAX(visor_band_end(g, 9.5, 1.25), MAX(visor_band_end(g, 6.0, 1.6), visor_band_end(g, 6.5, 1.8))) + 1;
    banded = visor_rect(&m, g->cx - g->hw - reach, g->gt - reach, g->cx + g->hw + reach, g->gt + g->gh + reach,
                        &roi, &around);
    if (banded) {
        visor_field(&f, g, reach, &m, &around, sc);
        inside = visor_patch(sc, VISOR_INSIDE, &m, &around, CAIRO_FORMAT_A8);
        cr = cairo_create(inside);
        visor_outline(cr, g);
        cairo_fill(cr);
        cairo_destroy(cr);
        cr = under;
    }
    VISOR_MARK("field");
    visor_paint_room(cr, g, l, x0, y0, x1, y1);
    VISOR_MARK("room");
    reach = visor_band_end(g, 6.5, 1.8) + 1;
    if (l->walk && banded && visor_rect(&m, g->cx - g->hw - reach, g->gt - reach, g->cx + g->hw + reach,
                                        g->gt + g->gh + reach, &around, &r)) {
        /* the light spills past the frame */
        cairo_surface_t *glow = visor_patch(sc, VISOR_GLOW, &m, &r, CAIRO_FORMAT_ARGB32);
        VisorSource band;

        p = visor_light(g, 1);
        band = (VisorSource) { .cell = visor_band(&f, g, 6.5, 0.50, 1.8, sc, VISOR_GLOW_BAND),
                               .end = visor_band_end(g, 6.5, 1.8),
                               .strip = visor_strip(p, &m, &r, false) };
        cairo_pattern_destroy(p);
        visor_lay(glow, &r, &m, &f, &band, 1, inside, &around, false);
        visor_lay_patch(cr, glow, &r, &m, &f, band.end, NULL, 0);
        g_free(band.strip);
        cairo_surface_destroy(glow);
        VISOR_MARK("walk glow");
    }
    visor_draw_crown(cr, g, l);
    visor_draw_top(cr, g, l);
    /* the frame: the outer half of a stroke along the glass's outline, a dark line where it meets the room */
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_set_line_width(cr, 2 * g->frame + 2);
    visor_outline(cr, g);
    cairo_stroke(cr);
    p = visor_aluminium(g, 0);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, 2 * g->frame);
    visor_outline(cr, g);
    cairo_stroke(cr);
    cairo_pattern_destroy(p);
    /* its chamfer towards the glass, a little brighter */
    p = visor_aluminium(g, 0.14);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, 2 * g->frame * 0.42);
    visor_outline(cr, g);
    cairo_stroke(cr);
    cairo_pattern_destroy(p);
    VISOR_MARK("knobs, frame");
    /* the glass: the colour the picture's edges fade to, a little lighter only above the picture */
    p = cairo_pattern_create_linear(0, g->gt, 0, g->sy);
    cairo_pattern_add_color_stop_rgb(p, 0.0, 0.030, 0.036, 0.052);
    cairo_pattern_add_color_stop_rgb(p, 1.0, 0.004, 0.005, 0.008);
    cairo_set_source(cr, p);
    visor_outline(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* the cameras under each eye, the larger pair by the nose */
    for (i = -1; i <= 1; i += 2) {
        visor_camera(cr, g, i * 0.290, 2.25 * u);
        visor_camera(cr, g, i * 0.392, 1.95 * u);
        visor_camera(cr, g, i * 0.585, 1.05 * u);
    }
    VISOR_MARK("glass, cameras");
    /* the legend, from the nose bridge's hollow down, only when that is being painted (its text is the dear part) */
    cy = g->gt + g->gh + g->frame + VISOR_PAD_BOTTOM * u + VISOR_LEGEND / 2.0;
    if (y1 > g->gt + g->ay * g->gh - 2 && y0 < cy + VISOR_LEGEND / 2.0) {
        visor_draw_legend(cr, g, l, pc);
    }
    VISOR_MARK("legend");
    if (over) {
        /* outside the outline the picture's rectangle shows what lies under it: the frame, the room */
        cairo_matrix_t ctm;

        cairo_save(over);
        cairo_set_fill_rule(over, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_rectangle(over, g->sx, g->sy, g->sw, g->sh);
        visor_outline(over, g);
        cairo_clip(over);
        p = cairo_pattern_create_for_surface(cairo_get_target(under));
        cairo_get_matrix(under, &ctm);
        cairo_pattern_set_matrix(p, &ctm);
        cairo_set_source(over, p);
        cairo_paint(over);
        cairo_pattern_destroy(p);
        cairo_restore(over);
        VISOR_MARK("over: frame");
    }
    if (banded && visor_rect(&m, g->cx - g->hw - 1, g->gt - 1, g->cx + g->hw + 1, g->gt + g->gh + 1, &around, &r)) {
        cairo_surface_t *glass = visor_glass(g, l, &m, &r, &f, inside, &around, sc);
        double fade = 2.2 * u, px0 = floor(g->sx), py0 = floor(g->sy), px1 = ceil(g->sx + g->sw);
        double py1 = ceil(g->sy + g->sh);
        /* it is near the outline, but for the picture's edges going dark into the glass */
        const double edges[4][4] = {
            { px0, py0, px1 - px0, ceil(g->sy + fade * 0.7) - py0 },
            { px0, floor(g->sy + g->sh - fade), px1 - px0, py1 - floor(g->sy + g->sh - fade) },
            { px0, py0, ceil(g->sx + fade) - px0, py1 - py0 },
            { floor(g->sx + g->sw - fade), py0, px1 - floor(g->sx + g->sw - fade), py1 - py0 },
        };

        visor_lay_patch(under, glass, &r, &m, &f, visor_band_end(g, 9.5, 1.25), edges, 4);
        if (over) {
            visor_lay_patch(over, glass, &r, &m, &f, visor_band_end(g, 9.5, 1.25), edges, 4);
        }
        cairo_surface_destroy(glass);
        VISOR_MARK("glass laid");
    }
    if (over) {
        cairo_set_source_rgba(over, 0, 0, 0, 0.9);
        cairo_set_line_width(over, 1.6);
        visor_outline(over, g);
        cairo_stroke(over);
    }
    if (inside) {
        cairo_surface_destroy(inside);
    }
}

/* How far down (in user space, a whole pixel) a knob's look can show: hovered, pressed, the crown turned */
static int visor_knob_rows(const VisorGeometry *g, int knob)
{
    const VisorKnob *k = knob == 1 ? &g->crown : &g->top;
    /* in the knob's own frame: the collar or the key, as far down as it goes pressed */
    double hx = k->w / 2 + 0.35 * g->u, low = (knob == 1 ? 0.45 : 1.0) * g->u;
    double c = cos(k->angle), s = sin(k->angle), y = -G_MAXDOUBLE;
    int i;

    for (i = 0; i < 2; i++) {
        y = MAX(y, k->y + (i ? hx : -hx) * s + low * c);
    }
    return (int)ceil(y) + 2;    /* and the edges' antialiasing */
}

/*
 * The knobs' hover, press or turn going from @was to @l, on layers painted for @was (the under layer @width wide,
 * the over layer the picture's rectangle with its origin at the picture's corner): the whole rows above the knobs
 * that changed are painted again, the under layer's, and the over layer's where they reach into the picture's
 * rectangle (the frame's edge over a knob's foot can lie in its top rows). Whole rows, not a box around a knob:
 * cairo's antialiasing of an edge on a row depends on the other edges on that row, so a box would come out a level
 * off here and there.
 */
static void visor_paint_knobs(cairo_surface_t *under, cairo_surface_t *over, double width, const VisorGeometry *g,
                              const VisorLook *was, const VisorLook *l, PangoContext *pc, VisorScratch *sc)
{
    cairo_t *cu, *co = NULL;
    int rows = 0, k;

    for (k = 1; k <= 2; k++) {
        if ((was->hover == k) != (l->hover == k) || (was->pressed == k) != (l->pressed == k) ||
            (k == 1 && was->crown_phase != l->crown_phase)) {
            rows = MAX(rows, visor_knob_rows(g, k));
        }
    }
    if (!rows) {
        return;
    }
    cu = cairo_create(under);
    cairo_rectangle(cu, 0, 0, width, rows);
    cairo_clip(cu);
    if (rows > g->sy) {
        co = cairo_create(over);
        cairo_translate(co, -g->sx, -g->sy);
        cairo_rectangle(co, 0, 0, width, rows);
        cairo_clip(co);
        cairo_set_operator(co, CAIRO_OPERATOR_CLEAR);
        cairo_paint(co);
        cairo_set_operator(co, CAIRO_OPERATOR_OVER);
    }
    visor_paint(cu, co, g, l, pc, sc);
    if (co) {
        cairo_destroy(co);
    }
    cairo_destroy(cu);
}

/* 1: the crown, 2: the top button, 0: neither (some slack around each, and the frame under it) */
static int visor_hit(const VisorGeometry *g, double x, double y)
{
    const VisorKnob *k[2] = { &g->crown, &g->top };
    int i;

    for (i = 0; i < 2; i++) {
        double dx = x - k[i]->x, dy = y - k[i]->y, c = cos(-k[i]->angle), s = sin(-k[i]->angle);
        double lx = dx * c - dy * s, ly = dx * s + dy * c;

        if (fabs(lx) < k[i]->w / 2 + 6 && ly > -k[i]->h - 7 && ly < g->frame + 3) {
            return i + 1;
        }
    }
    return 0;
}
