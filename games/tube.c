// tube.c — TUBE FLYER. An endless twisting neon tube, flown down the barrel.
//
// The tube is not a model, it's a formula: every ring's centre is two sines of its own
// distance down the track, so the whole thing curves without a single stored waypoint —
// build the ring at any z, anywhere, forever, and it's already the right shape. Walls are
// quads between consecutive rings; because the tube actually bends, those quads are never
// quite parallel to the direction of travel, so their true cross-product normal always has
// some z in it — which is what buys the flat-shaded gloss for free, without faking a light.
//
// Obstacles are barricades bolted to a ring: eight radial bars meeting at the axis, all of
// them except one contiguous gap. Dodging one is a bearing problem, not a reflex problem —
// find the open sector before the ring reaches you — which is exactly what a tube demands
// that a flat corridor doesn't.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
static int32_t mul(int32_t a, int32_t b)   { return (int32_t)(((int64_t)a * b) >> 16); }   // 16.16 * 16.16
static int32_t mulq15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }  // 16.16 * Q15

// ---- tunables -----------------------------------------------------------------
#define R0        U                        // tube radius, 1.0
#define SHIP_R    (U * 14 / 100)
#define SEGLEN    (U * 90 / 100)           // z between rings
#define NSIDE     10                       // wall panels per ring
#define RINGS_AHEAD 34                     // how far down the tube is built each frame
#define NSECT     8                        // barricade sectors (1024/8 divides exactly)
#define OBST_EVERY 5
#define OBST_START 8
#define GAP_MAX   3
#define GAP_MIN   1
#define RAMP_GAP_UNITS 40                  // world units between gap-width steps down
#define CAMDIST   (U * 370 / 100)          // how far behind the ship the lens sits — well
                                            // past the tube's own radius, or the ring right
                                            // beside the ship fills the whole frame
#define CAMY_OFF  (U * 3 / 100)
#define SPEED_MIN (U * 5 / 100)
#define SPEED_MAX (U * 22 / 100)
#define RAMP_DIST (U * 500)                // world units to reach top speed
#define STEER_ACCEL (U * 11 / 1000)
#define STEER_DRAG  4                      // velocity -= velocity >> 4 each frame
#define ANG1 32                            // table units of tube-bend per world unit z
#define ANG2 23
#define PH2  300
#define AMPX (R0 * 42 / 100)
#define AMPY (R0 * 28 / 100)

// palette
#define P_TUBE_A 8
#define P_TUBE_B 16
#define P_BAR    24
#define P_HULL   32
#define P_DARK   40
#define P_ENGINE 48
#define P_GLOW   56
#define P_SHARD  64

// ---- deterministic hash (lowbias32) -------------------------------------------
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int32_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int32_t)x;
}

// ---- box meshes at runtime (axis-aligned, own arena) --------------------------
#define MAXBOX 192
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

// ---- tube-wall quads (own arena; normal is a true cross product, not a guess) --
#define MAXQUAD 420
static V3   qv[MAXQUAD][4];
static Tri  qt[MAXQUAD][2];
static Mesh qm[MAXQUAD];
static int  nquad;

// Cross product, oriented so it points at `inside` (a point known to be on the tube's
// hollow side). Getting this from geometry instead of typing a sign by hand is what keeps
// a twisting tube from ever turning inside out, however sharply it bends.
static int quad(V3 a, V3 b, V3 c, V3 d, V3 inside, uint8_t ci) {
    if (nquad >= MAXQUAD) return -1;
    int i = nquad++;
    qv[i][0] = a; qv[i][1] = b; qv[i][2] = c; qv[i][3] = d;
    int64_t e1x = (b.x - a.x) >> 8, e1y = (b.y - a.y) >> 8, e1z = (b.z - a.z) >> 8;
    int64_t e2x = (c.x - a.x) >> 8, e2y = (c.y - a.y) >> 8, e2z = (c.z - a.z) >> 8;
    int64_t nx = e1y * e2z - e1z * e2y;
    int64_t ny = e1z * e2x - e1x * e2z;
    int64_t nz = e1x * e2y - e1y * e2x;
    int64_t ccx = (a.x + b.x + c.x + d.x) / 4, ccy = (a.y + b.y + c.y + d.y) / 4, ccz = (a.z + b.z + c.z + d.z) / 4;
    int64_t dot = nx * (inside.x - ccx) + ny * (inside.y - ccy) + nz * (inside.z - ccz);
    if (dot < 0) { nx = -nx; ny = -ny; nz = -nz; }
    int64_t magsq = nx * nx + ny * ny + nz * nz;
    int32_t mag = isqrt64(magsq); if (mag < 1) mag = 1;
    int16_t qnx = (int16_t)((nx * 32767) / mag), qny = (int16_t)((ny * 32767) / mag), qnz = (int16_t)((nz * 32767) / mag);
    qt[i][0] = (Tri){ 0, 1, 2, ci, qnx, qny, qnz };
    qt[i][1] = (Tri){ 0, 2, 3, ci, qnx, qny, qnz };
    qm[i] = (Mesh){ qv[i], 4, qt[i], 2 };
    return i;
}

