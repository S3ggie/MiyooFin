/*
 * miyoofin-player on-screen display: draws the playback overlay straight into a
 * YUV420P picture. Pure C99 with no FFmpeg/SDL dependency so it also compiles
 * as C++ and is unit-tested on the host (tests/test_player.cpp).
 *
 * Coordinates are LOGICAL (what the viewer sees). The device shows the video
 * rotated 180 degrees (the player's vflip,hflip filter), so every drawing
 * primitive maps logical (x, y) to picture (w-1-x, h-1-y) when `rot180` is set.
 * Palette and 8x16 font come from the app (src/ui/Design.hpp, BitmapFont).
 */
#ifndef MIYOOFIN_PLAYER_OSD_H
#define MIYOOFIN_PLAYER_OSD_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "osd_font.h"

typedef struct OsdPicture {
    uint8_t *data[3]; /* Y, U, V */
    int linesize[3];
    int w, h;
    int rot180;
} OsdPicture;

typedef struct OsdModel {
    double pos_sec;   /* current playback position, seconds */
    double dur_sec;   /* total duration, <= 0 when unknown */
    int paused;
    int bar_visible;  /* transport bar (time + seek bar) */
    const char *toast; /* short message near the top, or NULL */
} OsdModel;

typedef struct OsdRgb {
    uint8_t r, g, b;
} OsdRgb;

/* Design palette (src/ui/Design.hpp). */
#define OSD_PANEL_BG {7, 9, 18}
#define OSD_TRACK {38, 46, 68}
#define OSD_ACCENT {60, 150, 255}
#define OSD_ACCENT_HI {150, 205, 255}
#define OSD_TEXT {238, 244, 255}
#define OSD_TEXT_DIM {175, 180, 195}

/* Height (at scale 1) of the subtitle area above the transport panel. */
#define OSD_SUB_BAND 100

static inline int osd_scale(const OsdPicture *p)
{
    return p->w > 800 ? 2 : 1;
}

static inline int osd_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void osd_rgb_to_yuv(OsdRgb c, int *y, int *u, int *v)
{
    /* BT.601 limited range. */
    *y = ((66 * c.r + 129 * c.g + 25 * c.b + 128) >> 8) + 16;
    *u = ((-38 * c.r - 74 * c.g + 112 * c.b + 128) >> 8) + 128;
    *v = ((112 * c.r - 94 * c.g - 18 * c.b + 128) >> 8) + 128;
}

/* Blend one LOGICAL luma pixel (alpha 0..255). */
static inline void osd_luma(const OsdPicture *p, int x, int y, int Y, int alpha)
{
    if (x < 0 || y < 0 || x >= p->w || y >= p->h)
        return;
    if (p->rot180) {
        x = p->w - 1 - x;
        y = p->h - 1 - y;
    }
    uint8_t *d = p->data[0] + y * p->linesize[0] + x;
    *d = (uint8_t)((*d * (255 - alpha) + Y * alpha + 127) / 255);
}

/* Blend one chroma sample at picture chroma coordinates (cx, cy). */
static inline void osd_chroma_at(const OsdPicture *p, int cx, int cy, int U, int V, int alpha)
{
    int cw = (p->w + 1) / 2, ch = (p->h + 1) / 2;
    if (cx < 0 || cy < 0 || cx >= cw || cy >= ch)
        return;
    uint8_t *du = p->data[1] + cy * p->linesize[1] + cx;
    uint8_t *dv = p->data[2] + cy * p->linesize[2] + cx;
    *du = (uint8_t)((*du * (255 - alpha) + U * alpha + 127) / 255);
    *dv = (uint8_t)((*dv * (255 - alpha) + V * alpha + 127) / 255);
}

/* Filled rectangle in logical coordinates. */
static inline void osd_rect(const OsdPicture *p, int x, int y, int w, int h, OsdRgb c, int alpha)
{
    int Y, U, V;
    int ix, iy;
    int x0 = osd_clampi(x, 0, p->w), x1 = osd_clampi(x + w, 0, p->w);
    int y0 = osd_clampi(y, 0, p->h), y1 = osd_clampi(y + h, 0, p->h);
    if (x1 <= x0 || y1 <= y0)
        return;
    osd_rgb_to_yuv(c, &Y, &U, &V);
    for (iy = y0; iy < y1; iy++)
        for (ix = x0; ix < x1; ix++)
            osd_luma(p, ix, iy, Y, alpha);
    /* Chroma: one sample per 2x2 picture pixels covered. Work in picture space. */
    {
        int px0 = p->rot180 ? p->w - x1 : x0, px1 = p->rot180 ? p->w - x0 : x1;
        int py0 = p->rot180 ? p->h - y1 : y0, py1 = p->rot180 ? p->h - y0 : y1;
        int cx, cy;
        for (cy = py0 / 2; cy < (py1 + 1) / 2; cy++)
            for (cx = px0 / 2; cx < (px1 + 1) / 2; cx++)
                osd_chroma_at(p, cx, cy, U, V, alpha);
    }
}

