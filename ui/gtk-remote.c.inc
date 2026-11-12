/*
 * A TV guest's window (ASoftware tvOS), in cairo; ui/gtk.c includes it after ui/gtk-visor.c.inc, whose rounded
 * rectangle and legend keys it shares.
 *
 * The guest's picture is a TV's screen: a thin black bezel around it, on a dark wall it lights a little. Beside it
 * stands the Siri Remote (the aluminium one), seen from the front: a tall aluminium body with rounded ends, the
 * power button in its top right corner, the round black clickpad (its ring: up, down, left, right; its centre:
 * select), under it two columns of black buttons: Back and the TV button, Play/Pause and the volume rocker's +,
 * Mute and its -. The Siri button is on the body's right side: a sliver of it shows past the edge. Under the
 * remote, the keys that do the same on the keyboard.
 *
 * Everything is in units of a hundredth of the picture's height, as the headset's, but for the legend's column
 * (pixels: its text does not scale). Nothing here knows GTK: remote_layout() places the picture in an area,
 * remote_geometry() derives the TV and the remote from the picture, remote_paint() paints all of it that lies in
 * its clip (one opaque layer: nothing lies over the picture), remote_part_rows() says in which rows a part's look
 * shows (on the remote and in the legend), remote_paint_parts() paints only those again when looks change, and
 * remote_hit() says what is under the pointer.
 */

#define TV_PAD 3.0              /* the wall around the TV and right of the remote, in units */
#define TV_BEZEL 1.1            /* the TV's black bezel around the picture */
#define REMOTE_GAP 6.0          /* between the TV and the remote's column */
#define REMOTE_H 70.0           /* the remote's height; its top is the TV's */
#define REMOTE_ASPECT 3.842     /* the Siri Remote: 136 x 35.4 mm */
#define REMOTE_COLUMN 200       /* pixels: the remote's column is at least the legend's width */
#define REMOTE_ROW 21           /* pixels: a row of the legend */

/* The remote in units of its width W, from Apple's front view of it */
#define RM_END 0.30             /* the radius of its ends' corners */
#define RM_PAD_Y 0.735          /* the clickpad: its centre below the top, its radius */
#define RM_PAD_R 0.415
#define RM_PAD_C 0.58           /* the clickpad's centre (select), of the clickpad's radius */
#define RM_BTN_R 0.152          /* a round button's radius */
#define RM_COL 0.235            /* the two columns' centres, either side of the middle */
#define RM_ROW0 1.47            /* the first row's centre below the top, and the rows' pitch */
#define RM_PITCH 0.48
#define RM_PWR_X 0.295          /* the power button: right of the middle, below the top, its radius */
#define RM_PWR_Y 0.165
#define RM_PWR_R 0.074
#define RM_SIRI_Y0 0.86         /* the Siri button along the right side */
#define RM_SIRI_Y1 1.32

enum {
    REMOTE_NONE, REMOTE_UP, REMOTE_DOWN, REMOTE_LEFT, REMOTE_RIGHT, REMOTE_SELECT, REMOTE_BACK, REMOTE_TV,
    REMOTE_PLAY, REMOTE_MUTE, REMOTE_VOLUP, REMOTE_VOLDOWN, REMOTE_POWER, REMOTE_SIRI, REMOTE_PARTS
};

#define REMOTE_BIT(part) (1u << (part))
#define REMOTE_PAD_PARTS (REMOTE_BIT(REMOTE_UP) | REMOTE_BIT(REMOTE_DOWN) | REMOTE_BIT(REMOTE_LEFT) | \
                          REMOTE_BIT(REMOTE_RIGHT) | REMOTE_BIT(REMOTE_SELECT))

typedef struct RemoteGeometry {
    double sx, sy, sw, sh;      /* the picture */
    double u;                   /* the unit */
    double bz;                  /* the TV's bezel */
    double x, y, w, h;          /* the remote's body */
    double px, py, pr, pc;      /* the clickpad: its centre, its radius, its centre's radius */
    double br;                  /* a round button's radius */
    double c0, c1;              /* the left and the right column's centres */
    double row[3];              /* the rows' centres */
    double wx, wy, wr;          /* the power button */
    double siri0, siri1;        /* the Siri button's ends */
    double lx, ly;              /* the legend: the middle its widest row is centred on, its top */
    int rows;                   /* its rows that fit the area */
} RemoteGeometry;

typedef struct RemoteLook {
    int hover;                  /* the part under the pointer, or REMOTE_NONE */
    unsigned int lit;           /* the parts held down (REMOTE_BIT), by the mouse or by their keys */
    bool full_screen;           /* the window is full screen: the legend says how to leave it */
} RemoteLook;

/* The legend's rows: visor_legend_row()'s items ("k:" a key, "t:" what it does, "s" a gap) */
static const char *const remote_legend[][8] = {
    { "k:\xe2\x86\x91", "k:\xe2\x86\x93", "k:\xe2\x86\x90", "k:\xe2\x86\x92", "t:move", NULL },
    { "k:Return", "t:select", NULL },
    { "k:Esc", "t:back", NULL },
    { "k:F1", "t:TV, held: Control Center", NULL },
    { "k:F5", "t:play/pause", "s", "k:F6", "t:mute", NULL },
    { "k:F4", "k:F3", "t:volume", "s", "k:F2", "t:power", NULL },
    { "k:F7", "t:Siri, held", NULL },
    /* full screen only */
    { "k:F11", "t:leave full screen", NULL },
    { "k:F9", "t:hide the remote", NULL },
};
#define REMOTE_LEGEND_ROWS 7    /* the rows outside full screen */