// ---- the track: a formula, not a table -----------------------------------------
// The whole tube's shape lives here. Anything that wants a ring's centre — the wall
// builder, the barricades, the camera, the collision test — calls this with a z and gets
// the same answer, so nothing can ever disagree about where the tube is.
static void path_center(int32_t z, int32_t *cx, int32_t *cy) {
    int a1 = (int)(((int64_t)z * ANG1) >> 16) & 1023;
    int a2 = (int)((((int64_t)z * ANG2) >> 16) + PH2) & 1023;
    *cx = mulq15(AMPX, g_sin[a1]);
    *cy = mulq15(AMPY, g_sin[a2]);
}

static int32_t ring_z(int idx) { return (int32_t)((int64_t)idx * SEGLEN); }

static int32_t speed_for(int32_t dist) {
    int32_t d = dist; if (d < 0) d = 0; if (d > RAMP_DIST) d = RAMP_DIST;
    int32_t t = (int32_t)(((int64_t)d << 16) / RAMP_DIST);
    return SPEED_MIN + (int32_t)(((int64_t)(SPEED_MAX - SPEED_MIN) * t) >> 16);
}

// One barricade's whole personality: which sector its gap starts at, and how wide the gap
// still is. Both are pure functions of the ring's index and the run's progress, so the
// builder that draws the bars and the tick that judges a crossing can never disagree about
// where the opening was.
static void barrier_params(int idx, int *gapStart, int *gapWidth) {
    *gapStart = (int)(mix32((uint32_t)idx) % NSECT);
    int32_t worldUnits = ring_z(idx) >> 16;
    int w = GAP_MAX - (int)(worldUnits / RAMP_GAP_UNITS);
    if (w < GAP_MIN) w = GAP_MIN;
    if (w > GAP_MAX) w = GAP_MAX;
    *gapWidth = w;
}
static int in_gap(int sector, int gapStart, int gapWidth) {
    int d = (sector - gapStart) % NSECT; if (d < 0) d += NSECT;
    return d < gapWidth;
}
// Which of the NSECT bar directions a point is nearest. The sectors are evenly spaced, so
// "nearest by dot product" and "which wedge contains the angle" are the same question —
// no atan2 needed, just NSECT lookups in a table that's already there for the light.
static int sector_of(int32_t relx, int32_t rely) {
    int best = 0; int64_t bestDot = INT64_MIN;
    for (int k = 0; k < NSECT; k++) {
        int ac = (2 * k + 1) * 512 / NSECT;
        int32_t co = g_sin[(ac + 256) & 1023], si = g_sin[ac & 1023];
        int64_t d = (int64_t)relx * co + (int64_t)rely * si;
        if (d > bestDot) { bestDot = d; best = k; }
    }
    return best;
}

// ---- HUD text helpers -----------------------------------------------------------
static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 60 + i * 26;
        int r = (int)((rgb >> 16) & 255) * m / 255, g = (int)((rgb >> 8) & 255) * m / 255, b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) { char b[12]; digits(v, b); text_draw(x, y, sc, b, ci); }