/* One glyph (8x16 scaled by k) at logical (x, y). Returns advance in pixels. */
static inline int osd_glyph(const OsdPicture *p, int x, int y, unsigned char ch, OsdRgb c, int k)
{
    int Y, U, V, row, col;
    if (ch < 32 || ch > 126)
        ch = '?';
    osd_rgb_to_yuv(c, &Y, &U, &V);
    for (row = 0; row < 16; row++) {
        unsigned char bits = osd_font[ch - 32][row];
        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                int sx, sy;
                for (sy = 0; sy < k; sy++)
                    for (sx = 0; sx < k; sx++) {
                        int lx = x + col * k + sx, ly = y + row * k + sy;
                        osd_luma(p, lx, ly, Y, 255);
                        /* Text pixels own their chroma sample: pin it to the colour. */
                        {
                            int px = p->rot180 ? p->w - 1 - lx : lx;
                            int py = p->rot180 ? p->h - 1 - ly : ly;
                            osd_chroma_at(p, px / 2, py / 2, U, V, 255);
                        }
                    }
            }
        }
    }
    return 8 * k;
}

static inline int osd_text_width(const char *s, int k)
{
    return (int)strlen(s) * 8 * k;
}

static inline int osd_text(const OsdPicture *p, int x, int y, const char *s, OsdRgb c, int k)
{
    int w = 0;
    for (; *s; s++)
        w += osd_glyph(p, x + w, y, (unsigned char)*s, c, k);
    return w;
}

/* "m:ss" or "h:mm:ss"; negative/NaN become 0:00. */
static inline void osd_format_time(double sec, char *out, size_t cap)
{
    long t = (sec > 0 && sec == sec) ? (long)sec : 0;
    long h = t / 3600, m = (t / 60) % 60, s = t % 60;
    if (h > 0)
        snprintf(out, cap, "%ld:%02ld:%02ld", h, m, s);
    else
        snprintf(out, cap, "%ld:%02ld", m, s);
}

/* Filled triangle pointing right (play icon) inside box (x, y, size). */
static inline void osd_icon_play(const OsdPicture *p, int x, int y, int size, OsdRgb c)
{
    int row;
    for (row = 0; row < size; row++) {
        int d = 2 * row - (size - 1);
        if (d < 0)
            d = -d;
        osd_rect(p, x, y + row, size - d, 1, c, 255);
    }
}

static inline void osd_icon_pause(const OsdPicture *p, int x, int y, int size, OsdRgb c)
{
    int bar = size / 3;
    osd_rect(p, x, y, bar, size, c, 255);
    osd_rect(p, x + size - bar, y, bar, size, c, 255);
}

/* Geometry of the transport bar (logical), shared by drawing and tests. */
typedef struct OsdBarGeometry {
    int panel_y, panel_h;
    int bar_x, bar_y, bar_w, bar_h;
    int icon_x, icon_y, icon_size;
    int pos_x, text_y;
    int dur_right;
} OsdBarGeometry;

static inline OsdBarGeometry osd_bar_geometry(const OsdPicture *p)
{
    OsdBarGeometry g;
    int k = osd_scale(p);
    g.panel_h = 52 * k;
    g.panel_y = p->h - g.panel_h;
    g.icon_size = 16 * k;
    g.icon_x = 16 * k;
    g.icon_y = g.panel_y + (g.panel_h - g.icon_size) / 2;
    g.pos_x = g.icon_x + g.icon_size + 12 * k;
    g.text_y = g.panel_y + (g.panel_h - 16 * k) / 2;
    g.bar_x = g.pos_x + 8 * 8 * k + 8 * k; /* room for "h:mm:ss" */
    g.dur_right = p->w - 16 * k;
    g.bar_w = g.dur_right - 8 * 8 * k - 8 * k - g.bar_x;
    g.bar_h = 4 * k;
    g.bar_y = g.panel_y + (g.panel_h - g.bar_h) / 2;
    return g;
}

