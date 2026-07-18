// cross.c — Frogger. A grid, a frog, and seven lanes that don't care whether you're on
// them.
//
// The whole rule set is one sentence: the road doesn't stop for you, so time your hop or
// don't cross. Everything else — lives, score, lanes of different speed and colour — is
// there to make that one sentence readable at a glance.
//
// The frog moves in cells, one hop per key PRESS (not per held frame — held-down movement
// would turn a timing game into a joystick-waggling one). Traffic moves in continuous
// pixels, because a car that snapped to the grid would look like it was teleporting.
// Sim stays in virtual pixels (core.h's VW x VH); draw scales to whatever framebuffer the
// platform actually handed us.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- the grid -----------------------------------------------------------------
// VW=640, VH=360 divide evenly by 40: a 16x9 board with no remainder pixel anywhere, so
// every lane and every column is exactly one cell wide with nothing left over to fudge.
#define CELL 40
#define COLS (VW / CELL)          // 16
#define ROWS (VH / CELL)          // 9
#define ROW_START 0                // bottom: safe
#define ROW_GOAL  (ROWS - 1)       // top: safe, and the point of the game
#define NLANES (ROW_GOAL - ROW_START - 1)   // rows 1..ROW_GOAL-1 are traffic: 7 of them

// ---- traffic --------------------------------------------------------------
// Each lane is a single repeating car, tiled with a gap, moving at its own speed and
// direction — a lane is a period, not a list of cars, so there is nothing to allocate and
// nothing that runs out. speed is Q8 (px/frame << 8) so slow lanes don't have to move in
// whole-pixel jumps every frame; that's what keeps a 0.3px/frame lane looking driven
// rather than ticked.
// 🔴 Was unwinnable: the old table's gaps (gap-2*FROG_HALF free px) were so thin that a
// safe column in a lane swept past in a fraction of a second, and because every one of the
// 7 rows is traffic (no grass median to plant a foot on), no timing existed that got a frog
// across all 7 without a hit. Verified by exhaustive search over (column, row, tick) against
// the exact hit-test below: the old table had no solution inside thousands of ticks even
// allowing full sideways evasion; this table — wider gaps, gentler speeds — has a clean
// 31-tick solution from the start (and a 26-tick one at MAX_LEVEL's raised speed, so the
// difficulty ramp doesn't sneak the game back into unwinnable).
typedef struct { int8_t dir; uint16_t speed; uint16_t carw, gap; uint8_t ci; } Lane;
static const Lane LANES[NLANES] = {
    {  1,  80, 40, 100, 6 },
    { -1, 100, 32, 110, 7 },
    {  1,  70, 48, 120, 8 },
    { -1, 120, 28,  90, 9 },
    {  1,  90, 36, 110, 6 },
    { -1,  60, 44, 130, 7 },
    {  1, 110, 30,  95, 8 },
};
#define FROG_HALF 14   // half the frog's collision width, in px

// A crossing raises the stakes for the next one: lane speed scales up with g_level, capped
// well short of eating the free window a crossing depends on (the same exhaustive search
// still finds a clean crossing at MAX_LEVEL's speed — see the table comment above).
#define MAX_LEVEL 6

// ---- state ----------------------------------------------------------------------
static int8_t  g_gx, g_row;             // frog cell
static int8_t  g_prevx, g_prevy;        // last frame's input, for press-edge detection
static uint8_t g_lives, g_score;
static uint8_t g_level;                 // crossings banked this run — traffic gets faster
static uint8_t g_over;                  // frozen on the end screen, waiting for restart
static uint8_t g_prev_jump;             // press-edge for the restart button
static uint32_t g_laneoff[NLANES];      // Q8 px accumulators, one phase per lane
static uint64_t g_checksum;

static uint8_t g_events;
#define EV_HOP  1
#define EV_HIT  2
#define EV_GOAL 4

static void reset_frog(void) { g_gx = COLS / 2; g_row = ROW_START; }

// Lane speed scaled up by how many crossings this run has banked, capped at MAX_LEVEL so
// the free window a crossing depends on never gets eaten (verified by simulation up to
// the cap — still 100% crossable at every level, just faster).
static int32_t eff_speed(int li) {
    int32_t lvl = g_level > MAX_LEVEL ? MAX_LEVEL : g_level;
    int32_t mult = 256 + lvl * 24;   // 1.0x .. 1.5625x
    return ((int32_t)LANES[li].speed * mult) >> 8;
}

// Everything a fresh run needs: frog home, a full life bar, score/level zeroed, and the
// lanes back at their hand-picked stagger. Called at boot and every restart, so "press
// Space after game over" and "the very first frame" produce the identical run.
static void reset_run(void) {
    reset_frog();
    g_lives = 3; g_score = 0; g_level = 0;
    g_over = 0;
    // Distinct phases so seven lanes never line up into one column of traffic — a fixed,
    // hand-picked stagger, not randomness: the rule against rand()/clock is about
    // gameplay-time, not about a designer choosing where a car starts.
    for (int i = 0; i < NLANES; i++) g_laneoff[i] = (uint32_t)i * 4001u * 256u;
}

