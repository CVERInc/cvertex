// hopcube.c — isometric cube-hop puzzle, Q*bert's whole idea: a pyramid of cubes seen
// from a fixed oblique camera, and you don't walk on it, you HOP on it — diagonally,
// because that's the only way to move on a stack of cubes without a floor between them.
//
// The four diagonal directions are the interface. Up/down/left/right on the pad map to
// up-right/down-left/up-left/down-right on the pyramid, and that one twist is the whole
// game: every hop both moves you and repaints the cube you land on, so "solve the level"
// and "where do I step next" are the same question.
//
// The camera never moves. It doesn't need to — the pyramid recedes into the screen on
// its own (apex far and small, base near and large, real perspective doing the work an
// orthographic iso view fakes), and that's the proof this is a place, not a diagram.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)

// ---- the pyramid grid --------------------------------------------------------
// Row r (0 = apex) has r+1 cubes, col 0..r. cell_idx flattens the triangle into an array;
// cell_pos places it in world space so that row alone carries BOTH the stair-step drop
// (y) and the perspective recession (z) — apex is the row that's high AND far, base is
// the row that's low AND near, so depth does what a diagram has to fake.
#define ROWS 7
#define NCELL (ROWS * (ROWS + 1) / 2)   // 28
static int cell_idx(int row, int col) { return row * (row + 1) / 2 + col; }

#define STEP   U                    // column spacing / row depth
#define STEPY  (U * 7 / 10)         // row drop
#define HALF   (U * 84 / 100)       // cube half-extent, x/z (leaves a seam between cubes)
#define HALFY  (U * 62 / 100)       // cube half-height

static void cell_pos(int row, int col, int32_t *x, int32_t *y, int32_t *z) {
    *x = (int32_t)(2 * col - row) * STEP;
    *y = -(int32_t)row * STEPY;
    *z = (int32_t)(ROWS - 1 - row) * STEP;
}

// ---- box geometry, shared by every cube in the scene -------------------------
static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                 {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
// face 2 is +Y — the only one that's ever a different colour from the rest of the box.

static void build_box(V3 v[8], Tri t[12], int32_t hx, int32_t hy, int32_t hz,
                       uint8_t ci_side, uint8_t ci_top, uint8_t ci_bottom) {
    for (int i = 0; i < 8; i++) {
        v[i].x = VP[i][0] * hx; v[i].y = VP[i][1] * hy; v[i].z = VP[i][2] * hz;
    }
    for (int f = 0; f < 6; f++) {
        uint8_t ci = (f == 2) ? ci_top : (f == 3 ? ci_bottom : ci_side);
        for (int k = 0; k < 2; k++) {
            Tri *tt = &t[f * 2 + k];
            tt->a = FQ[f][0]; tt->b = FQ[f][1 + k]; tt->c = FQ[f][2 + k]; tt->ci = ci;
            tt->nx = (int16_t)(FN[f][0] * 32767);
            tt->ny = (int16_t)(FN[f][1] * 32767);
            tt->nz = (int16_t)(FN[f][2] * 32767);
        }
    }
}

// ---- palette ------------------------------------------------------------------
#define CI_SIDE     8    // cube body — visible on the two lit side faces the camera sees
#define CI_TOP_OFF 16    // an uncoloured top
#define CI_TOP_ON  24    // a coloured (solved) top
#define CI_PLAYER  32
#define CI_ENEMY   40

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ---- cube meshes: one shared vertex block, one Tri[12] per cell (the top's ci differs) --
static V3   g_cellv[8];
static Tri  g_cellt[NCELL][12];
static Mesh g_cellm[NCELL];
static V3   g_cellpos[NCELL];
static uint8_t g_lit[NCELL];
static int  g_littotal;

static void build_cells(void) {
    for (int i = 0; i < 8; i++) {
        g_cellv[i].x = VP[i][0] * HALF; g_cellv[i].y = VP[i][1] * HALFY; g_cellv[i].z = VP[i][2] * HALF;
    }
    for (int c = 0; c < NCELL; c++) {
        for (int f = 0; f < 6; f++) {
            uint8_t ci = (f == 2) ? CI_TOP_OFF : CI_SIDE;
            for (int k = 0; k < 2; k++) {
                Tri *t = &g_cellt[c][f * 2 + k];
                t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k]; t->ci = ci;
                t->nx = (int16_t)(FN[f][0] * 32767);
                t->ny = (int16_t)(FN[f][1] * 32767);
                t->nz = (int16_t)(FN[f][2] * 32767);
            }
        }
        g_cellm[c].v = g_cellv; g_cellm[c].nv = 8; g_cellm[c].t = g_cellt[c]; g_cellm[c].nt = 12;
    }
    int idx = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c <= r; c++) {
            int32_t x, y, z; cell_pos(r, c, &x, &y, &z);
            g_cellpos[idx++] = (V3){ x, y, z };
        }
}

