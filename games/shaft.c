// shaft.c — a falling-block well, but the well is a real place.
//
// Blockout asked its question once and then answered it flat: the shaft was drawn as a
// stack of floor plans, one layer at a time, because a CGA screen couldn't afford
// perspective. The question survives without the excuse. Point a camera down a real
// shaft and the depth stops being implied by a floor-plan convention and starts being
// something the lens does on its own — the top layer is close and bright, the ninth
// layer is small and dim, and a piece falling away from you visibly recedes instead of
// just changing which floor-plan you're looking at.
//
// So: one well, four corner pillars holding it up in perspective, a rim marking the
// mouth you drop pieces into, and eight polycube pieces that are honestly 3D — several
// of them span two layers of the shaft, which a flat tetromino never has to.
//
// Rotation is three fixed WORLD axes, not the piece's own — press "roll" and the piece
// always turns around the same axis regardless of which way it's currently facing. Less
// clever than body-relative rotation, more predictable, and predictable is what a puzzle
// needs from its controls.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- the shaft ----------------------------------------------------------------
#define WELL_W 4      // cross-section, x
#define WELL_D 4      // cross-section, z
#define WELL_H 10     // depth, layers
// A piece's cells sit in {-1,0,1} of its spawn cell, so the spawn column has to be at
// least 1 cell clear of every wall — the middle of a 4-wide shaft is the only place
// that's true on both axes.
#define CENTER_X ((WELL_W - 1) / 2)
#define CENTER_Z ((WELL_D - 1) / 2)
#define SPAWN_GY 1    // pieces can span gy-1..gy+1; spawning at 1 keeps that inside [0,H)
#define CELLU (1 << 16)

// Palette: every Tri.ci is a BASE that the renderer adds 0..7 of shade to, so every
// distinct colour needs its own base 8 apart or two colours' shade ramps overlap and
// paint each other's blocks. Bit me once already: the floor came out the piece's orange.
#define PAL_PILLAR 8
#define PAL_RIM    16
#define PAL_FLOOR  24
#define PAL_TYPE0  32          // 8 piece types x 8 shades = 32..95
#define PAL_FLASH  96

// ---- pieces ---------------------------------------------------------------------
// Local offsets, each component in {-1,0,1} — small enough that a 90-degree rotation
// matrix (entries only ever -1/0/1) can never carry a cell out of that range, so there's
// no bounding-box recompute after a turn, ever.
#define NPT 8
static const int8_t PIECE_CELLS[NPT][4][3] = {
    { {0,0,0}, {1,0,0}, {0,0,1}, {0,0,0} },              // L3          (flat corner)
    { {-1,0,0},{0,0,0}, {1,0,0}, {0,0,0} },              // I3 along x
    { {0,0,-1},{0,0,0}, {0,0,1}, {0,0,0} },              // I3 along z
    { {0,0,0}, {1,0,0}, {0,1,0}, {0,0,0} },              // corner3     (steps down a layer)
    { {-1,0,0},{0,0,0}, {0,1,0}, {1,1,0} },              // zigzag4     (spans two layers)
    { {0,0,0}, {1,0,0}, {0,0,1}, {1,0,1} },              // square4     (2x2, one layer)
    { {0,0,0}, {0,1,0}, {0,-1,0},{1,0,0} },              // tower4      (spans three layers)
    { {-1,0,0},{0,0,0}, {1,0,0}, {0,0,1} },              // tflat4
};
static const int8_t PIECE_N[NPT] = { 3, 3, 3, 3, 4, 4, 4, 4 };
static const uint32_t PIECE_RGB[NPT] = {
    0xFFFF9519, 0xFF29ADFF, 0xFFE64AC9, 0xFFFFD62E,
    0xFF33C759, 0xFF3355FF, 0xFFF23A35, 0xFFB15AFF,
};

// ---- rotation: three fixed world axes, 90 degrees at a time --------------------
typedef int8_t Mat3[3][3];
static const Mat3 RX_P = { {1,0,0}, {0,0,-1}, {0,1,0} };
static const Mat3 RX_N = { {1,0,0}, {0,0,1},  {0,-1,0} };
static const Mat3 RZ_P = { {0,-1,0},{1,0,0},  {0,0,1} };
static const Mat3 RZ_N = { {0,1,0}, {-1,0,0}, {0,0,1} };
static const Mat3 RY_P = { {0,0,1}, {0,1,0},  {-1,0,0} };
static const Mat3 IDENT = { {1,0,0}, {0,1,0}, {0,0,1} };

