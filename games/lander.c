// lander.c — the oldest genre there is: a rocket, a gravity well, a strip of ground that
// doesn't care how you feel about it.
//
// Everything else in this file is downstream of one asymmetry. Gravity always wins;
// thrust only borrows against it, and fuel is the loan running out. Rotating costs
// nothing because turning isn't the hard part — the hard part is that thrust always
// fires along your NOSE, not along "up", so pointing the ship is the actual control
// surface and the throttle is just a trigger.
//
// The ground isn't a floor, it's a verdict. Two conditions, checked once, at the one
// moment they can be checked: are you slow enough, level enough, and standing on a
// pad — or not. There's no partial credit and no second chance mid-fall; the whole
// game is spent buying the right to ask that question on your terms instead of
// gravity's.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- fixed point --------------------------------------------------------------
#define FP     16
#define F(v)   ((int32_t)(v) << FP)
#define PXI(v) ((int32_t)(v) >> FP)

#define ROT_STEP    10          // angle units/frame held, out of 1024 = one full turn
#define GRAVITY     900         // 16.16 px/frame^2 — weak, the way a moon's should be
#define THRUST      4500        // 16.16 px/frame^2 while burning — ~5x gravity, so
                                 // hovering costs a fifth of the throttle and braking
                                 // a hard fall is always possible if you started early
#define FUEL_START  800
#define FUEL_BURN   2
#define SAFE_VY     98304       // 1.5 px/frame descent — any faster snaps a leg
#define SAFE_VX     65536       // 1.0 px/frame sideways — a landing, not a slide
#define ANGLE_TOL   70          // ~25° off vertical still counts as "down"
#define SHIP_R      9

// ---- the ground -----------------------------------------------------------
// Not a level format, just a strip: NSEG points, SEG_W apart, height only. Two of the
// gaps between them get forced flat and marked as pads — one wide and forgiving, one
// narrow and worth three times as much, because a game about restraint should reward
// choosing the hard target on purpose.
#define NSEG     33
#define SEG_W    (VW / (NSEG - 1))   // 20, exactly — 640 / 32
#define NSEGM    (NSEG - 1)
#define PAD0_LO  5
#define PAD0_HI  8                   // 4 segments, 80px — the easy pad
#define PAD1_LO  24
#define PAD1_HI  25                  // 2 segments, 40px — the hard pad, pays 3x
#define NSTAR    40

static int16_t g_terrain[NSEG];
static uint8_t g_padseg[NSEGM];      // 0 rock, 1 easy pad, 2 hard pad
static int16_t g_star[NSTAR][2];

// 🔴 An LCG, not rand(): tick has to reproduce the same run from the same inputs on any
// machine forever, and libc's rand() doesn't promise that across platforms. This one's
// only ever called from init(), never from tick(), so the terrain is fixed the moment
// the game starts and never moves again — the run is deterministic even though the
// ground looks hand-drawn.
static uint32_t g_rng;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return (g_rng >> 16) & 0x7fff; }

static void gen_world(void) {
    g_rng = 0xC0FFEEu;           // fixed seed — the same jagged moon, every single time
    int32_t h = 220;
    for (int i = 0; i < NSEG; i++) {
        h += (int32_t)(rnd() % 31) - 15;
        if (h < 160) h = 160;
        if (h > 320) h = 320;
        g_terrain[i] = (int16_t)h;
    }
    for (int i = 0; i < NSEGM; i++) g_padseg[i] = 0;
    int16_t py0 = g_terrain[PAD0_LO];
    for (int i = PAD0_LO; i <= PAD0_HI + 1; i++) g_terrain[i] = py0;
    for (int i = PAD0_LO; i <= PAD0_HI; i++) g_padseg[i] = 1;
    int16_t py1 = g_terrain[PAD1_LO];
    for (int i = PAD1_LO; i <= PAD1_HI + 1; i++) g_terrain[i] = py1;
    for (int i = PAD1_LO; i <= PAD1_HI; i++) g_padseg[i] = 2;
    for (int i = 0; i < NSTAR; i++) {
        g_star[i][0] = (int16_t)(rnd() % VW);
        g_star[i][1] = (int16_t)(rnd() % (VH * 55 / 100));   // sky only, above the ground band
    }
}

