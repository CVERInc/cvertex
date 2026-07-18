// stack.c — a falling-block puzzle in the well-worn tradition, wearing its own name.
//
// The idea being tested here isn't new: seven shapes, a well ten wide, gravity that never
// lets up, and a row that vanishes when it's full. What IS worth writing down is that the
// entire board is nine bytes wide by twenty tall — 200 bytes of uint8_t — and the checksum
// is that array plus a handful of scalars, hashed. A game whose complexity is in the RULES
// and not in the state was always going to be a good fit for a "same inputs, same state,
// forever" contract; there is nowhere for a clock or a rand() to hide.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define WELL_W 10
#define WELL_H 20

// ---- the seven shapes ---------------------------------------------------------
// Each piece is 4 cells in a 4x4 box, one box per rotation, four rotations. Classic
// (non-SRS) rotation: no wall-kick table beyond a small nudge tried in order, which is
// plenty for a well this wide.
enum { P_I, P_O, P_T, P_S, P_Z, P_J, P_L, NPIECE };
static const int8_t SHAPE[NPIECE][4][4][2] = {
    [P_I] = { {{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}},
              {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}} },
    [P_O] = { {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}},
              {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}} },
    [P_T] = { {{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}},
              {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}} },
    [P_S] = { {{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}},
              {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}} },
    [P_Z] = { {{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}},
              {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}} },
    [P_J] = { {{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}},
              {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}} },
    [P_L] = { {{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}},
              {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}} },
};

// ---- state ----------------------------------------------------------------------
// 0 = empty; 1..NPIECE = which piece locked here, one past the last row that ever moves.
static uint8_t g_grid[WELL_H][WELL_W];
static int8_t  g_cur_type, g_cur_rot, g_cur_x, g_cur_y, g_next_type;
static uint32_t g_rng;
static int32_t g_drop_timer, g_score, g_lines, g_level;
static uint8_t g_over;
static int8_t  g_prev_jump, g_prev_up, g_move_dir;
static int16_t g_move_timer;
static uint32_t g_frame;
static uint8_t g_flash_mask;   // rows cleared this tick, 1 bit per row-count 1..4 — audio only

#define DAS_FIRST 10
#define DAS_REPEAT 4
#define SOFT_INTERVAL 3

static uint32_t rng_next(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static int fits(int type, int rot, int x, int y) {
    for (int i = 0; i < 4; i++) {
        int gx = x + SHAPE[type][rot][i][0];
        int gy = y + SHAPE[type][rot][i][1];
        if (gx < 0 || gx >= WELL_W || gy >= WELL_H) return 0;
        if (gy >= 0 && g_grid[gy][gx]) return 0;
    }
    return 1;
}

static void spawn(void) {
    g_cur_type = g_next_type;
    g_next_type = (int8_t)(rng_next() % NPIECE);
    g_cur_rot = 0;
    g_cur_x = (WELL_W - 4) / 2;
    g_cur_y = 0;
    g_drop_timer = 0;
    // A piece that can't even spawn means the stack has reached the top: game over.
    if (!fits(g_cur_type, g_cur_rot, g_cur_x, g_cur_y)) g_over = 1;
}

static void reset_game(void) {
    for (int y = 0; y < WELL_H; y++) for (int x = 0; x < WELL_W; x++) g_grid[y][x] = 0;
    g_score = 0; g_lines = 0; g_level = 0; g_over = 0;
    g_move_dir = 0; g_move_timer = 0; g_prev_jump = 0; g_prev_up = 0;
    g_flash_mask = 0;
    g_next_type = (int8_t)(rng_next() % NPIECE);
    spawn();
}

static void init(void) {
    tables_init();
    g_rng = 2463534242u;   // any nonzero seed; gameplay advances it from here on
    g_frame = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF14141F;   // background
    g_pal[1] = 0xFF3A3A52;   // well frame
    g_pal[2] = 0xFF1E1E2C;   // well interior
    g_pal[3] = 0xFFF5F5F8;   // text bright
    g_pal[4] = 0xFF8888A0;   // text dim
    g_pal[5] = 0xFF2A2A3C;   // ghost piece
    g_pal[6] = 0xFF23233A;   // side panel
    g_pal[7] = 0xFFEF5D6B;   // GAME OVER accent
    // seven piece colours, index 8..14 (grid stores type+1, palette is 8+type)
    g_pal[8]  = 0xFF4CD3F0;   // I — cyan
    g_pal[9]  = 0xFFF6D742;   // O — yellow
    g_pal[10] = 0xFFB35DF6;   // T — purple
    g_pal[11] = 0xFF6BE672;   // S — green
    g_pal[12] = 0xFFEF5D6B;   // Z — red
    g_pal[13] = 0xFF4C7EF0;   // J — blue
    g_pal[14] = 0xFFF6934C;   // L — orange
    reset_game();
}

static void try_rotate(void) {
    int nr = (g_cur_rot + 1) & 3;
    static const int8_t KICK[5] = { 0, -1, 1, -2, 2 };
    for (int k = 0; k < 5; k++) {
        if (fits(g_cur_type, nr, g_cur_x + KICK[k], g_cur_y)) {
            g_cur_rot = (int8_t)nr;
            g_cur_x = (int8_t)(g_cur_x + KICK[k]);
            return;
        }
    }
}

static void lock_and_clear(void) {
    uint8_t over = 0;
    for (int i = 0; i < 4; i++) {
        int gx = g_cur_x + SHAPE[g_cur_type][g_cur_rot][i][0];
        int gy = g_cur_y + SHAPE[g_cur_type][g_cur_rot][i][1];
        if (gy < 0) { over = 1; continue; }
        g_grid[gy][gx] = (uint8_t)(g_cur_type + 1);
    }
    // A cell locked above row 0 means the piece was still poking out of the well: over.
    if (over) { g_over = 1; return; }

    int cleared = 0;
    uint8_t mask = 0;
    for (int y = WELL_H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < WELL_W; x++) if (!g_grid[y][x]) { full = 0; break; }
        if (!full) continue;
        cleared++;
        if (cleared <= 4) mask |= (uint8_t)(1 << cleared);
        for (int yy = y; yy > 0; yy--)
            for (int x = 0; x < WELL_W; x++) g_grid[yy][x] = g_grid[yy - 1][x];
        for (int x = 0; x < WELL_W; x++) g_grid[0][x] = 0;
        y++;   // re-examine this row index: it now holds what used to sit above it
    }
    static const uint16_t AWARD[5] = { 0, 40, 100, 300, 1200 };
    g_score += AWARD[cleared] * (g_level + 1);
    g_lines += cleared;
    g_level = g_lines / 10;
    g_flash_mask = (uint8_t)mask;

    spawn();
}

