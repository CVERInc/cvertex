// aerostrike.c — AEROSTRIKE. A fixed oblique camera, a fortress that scrolls past beneath
// you, and one control that matters more than the rest: altitude.
//
// This is Zaxxon's whole trick, done with a real depth-tested 3D camera instead of a
// pre-rendered isometric background. The camera never turns — it sits behind and above
// the ship at a constant ~45 degree tilt, and only ever TRANSLATES, chasing the ship's
// forward progress. Because it's a real perspective camera and not a painted isometric,
// altitude reads for free: climb, and the ship rises on screen away from its own shadow;
// the vertical gap between ship and shadow IS the altitude gauge, the same way it was in
// 1982, except here it's actual triangles at actual heights instead of a sprite offset
// table.
//
// The fortress is a strip of numbered slots receding into the distance (the same trick
// tube.c and canyon.c use for their track): each slot's contents are a pure hash of its
// own index, so a wall a hundred slots out already knows it's a wall before the ship gets
// anywhere near it, and a replay always meets the same fortress.
//
// Two ways to die: fly too LOW under a WALL (go high, over it) or too HIGH into a GATE's
// underside (go low, under it). Ground turrets only take a hit while you're diving low
// over them — your guns fire dead level, not down — so hitting one costs the very
// altitude that keeps you safe from a wall a moment later. That trade is the whole game.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
#define FX(v) ((int32_t)((int64_t)(v) * 65536 / 100))   // v in hundredths of a world unit

static int32_t mulq15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }

// ---- deterministic hash (lowbias32) --------------------------------------------------
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ---- tunables -------------------------------------------------------------------------
#define CORR_HW      FX(220)     // corridor half-width — the fortress you fly over
#define SHIP_HALF_X  FX(16)
#define ALT_MIN      0
#define ALT_MAX      FX(300)
#define SHIP_HALF_ALT FX(10)
#define WALL_TOP     FX(175)     // fly ABOVE this + ship's own half-height to clear a wall
#define GATE_BOTTOM  FX(115)     // fly BELOW this - ship's own half-height to clear a gate
#define ALT_AIR      FX(180)     // where the air targets cruise
#define SLOT_LEN     FX(300)     // world units between fortress slots
#define OBST_START   3           // a clear runway before the fortress opens fire
#define RINGS_AHEAD  16

#define SPEED_MIN    FX(6)
#define SPEED_MAX    FX(11)
#define RAMP_DIST    FX(6000)
#define ACC_X        FX(2)
#define ACC_ALT      FX(2)
#define DRAG_SHIFT   4

#define PROJ_SPEED   FX(30)
#define PROJ_RANGE   FX(1400)
#define FIRE_CD      10
#define INV_TICKS    50
#define MAXPROJ      6
#define MAXFX        6
#define FX_MAXAGE    16

// 🔴 Tuned so the FULL altitude band (0..ALT_MAX) stays on screen. A steep tilt
// this close to the ship couples height and depth so tightly that a camera merely
// "above the ship" (CAM_Y < ALT_MAX) sends the ship off the TOP of the frame the
// moment it climbs past camera height — invisible exactly when it matters most,
// clearing a wall. CAM_Y sits above ALT_MAX so climbing always reads as "rises
// toward the top of frame", never "vanishes".
#define CAMDIST      FX(500)     // fixed distance behind the ship
#define CAM_Y        FX(450)     // fixed height — NOT tied to ship altitude, or climbing
                                  // would never move on screen
#define CAM_AX       91          // fixed tilt, ~32 degrees — the camera never rotates
#define CAM_FOLLOW_X 6           // /10 — how much the lens drifts sideways with the ship

