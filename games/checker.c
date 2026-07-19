// checker.c — a filled-polygon checkerboard, the way the first filled-3D arcade game
// (I,Robot, 1984) had to prove itself: no bitmaps, a field of solid tiles receding under
// an oblique camera, and something at the far end that watches.
//
// The whole game is one rule stacked on itself. Hop onto a tile and it flips; flip every
// tile and the field clears. A giant eye at the horizon opens on its own clock — while
// it's open, moving at all draws its attention (a life lost); while it's shut, the board
// is yours. Space doesn't move you, so it's always safe: fire it at the eye's mounted
// sentries to clear a path onto the tiles they're guarding.
//
// Pure hop-and-freeze, watched by something that isn't a wall or a pit — a rule that
// looks back.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t lerp32(int32_t a, int32_t b, int t /*0..1024*/) {
    return a + (int32_t)(((int64_t)(b - a) * t) >> 10);
}
static int clamp01(int t) { return t < 0 ? 0 : (t > 1024 ? 1024 : t); }

// ---- the board ----------------------------------------------------------------
#define GRID_W 7
#define GRID_D 9
#define PITCH  U                         // one unit between tile centres
#define ROW0_Z (U * 2)                   // nearest row sits 2 units ahead of the origin
#define TILE_HX (U * 44 / 100)           // tile top half-extent — leaves a grout gap
#define TILE_HZ (U * 44 / 100)
#define TILE_HY (U * 6 / 100)

static int32_t tile_x(int c) { return (c - GRID_W / 2) * PITCH; }
static int32_t tile_z(int r) { return ROW0_Z + r * PITCH; }

#define HAZ_N 3
static const int8_t HAZ_COL[HAZ_N] = { 1, 5, 3 };
static const int8_t HAZ_ROW[HAZ_N] = { 3, 3, 6 };

// ---- palette bases (each a full 8-shade ramp; base must be a multiple of 8) ----
enum {
    PAL_GND = 8, PAL_TILE_OFF = 16, PAL_TILE_ON = 24, PAL_PLAYER = 32, PAL_PLAYER_FLASH = 40,
    PAL_HAZ = 48, PAL_SCLERA = 56, PAL_PUPIL_SAFE = 64, PAL_PUPIL_DANGER = 72,
    PAL_WALL = 80, PAL_BOLT = 88,
};

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 50 + i * 27;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ---- box mesh helper (outward normals, the shared box helper) ----------------
static const int8_t BOX_VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                     {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t BOX_FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t BOX_FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static void box_verts(V3 v[8], int32_t hx, int32_t hy, int32_t hz) {
    for (int k = 0; k < 8; k++) {
        v[k].x = BOX_VP[k][0] * hx; v[k].y = BOX_VP[k][1] * hy; v[k].z = BOX_VP[k][2] * hz;
    }
}
static void box_tris(Tri t[12], uint8_t ci) {
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *tt = &t[f * 2 + k];
            tt->a = BOX_FQ[f][0]; tt->b = BOX_FQ[f][1 + k]; tt->c = BOX_FQ[f][2 + k];
            tt->ci = ci;
            tt->nx = (int16_t)(BOX_FN[f][0] * 32767);
            tt->ny = (int16_t)(BOX_FN[f][1] * 32767);
            tt->nz = (int16_t)(BOX_FN[f][2] * 32767);
        }
}
static void build_box(V3 v[8], Tri t[12], int32_t hx, int32_t hy, int32_t hz, uint8_t ci) {
    box_verts(v, hx, hy, hz); box_tris(t, ci);
}

// ---- geometry: one shared vertex set per shape, 2 colour variants where needed ----
static V3  gnd_v[8];  static Tri gnd_t[12];  static Mesh gnd_m;
static V3  wall_v[8]; static Tri wall_t[12]; static Mesh wall_m;

static V3  tile_v[8];
static Tri tile_off_t[12], tile_on_t[12];
static Mesh tile_off_m, tile_on_m;

static V3  haz_v[8];  static Tri haz_t[12];  static Mesh haz_m;
static V3  bolt_v[8]; static Tri bolt_t[12]; static Mesh bolt_m;

static V3  torso_v[8]; static Tri torso_n_t[12], torso_f_t[12]; static Mesh torso_n_m, torso_f_m;
static V3  head_v[8];  static Tri head_n_t[12],  head_f_t[12];  static Mesh head_n_m,  head_f_m;

