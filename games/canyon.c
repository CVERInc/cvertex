// canyon.c — CANYON DIVE: plunge forward through a sandstone gorge, threading rings that
// rush out of the haze and grow as they near. Steer with both hands (WASD and the arrows
// drive the same little ship), line the hole up with the reticle your own nose already is,
// and don't clip the frame.
//
// The whole trick is that nothing here moves the WORLD sideways. The ship's x/y is the one
// free variable; every gate's centre and every rock spur's jut is a pure function of its own
// index (or of total distance flown), so a gate a thousand slots out already knows exactly
// where it will be relative to z=0 the instant it gets there. That's what makes threading it
// possible at all — the shape rushing at you was never improvised, it's a wave arriving.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// v is in hundredths of a world unit — U(240) reads as "2.40 units" the way the rest of the
// house does it, and every constant below stays a small, checkable integer.
#define U(v) ((int32_t)((int64_t)(v) * 65536 / 100))
static int32_t mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static int32_t clampI(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// lowbias32: a real hash, not a Weyl sequence dressed as one — see the engine's note on the same
// function. Every gate and rock spur reads its own shape out of this, keyed on its own index,
// so replays and lockstep are free and nothing has to be stored.
static uint32_t g_hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ---- runtime box meshes -------------------------------------------------------------------
// Every solid in this game is a box built at draw time, exactly the engine's box(): eight verts,
// twelve tris, outward normals baked in so culling and the 8-shade gloss ramp both read the
// same truth. nbox resets every draw() — these are frame-lifetime meshes, not a model cache.
#define MAXBOX 200
static V3   bv[MAXBOX][8];
static Tri  bt[MAXBOX][12];
static Mesh bm[MAXBOX];
static int  nbox;
static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                 {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static int box(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return 0;
    if (sx < 1) sx = 1; if (sy < 1) sy = 1; if (sz < 1) sz = 1;
    int i = nbox++;
    for (int v = 0; v < 8; v++) {
        bv[i][v].x = VP[v][0] * sx; bv[i][v].y = VP[v][1] * sy; bv[i][v].z = VP[v][2] * sz;
    }
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &bt[i][f * 2 + k];
            t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k];
            t->ci = ci;
            t->nx = (int16_t)(FN[f][0] * 32767); t->ny = (int16_t)(FN[f][1] * 32767);
            t->nz = (int16_t)(FN[f][2] * 32767);
        }
    bm[i].v = bv[i]; bm[i].nv = 8; bm[i].t = bt[i]; bm[i].nt = 12;
    return i;
}

// ---- palette -------------------------------------------------------------------------------
#define P_HULL      8    //  8..15  the ship's body
#define P_WING     16    // 16..23  wings, a shade darker
#define P_COCKPIT  24    // 24..31  the one glassy, glowing thing on it
#define P_ENGINE   32    // 32..39  thruster glow, brightens on a boost
#define P_WALL     40    // 40..47  canyon rock
#define P_FLOOR    48    // 48..55  the gorge floor
#define P_FRAME    56    // 56..63  a gate's ring, at rest
#define P_FRAME_HOT 64   // 64..71  the very next gate — hot, pulsing, unmissable
#define P_FRAME_DIM 72   // 72..79  its dim half of the pulse
#define P_SPARK    80    // 80..87  wreckage, flung outward
#define P_FLASH    88    // 88..95  the pop of impact

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int k = 70 + i * 26;
        g_pal[base + i] = 0xFF000000u
            | ((uint32_t)(((rgb >> 16) & 255) * k / 255) << 16)
            | ((uint32_t)(((rgb >> 8) & 255) * k / 255) << 8)
            | (uint32_t)((rgb & 255) * k / 255);
    }
}