// ---- state (everything tick may touch) -------------------------------------------
static uint32_t g_frame;
static int32_t g_dist, g_speed;
static int32_t g_ox, g_oy, g_ovx, g_ovy;   // ship offset & velocity from the tube's centreline
static int g_over, g_crasht;
static int32_t g_crashx, g_crashy, g_crashz;
static int32_t g_best;
static uint32_t g_events;
#define EV_CRASH 1
#define EV_PASS  2

static void init(void) {
    tables_init();
    g_frame = 0; g_dist = 0; g_speed = SPEED_MIN;
    g_ox = g_oy = g_ovx = g_ovy = 0;
    g_over = 0; g_crasht = 0; g_events = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF06060F;             // deep space behind the tube
    g_pal[1] = 0xFF7C82A6;             // HUD label
    g_pal[2] = 0xFFF2F4FA;             // HUD number
    g_pal[3] = 0xFF15171F;             // crash banner band
    ramp(P_TUBE_A, 0xFF33E6E6);        // neon cyan
    ramp(P_TUBE_B, 0xFFC138E6);        // neon magenta
    ramp(P_BAR,    0xFFF2482E);        // warning red — the only thing that kills you
    ramp(P_HULL,   0xFFE4E9F6);
    ramp(P_DARK,   0xFF3A3E52);
    ramp(P_ENGINE, 0xFFFFC23C);
    ramp(P_GLOW,   0xFFFFFFFF);
    ramp(P_SHARD,  0xFFFF7A38);
}

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0;

    if (g_over) {
        if (g_crasht < 255) g_crasht++;
        if (in[0].jump || in[1].jump) { int32_t b = g_best; init(); g_best = b; }
        return;
    }

    int ix = in[0].x + in[1].x; if (ix > 1) ix = 1; if (ix < -1) ix = -1;
    int iy = in[0].y + in[1].y; if (iy > 1) iy = 1; if (iy < -1) iy = -1;

    g_ovx += ix * STEER_ACCEL; g_ovy += iy * STEER_ACCEL;
    g_ovx -= g_ovx >> STEER_DRAG; g_ovy -= g_ovy >> STEER_DRAG;
    g_ox += g_ovx; g_oy += g_ovy;

    int32_t maxr = R0 - SHIP_R;
    int64_t magsq = (int64_t)g_ox * g_ox + (int64_t)g_oy * g_oy;
    if (magsq > (int64_t)maxr * maxr) {
        int32_t mag = isqrt64(magsq); if (mag < 1) mag = 1;
        g_ox = (int32_t)(((int64_t)g_ox * maxr) / mag);
        g_oy = (int32_t)(((int64_t)g_oy * maxr) / mag);
        g_ovx = g_ovx * 6 / 10; g_ovy = g_ovy * 6 / 10;   // the wall takes some of the speed
    }

    int32_t prevDist = g_dist;
    g_speed = speed_for(g_dist);
    g_dist += g_speed;

    int prevIdx = (int)(prevDist / SEGLEN);
    int newIdx  = (int)(g_dist   / SEGLEN);
    for (int idx = prevIdx + 1; idx <= newIdx; idx++) {
        if (idx < OBST_START || (idx % OBST_EVERY) != 0) continue;
        int32_t zabs = ring_z(idx);
        int32_t cx, cy; path_center(zabs, &cx, &cy);
        int32_t pcx, pcy; path_center(g_dist, &pcx, &pcy);
        int32_t px = pcx + g_ox, py = pcy + g_oy;
        int gapStart, gapWidth; barrier_params(idx, &gapStart, &gapWidth);
        int sect = sector_of(px - cx, py - cy);
        if (in_gap(sect, gapStart, gapWidth)) {
            g_events |= EV_PASS;
        } else {
            g_over = 1; g_crasht = 0;
            g_crashx = px; g_crashy = py; g_crashz = zabs - g_dist;
            g_events |= EV_CRASH;
            if ((g_dist >> 16) > g_best) g_best = g_dist >> 16;
        }
    }
    if (!g_over && (g_dist >> 16) > g_best) g_best = g_dist >> 16;
}

static void audio(void) {
    if (g_events & EV_CRASH)      synth_note(NCHAN - 1, 3, 32, 220);
    else if (g_events & EV_PASS)  synth_note(NCHAN - 1, 4, 79, 90);
}