static int32_t terrain_h_at(int xpix) {
    if (xpix < 0) xpix = 0; if (xpix > VW - 1) xpix = VW - 1;
    int seg = xpix / SEG_W; if (seg > NSEGM - 1) seg = NSEGM - 1;
    int32_t x0 = seg * SEG_W, y0 = g_terrain[seg], y1 = g_terrain[seg + 1];
    return y0 + (int32_t)((int64_t)(y1 - y0) * (xpix - x0) / SEG_W);
}
static int pad_at(int xpix) {
    if (xpix < 0) xpix = 0; if (xpix > VW - 1) xpix = VW - 1;
    int seg = xpix / SEG_W; if (seg > NSEGM - 1) seg = NSEGM - 1;
    return g_padseg[seg];
}

// ---- the ship -------------------------------------------------------------
static int32_t g_px, g_py, g_vx, g_vy;
static int     g_ang;                // 0..1023, 0 = nose straight up
static int32_t g_fuel;
static int32_t g_score, g_best;
static uint32_t g_frame;
static uint64_t g_checksum;
enum { S_PLAY, S_LANDED, S_CRASHED };
static int g_state;
static int g_thrust_on;
static uint8_t g_events;
#define EV_THRUST 1
#define EV_LAND   2
#define EV_CRASH  4

static void init(void) {
    tables_init();
    gen_world();
    g_px = F(VW / 2); g_py = F(30);
    g_vx = 0; g_vy = 0;
    g_ang = 0;
    g_fuel = FUEL_START;
    g_score = 0;
    g_state = S_PLAY;
    g_frame = 0;
    g_checksum = 0;
    g_thrust_on = 0;
    g_events = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF05060F;   // space
    g_pal[1] = 0xFFE8ECF4;   // hull / hud text
    g_pal[2] = 0xFF3ED598;   // easy pad
    g_pal[3] = 0xFFFFB020;   // flame
    g_pal[4] = 0xFF57534E;   // bare rock
    g_pal[5] = 0xFFFFE9A8;   // pad top stripe / a safe landing's hull
    g_pal[6] = 0xFF14152A;   // end-screen panel
    g_pal[7] = 0xFFEF4B4B;   // crash red
    g_pal[8] = 0xFFB05CFF;   // hard pad
    g_pal[9] = 0xFF4A4E68;   // stars
}