// ---- the ship --------------------------------------------------------------------------
// Seven boxes: a fuselage, two wings, a fin, a canopy, two engines. Every limb is an offset
// from the ship's own centre, in hundredths, +z forward — so build_ship can rotate the whole
// rigid set by one orientation and it reads as one banking body, not seven boxes agreeing by
// coincidence.
typedef struct { int16_t x, y, z, sx, sy, sz; uint8_t mat; } Limb;
enum { M_HULL, M_WING, M_COCKPIT, M_ENGINE };
#define NSHIP 7
static const Limb SHIP[NSHIP] = {
    {   0,  0,   2,   9,  6,  24, M_HULL },      // fuselage
    { -17, -1,  -3,  15,  2,   9, M_WING },      // left wing
    {  17, -1,  -3,  15,  2,   9, M_WING },      // right wing
    {   0,  9, -16,   2,  9,   6, M_HULL },      // tailfin
    {   0,  5,   9,   5,  3,   7, M_COCKPIT },   // canopy — this is the nose, and the nose is
                                                  // the reticle: line it up with the hole.
    {  -7,  0, -22,   3,  3,   3, M_ENGINE },    // left engine
    {   7,  0, -22,   3,  3,   3, M_ENGINE },    // right engine
};

static int emit_ship(Inst *inst, int n, int32_t px, int32_t py, int32_t pz,
                      int ax, int ay, int az, int32_t glow) {
    for (int L = 0; L < NSHIP && n < MAXBOX; L++) {
        const Limb *b = &SHIP[L];
        int32_t sx = U(b->sx), sy = U(b->sy), sz = U(b->sz);
        if (b->mat == M_ENGINE) {                    // the thruster swells on a boost
            int32_t grow = mul(U(5), glow);
            sx += grow; sy += grow; sz += grow;
        }
        int32_t ox = U(b->x), oy = U(b->y), oz = U(b->z);
        g3d_rot(&ox, &oy, &oz, ax, ay, az);
        uint8_t ci = (uint8_t)(b->mat == M_HULL ? P_HULL : b->mat == M_WING ? P_WING
                               : b->mat == M_COCKPIT ? P_COCKPIT : P_ENGINE);
        inst[n].m = &bm[box(sx, sy, sz, ci)];
        inst[n].pos = (V3){ px + ox, py + oy, pz + oz };
        inst[n].ax = ax; inst[n].ay = ay; inst[n].az = az;
        inst[n].scale = 1 << 16;
        n++;
    }
    return n;
}

// ---- the gorge -------------------------------------------------------------------------
// A straight, wide corridor: a flat floor, two flat cliffs at its edges, and a scattering of
// rock spurs along the rim for a skyline that isn't a plane. Every spur is keyed on its own
// world-anchored index (segment base + offset, not a frame-relative loop counter) so it has a
// fixed identity — real terrain sliding past, not wallpaper that swims independently of the
// ground it's supposed to be attached to.
#define CANYON_HW     U(240)     // half width of the flyable gorge
#define CANYON_Y_MAX  U(300)     // soft ceiling
#define WALL_X_HALF   U(150)
#define WALL_Y_HALF   U(180)
#define SPUR_SEG      U(420)
#define NSPUR         16
#define SPUR_H_BASE   U(90)
#define SPUR_H_VAR    U(230)
#define SPUR_HW_BASE  U(18)
#define SPUR_HW_VAR   U(34)
#define SPUR_INSET_MAX U(10)     // how far a spur may lean into the gorge — kept a couple units
                                  // shy of where the ship's own clamp can ever put a wingtip
                                  // (CANYON_HW - SHIP_HIT_HW = 229), so a spur is texture, never
                                  // something the ship can be seen poking through.

static int64_t g_dist;   // how far the gorge has rushed past — the one clock everything reads

