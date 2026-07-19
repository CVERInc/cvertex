// vectank.c — first-person vector tank. The camera IS the tank: it stands on a vast flat
// plane and looks down whatever heading the tank is pointed at. Steering doesn't slide a
// sprite around a fixed lens, it yaws the lens itself — the ground swings past instead of
// scrolling past, which is the one thing a rail camera can never do. Forward/back drives
// along that same heading, so "which way am I facing" and "which way do I go" are the same
// question for the first time in this tree.
//
// Everything here is the plane's own shapes: a checkerboard of thin flat boxes for the
// ground (dark-on-dark, gaps between them read as grid lines for free), pyramids for
// obelisks and a mountain horizon, a hand-built wedge for the enemy hull because a tank
// silhouette is a slope, not a box. Green on near-black, because a vector cabinet never
// needed a second colour to say "the future."
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- fixed point --------------------------------------------------------------
#define U(v) ((int32_t)((int64_t)(v) * 65536 / 100))   // 1/100 world unit -> 16.16
static int32_t mul16(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
// One operand 16.16, one Q15 (a g_sin/g_cos table read) -> 16.16. Every place a distance
// meets an angle, this is the join — get the shift wrong here and things drift.
static int32_t mulq(int32_t a, int32_t q15) { return (int32_t)(((int64_t)a * q15) >> 15); }

static uint32_t mix32(uint32_t x) {   // lowbias32: a Weyl sequence of indices is not noise
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
// Magnitude of a 16.16 (x,z) pair, itself 16.16. (16.16)*(16.16) as a raw int64 already
// carries 32 fractional bits, so its integer square root comes back out at 16.16 for free.
static int32_t mag2(int32_t x, int32_t z) { return isqrt64((int64_t)x * x + (int64_t)z * z); }
static int32_t fdiv_floor(int32_t a, int32_t b) {
    int32_t q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) q--;
    return q;
}

// ---- runtime meshes -----------------------------------------------------------
// One shared arena, rebuilt from scratch every draw(): every box/pyramid/wedge on screen
// is a fresh call into it, the same bargain the engine's built worlds make. Nothing here is const
// because everything is generated, and nothing persists across frames because nothing
// needs to.
#define MAXBOX 420
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

// A general face normal, for the two shapes that aren't axis-aligned boxes. `ref` is a
// point known to sit STRICTLY inside the solid — every convex shape's own vertex centroid
// works — so "the vector from ref to this triangle is outward" is exact for any face.
// 🔴 The local origin (0,0,0) is NOT always that point: the wedge's sloped top passes
// exactly through it (back-top is +sy at z=-sz, front-bottom is -sy at z=+sz, and the
// straight line between them crosses y=0 exactly at z=0). A reference ON a face makes the
// sign test degenerate right at that face — which read as fine head-on and turned a
// triangle inside-out the moment the camera rotated a few degrees off it. Use the true
// centroid and every face has the reference strictly on one side, always.
// 🔴 A mountain is a pyramid with a half-height past a thousand world units — 16.16, that's
// ~7e7. The cross product of two such edges is safe in an int64, but the FLIP TEST's dot
// product (cross · reference) squares that up again and overflows by six orders of
// magnitude, silently wrapping into a garbage sign. Wrapped once every rotation, that's a
// face whose winding flips as the camera turns past it — which is exactly the "fine
// head-on, inside-out from the side" symptom this shape produced. Shift every input coordinate
// down before any arithmetic touches it: direction survives losing 12 bits nobody was
// reading, and every product downstream shrinks by the same 2^12, so nothing this file
// ever builds can push it back into overflow.
#define NSHIFT 12
static void face_normal(V3 a, V3 b, V3 c, V3 ref, int16_t *nx, int16_t *ny, int16_t *nz) {
    int64_t ax = a.x >> NSHIFT, ay = a.y >> NSHIFT, az = a.z >> NSHIFT;
    int64_t bx = b.x >> NSHIFT, by = b.y >> NSHIFT, bz = b.z >> NSHIFT;
    int64_t cxp = c.x >> NSHIFT, cyp = c.y >> NSHIFT, czp = c.z >> NSHIFT;
    int64_t rfx = ref.x >> NSHIFT, rfy = ref.y >> NSHIFT, rfz = ref.z >> NSHIFT;
    int64_t e1x = bx - ax, e1y = by - ay, e1z = bz - az;
    int64_t e2x = cxp - ax, e2y = cyp - ay, e2z = czp - az;
    int64_t cx = e1y * e2z - e1z * e2y;
    int64_t cy = e1z * e2x - e1x * e2z;
    int64_t cz = e1x * e2y - e1y * e2x;
    int64_t mx = ax + bx + cxp - 3 * rfx;
    int64_t my = ay + by + cyp - 3 * rfy;
    int64_t mz = az + bz + czp - 3 * rfz;
    if (cx * mx + cy * my + cz * mz < 0) { cx = -cx; cy = -cy; cz = -cz; }
    int64_t len = isqrt64(cx * cx + cy * cy + cz * cz);
    if (len == 0) len = 1;
    *nx = (int16_t)(cx * 32767 / len);
    *ny = (int16_t)(cy * 32767 / len);
    *nz = (int16_t)(cz * 32767 / len);
}

static void set_tri(int m, int k, int a, int b, int c, V3 ref, uint8_t ci) {
    Tri *t = &bt[m][k];
    t->a = (uint16_t)a; t->b = (uint16_t)b; t->c = (uint16_t)c; t->ci = ci;
    int16_t nx, ny, nz; face_normal(bv[m][a], bv[m][b], bv[m][c], ref, &nx, &ny, &nz);
    t->nx = nx; t->ny = ny; t->nz = nz;
}

// A square obelisk: base to a point. Apex at +sy, base at -sy, so placing it with
// pos.y = sy sits the base exactly on the ground.
static int pyramid(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return 0;
    int i = nbox++;
    bv[i][0] = (V3){ 0, sy, 0 };
    bv[i][1] = (V3){ -sx, -sy, -sz }; bv[i][2] = (V3){ sx, -sy, -sz };
    bv[i][3] = (V3){ sx, -sy,  sz };  bv[i][4] = (V3){ -sx, -sy,  sz };
    V3 ref = { 0, -3 * sy / 5, 0 };   // the true centroid of apex + four base verts
    set_tri(i, 0, 0, 1, 2, ref, ci); set_tri(i, 1, 0, 2, 3, ref, ci);
    set_tri(i, 2, 0, 3, 4, ref, ci); set_tri(i, 3, 0, 4, 1, ref, ci);
    set_tri(i, 4, 1, 3, 2, ref, ci); set_tri(i, 5, 1, 4, 3, ref, ci);
    bm[i].v = bv[i]; bm[i].nv = 5; bm[i].t = bt[i]; bm[i].nt = 6;
    return i;
}

// The enemy hull: a rectangular back tapering to a low front edge — a wedge, not a box,
// because a tank silhouette is a slope. Front (the low, tapered end) sits at +z, which is
// this whole file's "forward", so an Inst rotated by ay = heading points its nose the
// right way for free.
static int wedge(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return 0;
    int i = nbox++;
    bv[i][0] = (V3){ -sx, -sy, -sz }; bv[i][1] = (V3){ sx, -sy, -sz };
    bv[i][2] = (V3){ sx,  sy, -sz };  bv[i][3] = (V3){ -sx,  sy, -sz };
    bv[i][4] = (V3){ -sx, -sy,  sz }; bv[i][5] = (V3){ sx, -sy,  sz };
    V3 ref = { 0, -sy / 3, -sz / 3 };   // the true centroid of all six verts
    set_tri(i, 0, 0, 1, 5, ref, ci); set_tri(i, 1, 0, 5, 4, ref, ci);   // bottom
    set_tri(i, 2, 0, 1, 2, ref, ci); set_tri(i, 3, 0, 2, 3, ref, ci);   // back
    set_tri(i, 4, 3, 2, 5, ref, ci); set_tri(i, 5, 3, 5, 4, ref, ci);   // sloped top
    set_tri(i, 6, 0, 3, 4, ref, ci);                               // left
    set_tri(i, 7, 1, 5, 2, ref, ci);                               // right
    bm[i].v = bv[i]; bm[i].nv = 6; bm[i].t = bt[i]; bm[i].nt = 8;
    return i;
}

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int k = 70 + i * 26;
        g_pal[base + i] = 0xFF000000u
            | ((uint32_t)(((rgb >> 16) & 255) * k / 255) << 16)
            | ((uint32_t)(((rgb >> 8) & 255) * k / 255) << 8)
            | (uint32_t)((rgb & 255) * k / 255);
    }
}