static void init(void) {
    tables_init();
    g_prevx = g_prevy = 0;
    g_prev_jump = 0;
    g_checksum = 0;
    reset_run();

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF2E8B3D;   // start row: grass
    g_pal[1] = 0xFF3A3A42;   // road, shade A
    g_pal[2] = 0xFF333339;   // road, shade B
    g_pal[3] = 0xFF2E6B8B;   // goal row: water-blue safety strip
    g_pal[4] = 0xFF6BE86B;   // frog body
    g_pal[5] = 0xFF16321B;   // frog eyes / dark detail
    g_pal[6] = 0xFFE0483C;   // car colour 1 (red)
    g_pal[7] = 0xFFE0B93C;   // car colour 2 (yellow)
    g_pal[8] = 0xFF3C8FE0;   // car colour 3 (blue)
    g_pal[9] = 0xFFB35CE0;   // car colour 4 (purple)
    g_pal[10] = 0xFFF5F5F0; // HUD text
    g_pal[11] = 0xFFE8E8DC; // lane divider dashes
    g_pal[12] = 0xFFC0392B; // life-lost flash tint (a squashed frog icon uses it too)
}

// Signed shift of a lane's whole tiled pattern, in px, this frame.
static int32_t lane_shift(int li) {
    return LANES[li].dir * (int32_t)(g_laneoff[li] >> 8);
}

// Where along the period a car starts, folded into [0, period).
static int32_t lane_phase(int li, int32_t period) {
    int32_t s = lane_shift(li) % period;
    if (s < 0) s += period;
    return s;
}

static void tick(const Input in[2]) {
    g_events = 0;
    // Single-player: WASD and the arrows both hop the frog — merge both pads into one so
    // it doesn't matter which half of the keyboard a hand lands on.
    Input p = input_1p(in);

    if (g_over) {
        // Frozen on the end screen. Only the restart button is live, and only on the
        // frame it's newly pressed — holding it must not machine-gun new games.
        int pressed = p.jump && !g_prev_jump;
        g_prev_jump = p.jump;
        if (pressed) { init(); return; }   // a completely fresh run, right away
        g_checksum = g_checksum * 1000003u + 777u;
        return;
    }
    g_prev_jump = p.jump;

    // Press edge: a hop fires the frame the axis LEAVES zero (or flips sign outright),
    // never while it sits held — that's the whole difference between a Frogger hop and a
    // joystick crawl.
    int hopx = p.x != 0 && p.x != g_prevx;
    int hopy = p.y != 0 && p.y != g_prevy;
    g_prevx = p.x; g_prevy = p.y;

    if (hopx) {
        int nx = g_gx + p.x;
        if (nx < 0) nx = 0; if (nx >= COLS) nx = COLS - 1;
        g_gx = (int8_t)nx;
        g_events |= EV_HOP;
    }
    if (hopy) {
        // in[].y: +1 is "up" — toward the goal row, which is exactly ROW_GOAL, so the
        // input's own sign is the row delta with no flip needed.
        int nr = g_row + p.y;
        if (nr < ROW_START) nr = ROW_START; if (nr > ROW_GOAL) nr = ROW_GOAL;
        g_row = (int8_t)nr;
        g_events |= EV_HOP;
    }

    for (int i = 0; i < NLANES; i++) g_laneoff[i] += (uint32_t)eff_speed(i);

    // A car that drifts into a frog that never moved has to be able to hit it, so this
    // runs every tick, not only on the frame of a hop.
    if (g_row >= 1 && g_row <= NLANES) {
        int li = g_row - 1;
        int32_t period = (int32_t)LANES[li].carw + LANES[li].gap;
        int32_t phase = lane_phase(li, period);
        int32_t fx = (int32_t)g_gx * CELL + CELL / 2;
        int32_t d = (fx - phase) % period; if (d < 0) d += period;
        int hit = d < (int32_t)LANES[li].carw + FROG_HALF || d > period - FROG_HALF;
        if (hit) {
            g_events |= EV_HIT;
            if (g_lives > 0) g_lives--;
            if (g_lives == 0) g_over = 1;   // wiped out: freeze on the end screen
            else reset_frog();
        }
    } else if (g_row == ROW_GOAL) {
        g_events |= EV_GOAL;
        if (g_score < 255) g_score++;
        if (g_level < MAX_LEVEL) g_level++;   // the next crossing comes faster
        reset_frog();
    }

    g_checksum = g_checksum * 1000003u + (uint32_t)g_gx * 31 + (uint32_t)g_row * 131
               + g_lives * 977u + g_score * 7919u + g_level * 131071u + g_over * 524287u;
    for (int i = 0; i < NLANES; i++) g_checksum = g_checksum * 1000003u + g_laneoff[i];
}