// ---- palette ----------------------------------------------------------------------
#define P_GNDA   8    //  8..15  fortress floor, band A
#define P_GNDB   16   // 16..23  fortress floor, band B
#define P_PYLON  24   // 24..31  edge towers — decorative, but they sell the corridor
#define P_WALL   32   // 32..39  a wall: go HIGH
#define P_GATE   40   // 40..47  a gate: go LOW
#define P_GTGT   48   // 48..55  ground turret — shootable, not solid
#define P_ATGT   56   // 56..63  air target — shootable AND solid
#define P_HULL   64   // 64..71  ship hull
#define P_DARK   72   // 72..79  ship cockpit / fin
#define P_ENGINE 80   // 80..87  engine glow
#define P_SHADOW 88   // 88..95  the ground shadow — the whole altitude readout
#define P_SPARK  96   // 96..103 explosions
#define P_PROJ   104  // 104..111 cannon bolt

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 55 + i * 28;
        int r = (int)((rgb >> 16) & 255) * m / 255, g = (int)((rgb >> 8) & 255) * m / 255, b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ---- box meshes at runtime, one arena, rebuilt every draw() -------------------------
#define MAXBOX 300
static V3   bv[MAXBOX][8];
static Tri  bt[MAXBOX][12];
static Mesh bm[MAXBOX];
static int  nbox;
static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                 {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static int box(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return -1;
    if (sx < 1) sx = 1; if (sy < 1) sy = 1; if (sz < 1) sz = 1;
    int i = nbox++;
    for (int v = 0; v < 8; v++)
        bv[i][v] = (V3){ VP[v][0] * sx, VP[v][1] * sy, VP[v][2] * sz };
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &bt[i][f * 2 + k];
            t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k];
            t->ci = ci;
            t->nx = (int16_t)(FN[f][0] * 32767); t->ny = (int16_t)(FN[f][1] * 32767); t->nz = (int16_t)(FN[f][2] * 32767);
        }
    bm[i] = (Mesh){ bv[i], 8, bt[i], 12 };
    return i;
}

// ---- the fortress: a formula, not a table ------------------------------------------
// Every slot's contents are a pure function of its own index, so the ship, a projectile
// and the renderer can each ask "what's at slot 40" independently and never disagree.
enum { T_EMPTY, T_WALL, T_GATE, T_GROUND, T_AIR };

static int slot_type(int idx) {
    if (idx < OBST_START) return T_EMPTY;
    uint32_t h = mix32((uint32_t)idx * 0x9E3779B1u + 12345u);
    switch (h % 7) {
        case 0: return T_WALL;
        case 1: return T_GATE;
        case 2: return T_GROUND;
        case 3: return T_AIR;
        default: return T_EMPTY;
    }
}
// A slot's lateral offset for a ground/air target — its own hash, spread across the
// corridor but kept off the edge pylons.
static int32_t slot_x(int idx, int32_t range) {
    uint32_t h = mix32((uint32_t)idx * 2246822519u + 3266489917u);
    int32_t t = (int32_t)(h % 2001) - 1000;                     // -1000..1000
    return (int32_t)(((int64_t)range * t) / 1000);
}
static int32_t slot_z(int idx) { return (int32_t)((int64_t)idx * SLOT_LEN); }
static void target_pos(int idx, int type, int32_t *tx, int32_t *ty) {
    if (type == T_GROUND) { *tx = slot_x(idx, CORR_HW * 55 / 100); *ty = FX(45); }
    else                  { *tx = slot_x(idx, CORR_HW * 70 / 100); *ty = ALT_AIR; }
}

// A shootable target's death is state that outlives its own slot's brief time on screen,
// but not FOREVER — a ring buffer keyed on idx&mask means a far-future slot that happens
// to land on the same cell just finds it doesn't own the record and reads as alive again.
#define RINGN 128
static int32_t g_ring_owner[RINGN];
static uint8_t g_ring_dead[RINGN];
static int target_dead(int idx) { int r = idx & (RINGN - 1); return g_ring_owner[r] == idx && g_ring_dead[r]; }
static void  target_kill(int idx) { int r = idx & (RINGN - 1); g_ring_owner[r] = idx; g_ring_dead[r] = 1; }

static int32_t speed_for(int32_t dist) {
    int32_t d = dist; if (d < 0) d = 0; if (d > RAMP_DIST) d = RAMP_DIST;
    int32_t t = (int32_t)(((int64_t)d << 16) / RAMP_DIST);
    return SPEED_MIN + (int32_t)(((int64_t)(SPEED_MAX - SPEED_MIN) * t) >> 16);
}