/* The parts a key of the legend stands for */
static unsigned int remote_key_parts(const char *key)
{
    static const struct {
        const char *key;
        int part;
    } keys[] = {
        { "\xe2\x86\x91", REMOTE_UP }, { "\xe2\x86\x93", REMOTE_DOWN }, { "\xe2\x86\x90", REMOTE_LEFT },
        { "\xe2\x86\x92", REMOTE_RIGHT }, { "Return", REMOTE_SELECT }, { "Esc", REMOTE_BACK },
        { "F1", REMOTE_TV }, { "F2", REMOTE_POWER }, { "F3", REMOTE_VOLDOWN }, { "F4", REMOTE_VOLUP },
        { "F5", REMOTE_PLAY }, { "F6", REMOTE_MUTE }, { "F7", REMOTE_SIRI },
    };
    int i;

    for (i = 0; i < G_N_ELEMENTS(keys); i++) {
        if (!strcmp(keys[i].key, key)) {
            return REMOTE_BIT(keys[i].part);
        }
    }
    return 0;
}

/* The remote's column: as wide as the remote or the legend */
static double remote_column(double u)
{
    return MAX(REMOTE_H / REMOTE_ASPECT * u, REMOTE_COLUMN);
}

/*
 * The room the TV and the remote take beside a picture of @sh pixels' height: left+right and top+bottom; the
 * remote's column comes on top of @wide, the remote's own width (@remote) or REMOTE_COLUMN, whichever is wider.
 */
static void remote_room(double sh, double *wide, double *high, double *remote)
{
    double u = sh / 100;

    *wide = (2 * TV_PAD + 2 * TV_BEZEL + REMOTE_GAP) * u;
    *high = (2 * TV_PAD + 2 * TV_BEZEL) * u;
    *remote = REMOTE_H / REMOTE_ASPECT * u;
}

/* The picture (aspect @aspect, width over height) as large as a w x h area lets the TV and the remote be */
static void remote_layout(double w, double h, double aspect, int *sx, int *sy, int *sw, int *sh)
{
    double kx = (2 * TV_PAD + 2 * TV_BEZEL + REMOTE_GAP) / 100, ky = (2 * TV_PAD + 2 * TV_BEZEL) / 100;
    double kr = REMOTE_H / REMOTE_ASPECT / 100, ph, total;

    /* the column is the legend's or the remote's, whichever is wider */
    ph = MIN(h / (1 + ky), MIN((w - REMOTE_COLUMN) / (aspect + kx), w / (aspect + kx + kr)));
    ph = MAX(ph, 40);
    total = ph * (aspect + kx) + remote_column(ph / 100);
    *sh = (int)floor(ph);
    *sw = (int)floor(ph * aspect);
    *sx = (int)floor(MAX((w - total) / 2, 0) + (TV_PAD + TV_BEZEL) * ph / 100);
    *sy = (int)floor(MAX((h - *sh) / 2, 0));
}

/* The TV and the remote from the picture; @area_h says how many of the legend's rows fit under the remote */
static void remote_geometry(double sx, double sy, double sw, double sh, double area_h, bool full_screen,
                            RemoteGeometry *g)
{
    double u = sh / 100, col, x0, w;
    int fit;

    g->sx = sx;
    g->sy = sy;
    g->sw = sw;
    g->sh = sh;
    g->u = u;
    g->bz = MAX(2, TV_BEZEL * u);
    col = remote_column(u);
    x0 = sx + sw + g->bz + REMOTE_GAP * u;
    g->h = REMOTE_H * u;
    g->w = w = g->h / REMOTE_ASPECT;
    g->x = floor(x0 + (col - w) / 2);
    g->y = sy - g->bz;
    g->px = g->x + w / 2;
    g->py = g->y + RM_PAD_Y * w;
    g->pr = RM_PAD_R * w;
    g->pc = RM_PAD_C * g->pr;
    g->br = RM_BTN_R * w;
    g->c0 = g->px - RM_COL * w;
    g->c1 = g->px + RM_COL * w;
    g->row[0] = g->y + RM_ROW0 * w;
    g->row[1] = g->row[0] + RM_PITCH * w;
    g->row[2] = g->row[1] + RM_PITCH * w;
    g->wx = g->px + RM_PWR_X * w;
    g->wy = g->y + RM_PWR_Y * w;
    g->wr = MAX(4, RM_PWR_R * w);
    g->siri0 = g->y + RM_SIRI_Y0 * w;
    g->siri1 = g->y + RM_SIRI_Y1 * w;
    /* the legend under the remote, its rows as many as the area holds */
    g->ly = floor(g->y + g->h + MAX(10, 3.0 * u));
    g->lx = floor(x0 + col / 2);    /* (its rows' left edge comes from their widths: remote_legend_left()) */
    fit = (int)floor((area_h - 4 - g->ly) / REMOTE_ROW);
    g->rows = MAX(0, MIN(fit, REMOTE_LEGEND_ROWS + (full_screen ? 2 : 0)));
}

