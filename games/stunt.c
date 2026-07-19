// stunt.c — Stunt Track Racer: a winding track with real elevation. Ramps launch you,
// gravity brings you back down, and the camera rides the whole arc with you.
//
// The trick that makes this different from a flat racer: the road surface is a strip of
// quads whose height comes straight out of trackY(d), a function of distance travelled —
// not a flat plane with obstacles glued on top. A ramp isn't a sprite, it's the road
// itself standing up. Catching air isn't a state machine bolted onto driving, it's what
// happens when the ground drops away faster than gravity can follow a rolling car: the
// two rates are compared every tick, and whichever one loses decides whether you're
// still touching the track.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
static int32_t mulsin(int32_t amp, int idx) { return (int32_t)(((int64_t)amp * g_sin[idx & 1023]) >> 15); }

// ---- the track: pure functions of distance travelled -----------------------------
// d is a running total in 16.16 that never wraps back to zero, so every trig lookup goes
// through idxfor() first — modulo a period, THEN into the 0..1023 table. Keeping d itself
// unbounded (int64) rather than wrapping it is what lets a run go on indefinitely without
// the track ever visibly seaming.
#define TRACK_HALF (U * 3)              // the road is 6 units wide

#define P1 (60 * U)                     // a slow, lazy roll — the landscape breathing
#define A1 (U * 13 / 5)                 // +/- 2.6 units
#define P2 (26 * U)                     // the ramp cycle: run up, launch, land, recover
#define RAMP_H (U * 4)                  // 4 units of ramp
#define RISE_END   180                  // 0..1023 within one P2 cycle
#define CREST_END  240
#define FALL_END   420
#define P3 (70 * U)                     // the S-curve that makes steering matter
#define B1 (U * 7 / 2)                  // +/- 3.5 units

static int idxfor(int64_t d, int32_t period) {
    int64_t m = d % (int64_t)period;
    if (m < 0) m += period;
    return (int)((m * 1024) / period);
}

// A ramp is three straight lines, not a curve: a fast rise (the launch), a short flat
// crest (the lip you leave from), a fast fall (the cliff you fly over), then a long flat
// run before the next one. Real stunt-track ramps are polygons, not sine waves — a car
// launches off a CORNER, the point where the slope changes abruptly, and a smooth curve
// never has one.
static int32_t ramp_component(int idx) {
    if (idx < RISE_END)  return (int32_t)(((int64_t)RAMP_H * idx) / RISE_END);
    if (idx < CREST_END) return RAMP_H;
    if (idx < FALL_END)  return (int32_t)(((int64_t)RAMP_H * (FALL_END - idx)) / (FALL_END - CREST_END));
    return 0;
}
static int32_t trackY(int64_t d) { return mulsin(A1, idxfor(d, P1)) + ramp_component(idxfor(d, P2)); }
static int32_t trackX(int64_t d) { return mulsin(B1, idxfor(d, P3)); }

static int angle_from_slope(int32_t dy, int32_t dz) {
    if (dz == 0) return 0;
    int64_t ratio = ((int64_t)dy << 16) / dz;        // 16.16, dy/dz
    int32_t ang = (int32_t)((ratio * 163) >> 16);     // small-angle: 1024/(2*pi) ~= 163
    if (ang > 140) ang = 140; else if (ang < -140) ang = -140;
    return ang;
}

// ---- palette ------------------------------------------------------------------
static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
#define P_ROAD    8
#define P_GROUND  16
#define P_KERB    24
#define P_KERB2   32
#define P_CARBODY 40
#define P_CABIN   48
#define P_WHEEL   56

