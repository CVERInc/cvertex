// vikings.c — two characters, gravity, a floor. The oldest test in the tree: it's what
// the engine was first pointed at, and it stays because it exercises input routing,
// facing, extrusion and local co-op at once.
#include "core.h"
#include "g3d.h"
#include "shape.h"
#include "game.h"

// Scratch art for pipeline work lives outside the tree (see .gitignore) and is simply
// absent from a clean checkout. Nothing here is part of the engine.
#if defined(__has_include)
#  if __has_include("../src/local/shape_demo.h")
#    include "../src/local/shape_demo.h"
#    define HAVE_DEMO_TURN 1
#  endif
#endif

static uint32_t g_frame;
static uint64_t g_checksum;

// Events this game emits. The vocabulary is the GAME's, not the engine's — an engine
// that knew what a jump was would be a game.
static uint8_t g_events;
#define EV_JUMP_A 1
#define EV_JUMP_B 2
#define EV_LAND_A 4
#define EV_LAND_B 8

#include "synth.h"

// ---- deterministic simulation ---------------------------------------------------
// Fixed point: 1 unit = 1/256 pixel. Integer math → bit-exact reproducibility.
#define FP 8
typedef struct { int32_t x, y, vx, vy; int16_t facing; uint8_t grounded; } Actor;
static Actor g_act[2];
static uint64_t g_checksum;

static void init(void) {
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


#define GROUND (300 << FP)   // virtual pixels
#define CAMZ    (5 << 16)      // camera distance. The sim never sees this.
#define PROJ_V  VH             // focal length in virtual pixels
#define CH_SIZE (210 << 10)    // character height, world units (~1.6)
#define CH_PX   (((int64_t)CH_SIZE * PROJ_V) / CAMZ)   // ...and in virtual pixels, derived not guessed
#define FOOT     8             // actor origin sits this far above the soles


static void tick(const Input in[2]) {
    g_events = 0;
    g_frame++;   // rotation angle is driven by the frame count, never the clock → determinism holds
    for (int i = 0; i < 2; i++) {
        Actor *a = &g_act[i];
        a->vx = in[i].x * (2 << FP) / 2;
        if (in[i].jump && a->grounded) {
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

static void draw(void) {
    fb_clear(0);



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
        const Shape *v = turn_pick(&g_hero, g_act[i].facing, &flip, &residual);
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

// tick decided something happened; this decides what it sounds like. Separate, so the
// audio thread never gets a path back into determinism.
static void audio(void) {
    if (g_events & (EV_JUMP_A | EV_JUMP_B)) synth_note(NCHAN - 1, 5, (g_events & EV_JUMP_A) ? 84 : 79, 180);
    if (g_events & (EV_LAND_A | EV_LAND_B)) synth_note(NCHAN - 1, 4, 48, 140);
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_vikings = { "vikings", init, tick, audio, draw, checksum };