// ---- drawing --------------------------------------------------------------------

// A barricade: NSECT radial bars meeting at the axis, minus one contiguous gap. Mounted
// flush on a ring, so the whole thing reads as part of the tube rather than a prop dropped
// into it.
static int emit_barrier(Inst *inst, int n, int idx, int32_t relz, int32_t cx, int32_t cy) {
    int gapStart, gapWidth; barrier_params(idx, &gapStart, &gapWidth);
    int32_t halfLen = R0 / 2;
    int32_t tangHalf = mul(mulq15(R0, g_sin[(512 / NSECT) & 1023]), U * 85 / 100);
    int32_t depthHalf = SEGLEN * 32 / 100;
    for (int k = 0; k < NSECT && n < MAXBOX + MAXQUAD - 1; k++) {
        if (in_gap(k, gapStart, gapWidth)) continue;
        int ac = (2 * k + 1) * 512 / NSECT;
        int32_t co = g_sin[(ac + 256) & 1023], si = g_sin[ac & 1023];
        int bi = box(halfLen, tangHalf, depthHalf, P_BAR);
        if (bi < 0) break;
        inst[n].m = &bm[bi];
        inst[n].pos = (V3){ cx + mulq15(halfLen, co), cy + mulq15(halfLen, si), relz };
        inst[n].ax = 0; inst[n].ay = 0; inst[n].az = ac & 1023;
        inst[n].scale = U;
        n++;
    }
    return n;
}

// The ship: a fuselage, swept wings, a fin, a cockpit bump and a glowing tail — six boxes,
// rotated by hand about the figure's own centre so a bank is a real roll and not six crates
// spinning on the spot (the same trick as every runtime figure in this engine).
typedef struct { int16_t x, y, z, sx, sy, sz, ay0; uint8_t mat; } Part;
#define NPART 6
static const Part SHIP[NPART] = {
    {   0,   0,   0,  45,  30,  95,    0, P_HULL   },   // fuselage
    { -85,  -8, -15,  60,  10,  50,  110, P_HULL   },   // left wing, swept back
    {  85,  -8, -15,  60,  10,  50, -110, P_HULL   },   // right wing, swept back
    {   0,  35, -75,  10,  25,  22,    0, P_DARK   },   // tail fin
    {   0,  18,  55,  20,  16,  22,    0, P_DARK   },   // cockpit
    {   0,  -8, -100, 18,  18,  16,    0, P_ENGINE },   // engine glow, faces the camera
};

static int emit_ship(Inst *inst, int n, int32_t wx, int32_t wy, int32_t wz, int roll, int pitch) {
    int32_t rc = g_sin[(roll + 256) & 1023], rs = g_sin[roll & 1023];
    int32_t pc = g_sin[(pitch + 256) & 1023], ps = g_sin[pitch & 1023];
    for (int p = 0; p < NPART && n < MAXBOX + MAXQUAD - 1; p++) {
        const Part *pt = &SHIP[p];
        int32_t ox = (int32_t)pt->x * U / 1000, oy = (int32_t)pt->y * U / 1000, oz = (int32_t)pt->z * U / 1000;
        // roll about z, then pitch about x — the ship's own centre, by hand.
        int32_t rx = (int32_t)(((int64_t)ox * rc - (int64_t)oy * rs) >> 15);
        int32_t ry = (int32_t)(((int64_t)ox * rs + (int64_t)oy * rc) >> 15);
        int32_t fy = (int32_t)(((int64_t)ry * pc - (int64_t)oz * ps) >> 15);
        int32_t fz = (int32_t)(((int64_t)ry * ps + (int64_t)oz * pc) >> 15);
        int32_t sx = (int32_t)pt->sx * U / 1000, sy = (int32_t)pt->sy * U / 1000, sz = (int32_t)pt->sz * U / 1000;
        int bi = box(sx, sy, sz, pt->mat);
        if (bi < 0) break;
        inst[n].m = &bm[bi];
        inst[n].pos = (V3){ wx + rx, wy + fy, wz + fz };
        inst[n].ax = pitch; inst[n].ay = pt->ay0; inst[n].az = roll;
        inst[n].scale = U;
        n++;
    }
    return n;
}