static void paint_cell(int row, int col, int lit) {
    Tri *t = g_cellt[cell_idx(row, col)];
    uint8_t ci = lit ? CI_TOP_ON : CI_TOP_OFF;
    t[4].ci = ci; t[5].ci = ci;
}

// ---- player and enemy markers --------------------------------------------------
#define PLAYER_HALF (HALF * 45 / 100)
#define ENEMY_HALF  (HALF * 40 / 100)
static V3 g_pv[8]; static Tri g_pt[12]; static Mesh g_pm;
static V3 g_ev[8]; static Tri g_et[12]; static Mesh g_em;

static void build_actor_meshes(void) {
    build_box(g_pv, g_pt, PLAYER_HALF, PLAYER_HALF, PLAYER_HALF, CI_PLAYER, CI_PLAYER, CI_PLAYER);
    g_pm.v = g_pv; g_pm.nv = 8; g_pm.t = g_pt; g_pm.nt = 12;
    build_box(g_ev, g_et, ENEMY_HALF, ENEMY_HALF, ENEMY_HALF, CI_ENEMY, CI_ENEMY, CI_ENEMY);
    g_em.v = g_ev; g_em.nv = 8; g_em.t = g_et; g_em.nt = 12;
}

// ---- small fixed-point helpers -------------------------------------------------
static int32_t lerp32(int32_t a, int32_t b, int u /* 0..1024 */) {
    return a + (int32_t)(((int64_t)(b - a) * u) >> 10);
}
static V3 rest_at(int row, int col, int32_t extra_half) {
    int32_t x, y, z; cell_pos(row, col, &x, &y, &z);
    y += HALFY + extra_half;
    return (V3){ x, y, z };
}

// ---- input: the four diagonals, mapped onto up/down/left/right -----------------
// Up->UR, Down->DL, Left->UL, Right->DR. Up/Down are one opposing pair, Left/Right the
// other — press-and-release of either key always undoes itself, which is the only way
// a diagonal-hop game reads as having a "left" and a "right" at all.
static int dir_from_input(int x, int y) {
    if (y > 0) return 1;   // UR
    if (y < 0) return 2;   // DL
    if (x > 0) return 4;   // DR
    if (x < 0) return 3;   // UL
    return 0;
}
static void dir_target(int row, int col, int dc, int *nr, int *nc) {
    switch (dc) {
        case 1: *nr = row - 1; *nc = col;     break;   // UR
        case 2: *nr = row + 1; *nc = col;     break;   // DL
        case 3: *nr = row - 1; *nc = col - 1; break;   // UL
        default:*nr = row + 1; *nc = col + 1; break;   // DR
    }
}

// ---- timings --------------------------------------------------------------------
#define HOP_FRAMES       14
#define FALL_FRAMES      34
#define CLEAR_FRAMES     90
#define ARC_H       (U * 6 / 10)
#define FALL_DROP   (U * 5)

// ---- player state -----------------------------------------------------------
enum { ST_IDLE, ST_HOP, ST_FALL, ST_DEAD, ST_CLEAR };
static int g_prow, g_pcol, g_st, g_timer;
static int g_from_row, g_from_col, g_diract_row, g_diract_col;
static int g_lives, g_level;
static int g_prev_dir, g_prev_jump;
static int g_igrace;   // frames of spawn immunity — apex is both the player's and the
                        // enemy's home cell, so a fresh spawn needs a beat before the
                        // collision check can fire, or landing at the top ever kills you.
#define GRACE_FRAMES 45