// ---- palette --------------------------------------------------------------
#define P_SKY    1
#define P_TILEA  8
#define P_TILEB  16
#define P_MOUNT  24
#define P_OBST   32
#define P_SPIRE  40
#define P_ENEMY  48
#define P_TURRET 56
#define P_SHELL  64
#define P_ESHELL 72
#define P_FLASH  80
#define P_DEBRIS 88
#define HUD_TXT  150
#define HUD_DIM  151
#define HUD_WARN 152
#define HUD_BG   153
#define HUD_RING 154
#define HUD_BLIP 155
#define HUD_SELF 156

// ---- the world --------------------------------------------------------------
#define WORLD_R    U(70000)     // the playable disc: 700 units out from the origin
#define TILE       U(1000)      // 10-unit ground tiles
#define GRID_N     14           // 14x14 of them, centred on the player
#define MOUNT_N    10
#define MOUNT_R_C  250000        // centiunits: the mountain ring sits 2500 units out
#define OBST_N     18
#define OBST_MINR_C 4000
#define OBST_MAXR_C 60000

#define ENEMY_MAX     5
#define ENEMY_R       U(180)
#define ENEMY_SPEED   U(35)
#define ENEMY_TURN    5
#define DETECT_R      U(45000)
#define ENGAGE_LOSE_R U(60000)
#define ENGAGE_NEAR   U(12000)
#define FIRE_R        U(35000)
#define ECOOLDOWN_BASE 90
#define SPAWN_MIN_C   15000
#define SPAWN_MAX_C   30000
#define RESPAWN_DELAY 100