// The crash: a white pop and a spray of shards flung out from where the ship met the bar,
// pure functions of frames-since-impact so a replay always blows up exactly the same way.
static int emit_explosion(Inst *inst, int n, int32_t wx, int32_t wy, int32_t wz, int age) {
    if (age < 10) {
        int32_t fl = U * 22 / 100 - (int32_t)(((int64_t)(U * 19 / 100) * age) / 10); if (fl < U / 20) fl = U / 20;
        int bi = box(fl, fl, fl, P_GLOW);
        if (bi >= 0) { inst[n].m = &bm[bi]; inst[n].pos = (V3){ wx, wy, wz }; inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++; }
    }
    int32_t a16 = age * U / 40; if (a16 > U) a16 = U;
    for (int s = 0; s < 10 && n < MAXBOX + MAXQUAD - 1; s++) {
        uint32_t h = mix32((uint32_t)s * 977u + 17u);
        int ang = (int)(h & 1023);
        int updown = (int)((h >> 10) & 1023);
        int32_t rad = mul(U * 45 / 100, a16);
        int32_t sxp = wx + mulq15(rad, g_sin[ang]);
        int32_t szp = wz + mulq15(rad, g_sin[(ang + 256) & 1023]);
        int32_t syp = wy + mulq15(rad, g_sin[updown]) / 2 - mul(mul(U * 30 / 100, a16), a16);
        int bi = box(U * 5 / 100, U * 5 / 100, U * 5 / 100, P_SHARD);
        if (bi < 0) break;
        inst[n].m = &bm[bi];
        inst[n].pos = (V3){ sxp, syp, szp };
        inst[n].ax = (int)(h & 1023); inst[n].ay = (int)((h >> 5) & 1023); inst[n].az = 0;
        inst[n].scale = U;
        n++;
    }
    return n;
}

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "TUBE FLYER", 1);
    num(6 * sc, hud_top() + 11 * sc, sc, g_dist >> 16, 2);
    text_draw(6 * sc + 6 * sc * 6, hud_top() + 11 * sc, sc, "M", 1);
    if (g_best) { text_draw(g_fbw - 60 * sc, hud_top(), sc, "BEST", 1); num(g_fbw - 30 * sc, hud_top(), sc, g_best, 1); }
    if (!g_over) return;

    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int y = cy - 30 * sc; y < cy + 34 * sc; y++)
        for (int x = 0; x < g_fbw; x++) if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = 3;
    text_draw(cx - text_width("CRASHED", sc * 3) / 2, cy - 22 * sc, sc * 3, "CRASHED", 2);
    char b[12]; digits(g_dist >> 16, b);
    int w = text_width(b, sc * 2) + 6 * sc * 2 + text_width("M", sc * 2);
    num(cx - w / 2, cy + 3 * sc, sc * 2, g_dist >> 16, 2);
    text_draw(cx - w / 2 + text_width(b, sc * 2) + 6 * sc * 2, cy + 3 * sc, sc * 2, "M", 1);
    text_draw(cx - text_width("SPACE TO FLY AGAIN", sc) / 2, cy + 22 * sc, sc, "SPACE TO FLY AGAIN", 1);
}