// ---- the enemy: a ball that bounces down the pyramid, no colour of its own to give --
enum { E_IDLE, E_HOP, E_HOPOFF, E_GONE };
static int g_erow, g_ecol, g_etorow, g_etocol, g_est, g_etimer;
static uint32_t g_rng;
#define ENEMY_PAUSE_FRAMES   10
#define ENEMY_RESPAWN_FRAMES 60
#define ENEMY_ARC_H (U * 45 / 100)

// House rule: a game's own LCG, never libc rand.
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static int enemy_hop_frames(void) {
    int hf = 26 - (g_level - 1) * 3;
    return hf < 12 ? 12 : hf;
}

static void enemy_reset(void) { g_erow = 0; g_ecol = 0; g_est = E_IDLE; g_etimer = 0; }

static void enemy_tick(void) {
    if (g_est == E_GONE) {
        g_etimer++;
        if (g_etimer >= ENEMY_RESPAWN_FRAMES) enemy_reset();
        return;
    }
    if (g_est == E_IDLE) {
        g_etimer++;
        if (g_etimer >= ENEMY_PAUSE_FRAMES) {
            int nr = g_erow + 1, nc = g_ecol + (int)(rnd() & 1u);
            g_etorow = nr; g_etocol = nc; g_etimer = 0;
            g_est = (nr < ROWS && nc <= nr) ? E_HOP : E_HOPOFF;
        }
        return;
    }
    g_etimer++;
    if (g_etimer >= enemy_hop_frames()) {
        if (g_est == E_HOP) { g_erow = g_etorow; g_ecol = g_etocol; g_est = E_IDLE; g_etimer = 0; }
        else { g_est = E_GONE; g_etimer = 0; }
    }
}

static V3 enemy_pos(void) {
    if (g_est == E_HOP || g_est == E_HOPOFF) {
        int u = g_etimer * 1024 / enemy_hop_frames(); if (u > 1024) u = 1024;
        V3 a = rest_at(g_erow, g_ecol, ENEMY_HALF);
        int32_t tx, ty, tz; cell_pos(g_etorow, g_etocol, &tx, &ty, &tz); ty += HALFY + ENEMY_HALF;
        int32_t arc = (int32_t)(((int64_t)ENEMY_ARC_H * g_sin[(u / 2) & 1023]) >> 15);
        V3 p = { lerp32(a.x, tx, u), lerp32(a.y, ty, u) + arc, lerp32(a.z, tz, u) };
        return p;
    }
    return rest_at(g_erow, g_ecol, ENEMY_HALF);
}

// ---- run/level/life management --------------------------------------------------
static void level_clear_board(void) {
    for (int i = 0; i < NCELL; i++) g_lit[i] = 0;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c <= r; c++) paint_cell(r, c, 0);
    g_littotal = 0;
}
static void respawn_player(void) {
    g_prow = 0; g_pcol = 0; g_st = ST_IDLE; g_timer = 0;
    g_igrace = GRACE_FRAMES;
}
static void new_level(void) {
    level_clear_board();
    g_level++;
    respawn_player();
    enemy_reset();
}
static void new_run(void) {
    g_lives = 3; g_level = 1;
    level_clear_board();
    respawn_player();
    g_rng = 2463534242u;
    enemy_reset();
}
static void lose_life(uint8_t *events) {
    g_lives--;
    *events |= 16;   // EV_DIE
    if (g_lives <= 0) { g_st = ST_DEAD; g_timer = 0; }
    else respawn_player();
}

// ---- the game loop ----------------------------------------------------------------
static uint32_t g_frame;
static uint64_t g_checksum;
static uint8_t  g_events;
#define EV_HOP   1
#define EV_LAND  2
#define EV_FALL  4
#define EV_CLEAR 8
#define EV_DIE  16