// ---- HUD text helpers ---------------------------------------------------------------
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) { char b[12]; digits(v, b); text_draw(x, y, sc, b, ci); }

// ---- state (everything tick may touch) -----------------------------------------------
static uint32_t g_frame;
static int32_t g_dist, g_x, g_vx, g_alt, g_valt;
static int g_lives, g_over, g_inv, g_fire_cd;
static int32_t g_score, g_best;
static uint32_t g_events;
#define EV_FIRE 1
#define EV_HIT  2
#define EV_KILL 4
#define EV_OVER 8

static uint8_t  g_proj_on[MAXPROJ];
static int32_t  g_proj_x[MAXPROJ], g_proj_y[MAXPROJ], g_proj_z[MAXPROJ];

static uint8_t  g_fx_on[MAXFX];
static uint32_t g_fx_t0[MAXFX];
static int32_t  g_fx_x[MAXFX], g_fx_y[MAXFX], g_fx_z[MAXFX];
static uint8_t  g_fx_ci[MAXFX];

static void spawn_fx(int32_t x, int32_t y, int32_t z, uint8_t ci) {
    for (int k = 0; k < MAXFX; k++)
        if (!g_fx_on[k] || g_frame - g_fx_t0[k] > FX_MAXAGE) {
            g_fx_on[k] = 1; g_fx_t0[k] = g_frame;
            g_fx_x[k] = x; g_fx_y[k] = y; g_fx_z[k] = z; g_fx_ci[k] = ci;
            return;
        }
}

static void init(void) {
    tables_init();
    g_frame = 0;
    g_dist = 0; g_x = 0; g_vx = 0; g_alt = FX(140); g_valt = 0;
    g_lives = 3; g_over = 0; g_inv = 0; g_fire_cd = 0;
    g_score = 0; g_events = 0;
    for (int k = 0; k < MAXPROJ; k++) g_proj_on[k] = 0;
    for (int k = 0; k < MAXFX; k++) g_fx_on[k] = 0;
    for (int r = 0; r < RINGN; r++) { g_ring_owner[r] = -1; g_ring_dead[r] = 0; }

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0C0E1A;   // sky
    g_pal[1] = 0xFF8A90B8;   // HUD label
    g_pal[2] = 0xFFF2F4FA;   // HUD number
    g_pal[3] = 0xFF15171F;   // banner band
    ramp(P_GNDA,   0xFF3A4058);
    ramp(P_GNDB,   0xFF454C6C);
    ramp(P_PYLON,  0xFF2E3348);
    ramp(P_WALL,   0xFFE6432E);   // danger red — go HIGH
    ramp(P_GATE,   0xFF33A6E6);   // danger blue — go LOW
    ramp(P_GTGT,   0xFFD6A93C);   // ground turret — olive gold
    ramp(P_ATGT,   0xFFC23CE6);   // air target — magenta
    ramp(P_HULL,   0xFFE4E9F6);
    ramp(P_DARK,   0xFF2A2C3E);
    ramp(P_ENGINE, 0xFFFFC23C);
    ramp(P_SHADOW, 0xFF04050A);
    ramp(P_SPARK,  0xFFFFFFFF);
    ramp(P_PROJ,   0xFF44FFB0);
}