static void draw(void) {
    fb_clear(0);
    nbox = 0; nquad = 0;
    static Inst inst[MAXBOX + MAXQUAD];
    int n = 0;

    int i0 = (int)(g_dist / SEGLEN);
    int istart = i0 - 1; if (istart < 0) istart = 0;

    int32_t cxPrev, cyPrev; path_center(ring_z(istart), &cxPrev, &cyPrev);
    for (int i = istart; i < i0 + RINGS_AHEAD; i++) {
        int32_t z0 = ring_z(i), z1 = ring_z(i + 1);
        int32_t relz0 = z0 - g_dist, relz1 = z1 - g_dist;
        if (relz1 < -SEGLEN * 2) continue;
        int32_t cx0 = cxPrev, cy0 = cyPrev;
        int32_t cx1, cy1; path_center(z1, &cx1, &cy1);
        cxPrev = cx1; cyPrev = cy1;

        uint8_t band = ((i / 2) & 1) ? P_TUBE_B : P_TUBE_A;
        V3 inside = { (cx0 + cx1) / 2, (cy0 + cy1) / 2, (relz0 + relz1) / 2 };
        for (int s = 0; s < NSIDE && n < MAXBOX + MAXQUAD - 1; s++) {
            int a0 = s * 1024 / NSIDE, a1 = (s + 1) * 1024 / NSIDE;
            int32_t co0 = g_sin[(a0 + 256) & 1023], si0 = g_sin[a0 & 1023];
            int32_t co1 = g_sin[(a1 + 256) & 1023], si1 = g_sin[a1 & 1023];
            V3 A = { cx0 + mulq15(R0, co0), cy0 + mulq15(R0, si0), relz0 };
            V3 B = { cx0 + mulq15(R0, co1), cy0 + mulq15(R0, si1), relz0 };
            V3 C = { cx1 + mulq15(R0, co1), cy1 + mulq15(R0, si1), relz1 };
            V3 D = { cx1 + mulq15(R0, co0), cy1 + mulq15(R0, si0), relz1 };
            int qi = quad(A, B, C, D, inside, band);
            if (qi < 0) continue;
            inst[n].m = &qm[qi]; inst[n].pos = (V3){ 0, 0, 0 }; inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U;
            n++;
        }
        if (i >= OBST_START && (i % OBST_EVERY) == 0 && relz0 > -SEGLEN)
            n = emit_barrier(inst, n, i, relz0, cx0, cy0);
    }

    // A glow at the far end — the vanishing point gets a light so the perspective has
    // something to converge on besides the walls thinning out.
    {
        int fi = i0 + RINGS_AHEAD;
        int32_t fz = ring_z(fi); int32_t fcx, fcy; path_center(fz, &fcx, &fcy);
        int bi = box(R0 * 85 / 100, R0 * 85 / 100, SEGLEN / 8, P_GLOW);
        if (bi >= 0) {
            inst[n].m = &bm[bi]; inst[n].pos = (V3){ fcx, fcy, fz - g_dist };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
        }
    }

    int32_t pcx, pcy; path_center(g_dist, &pcx, &pcy);
    int roll  = (int)((-(int64_t)g_ovx * 1400) >> 16); if (roll > 240) roll = 240; if (roll < -240) roll = -240;
    int pitch = (int)(((int64_t)g_ovy * 1400) >> 16);  if (pitch > 220) pitch = 220; if (pitch < -220) pitch = -220;

    if (g_over) {
        n = emit_explosion(inst, n, g_crashx, g_crashy, g_crashz, g_crasht);
    } else {
        n = emit_ship(inst, n, pcx + g_ox, pcy + g_oy, 0, roll & 1023, pitch & 1023);
    }

    int32_t camx = pcx, camy = pcy + CAMY_OFF, camz = -CAMDIST;
    int camaz = (roll / 5) & 1023, camax = (pitch / 6) & 1023;
    if (g_over && g_crasht < 30) {
        uint32_t r = mix32(g_frame * 2654435761u + 7u);
        int32_t sh = U * 6 / 100 - mul(U * 6 / 100, (g_crasht * U / 30));
        if (sh < 0) sh = 0;
        camx += (int32_t)((((int64_t)((int)(r & 511) - 256) * sh)) >> 9);
        camy += (int32_t)((((int64_t)((int)((r >> 9) & 511) - 256) * sh)) >> 9);
    }
    Cam cam = { { camx, camy, camz }, camax, 0, camaz };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    hud();
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ull;
    h = h * 31 + (uint32_t)g_dist; h = h * 31 + (uint32_t)g_ox; h = h * 31 + (uint32_t)g_oy;
    h = h * 31 + (uint32_t)g_ovx; h = h * 31 + (uint32_t)g_ovy; h = h * 31 + (uint32_t)g_over;
    h = h * 31 + (uint32_t)g_crasht; h = h * 31 + (uint32_t)g_best; h = h * 31 + g_frame;
    return h;
}

const Game game_tube = { "tube", init, tick, audio, draw, checksum };