static void tick(const Input in[2]) {
    g_events = 0;
    g_frame++;

    int x = in[0].x + in[1].x; if (x > 1) x = 1; else if (x < -1) x = -1;
    int y = in[0].y + in[1].y; if (y > 1) y = 1; else if (y < -1) y = -1;
    int jump = in[0].jump || in[1].jump;

    int dc = dir_from_input(x, y);
    int dc_edge = dc && dc != g_prev_dir;
    g_prev_dir = dc;
    int jedge = jump && !g_prev_jump;
    g_prev_jump = jump;

    if (g_st == ST_DEAD) {
        if (jedge) new_run();
        return;
    }

    enemy_tick();

    if (g_st == ST_CLEAR) {
        g_timer++;
        if (g_timer >= CLEAR_FRAMES) new_level();
    } else {
        if (g_st == ST_IDLE && dc_edge) {
            int nr, nc; dir_target(g_prow, g_pcol, dc, &nr, &nc);
            g_from_row = g_prow; g_from_col = g_pcol;
            g_diract_row = nr; g_diract_col = nc;
            if (nr >= 0 && nr < ROWS && nc >= 0 && nc <= nr) { g_st = ST_HOP; g_timer = 0; g_events |= EV_HOP; }
            else { g_st = ST_FALL; g_timer = 0; g_events |= EV_FALL; }
        } else if (g_st == ST_HOP) {
            g_timer++;
            if (g_timer >= HOP_FRAMES) {
                g_prow = g_diract_row; g_pcol = g_diract_col; g_st = ST_IDLE; g_timer = 0;
                int idx = cell_idx(g_prow, g_pcol);
                if (!g_lit[idx]) {
                    g_lit[idx] = 1; paint_cell(g_prow, g_pcol, 1); g_littotal++; g_events |= EV_LAND;
                }
                if (g_littotal >= NCELL) { g_st = ST_CLEAR; g_timer = 0; g_events |= EV_CLEAR; }
            }
        } else if (g_st == ST_FALL) {
            g_timer++;
            if (g_timer >= FALL_FRAMES) lose_life(&g_events);
        }

        if (g_igrace > 0) g_igrace--;
        if (g_igrace == 0 && g_st == ST_IDLE && g_est == E_IDLE && g_erow == g_prow && g_ecol == g_pcol)
            lose_life(&g_events);
    }

    g_checksum = g_checksum * 1000003u
        + (uint32_t)g_prow * 7u + (uint32_t)g_pcol * 13u + (uint32_t)g_st * 97u
        + (uint32_t)g_lives * 191u + (uint32_t)g_littotal * 389u + (uint32_t)g_level * 601u
        + (uint32_t)g_erow * 53u + (uint32_t)g_ecol * 59u + (uint32_t)g_est * 911u + g_frame;
}

static void audio(void) {
    if (g_events & EV_HOP)   synth_note(NCHAN - 1, 3, 72, 110);
    if (g_events & EV_LAND)  synth_note(NCHAN - 1, 4, 60, 130);
    if (g_events & EV_FALL)  synth_note(NCHAN - 1, 5, 50, 150);
    if (g_events & EV_DIE)   synth_note(NCHAN - 1, 5, 36, 200);
    if (g_events & EV_CLEAR) synth_note(NCHAN - 1, 4, 84, 200);
}

