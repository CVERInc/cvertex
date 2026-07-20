// core.c — the framebuffer and the rasterizer. Plain C, zero dependencies.
//
// Everything here is engine: pixels, a palette, a sine table, filling polygons. Nothing
// here knows what a character is, or what gravity is, or what the rules are. That side
// of the line lives in games/ — see game.h for why.
#include "core.h"
#include <math.h>

int16_t g_sin[1024];

void tables_init(void) {
    for (int i = 0; i < 1024; i++)
        g_sin[i] = (int16_t)(sinf((float)i * 6.2831853f / 1024.f) * 32767.f);
}

uint8_t  g_fb[MAXFBW * MAXFBH];
int g_fbw, g_fbh;
int32_t g_dev_camz;
int      g_mx, g_my;   // pointer, in framebuffer pixels (top-left origin)
uint8_t  g_mbtn;       // bit0 = left button, bit1 = right button
uint8_t  g_view_toggle;// one-frame edge: platform pulses (Tab / --view), a game consumes it
uint8_t  g_esc;        // one-frame edge: platform pulses (Esc / CV_ESC_AT), the shell routes it
int      g_menu_return;   // platform -> menu: returned from a game, play the insert in reverse (all platforms link)
int      g_quit;          // menu -> platform: the CRT power-off finished, stop the loop
// The OPTIONS panel's settings. 🔴 Zero-init IS the default for every one — off / solo /
// windowed / first-person — so the synth.c non-zero-initializer lesson holds (these live in
// __bss, nothing on disk) AND "untouched == today's behaviour" is true by construction. The
// menu raises g_crt_off to 1 at init() (the collapse plays by default); mac.c sets g_fullscreen
// from the --fullscreen flag. Both are runtime assignments, never data-segment initializers.
int      g_gentle, g_coop, g_fullscreen, g_crt_off, g_cam_chase;

void fb_resize(int w, int h) {
    if (w > MAXFBW) w = MAXFBW;
    if (h > MAXFBH) h = MAXFBH;
    g_fbw = w; g_fbh = h;
}
uint32_t g_pal[256];

void fb_clear(uint8_t ci) {
    for (int i = 0; i < g_fbw * g_fbh; i++) g_fb[i] = ci;
}

// Scanline fill: for each scanline, find every edge intersection, sort, fill pairs.
// Even-odd rule. Integer math, no floats → deterministic.
void poly_fill_n(const int16_t *pts, const uint16_t *lens, int nc, uint8_t ci) {
    int total = 0;
    for (int c = 0; c < nc; c++) total += lens[c];
    if (total < 3) return;

    int miny = pts[1], maxy = pts[1];
    for (int i = 1; i < total; i++) {
        int y = pts[i * 2 + 1];
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (miny < 0) miny = 0;
    if (maxy > g_fbh - 1) maxy = g_fbh - 1;

    int xs[MAXPTS];
    for (int y = miny; y <= maxy; y++) {
        int cnt = 0;
        int base = 0;
        for (int c = 0; c < nc; c++) {              // each contour closes on its own; no edges cross between contours
            int n = lens[c];
            for (int i = 0; i < n && cnt < MAXPTS; i++) {
                int p = base + i, q = base + (i + 1) % n;
                int y0 = pts[p * 2 + 1], y1 = pts[q * 2 + 1];
                if (y0 == y1) continue;
                // half-open [min,max) → a shared vertex isn't counted twice
                int ymin = y0 < y1 ? y0 : y1;
                int ymax = y0 < y1 ? y1 : y0;
                if (y < ymin || y >= ymax) continue;
                int x0 = pts[p * 2], x1 = pts[q * 2];
                xs[cnt++] = x0 + (int)((int32_t)(y - y0) * (x1 - x0) / (y1 - y0));
            }
            base += n;
        }
        // insertion sort (cnt is small)
        for (int a = 1; a < cnt; a++) {
            int v = xs[a], b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < cnt; a += 2) {
            int L = xs[a], R = xs[a + 1];
            if (R < 0 || L > g_fbw - 1) continue;
            if (L < 0) L = 0;
            if (R > g_fbw - 1) R = g_fbw - 1;
            uint8_t *row = &g_fb[y * g_fbw];
            for (int x = L; x <= R; x++) row[x] = ci;
        }
    }
}

void poly_fill(const int16_t *pts, int n, uint8_t ci) {
    uint16_t one = (uint16_t)n;
    poly_fill_n(pts, &one, 1, ci);
}


// ---- depth-buffered triangles ------------------------------------------------

uint32_t g_zb[MAXFBW * MAXFBH];

void zb_clear(void) {
    for (int i = 0; i < g_fbw * g_fbh; i++) g_zb[i] = 0;   // 0 = infinitely far
}

// Scanline fill with a per-pixel depth test. Only 1/z is linear in screen space, so
// that's what's interpolated — the alternative is a depth test that's subtly wrong in
// exactly the places perspective matters.
void tri_fill_z(const int16_t *xy, const uint32_t *w, uint8_t ci) {
    int x0 = xy[0], y0 = xy[1], x1 = xy[2], y1 = xy[3], x2 = xy[4], y2 = xy[5];
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (miny < 0) miny = 0;
    if (maxy > g_fbh - 1) maxy = g_fbh - 1;
    if (miny > maxy) return;

    // Edge function denominator: twice the signed area. Zero = degenerate, nothing to do.
    int32_t area = (int32_t)(x1 - x0) * (y2 - y0) - (int32_t)(x2 - x0) * (y1 - y0);
    if (area == 0) return;

    for (int y = miny; y <= maxy; y++) {
        int xs[3], ws_i = 0;
        int32_t xl = 0, xr = 0;
        // Find this scanline's span from the three edges.
        int cnt = 0; int px[3];
        int vx[3] = { x0, x1, x2 }, vy[3] = { y0, y1, y2 };
        for (int e = 0; e < 3 && cnt < 2; e++) {
            int a = e, b = (e + 1) % 3;
            int ya = vy[a], yb = vy[b];
            if (ya == yb) continue;
            int lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
            if (y < lo || y >= hi) continue;
            px[cnt++] = vx[a] + (int)((int32_t)(y - ya) * (vx[b] - vx[a]) / (yb - ya));
        }
        if (cnt < 2) continue;
        xl = px[0] < px[1] ? px[0] : px[1];
        xr = px[0] < px[1] ? px[1] : px[0];
        if (xr < 0 || xl > g_fbw - 1) continue;
        if (xl < 0) xl = 0;
        if (xr > g_fbw - 1) xr = g_fbw - 1;

        uint8_t *row = &g_fb[y * g_fbw];
        uint32_t *zrow = &g_zb[y * g_fbw];
        for (int x = xl; x <= xr; x++) {
            // Barycentric from edge functions — exact, and it gives the 1/z weights.
            int32_t b0 = (int32_t)(x1 - x) * (y2 - y) - (int32_t)(x2 - x) * (y1 - y);
            int32_t b1 = (int32_t)(x2 - x) * (y0 - y) - (int32_t)(x0 - x) * (y2 - y);
            int32_t b2 = area - b0 - b1;
            int64_t ww = ((int64_t)b0 * w[0] + (int64_t)b1 * w[1] + (int64_t)b2 * w[2]) / area;
            if (ww <= 0) continue;
            uint32_t d = (uint32_t)ww;
            if (d > zrow[x]) { zrow[x] = d; row[x] = ci; }
        }
        (void)xs; (void)ws_i;
    }
}
