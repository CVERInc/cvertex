// brick.c — breakout. A paddle, a ball, and a wall that owes you nothing.
//
// The whole game is one rule played twice: a moving thing meets a static thing, and axis-
// at-a-time decides which side gave way. Bricks and walls and paddle are all rectangles;
// the ball is a point with a radius. Nothing here is smarter than that on purpose — a brick
// doesn't know it's a brick, it's a Box that happens to disappear when the ball crosses it.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define FP 8                         // 1 unit = 1/256 virtual pixel, the engine's usual fixed point

#define PADW   56
#define PADH   8
#define PADY   (VH - 24)
#define BALLR  4

#define COLS   10
#define ROWS   5
#define BRICKW (VW / COLS)
#define BRICKH 14
#define BRICKTOP 40
#define BRICKGAP 1

typedef struct { int32_t x, y, vx, vy; } Ball;

static int32_t g_padx;                 // paddle centre, FP fixed point
static Ball    g_ball;
static uint8_t g_launched;
static uint8_t g_alive[ROWS][COLS];    // 1 = still there
static int     g_lives;
static int     g_bricks_left;
static uint8_t g_won, g_lost;
static uint8_t g_prev_jump;
static uint32_t g_rng;
static uint64_t g_checksum;

// events for audio(), set fresh each tick
enum { EV_WALL = 1, EV_PADDLE = 2, EV_BRICK = 4, EV_DEATH = 8, EV_WIN = 16, EV_LAUNCH = 32 };
static uint8_t g_events;

static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static void reset_ball(void) {
    g_ball.x = (VW / 2) << FP;
    g_ball.y = (PADY - BALLR - 2) << FP;
    g_ball.vx = 0;
    g_ball.vy = 0;
    g_launched = 0;
}

static void init(void) {
    tables_init();
    g_padx = (VW / 2) << FP;
    g_rng = 0xC0FFEEu;
    reset_ball();
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) g_alive[r][c] = 1;
    g_bricks_left = ROWS * COLS;
    g_lives = 3;
    g_won = g_lost = 0;
    g_prev_jump = 0;
    g_checksum = 0;
    g_events = 0;

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF14121F;   // background
    g_pal[1] = 0xFFE8E4F0;   // paddle
    g_pal[2] = 0xFFF5F0FF;   // ball
    g_pal[3] = 0xFF3A3552;   // side/top wall
    g_pal[4] = 0xFFF23A5C;   // brick row 0 (top)
    g_pal[5] = 0xFFFF8A3D;   // brick row 1
    g_pal[6] = 0xFFFFD23D;   // brick row 2
    g_pal[7] = 0xFF3DDC6E;   // brick row 3
    g_pal[8] = 0xFF3D9BFF;   // brick row 4 (bottom)
    g_pal[9] = 0xFFFFFFFF;   // text / HUD
    g_pal[10] = 0xFF87ffb0;  // win text
    g_pal[11] = 0xFFff5a5a;  // lose text
}

static uint8_t row_color(int r) { return (uint8_t)(4 + r); }

// Resolve one axis of the ball against every live brick and the play-field walls.
// Returns 1 if it hit something on this axis (caller flips velocity / removes a brick).
static void ball_axis(int32_t dx, int32_t dy) {
    int32_t nx = g_ball.x + dx, ny = g_ball.y + dy;
    int px = nx >> FP, py = ny >> FP;

    // side walls
    if (px - BALLR < 0)      { nx = (BALLR) << FP;      g_ball.vx = -g_ball.vx; g_events |= EV_WALL; }
    if (px + BALLR > VW)     { nx = (VW - BALLR) << FP;  g_ball.vx = -g_ball.vx; g_events |= EV_WALL; }
    // top wall
    if (py - BALLR < 16)     { ny = (16 + BALLR) << FP;  g_ball.vy = -g_ball.vy; g_events |= EV_WALL; }

    px = nx >> FP; py = ny >> FP;

    // paddle — only matters while the ball is falling toward it
    int pl = (g_padx >> FP) - PADW / 2, pr = pl + PADW;
    if (g_ball.vy > 0 && py + BALLR >= PADY && py - BALLR <= PADY + PADH &&
        px + BALLR > pl && px - BALLR < pr) {
        ny = (PADY - BALLR) << FP;
        g_ball.vy = -g_ball.vy;
        // where it lands on the paddle steers the bounce — a dead-centre hit stays
        // vertical, an edge hit goes sideways. That's what makes the paddle a tool
        // instead of a wall that happens to move.
        int32_t off = (px - (pl + PADW / 2)) * (2 << FP) / (PADW / 2);
        g_ball.vx = off;
        g_events |= EV_PADDLE;
        py = ny >> FP;
    }

    // bricks
    for (int r = 0; r < ROWS; r++) {
        int by = BRICKTOP + r * BRICKH;
        if (py + BALLR < by || py - BALLR > by + BRICKH - BRICKGAP) continue;
        for (int c = 0; c < COLS; c++) {
            if (!g_alive[r][c]) continue;
            int bx = c * BRICKW;
            if (px + BALLR <= bx || px - BALLR >= bx + BRICKW - BRICKGAP) continue;
            g_alive[r][c] = 0;
            g_bricks_left--;
            g_events |= EV_BRICK;
            // bounce whichever axis this call is resolving
            if (dx) g_ball.vx = -g_ball.vx; else g_ball.vy = -g_ball.vy;
            if (dx) nx = g_ball.x; else ny = g_ball.y;
            goto done;
        }
    }
done:
    g_ball.x = nx; g_ball.y = ny;
}