static void tick(const Input in[2]) {
    // Single seat: WASD and the arrows both fly the ship, so nobody has to remember
    // which pad they landed on. The primary (and only) verb is Space — thrust — never
    // the act key, because a lander has one button and it isn't a secondary one.
    Input p;
    p.x = in[0].x ? in[0].x : in[1].x;
    p.y = in[0].y ? in[0].y : in[1].y;
    p.jump = (uint8_t)(in[0].jump | in[1].jump);
    p.act  = (uint8_t)(in[0].act  | in[1].act);

    g_frame++;
    g_thrust_on = 0;
    g_events = 0;

    if (g_state != S_PLAY) {
        // Frozen on the verdict. Space flies a whole new moon, not a reset of this one —
        // best score rides through, same as any other run.
        if (p.jump) { int32_t best = g_best; init(); g_best = best; }
        return;
    }

    g_ang = (g_ang + p.x * ROT_STEP) & 1023;

    if (p.jump && g_fuel > 0) {
        // Thrust fires along the nose. g_sin[ang] is the table's x-component and
        // g_sin[ang+256] (a quarter turn on) is its y — the same trick g3d uses to
        // rotate a point, just spent on one vector instead of a whole mesh.
        int32_t s = g_sin[g_ang & 1023];
        int32_t c = g_sin[(g_ang + 256) & 1023];
        g_vx += (int32_t)(((int64_t)THRUST * s) >> 15);
        g_vy -= (int32_t)(((int64_t)THRUST * c) >> 15);
        g_fuel -= FUEL_BURN;
        if (g_fuel < 0) g_fuel = 0;
        g_thrust_on = 1;
        g_events |= EV_THRUST;
    }
    g_vy += GRAVITY;

    g_px += g_vx;
    g_py += g_vy;

    if (g_px < F(SHIP_R))          { g_px = F(SHIP_R);          if (g_vx < 0) g_vx = 0; }
    if (g_px > F(VW - SHIP_R))     { g_px = F(VW - SHIP_R);     if (g_vx > 0) g_vx = 0; }
    if (g_py < 0)                  { g_py = 0;                  if (g_vy < 0) g_vy = 0; }

    int shipx = PXI(g_px), shipy = PXI(g_py);
    int32_t gy = terrain_h_at(shipx);
    if (shipy + SHIP_R >= gy) {
        int padid = pad_at(shipx);
        int32_t avy = g_vy < 0 ? -g_vy : g_vy;
        int32_t avx = g_vx < 0 ? -g_vx : g_vx;
        int adiff = g_ang; if (adiff > 512) adiff -= 1024; if (adiff < 0) adiff = -adiff;
        int safe = padid && avy <= SAFE_VY && avx <= SAFE_VX && adiff <= ANGLE_TOL;

        g_py = F(gy - SHIP_R);
        g_vx = 0; g_vy = 0;

        if (safe) {
            // Fuel saved is the score — the hard pad pays 3x for the same restraint,
            // so going for it on purpose is a real choice, not just a longer fall.
            int mult = (padid == 2) ? 3 : 1;
            g_score = (200 + g_fuel) * mult;
            if (g_score > g_best) g_best = g_score;
            g_state = S_LANDED;
            g_events |= EV_LAND;
        } else {
            g_score = 0;
            g_state = S_CRASHED;
            g_events |= EV_CRASH;
        }
    }

    g_checksum = g_checksum * 31 + (uint32_t)g_px + (uint32_t)g_py
               + (uint32_t)g_ang + (uint32_t)g_fuel;
}

static void audio(void) {
    // One sfx channel, so a thrust hum is re-struck on a slow beat rather than every
    // frame — retriggering an ADSR sixty times a second is just noise, not a sound.
    if ((g_events & EV_THRUST) && (g_frame % 6 == 0)) synth_note(NCHAN - 1, 6, 45, 90);
    if (g_events & EV_LAND)  synth_note(NCHAN - 1, 1, 72, 200);
    if (g_events & EV_CRASH) { synth_note(NCHAN - 1, 3, 40, 220); synth_note(NCHAN - 1, 4, 36, 200); }
}

// ---- drawing ----------------------------------------------------------------
// The sim thinks in VWxVH virtual pixels; the framebuffer may be any size at all, so
// every shape gets scaled at the last moment, the engine's usual way.
static void quad4(int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, uint8_t ci) {
    int16_t p[8] = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1,
                      (int16_t)x2, (int16_t)y2, (int16_t)x3, (int16_t)y3 };
    for (int i = 0; i < 8; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i] * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1] * g_fbh / VH);
    }
    poly_fill(p, 4, ci);
}
static void rectq(int x, int y, int w, int h, uint8_t ci) { quad4(x, y, x + w, y, x + w, y + h, x, y + h, ci); }