// ---- a low-poly sphere for the eye: one vertex set, two colour variants -------
#define SPH_RINGS 8
#define SPH_SEGS  10
#define SPH_NV ((SPH_RINGS + 1) * SPH_SEGS)
#define SPH_NT (SPH_RINGS * SPH_SEGS * 2)
static V3  sph_v[SPH_NV];
static Tri sph_sclera_t[SPH_NT], sph_pupil_t[SPH_NT];
static Mesh sph_sclera_m, sph_pupil_m;

static int64_t isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

// A unit sphere (radius 1<<16, so Inst.scale doubles as the world radius directly).
static void build_sphere(void) {
    for (int row = 0; row <= SPH_RINGS; row++) {
        int lat = -256 + row * 512 / SPH_RINGS;          // table units: -256..+256 = -90..+90 deg
        int32_t sy = g_sin[lat & 1023], cy = g_sin[(lat + 256) & 1023];
        int32_t horiz = mul15(U, cy);
        for (int col = 0; col < SPH_SEGS; col++) {
            int lon = col * 1024 / SPH_SEGS;
            int32_t sx = g_sin[lon & 1023], cx = g_sin[(lon + 256) & 1023];
            V3 *p = &sph_v[row * SPH_SEGS + col];
            p->x = mul15(horiz, sx); p->y = mul15(U, sy); p->z = mul15(horiz, cx);
        }
    }
    int n = 0;
    for (int row = 0; row < SPH_RINGS; row++)
        for (int col = 0; col < SPH_SEGS; col++) {
            int a = row * SPH_SEGS + col, b = row * SPH_SEGS + (col + 1) % SPH_SEGS;
            int c = (row + 1) * SPH_SEGS + (col + 1) % SPH_SEGS, d = (row + 1) * SPH_SEGS + col;
            int tri[2][3] = { { a, b, c }, { a, c, d } };
            for (int k = 0; k < 2; k++) {
                int ia = tri[k][0], ib = tri[k][1], ic = tri[k][2];
                V3 A = sph_v[ia], B = sph_v[ib], C = sph_v[ic];
                int64_t e1x = B.x - A.x, e1y = B.y - A.y, e1z = B.z - A.z;
                int64_t e2x = C.x - A.x, e2y = C.y - A.y, e2z = C.z - A.z;
                int64_t cxp = (e1y * e2z - e1z * e2y) >> 24;
                int64_t cyp = (e1z * e2x - e1x * e2z) >> 24;
                int64_t czp = (e1x * e2y - e1y * e2x) >> 24;
                int64_t mag = isqrt64(cxp * cxp + cyp * cyp + czp * czp);
                int16_t nx = 0, ny = 0, nz = 0;
                if (mag > 0) {
                    nx = (int16_t)(cxp * 32767 / mag); ny = (int16_t)(cyp * 32767 / mag); nz = (int16_t)(czp * 32767 / mag);
                    // Sphere is centred on the origin, so A itself is a valid outward reference.
                    int64_t dot = (int64_t)nx * A.x + (int64_t)ny * A.y + (int64_t)nz * A.z;
                    if (dot < 0) { nx = (int16_t)-nx; ny = (int16_t)-ny; nz = (int16_t)-nz; }
                }
                sph_sclera_t[n].a = (uint16_t)ia; sph_sclera_t[n].b = (uint16_t)ib; sph_sclera_t[n].c = (uint16_t)ic;
                sph_sclera_t[n].nx = nx; sph_sclera_t[n].ny = ny; sph_sclera_t[n].nz = nz; sph_sclera_t[n].ci = PAL_SCLERA;
                sph_pupil_t[n] = sph_sclera_t[n]; sph_pupil_t[n].ci = PAL_PUPIL_SAFE;
                n++;
            }
        }
    sph_sclera_m.v = sph_v; sph_sclera_m.nv = SPH_NV; sph_sclera_m.t = sph_sclera_t; sph_sclera_m.nt = SPH_NT;
    sph_pupil_m.v  = sph_v; sph_pupil_m.nv  = SPH_NV; sph_pupil_m.t  = sph_pupil_t;  sph_pupil_m.nt  = SPH_NT;
}