/* The aluminium's light across the body: its sides curve away, the light comes from the upper left */
static cairo_pattern_t *remote_aluminium(const RemoteGeometry *g, double lit)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(g->x, 0, g->x + g->w, 0);
    static const double stops[][2] = {
        { 0.000, 0.42 }, { 0.022, 0.62 }, { 0.060, 0.86 }, { 0.115, 0.88 }, { 0.30, 0.835 },
        { 0.62, 0.815 }, { 0.880, 0.790 }, { 0.940, 0.700 }, { 0.978, 0.540 }, { 1.000, 0.34 },
    };
    int i;

    for (i = 0; i < G_N_ELEMENTS(stops); i++) {
        double v = MIN(1, stops[i][1] + lit);

        cairo_pattern_add_color_stop_rgb(p, stops[i][0], v, v, v + 0.012);
    }
    return p;
}

static void remote_body_path(cairo_t *cr, const RemoteGeometry *g)
{
    visor_round_rect(cr, g->x, g->y, g->w, g->h, RM_END * g->w);
}

/*
 * The hole a black key sits in, over the path laid (from @top to @bottom): its chamfer, cut bright, lit from above
 * (the lower half of the cut faces the light)
 */
static void remote_chamfer(cairo_t *cr, double top, double bottom)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(0, top, 0, bottom);

    cairo_pattern_add_color_stop_rgb(p, 0.00, 0.50, 0.50, 0.52);
    cairo_pattern_add_color_stop_rgb(p, 0.45, 0.70, 0.70, 0.72);
    cairo_pattern_add_color_stop_rgb(p, 1.00, 0.97, 0.97, 0.98);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

/* How much lighter a key's cap is hovered or held */
static double remote_cap_add(bool hover, bool lit)
{
    return lit ? 0.30 : hover ? 0.10 : 0;
}

/* A black key's cap over the path laid, from @top to @bottom: a soft sheen at the top; hovered or held lighter */
static void remote_cap(cairo_t *cr, double top, double bottom, bool hover, bool lit)
{
    double add = remote_cap_add(hover, lit);
    cairo_pattern_t *p = cairo_pattern_create_linear(0, top, 0, bottom);

    cairo_pattern_add_color_stop_rgb(p, 0.00, 0.235 + add, 0.235 + add, 0.25 + add);
    cairo_pattern_add_color_stop_rgb(p, 0.35, 0.105 + add, 0.105 + add, 0.115 + add);
    cairo_pattern_add_color_stop_rgb(p, 1.00, 0.050 + add, 0.050 + add, 0.058 + add);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
}

static void remote_glyph_colour(cairo_t *cr, bool lit)
{
    cairo_set_source_rgba(cr, 1, 1, 1, lit ? 1.0 : 0.90);
}

/* The glyphs, about a key's centre @x, @y, in units of its radius @r, on a cap @add lighter (remote_cap_add) */
static void remote_glyph(cairo_t *cr, int part, double x, double y, double r, bool lit, double add)
{
    double lw = MAX(1.1, 0.115 * r);

    cairo_save(cr);
    cairo_translate(cr, x, y);
    remote_glyph_colour(cr, lit);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, lw);
    switch (part) {
    case REMOTE_BACK:           /* a chevron */
        cairo_move_to(cr, 0.13 * r, -0.36 * r);
        cairo_line_to(cr, -0.20 * r, 0);
        cairo_line_to(cr, 0.13 * r, 0.36 * r);
        cairo_stroke(cr);
        break;
    case REMOTE_TV:             /* a TV: its screen and its foot */
        visor_round_rect(cr, -0.44 * r, -0.33 * r, 0.88 * r, 0.56 * r, 0.09 * r);
        cairo_set_line_width(cr, 0.9 * lw);
        cairo_stroke(cr);
        cairo_move_to(cr, -0.20 * r, 0.40 * r);
        cairo_line_to(cr, 0.20 * r, 0.40 * r);
        cairo_stroke(cr);
        break;
    case REMOTE_PLAY:           /* play and pause */
        cairo_move_to(cr, -0.47 * r, -0.27 * r);
        cairo_line_to(cr, -0.47 * r, 0.27 * r);
        cairo_line_to(cr, -0.08 * r, 0);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_rectangle(cr, 0.08 * r, -0.26 * r, 0.12 * r, 0.52 * r);
        cairo_rectangle(cr, 0.30 * r, -0.26 * r, 0.12 * r, 0.52 * r);
        cairo_fill(cr);
        break;
    case REMOTE_MUTE:           /* a speaker and its sound, struck through */
        cairo_move_to(cr, -0.40 * r, -0.14 * r);
        cairo_line_to(cr, -0.22 * r, -0.14 * r);
        cairo_line_to(cr, 0.04 * r, -0.38 * r);
        cairo_line_to(cr, 0.04 * r, 0.38 * r);
        cairo_line_to(cr, -0.22 * r, 0.14 * r);
        cairo_line_to(cr, -0.40 * r, 0.14 * r);
        cairo_close_path(cr);
        cairo_fill(cr);
        if (r >= 16) {
            cairo_set_line_width(cr, 0.8 * lw);
            cairo_arc(cr, 0.08 * r, 0, 0.30 * r, -0.85, 0.85);
            cairo_stroke(cr);
        }
        /* the stroke through, with the cap's black either side of it */
        cairo_move_to(cr, -0.40 * r, -0.42 * r);
        cairo_line_to(cr, 0.42 * r, 0.42 * r);
        cairo_set_source_rgb(cr, 0.092 + add, 0.092 + add, 0.10 + add);
        cairo_set_line_width(cr, 2.0 * lw);
        cairo_stroke_preserve(cr);
        remote_glyph_colour(cr, lit);
        cairo_set_line_width(cr, 0.85 * lw);
        cairo_stroke(cr);
        break;
    case REMOTE_VOLUP:
        cairo_move_to(cr, -0.26 * r, 0);
        cairo_line_to(cr, 0.26 * r, 0);
        cairo_move_to(cr, 0, -0.26 * r);
        cairo_line_to(cr, 0, 0.26 * r);
        cairo_stroke(cr);
        break;
    case REMOTE_VOLDOWN:
        cairo_move_to(cr, -0.26 * r, 0);
        cairo_line_to(cr, 0.26 * r, 0);
        cairo_stroke(cr);
        break;
    case REMOTE_POWER:          /* a circle open at the top, a stroke down into it */
        cairo_set_line_width(cr, MAX(1, 0.15 * r));
        cairo_arc(cr, 0, 0.04 * r, 0.42 * r, -G_PI / 2 + 0.75, 3 * G_PI / 2 - 0.75);
        cairo_stroke(cr);
        cairo_move_to(cr, 0, -0.48 * r);
        cairo_line_to(cr, 0, -0.06 * r);
        cairo_stroke(cr);
        break;
    }
    cairo_restore(cr);
}