#define PLAYER_R    U(150)
#define SPEED_FWD   U(80)
#define SPEED_BACK  U(50)
#define TURNRATE    6
#define FIRE_COOLDOWN 15
#define INVULN_T    50
#define LIVES_START 3
#define EYE_H       U(180)

#define PSHOT_MAX   4
#define ESHOT_MAX   6
#define SHOT_R      U(220)
#define PSHOT_SPEED U(600)
#define PSHOT_LIFE  45
#define ESHOT_SPEED U(280)
#define ESHOT_LIFE  70

#define BOOM_MAX    4
#define BOOM_LIFE   24

#define RAM_PUSH    U(500)
#define SHAKE_HIT   6
#define SHAKE_RAM   12

// ---- events (tick decides, audio() speaks) -----------------------------------
#define EV_FIRE  1
#define EV_EFIRE 2
#define EV_HIT   4
#define EV_RAM   8
#define EV_KILL  16
#define EV_OVER  32

typedef struct { int32_t x, z; int ang; uint8_t alive, state; int fireCD, patrolAng, patrolT, spawnT; } Enemy;
typedef struct { int32_t x, y, z; int ang, life; uint8_t alive; } Shot;
typedef struct { int32_t x, y, z; int age; uint8_t alive; } Boom;

static Enemy g_en[ENEMY_MAX];
static Shot  g_ps[PSHOT_MAX], g_es[ESHOT_MAX];
static Boom  g_bo[BOOM_MAX];
static int32_t g_px, g_pz;
static int g_ang, g_lives, g_score, g_fireCD, g_invuln, g_shake, g_over;
static uint32_t g_frame, g_rng, g_events;
static uint64_t g_checksum;

// ---- procedural placement: pure functions of an index, so nothing needs storing -----
static int32_t obst_ang(int i)  { return (int32_t)(mix32((uint32_t)i * 2u + 11u) & 1023u); }
static int32_t obst_rad(int i)  {
    uint32_t f = mix32((uint32_t)i * 2u + 97u) % (uint32_t)(OBST_MAXR_C - OBST_MINR_C + 1);
    return U(OBST_MINR_C + (int32_t)f);
}
static int32_t obst_x(int i) { int a = obst_ang(i); return mulq(obst_rad(i), g_sin[a]); }
static int32_t obst_z(int i) { int a = obst_ang(i); return mulq(obst_rad(i), g_sin[(a + 256) & 1023]); }
static int  obst_kind(int i) { return (int)(mix32((uint32_t)i * 5u + 3u) & 1u); }
static int32_t obst_r(int i) { return obst_kind(i) ? U(220) : U(320); }
static int32_t obst_h(int i) { return obst_kind(i) ? U(600) : U(300); }

static int32_t mount_ang(int i) { return (i * 1024) / MOUNT_N; }
static int32_t mount_h(int i) {
    return U(80000 + (int32_t)(mix32((uint32_t)i * 13u + 7u) % 40000u));
}

// Push (x,z) out of every obstacle it's overlapping, radius `rad`. Used by the player and
// every enemy — one rule, so a wall behaves the same for both.
static void resolve_obstacles(int32_t *x, int32_t *z, int32_t rad) {
    for (int i = 0; i < OBST_N; i++) {
        int32_t ox = obst_x(i), oz = obst_z(i), minr = rad + obst_r(i);
        int32_t dx = *x - ox, dz = *z - oz;
        int32_t dist = mag2(dx, dz);
        if (dist < minr) {
            if (dist < 256) { dx = 65536; dz = 0; dist = 65536; }
            int32_t push = minr - dist;
            *x += (int32_t)(((int64_t)dx * push) / dist);
            *z += (int32_t)(((int64_t)dz * push) / dist);
        }
    }
}
static void clamp_world(int32_t *x, int32_t *z) {
    int32_t d = mag2(*x, *z);
    if (d > WORLD_R) {
        *x = (int32_t)(((int64_t)*x * WORLD_R) / d);
        *z = (int32_t)(((int64_t)*z * WORLD_R) / d);
    }
}
static int32_t dist2(int32_t x1, int32_t z1, int32_t x2, int32_t z2) { return mag2(x1 - x2, z1 - z2); }

static void spawn_boom(int32_t x, int32_t y, int32_t z) {
    for (int i = 0; i < BOOM_MAX; i++) if (!g_bo[i].alive) { g_bo[i] = (Boom){ x, y, z, 0, 1 }; return; }
}
static void spawn_enemy(Enemy *e) {
    uint32_t h = mix32(g_rng + (uint32_t)(e - g_en) * 131u + g_frame * 7u + 1u);
    int ang = (int)(h & 1023);
    uint32_t rf = mix32(h ^ 0x5bd1e995u) % (uint32_t)(SPAWN_MAX_C - SPAWN_MIN_C + 1);
    int32_t rad = U(SPAWN_MIN_C + (int32_t)rf);
    int32_t ex = g_px + mulq(rad, g_sin[ang]), ez = g_pz + mulq(rad, g_sin[(ang + 256) & 1023]);
    clamp_world(&ex, &ez);
    e->x = ex; e->z = ez; e->ang = (ang + 512) & 1023;   // nose roughly back toward the player
    e->alive = 1; e->state = 0; e->fireCD = 60 + (int)(h % 40u);
    e->patrolAng = e->ang; e->patrolT = 60;
}

