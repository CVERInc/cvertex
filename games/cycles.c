// cycles.c — TRON light-cycles, 2 players, one grid, no mercy.
//
// The whole game is a grid that remembers where you've been. A cycle never stops moving;
// the only choice either player ever makes is which way to turn, and every cell it has
// ever occupied stays solid forever after. That's the entire rule set — there is no health,
// no score, just a trail that only gets longer and a grid that only gets more full — and
// it is enough, because the danger a player is dodging is the danger they made themselves.
//
// Movement lives on a grid, not in continuous space, on purpose: a light-cycle trail that
// isn't axis-aligned to the wall it eventually forms isn't a wall, it's a diagonal streak
// nothing can reason about. So position is a cell, heading is one of four compass
// directions, and turning is a 90-degree rotation applied exactly on a cell boundary —
// never mid-cell — which is what keeps the trail a real polyline instead of a smear.
#include "core.h"
#include "game.h"
#include "text.h"
#include "synth.h"

#define CELL 8                      // virtual pixels per grid cell
#define GW (VW / CELL)              // 80
#define GH (VH / CELL)              // 45
#define STEP 4                      // ticks per grid move — the whole game's clock

typedef struct {
    int16_t ix, iy;      // current cell
    int8_t  dx, dy;      // heading: exactly one of dx,dy is nonzero
    int8_t  turn_req;     // queued turn from the last press, consumed at the next move
    int8_t  prev_x;       // last frame's raw input.x, for rising-edge detection
    uint8_t alive;
} Cycle;

static Cycle   g_cyc[2];
static uint8_t g_trail[GH][GW];     // 0 empty, 1 = P1's, 2 = P2's
static int     g_subtick;
static uint8_t g_round_over;
static uint8_t g_winner;            // 0 = draw/none, 1 or 2
static uint8_t g_prev_jump[2];
static uint8_t g_events;
#define EV_DIE0   1
#define EV_DIE1   2
#define EV_TURN   4
#define EV_START  8

// A round is its own reset, separate from init: jump restarts without touching the
// palette or reopening the window. A round reset isn't a cold boot, so it isn't init().
static void reset_round(void) {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            g_trail[y][x] = 0;

    // Opposite corners, opposite headings, so the opening seconds are a genuine race
    // across the arena rather than an immediate head-on wall.
    g_cyc[0] = (Cycle){ (int16_t)(GW / 4),     (int16_t)(GH / 4),     1, 0, 0, 0, 1 };
    g_cyc[1] = (Cycle){ (int16_t)(GW * 3 / 4), (int16_t)(GH * 3 / 4), -1, 0, 0, 0, 1 };
    g_trail[g_cyc[0].iy][g_cyc[0].ix] = 1;
    g_trail[g_cyc[1].iy][g_cyc[1].ix] = 2;

    g_subtick = 0;
    g_round_over = 0;
    g_winner = 0;
}

static void init(void) {
    tables_init();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0A0E1A;   // arena floor
    g_pal[1] = 0xFF0DB4C9;   // P1 trail — cyan
    g_pal[2] = 0xFFE8532E;   // P2 trail — hot orange
    g_pal[3] = 0xFFAFFFFA;   // P1 head — near-white cyan
    g_pal[4] = 0xFFFFCFAA;   // P2 head — near-white orange
    g_pal[5] = 0xFF2A3350;   // arena wall / frame
    g_pal[6] = 0xFFF5F5F8;   // text

    g_prev_jump[0] = g_prev_jump[1] = 0;
    reset_round();
}

// Right turn = clockwise on screen (y grows downward): (dx,dy) -> (-dy,dx).
// Left turn = the inverse: (dx,dy) -> (dy,-dx).
static void apply_turn(Cycle *c) {
    if (!c->turn_req) return;
    int8_t ndx, ndy;
    if (c->turn_req > 0) { ndx = (int8_t)(-c->dy); ndy = c->dx; }
    else                 { ndx = c->dy;            ndy = (int8_t)(-c->dx); }
    c->dx = ndx; c->dy = ndy;
    c->turn_req = 0;
}