/* A round key in its hole */
static void remote_round_key(cairo_t *cr, const RemoteGeometry *g, const RemoteLook *l, int part, double x,
                             double y, double r)
{
    double cut = MAX(1.0, 0.07 * r);
    bool lit = l->lit & REMOTE_BIT(part), hover = l->hover == part;

    cairo_arc(cr, x, y, r + cut, 0, 2 * G_PI);
    remote_chamfer(cr, y - r - cut, y + r + cut);
    cairo_arc(cr, x, y, r, 0, 2 * G_PI);
    remote_cap(cr, y - r, y + r, hover, lit);
    remote_glyph(cr, part, x, y, r, lit, remote_cap_add(hover, lit));
}

/* The volume rocker: one long key, its upper half +, its lower half - */
static void remote_rocker(cairo_t *cr, const RemoteGeometry *g, const RemoteLook *l)
{
    double r = g->br, cut = MAX(1.0, 0.07 * r), top = g->row[1] - r, bottom = g->row[2] + r, mid = (top + bottom) / 2;
    int half;

    visor_round_rect(cr, g->c1 - r - cut, top - cut, 2 * (r + cut), bottom - top + 2 * cut, r + cut);
    remote_chamfer(cr, top - cut, bottom + cut);
    for (half = 0; half < 2; half++) {
        int part = half ? REMOTE_VOLDOWN : REMOTE_VOLUP;
        bool lit = l->lit & REMOTE_BIT(part), hover = l->hover == part;

        cairo_save(cr);
        cairo_rectangle(cr, g->c1 - r - 1, half ? mid : top - 1, 2 * r + 2, half ? bottom - mid + 1 : mid - top + 1);
        cairo_clip(cr);
        visor_round_rect(cr, g->c1 - r, top, 2 * r, bottom - top, r);
        remote_cap(cr, top, bottom, hover, lit);
        cairo_restore(cr);
        remote_glyph(cr, part, g->c1, half ? g->row[2] : g->row[1], r, lit, remote_cap_add(hover, lit));
    }
    /* where the two halves meet, a faint seam */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, g->c1 - r * 0.55, floor(mid) + 0.5);
    cairo_line_to(cr, g->c1 + r * 0.55, floor(mid) + 0.5);
    cairo_stroke(cr);
}

/* A quarter of the clickpad's ring, centred on @angle */
static void remote_ring_quarter(cairo_t *cr, const RemoteGeometry *g, double angle)
{
    double a = G_PI / 4 - 0.035, r0 = g->pc + 1.5, r1 = g->pr - 1.5;

    cairo_new_sub_path(cr);
    cairo_arc(cr, g->px, g->py, r1, angle - a, angle + a);
    cairo_arc_negative(cr, g->px, g->py, r0, angle + a, angle - a);
    cairo_close_path(cr);
}

