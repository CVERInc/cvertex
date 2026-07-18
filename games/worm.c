// worm.c — snake, the oldest closed loop there is: eat, grow, become your own wall.
//
// The whole game is one array acting as a ring buffer for the body, plus a heading that
// only turns on a 90-degree grid. No physics, no polygons that aren't squares — the grid
// IS the coordinate system, so drawing is "for each occupied cell, paint a square."
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define GW 32
#define GH 18
#define MAXLEN (GW * GH)
#define STEP_TICKS 7      // one cell every 7 ticks — a readable, arcade pace

enum { D_UP, D_RIGHT, D_DOWN, D_LEFT };
static const int8_t DX[4] = { 0, 1, 0, -1 };
static const int8_t DY[4] = { -1, 0, 1, 0 };

// ---- state --------------------------------------------------------------------
static int8_t g_body[MAXLEN][2];   // ring buffer; g_head indexes the front
static int g_head, g_len;
static int g_dir, g_pending_dir;   // pending: the turn a player asked for, applied once
                                    // per step so two turns in one step can't reverse you
static int g_food_x, g_food_y;
static uint32_t g_rng;
static int g_tick;
static int g_over;
static int g_prev_jump;
static uint64_t g_checksum;

// own LCG, advanced only by gameplay events (never real time) — deterministic per the contract
static uint32_t rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static int cell_occupied(int x, int y) {
    for (int i = 0; i < g_len; i++) {
        int idx = (g_head - i + MAXLEN) % MAXLEN;
        if (g_body[idx][0] == x && g_body[idx][1] == y) return 1;
    }
    return 0;
}

static void place_food(void) {
    // Bounded retry, not an infinite loop: a near-full board (len close to GW*GH) could
    // otherwise spin forever hunting for a cell that no longer exists.
    for (int tries = 0; tries < 64; tries++) {
        int x = (int)(rng_next() % GW);
        int y = (int)(rng_next() % GH);
        if (!cell_occupied(x, y)) { g_food_x = x; g_food_y = y; return; }
    }
    // Board effectively full — leave food wherever it last was; a win state cvertex
    // doesn't otherwise track.
}

static void reset_game(void) {
    g_len = 3;
    g_head = 0;
    // Start centred, heading right, laid out tail-to-the-left so growth reads naturally.
    int sx = GW / 2, sy = GH / 2;
    for (int i = 0; i < g_len; i++) {
        int idx = (g_head - i + MAXLEN) % MAXLEN;
        g_body[idx][0] = (int8_t)(sx - i);
        g_body[idx][1] = (int8_t)sy;
    }
    g_dir = D_RIGHT;
    g_pending_dir = D_RIGHT;
    g_tick = 0;
    g_over = 0;
    place_food();
}

static void init(void) {
    tables_init();
    g_rng = 0xC0FFEEu;
    g_prev_jump = 0;
    g_checksum = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;   // background
    g_pal[1] = 0xFF262A3E;   // board tint (alternating not needed, one flat board)
    g_pal[2] = 0xFF41A6F6;   // body
    g_pal[3] = 0xFFB4E8FF;   // head — brighter than body
    g_pal[4] = 0xFFEF7D57;   // food
    g_pal[5] = 0xFFF5F5F8;   // text
    g_pal[6] = 0xFF3A2E4A;   // grid line
    reset_game();
}

static uint8_t g_ev;
#define EV_EAT   1
#define EV_DEAD  2

