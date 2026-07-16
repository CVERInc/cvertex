// core.c — framebuffer + polygon rasterizer + deterministic simulation. Plain C, zero dependencies.
#include "core.h"
#include "g3d.h"
#include "shape.h"
#include <math.h>

// Scratch art for pipeline work lives outside the tree (see .gitignore) and is
// simply absent from a clean checkout. Nothing here is part of the engine.
#if defined(__has_include)
#  if __has_include("local/shape_demo.h")
#    include "local/shape_demo.h"
#    define HAVE_DEMO_SHAPE 1
#  endif
#endif

int16_t g_sin[1024];

void tables_init(void) {
    for (int i = 0; i < 1024; i++)
        g_sin[i] = (int16_t)(sinf((float)i * 6.2831853f / 1024.f) * 32767.f);
}

uint8_t  g_fb[FBW * FBH];
uint32_t g_pal[256];

void fb_clear(uint8_t ci) {
    for (int i = 0; i < FBW * FBH; i++) g_fb[i] = ci;
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
    if (maxy > FBH - 1) maxy = FBH - 1;

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
            if (R < 0 || L > FBW - 1) continue;
            if (L < 0) L = 0;
            if (R > FBW - 1) R = FBW - 1;
            uint8_t *row = &g_fb[y * FBW];
            for (int x = L; x <= R; x++) row[x] = ci;
        }
    }
}

void poly_fill(const int16_t *pts, int n, uint8_t ci) {
    uint16_t one = (uint16_t)n;
    poly_fill_n(pts, &one, 1, ci);
}

// ---- deterministic simulation ---------------------------------------------------
// Fixed point: 1 unit = 1/256 pixel. Integer math → bit-exact reproducibility.
#define FP 8
typedef struct { int32_t x, y, vx, vy; uint8_t grounded; } Actor;
static Actor g_act[2];
uint64_t g_checksum;   // determinism self-check

void sim_init(void) {
    tables_init();
    g_act[0] = (Actor){ 80 << FP, 100 << FP, 0, 0, 0 };
    g_act[1] = (Actor){ 240 << FP, 100 << FP, 0, 0, 0 };
    g_checksum = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;  // background
    g_pal[1] = 0xFF5D275D;  // ground
    g_pal[2] = 0xFFEF7D57;  // character A
    g_pal[3] = 0xFF41A6F6;  // character B

    // 8..15 = one material's eight-step brightness ramp. 3D lighting never touches a
    // pixel, it just picks a step on the ramp — that's the palette-index dividend:
    // lighting is a lookup, not a computation.
    for (int i = 0; i < 8; i++) {
        int r = 30 + i * 26, g = 90 + i * 22, b = 120 + i * 18;
        g_pal[8 + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
#ifdef HAVE_DEMO_SHAPE
    shape_install_palette(&g_demo);
#endif
    g_frame = 0;
}

uint32_t g_frame;

#define GROUND (150 << FP)

uint8_t g_events;

void sim_tick(const Input in[2]) {
    g_events = 0;
    g_frame++;   // rotation angle is driven by the frame count, never the clock → determinism holds
    for (int i = 0; i < 2; i++) {
        Actor *a = &g_act[i];
        a->vx = in[i].x * (2 << FP) / 2;
        if (in[i].act && a->grounded) {
            a->vy = -(5 << FP); a->grounded = 0;
            g_events |= (EV_JUMP_A << i);
        }
        a->vy += (1 << FP) / 4;                 // gravity
        a->x += a->vx;
        a->y += a->vy;
        if (a->y >= GROUND) {
            if (!a->grounded) g_events |= (EV_LAND_A << i);
            a->y = GROUND; a->vy = 0; a->grounded = 1;
        }
        if (a->x < (8 << FP))         a->x = 8 << FP;
        if (a->x > ((FBW - 8) << FP)) a->x = (FBW - 8) << FP;
        g_checksum = g_checksum * 31 + (uint32_t)a->x + (uint32_t)a->y;
    }
}

void sim_draw(void) {
    fb_clear(0);

    // 3D behind, 2D in front — the smallest proof of "render in 3D, play in 2D".
    g3d_draw(&g_torus, (int)(g_frame * 3 / 2), (int)(g_frame * 2), 0, 5 << 16);

#ifdef HAVE_DEMO_SHAPE
    shape_draw(&g_demo, FBW / 2, 92, 120);
#endif

    int16_t ground[8] = { 0, 158, FBW, 158, FBW, FBH, 0, FBH };
    poly_fill(ground, 4, 1);

    // Two characters, each one polygon (spike uses a hand-written shape in place of a motifmint asset)
    for (int i = 0; i < 2; i++) {
        int cx = g_act[i].x >> FP, cy = g_act[i].y >> FP;
        int16_t body[12] = {
            cx,     cy - 12,
            cx + 7, cy - 4,
            cx + 5, cy + 8,
            cx,     cy + 4,
            cx - 5, cy + 8,
            cx - 7, cy - 4,
        };
        poly_fill(body, 6, 2 + i);
    }
}