static void hit_player(void) {
    if (g_inv > 0 || g_over) return;
    g_lives--; g_inv = INV_TICKS;
    g_events |= EV_HIT;
    spawn_fx(g_x, g_alt, g_dist, P_SPARK);
    if (g_lives <= 0) { g_over = 1; g_events |= EV_OVER; if (g_score > g_best) g_best = g_score; }
}

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0;
    Input p = input_1p(in);

    if (g_over) {
        if (p.jump) { int32_t b = g_best; init(); g_best = b; }
        return;
    }

    // ---- steering: lateral and altitude, the only two axes the pilot owns -----------
    g_vx += p.x * ACC_X; g_vx -= g_vx >> DRAG_SHIFT;
    g_x += g_vx;
    int32_t xmax = CORR_HW - SHIP_HALF_X;
    if (g_x > xmax)  { g_x = xmax;  if (g_vx > 0) g_vx = 0; }
    if (g_x < -xmax) { g_x = -xmax; if (g_vx < 0) g_vx = 0; }

    g_valt += p.y * ACC_ALT; g_valt -= g_valt >> DRAG_SHIFT;
    g_alt += g_valt;
    if (g_alt > ALT_MAX) { g_alt = ALT_MAX; if (g_valt > 0) g_valt = 0; }
    if (g_alt < ALT_MIN) { g_alt = ALT_MIN; if (g_valt < 0) g_valt = 0; }

    // ---- forward: automatic, and the fortress is what moves relative to it ----------
    int32_t prevDist = g_dist;
    g_dist += speed_for(g_dist);

    // ---- crossing test: any slot the ship passed through this tick ------------------
    int prevIdx = (int)(prevDist / SLOT_LEN), newIdx = (int)(g_dist / SLOT_LEN);
    for (int idx = prevIdx + 1; idx <= newIdx; idx++) {
        int t = slot_type(idx);
        if (t == T_WALL) {
            if (g_alt < WALL_TOP + SHIP_HALF_ALT) hit_player();
        } else if (t == T_GATE) {
            if (g_alt > GATE_BOTTOM - SHIP_HALF_ALT) hit_player();
        } else if (t == T_AIR && !target_dead(idx)) {
            int32_t tx, ty; target_pos(idx, t, &tx, &ty);
            if ((g_x - tx < FX(35) && tx - g_x < FX(35)) && (g_alt - ty < FX(35) && ty - g_alt < FX(35))) {
                target_kill(idx);
                spawn_fx(tx, ty, slot_z(idx), P_SPARK);
                hit_player();
            }
        }
    }

    // ---- firing --------------------------------------------------------------------
    if (g_fire_cd > 0) g_fire_cd--;
    if (p.jump && g_fire_cd <= 0) {
        for (int k = 0; k < MAXPROJ; k++)
            if (!g_proj_on[k]) {
                g_proj_on[k] = 1; g_proj_x[k] = g_x; g_proj_y[k] = g_alt; g_proj_z[k] = g_dist;
                g_fire_cd = FIRE_CD; g_events |= EV_FIRE;
                break;
            }
    }

    // ---- projectiles: advance, and test the slots they just crossed -----------------
    for (int k = 0; k < MAXPROJ; k++) {
        if (!g_proj_on[k]) continue;
        int32_t pz0 = g_proj_z[k];
        g_proj_z[k] += PROJ_SPEED;
        if (g_proj_z[k] - g_dist > PROJ_RANGE) { g_proj_on[k] = 0; continue; }
        int pi0 = (int)(pz0 / SLOT_LEN), pi1 = (int)(g_proj_z[k] / SLOT_LEN);
        for (int idx = pi0; idx <= pi1 && g_proj_on[k]; idx++) {
            int t = slot_type(idx);
            if ((t != T_GROUND && t != T_AIR) || target_dead(idx)) continue;
            int32_t tx, ty; target_pos(idx, t, &tx, &ty);
            int32_t tz = slot_z(idx);
            if (g_proj_z[k] - tz > FX(60) || tz - g_proj_z[k] > FX(60)) continue;
            if (g_proj_x[k] - tx > FX(45) || tx - g_proj_x[k] > FX(45)) continue;
            if (g_proj_y[k] - ty > FX(45) || ty - g_proj_y[k] > FX(45)) continue;
            target_kill(idx);
            g_score += (t == T_GROUND) ? 50 : 80;
            spawn_fx(tx, ty, tz, P_SPARK);
            g_events |= EV_KILL;
            g_proj_on[k] = 0;
        }
    }

    if (g_inv > 0) g_inv--;
}