static int emit_canyon(Inst *inst, int n) {
    int32_t zc = U(4000), zh = U(4200);            // one long floor, covers LEAD..the horizon
    if (n < MAXBOX) {
        inst[n].m = &bm[box(CANYON_HW + WALL_X_HALF * 2, U(6), zh, P_FLOOR)];
        inst[n].pos = (V3){ 0, -U(6), zc };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }
    if (n < MAXBOX) {                               // right cliff, flat and constant
        inst[n].m = &bm[box(WALL_X_HALF, WALL_Y_HALF, zh, P_WALL)];
        inst[n].pos = (V3){ CANYON_HW + WALL_X_HALF, WALL_Y_HALF, zc };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }
    if (n < MAXBOX) {                               // left cliff
        inst[n].m = &bm[box(WALL_X_HALF, WALL_Y_HALF, zh, P_WALL)];
        inst[n].pos = (V3){ -(CANYON_HW + WALL_X_HALF), WALL_Y_HALF, zc };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }

    uint32_t base = (uint32_t)(g_dist / SPUR_SEG);
    for (int i = -1; i < NSPUR && n < MAXBOX - 2; i++) {
        int32_t wk = (int32_t)base + i; if (wk < 0) continue;
        int32_t z = (int32_t)((int64_t)wk * SPUR_SEG - g_dist) + SPUR_SEG / 2;
        if (z < -U(200) || z > U(6500)) continue;
        for (int side = 0; side < 2 && n < MAXBOX; side++) {
            uint32_t h = g_hash32((uint32_t)wk * 2u + (uint32_t)side * 991u + 7u);
            int32_t hh = SPUR_H_BASE + (int32_t)(h % (uint32_t)(SPUR_H_VAR + 1));
            int32_t inset = (int32_t)((h >> 8) % (uint32_t)(SPUR_INSET_MAX + 1));
            int32_t hw = SPUR_HW_BASE + (int32_t)((h >> 16) % (uint32_t)(SPUR_HW_VAR + 1));
            int32_t sign = side ? 1 : -1;
            int32_t cx = sign * (CANYON_HW - inset + hw);
            inst[n].m = &bm[box(hw, hh, SPUR_SEG / 2, P_WALL)];
            inst[n].pos = (V3){ cx, hh, z };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++;
        }
    }
    return n;
}

// ---- the gates -------------------------------------------------------------------------
// A gate is a rectangular ring: four struts framing a hole. Its centre wanders on two slow,
// incommensurate sine waves keyed on its own index — a Lissajous meander, so consecutive
// gates never teleport but the course never repeats on any timescale a run could reach. The
// hole itself shrinks with total distance flown, not with any one gate's index, so the whole
// course tightens together instead of one gate being arbitrarily meaner than its neighbours.
#define SPACING       U(700)
#define LEAD          (SPACING * 3)
#define NGATE_DRAW    8
#define FRAME_HT      U(5)      // strut half-thickness
#define STRUT_HZ      U(6)      // strut half-depth, so a ring reads as solid, not paper

#define AMPX   U(140)
#define FREQX  37
#define CANYON_MIDY U(150)
#define AMPY   U(85)
#define FREQY  53
#define PHASEY 0     // k=0's centre lines up with the ship's own spawn point — a fair first gate

#define HOLE_HW_START U(55)
#define HOLE_HW_MIN   U(26)
#define HOLE_HW_RATE  27        // raw 16.16 shrink per real unit of distance flown
#define HOLE_HH_START U(42)
#define HOLE_HH_MIN   U(20)
#define HOLE_HH_RATE  20

static int32_t gate_z(uint32_t k) { return (int32_t)((int64_t)k * SPACING + LEAD - g_dist); }
static int32_t gate_cx(uint32_t k) {
    return (int32_t)(((int64_t)g_sin[(k * FREQX) & 1023] * AMPX) >> 15);
}
static int32_t gate_cy(uint32_t k) {
    return CANYON_MIDY + (int32_t)(((int64_t)g_sin[(k * FREQY + PHASEY) & 1023] * AMPY) >> 15);
}
static int32_t gate_hw(void) {
    int32_t real = (int32_t)(g_dist >> 16);
    int32_t hw = HOLE_HW_START - real * HOLE_HW_RATE;
    return hw < HOLE_HW_MIN ? HOLE_HW_MIN : hw;
}
static int32_t gate_hh(void) {
    int32_t real = (int32_t)(g_dist >> 16);
    int32_t hh = HOLE_HH_START - real * HOLE_HH_RATE;
    return hh < HOLE_HH_MIN ? HOLE_HH_MIN : hh;
}
static uint32_t live_base(void) {
    int64_t t = g_dist - LEAD;
    return t < 0 ? 0u : (uint32_t)(t / SPACING);
}
// The first gate that hasn't crossed z=0 yet — the one the hot ramp points at.
static uint32_t next_gate(void) {
    uint32_t k = live_base();
    for (int i = 0; i < 6; i++) { if (gate_z(k) > 0) return k; k++; }
    return k;
}