static void mat_apply(const Mat3 m, int8_t dx, int8_t dy, int8_t dz, int8_t *ox, int8_t *oy, int8_t *oz) {
    *ox = (int8_t)(m[0][0]*dx + m[0][1]*dy + m[0][2]*dz);
    *oy = (int8_t)(m[1][0]*dx + m[1][1]*dy + m[1][2]*dz);
    *oz = (int8_t)(m[2][0]*dx + m[2][1]*dy + m[2][2]*dz);
}
static void mat_mul(Mat3 out, const Mat3 a, const Mat3 b) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        int s = 0;
        for (int k = 0; k < 3; k++) s += a[r][k] * b[k][c];
        out[r][c] = (int8_t)s;
    }
}

// ---- state ------------------------------------------------------------------
// grid[y][x][z]: 0 empty, else 1+piece-type — the type is kept so a locked cube still
// carries its colour.
static uint8_t g_grid[WELL_H][WELL_W][WELL_D];

typedef struct {
    int type;
    Mat3 rot;
    int gx, gy, gz;
} Piece;
static Piece g_pc;

enum { ST_FALL, ST_FLASH, ST_OVER };
static int g_state;
static int g_grav_t, g_score, g_lines;
static uint32_t g_rng, g_frame;
static int g_move_cool;
static uint8_t g_prev_jump, g_prev_up, g_prev_down, g_prev_left, g_prev_right, g_prev_restart;
static int g_flash_mask;     // bit y set => layer y is flashing
static int g_flash_t;
static int g_flash_lines;    // lines counted at lock time, held across the flash countdown
static uint8_t g_events;
#define EV_ROTATE 1
#define EV_LOCK   2
#define EV_CLEAR  4
#define EV_OVER   8
#define EV_DENY   16
static int g_ev_lines;       // lines cleared this tick, for audio pitch

static uint32_t lcg(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static void offsets_of(const Piece *p, int8_t off[4][3]) {
    for (int k = 0; k < PIECE_N[p->type]; k++)
        mat_apply(p->rot, PIECE_CELLS[p->type][k][0], PIECE_CELLS[p->type][k][1], PIECE_CELLS[p->type][k][2],
                  &off[k][0], &off[k][1], &off[k][2]);
}

static int fits(int type, int gx, int gy, int gz, const int8_t off[4][3]) {
    for (int k = 0; k < PIECE_N[type]; k++) {
        int x = gx + off[k][0], y = gy + off[k][1], z = gz + off[k][2];
        if (x < 0 || x >= WELL_W || z < 0 || z >= WELL_D || y < 0 || y >= WELL_H) return 0;
        if (g_grid[y][x][z]) return 0;
    }
    return 1;
}

static void spawn_piece(void) {
    g_pc.type = (int)(lcg() % NPT);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) g_pc.rot[r][c] = IDENT[r][c];
    g_pc.gx = CENTER_X; g_pc.gy = SPAWN_GY; g_pc.gz = CENTER_Z;
    int8_t off[4][3]; offsets_of(&g_pc, off);
    if (!fits(g_pc.type, g_pc.gx, g_pc.gy, g_pc.gz, off)) { g_state = ST_OVER; g_events |= EV_OVER; }
}

static void reset_game(uint32_t seed) {
    for (int y = 0; y < WELL_H; y++) for (int x = 0; x < WELL_W; x++) for (int z = 0; z < WELL_D; z++) g_grid[y][x][z] = 0;
    g_state = ST_FALL; g_grav_t = 0; g_score = 0; g_lines = 0;
    g_rng = seed; g_move_cool = 0;
    g_prev_jump = g_prev_up = g_prev_down = g_prev_left = g_prev_right = g_prev_restart = 0;
    g_flash_mask = 0; g_flash_t = 0; g_flash_lines = 0;
    spawn_piece();
}

static int try_move(int dx, int dy, int dz) {
    int8_t off[4][3]; offsets_of(&g_pc, off);
    int nx = g_pc.gx + dx, ny = g_pc.gy + dy, nz = g_pc.gz + dz;
    if (!fits(g_pc.type, nx, ny, nz, off)) return 0;
    g_pc.gx = nx; g_pc.gy = ny; g_pc.gz = nz;
    return 1;
}