static void audio(void) {
    if (g_events & EV_OVER)      synth_note(NCHAN - 1, 3, 36, 220);
    else if (g_events & EV_HIT)  synth_note(NCHAN - 1, 3, 48, 200);
    else if (g_events & EV_KILL) synth_note(NCHAN - 1, 4, 72, 150);
    else if (g_events & EV_FIRE) synth_note(NCHAN - 1, 5, 88, 90);
}

// ---- the ship: a fuselage, delta wings, a fin, a cockpit and an engine glow — six
// boxes, banked and pitched by hand about the ship's own centre so a strafe is a real
// roll, not six crates sliding sideways in formation.
typedef struct { int16_t x, y, z, sx, sy, sz; uint8_t mat; } Part;
#define NPART 6
// Sized against the corridor (CORR_HW = 2.20 units): a 0.46-unit-long fighter is about a
// tenth of the fortress width, big enough to read at any resolution the game runs at.
static const Part SHIP[NPART] = {
    {   0,   0,   0, 110,  45, 230,  P_HULL   },   // fuselage
    { -190, -15, -40, 160,  22, 130,  P_HULL   },   // left delta wing
    {  190, -15, -40, 160,  22, 130,  P_HULL   },   // right delta wing
    {   0,  60,-140,  24,  60,  50,  P_DARK   },   // tail fin
    {   0,  35,  90,  48,  38,  60,  P_DARK   },   // cockpit
    {   0,  -8,-210,  42,  42,  38,  P_ENGINE },   // engine glow, faces the camera
};

static int emit_ship(Inst *inst, int n, int32_t wx, int32_t wy, int32_t wz, int roll, int pitch) {
    for (int p = 0; p < NPART && n < MAXBOX - 1; p++) {
        const Part *pt = &SHIP[p];
        int32_t sx = (int32_t)pt->sx * U / 1000, sy = (int32_t)pt->sy * U / 1000, sz = (int32_t)pt->sz * U / 1000;
        int bi = box(sx, sy, sz, pt->mat);
        if (bi < 0) break;
        inst[n].m = &bm[bi];
        inst[n].pos = (V3){ wx + (int32_t)pt->x * U / 1000, wy + (int32_t)pt->y * U / 1000, wz + (int32_t)pt->z * U / 1000 };
        inst[n].ax = pitch & 1023; inst[n].ay = 0; inst[n].az = roll & 1023;
        inst[n].scale = U;
        n++;
    }
    return n;
}

static int emit_burst(Inst *inst, int n, int32_t wx, int32_t wy, int32_t wz, uint32_t age, uint8_t ci) {
    if (age > FX_MAXAGE) return n;
    int32_t a16 = (int32_t)(age * U / FX_MAXAGE); if (a16 > U) a16 = U;
    if (age < 6) {
        int32_t fl = FX(24) - (int32_t)(((int64_t)FX(20) * age) / 6); if (fl < FX(2)) fl = FX(2);
        int bi = box(fl, fl, fl, ci);
        if (bi >= 0) { inst[n].m = &bm[bi]; inst[n].pos = (V3){ wx, wy, wz }; inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++; }
    }
    for (int s = 0; s < 6 && n < MAXBOX - 1; s++) {
        uint32_t h = mix32((uint32_t)s * 977u + 13u + age * 31u);
        int ang = (int)(h & 1023), updown = (int)((h >> 10) & 1023);
        int32_t rad = (int32_t)(((int64_t)FX(55) * a16) >> 16);
        int32_t sxp = wx + mulq15(rad, g_sin[ang]);
        int32_t szp = wz + mulq15(rad, g_sin[(ang + 256) & 1023]);
        int32_t syp = wy + mulq15(rad, g_sin[updown]) / 2;
        int bi = box(FX(4), FX(4), FX(4), ci);
        if (bi < 0) break;
        inst[n].m = &bm[bi]; inst[n].pos = (V3){ sxp, syp, szp };
        inst[n].ax = (int)(h & 1023); inst[n].ay = (int)((h >> 5) & 1023); inst[n].az = 0;
        inst[n].scale = U;
        n++;
    }
    return n;
}

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "SCORE", 1);
    num(6 * sc + 34 * sc, hud_top(), sc, g_score, 2);
    text_draw(6 * sc, hud_top() + 10 * sc, sc, "LIVES", 1);
    num(6 * sc + 34 * sc, hud_top() + 10 * sc, sc, g_lives, g_lives > 1 ? (uint8_t)2 : (uint8_t)5);
    text_draw(g_fbw - 60 * sc, hud_top(), sc, "ALT", 1);
    num(g_fbw - 60 * sc + 22 * sc, hud_top(), sc, g_alt >> 16, 2);
    if (g_best) { text_draw(g_fbw - 60 * sc, hud_top() + 10 * sc, sc, "BEST", 1); num(g_fbw - 60 * sc + 28 * sc, hud_top() + 10 * sc, sc, g_best, 1); }

    if (!g_over) return;
    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int y = cy - 30 * sc; y < cy + 34 * sc; y++)
        for (int x = 0; x < g_fbw; x++) if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = 3;
    text_draw(cx - text_width("SHOT DOWN", sc * 3) / 2, cy - 22 * sc, sc * 3, "SHOT DOWN", 2);
    text_draw(cx - text_width("SCORE", sc * 2) / 2 - 26 * sc, cy + 3 * sc, sc * 2, "SCORE", 1);
    num(cx + 22 * sc, cy + 3 * sc, sc * 2, g_score, 2);
    text_draw(cx - text_width("SPACE TO FLY AGAIN", sc) / 2, cy + 22 * sc, sc, "SPACE TO FLY AGAIN", 1);
}