// ---- shared box geometry (axis-aligned; Inst rotation handles orientation) -------
static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                 {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

// ---- the car: one rigid mesh, built once. A composite of boxes so the whole thing
// pitches, rolls and lands as one body instead of six independently-rotated parts.
#define CARV 64
#define CART 96
static V3   carv[CARV];
static Tri  cart_[CART];
static Mesh carm;
static int  carnv, carnt;

static void car_box(V3 c, int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    int base = carnv;
    for (int v = 0; v < 8; v++)
        carv[carnv++] = (V3){ c.x + VP[v][0]*sx, c.y + VP[v][1]*sy, c.z + VP[v][2]*sz };
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &cart_[carnt++];
            t->a = (uint16_t)(base + FQ[f][0]); t->b = (uint16_t)(base + FQ[f][1+k]); t->c = (uint16_t)(base + FQ[f][2+k]);
            t->ci = ci;
            t->nx = (int16_t)(FN[f][0]*32767); t->ny = (int16_t)(FN[f][1]*32767); t->nz = (int16_t)(FN[f][2]*32767);
        }
}
// Wheel bottoms sit at local y=0 — that's the car's ground-contact plane, so the car's
// world Y can just BE the track height, no ride-height fudge needed at the call site.
static void build_car(void) {
    carnv = carnt = 0;
    car_box((V3){0, U*35/100, 0},            U*45/100, U*18/100, U*80/100, P_CARBODY); // chassis
    car_box((V3){0, U*58/100, U*20/100},     U*33/100, U*14/100, U*35/100, P_CABIN);   // cabin
    car_box((V3){-U*40/100, U*14/100,  U*55/100}, U*10/100, U*14/100, U*16/100, P_WHEEL); // FL
    car_box((V3){ U*40/100, U*14/100,  U*55/100}, U*10/100, U*14/100, U*16/100, P_WHEEL); // FR
    car_box((V3){-U*40/100, U*14/100, -U*55/100}, U*10/100, U*14/100, U*16/100, P_WHEEL); // RL
    car_box((V3){ U*40/100, U*14/100, -U*55/100}, U*10/100, U*14/100, U*16/100, P_WHEEL); // RR
    car_box((V3){0, U*60/100, -U*72/100},    U*38/100, U*6/100,  U*8/100,  P_CABIN);   // spoiler
    carm.v = carv; carm.nv = carnv; carm.t = cart_; carm.nt = carnt;
}

// ---- a scratch pool of boxes, rebuilt fresh every frame: the ground plane and the
// slalom pylons that mark the road's edge. ------------------------------------------
#define MAXBOX 96
static V3   bv[MAXBOX][8];
static Tri  bt[MAXBOX][12];
static Mesh bm[MAXBOX];
static int  nbox;
static int box(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return 0;
    int i = nbox++;
    for (int v = 0; v < 8; v++) bv[i][v] = (V3){ VP[v][0]*sx, VP[v][1]*sy, VP[v][2]*sz };
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &bt[i][f*2+k];
            t->a = FQ[f][0]; t->b = FQ[f][1+k]; t->c = FQ[f][2+k]; t->ci = ci;
            t->nx = (int16_t)(FN[f][0]*32767); t->ny = (int16_t)(FN[f][1]*32767); t->nz = (int16_t)(FN[f][2]*32767);
        }
    bm[i].v = bv[i]; bm[i].nv = 8; bm[i].t = bt[i]; bm[i].nt = 12;
    return i;
}

// ---- the road ribbon: a strip of quads following trackX/trackY, with a REAL computed
// normal per quad. That's not decoration — the shader lights faces by their normal, so a
// hand-typed "up" normal would leave every ramp lit exactly like the flat between them,
// and the elevation the whole game is about would stop reading in the picture.
static uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}
static void quad_normal(const V3 *a, const V3 *b, const V3 *c, int16_t *nx, int16_t *ny, int16_t *nz) {
    int32_t ux = (b->x - a->x) >> 6, uy = (b->y - a->y) >> 6, uz = (b->z - a->z) >> 6;
    int32_t vx = (c->x - a->x) >> 6, vy = (c->y - a->y) >> 6, vz = (c->z - a->z) >> 6;
    int64_t cx = (int64_t)uy*vz - (int64_t)uz*vy;
    int64_t cy = (int64_t)uz*vx - (int64_t)ux*vz;
    int64_t cz = (int64_t)ux*vy - (int64_t)uy*vx;
    uint64_t mag2 = (uint64_t)(cx*cx + cy*cy + cz*cz);
    if (mag2 == 0) { *nx = 0; *ny = 32767; *nz = 0; return; }
    int64_t mag = (int64_t)isqrt64(mag2);
    int32_t rx = (int32_t)((cx * 32767) / mag), ry = (int32_t)((cy * 32767) / mag), rz = (int32_t)((cz * 32767) / mag);
    if (rx > 32767) rx = 32767; else if (rx < -32767) rx = -32767;
    if (ry > 32767) ry = 32767; else if (ry < -32767) ry = -32767;
    if (rz > 32767) rz = 32767; else if (rz < -32767) rz = -32767;
    *nx = (int16_t)rx; *ny = (int16_t)ry; *nz = (int16_t)rz;
}