static void tick(const Input in[2]) {
    Input p = input_1p(in);   // WASD or arrows, either drives — single-player merge
    g_frame++;
    g_flash_mask = 0;

    int pressed_jump = p.jump && !g_prev_jump;
    g_prev_jump = p.jump;

    if (g_over) {
        if (pressed_jump) reset_game();
        return;
    }

    if (pressed_jump) try_rotate();   // Space — the primary action button — rotates

    int pressed_up = (p.y > 0) && !g_prev_up;
    g_prev_up = (p.y > 0);

    if (p.x != 0) {
        if (p.x != g_move_dir) {
            if (fits(g_cur_type, g_cur_rot, g_cur_x + p.x, g_cur_y)) g_cur_x = (int8_t)(g_cur_x + p.x);
            g_move_dir = p.x;
            g_move_timer = DAS_FIRST;
        } else if (--g_move_timer <= 0) {
            if (fits(g_cur_type, g_cur_rot, g_cur_x + p.x, g_cur_y)) g_cur_x = (int8_t)(g_cur_x + p.x);
            g_move_timer = DAS_REPEAT;
        }
    } else {
        g_move_dir = 0; g_move_timer = 0;
    }

    if (pressed_up) {   // Up — hard drop
        while (fits(g_cur_type, g_cur_rot, g_cur_x, g_cur_y + 1)) g_cur_y++;
        lock_and_clear();
        return;   // the drop that locked this piece doesn't also fall under gravity
    }

    int interval = (p.y < 0) ? SOFT_INTERVAL : (30 - g_level * 2);   // Down — soft drop
    if (interval < 6) interval = 6;
    if (++g_drop_timer >= interval) {
        g_drop_timer = 0;
        if (fits(g_cur_type, g_cur_rot, g_cur_x, g_cur_y + 1)) g_cur_y++;
        else lock_and_clear();
    }
}

static void audio(void) {
    if (g_flash_mask) synth_note(NCHAN - 1, 3, (uint8_t)(60 + g_flash_mask * 4), 200);
}

// ---- drawing ---------------------------------------------------------------------
static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    for (int i = 0; i < 8; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i] * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1] * g_fbh / VH);
    }
    poly_fill(p, 4, ci);
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
    char b[12]; digits(v, b);
    text_draw(x, y, sc, b, ci);
}

#define CELL   14
#define WELL_X 44
#define WELL_Y 36   // clears the top ~8% band a window title bar covers (VH=360 -> 8% = 28.8)
#define PANEL_X (WELL_X + WELL_W * CELL + 24)

static void draw_cell(int x, int y, uint8_t ci) {
    quad(WELL_X + x * CELL + 1, WELL_Y + y * CELL + 1, CELL - 2, CELL - 2, ci);
}