static int emit_one_gate(Inst *inst, int n, int32_t gx, int32_t gy, int32_t z,
                          int32_t hw, int32_t hh, uint8_t ci) {
    int32_t ow = hw + FRAME_HT;
    if (n < MAXBOX) { inst[n].m = &bm[box(ow, FRAME_HT, STRUT_HZ, ci)];
        inst[n].pos = (V3){ gx, gy + hh + FRAME_HT, z };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++; }
    if (n < MAXBOX) { inst[n].m = &bm[box(ow, FRAME_HT, STRUT_HZ, ci)];
        inst[n].pos = (V3){ gx, gy - hh - FRAME_HT, z };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++; }
    if (n < MAXBOX) { inst[n].m = &bm[box(FRAME_HT, hh, STRUT_HZ, ci)];
        inst[n].pos = (V3){ gx - hw - FRAME_HT, gy, z };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++; }
    if (n < MAXBOX) { inst[n].m = &bm[box(FRAME_HT, hh, STRUT_HZ, ci)];
        inst[n].pos = (V3){ gx + hw + FRAME_HT, gy, z };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++; }
    return n;
}

static uint32_t g_frame;
static int emit_gates(Inst *inst, int n) {
    uint32_t base = live_base(), nk = next_gate();
    for (uint32_t k = base; k < base + NGATE_DRAW && n < MAXBOX - 4; k++) {
        int32_t z = gate_z(k);
        if (z < -U(80) || z > U(6500)) continue;
        uint8_t ci;
        if (k == nk) {
            int lit = g_sin[(g_frame * 30) & 1023] > 0;
            ci = (uint8_t)(lit ? P_FRAME_HOT : P_FRAME_DIM);
        } else ci = P_FRAME;
        n = emit_one_gate(inst, n, gate_cx(k), gate_cy(k), z, gate_hw(), gate_hh(), ci);
    }
    return n;
}

// Do you fit? Exactly the engine's fits(), specialised to one axis-aligned rectangular hole and a
// ship whose hitbox is deliberately smaller than its wings — a near-miss that reads as a
// graze on screen and a clean pass in the sim is the whole point of the genre.
#define SHIP_HIT_HW U(11)
#define SHIP_HIT_HH U(8)
static int gate_fits(int32_t px, int32_t py, int32_t gx, int32_t gy, int32_t hw, int32_t hh) {
    int32_t dx = px - gx; if (dx < 0) dx = -dx;
    int32_t dy = py - gy; if (dy < 0) dy = -dy;
    return dx + SHIP_HIT_HW <= hw && dy + SHIP_HIT_HH <= hh;
}

// ---- wreckage on a clip -----------------------------------------------------------------
#define HIT_FRAMES 42
static int emit_explosion(Inst *inst, int n, int32_t px, int32_t py, int32_t hit_t) {
    if (hit_t <= 0) return n;
    int32_t elapsed = HIT_FRAMES - hit_t;
    int32_t age = (elapsed << 16) / HIT_FRAMES;
    if (elapsed < 8 && n < MAXBOX) {                  // the flash: brief, bright, gone fast
        int32_t fl = U(70) - mul(U(260), age); if (fl < U(6)) fl = U(6);
        inst[n].m = &bm[box(fl, fl, fl, P_FLASH)];
        inst[n].pos = (V3){ px, py, 0 };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }
    for (int s = 0; s < 8 && n < MAXBOX; s++) {        // shards, flung radially, shrinking
        uint32_t h = g_hash32((uint32_t)(s * 131 + 7));
        int ang = (int)(h & 1023);
        int elev = (int)((h >> 10) & 1023);
        int32_t rad = mul(U(130), age);
        int32_t sxp = px + (int32_t)(((int64_t)g_sin[ang] * rad) >> 15);
        int32_t szp = 0  + (int32_t)(((int64_t)g_sin[(ang + 256) & 1023] * rad) >> 15);
        int32_t syp = py + (int32_t)(((int64_t)g_sin[elev] * rad) >> 16);
        int32_t sz = U(9) - mul(U(7), age); if (sz < U(2)) sz = U(2);
        inst[n].m = &bm[box(sz, sz, sz, P_SPARK)];
        inst[n].pos = (V3){ sxp, syp, szp };
        inst[n].ax = (int)(h & 1023); inst[n].ay = (int)((h >> 5) & 1023); inst[n].az = 0;
        inst[n].scale = 1 << 16; n++;
    }
    return n;
}