#define RSTEP   (U * 3)
#define RBEHIND 4
#define RAHEAD  90
#define RSAMP   (RBEHIND + RAHEAD + 1)
static V3   rv[RSAMP * 2];
static Tri  rt[(RSAMP - 1) * 2];
static Mesh rm;

// Everything here is placed relative to dref (the car's current distance), not in
// absolute world Z — d itself grows without bound over a long run, and int32 vertex
// coordinates can't hold that. The camera and every instance agree on the same
// dref, so it's invisible: the world just always looks the same size close up.
static void build_ribbon(int64_t dref) {
    for (int i = 0; i < RSAMP; i++) {
        int64_t ds = dref + (int64_t)(i - RBEHIND) * RSTEP;
        int32_t tx = trackX(ds), ty = trackY(ds);
        int32_t relz = (int32_t)(ds - dref);
        rv[i*2+0] = (V3){ tx - TRACK_HALF, ty, relz };
        rv[i*2+1] = (V3){ tx + TRACK_HALF, ty, relz };
    }
    int nt = 0;
    for (int i = 0; i < RSAMP - 1; i++) {
        int L0 = i*2, R0 = i*2+1, L1 = (i+1)*2, R1 = (i+1)*2+1;
        int16_t nx, ny, nz;
        quad_normal(&rv[L0], &rv[R1], &rv[R0], &nx, &ny, &nz);
        rt[nt] = (Tri){ (uint16_t)L0, (uint16_t)R1, (uint16_t)R0, P_ROAD, nx, ny, nz }; nt++;
        rt[nt] = (Tri){ (uint16_t)L0, (uint16_t)L1, (uint16_t)R1, P_ROAD, nx, ny, nz }; nt++;
    }
    rm.v = rv; rm.nv = RSAMP*2; rm.t = rt; rm.nt = nt;
}

// ---- run state ------------------------------------------------------------------
#define SPEED_BASE    (U * 3 / 10)
#define SPEED_MAX     (U * 9 / 10)
#define SPEED_RAMPUP  (U / 3000)
#define BOOST_ADD     (U * 6 / 10)
#define BOOST_DUR     50
#define GRAVITY       (U / 55)
#define LAUNCH_SLACK  (U / 10)
#define SAFE_LANDING_VY (-(U * 12 / 10))
#define STEER_RATE    (U * 35 / 100)
#define GROUND_Y      (-(A1) - U * 4)
#define CAM_BEHIND    (U * 8)
#define CAM_HEIGHT    (U * 3)
#define CAM_PITCH     60

#define EV_LAUNCH 1
#define EV_LAND   2
#define EV_BOOST  4
#define EV_CRASH  8

typedef struct {
    int64_t d;
    int32_t wx, y, vy, speed, boostT;
    uint8_t airborne, prevJump, over;
    int8_t  lives;
    uint32_t frame, events;
} Run;
static Run g_run;

static void reset_run(void) {
    g_run.d = 0;
    g_run.wx = 0;
    g_run.y = trackY(0);
    g_run.vy = 0;
    g_run.speed = SPEED_BASE;
    g_run.boostT = 0;
    g_run.airborne = 0;
    g_run.events = 0;
}

// ---- text helpers (each cartridge carries its own; text.h has no layout of its own) --
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i/2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; digits(v, b);
    text_draw(x, y, sc, b, ci);
}

// ---- the contract -----------------------------------------------------------------
static void init(void) {
    tables_init();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF262A46;              // dusk sky
    ramp(P_ROAD,    0xFF4A4E63);
    ramp(P_GROUND,  0xFF2F6E4A);
    ramp(P_KERB,    0xFFE0483C);
    ramp(P_KERB2,   0xFF3EA6F5);
    ramp(P_CARBODY, 0xFFEF5B3B);
    ramp(P_CABIN,   0xFF2A2E38);
    ramp(P_WHEEL,   0xFF1C1C22);
    build_car();
    reset_run();
    g_run.lives = 3;
    g_run.over = 0;
    g_run.frame = 0;
    g_run.prevJump = 0;
}