static void draw_piece_at(int type, int rot, int ox, int oy, int cell, int scr_x, int scr_y, uint8_t ci) {
    for (int i = 0; i < 4; i++) {
        int cx = SHAPE[type][rot][i][0] - ox, cy = SHAPE[type][rot][i][1] - oy;
        quad(scr_x + cx * cell + 1, scr_y + cy * cell + 1, cell - 2, cell - 2, ci);
    }
}

// text_draw/text_width work in REAL framebuffer pixels; quad() works in VIRTUAL ones.
// Every other layout number in this file is virtual, so text needs the same transform
// quad() applies internally, or it drifts off its label at any resolution but the
// 640x360 default — invisible until something actually renders at a second size.
static int SX(int vx) { return vx * g_fbw / VW; }
static int SY(int vy) { return vy * g_fbh / VH; }
static int text_scale(void) { int s = g_fbh / VH; return s < 1 ? 1 : s; }

static void draw(void) {
    fb_clear(0);
    int s = text_scale();

    // well frame + interior
    quad(WELL_X - 4, WELL_Y - 4, WELL_W * CELL + 8, WELL_H * CELL + 8, 1);
    quad(WELL_X, WELL_Y, WELL_W * CELL, WELL_H * CELL, 2);

    for (int y = 0; y < WELL_H; y++)
        for (int x = 0; x < WELL_W; x++)
            if (g_grid[y][x]) draw_cell(x, y, (uint8_t)(8 + g_grid[y][x] - 1));

    if (!g_over) {
        int gy = g_cur_y;
        while (fits(g_cur_type, g_cur_rot, g_cur_x, gy + 1)) gy++;
        for (int i = 0; i < 4; i++) {
            int cx = g_cur_x + SHAPE[g_cur_type][g_cur_rot][i][0];
            int cy = gy + SHAPE[g_cur_type][g_cur_rot][i][1];
            if (cy >= 0) draw_cell(cx, cy, 5);
        }
        for (int i = 0; i < 4; i++) {
            int cx = g_cur_x + SHAPE[g_cur_type][g_cur_rot][i][0];
            int cy = g_cur_y + SHAPE[g_cur_type][g_cur_rot][i][1];
            if (cy >= 0) draw_cell(cx, cy, (uint8_t)(8 + g_cur_type));
        }
    }

    // side panel: NEXT, SCORE, LINES, LEVEL
    int px = PANEL_X;
    quad(px, WELL_Y, 5 * CELL, 5 * CELL, 6);
    text_draw(SX(px + CELL / 2), SY(WELL_Y + 4), s, "NEXT", 4);
    draw_piece_at(g_next_type, 0, 1, 1, CELL, px + CELL / 2, WELL_Y + 2 * CELL + CELL / 2, 8 + g_next_type);

    int ty = WELL_Y + 5 * CELL + 16;
    text_draw(SX(px), SY(ty), s, "SCORE", 4);
    num(SX(px), SY(ty) + 10 * s, s * 2, g_score, 3);
    ty += 34;
    text_draw(SX(px), SY(ty), s, "LINES", 4);
    num(SX(px), SY(ty) + 10 * s, s * 2, g_lines, 3);
    ty += 34;
    text_draw(SX(px), SY(ty), s, "LEVEL", 4);
    num(SX(px), SY(ty) + 10 * s, s * 2, g_level, 3);

    if (g_over) {
        int bw = WELL_W * CELL - 6, bh = 5 * CELL;
        int bx = WELL_X + WELL_W * CELL / 2 - bw / 2, by = WELL_Y + WELL_H * CELL / 2 - bh / 2;
        quad(bx, by, bw, bh, 6);
        quad(bx, by, bw, 3, 7);
        text_draw(SX(bx + bw / 2) - text_width("GAME OVER", s) / 2, SY(by + 16), s, "GAME OVER", 7);
        text_draw(SX(bx + bw / 2) - text_width("SPACE TO", s) / 2, SY(by + bh - 34), s, "SPACE TO", 4);
        text_draw(SX(bx + bw / 2) - text_width("RESTART", s) / 2, SY(by + bh - 18), s, "RESTART", 4);
    }
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < WELL_H; y++)
        for (int x = 0; x < WELL_W; x++) h = (h ^ g_grid[y][x]) * 1099511628211ULL;
    h = h * 31 + (uint32_t)g_cur_type;
    h = h * 31 + (uint32_t)g_cur_rot;
    h = h * 31 + (uint32_t)(uint8_t)g_cur_x;
    h = h * 31 + (uint32_t)(uint8_t)g_cur_y;
    h = h * 31 + (uint32_t)g_next_type;
    h = h * 31 + (uint32_t)g_score;
    h = h * 31 + (uint32_t)g_lines;
    h = h * 31 + (uint32_t)g_over;
    h = h * 31 + g_frame;
    return h;
}

const Game game_stack = { "stack", init, tick, audio, draw, checksum };