static void try_rotate(const Mat3 step) {
    Piece cand = g_pc;
    mat_mul(cand.rot, step, g_pc.rot);
    int8_t off[4][3]; offsets_of(&cand, off);
    if (fits(cand.type, cand.gx, cand.gy, cand.gz, off)) { g_pc = cand; g_events |= EV_ROTATE; }
    else g_events |= EV_DENY;
}

static void lock_piece(void) {
    int8_t off[4][3]; offsets_of(&g_pc, off);
    for (int k = 0; k < PIECE_N[g_pc.type]; k++) {
        int x = g_pc.gx + off[k][0], y = g_pc.gy + off[k][1], z = g_pc.gz + off[k][2];
        g_grid[y][x][z] = (uint8_t)(1 + g_pc.type);
    }
    g_score += 10;
    g_events |= EV_LOCK;

    int mask = 0, lines = 0;
    for (int y = 0; y < WELL_H; y++) {
        int full = 1;
        for (int x = 0; x < WELL_W && full; x++) for (int z = 0; z < WELL_D; z++) if (!g_grid[y][x][z]) { full = 0; break; }
        if (full) { mask |= (1 << y); lines++; }
    }
    if (lines) {
        g_flash_mask = mask; g_flash_t = 18; g_flash_lines = lines;
        g_state = ST_FLASH;
        g_events |= EV_CLEAR; g_ev_lines = lines;   // g_ev_lines is for THIS tick's audio only —
                                                     // it gets reset every tick, so do_clear_and_shift
                                                     // (which fires 18 ticks later) reads g_flash_lines instead.
    } else {
        spawn_piece();
    }
}

static void do_clear_and_shift(void) {
    for (int y = 0; y < WELL_H; y++) {
        if (!(g_flash_mask & (1 << y))) continue;
        for (int yy = y; yy > 0; yy--) for (int x = 0; x < WELL_W; x++) for (int z = 0; z < WELL_D; z++)
            g_grid[yy][x][z] = g_grid[yy-1][x][z];
        for (int x = 0; x < WELL_W; x++) for (int z = 0; z < WELL_D; z++) g_grid[0][x][z] = 0;
    }
    g_lines += g_flash_lines;
    g_score += 100 * g_flash_lines * g_flash_lines;
    g_flash_mask = 0; g_flash_lines = 0;
}

static int grav_frames(void) {
    int g = 26 - g_score / 150;
    return g < 8 ? 8 : g;
}
#define FAST_GRAV 4
#define MOVE_COOLDOWN 8

static void init(void);   // fwd

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0; g_ev_lines = 0;

    int jump = in[0].jump || in[1].jump;

    if (g_state == ST_OVER) {
        if (jump && !g_prev_restart) reset_game(0xC0FFEEu ^ g_frame);
        g_prev_restart = jump;
        return;
    }

    if (g_state == ST_FLASH) {
        if (--g_flash_t <= 0) { do_clear_and_shift(); spawn_piece(); if (g_state != ST_OVER) g_state = ST_FALL; }
        return;
    }

    // -- movement: WASD/left-stick, debounced so a held key doesn't scroll every frame
    if (g_move_cool > 0) g_move_cool--;
    else {
        int dx = in[0].x > 0 ? 1 : (in[0].x < 0 ? -1 : 0);
        int dz = in[0].y > 0 ? 1 : (in[0].y < 0 ? -1 : 0);
        if (dx || dz) {
            if (dx) try_move(dx, 0, 0);
            if (dz) try_move(0, 0, dz);
            g_move_cool = MOVE_COOLDOWN;
        }
    }

    // -- rotation: space rolls yaw, up/down pitch, left/right the third axis. Edge
    // triggered — a rotation is a discrete 90 degrees, not something you can hold.
    int up = in[1].y > 0, down = in[1].y < 0, left = in[1].x < 0, right = in[1].x > 0;
    if (jump && !g_prev_jump) try_rotate(RY_P);
    if (up && !g_prev_up) try_rotate(RX_P);
    if (down && !g_prev_down) try_rotate(RX_N);
    if (right && !g_prev_right) try_rotate(RZ_P);
    if (left && !g_prev_left) try_rotate(RZ_N);
    g_prev_jump = (uint8_t)jump; g_prev_up = (uint8_t)up; g_prev_down = (uint8_t)down;
    g_prev_left = (uint8_t)left; g_prev_right = (uint8_t)right;

    // -- gravity: held act (E / shift//) drops fast, same axis the whole game runs on
    int held_fast = in[0].act || in[1].act;
    int gf = held_fast ? FAST_GRAV : grav_frames();
    if (++g_grav_t >= gf) {
        g_grav_t = 0;
        if (!try_move(0, 1, 0)) lock_piece();
    }
}