// ---- state -------------------------------------------------------------------------------
static int32_t g_px, g_py, g_pvx, g_pvy;   // ship position and steering velocity, 16.16
static int32_t g_speed;
static int32_t g_boostglow;                // 0..65536, eased — the engines' own inertia
static int g_score, g_lives, g_best, g_over;
static int32_t g_hit_t;                    // frames of tumble/invulnerability left after a clip
static int32_t g_shake;
static uint8_t g_events;
static uint64_t g_checksum;
#define EV_PASS 1
#define EV_HIT  2
#define EV_OVER 4

#define SPEED_MIN U(12)
#define SPEED_MAX U(30)
#define SPEED_SHIFT 12
#define BOOST_ADD U(10)
#define SPEED_CAP U(40)
#define MAXV      U(10)
#define STEER_K   18000          // ~0.27 in 16.16 — a one-pole filter, snappy but not twitchy
#define SHAKE_HIT U(18)

#define CAM_PITCH_BASE 18
#define CHASE_BACK U(75)
#define CHASE_UP   U(22)
#define BANK_K    700     // full stick lean maxes out around 25 degrees — a lean, not a barrel roll
#define BANK_MAX  100
#define PITCH_K   450
#define PITCH_MAX  70
#define CAMPITCH_K 600
#define CAMYAW_K   500

static void init(void) {
    tables_init();
    nbox = 0;
    g_frame = 0; g_dist = 0; g_speed = SPEED_MIN;
    g_px = 0; g_py = CANYON_MIDY; g_pvx = 0; g_pvy = 0; g_boostglow = 0;
    g_score = 0; g_lives = 3; g_over = 0; g_hit_t = 0; g_shake = 0;
    g_checksum = 0; g_events = 0;

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF241A2E;             // dusk, deep and a little warm — a gorge at last light
    g_pal[1] = 0xFF9A8FB0;             // a label
    g_pal[2] = 0xFFF5F0FA;             // a number
    g_pal[3] = 0xFF17121F;             // the band a crash sits on
    ramp(P_HULL,    0xFF4FA8C7);
    ramp(P_WING,    0xFF2C5C74);
    ramp(P_COCKPIT, 0xFF4DF0FF);       // the nose — bright, glassy, the reticle
    ramp(P_ENGINE,  0xFFFF7A29);       // hot, and hotter on a boost
    ramp(P_WALL,    0xFF6B4A36);
    ramp(P_FLOOR,   0xFF4A3527);
    ramp(P_FRAME,   0xFFB8A888);
    ramp(P_FRAME_HOT, 0xFF4DF0FF);     // the same cyan as the cockpit — "this is your line"
    ramp(P_FRAME_DIM, 0xFF2C8A99);
    ramp(P_SPARK,   0xFFFF5A36);
    ramp(P_FLASH,   0xFFFFE9A8);
}

// Both control schemes drive the one ship — WASD and the arrows both land in x/y, added and
// clamped, exactly the two-keyboard-halves idea menu.c already uses for up/down.
static Input combine(const Input in[2]) {
    Input p;
    p.x = (int8_t)clampI(in[0].x + in[1].x, -1, 1);
    p.y = (int8_t)clampI(in[0].y + in[1].y, -1, 1);
    p.jump = (uint8_t)(in[0].jump || in[1].jump);
    p.act  = (uint8_t)(in[0].act  || in[1].act);
    return p;
}