// ---- input: one tank, either hand on the stick ------------------------------
static Input merge_input(const Input in[2]) {
    int x = in[0].x + in[1].x; if (x > 1) x = 1; if (x < -1) x = -1;
    int y = in[0].y + in[1].y; if (y > 1) y = 1; if (y < -1) y = -1;
    Input r; r.x = (int8_t)x; r.y = (int8_t)y;
    r.jump = (uint8_t)(in[0].jump | in[1].jump); r.act = (uint8_t)(in[0].act | in[1].act);
    return r;
}

static void reset_game(void) {
    g_px = 0; g_pz = 0; g_ang = 0;
    g_lives = LIVES_START; g_score = 0;
    g_fireCD = 0; g_invuln = 0; g_shake = 0; g_over = 0;
    g_frame = 0; g_rng = 0x9e3779b9u; g_checksum = 0;
    for (int i = 0; i < PSHOT_MAX; i++) g_ps[i].alive = 0;
    for (int i = 0; i < ESHOT_MAX; i++) g_es[i].alive = 0;
    for (int i = 0; i < BOOM_MAX; i++)  g_bo[i].alive = 0;
    for (int i = 0; i < ENEMY_MAX; i++) {
        g_en[i].alive = 0;
        g_en[i].spawnT = 20 + i * 25;   // staggered so the field doesn't arrive all at once
    }
}

static void init(void) {
    tables_init();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[P_SKY] = 0xFF030A07;
    ramp(P_TILEA,  0xFF1A5030);
    ramp(P_TILEB,  0xFF2C7A4A);
    ramp(P_MOUNT,  0xFF04190C);
    ramp(P_OBST,   0xFF1FAA55);
    ramp(P_SPIRE,  0xFF29C46A);
    ramp(P_ENEMY,  0xFF33FF66);
    ramp(P_TURRET, 0xFF22DD55);
    ramp(P_SHELL,  0xFFEFFFAA);
    ramp(P_ESHELL, 0xFFFF5533);
    ramp(P_FLASH,  0xFFFFF3C0);
    ramp(P_DEBRIS, 0xFFFF8844);
    g_pal[HUD_TXT]  = 0xFF33FF66;
    g_pal[HUD_DIM]  = 0xFF1A5A33;
    g_pal[HUD_WARN] = 0xFFFF4433;
    g_pal[HUD_BG]   = 0xFF07200E;
    g_pal[HUD_RING] = 0xFF1FAA55;
    g_pal[HUD_BLIP] = 0xFFFF5533;
    g_pal[HUD_SELF] = 0xFF33FF66;
    reset_game();
}