static void tick(const Input in[2]) {
    Input p = input_1p(in);   // WASD or arrows, either pad — one player, one paddle
    g_events = 0;

    int jpress = p.jump && !g_prev_jump;
    g_prev_jump = p.jump;

    if (g_won || g_lost) {
        g_checksum = g_checksum * 31 + g_won + g_lost * 2;
        // Space: freeze on the end screen, then a fresh press starts a whole new game.
        if (jpress) { init(); g_prev_jump = p.jump; }
        return;
    }

    // paddle, player 1 only — this is a one-player wall of bricks
    g_padx += p.x * (4 << FP);
    int32_t half = (PADW / 2) << FP;
    if (g_padx < half) g_padx = half;
    if (g_padx > (VW << FP) - half) g_padx = (VW << FP) - half;

    if (!g_launched) {
        g_ball.x = g_padx;
        if (jpress) {
            g_launched = 1;
            // a small deterministic wobble off dead-centre, from our own LCG — never real time
            int32_t j = (int32_t)(rnd() % 5) - 2;
            g_ball.vx = j << (FP - 2);
            g_ball.vy = -(3 << FP);
            g_events |= EV_LAUNCH;
        }
    } else {
        ball_axis(g_ball.vx, 0);
        ball_axis(0, g_ball.vy);

        if ((g_ball.y >> FP) - BALLR > VH) {
            g_lives--;
            g_events |= EV_DEATH;
            if (g_lives <= 0) g_lost = 1;
            else reset_ball();
        }
        if (g_bricks_left <= 0) { g_won = 1; g_events |= EV_WIN; }
    }

    g_checksum = g_checksum * 31 + (uint32_t)g_ball.x + (uint32_t)g_ball.y * 7 +
                 (uint32_t)g_padx * 13 + (uint32_t)g_bricks_left * 5 + g_lives;
}

static void audio(void) {
    if (g_events & EV_WIN)    { synth_note(NCHAN - 1, 4, 76, 200); return; }
    if (g_events & EV_DEATH)  { synth_note(NCHAN - 1, 3, 40, 200); return; }
    if (g_events & EV_BRICK)  synth_note(NCHAN - 1, 5, 72, 180);
    else if (g_events & EV_PADDLE) synth_note(NCHAN - 1, 4, 60, 150);
    else if (g_events & EV_WALL)   synth_note(NCHAN - 1, 3, 50, 100);
    else if (g_events & EV_LAUNCH) synth_note(NCHAN - 1, 5, 84, 160);
}

// Draws directly in real framebuffer pixels — no VW/VH rescale. Used for HUD that has to
// clear an exact band of the real screen (the title bar) regardless of virtual resolution.
static void rquad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    poly_fill(p, 4, ci);
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

// A ball is a small octagon, not a rect — the one place this game bothers to look round.
static void circle(int cx, int cy, int r, uint8_t ci) {
    int16_t p[16];
    for (int i = 0; i < 8; i++) {
        int a = i * 128;   // 1024/8
        int32_t px = cx + (((int32_t)r * g_sin[(a + 256) & 1023]) >> 15);
        int32_t py = cy + (((int32_t)r * g_sin[a & 1023]) >> 15);
        p[i * 2]     = (int16_t)(px * g_fbw / VW);
        p[i * 2 + 1] = (int16_t)(py * g_fbh / VH);
    }
    poly_fill(p, 8, ci);
}

static void draw(void) {
    fb_clear(0);

    // side & top walls
    quad(0, 0, VW, 16, 3);
    quad(0, 0, 6, VH, 3);
    quad(VW - 6, 0, 6, VH, 3);

    // bricks
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (g_alive[r][c])
                quad(c * BRICKW, BRICKTOP + r * BRICKH, BRICKW - BRICKGAP, BRICKH - BRICKGAP, row_color(r));

    // paddle
    quad((g_padx >> FP) - PADW / 2, PADY, PADW, PADH, 1);

    // ball
    circle(g_ball.x >> FP, g_ball.y >> FP, BALLR, 2);

    // HUD: lives as pips, top-left; bricks-left implied by the wall shrinking.
    // Real pixels, pinned below the title-bar band (~8% of the real screen), not virtual
    // ones — the title bar covers a fixed slice of the actual window, not of VW/VH.
    int sc = g_fbh / 90; if (sc < 1) sc = 1;
    int hudy = g_fbh * 9 / 100;
    for (int i = 0; i < g_lives; i++)
        rquad(8 * sc + i * 12 * sc, hudy, 8 * sc, 6 * sc, 1);

    if (g_won) {
        const char *s = "YOU WIN";
        text_draw(g_fbw / 2 - text_width(s, sc * 3) / 2, g_fbh / 2 - sc * 6, sc * 3, s, 10);
    } else if (g_lost) {
        const char *s = "GAME OVER";
        text_draw(g_fbw / 2 - text_width(s, sc * 3) / 2, g_fbh / 2 - sc * 6, sc * 3, s, 11);
    } else if (!g_launched) {
        const char *s = "SPACE TO LAUNCH";
        text_draw(g_fbw / 2 - text_width(s, sc) / 2, PADY - 22, sc, s, 9);
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_brick = { "brick", init, tick, audio, draw, checksum };