static void tick(const Input raw[2]) {
    g_frame++;
    g_events = 0;

    if (g_shake > 0) { g_shake -= (g_shake >> 3) + U(1); if (g_shake < 0) g_shake = 0; }

    Input p = combine(raw);

    if (g_over) {
        // The dive is stopped; the wreck just sits there. Space starts it again — and the
        // best score is the one thing that survives the trip through init().
        if (p.jump) { int b = g_best; init(); g_best = b; }
        return;
    }

    if (g_hit_t > 0) g_hit_t--;

    int boosting = p.jump && g_hit_t == 0;
    int32_t speed = SPEED_MIN + (int32_t)(g_dist >> SPEED_SHIFT);
    if (speed > SPEED_MAX) speed = SPEED_MAX;
    if (boosting) { speed += BOOST_ADD; if (speed > SPEED_CAP) speed = SPEED_CAP; }
    g_speed = speed;
    g_dist += g_speed;

    int32_t targetglow = boosting ? 65536 : 0;
    g_boostglow += mul(targetglow - g_boostglow, 12000);

    // Steering is a one-pole filter chasing the held direction, not a teleport to it — that's
    // the whole of the banking feel below, for free: the velocity IS the lean.
    int32_t tvx = 0, tvy = 0;
    if (g_hit_t == 0) { tvx = (int32_t)p.x * MAXV; tvy = (int32_t)p.y * MAXV; }
    g_pvx += mul(tvx - g_pvx, STEER_K);
    g_pvy += mul(tvy - g_pvy, STEER_K);
    g_px += g_pvx; g_py += g_pvy;

    int32_t lo = -CANYON_HW + SHIP_HIT_HW, hi = CANYON_HW - SHIP_HIT_HW;
    if (g_px < lo) { g_px = lo; if (g_pvx < 0) g_pvx = 0; }
    if (g_px > hi) { g_px = hi; if (g_pvx > 0) g_pvx = 0; }
    lo = SHIP_HIT_HH; hi = CANYON_Y_MAX - SHIP_HIT_HH;
    if (g_py < lo) { g_py = lo; if (g_pvy < 0) g_pvy = 0; }
    if (g_py > hi) { g_py = hi; if (g_pvy > 0) g_pvy = 0; }

    // Judge whichever gate crosses z=0 THIS frame — crossing, not proximity, the same guard
    // the engine's wall collision earns: at top speed a gate can cover more ground in one frame
    // than a proximity window would ever catch.
    uint32_t base = live_base();
    for (uint32_t k = base; k < base + 3; k++) {
        int32_t z = gate_z(k);
        if (!(z <= 0 && z + g_speed > 0)) continue;
        if (g_hit_t > 0) continue;               // still tumbling — this one passes unjudged
        if (gate_fits(g_px, g_py, gate_cx(k), gate_cy(k), gate_hw(), gate_hh())) {
            g_score++; g_events |= EV_PASS;
        } else {
            g_lives--; g_hit_t = HIT_FRAMES; g_shake = SHAKE_HIT; g_events |= EV_HIT;
            if (g_lives <= 0) { g_lives = 0; g_over = 1; g_events |= EV_OVER; }
        }
    }
    if (g_score > g_best) g_best = g_score;

    g_checksum = g_checksum * 1000003ull
        ^ ((uint64_t)(uint32_t)g_px << 32 | (uint32_t)g_py)
        ^ ((uint64_t)(uint32_t)g_score << 24 | (uint32_t)g_lives << 16 | g_events)
        ^ (uint64_t)(g_dist >> 8);
}

static void audio(void) {
    if (g_events & EV_PASS) synth_note(NCHAN - 1, 5, 80, 180);
    if (g_events & EV_HIT)  synth_note(NCHAN - 1, 4, 40, 220);
    if (g_events & EV_OVER) synth_note(NCHAN - 1, 3, 28, 200);
}