static void build_meshes(void) {
    build_box(gnd_v, gnd_t, PITCH * GRID_W * 2 / 3, U * 4 / 100, PITCH * (GRID_D + 3) / 2, PAL_GND);
    gnd_m = (Mesh){ gnd_v, 8, gnd_t, 12 };

    build_box(wall_v, wall_t, PITCH * GRID_W * 65 / 100, U * 12 / 5, U * 10 / 100, PAL_WALL);
    wall_m = (Mesh){ wall_v, 8, wall_t, 12 };

    box_verts(tile_v, TILE_HX, TILE_HY, TILE_HZ);
    box_tris(tile_off_t, PAL_TILE_OFF); box_tris(tile_on_t, PAL_TILE_ON);
    tile_off_m = (Mesh){ tile_v, 8, tile_off_t, 12 };
    tile_on_m  = (Mesh){ tile_v, 8, tile_on_t,  12 };

    build_box(haz_v, haz_t, U * 20 / 100, U * 20 / 100, U * 20 / 100, PAL_HAZ);
    haz_m = (Mesh){ haz_v, 8, haz_t, 12 };

    build_box(bolt_v, bolt_t, U * 8 / 100, U * 8 / 100, U * 16 / 100, PAL_BOLT);
    bolt_m = (Mesh){ bolt_v, 8, bolt_t, 12 };

    box_verts(torso_v, U * 22 / 100, U * 26 / 100, U * 16 / 100);
    box_tris(torso_n_t, PAL_PLAYER); box_tris(torso_f_t, PAL_PLAYER_FLASH);
    torso_n_m = (Mesh){ torso_v, 8, torso_n_t, 12 };
    torso_f_m = (Mesh){ torso_v, 8, torso_f_t, 12 };

    box_verts(head_v, U * 14 / 100, U * 13 / 100, U * 14 / 100);
    box_tris(head_n_t, PAL_PLAYER); box_tris(head_f_t, PAL_PLAYER_FLASH);
    head_n_m = (Mesh){ head_v, 8, head_n_t, 12 };
    head_f_m = (Mesh){ head_v, 8, head_f_t, 12 };

    build_sphere();
}

// ---- eye clock: closed -> telegraph open -> DANGER -> telegraph close -> repeat ----
#define EYE_CLOSED   130
#define EYE_TELE     18
#define EYE_DANGER   66
#define EYE_PERIOD   (EYE_CLOSED + EYE_TELE + EYE_DANGER + EYE_TELE)

#define HOP_FRAMES   9
#define MOVE_COOL    11
#define ZAP_FLASH    24
#define MAX_LIVES    3

enum { FACE_E, FACE_W, FACE_N, FACE_S };

typedef struct {
    uint32_t frame;
    int      pcol, prow;              // player's tile
    int      from_col, from_row;      // hop animation endpoints
    int      hop_t;                   // counts down HOP_FRAMES..0
    int      facing;
    int      cool;
    int      lives;
    int      zap_t;
    int      flips_left;
    uint8_t  flipped[GRID_D][GRID_W];
    uint8_t  haz_alive[HAZ_N];
    int      bolt_active;
    int32_t  bolt_x, bolt_z;
    int      bolt_dir;
    int      over;                    // 0 playing, 1 dead, 2 won
    uint64_t seed;                    // folded state, for checksum only
} St;
static St g;

static int haz_at(int c, int r) {
    for (int i = 0; i < HAZ_N; i++)
        if (g.haz_alive[i] && HAZ_COL[i] == c && HAZ_ROW[i] == r) return i;
    return -1;
}

static void flip_tile(int c, int r) {
    if (!g.flipped[r][c]) { g.flipped[r][c] = 1; g.flips_left--; }
}

static void eye_phase(uint32_t frame, int *danger, int *pupil_open /*0..1024*/) {
    int t = (int)(frame % EYE_PERIOD);
    if (t < EYE_CLOSED) { *danger = 0; *pupil_open = 0; return; }
    t -= EYE_CLOSED;
    if (t < EYE_TELE) { *danger = 0; *pupil_open = clamp01(t * 1024 / EYE_TELE); return; }
    t -= EYE_TELE;
    if (t < EYE_DANGER) { *danger = 1; *pupil_open = 1024; return; }
    t -= EYE_DANGER;
    *danger = 0; *pupil_open = clamp01(1024 - t * 1024 / EYE_TELE);
}

static uint8_t g_events;
#define EV_HOP   1
#define EV_FLIP  2
#define EV_ZAP   4
#define EV_BUMP  8
#define EV_SHOT  16
#define EV_HIT   32
#define EV_WIN   64
#define EV_OVER  128