static void tick(const Input in[2]) {
    g_events = 0;
    Input p = merge_input(in);

    if (g_over) { if (p.jump) reset_game(); return; }

    g_frame++;
    g_rng = g_rng * 1664525u + 1013904223u;

    // ---- steer and drive: yaw is the whole camera, forward rides the same heading ----
    g_ang = (g_ang + p.x * TURNRATE) & 1023;
    int32_t fx = g_sin[g_ang], fz = g_sin[(g_ang + 256) & 1023];
    int32_t speed = p.y > 0 ? SPEED_FWD : (p.y < 0 ? -SPEED_BACK : 0);
    if (speed) {
        int32_t nx = g_px + mulq(speed, fx), nz = g_pz + mulq(speed, fz);
        resolve_obstacles(&nx, &nz, PLAYER_R);
        clamp_world(&nx, &nz);
        g_px = nx; g_pz = nz;
    }

    if (g_fireCD > 0) g_fireCD--;
    if (g_invuln > 0) g_invuln--;
    if (g_shake > 0) { g_shake -= (g_shake >> 2) + 1; if (g_shake < 0) g_shake = 0; }

    if (p.jump && g_fireCD == 0) {
        for (int i = 0; i < PSHOT_MAX; i++) if (!g_ps[i].alive) {
            g_ps[i] = (Shot){ g_px + mulq(U(180), fx), U(140), g_pz + mulq(U(180), fz),
                               g_ang, PSHOT_LIFE, 1 };
            g_fireCD = FIRE_COOLDOWN; g_events |= EV_FIRE;
            break;
        }
    }

    // ---- player shells ----
    for (int i = 0; i < PSHOT_MAX; i++) if (g_ps[i].alive) {
        Shot *s = &g_ps[i];
        int32_t sfx = g_sin[s->ang & 1023], sfz = g_sin[(s->ang + 256) & 1023];
        s->x += mulq(PSHOT_SPEED, sfx); s->z += mulq(PSHOT_SPEED, sfz);
        if (--s->life <= 0) { s->alive = 0; continue; }
        int gone = 0;
        for (int j = 0; j < OBST_N && !gone; j++)
            if (dist2(s->x, s->z, obst_x(j), obst_z(j)) < obst_r(j) + SHOT_R) {
                spawn_boom(s->x, U(200), s->z); gone = 1;
            }
        for (int j = 0; j < ENEMY_MAX && !gone; j++) {
            Enemy *e = &g_en[j];
            if (!e->alive) continue;
            if (dist2(s->x, s->z, e->x, e->z) < ENEMY_R + SHOT_R) {
                e->alive = 0; e->spawnT = RESPAWN_DELAY;
                g_score++; g_events |= EV_KILL;
                spawn_boom(e->x, U(150), e->z); gone = 1;
            }
        }
        if (gone) s->alive = 0;
    }

    // ---- enemy shells ----
    for (int i = 0; i < ESHOT_MAX; i++) if (g_es[i].alive) {
        Shot *s = &g_es[i];
        int32_t sfx = g_sin[s->ang & 1023], sfz = g_sin[(s->ang + 256) & 1023];
        s->x += mulq(ESHOT_SPEED, sfx); s->z += mulq(ESHOT_SPEED, sfz);
        if (--s->life <= 0) { s->alive = 0; continue; }
        if (g_invuln == 0 && dist2(s->x, s->z, g_px, g_pz) < PLAYER_R + SHOT_R) {
            s->alive = 0; g_lives--; g_invuln = INVULN_T; g_shake = SHAKE_HIT;
            g_events |= EV_HIT; spawn_boom(g_px, U(150), g_pz);
            if (g_lives <= 0) { g_over = 1; g_events |= EV_OVER; }
            continue;
        }
        for (int j = 0; j < OBST_N; j++)
            if (dist2(s->x, s->z, obst_x(j), obst_z(j)) < obst_r(j) + SHOT_R) {
                s->alive = 0; spawn_boom(s->x, U(200), s->z); break;
            }
    }

    // ---- enemies: patrol, engage, fire, ram ----
    for (int i = 0; i < ENEMY_MAX; i++) {
        Enemy *e = &g_en[i];
        if (!e->alive) {
            if (e->spawnT > 0) e->spawnT--; else spawn_enemy(e);
            continue;
        }
        int32_t dx = g_px - e->x, dz = g_pz - e->z;
        int32_t d = mag2(dx, dz);
        int32_t es = g_sin[e->ang & 1023], ec = g_sin[(e->ang + 256) & 1023];
        int32_t lx = mulq(dx, ec) - mulq(dz, es);   // player's position in the enemy's own frame

        if (e->state == 0) {                        // patrol
            if (d < DETECT_R) e->state = 1;
            else {
                if (--e->patrolT <= 0) {
                    uint32_t h = mix32(g_rng + (uint32_t)i * 7u + g_frame);
                    e->patrolAng = (int)(h & 1023); e->patrolT = 70 + (int)(h % 90u);
                }
                int diff = ((e->patrolAng - e->ang + 512) & 1023) - 512;
                int step = diff > ENEMY_TURN ? ENEMY_TURN : (diff < -ENEMY_TURN ? -ENEMY_TURN : diff);
                e->ang = (e->ang + step) & 1023;
                int32_t pfx = g_sin[e->ang & 1023], pfz = g_sin[(e->ang + 256) & 1023];
                int32_t nx = e->x + mulq(ENEMY_SPEED, pfx), nz = e->z + mulq(ENEMY_SPEED, pfz);
                resolve_obstacles(&nx, &nz, ENEMY_R); clamp_world(&nx, &nz);
                e->x = nx; e->z = nz;
            }
        } else {                                     // engage
            if (d > ENGAGE_LOSE_R) e->state = 0;
            else {
                if (lx > U(10)) e->ang = (e->ang + ENEMY_TURN) & 1023;
                else if (lx < -U(10)) e->ang = (e->ang - ENEMY_TURN) & 1023;
                int32_t efx = g_sin[e->ang & 1023], efz = g_sin[(e->ang + 256) & 1023];
                int32_t mv = d > ENGAGE_NEAR ? ENEMY_SPEED : 0;
                if (mv) {
                    int32_t nx = e->x + mulq(mv, efx), nz = e->z + mulq(mv, efz);
                    resolve_obstacles(&nx, &nz, ENEMY_R); clamp_world(&nx, &nz);
                    e->x = nx; e->z = nz;
                }
                if (e->fireCD > 0) e->fireCD--;
                else if (d < FIRE_R) {
                    int32_t absx = lx < 0 ? -lx : lx;
                    if (absx < d / 3 + U(50)) {
                        for (int k = 0; k < ESHOT_MAX; k++) if (!g_es[k].alive) {
                            g_es[k] = (Shot){ e->x + mulq(U(150), efx), U(140),
                                               e->z + mulq(U(150), efz), e->ang, ESHOT_LIFE, 1 };
                            e->fireCD = ECOOLDOWN_BASE + (int)(mix32(g_rng + (uint32_t)i * 3u + g_frame) % 60u);
                            g_events |= EV_EFIRE;
                            break;
                        }
                    }
                }
            }
        }
        if (e->alive && g_invuln == 0) {
            int32_t rd = mag2(g_px - e->x, g_pz - e->z);
            if (rd < PLAYER_R + ENEMY_R) {
                e->alive = 0; e->spawnT = RESPAWN_DELAY;
                g_lives--; g_invuln = INVULN_T; g_shake = SHAKE_RAM; g_events |= EV_RAM;
                spawn_boom((g_px + e->x) / 2, U(150), (g_pz + e->z) / 2);
                g_px -= mulq(RAM_PUSH, fx); g_pz -= mulq(RAM_PUSH, fz);
                if (g_lives <= 0) { g_over = 1; g_events |= EV_OVER; }
            }
        }
    }

    for (int i = 0; i < BOOM_MAX; i++) if (g_bo[i].alive) { if (++g_bo[i].age > BOOM_LIFE) g_bo[i].alive = 0; }

    g_checksum = g_checksum * 31 + (uint32_t)g_px + (uint32_t)g_pz * 7u
               + (uint32_t)g_ang * 13u + (uint32_t)g_lives * 101u + (uint32_t)g_score * 997u + g_frame;
    for (int i = 0; i < ENEMY_MAX; i++)
        g_checksum = g_checksum * 31 + (uint32_t)g_en[i].x + (uint32_t)g_en[i].z
                   + (uint32_t)g_en[i].ang + (uint32_t)g_en[i].alive * 31u;
}