static void tick(const Input in[2]) {
    g_events = 0;

    if (g_round_over) {
        // Jump restarts, on the press — not a hold that would blow through the win
        // screen the instant it draws it.
        for (int i = 0; i < 2; i++) {
            uint8_t pressed = in[i].jump && !g_prev_jump[i];
            g_prev_jump[i] = in[i].jump;
            if (pressed) { reset_round(); g_events |= EV_START; }
        }
        return;
    }

    // A tap queues a turn; it's consumed once, at the next grid line, however long that
    // is from now. Holding the key doesn't add more turns — only a fresh press does.
    for (int i = 0; i < 2; i++) {
        Cycle *c = &g_cyc[i];
        if (!c->alive) continue;
        int8_t x = in[i].x;
        if (x != 0 && x != c->prev_x) { c->turn_req = x; g_events |= EV_TURN; }
        c->prev_x = x;
    }

    if (++g_subtick < STEP) return;
    g_subtick = 0;

    int16_t nix[2], niy[2];
    uint8_t crash[2] = { 0, 0 };

    for (int i = 0; i < 2; i++) {
        Cycle *c = &g_cyc[i];
        if (!c->alive) { nix[i] = c->ix; niy[i] = c->iy; continue; }
        apply_turn(c);
        nix[i] = (int16_t)(c->ix + c->dx);
        niy[i] = (int16_t)(c->iy + c->dy);
        if (nix[i] < 0 || nix[i] >= GW || niy[i] < 0 || niy[i] >= GH) crash[i] = 1;
        else if (g_trail[niy[i]][nix[i]]) crash[i] = 1;
    }

    // Two more ways to die that a per-player bounds/trail check alone can't see: driving
    // straight into each other's front, or swapping cells like a level crossing.
    if (g_cyc[0].alive && g_cyc[1].alive) {
        if (nix[0] == nix[1] && niy[0] == niy[1]) { crash[0] = crash[1] = 1; }
        if (nix[0] == g_cyc[1].ix && niy[0] == g_cyc[1].iy &&
            nix[1] == g_cyc[0].ix && niy[1] == g_cyc[0].iy) { crash[0] = crash[1] = 1; }
    }

    for (int i = 0; i < 2; i++) {
        Cycle *c = &g_cyc[i];
        if (!c->alive) continue;
        if (crash[i]) {
            c->alive = 0;
            g_events |= (uint8_t)(i == 0 ? EV_DIE0 : EV_DIE1);
        } else {
            c->ix = nix[i]; c->iy = niy[i];
            g_trail[c->iy][c->ix] = (uint8_t)(i + 1);
        }
    }

    int nalive = g_cyc[0].alive + g_cyc[1].alive;
    if (nalive <= 1) {
        g_round_over = 1;
        g_winner = (uint8_t)(g_cyc[0].alive ? 1 : (g_cyc[1].alive ? 2 : 0));
    }
}

static void audio(void) {
    if (g_events & (EV_DIE0 | EV_DIE1)) synth_note(NCHAN - 1, 3, 40, 190);
    else if (g_events & EV_TURN)        synth_note(NCHAN - 1, 5, 76, 90);
    if (g_events & EV_START)            synth_note(NCHAN - 1, 4, 64, 150);
}

static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    for (int i = 0; i < 8; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i] * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1] * g_fbh / VH);
    }
    poly_fill(p, 4, ci);
}

static void draw(void) {
    fb_clear(0);

    // The frame the arena lives inside — walls are as lethal as any trail, so drawing
    // one is a promise the collision check already keeps.
    quad(0, 0, VW, 3, 5);
    quad(0, VH - 3, VW, 3, 5);
    quad(0, 0, 3, VH, 5);
    quad(VW - 3, 0, 3, VH, 5);

    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            uint8_t t = g_trail[y][x];
            if (!t) continue;
            quad(x * CELL, y * CELL, CELL - 1, CELL - 1, t);
        }
    }
    // Heads redrawn last and brighter, so a player can always find themselves at a
    // glance even where the trails have gotten dense.
    for (int i = 0; i < 2; i++) {
        if (!g_cyc[i].alive) continue;
        quad(g_cyc[i].ix * CELL, g_cyc[i].iy * CELL, CELL - 1, CELL - 1, (uint8_t)(3 + i));
    }

    if (g_round_over) {
        int s = g_fbh / 120; if (s < 1) s = 1;
        const char *msg = g_winner == 1 ? "PLAYER 1 WINS" :
                           g_winner == 2 ? "PLAYER 2 WINS" : "DRAW";
        text_draw(g_fbw / 2 - text_width(msg, s * 2) / 2, g_fbh / 2 - 6 * s, s * 2, msg, 6);
        const char *sub = "SPACE TO RESTART";
        text_draw(g_fbw / 2 - text_width(sub, s) / 2, g_fbh / 2 + 10 * s, s, sub, 6);
    }
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ULL;
    #define MIX(v) do { h = (h ^ (uint64_t)(v)) * 1099511628211ULL; } while (0)
    for (int i = 0; i < 2; i++) {
        MIX((uint16_t)g_cyc[i].ix); MIX((uint16_t)g_cyc[i].iy);
        MIX((uint8_t)g_cyc[i].dx);  MIX((uint8_t)g_cyc[i].dy);
        MIX(g_cyc[i].alive);
    }
    MIX((uint32_t)g_subtick); MIX(g_round_over); MIX(g_winner);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            if (g_trail[y][x]) MIX((uint32_t)(y * GW + x) * 3u + g_trail[y][x]);
    #undef MIX
    return h;
}

const Game game_cycles = { "cycles", init, tick, audio, draw, checksum };