static Input combine(const Input in[2]) {
    int x = in[0].x + in[1].x; if (x > 1) x = 1; else if (x < -1) x = -1;
    int y = in[0].y + in[1].y; if (y > 1) y = 1; else if (y < -1) y = -1;
    Input p; p.x = (int8_t)x; p.y = (int8_t)y;
    p.jump = (uint8_t)(in[0].jump || in[1].jump);
    p.act  = (uint8_t)(in[0].act  || in[1].act);
    return p;
}

// 🔴 PURE: no clock, no rand, no drawing. Same inputs from the same init give the same
// state, forever — that's what makes a scripted --keys run reproducible enough to verify.
static void tick(const Input in[2]) {
    Input p = combine(in);
    g_run.events = 0;

    if (g_run.over) {
        int pressed = p.jump && !g_run.prevJump;
        g_run.prevJump = p.jump;
        if (pressed) { reset_run(); g_run.lives = 3; g_run.over = 0; }
        g_run.frame++;
        return;
    }

    int pressed = p.jump && !g_run.prevJump;
    g_run.prevJump = p.jump;
    if (pressed && g_run.boostT <= 0) { g_run.boostT = BOOST_DUR; g_run.events |= EV_BOOST; }
    if (g_run.boostT > 0) g_run.boostT--;

    if (g_run.speed < SPEED_MAX) { g_run.speed += SPEED_RAMPUP; if (g_run.speed > SPEED_MAX) g_run.speed = SPEED_MAX; }
    int32_t curSpeed = g_run.speed + (g_run.boostT > 0 ? BOOST_ADD : 0);

    int32_t steerRate = g_run.airborne ? STEER_RATE / 2 : STEER_RATE;
    g_run.wx += (int32_t)p.x * steerRate;

    int64_t d0 = g_run.d, d1 = d0 + curSpeed;
    int crashed = 0;

    if (!g_run.airborne) {
        // Compare the vertical rate the track just asked of you against the rate it's
        // about to ask next tick. A rolling hill changes that rate gently; a ramp's lip
        // changes it in one step — that jump is the whole detector.
        int32_t vPrev = trackY(d0) - trackY(d0 - curSpeed);
        int32_t vNext = trackY(d1) - trackY(d0);
        if (vPrev - vNext > LAUNCH_SLACK) {
            g_run.airborne = 1;
            g_run.vy = vPrev;              // keep the momentum the ramp gave you
            g_run.y += vPrev;
            g_run.events |= EV_LAUNCH;
        } else {
            g_run.y = trackY(d1);          // grounded: snap to the road
        }
    } else {
        g_run.y += g_run.vy;
        g_run.vy -= GRAVITY;
        int32_t groundY = trackY(d1);
        if (g_run.y <= groundY) {
            if (g_run.vy < SAFE_LANDING_VY) {
                crashed = 1;                // came down too hard
            } else {
                g_run.y = groundY; g_run.vy = 0; g_run.airborne = 0;
                g_run.events |= EV_LAND;
            }
        }
    }

    g_run.d = d1;

    if (!crashed) {
        int32_t off = g_run.wx - trackX(d1);
        if (off > TRACK_HALF || off < -TRACK_HALF) crashed = 1;   // flew off the side
    }

    if (crashed) {
        g_run.events |= EV_CRASH;
        g_run.lives--;
        if (g_run.lives <= 0) { g_run.lives = 0; g_run.over = 1; }
        else {
            g_run.wx = trackX(g_run.d);
            g_run.y = trackY(g_run.d);
            g_run.vy = 0; g_run.airborne = 0;
            g_run.speed = SPEED_BASE; g_run.boostT = 0;
        }
    }

    g_run.frame++;
}

static void audio(void) {
    if (g_run.events & EV_LAUNCH) synth_note(NCHAN - 1, 4, 74, 190);
    if (g_run.events & EV_LAND)   synth_note(NCHAN - 1, 3, 48, 140);
    if (g_run.events & EV_BOOST)  synth_note(NCHAN - 1, 5, 86, 210);
    if (g_run.events & EV_CRASH)  synth_note(NCHAN - 1, 5, 32, 230);
}