static void audio(void) {
    if (g_events & EV_FIRE)  synth_note(NCHAN - 1, 5, 76, 150);
    if (g_events & EV_EFIRE) synth_note(NCHAN - 1, 5, 58, 130);
    if (g_events & EV_KILL)  synth_note(NCHAN - 1, 3, 40, 200);
    if (g_events & EV_HIT)   synth_note(NCHAN - 1, 4, 46, 210);
    if (g_events & EV_RAM)   synth_note(NCHAN - 1, 4, 32, 230);
    if (g_events & EV_OVER)  synth_note(NCHAN - 1, 3, 24, 220);
}

// ---- HUD text helpers ---------------------------------------------------------
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; digits(v, b);
    text_draw(x, y, sc, b, ci);
}
static void px_set(int x, int y, uint8_t ci) {
    if (x >= 0 && x < g_fbw && y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = ci;
}
static void px_box(int x0, int y0, int x1, int y1, uint8_t ci) {
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) px_set(x, y, ci);
}

// ---- draw -----------------------------------------------------------------
#define MAXINST 400

// 🔴 The renderer clips against the near plane and nothing else — there's no left/right
// frustum test, because every other cartridge in this tree only ever looks down a receding
// road where nothing sits close and to the side of the lens. A free-look tank standing next
// to its own obstacle field is the first camera here that CAN look almost broadside at
// something a few units away, and right at that angle a vertex's view-space z crosses
// through zero while its x stays large — the projector's divide doesn't clip that, it
// blows up, and one huge mis-projected triangle paints over the whole frame. Caught by
// turning three ticks in the debug build and watching the screen go solid; the fix belongs
// at the game layer, not the engine, so it lives here: don't hand the renderer anything
// whose centre isn't safely inside a generous forward cone.
static int32_t g_camx2, g_camz2, g_camfx, g_camfz;
static int visible_fwd(int32_t ox, int32_t oz, int32_t radius) {
    int32_t dx = ox - g_camx2, dz = oz - g_camz2;
    int32_t dist = mag2(dx, dz);
    if (dist < radius + U(300)) return 1;   // camera is basically on top of it either way
    int32_t fwd = mulq(dx, g_camfx) + mulq(dz, g_camfz);   // how far ahead, along the view axis
    return fwd > (dist >> 2);                              // inside a wide ~75-degree half-cone
}