// ---- HUD ---------------------------------------------------------------------------------
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; digits(v, b); text_draw(x, y, sc, b, ci);
}

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "SCORE", 1);
    num(6 * sc + 34 * sc, hud_top(), sc, g_score, 2);
    text_draw(6 * sc, hud_top() + 10 * sc, sc, "LIVES", 1);
    num(6 * sc + 34 * sc, hud_top() + 10 * sc, sc, g_lives, 2);
    if (g_best) {
        text_draw(6 * sc, hud_top() + 20 * sc, sc, "BEST", 1);
        num(6 * sc + 28 * sc, hud_top() + 20 * sc, sc, g_best, 1);
    }
    if (!g_over) return;

    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int y = cy - 30 * sc; y < cy + 34 * sc; y++)
        for (int x = 0; x < g_fbw; x++)
            if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = 3;
    text_draw(cx - text_width("CRASHED", sc * 3) / 2, cy - 22 * sc, sc * 3, "CRASHED", 2);
    char b[12]; digits(g_score, b);
    int w = text_width("SCORE", sc * 2) + 8 * sc * 2 + text_width(b, sc * 2);
    text_draw(cx - w / 2, cy + 3 * sc, sc * 2, "SCORE", 1);
    num(cx - w / 2 + text_width("SCORE", sc * 2) + 8 * sc * 2, cy + 3 * sc, sc * 2, g_score, 2);
    text_draw(cx - text_width("SPACE TO DIVE AGAIN", sc) / 2, cy + 22 * sc, sc,
              "SPACE TO DIVE AGAIN", 1);
}

// ---- draw ----------------------------------------------------------------------------------
static void draw(void) {
    fb_clear(0);
    nbox = 0;
    static Inst inst[MAXBOX];
    int n = 0;

    n = emit_canyon(inst, n);
    n = emit_gates(inst, n);
    n = emit_explosion(inst, n, g_px, g_py, g_hit_t);

    int32_t bank, pitch; int yaw = 0;
    if (g_hit_t > 0) {                            // tumbling: the ship stops answering you and
        int32_t elapsed = HIT_FRAMES - g_hit_t;   // spins on the very momentum that hurt it
        bank  = (elapsed * 160) & 1023;
        pitch = (elapsed * 70) & 1023;
        yaw   = (elapsed * 90) & 1023;
    } else {
        bank  = clampI((int32_t)(((int64_t)g_pvx * BANK_K) >> 16), -BANK_MAX, BANK_MAX);
        pitch = clampI(-(int32_t)(((int64_t)g_pvy * PITCH_K) >> 16), -PITCH_MAX, PITCH_MAX);
    }
    n = emit_ship(inst, n, g_px, g_py, 0, pitch & 1023, yaw & 1023, bank & 1023, g_boostglow);

    // The chase camera: glued to the ship's own x/y (so the reticle — the ship's nose — is
    // always exactly where you're looking) and offset behind and above along z, which is the
    // one axis nothing here steers. A little pitch/yaw lean toward the way you're steering, and
    // a little roll borrowed from the ship's own bank, so the picture leans into a turn instead
    // of just translating through one.
    int32_t camx = g_px, camy = g_py + CHASE_UP, camz = -CHASE_BACK;
    int camax = CAM_PITCH_BASE - clampI((int32_t)(((int64_t)g_pvy * CAMPITCH_K) >> 16), -40, 40);
    int camay = clampI((int32_t)(((int64_t)g_pvx * CAMYAW_K) >> 16), -40, 40);
    int camaz = bank / 6;

    if (g_shake > 0) {
        uint32_t r = g_hash32(g_frame * 2654435761u + 1u);
        camx += (int32_t)((((int64_t)((int)(r & 511) - 256) * g_shake)) >> 9);
        int32_t camy_j = (int32_t)((((int64_t)((int)((r >> 9) & 511) - 256) * g_shake)) >> 9);
        camay += (int)((((int64_t)((int)((r >> 18) & 255) - 128) * g_shake) >> 16));
        Cam cam = { { camx, camy + camy_j, camz }, camax & 1023, camay & 1023, camaz & 1023 };
        g3d_scene(inst, n, &cam, 0, 0, 0);
        hud();
        return;
    }
    Cam cam = { { camx, camy, camz }, camax & 1023, camay & 1023, camaz & 1023 };
    g3d_scene(inst, n, &cam, 0, 0, 0);
    hud();
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_canyon = { "canyon", init, tick, audio, draw, checksum };