static void tick(const Input in[2]) {
    g_ev = 0;
    // Single-player: WASD and the arrows both steer — merge both pads into one.
    Input p = input_1p(in);

    // Turning is a request, not an instant snap: only one turn is honoured per grid step,
    // so mashing keys within a step can't double-turn into a 180 and eat yourself for free.
    // 🔴 Reversal guard checks against the CURRENT direction, not the already-pending one,
    // so a player can queue up a turn even if it happens to equal the reverse of a turn
    // they already queued this step — the only illegal move is reversing the direction
    // actually being travelled.
    int want = -1;
    if (p.x > 0) want = D_RIGHT;
    else if (p.x < 0) want = D_LEFT;
    else if (p.y > 0) want = D_UP;     // 'u' sets y=+1 (see menu.c's own y>0=up)
    else if (p.y < 0) want = D_DOWN;
    if (want >= 0) {
        int reverse = (DX[want] == -DX[g_dir] && DY[want] == -DY[g_dir]);
        if (!reverse) g_pending_dir = want;
    }

    if (g_over) {
        // Press-not-hold restart: only the RISING edge of the merged action button, so
        // holding it doesn't machine-gun restarts.
        int pressed = p.jump && !g_prev_jump;
        g_prev_jump = p.jump;
        if (pressed) reset_game();
        g_checksum = g_checksum * 31 + (uint32_t)g_over * 7 + (uint32_t)g_len;
        return;
    }
    g_prev_jump = p.jump;

    g_tick++;
    if (g_tick < STEP_TICKS) {
        g_checksum = g_checksum * 31 + (uint32_t)g_tick;
        return;
    }
    g_tick = 0;
    g_dir = g_pending_dir;

    int nx = g_body[g_head][0] + DX[g_dir];
    int ny = g_body[g_head][1] + DY[g_dir];

    if (nx < 0 || nx >= GW || ny < 0 || ny >= GH) {
        g_over = 1; g_ev |= EV_DEAD;
        g_checksum = g_checksum * 31 + 999;
        return;
    }

    int eating = (nx == g_food_x && ny == g_food_y);
    // Self-collision: the tail cell is about to vacate (unless we're eating, in which
    // case it stays put and growth makes that cell dangerous too) — so check against the
    // body EXCLUDING the tail when not eating.
    int check_len = eating ? g_len : g_len - 1;
    for (int i = 0; i < check_len; i++) {
        int idx = (g_head - i + MAXLEN) % MAXLEN;
        if (g_body[idx][0] == nx && g_body[idx][1] == ny) { g_over = 1; g_ev |= EV_DEAD; break; }
    }
    if (g_over) { g_checksum = g_checksum * 31 + 999; return; }

    g_head = (g_head + 1) % MAXLEN;
    g_body[g_head][0] = (int8_t)nx;
    g_body[g_head][1] = (int8_t)ny;
    if (eating) {
        if (g_len < MAXLEN) g_len++;
        g_ev |= EV_EAT;
        place_food();
    }

    g_checksum = g_checksum * 31 + (uint32_t)nx * 131 + (uint32_t)ny + (uint32_t)g_len * 17;
}

static void audio(void) {
    if (g_ev & EV_EAT)  synth_note(NCHAN - 1, 4, 72, 160);
    if (g_ev & EV_DEAD) synth_note(NCHAN - 1, 3, 40, 200);
}

// ---- draw -----------------------------------------------------------------
static void cell(int gx, int gy, int cw, int ch, int ox, int oy, uint8_t ci) {
    int16_t p[8];
    int x0 = ox + gx * cw, y0 = oy + gy * ch;
    p[0] = (int16_t)(x0 + 1);      p[1] = (int16_t)(y0 + 1);
    p[2] = (int16_t)(x0 + cw - 1); p[3] = (int16_t)(y0 + 1);
    p[4] = (int16_t)(x0 + cw - 1); p[5] = (int16_t)(y0 + ch - 1);
    p[6] = (int16_t)(x0 + 1);      p[7] = (int16_t)(y0 + ch - 1);
    poly_fill(p, 4, ci);
}

static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    poly_fill(p, 4, ci);
}

static void draw(void) {
    fb_clear(0);

    // The board fills the frame at any resolution: pick the largest cell size that fits
    // GWxGH cells, then centre the resulting board.
    int cw = g_fbw / GW, ch = g_fbh / GH;
    int cs = cw < ch ? cw : ch;
    if (cs < 1) cs = 1;
    int bw = cs * GW, bh = cs * GH;
    int ox = (g_fbw - bw) / 2, oy = (g_fbh - bh) / 2;

    quad(ox, oy, bw, bh, 1);
    // A light grid, every 4 cells, so the board reads as a grid without looking busy.
    for (int gx = 0; gx <= GW; gx += 4) quad(ox + gx * cs, oy, 1, bh, 6);
    for (int gy = 0; gy <= GH; gy += 4) quad(ox, oy + gy * cs, bw, 1, 6);

    cell(g_food_x, g_food_y, cs, cs, ox, oy, 4);

    for (int i = 0; i < g_len; i++) {
        int idx = (g_head - i + MAXLEN) % MAXLEN;
        cell(g_body[idx][0], g_body[idx][1], cs, cs, ox, oy, (uint8_t)(i == 0 ? 3 : 2));
    }

    int s = g_fbh / 180;
    if (s < 1) s = 1;
    char buf[16];
    { int n = g_len, i = 0; char tmp[8];
      if (n == 0) tmp[i++] = '0';
      while (n > 0) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
      int j = 0; buf[j++] = 'L'; buf[j++] = 'E'; buf[j++] = 'N'; buf[j++] = ' ';
      while (i > 0) buf[j++] = tmp[--i];
      buf[j] = 0;
    }
    // Keep the HUD clear of the top ~8% of the screen — the window's title bar covers it.
    int hud_y = oy - 10 * s;
    int min_y = g_fbh * 10 / 100;
    if (hud_y < min_y) hud_y = min_y;
    text_draw(ox, hud_y, s, buf, 5);

    if (g_over) {
        const char *m1 = "GAME OVER";
        text_draw(g_fbw / 2 - text_width(m1, s * 3) / 2, g_fbh / 2 - 16 * s, s * 3, m1, 4);
        const char *m2 = "SPACE TO RESTART";
        text_draw(g_fbw / 2 - text_width(m2, s) / 2, g_fbh / 2 + 14 * s, s, m2, 5);
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_worm = { "worm", init, tick, audio, draw, checksum };