// ---- rendering ----------------------------------------------------------------
static V3 player_pos(void) {
    if (g_st == ST_HOP) {
        int u = g_timer * 1024 / HOP_FRAMES; if (u > 1024) u = 1024;
        V3 a = rest_at(g_from_row, g_from_col, PLAYER_HALF);
        V3 b = rest_at(g_diract_row, g_diract_col, PLAYER_HALF);
        int32_t arc = (int32_t)(((int64_t)ARC_H * g_sin[(u / 2) & 1023]) >> 15);
        return (V3){ lerp32(a.x, b.x, u), lerp32(a.y, b.y, u) + arc, lerp32(a.z, b.z, u) };
    }
    if (g_st == ST_FALL) {
        int u = g_timer * 1024 / FALL_FRAMES; if (u > 1024) u = 1024;
        int u2 = u + u * 3 / 8; if (u2 > 1536) u2 = 1536;
        V3 a = rest_at(g_from_row, g_from_col, PLAYER_HALF);
        V3 b = rest_at(g_diract_row, g_diract_col, PLAYER_HALF);   // extrapolated off-board
        int32_t drop = (int32_t)(((int64_t)FALL_DROP * u * u) >> 20);
        return (V3){ lerp32(a.x, b.x, u2), lerp32(a.y, b.y, u) - drop, lerp32(a.z, b.z, u2) };
    }
    return rest_at(g_prow, g_pcol, PLAYER_HALF);
}

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
    int bar = 12 * sc;
    for (int yy = 0; yy < bar && yy < g_fbh; yy++)
        for (int xx = 0; xx < g_fbw; xx++) g_fb[yy * g_fbw + xx] = 0;

    text_draw(6 * sc, hud_top(), sc, "LEVEL", 1);
    num(6 * sc + 34 * sc, hud_top(), sc, g_level, 2);

    char lb[12]; int lw = digits(g_littotal, lb);
    int sw = text_width(lb, sc) + text_width("/28", sc);
    int cx = g_fbw / 2;
    num(cx - sw / 2, hud_top(), sc, g_littotal, 2);
    text_draw(cx - sw / 2 + text_width(lb, sc), hud_top(), sc, "/28", 1); (void)lw;

    text_draw(g_fbw - 46 * sc, hud_top(), sc, "LIVES", 1);
    num(g_fbw - 8 * sc, hud_top(), sc, g_lives, 2);

    if (g_st == ST_CLEAR)
        text_draw(cx - text_width("LEVEL CLEAR", sc * 2) / 2, g_fbh / 2 - 10 * sc, sc * 2, "LEVEL CLEAR", 2);

    if (g_st == ST_DEAD) {
        int cy = g_fbh / 2;
        for (int yy = cy - 30 * sc; yy < cy + 34 * sc; yy++)
            for (int xx = 0; xx < g_fbw; xx++) if (yy >= 0 && yy < g_fbh) g_fb[yy * g_fbw + xx] = 3;
        text_draw(cx - text_width("GAME OVER", sc * 3) / 2, cy - 22 * sc, sc * 3, "GAME OVER", 2);
        text_draw(cx - text_width("SPACE TO RESTART", sc) / 2, cy + 22 * sc, sc, "SPACE TO RESTART", 1);
    }
}

// A fixed oblique camera: 45 degrees of yaw so the grid reads as a diamond, tipped down
// so the tops read as tops. It never moves — the pyramid's own layout (apex high and far,
// base low and near) does all the depth work, in real perspective, not a projection trick.
#define CAM_AX 100
#define CAM_AY 128
#define CAM_AZ 0
#define CAM_X  (-(U * 636 / 100))
#define CAM_Y  ( (U * 156 / 100))
#define CAM_Z  (-(U * 220 / 100))

static void draw(void) {
    fb_clear(0);
    static Inst inst[NCELL + 2];
    int n = 0;
    for (int i = 0; i < NCELL; i++) {
        inst[n].m = &g_cellm[i]; inst[n].pos = g_cellpos[i];
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
    }
    if (g_st != ST_DEAD) {
        inst[n].m = &g_pm; inst[n].pos = player_pos();
        inst[n].ax = (g_st == ST_FALL) ? (int)((g_timer * 48) & 1023) : 0;
        inst[n].ay = (g_st == ST_FALL) ? (int)((g_timer * 33) & 1023) : 0;
        inst[n].az = 0; inst[n].scale = U; n++;
    }
    if (g_est != E_GONE) {
        inst[n].m = &g_em; inst[n].pos = enemy_pos();
        inst[n].ax = inst[n].ay = 0; inst[n].az = (int)((g_frame * 20) & 1023); inst[n].scale = U; n++;
    }

    Cam cam = { { CAM_X, CAM_Y, CAM_Z }, CAM_AX, CAM_AY, CAM_AZ };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    hud();
}

static uint64_t checksum(void) { return g_checksum; }

static void init(void) {
    tables_init();
    g_frame = 0; g_checksum = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;   // sky
    g_pal[1] = 0xFF8B8FA8;   // dim hud text
    g_pal[2] = 0xFFF5F5F8;   // bright hud text
    g_pal[3] = 0xFF3A1A2C;   // game-over bar
    ramp(CI_SIDE,    0xFF3355AA);   // cube body — cool blue plastic
    ramp(CI_TOP_OFF, 0xFF445566);   // an unsolved top
    ramp(CI_TOP_ON,  0xFFFFC93C);   // a solved top
    ramp(CI_PLAYER,  0xFFEF7D57);   // the hopper
    ramp(CI_ENEMY,   0xFFF23A35);   // the thing to dodge

    build_cells();
    build_actor_meshes();
    new_run();
}

const Game game_hopcube = { "hopcube", init, tick, audio, draw, checksum };