static void draw(void) {
    fb_clear(0);
    nbox = 0;
    static Inst inst[8 + MAXBOX];
    int n = 0;

    build_ribbon(g_run.d);
    inst[n].m = &rm; inst[n].pos = (V3){0,0,0}; inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;

    // The ground: one wide, thin slab well below the lowest dip in the road, so a ramp's
    // gap reads as air over open ground, not a hole in the world.
    {
        int bi = box(U * 40, U / 50, RAHEAD * RSTEP, P_GROUND);
        inst[n].m = &bm[bi];
        inst[n].pos = (V3){ trackX(g_run.d), GROUND_Y, 0 };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
    }

    // Slalom pylons, alternating sides, so the curve and the elevation both have a
    // ruler standing next to them — and so the sense of speed has something to pass.
    for (int i = 0; i < RAHEAD; i += 2) {
        int64_t ds = g_run.d + (int64_t)i * RSTEP;
        int32_t tx = trackX(ds), ty = trackY(ds);
        int side = (i / 2) & 1;
        int32_t px = tx + (side ? (TRACK_HALF + U/2) : -(TRACK_HALF + U/2));
        int bi = box(U/10, U*6/10, U/10, (uint8_t)(side ? P_KERB2 : P_KERB));
        inst[n].m = &bm[bi];
        inst[n].pos = (V3){ px, ty + U*3/10, (int32_t)(ds - g_run.d) };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
    }

    // The car: pitch with the slope ahead, roll with the curve, sitting exactly on
    // trackY (or wherever gravity has carried it, mid-arc).
    int32_t dyF = trackY(g_run.d + RSTEP) - trackY(g_run.d - RSTEP);
    int32_t dxF = trackX(g_run.d + RSTEP) - trackX(g_run.d - RSTEP);
    int pitch = angle_from_slope(-dyF, 2 * RSTEP);
    int roll  = angle_from_slope(dxF, 2 * RSTEP);
    inst[n].m = &carm;
    inst[n].pos = (V3){ g_run.wx, g_run.y, 0 };
    inst[n].ax = pitch; inst[n].ay = 0; inst[n].az = roll;
    inst[n].scale = U; n++;

    int32_t camX = g_run.wx;
    int32_t camY = g_run.y + CAM_HEIGHT;
    Cam cam = { { camX, camY, -CAM_BEHIND }, CAM_PITCH, 0, 0 };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    // ---- HUD --------------------------------------------------------------------
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6*sc, hud_top(), sc, "DIST", 1);
    num(6*sc + 30*sc, hud_top(), sc, (int32_t)(g_run.d >> 16), 2);

    char lb[12]; int lw = digits(g_run.lives, lb);
    int lrmargin = 6, lgap = 4;                          // right margin, and gap before the number
    int lnumx = g_fbw - (lrmargin + lw*6 - 1)*sc;         // -1: text_draw's last glyph has no trailing pitch
    text_draw(lnumx - (lgap + 30)*sc, hud_top(), sc, "LIVES", 1);
    num(lnumx, hud_top(), sc, g_run.lives, 2);

    if (g_run.boostT > 0) {
        const char *s = "BOOST";
        text_draw(g_fbw/2 - text_width(s, sc) / 2, hud_top(), sc, s, 3);
    } else if (!g_run.over && g_run.airborne) {
        const char *s = "AIRBORNE";
        text_draw(g_fbw/2 - text_width(s, sc) / 2, hud_top(), sc, s, 2);
    }

    if (g_run.over) {
        int cx = g_fbw/2, cy = g_fbh/2;
        for (int y = cy - 26*sc; y < cy + 26*sc; y++)
            for (int x = 0; x < g_fbw; x++)
                if (y >= 0 && y < g_fbh) g_fb[y*g_fbw + x] = 25;   // a dark band from the KERB ramp
        text_draw(cx - text_width("CRASHED", sc*3) / 2, cy - 18*sc, sc*3, "CRASHED", 2);
        text_draw(cx - text_width("SPACE TO RESTART", sc) / 2, cy + 14*sc, sc, "SPACE TO RESTART", 1);
    }
}

static uint64_t checksum(void) {
    uint64_t h = (uint64_t)g_run.d;
    h = h*31 + (uint32_t)g_run.wx;
    h = h*31 + (uint32_t)g_run.y;
    h = h*31 + (uint32_t)g_run.vy;
    h = h*31 + (uint32_t)g_run.speed;
    h = h*31 + (uint32_t)g_run.boostT;
    h = h*31 + g_run.airborne;
    h = h*31 + (uint32_t)g_run.lives;
    h = h*31 + g_run.over;
    h = h*31 + g_run.frame;
    return h;
}

const Game game_stunt = { "stunt", init, tick, audio, draw, checksum };