/* The clickpad: black glass in a bright cut, its centre a little darker inside a faint seam */
static void remote_clickpad(cairo_t *cr, const RemoteGeometry *g, const RemoteLook *l)
{
    static const struct {
        int part;
        double angle;
    } ring[] = {
        { REMOTE_RIGHT, 0 }, { REMOTE_DOWN, G_PI / 2 }, { REMOTE_LEFT, G_PI }, { REMOTE_UP, -G_PI / 2 },
    };
    double cut = MAX(1.2, 0.035 * g->w);
    cairo_pattern_t *p;
    int i;

    cairo_arc(cr, g->px, g->py, g->pr + cut, 0, 2 * G_PI);
    remote_chamfer(cr, g->py - g->pr - cut, g->py + g->pr + cut);
    p = cairo_pattern_create_radial(g->px - 0.25 * g->pr, g->py - 0.45 * g->pr, 0.1 * g->pr,
                                    g->px, g->py, g->pr * 1.05);
    cairo_pattern_add_color_stop_rgb(p, 0.0, 0.165, 0.165, 0.18);
    cairo_pattern_add_color_stop_rgb(p, 0.6, 0.080, 0.080, 0.088);
    cairo_pattern_add_color_stop_rgb(p, 1.0, 0.045, 0.045, 0.052);
    cairo_set_source(cr, p);
    cairo_arc(cr, g->px, g->py, g->pr, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* the ring under the pointer, or held */
    for (i = 0; i < G_N_ELEMENTS(ring); i++) {
        bool lit = l->lit & REMOTE_BIT(ring[i].part);

        if (lit || l->hover == ring[i].part) {
            remote_ring_quarter(cr, g, ring[i].angle);
            cairo_set_source_rgba(cr, 1, 1, 1, lit ? 0.30 : 0.10);
            cairo_fill(cr);
        }
    }
    /* the centre */
    cairo_arc(cr, g->px, g->py, g->pc, 0, 2 * G_PI);
    if (l->lit & REMOTE_BIT(REMOTE_SELECT)) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.30);
    } else if (l->hover == REMOTE_SELECT) {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    } else {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
    }
    cairo_fill(cr);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.13);
    cairo_arc(cr, g->px, g->py, g->pc + 0.5, 0, 2 * G_PI);
    cairo_stroke(cr);
    /* the glass's edge catches the light at the top */
    cairo_set_line_width(cr, MAX(1, 0.012 * g->w));
    p = cairo_pattern_create_linear(0, g->py - g->pr, 0, g->py);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.30);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0);
    cairo_set_source(cr, p);
    cairo_arc(cr, g->px, g->py, g->pr - 0.6, G_PI * 1.08, G_PI * 1.92);
    cairo_stroke(cr);
    cairo_pattern_destroy(p);
}

/* The Siri button on the right side: a sliver of it past the body's edge */
static void remote_siri(cairo_t *cr, const RemoteGeometry *g, const RemoteLook *l)
{
    double out = MAX(1.6, 0.045 * g->w), lit = l->lit & REMOTE_BIT(REMOTE_SIRI) ? 0.30
                                                 : l->hover == REMOTE_SIRI ? 0.08 : 0;

    visor_round_rect(cr, g->x + g->w - 3 * out, g->siri0, 4 * out, g->siri1 - g->siri0, 1.6 * out);
    cairo_set_source_rgb(cr, 0.10 + lit, 0.10 + lit, 0.11 + lit);
    cairo_fill(cr);
}

/* The body: the aluminium, its ends curving away too, its edge */
static void remote_body(cairo_t *cr, const RemoteGeometry *g)
{
    double end = 0.10 * g->w;
    cairo_pattern_t *p;
    int i;

    /* its shadow on the wall, short and soft */
    for (i = 3; i >= 1; i--) {
        visor_round_rect(cr, g->x - i, g->y - i * 0.5 + 0.012 * g->w, g->w + 2 * i, g->h + i * 1.5,
                         RM_END * g->w + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.16);
        cairo_fill(cr);
    }
    p = remote_aluminium(g, 0);
    remote_body_path(cr, g);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_save(cr);
    cairo_clip(cr);
    /* down its length the light falls off a little; its ends turn away from it */
    p = cairo_pattern_create_linear(0, g->y, 0, g->y + g->h);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0, 0, 0, 0.30);
    cairo_pattern_add_color_stop_rgba(p, end / g->h * 0.8, 1, 1, 1, 0.07);
    cairo_pattern_add_color_stop_rgba(p, end / g->h * 2.0, 1, 1, 1, 0.04);
    cairo_pattern_add_color_stop_rgba(p, 1 - end / g->h * 2.5, 0, 0, 0, 0.05);
    cairo_pattern_add_color_stop_rgba(p, 1 - end / g->h * 0.8, 0, 0, 0, 0.10);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0, 0, 0, 0.42);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_restore(cr);
    /* its outline against the wall */
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
    visor_round_rect(cr, g->x + 0.5, g->y + 0.5, g->w - 1, g->h - 1, RM_END * g->w - 0.5);
    cairo_stroke(cr);
}

/*
 * The wall, on x0..x1, y0..y1 of user space: dark, a little of the screen's light on it around the TV. It changes
 * far too slowly to show, so it is painted at one value per REMOTE_WALL units and stretched (as the headset's room).
 */
#define REMOTE_WALL 8