/* Fraction (0..1) of the bar filled for this model. */
static inline double osd_progress(const OsdModel *m)
{
    if (!(m->dur_sec > 0))
        return 0.0;
    double f = m->pos_sec / m->dur_sec;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

/* Rectangles (PICTURE coordinates) osd_render may touch, as {x, y, w, h}: the
 * toast strip, the transport panel and the centre badge. The player saves these
 * before drawing and restores them when the same frame is shown again. */
#define OSD_FOOTPRINT_RECTS 3
static inline void osd_footprint(const OsdPicture *p, int rects[OSD_FOOTPRINT_RECTS][4])
{
    OsdBarGeometry g = osd_bar_geometry(p);
    int k = osd_scale(p);
    int cs = 64 * k;
    int logical[OSD_FOOTPRINT_RECTS][4];
    int i;
    logical[0][0] = 0;
    logical[0][1] = 12 * k;
    logical[0][2] = p->w;
    logical[0][3] = 28 * k;
    /* Bottom band: the transport panel plus room for 3 subtitle lines above it. */
    logical[1][0] = 0;
    logical[1][1] = g.panel_y - OSD_SUB_BAND * k;
    logical[1][2] = p->w;
    logical[1][3] = g.panel_h + OSD_SUB_BAND * k;
    logical[2][0] = (p->w - cs) / 2;
    logical[2][1] = (p->h - cs) / 2;
    logical[2][2] = cs;
    logical[2][3] = cs;
    for (i = 0; i < OSD_FOOTPRINT_RECTS; i++) {
        rects[i][0] = p->rot180 ? p->w - (logical[i][0] + logical[i][2]) : logical[i][0];
        rects[i][1] = p->rot180 ? p->h - (logical[i][1] + logical[i][3]) : logical[i][1];
        rects[i][2] = logical[i][2];
        rects[i][3] = logical[i][3];
    }
}

/* Blends an RGBA (byte order R,G,B,A) bitmap at logical (x, y), honouring its
 * alpha, rotation and the picture bounds. Used for rendered subtitle text. */
static inline void osd_blit_rgba(const OsdPicture *p, int x, int y, const uint8_t *rgba, int w,
                                 int h, int pitch)
{
    int ix, iy;
    for (iy = 0; iy < h; iy++) {
        int ly = y + iy;
        if (ly < 0 || ly >= p->h)
            continue;
        for (ix = 0; ix < w; ix++) {
            const uint8_t *s = rgba + iy * pitch + ix * 4;
            int lx = x + ix, a = s[3], Y, U, V, px, py;
            OsdRgb c;
            if (!a || lx < 0 || lx >= p->w)
                continue;
            c.r = s[0];
            c.g = s[1];
            c.b = s[2];
            osd_rgb_to_yuv(c, &Y, &U, &V);
            osd_luma(p, lx, ly, Y, a);
            px = p->rot180 ? p->w - 1 - lx : lx;
            py = p->rot180 ? p->h - 1 - ly : ly;
            osd_chroma_at(p, px / 2, py / 2, U, V, a);
        }
    }
}

/* Draws the whole overlay for `m` into `p`. */
static inline void osd_render(const OsdPicture *p, const OsdModel *m)
{
    OsdRgb bg = OSD_PANEL_BG, track = OSD_TRACK, accent = OSD_ACCENT, hi = OSD_ACCENT_HI;
    OsdRgb text = OSD_TEXT, dim = OSD_TEXT_DIM;
    int k = osd_scale(p);
    char buf[32];

    if (m->paused) {
        /* Centre badge: dark disc-ish square with the pause icon. */
        int cs = 64 * k;
        int cx = (p->w - cs) / 2, cy = (p->h - cs) / 2;
        osd_rect(p, cx, cy, cs, cs, bg, 190);
        osd_rect(p, cx, cy, cs, 2 * k, accent, 255);
        osd_icon_pause(p, cx + cs / 4, cy + cs / 4, cs / 2, text);
    }

    if (m->toast && m->toast[0]) {
        int tw = osd_text_width(m->toast, k) + 24 * k;
        int th = 28 * k;
        int tx = (p->w - tw) / 2, ty = 12 * k;
        osd_rect(p, tx, ty, tw, th, bg, 210);
        osd_rect(p, tx, ty + th - 2 * k, tw, 2 * k, accent, 255);
        osd_text(p, tx + 12 * k, ty + (th - 16 * k) / 2, m->toast, text, k);
    }

    if (m->bar_visible || m->paused) {
        OsdBarGeometry g = osd_bar_geometry(p);
        double f = osd_progress(m);
        int fill = (int)(g.bar_w * f);
        osd_rect(p, 0, g.panel_y, p->w, g.panel_h, bg, 215);
        osd_rect(p, 0, g.panel_y, p->w, k, accent, 255);
        if (m->paused)
            osd_icon_pause(p, g.icon_x, g.icon_y, g.icon_size, text);
        else
            osd_icon_play(p, g.icon_x, g.icon_y, g.icon_size, text);
        osd_format_time(m->pos_sec, buf, sizeof(buf));
        osd_text(p, g.pos_x, g.text_y, buf, text, k);
        osd_rect(p, g.bar_x, g.bar_y, g.bar_w, g.bar_h, track, 255);
        if (fill > 0)
            osd_rect(p, g.bar_x, g.bar_y, fill, g.bar_h, accent, 255);
        osd_rect(p, g.bar_x + fill - 3 * k, g.bar_y - 3 * k, 6 * k, g.bar_h + 6 * k, hi, 255);
        if (m->dur_sec > 0) {
            osd_format_time(m->dur_sec, buf, sizeof(buf));
            osd_text(p, g.dur_right - osd_text_width(buf, k), g.text_y, buf, dim, k);
        }
    }
}

#endif /* MIYOOFIN_PLAYER_OSD_H */