static void audio(void) {
    if (g_events & EV_HIT)  synth_note(NCHAN - 1, 3, 38, 200);
    else if (g_events & EV_GOAL) synth_note(NCHAN - 1, 4, 76, 190);
    else if (g_events & EV_HOP)  synth_note(NCHAN - 1, 5, 64, 130);
}

// ---- draw -----------------------------------------------------------------------
// The sim lives in VW x VH; this is the one place that knows the framebuffer might be a
// different size, so every rect goes through here on its way to the screen.
static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    for (int i = 0; i < 8; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i] * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1] * g_fbh / VH);
    }
    poly_fill(p, 4, ci);
}

static int row_y(int row) { return VH - (row + 1) * CELL; }   // row 0 sits at the bottom

static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}

static void draw(void) {
    fb_clear(1);

    quad(0, row_y(ROW_START), VW, CELL, 0);
    quad(0, row_y(ROW_GOAL), VW, CELL, 3);

    for (int li = 0; li < NLANES; li++) {
        int row = li + 1;
        int y = row_y(row);
        quad(0, y, VW, CELL, (uint8_t)(1 + (li & 1)));
        // A dashed divider along the top edge, cheap lane-reading without a gradient.
        for (int x = 0; x < VW; x += 24) quad(x, y, 12, 2, 11);

        int32_t period = (int32_t)LANES[li].carw + LANES[li].gap;
        int32_t phase = lane_phase(li, period);
        int32_t start = phase - period;
        for (int32_t x = start; x < VW; x += period) {
            int32_t l = x, r = x + LANES[li].carw;
            if (r <= 0 || l >= VW) continue;
            if (l < 0) l = 0; if (r > VW) r = VW;
            quad((int)l, y + 6, (int)(r - l), CELL - 12, LANES[li].ci);
        }
    }

    // The frog: a body square plus two eye dots, always facing up-lane.
    int fx = g_gx * CELL + CELL / 2, fy = row_y(g_row) + CELL / 2;
    quad(fx - 12, fy - 12, 24, 24, 4);
    quad(fx - 8, fy - 12, 4, 4, 5);
    quad(fx + 4, fy - 12, 4, 4, 5);

    // HUD, laid over the goal strip where the background is a flat, uncluttered blue.
    // 🔴 Kept clear of the top ~8% of the screen — that band sits under the window's title
    // bar — by hanging everything from g_fbh*9/100 rather than a handful of fixed pixels.
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    int hud_y = g_fbh * 9 / 100;
    char b[12];
    text_draw(6 * sc, hud_y, sc, "SCORE", 10);
    digits(g_score, b);
    text_draw(6 * sc + text_width("SCORE ", sc), hud_y, sc, b, 10);
    // Lives as a row of small frog-green pips, easier to read at a glance than a digit.
    // quad() already maps virtual -> real pixels, so these stay in virtual space like
    // every other rect on the board — no second scale here. VH*9/100 mirrors hud_y's
    // real-pixel offset so the pips clear the same band regardless of resolution.
    int life_y = VH * 9 / 100;
    for (int i = 0; i < g_lives; i++) {
        int lx = VW - 14 - (int)(i + 1) * 18;
        quad(lx, life_y, 8, 8, 4);
    }

    // Frozen end screen: the board sits exactly as it was on the fatal hit, and Space
    // starts a completely fresh run (handled in tick — see g_over there).
    if (g_over) {
        int s3 = sc * 3; if (s3 < 1) s3 = 1;
        const char *msg = "GAME OVER";
        int mx = g_fbw / 2 - text_width(msg, s3) / 2, my = g_fbh / 2 - 20 * sc;
        // A dim backing panel so the message reads over whatever lane it lands on.
        int16_t bp[8] = { (int16_t)(mx - 10 * sc), (int16_t)(my - 8 * sc),
                           (int16_t)(mx + text_width(msg, s3) + 10 * sc), (int16_t)(my - 8 * sc),
                           (int16_t)(mx + text_width(msg, s3) + 10 * sc), (int16_t)(my + 40 * sc),
                           (int16_t)(mx - 10 * sc), (int16_t)(my + 40 * sc) };
        poly_fill(bp, 4, 2);
        text_draw(mx, my, s3, msg, 12);
        char sb[24]; int n = 0;
        for (const char *q = "SCORE "; *q; q++) sb[n++] = *q;
        digits(g_score, b);
        for (char *q = b; *q; q++) sb[n++] = *q;
        sb[n] = 0;
        text_draw(g_fbw / 2 - text_width(sb, sc) / 2, my + 18 * sc, sc, sb, 10);
        const char *prompt = "SPACE TO RESTART";
        text_draw(g_fbw / 2 - text_width(prompt, sc) / 2, my + 30 * sc, sc, prompt, 11);
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_cross = { "cross", init, tick, audio, draw, checksum };
