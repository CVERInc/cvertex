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