static void draw(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    int horizon = g_fbh / 2;

    fb_clear(P_SKY);
    { int16_t gp[8] = { 0, (int16_t)horizon, (int16_t)g_fbw, (int16_t)horizon,
                         (int16_t)g_fbw, (int16_t)g_fbh, 0, (int16_t)g_fbh };
      poly_fill(gp, 4, P_TILEA); }

    nbox = 0;
    static Inst inst[MAXINST];
    int n = 0;

    g_camx2 = g_px; g_camz2 = g_pz;
    g_camfx = g_sin[g_ang & 1023]; g_camfz = g_sin[(g_ang + 256) & 1023];

    // ---- ground: a checkerboard near the player, one huge flat slab beyond it so the
    // horizon never shows a seam. Gaps between tiles are the grid lines — free geometry.
    int32_t tx0 = fdiv_floor(g_px, TILE), tz0 = fdiv_floor(g_pz, TILE);
    for (int j = -(GRID_N / 2); j < GRID_N / 2 && n < MAXINST - 20; j++)
        for (int i = -(GRID_N / 2); i < GRID_N / 2 && n < MAXINST - 20; i++) {
            int32_t tix = tx0 + i, tiz = tz0 + j;
            int32_t cx = tix * TILE + TILE / 2, cz = tiz * TILE + TILE / 2;
            uint8_t ci = (uint8_t)(((tix + tiz) & 1) ? P_TILEB : P_TILEA);
            inst[n].m = &bm[box(TILE / 2 - U(25), U(2), TILE / 2 - U(25), ci)];
            inst[n].pos = (V3){ cx, 0, cz }; inst[n].ax = inst[n].ay = inst[n].az = 0;
            inst[n].scale = 1 << 16; n++;
        }
    inst[n].m = &bm[box(U(600000), U(1), U(600000), P_TILEA)];
    inst[n].pos = (V3){ g_px, -U(3), g_pz }; inst[n].ax = inst[n].ay = inst[n].az = 0;
    inst[n].scale = 1 << 16; n++;

    // ---- the mountain ring: a fixed horizon, always there regardless of where you drive ----
    for (int i = 0; i < MOUNT_N && n < MAXINST; i++) {
        int a = mount_ang(i);
        int32_t mr = U(MOUNT_R_C);
        int32_t mx = mulq(mr, g_sin[a]), mz = mulq(mr, g_sin[(a + 256) & 1023]);
        int32_t hh = mount_h(i);
        if (!visible_fwd(mx, mz, U(35000))) continue;
        inst[n].m = &bm[pyramid(U(35000), hh, U(35000), P_MOUNT)];
        inst[n].pos = (V3){ mx, hh, mz }; inst[n].ax = inst[n].ay = inst[n].az = 0;
        inst[n].scale = 1 << 16; n++;
    }

    // ---- the obstacle field: obelisks and bunkers, always the same field, never rebuilt ----
    for (int i = 0; i < OBST_N && n < MAXINST; i++) {
        int32_t ox = obst_x(i), oz = obst_z(i), hh = obst_h(i);
        if (!visible_fwd(ox, oz, obst_r(i))) continue;
        int m = obst_kind(i) ? pyramid(obst_r(i), hh, obst_r(i), P_SPIRE)
                              : box(obst_r(i), hh, obst_r(i), P_OBST);
        inst[n].m = &bm[m]; inst[n].pos = (V3){ ox, hh, oz };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }

    // ---- enemy tanks: hull, turret, a barrel that points where it's aiming ----
    for (int i = 0; i < ENEMY_MAX && n < MAXINST - 4; i++) {
        Enemy *e = &g_en[i]; if (!e->alive) continue;
        if (!visible_fwd(e->x, e->z, U(250))) continue;
        int32_t hull_sy = U(90);
        int hm = wedge(U(150), hull_sy, U(220), P_ENEMY);
        inst[n].m = &bm[hm]; inst[n].pos = (V3){ e->x, hull_sy, e->z };
        inst[n].ax = 0; inst[n].ay = e->ang & 1023; inst[n].az = 0; inst[n].scale = 1 << 16; n++;

        int32_t tur_y = hull_sy * 2 + U(60);
        int tm = box(U(70), U(60), U(70), P_TURRET);
        inst[n].m = &bm[tm]; inst[n].pos = (V3){ e->x, tur_y, e->z };
        inst[n].ax = 0; inst[n].ay = e->ang & 1023; inst[n].az = 0; inst[n].scale = 1 << 16; n++;

        int32_t efx = g_sin[e->ang & 1023], efz = g_sin[(e->ang + 256) & 1023];
        int32_t bo = U(120), by = tur_y + U(60);
        int bm_ = box(U(15), U(15), U(90), P_TURRET);
        inst[n].m = &bm[bm_];
        inst[n].pos = (V3){ e->x + mulq(bo, efx), by, e->z + mulq(bo, efz) };
        inst[n].ax = 0; inst[n].ay = e->ang & 1023; inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }

    // ---- shells: tracers, oriented along their own flight ----
    for (int i = 0; i < PSHOT_MAX && n < MAXINST; i++) if (g_ps[i].alive && visible_fwd(g_ps[i].x, g_ps[i].z, U(90))) {
        Shot *s = &g_ps[i];
        inst[n].m = &bm[box(U(20), U(20), U(90), P_SHELL)];
        inst[n].pos = (V3){ s->x, s->y, s->z }; inst[n].ax = 0; inst[n].ay = s->ang & 1023;
        inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }
    for (int i = 0; i < ESHOT_MAX && n < MAXINST; i++) if (g_es[i].alive && visible_fwd(g_es[i].x, g_es[i].z, U(90))) {
        Shot *s = &g_es[i];
        inst[n].m = &bm[box(U(24), U(24), U(64), P_ESHELL)];
        inst[n].pos = (V3){ s->x, s->y, s->z }; inst[n].ax = 0; inst[n].ay = s->ang & 1023;
        inst[n].az = 0; inst[n].scale = 1 << 16; n++;
    }

    // ---- explosions: a flash that pops then thins, shards that fly and fall ----
    for (int i = 0; i < BOOM_MAX && n < MAXINST - 8; i++) if (g_bo[i].alive && visible_fwd(g_bo[i].x, g_bo[i].z, U(200))) {
        Boom *b = &g_bo[i];
        int32_t age = (int32_t)(((int64_t)b->age << 16) / BOOM_LIFE);   // 0..65536
        if (b->age < 6) {
            int32_t fl = U(80) - mul16(U(220), age); if (fl < U(10)) fl = U(10);
            inst[n].m = &bm[box(fl, fl, fl, P_FLASH)];
            inst[n].pos = (V3){ b->x, b->y, b->z }; inst[n].ax = inst[n].ay = inst[n].az = 0;
            inst[n].scale = 1 << 16; n++;
        }
        for (int s = 0; s < 6 && n < MAXINST; s++) {
            uint32_t h = mix32((uint32_t)(i * 37 + s) ^ (uint32_t)(b->x + b->z));
            int ang = (int)(h & 1023);
            int32_t rad = mul16(U(120), age);
            int32_t sx = b->x + mulq(rad, g_sin[ang]), sz = b->z + mulq(rad, g_sin[(ang + 256) & 1023]);
            int32_t sy = mul16(U(140), age) - mul16(mul16(U(180), age), age);
            if (sy < 0) sy = 0;
            inst[n].m = &bm[box(U(8), U(8), U(8), P_DEBRIS)];
            inst[n].pos = (V3){ sx, b->y + sy, sz };
            inst[n].ax = (int)(h & 1023); inst[n].ay = (int)((h >> 10) & 1023); inst[n].az = 0;
            inst[n].scale = 1 << 16; n++;
        }
    }

    // ---- the camera IS the tank ----
    int32_t camx = g_px, camy = EYE_H, camz = g_pz;
    int camax = 0, camay = g_ang, camaz = 0;
    if (g_shake > 0) {
        uint32_t r = mix32(g_frame * 2654435761u + 7u);
        camx += (int32_t)((((int64_t)((int)(r & 511) - 256) * g_shake)) >> 9);
        camy += (int32_t)((((int64_t)((int)((r >> 9) & 511) - 256) * g_shake)) >> 10);
        camay += (int)((((int64_t)((int)((r >> 18) & 255) - 128) * g_shake) >> 15));
    }
    Cam cam = { { camx, camy, camz }, camax, camay & 1023, camaz };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    // ---- HUD: lives, score, a crosshair, a radar that turns with you ----
    text_draw(6 * sc, hud_top(), sc, "LIVES", HUD_TXT);
    num(6 * sc + text_width("LIVES", sc) + 8 * sc, hud_top(), sc, g_lives, (uint8_t)(g_lives <= 1 ? HUD_WARN : HUD_TXT));
    text_draw(6 * sc, hud_top() + 10 * sc, sc, "SCORE", HUD_DIM);
    num(6 * sc + text_width("SCORE", sc) + 8 * sc, hud_top() + 10 * sc, sc, g_score, HUD_DIM);
    if (g_invuln > 0 && ((g_frame / 4) & 1))
        text_draw(g_fbw / 2 - text_width("HIT", sc) / 2, hud_top(), sc, "HIT", HUD_WARN);

    // crosshair
    int ccx = g_fbw / 2, ccy = g_fbh / 2;
    px_box(ccx - 4 * sc, ccy - sc, ccx - sc, ccy + sc, HUD_DIM);
    px_box(ccx + sc, ccy - sc, ccx + 4 * sc, ccy + sc, HUD_DIM);
    px_box(ccx - sc, ccy - 4 * sc, ccx + sc, ccy - sc, HUD_DIM);
    px_box(ccx - sc, ccy + sc, ccx + sc, ccy + 4 * sc, HUD_DIM);

    // radar: bottom right, forward is always "up" — it turns with the tank, not the world
    int rs = 46 * sc;
    int rx = g_fbw - rs - 8 * sc, ry = g_fbh - rs - 8 * sc;
    int rcx = rx + rs / 2, rcy = ry + rs / 2;
    px_box(rx, ry, rx + rs, ry + rs, HUD_BG);
    for (int t = 0; t < rs; t++) { px_set(rx + t, ry, HUD_RING); px_set(rx + t, ry + rs - 1, HUD_RING);
                                    px_set(rx, ry + t, HUD_RING); px_set(rx + rs - 1, ry + t, HUD_RING); }
    px_box(rcx - sc, rcy - sc, rcx + sc, rcy + sc, HUD_SELF);
    int32_t rrange = U(50000);
    int32_t s_ = g_sin[g_ang & 1023], c_ = g_sin[(g_ang + 256) & 1023];
    for (int i = 0; i < ENEMY_MAX; i++) {
        Enemy *e = &g_en[i]; if (!e->alive) continue;
        int32_t dx = e->x - g_px, dz = e->z - g_pz;
        int32_t lx = mulq(dx, c_) - mulq(dz, s_), lz = mulq(dx, s_) + mulq(dz, c_);
        int32_t d = mag2(lx, lz); if (d < 1) d = 1;
        int32_t clampd = d > rrange ? rrange : d;
        int32_t px = (int32_t)(((int64_t)lx * clampd / d) * (rs / 2 - 3 * sc) / rrange);
        int32_t py = (int32_t)(((int64_t)lz * clampd / d) * (rs / 2 - 3 * sc) / rrange);
        int bx = rcx + px, by = rcy - py;
        px_box(bx - sc, by - sc, bx + sc, by + sc, HUD_BLIP);
    }

    if (g_over) {
        int cx = g_fbw / 2, cy = g_fbh / 2;
        px_box(0, cy - 26 * sc, g_fbw, cy + 30 * sc, HUD_BG);
        text_draw(cx - text_width("DESTROYED", sc * 3) / 2, cy - 20 * sc, sc * 3, "DESTROYED", HUD_WARN);
        text_draw(cx - text_width("SCORE", sc) / 2 - 20 * sc, cy + 6 * sc, sc, "SCORE", HUD_TXT);
        num(cx + 10 * sc, cy + 6 * sc, sc, g_score, HUD_TXT);
        text_draw(cx - text_width("SPACE TO RESTART", sc) / 2, cy + 18 * sc, sc, "SPACE TO RESTART", HUD_DIM);
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_vectank = { "vectank", init, tick, audio, draw, checksum };
