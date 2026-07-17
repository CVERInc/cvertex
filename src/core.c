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
#    define HAVE_DEMO_TURN 1
#  endif
#endif

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

// ---- deterministic simulation ---------------------------------------------------
// Fixed point: 1 unit = 1/256 pixel. Integer math → bit-exact reproducibility.
#define FP 8
typedef struct { int32_t x, y, vx, vy; int16_t facing; uint8_t grounded; } Actor;
static Actor g_act[2];
uint64_t g_checksum;   // determinism self-check

void sim_init(void) {
    tables_init();
    g_act[0] = (Actor){ 160 << FP, 200 << FP, 0, 0, 768, 0 };
    g_act[1] = (Actor){ 480 << FP, 200 << FP, 0, 0, 256, 0 };
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
#ifdef HAVE_DEMO_TURN
    shape_install_palette(g_hero.view[0]);   // one shared palette; any view carries it
#endif
    g_frame = 0;
}

uint32_t g_frame;
int g_demo_spin;   // dev: spin the characters to inspect the turnaround

#define GROUND (300 << FP)   // virtual pixels
#define CAMZ    (5 << 16)      // camera distance. The sim never sees this.
#define PROJ_V  VH             // focal length in virtual pixels
#define CH_SIZE (210 << 10)    // character height, world units (~1.6)
#define CH_PX   (((int64_t)CH_SIZE * PROJ_V) / CAMZ)   // ...and in virtual pixels, derived not guessed
#define FOOT     8             // actor origin sits this far above the soles

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
        if (a->x < (8 << FP))       a->x = 8 << FP;
        if (a->x > ((VW - 8) << FP)) a->x = (VW - 8) << FP;
        // Turning is state, not decoration: reverse direction and she rotates through
        // the drawn views to get there instead of flipping in a single frame.
        if (in[i].x) {
            // Which drawn angle looks screen-right is a property of the ART, not of the
            // maths: this sheet's 90 degree view faces LEFT. Read the sheet, don't assume.
            int want = in[i].x > 0 ? 768 : 256;
            int diff = ((want - a->facing + 512) & 1023) - 512;   // shortest way round
            int step = diff > 40 ? 40 : (diff < -40 ? -40 : diff);
            a->facing = (int16_t)((a->facing + step) & 1023);
        }
        g_checksum = g_checksum * 31 + (uint32_t)a->x + (uint32_t)a->y + (uint32_t)a->facing;
    }
}

void sim_draw(void) {
    fb_clear(0);

    // 3D behind, 2D in front — the smallest proof of "render in 3D, play in 2D".
    g3d_draw(&g_torus, (int)(g_frame * 3 / 2), (int)(g_frame * 2), 0, 5 << 16);

    int16_t ground[8] = { 0, 316, VW, 316, VW, VH, 0, VH };   // virtual; g3d scales it
    poly_fill(ground, 4, 1);

    // The two characters.
    for (int i = 0; i < 2; i++) {
        int cx = g_act[i].x >> FP, cy = g_act[i].y >> FP;
#ifdef HAVE_DEMO_TURN
        // Gameplay is 2D, so the sim only ever thinks in screen pixels. The conversion
        // happens here and nowhere else — the one place that opinion meets the camera.
        int32_t wpp = world_per_px(CAMZ);
        int cyc = cy + FOOT * 2 - (int)(CH_PX / 2);          // soles on the actor, not the middle
        int32_t wx =  (cx - VW / 2) * wpp;
        int32_t wy = -(cyc - VH / 2) * wpp;
        int flip, residual;
        int fc = g_demo_spin ? (int)((g_frame * 4) & 1023) : g_act[i].facing;
        const Shape *v = turn_pick(&g_hero, fc, &flip, &residual);
        if (v) shape_draw3d(v, wx, wy, CAMZ, 0, residual, 0, CH_SIZE, 9000, flip);
#else
        // No art present (a clean checkout): a placeholder body, so the engine still runs.
        int16_t body[12] = {
            cx,     cy - 24,  cx + 14, cy - 8,  cx + 10, cy + 16,
            cx,     cy + 8,   cx - 10, cy + 16, cx - 14, cy - 8,
        };
        poly_fill(body, 6, 2 + i);
#endif
    }
}