static void audio(void) {
    if (g_events & EV_ROTATE) synth_note(NCHAN - 1, 4, 72, 90);
    if (g_events & EV_DENY)   synth_note(NCHAN - 1, 4, 48, 60);
    if (g_events & EV_LOCK)   synth_note(NCHAN - 1, 5, 40, 150);
    if (g_events & EV_CLEAR)  synth_note(NCHAN - 1, 3, (uint8_t)(64 + g_ev_lines * 5), 220);
    if (g_events & EV_OVER)   synth_note(NCHAN - 1, 5, 28, 200);
}

// ---- geometry ---------------------------------------------------------------------
// One shared unit-cube skeleton (8 verts, 12 tris, proper outward normals). Every
// coloured thing in the well — locked cubes, the falling piece, the flash, the
// structure — is this same skeleton with a different Tri.ci baked in and a different
// Inst.scale/pos on top, exactly the box() pattern the engine uses.
static const int8_t CUBE_VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                       {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t CUBE_FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t CUBE_FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static V3  unit_v[8];
static void build_unit_cube(void) {
    for (int v = 0; v < 8; v++) {
        unit_v[v].x = CUBE_VP[v][0] << 15; unit_v[v].y = CUBE_VP[v][1] << 15; unit_v[v].z = CUBE_VP[v][2] << 15;
    }
}
static void fill_tris(Tri t[12], uint8_t ci) {
    for (int f = 0; f < 6; f++) for (int k = 0; k < 2; k++) {
        Tri *o = &t[f * 2 + k];
        o->a = CUBE_FQ[f][0]; o->b = CUBE_FQ[f][1+k]; o->c = CUBE_FQ[f][2+k];
        o->ci = ci;
        o->nx = (int16_t)(CUBE_FN[f][0] * 32767); o->ny = (int16_t)(CUBE_FN[f][1] * 32767); o->nz = (int16_t)(CUBE_FN[f][2] * 32767);
    }
}

// Per-type cube meshes (colour baked in, geometry shared) — this is how one instance
// array can hold cubes of eight different colours without Inst ever carrying a colour.
static Tri  type_t[NPT][12];
static Mesh type_m[NPT];
static Tri  flash_t[12];
static Mesh flash_m;

// Structural meshes: pillars, the mouth rim, the floor. These need their OWN vertices
// (non-uniform boxes — a pillar is tall and thin, Inst.scale is a single number) baked
// once in init, the same trick the engine's box helper uses.
#define NSTRUCT 9   // 4 pillars + 4 rim bars + 1 floor
static V3  struct_v[NSTRUCT][8];
static Tri struct_t[NSTRUCT][12];
static Mesh struct_m[NSTRUCT];
static void shape_box(int i, int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    for (int v = 0; v < 8; v++) {
        struct_v[i][v].x = CUBE_VP[v][0] * sx; struct_v[i][v].y = CUBE_VP[v][1] * sy; struct_v[i][v].z = CUBE_VP[v][2] * sz;
    }
    fill_tris(struct_t[i], ci);
    struct_m[i].v = struct_v[i]; struct_m[i].nv = 8; struct_m[i].t = struct_t[i]; struct_m[i].nt = 12;
}

// World placement: the well's mouth sits at world y=0, and depth grows downward (-y),
// exactly the axis gravity already falls on. Cross-section is centred on x=0/z=0 so the
// camera can sit dead on the shaft's own axis and still see it recede off-centre.
#define HALFW ((WELL_W * CELLU) / 2)
#define HALFD ((WELL_D * CELLU) / 2)
#define DEPTH (WELL_H * CELLU)
// Cell (gx,gz) -> world centre. A WELL_W-wide row of unit cells spans
// [-HALFW, +HALFW], so cell gx's centre is half-integer steps from the middle —
// (gx - CENTER_X) is wrong the moment WELL_W is even, because CENTER_X is a cell
// INDEX (must be a whole grid column, so the spawn piece has real neighbours on both
// sides) while the world middle sits between columns 1 and 2. Two different things;
// keep them apart.
static void cell_world(int gx, int gy, int gz, int32_t *wx, int32_t *wy, int32_t *wz) {
    *wx = (2 * gx - (WELL_W - 1)) * (CELLU / 2);
    *wy = -(gy * CELLU + CELLU / 2);
    *wz = (2 * gz - (WELL_D - 1)) * (CELLU / 2);
}

static void init_geo(void) {
    build_unit_cube();
    for (int i = 0; i < NPT; i++) {
        fill_tris(type_t[i], (uint8_t)(PAL_TYPE0 + i * 8));
        type_m[i].v = unit_v; type_m[i].nv = 8; type_m[i].t = type_t[i]; type_m[i].nt = 12;
    }
    fill_tris(flash_t, PAL_FLASH);
    flash_m.v = unit_v; flash_m.nv = 8; flash_m.t = flash_t; flash_m.nt = 12;

    // 4 corner pillars: thin in x/z, spanning the whole shaft depth.
    static const int8_t CORNER[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    int32_t pillar_half = CELLU / 14;
    for (int i = 0; i < 4; i++) shape_box(i, pillar_half, DEPTH / 2, pillar_half, PAL_PILLAR);
    // 4 rim bars marking the mouth, one per edge, sitting just above y=0.
    int32_t bar_half = CELLU / 14;
    shape_box(4, HALFW, bar_half, bar_half, PAL_RIM);   // north/south run along x -> placed at +-z
    shape_box(5, HALFW, bar_half, bar_half, PAL_RIM);
    shape_box(6, bar_half, bar_half, HALFD, PAL_RIM);   // east/west run along z -> placed at +-x
    shape_box(7, bar_half, bar_half, HALFD, PAL_RIM);
    // the floor, flat, at the bottom.
    shape_box(8, HALFW, CELLU / 10, HALFD, PAL_FLOOR);
}

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

static void init(void) {
    tables_init();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF12131C;                 // the dark past the shaft's mouth
    g_pal[1] = 0xFFF5F5F8;                 // HUD text
    g_pal[2] = 0xFF8890A6;                 // HUD dim
    g_pal[3] = 0xFFEF7D57;                 // game-over banner
    ramp(PAL_PILLAR, 0xFF5B6478);          // pillars
    ramp(PAL_RIM,    0xFFEAEAF0);          // mouth rim — bright, the nearest thing in the shot
    ramp(PAL_FLOOR,  0xFF3A3F52);          // floor, far away and dim by distance alone
    for (int i = 0; i < NPT; i++) ramp(PAL_TYPE0 + i * 8, PIECE_RGB[i]);
    ramp(PAL_FLASH,  0xFFFFFFFF);          // flash — a cleared layer goes white before it vanishes

    init_geo();
    reset_game(0x9E3779B9u);
}

// ---- draw -------------------------------------------------------------------------
static void num_draw(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = 0;
    text_draw(x, y, sc, b, ci);
}

static Inst g_inst[512];

static void draw(void) {
    fb_clear(0);
    int n = 0;

    // structure: 4 pillars at the corners, 4 rim bars round the mouth, one floor.
    int32_t pz = DEPTH / 2;
    static const int8_t CORNER[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    for (int i = 0; i < 4; i++) {
        g_inst[n].m = &struct_m[i];
        g_inst[n].pos = (V3){ CORNER[i][0] * HALFW, -pz, CORNER[i][1] * HALFD };
        g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0; g_inst[n].scale = CELLU; n++;
    }
    g_inst[n].m = &struct_m[4]; g_inst[n].pos = (V3){ 0, 0, -HALFD }; g_inst[n].ax=g_inst[n].ay=g_inst[n].az=0; g_inst[n].scale=CELLU; n++;
    g_inst[n].m = &struct_m[5]; g_inst[n].pos = (V3){ 0, 0,  HALFD }; g_inst[n].ax=g_inst[n].ay=g_inst[n].az=0; g_inst[n].scale=CELLU; n++;
    g_inst[n].m = &struct_m[6]; g_inst[n].pos = (V3){ -HALFW, 0, 0 }; g_inst[n].ax=g_inst[n].ay=g_inst[n].az=0; g_inst[n].scale=CELLU; n++;
    g_inst[n].m = &struct_m[7]; g_inst[n].pos = (V3){  HALFW, 0, 0 }; g_inst[n].ax=g_inst[n].ay=g_inst[n].az=0; g_inst[n].scale=CELLU; n++;
    g_inst[n].m = &struct_m[8]; g_inst[n].pos = (V3){ 0, -DEPTH, 0 }; g_inst[n].ax=g_inst[n].ay=g_inst[n].az=0; g_inst[n].scale=CELLU; n++;

    // locked cubes
    int blink = (int)((g_frame / 3) & 1);
    for (int y = 0; y < WELL_H; y++) {
        int flashing = (g_state == ST_FLASH) && (g_flash_mask & (1 << y));
        for (int x = 0; x < WELL_W; x++) for (int z = 0; z < WELL_D; z++) {
            uint8_t c = g_grid[y][x][z];
            if (!c) continue;
            if (n >= 512) continue;
            int32_t wx, wy, wz; cell_world(x, y, z, &wx, &wy, &wz);
            g_inst[n].m = (flashing && blink) ? &flash_m : &type_m[c - 1];
            g_inst[n].pos = (V3){ wx, wy, wz };
            g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0;
            g_inst[n].scale = CELLU * 82 / 100;
            n++;
        }
    }

    // the falling piece, mid-air — same cell centres, no interpolation needed for the
    // grid itself, but its DESCENT within the current layer is shown continuously so a
    // piece reads as falling, not stepping.
    if (g_state == ST_FALL) {
        int8_t off[4][3]; offsets_of(&g_pc, off);
        int gf = grav_frames();
        int32_t creep = (int32_t)(((int64_t)g_grav_t * CELLU) / gf);
        for (int k = 0; k < PIECE_N[g_pc.type]; k++) {
            int32_t wx, wy, wz;
            cell_world(g_pc.gx + off[k][0], g_pc.gy + off[k][1], g_pc.gz + off[k][2], &wx, &wy, &wz);
            wy -= creep;
            g_inst[n].m = &type_m[g_pc.type];
            g_inst[n].pos = (V3){ wx, wy, wz };
            g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0;
            g_inst[n].scale = CELLU * 95 / 100;
            n++;
        }
    }

    // Above and behind the mouth, tipped down into the shaft — near cubes (the mouth)
    // read bigger and brighter than the floor purely because the lens says so.
    Cam cam = { { 0, CELLU * 6, -CELLU * 6 }, 165, 0, 0 };
    g3d_scene(g_inst, n, &cam, 0, 0, 0);

    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "SCORE", 2);
    num_draw(6 * sc + 34 * sc, hud_top(), sc, g_score, 1);
    text_draw(6 * sc, hud_top() + 10 * sc, sc, "LINES", 2);
    num_draw(6 * sc + 34 * sc, hud_top() + 10 * sc, sc, g_lines, 1);

    if (g_state == ST_OVER) {
        int cx = g_fbw / 2, cy = g_fbh / 2;
        for (int y = cy - 26 * sc; y < cy + 30 * sc; y++)
            for (int x = 0; x < g_fbw; x++) if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = 3;
        text_draw(cx - text_width("TOPPED OUT", sc * 2) / 2, cy - 18 * sc, sc * 2, "TOPPED OUT", 1);
        text_draw(cx - text_width("SPACE TO DROP AGAIN", sc) / 2, cy + 10 * sc, sc, "SPACE TO DROP AGAIN", 1);
    }
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < WELL_H; y++) for (int x = 0; x < WELL_W; x++) for (int z = 0; z < WELL_D; z++)
        h = h * 31 + g_grid[y][x][z];
    h = h * 31 + (uint32_t)g_pc.gx; h = h * 31 + (uint32_t)g_pc.gy; h = h * 31 + (uint32_t)g_pc.gz;
    h = h * 31 + (uint32_t)g_pc.type;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) h = h * 31 + (uint8_t)g_pc.rot[r][c];
    h = h * 31 + (uint32_t)g_score + (uint32_t)g_lines + g_rng + (uint32_t)g_state + g_frame;
    return h;
}

const Game game_shaft = { "shaft", init, tick, audio, draw, checksum };