// A small polygon, rotated about the ship's own centre and dropped at (cx,cy) — used
// for both the hull and the flame, so a rotated ship and a rotated flame can never
// drift apart into two different angles.
static void ship_poly(int32_t cx, int32_t cy, int ang, const int16_t *loc, int n, uint8_t ci) {
    int32_t s = g_sin[ang & 1023];
    int32_t c = g_sin[(ang + 256) & 1023];
    int16_t p[20];
    for (int i = 0; i < n; i++) {
        int32_t lx = loc[2 * i], ly = loc[2 * i + 1];
        int32_t rx = (int32_t)(((int64_t)lx * c - (int64_t)ly * s) >> 15);
        int32_t ry = (int32_t)(((int64_t)lx * s + (int64_t)ly * c) >> 15);
        int32_t wx = cx + rx, wy = cy + ry;
        p[2 * i]     = (int16_t)((int32_t)wx * g_fbw / VW);
        p[2 * i + 1] = (int16_t)((int32_t)wy * g_fbh / VH);
    }
    poly_fill(p, n, ci);
}

static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) { char b[12]; digits(v, b); text_draw(x, y, sc, b, ci); }

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    // The title bar covers the top ~8% of the window — everything the player needs to
    // read lives below it, never in the band a real window chrome can eat.
    int y = g_fbh * 9 / 100;
    text_draw(6 * sc, y, sc, "FUEL", 1);
    num(6 * sc + 30 * sc, y, sc, g_fuel, 1);
    text_draw(6 * sc, y + 12 * sc, sc, "BEST", 1);
    num(6 * sc + 30 * sc, y + 12 * sc, sc, g_best, 1);

    if (g_state == S_PLAY) return;

    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int yy = cy - 30 * sc; yy < cy + 34 * sc; yy++)
        for (int xx = 0; xx < g_fbw; xx++)
            if (yy >= 0 && yy < g_fbh) g_fb[yy * g_fbw + xx] = 6;

    const char *title = (g_state == S_LANDED) ? "LANDED" : "CRASHED";
    uint8_t tc = (uint8_t)((g_state == S_LANDED) ? 5 : 7);
    text_draw(cx - text_width(title, sc * 3) / 2, cy - 22 * sc, sc * 3, title, tc);

    char b[12]; digits(g_score, b);
    num(cx - text_width(b, sc * 2) / 2, cy + 3 * sc, sc * 2, g_score, 1);

    text_draw(cx - text_width("SPACE TO FLY AGAIN", sc) / 2, cy + 22 * sc, sc, "SPACE TO FLY AGAIN", 1);
}

static void draw(void) {
    fb_clear(0);
    for (int i = 0; i < NSTAR; i++) rectq(g_star[i][0], g_star[i][1], 2, 2, 9);

    for (int i = 0; i < NSEGM; i++) {
        int x0 = i * SEG_W, x1 = x0 + SEG_W;
        int y0 = g_terrain[i], y1 = g_terrain[i + 1];
        uint8_t fillc = (uint8_t)((g_padseg[i] == 2) ? 8 : (g_padseg[i] == 1 ? 2 : 4));
        quad4(x0, y0, x1, y1, x1, VH, x0, VH, fillc);
        if (g_padseg[i]) rectq(x0, y0 - 2, SEG_W, 2, 5);   // a lit strip: this is the pad
    }

    int cxp = PXI(g_px), cyp = PXI(g_py);
    if (g_state != S_CRASHED) {
        static const int16_t HULL[10] = { 0, -9,  6, -2,  7, 7,  -7, 7,  -6, -2 };
        ship_poly(cxp, cyp, g_ang, HULL, 5, (uint8_t)(g_state == S_LANDED ? 5 : 1));
        if (g_thrust_on) {
            // Flicker on the frame parity, not a random draw — deterministic, and it
            // still reads as fire because two lengths alternating is all a flame needs.
            int len = (g_frame & 2) ? 10 : 6;
            int16_t flame[6] = { 0, (int16_t)(7 + len),  4, 7,  -4, 7 };
            ship_poly(cxp, cyp, g_ang, flame, 3, 3);
        }
    } else {
        rectq(cxp - 7, cyp - 1, 14, 2, 7);
        rectq(cxp - 1, cyp - 7, 2, 14, 7);
    }

    hud();
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_lander = { "lander", init, tick, audio, draw, checksum };