static void reset_run(void) {
    g.frame = 0;
    g.pcol = GRID_W / 2; g.prow = 0;
    g.from_col = g.pcol; g.from_row = g.prow; g.hop_t = 0;
    g.facing = FACE_N;
    g.cool = 0;
    g.lives = MAX_LIVES;
    g.zap_t = 0;
    for (int r = 0; r < GRID_D; r++) for (int c = 0; c < GRID_W; c++) g.flipped[r][c] = 0;
    g.flips_left = GRID_W * GRID_D;
    for (int i = 0; i < HAZ_N; i++) g.haz_alive[i] = 1;
    g.bolt_active = 0; g.bolt_dir = FACE_N;
    g.over = 0;
    flip_tile(g.pcol, g.prow);
    g.seed = 0;
}

static void init(void) {
    tables_init();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0A0B14;
    g_pal[1] = 0xFF8A8FA8;   // HUD label grey
    g_pal[2] = 0xFFF5F5F8;   // HUD value / highlight white
    g_pal[3] = 0xFFE8342E;   // HUD warning red (low lives)
    ramp(PAL_GND, 0xFF1B1E33);
    ramp(PAL_TILE_OFF, 0xFFB5401E);
    ramp(PAL_TILE_ON,  0xFF31D6A0);
    ramp(PAL_PLAYER, 0xFF4FA8FF);
    ramp(PAL_PLAYER_FLASH, 0xFFF5F5F8);
    ramp(PAL_HAZ, 0xFFE8342E);
    ramp(PAL_SCLERA, 0xFFECE6D8);
    ramp(PAL_PUPIL_SAFE, 0xFF16141C);
    ramp(PAL_PUPIL_DANGER, 0xFFFF3B1F);
    ramp(PAL_WALL, 0xFF2A2438);
    ramp(PAL_BOLT, 0xFFFFE24A);
    build_meshes();
    reset_run();
}

static void tick(const Input in[2]) {
    g_events = 0;
    Input p = in[0];

    if (g.over) {
        if (p.jump) { reset_run(); }
        g.frame++;
        return;
    }

    if (g.hop_t > 0) g.hop_t--;
    if (g.zap_t > 0) g.zap_t--;

    int danger, pop;
    eye_phase(g.frame, &danger, &pop);

    // The bolt: single shot in flight, moves a fixed step per tick along its facing.
    if (g.bolt_active) {
        int32_t step = U * 55 / 100;
        switch (g.bolt_dir) {
            case FACE_E: g.bolt_x += step; break;
            case FACE_W: g.bolt_x -= step; break;
            case FACE_N: g.bolt_z += step; break;
            default:     g.bolt_z -= step; break;
        }
        int bc = (int)((g.bolt_x / PITCH) + GRID_W / 2);
        int br = (int)((g.bolt_z - ROW0_Z) / PITCH);
        int hz = (bc >= 0 && bc < GRID_W && br >= 0 && br < GRID_D) ? haz_at(bc, br) : -1;
        if (hz >= 0) { g.haz_alive[hz] = 0; g.bolt_active = 0; g_events |= EV_HIT; }
        else if (g.bolt_z > tile_z(GRID_D - 1) + PITCH * 2 || g.bolt_z < ROW0_Z - PITCH
                 || g.bolt_x > tile_x(GRID_W - 1) + PITCH || g.bolt_x < tile_x(0) - PITCH)
            g.bolt_active = 0;
    }
    if (p.jump && !g.bolt_active) {
        g.bolt_active = 1; g.bolt_dir = g.facing;
        g.bolt_x = tile_x(g.pcol); g.bolt_z = tile_z(g.prow);
        g_events |= EV_SHOT;
    }

    if (g.cool > 0) { g.cool--; }
    else {
        int ax = p.x < 0 ? -p.x : p.x, ay = p.y < 0 ? -p.y : p.y;
        int dcol = 0, drow = 0;
        if (ax || ay) {
            if (ax >= ay) { dcol = p.x > 0 ? 1 : -1; g.facing = dcol > 0 ? FACE_E : FACE_W; }
            else          { drow = p.y > 0 ? 1 : -1; g.facing = drow > 0 ? FACE_N : FACE_S; }
        }
        if (dcol || drow) {
            g.cool = MOVE_COOL;
            if (danger) {
                if (g.lives > 0) { g.lives--; g.zap_t = ZAP_FLASH; g_events |= EV_ZAP;
                    if (g.lives == 0) { g.over = 1; g_events |= EV_OVER; } }
            } else {
                int nc = g.pcol + dcol, nr = g.prow + drow;
                if (nc >= 0 && nc < GRID_W && nr >= 0 && nr < GRID_D && haz_at(nc, nr) < 0) {
                    g.from_col = g.pcol; g.from_row = g.prow;
                    g.pcol = nc; g.prow = nr; g.hop_t = HOP_FRAMES;
                    g_events |= EV_HOP;
                    if (!g.flipped[nr][nc]) { flip_tile(nc, nr); g_events |= EV_FLIP; }
                    if (g.flips_left == 0) { g.over = 2; g_events |= EV_WIN; }
                } else g_events |= EV_BUMP;
            }
        }
    }

    g.seed = g.seed * 1664525u + 1013904223u + (uint32_t)(g.pcol * 7 + g.prow * 13);
    g.frame++;
}