static void remote_paint_wall(cairo_t *cr, const RemoteGeometry *g, double x0, double y0, double x1, double y1)
{
    double cx = g->sx + g->sw / 2, cy = g->sy + g->sh / 2;
    int i0 = (int)floor(x0 / REMOTE_WALL - 0.5) - 1, j0 = (int)floor(y0 / REMOTE_WALL - 0.5) - 1;
    int i1 = (int)floor(x1 / REMOTE_WALL - 0.5) + 3, j1 = (int)floor(y1 / REMOTE_WALL - 0.5) + 3;
    cairo_surface_t *low = cairo_image_surface_create(CAIRO_FORMAT_RGB24, i1 - i0, j1 - j0);
    cairo_t *lc = cairo_create(low);
    cairo_pattern_t *p;
    cairo_matrix_t place;

    cairo_scale(lc, 1.0 / REMOTE_WALL, 1.0 / REMOTE_WALL);
    cairo_translate(lc, -i0 * REMOTE_WALL, -j0 * REMOTE_WALL);
    cairo_set_source_rgb(lc, 0.047, 0.047, 0.055);
    cairo_paint(lc);
    p = cairo_pattern_create_radial(cx, cy, g->sh * 0.30, cx, cy, g->sw * 0.78);
    cairo_pattern_add_color_stop_rgba(p, 0, 0.55, 0.62, 0.80, 0.085);
    cairo_pattern_add_color_stop_rgba(p, 1, 0.55, 0.62, 0.80, 0);
    cairo_set_source(lc, p);
    cairo_paint(lc);
    cairo_pattern_destroy(p);
    cairo_destroy(lc);
    p = cairo_pattern_create_for_surface(low);
    cairo_matrix_init_scale(&place, 1.0 / REMOTE_WALL, 1.0 / REMOTE_WALL);
    cairo_matrix_translate(&place, -i0 * REMOTE_WALL, -j0 * REMOTE_WALL);
    cairo_pattern_set_matrix(p, &place);
    cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
    cairo_surface_destroy(low);
}

/* The TV on the wall: the bezel, black under the picture */
static void remote_paint_tv(cairo_t *cr, const RemoteGeometry *g)
{
    double b = g->bz, x = g->sx - b, y = g->sy - b, w = g->sw + 2 * b, h = g->sh + 2 * b;
    cairo_pattern_t *p;

    /* the bezel, and its rim catching the room's light, more at the top */
    visor_round_rect(cr, x, y, w, h, 0.8 * g->u);
    cairo_set_source_rgb(cr, 0.012, 0.012, 0.016);
    cairo_fill(cr);
    p = cairo_pattern_create_linear(0, y, 0, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0, 1, 1, 1, 0.30);
    cairo_pattern_add_color_stop_rgba(p, 0.5, 1, 1, 1, 0.12);
    cairo_pattern_add_color_stop_rgba(p, 1, 1, 1, 1, 0.07);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, 1);
    visor_round_rect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 0.8 * g->u - 0.5);
    cairo_stroke(cr);
    cairo_pattern_destroy(p);
    cairo_rectangle(cr, g->sx, g->sy, g->sw, g->sh);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_fill(cr);
}