static void draw(void) {
    fb_clear(0);
    nbox = 0;
    static Inst inst[MAXBOX];
    int n = 0;

    int i0 = (int)(g_dist / SLOT_LEN);
    int istart = i0 - 1; if (istart < 0) istart = 0;

    // ---- the fortress floor, banded, and its edge towers -----------------------------
    for (int i = istart; i < i0 + RINGS_AHEAD; i++) {
        int32_t relz = slot_z(i) - g_dist;
        int bi = box(CORR_HW, FX(8), SLOT_LEN / 2, (i & 1) ? P_GNDB : P_GNDA);
        if (bi >= 0) {
            inst[n].m = &bm[bi]; inst[n].pos = (V3){ 0, -FX(8), relz };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
        }
        if ((i & 1) == 0 && n < MAXBOX - 3) {
            int32_t ph = ALT_MAX + FX(60);
            for (int side = -1; side <= 1; side += 2) {
                int pi = box(FX(12), ph / 2, FX(30), P_PYLON);
                if (pi < 0) break;
                inst[n].m = &bm[pi];
                inst[n].pos = (V3){ side * (CORR_HW + FX(20)), ph / 2, relz };
                inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
            }
        }
    }

    // ---- the fortress hardware: walls, gates, turrets, air targets ------------------
    for (int i = istart; i < i0 + RINGS_AHEAD && n < MAXBOX - 4; i++) {
        int32_t relz = slot_z(i) - g_dist;
        int t = slot_type(i);
        if (t == T_WALL) {
            int bi = box(CORR_HW + FX(15), WALL_TOP / 2, SLOT_LEN * 35 / 100, P_WALL);
            if (bi >= 0) { inst[n].m = &bm[bi]; inst[n].pos = (V3){ 0, WALL_TOP / 2, relz };
                inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++; }
        } else if (t == T_GATE) {
            int32_t ceil = ALT_MAX + FX(60);
            int32_t hh = (ceil - GATE_BOTTOM) / 2;
            int bi = box(CORR_HW + FX(15), hh, SLOT_LEN * 35 / 100, P_GATE);
            if (bi >= 0) { inst[n].m = &bm[bi]; inst[n].pos = (V3){ 0, GATE_BOTTOM + hh, relz };
                inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++; }
        } else if (t == T_GROUND && !target_dead(i)) {
            int32_t tx, ty; target_pos(i, t, &tx, &ty);
            int bi = box(FX(35), ty, FX(35), P_GTGT);
            if (bi >= 0) { inst[n].m = &bm[bi]; inst[n].pos = (V3){ tx, ty, relz };
                inst[n].ax = inst[n].ay = ((int)g_frame * 2) & 1023; inst[n].az = 0; inst[n].scale = U; n++; }
        } else if (t == T_AIR && !target_dead(i)) {
            int32_t tx, ty; target_pos(i, t, &tx, &ty);
            int bi = box(FX(26), FX(26), FX(26), P_ATGT);
            if (bi >= 0) { inst[n].m = &bm[bi]; inst[n].pos = (V3){ tx, ty, relz };
                inst[n].ax = 0; inst[n].ay = (128 + (int)g_frame * 4) & 1023; inst[n].az = 0; inst[n].scale = U; n++; }
        }
    }

    // ---- projectiles ------------------------------------------------------------------
    for (int k = 0; k < MAXPROJ && n < MAXBOX - 1; k++) {
        if (!g_proj_on[k]) continue;
        int bi = box(FX(4), FX(4), FX(10), P_PROJ);
        if (bi < 0) break;
        inst[n].m = &bm[bi]; inst[n].pos = (V3){ g_proj_x[k], g_proj_y[k], g_proj_z[k] - g_dist };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
    }

    // ---- bursts -------------------------------------------------------------------
    for (int k = 0; k < MAXFX; k++) {
        if (!g_fx_on[k]) continue;
        uint32_t age = g_frame - g_fx_t0[k];
        if (age > FX_MAXAGE) { g_fx_on[k] = 0; continue; }
        n = emit_burst(inst, n, g_fx_x[k], g_fx_y[k], g_fx_z[k] - g_dist, age, g_fx_ci[k]);
    }

    // ---- the shadow — the whole altitude readout, and the ship itself ----------------
    if (!g_over || (g_frame & 4)) {
        int si = box(FX(24), FX(1), FX(34), P_SHADOW);
        if (si >= 0 && n < MAXBOX - 1) {
            inst[n].m = &bm[si]; inst[n].pos = (V3){ g_x, FX(2), 0 };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
        }
    }
    if (!g_over || (g_frame & 4)) {
        int roll  = (int)((-(int64_t)g_vx * 1600) >> 16); if (roll > 240) roll = 240; if (roll < -240) roll = -240;
        int pitch = (int)(((int64_t)g_valt * 1600) >> 16); if (pitch > 200) pitch = 200; if (pitch < -200) pitch = -200;
        n = emit_ship(inst, n, g_x, g_alt, 0, roll, pitch);
    }

    // ---- the fixed oblique lens: translates with the ship, never rotates -------------
    int32_t camx = (g_x * CAM_FOLLOW_X) / 10;
    Cam cam = { { camx, CAM_Y, -CAMDIST }, CAM_AX, 0, 0 };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    hud();
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ull;
    h = h * 31 + (uint32_t)g_frame; h = h * 31 + (uint32_t)g_dist;
    h = h * 31 + (uint32_t)g_x; h = h * 31 + (uint32_t)g_vx;
    h = h * 31 + (uint32_t)g_alt; h = h * 31 + (uint32_t)g_valt;
    h = h * 31 + (uint32_t)g_lives; h = h * 31 + (uint32_t)g_over;
    h = h * 31 + (uint32_t)g_inv; h = h * 31 + (uint32_t)g_fire_cd;
    h = h * 31 + (uint32_t)g_score; h = h * 31 + (uint32_t)g_best;
    for (int k = 0; k < MAXPROJ; k++) {
        h = h * 31 + g_proj_on[k];
        h = h * 31 + (uint32_t)g_proj_x[k]; h = h * 31 + (uint32_t)g_proj_y[k]; h = h * 31 + (uint32_t)g_proj_z[k];
    }
    for (int r = 0; r < RINGN; r++) { h = h * 31 + (uint32_t)g_ring_owner[r]; h = h * 31 + g_ring_dead[r]; }
    return h;
}

const Game game_aerostrike = { "aerostrike", init, tick, audio, draw, checksum };