static void audio(void) {
    if (g_events & EV_HOP)   synth_note(NCHAN - 1, 3, 64, 90);
    if (g_events & EV_FLIP)  synth_note(NCHAN - 1, 4, 76, 120);
    if (g_events & EV_SHOT)  synth_note(NCHAN - 1, 5, 88, 100);
    if (g_events & EV_HIT)   synth_note(NCHAN - 1, 3, 52, 160);
    if (g_events & EV_BUMP)  synth_note(NCHAN - 1, 3, 40, 80);
    if (g_events & EV_ZAP)   synth_note(NCHAN - 1, 3, 30, 220);
    if (g_events & EV_WIN)   synth_note(NCHAN - 1, 4, 96, 200);
    if (g_events & EV_OVER)  synth_note(NCHAN - 1, 3, 24, 220);
}

// ---- draw -----------------------------------------------------------------
#define MAXI (2 + 1 + GRID_W * GRID_D + HAZ_N + 1 + 3)
static Inst inst[MAXI];

static void draw(void) {
    fb_clear(0);

    int danger, pop;
    eye_phase(g.over ? 0 : g.frame, &danger, &pop);
    if (g.over) { danger = 0; pop = 0; }

    int n = 0;

    inst[n].m = &gnd_m;
    inst[n].pos = (V3){ 0, -(U * 4 / 100) - U / 40, tile_z((GRID_D - 1) / 2) };
    inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;

    int32_t eye_z = tile_z(GRID_D - 1) + PITCH * 2;
    inst[n].m = &wall_m;
    inst[n].pos = (V3){ 0, U * 12 / 10, eye_z + U * 40 / 100 };
    inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;

    // tiles
    for (int r = 0; r < GRID_D; r++)
        for (int c = 0; c < GRID_W; c++) {
            inst[n].m = g.flipped[r][c] ? &tile_on_m : &tile_off_m;
            inst[n].pos = (V3){ tile_x(c), -TILE_HY, tile_z(r) };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
        }

    // hazards — a slow spin so they read as live sentries, not scenery
    for (int i = 0; i < HAZ_N; i++) if (g.haz_alive[i]) {
        inst[n].m = &haz_m;
        inst[n].pos = (V3){ tile_x(HAZ_COL[i]), U * 22 / 100, tile_z(HAZ_ROW[i]) };
        inst[n].ax = 0; inst[n].ay = (int)((g.frame * 6) & 1023); inst[n].az = 0;
        inst[n].scale = U; n++;
    }

    // the eye: sclera fixed, pupil scales with pop and turns red once fully open
    inst[n].m = &sph_sclera_m;
    inst[n].pos = (V3){ 0, U * 15 / 10, eye_z };
    inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U * 11 / 10; n++;

    if (pop > 8) {
        for (int i = 0; i < SPH_NT; i++) sph_pupil_t[i].ci = danger ? PAL_PUPIL_DANGER : PAL_PUPIL_SAFE;
        int32_t pr = (int32_t)((int64_t)(U * 42 / 100) * pop >> 10);
        inst[n].m = &sph_pupil_m;
        inst[n].pos = (V3){ 0, U * 15 / 10, eye_z - U * 105 / 100 };
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = pr; n++;
    }

    // the player, hopping between tiles with a small arc
    int32_t px, pz, ph = 0;
    if (g.hop_t > 0) {
        int t = clamp01(1024 - g.hop_t * 1024 / HOP_FRAMES);
        px = lerp32(tile_x(g.from_col), tile_x(g.pcol), t);
        pz = lerp32(tile_z(g.from_row), tile_z(g.prow), t);
        int arc = t < 512 ? t : 1024 - t;                      // 0..512..0
        ph = (int32_t)((int64_t)(U * 22 / 100) * arc >> 9);
    } else { px = tile_x(g.pcol); pz = tile_z(g.prow); }
    int ay = g.facing == FACE_E ? 256 : g.facing == FACE_W ? 768 : g.facing == FACE_N ? 0 : 512;
    int flash = (g.zap_t > 0) && ((g.frame >> 1) & 1);

    inst[n].m = flash ? &torso_f_m : &torso_n_m;
    inst[n].pos = (V3){ px, U * 26 / 100 + ph, pz };
    inst[n].ax = 0; inst[n].ay = ay; inst[n].az = 0; inst[n].scale = U; n++;
    inst[n].m = flash ? &head_f_m : &head_n_m;
    inst[n].pos = (V3){ px, U * 52 / 100 + ph, pz };
    inst[n].ax = 0; inst[n].ay = ay; inst[n].az = 0; inst[n].scale = U; n++;

    if (g.bolt_active) {
        inst[n].m = &bolt_m;
        inst[n].pos = (V3){ g.bolt_x, U * 18 / 100, g.bolt_z };
        inst[n].ax = 0; inst[n].ay = (g.bolt_dir == FACE_E || g.bolt_dir == FACE_W) ? 256 : 0; inst[n].az = 0;
        inst[n].scale = U; n++;
    }

    // camera: high, behind row 0, pitched down at the board — the oblique arcade look.
    Cam cam = { { 0, U * 46 / 10, -(U * 40 / 10) }, 90, 0, 0 };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    // ---- HUD --------------------------------------------------------------
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "LIVES", 1);
    char lb[4]; lb[0] = (char)('0' + (g.lives < 0 ? 0 : g.lives)); lb[1] = 0;
    text_draw(6 * sc + 36 * sc, hud_top(), sc, lb, g.lives > 1 ? 2 : 3);

    char b[8]; int v = g.flips_left, bi = 0; if (v <= 0) b[bi++] = '0';
    while (v > 0 && bi < 6) { b[bi++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < bi / 2; j++) { char t = b[j]; b[j] = b[bi - 1 - j]; b[bi - 1 - j] = t; }
    b[bi] = 0;
    text_draw(g_fbw - 90 * sc, hud_top(), sc, "LEFT", 1);
    text_draw(g_fbw - 40 * sc, hud_top(), sc, b, 2);

    if (danger && !g.over) {
        const char *msg = "EYE OPEN - FREEZE";
        text_draw(g_fbw / 2 - text_width(msg, sc) / 2, hud_top(), sc, msg, (uint8_t)(PAL_PUPIL_DANGER + 4));
    }
    if (g.over == 1) {
        const char *m1 = "CAUGHT LOOKING";
        const char *m2 = "SPACE TO RETRY";
        text_draw(g_fbw / 2 - text_width(m1, sc * 2) / 2, g_fbh / 2 - 14 * sc, sc * 2, m1, 2);
        text_draw(g_fbw / 2 - text_width(m2, sc) / 2, g_fbh / 2 + 14 * sc, sc, m2, 1);
    } else if (g.over == 2) {
        const char *m1 = "FIELD CLEARED";
        const char *m2 = "SPACE FOR ANOTHER";
        text_draw(g_fbw / 2 - text_width(m1, sc * 2) / 2, g_fbh / 2 - 14 * sc, sc * 2, m1, 2);
        text_draw(g_fbw / 2 - text_width(m2, sc) / 2, g_fbh / 2 + 14 * sc, sc, m2, 1);
    }
}

static uint64_t checksum(void) {
    uint64_t h = g.seed;
    h = h * 31 + g.frame; h = h * 31 + (uint32_t)g.pcol; h = h * 31 + (uint32_t)g.prow;
    h = h * 31 + (uint32_t)g.lives; h = h * 31 + (uint32_t)g.flips_left; h = h * 31 + (uint32_t)g.over;
    for (int i = 0; i < HAZ_N; i++) h = h * 31 + g.haz_alive[i];
    return h;
}

const Game game_checker = { "checker", init, tick, audio, draw, checksum };