/* The legend's fonts: the keys' and their words' */
static void remote_legend_fonts(PangoContext *pc, PangoLayout **keys, PangoLayout **text)
{
    PangoFontDescription *fd = pango_font_description_copy(pango_context_get_font_description(pc));

    *keys = pango_layout_new(pc);
    *text = pango_layout_new(pc);
    pango_font_description_set_absolute_size(fd, 10.5 * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_SEMIBOLD);
    pango_layout_set_font_description(*keys, fd);
    pango_font_description_set_absolute_size(fd, 12 * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(*text, fd);
    pango_font_description_free(fd);
}

/*
 * An arrow key's cap: the arrow drawn (the fonts' arrows are small and not in every font), centred on @x, @cy,
 * pointing as @parts says; returns the cap's width
 */
static double remote_arrow_key(cairo_t *cr, PangoLayout *keys, double x, double cy, unsigned int parts, bool lit)
{
    double w = visor_keycap(cr, keys, " ", x, cy, lit), angle;

    if (!cr) {
        return w;
    }
    angle = parts & REMOTE_BIT(REMOTE_UP) ? -G_PI / 2 : parts & REMOTE_BIT(REMOTE_DOWN) ? G_PI / 2
          : parts & REMOTE_BIT(REMOTE_LEFT) ? G_PI : 0;
    cairo_save(cr);
    cairo_translate(cr, x + w / 2, cy - 0.5);
    cairo_rotate(cr, angle);
    cairo_set_source_rgba(cr, 1, 1, 1, lit ? 0.95 : 0.74);
    cairo_set_line_width(cr, 1.3);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_move_to(cr, -4, 0);
    cairo_line_to(cr, 4, 0);
    cairo_move_to(cr, 0.8, -3.2);
    cairo_line_to(cr, 4, 0);
    cairo_line_to(cr, 0.8, 3.2);
    cairo_stroke(cr);
    cairo_restore(cr);
    return w;
}

/* A row of the legend, as visor_legend_row() lays it, its keys lit while their parts are; returns its width */
static double remote_legend_row(cairo_t *cr, PangoLayout *keys, PangoLayout *text, const char *const *items,
                                double x, double cy, unsigned int lit)
{
    double x0 = x;
    int tw, th, i;

    for (i = 0; items[i]; i++) {
        const char *it = items[i];
        unsigned int parts = it[0] == 'k' ? remote_key_parts(it + 2) : 0;

        if (it[0] == 'k' && (parts & REMOTE_PAD_PARTS & ~REMOTE_BIT(REMOTE_SELECT))) {
            x += remote_arrow_key(cr, keys, x, cy, parts, lit & parts) + 3;
        } else if (it[0] == 'k') {
            x += visor_keycap(cr, keys, it + 2, x, cy, lit & parts) + 3;
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
            x += 14;
        }
    }
    return x - x0;
}

/* The legend's rows share one left edge, the widest row centred under the remote: that edge */
static double remote_legend_left(const RemoteGeometry *g, PangoLayout *keys, PangoLayout *text)
{
    double widest = 0;
    int i;

    for (i = 0; i < REMOTE_LEGEND_ROWS; i++) {
        widest = MAX(widest, remote_legend_row(NULL, keys, text, remote_legend[i], 0, 0, 0));
    }
    return floor(g->lx - widest / 2);
}

static void remote_draw_legend(cairo_t *cr, const RemoteGeometry *g, const RemoteLook *l, PangoContext *pc,
                               double y0, double y1)
{
    PangoLayout *keys, *text;
    double x;
    int i;

    remote_legend_fonts(pc, &keys, &text);
    x = remote_legend_left(g, keys, text);
    for (i = 0; i < g->rows; i++) {
        double cy = g->ly + (i + 0.5) * REMOTE_ROW;

        if (cy + REMOTE_ROW / 2.0 > y0 && cy - REMOTE_ROW / 2.0 < y1) {
            remote_legend_row(cr, keys, text, remote_legend[i], x, floor(cy), l->lit);
        }
    }
    g_object_unref(keys);
    g_object_unref(text);
}

/* Where a part is drawn, with its cut and the edges' antialiasing: x0, y0, x1, y1 */
static void remote_part_extent(const RemoteGeometry *g, int part, double b[4])
{
    double x, y, rx, ry, cut = MAX(1.0, 0.07 * g->br) + 2;

    switch (part) {
    case REMOTE_UP: case REMOTE_DOWN: case REMOTE_LEFT: case REMOTE_RIGHT: case REMOTE_SELECT:
        x = g->px;
        y = g->py;
        rx = ry = g->pr + MAX(1.2, 0.035 * g->w) + 2;
        break;
    case REMOTE_BACK: case REMOTE_PLAY: case REMOTE_MUTE:
        x = g->c0;
        y = g->row[part == REMOTE_BACK ? 0 : part == REMOTE_PLAY ? 1 : 2];
        rx = ry = g->br + cut;
        break;
    case REMOTE_TV:
        x = g->c1;
        y = g->row[0];
        rx = ry = g->br + cut;
        break;
    case REMOTE_VOLUP: case REMOTE_VOLDOWN:
        x = g->c1;
        y = (g->row[1] + g->row[2]) / 2;
        rx = g->br + cut;
        ry = (g->row[2] - g->row[1]) / 2 + g->br + cut;
        break;
    case REMOTE_POWER:
        x = g->wx;
        y = g->wy;
        rx = ry = g->wr + cut;
        break;
    case REMOTE_SIRI:
        x = g->x + g->w;
        y = (g->siri0 + g->siri1) / 2;
        rx = 3 * MAX(1.6, 0.045 * g->w) + 2;
        ry = (g->siri1 - g->siri0) / 2 + 2;
        break;
    default:
        b[0] = b[1] = b[2] = b[3] = 0;
        return;
    }
    b[0] = floor(x - rx);
    b[1] = floor(y - ry);
    b[2] = ceil(x + rx);
    b[3] = ceil(y + ry);
}

/*
 * The rows (whole pixels: from [0], [1] excluded) where a part's look shows: its own, and its key's in the legend
 * when that row is shown. Returns how many bands.
 */
static int remote_part_rows(const RemoteGeometry *g, int part, double rows[2][2])
{
    double b[4];
    int n = 0, i, k;

    remote_part_extent(g, part, b);
    if (b[3] > b[1]) {
        rows[n][0] = b[1];
        rows[n++][1] = b[3];
    }
    for (i = 0; i < g->rows; i++) {
        for (k = 0; remote_legend[i][k]; k++) {
            if (remote_legend[i][k][0] == 'k' && (remote_key_parts(remote_legend[i][k] + 2) & REMOTE_BIT(part))) {
                double cy = floor(g->ly + (i + 0.5) * REMOTE_ROW);

                rows[n][0] = cy - REMOTE_ROW / 2;
                rows[n++][1] = cy + REMOTE_ROW / 2 + 1;
                return n;
            }
        }
    }
    return n;
}

/* Where the TV ends: right of it lie only the wall, the remote and its legend (what a part's look can change) */
static double remote_tv_right(const RemoteGeometry *g)
{
    return floor(g->sx + g->sw + g->bz);
}

/*
 * The remote, the TV and the wall, as far as they lie in @cr's clip (paint whole rows: cairo's antialiasing of an
 * edge on a row depends on the other edges on that row, so a box would come out a level off here and there)
 */
static void remote_paint(cairo_t *cr, const RemoteGeometry *g, const RemoteLook *l, PangoContext *pc)
{
    double x0, y0, x1, y1;
    int part;

    cairo_clip_extents(cr, &x0, &y0, &x1, &y1);
    remote_paint_wall(cr, g, x0, y0, x1, y1);
    if (x0 < g->sx + g->sw + g->bz + 1 && y0 < g->sy + g->sh + g->bz + 1 && y1 > g->sy - g->bz - 1) {
        remote_paint_tv(cr, g);
    }
    if (y0 < g->y + g->h + 6 && y1 > g->y - 6) {
        remote_siri(cr, g, l);
        remote_body(cr, g);
        if (y0 < g->wy + g->wr + 4 && y1 > g->wy - g->wr - 4) {
            remote_round_key(cr, g, l, REMOTE_POWER, g->wx, g->wy, g->wr);
        }
        if (y0 < g->py + g->pr + 6 && y1 > g->py - g->pr - 6) {
            remote_clickpad(cr, g, l);
        }
        for (part = REMOTE_BACK; part <= REMOTE_MUTE; part++) {
            double b[4];

            remote_part_extent(g, part, b);
            if (y0 < b[3] && y1 > b[1]) {
                remote_round_key(cr, g, l, part, part == REMOTE_TV ? g->c1 : g->c0,
                                 g->row[part == REMOTE_BACK || part == REMOTE_TV ? 0 : part == REMOTE_PLAY ? 1 : 2],
                                 g->br);
            }
        }
        if (y0 < g->row[2] + g->br + 6 && y1 > g->row[1] - g->br - 6) {
            remote_rocker(cr, g, l);
        }
    }
    if (g->rows && y1 > g->ly) {
        remote_draw_legend(cr, g, l, pc, y0, y1);
    }
}

/* The parts whose look differs from @was to @l */
static unsigned int remote_changed(const RemoteLook *was, const RemoteLook *l)
{
    unsigned int changed = was->lit ^ l->lit;

    if (was->hover != l->hover) {
        changed |= REMOTE_BIT(was->hover) | REMOTE_BIT(l->hover);
    }
    return changed & ~REMOTE_BIT(REMOTE_NONE);
}

/*
 * The parts' looks going from @was to @l, on a layer @width wide painted for @was: the whole rows of the parts that
 * changed are painted again (theirs on the remote, their keys' in the legend). Exactly what a whole repaint gives.
 */
static void remote_paint_parts(cairo_surface_t *layer, double width, const RemoteGeometry *g, const RemoteLook *was,
                               const RemoteLook *l, PangoContext *pc)
{
    unsigned int changed = remote_changed(was, l);
    double rows[2][2];
    cairo_t *cr;
    int part, n, i;

    if (!changed) {
        return;
    }
    cr = cairo_create(layer);
    for (part = REMOTE_UP; part < REMOTE_PARTS; part++) {
        if (changed & REMOTE_BIT(part)) {
            n = remote_part_rows(g, part, rows);
            for (i = 0; i < n; i++) {
                cairo_rectangle(cr, 0, rows[i][0], width, rows[i][1] - rows[i][0]);
            }
        }
    }
    cairo_clip(cr);
    remote_paint(cr, g, l, pc);
    cairo_destroy(cr);
}

/* What is under the pointer: a part, or REMOTE_NONE (some slack around each) */
static int remote_hit(const RemoteGeometry *g, double x, double y)
{
    double dx = x - g->px, dy = y - g->py, d = hypot(dx, dy), slack = MAX(1.5, MIN(4, 0.05 * g->w));
    int part;

    if (d <= g->pr + slack) {
        if (d <= g->pc) {
            return REMOTE_SELECT;
        }
        if (fabs(dx) > fabs(dy)) {
            return dx > 0 ? REMOTE_RIGHT : REMOTE_LEFT;
        }
        return dy > 0 ? REMOTE_DOWN : REMOTE_UP;
    }
    if (hypot(x - g->wx, y - g->wy) <= g->wr + MAX(5, 0.04 * g->w)) {
        return REMOTE_POWER;
    }
    if (fabs(x - g->c1) <= g->br + slack && y >= g->row[1] - g->br - slack && y <= g->row[2] + g->br + slack) {
        return y < (g->row[1] + g->row[2]) / 2 ? REMOTE_VOLUP : REMOTE_VOLDOWN;
    }
    for (part = REMOTE_BACK; part <= REMOTE_MUTE; part++) {
        double cx = part == REMOTE_TV ? g->c1 : g->c0;
        double cy = g->row[part == REMOTE_BACK || part == REMOTE_TV ? 0 : part == REMOTE_PLAY ? 1 : 2];

        if (hypot(x - cx, y - cy) <= g->br + slack) {
            return part;
        }
    }
    if (x >= g->x + g->w - 4 && x <= g->x + g->w + 10 && y >= g->siri0 - 4 && y <= g->siri1 + 4) {
        return REMOTE_SIRI;
    }
    return REMOTE_NONE;
}
