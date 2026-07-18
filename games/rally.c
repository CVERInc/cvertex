// rally.c — pong, the engine's own showcase.
//
// Two paddles, a ball, a wall on top and bottom. Nothing else. It's here because every
// engine needs the one game whose rules fit in a sentence, so a stranger reading this
// file can hold the whole simulation in their head and check it against what draw()
// paints — no level format, no forms, no camera, just the oldest deflection in games:
// where the ball hits the paddle decides where it leaves.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define FP 8                       // 1 unit = 1/256 of a virtual pixel
#define PADW   (6 << FP)
#define PADH   (48 << FP)
#define BALLW  (6 << FP)
#define PAD_MARGIN (16 << FP)
#define PAD_SPEED  (4 << FP)
#define BALL_SPEED0 (3 << FP)
#define WINSCORE 7

typedef struct { int32_t x, y, vx, vy; } Ball;

static int32_t g_pad_y[2];     // paddle centre, fixed point
static Ball    g_ball;
static uint8_t g_score[2];
static uint32_t g_rng;
static uint8_t g_events;
#define EV_WALL   1
#define EV_PADDLE 2
#define EV_SCORE  4
#define EV_WIN    8

static uint32_t rng_next(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static void serve(int dir) {
    g_ball.x = (VW / 2) << FP;
    g_ball.y = (VH / 2) << FP;
    // Deterministic but not always the same angle — driven by the game's own LCG, not
    // the clock, so a serve after a fixed sequence of inputs always looks the same.
    // 🔴 Kept shallow on purpose: at BALL_SPEED0 the ball needs ~100 ticks to cross
    // half the court, so anything past a gentle drift accumulates more vertical travel
    // than a stationary paddle is tall — an unreturnable serve straight off the pull
    // cord, which is a bug wearing a "random" costume.
    int32_t spread = (int32_t)(rng_next() % 3) - 1;   // -1..1
    g_ball.vx = dir ? BALL_SPEED0 : -BALL_SPEED0;
    g_ball.vy = spread * (1 << (FP - 3));
}

static void init(void) {
    tables_init();
    g_rng = 12345u;
    g_pad_y[0] = g_pad_y[1] = (VH / 2) << FP;
    g_score[0] = g_score[1] = 0;
    serve(0);
    g_events = 0;

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0E1420;   // court
    g_pal[1] = 0xFFF5F5F8;   // paddles, ball, net, score
    g_pal[2] = 0xFF2A3548;   // net dashes (dimmer than the ball/paddles)
    g_pal[3] = 0xFF41A6F6;   // player 0 accent (left)
    g_pal[4] = 0xFFEF7D57;   // player 1 accent (right)
    g_pal[5] = 0xFF57E389;   // win text
}

static void tick(const Input in[2]) {
    g_events = 0;

    // Match point freezes the court instead of quietly rallying on behind the banner.
    // Either player's Space/Enter serves a fresh match — a shared button on purpose,
    // since whoever just lost is usually the one reaching for it first.
    if (g_score[0] >= WINSCORE || g_score[1] >= WINSCORE) {
        if (in[0].jump || in[1].jump) init();
        return;
    }

    // Paddles. Held input is fine here — this is the ONE game where holding a
    // direction is exactly what a paddle should do, not a repeat-fire bug.
    // 🔴 y=+1 is "up" (the up arrow / 'u'), and up is a SMALLER screen y — so the
    // paddle's y coordinate moves opposite the input sign.
    for (int i = 0; i < 2; i++) {
        g_pad_y[i] -= in[i].y * PAD_SPEED;
        int32_t half = PADH / 2;
        if (g_pad_y[i] < half) g_pad_y[i] = half;
        if (g_pad_y[i] > (VH << FP) - half) g_pad_y[i] = (VH << FP) - half;
    }

    // Ball advance.
    g_ball.x += g_ball.vx;
    g_ball.y += g_ball.vy;

    // Top/bottom walls.
    int32_t br = BALLW / 2;
    if (g_ball.y - br < 0) { g_ball.y = br; g_ball.vy = -g_ball.vy; g_events |= EV_WALL; }
    if (g_ball.y + br > (VH << FP)) { g_ball.y = (VH << FP) - br; g_ball.vy = -g_ball.vy; g_events |= EV_WALL; }

    // Paddle faces, left then right.
    int32_t left_x  = PAD_MARGIN + PADW / 2;
    int32_t right_x = (VW << FP) - PAD_MARGIN - PADW / 2;

    if (g_ball.vx < 0 && g_ball.x - br <= left_x + PADW / 2 && g_ball.x - br >= left_x - PADW / 2) {
        int32_t diff = g_ball.y - g_pad_y[0];
        if (diff < 0) diff = -diff;
        if (diff <= PADH / 2 + br) {
            g_ball.x = left_x + PADW / 2 + br;
            g_ball.vx = -g_ball.vx;
            // Deflection: where on the paddle it landed steers the exit angle. Centre
            // hit goes flat, an edge hit goes steep — the classic pong "aim" trick.
            int32_t off = g_ball.y - g_pad_y[0];               // -PADH/2 .. PADH/2
            g_ball.vy = off * BALL_SPEED0 / (PADH / 2);
            int32_t speed = g_ball.vx < 0 ? -g_ball.vx : g_ball.vx;
            speed += (1 << (FP - 3));                          // a hair faster each rally
            if (speed > (7 << FP)) speed = 7 << FP;
            g_ball.vx = speed;
            g_events |= EV_PADDLE;
        }
    }
    if (g_ball.vx > 0 && g_ball.x + br >= right_x - PADW / 2 && g_ball.x + br <= right_x + PADW / 2) {
        int32_t diff = g_ball.y - g_pad_y[1];
        if (diff < 0) diff = -diff;
        if (diff <= PADH / 2 + br) {
            g_ball.x = right_x - PADW / 2 - br;
            int32_t off = g_ball.y - g_pad_y[1];
            g_ball.vy = off * BALL_SPEED0 / (PADH / 2);
            int32_t speed = g_ball.vx;
            speed += (1 << (FP - 3));
            if (speed > (7 << FP)) speed = 7 << FP;
            g_ball.vx = -speed;
            g_events |= EV_PADDLE;
        }
    }

    // Scoring: off the left edge means player 0 failed to return it, so player 1
    // scores — and vice versa. The next serve heads back at whoever just missed, so
    // they get first crack at the rally instead of watching a serve they can't reach.
    if (g_ball.x < 0) {
        if (g_score[1] < 255) g_score[1]++;
        g_events |= EV_SCORE;
        serve(0);
    } else if (g_ball.x > (VW << FP)) {
        if (g_score[0] < 255) g_score[0]++;
        g_events |= EV_SCORE;
        serve(1);
    }

    if (g_score[0] >= WINSCORE || g_score[1] >= WINSCORE) g_events |= EV_WIN;
}

static void audio(void) {
    if (g_events & EV_WIN)        synth_note(NCHAN - 1, 4, 76, 200);
    else if (g_events & EV_SCORE) synth_note(NCHAN - 1, 3, 48, 200);
    else if (g_events & EV_PADDLE) synth_note(NCHAN - 1, 5, 64, 180);
    else if (g_events & EV_WALL)   synth_note(NCHAN - 1, 5, 52, 140);
}

static void quad(int32_t cx, int32_t cy, int32_t w, int32_t h, uint8_t ci) {
    int x = (cx - w / 2) >> FP, y = (cy - h / 2) >> FP;
    int ww = w >> FP, hh = h >> FP;
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + ww), (int16_t)y,
                     (int16_t)(x + ww), (int16_t)(y + hh), (int16_t)x, (int16_t)(y + hh) };
    for (int i = 0; i < 8; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i] * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1] * g_fbh / VH);
    }
    poly_fill(p, 4, ci);
}

static void draw(void) {
    fb_clear(0);

    // Net: dashed centre line, virtual-pixel spaced so it scales with resolution.
    for (int y = 4; y < VH; y += 16)
        quad((VW / 2) << FP, (int32_t)(y + 4) << FP, 3 << FP, 8 << FP, 2);

    // Paddles.
    quad(PAD_MARGIN + PADW / 2, g_pad_y[0], PADW, PADH, 1);
    quad((VW << FP) - PAD_MARGIN - PADW / 2, g_pad_y[1], PADW, PADH, 1);

    // Ball.
    quad(g_ball.x, g_ball.y, BALLW, BALLW, 1);

    // Scores, top-left and top-right of centre — big enough to read, never colliding
    // with the net. Kept clear of the top ~8% of the frame, which a window's title
    // bar covers.
    int s = g_fbh / 80; if (s < 1) s = 1;
    int hud_y = g_fbh * 9 / 100;
    char buf[4];
    buf[0] = (char)('0' + (g_score[0] % 10)); buf[1] = 0;
    text_draw(g_fbw / 2 - text_width(buf, s) - g_fbw / 14, hud_y, s, buf, 3);
    buf[0] = (char)('0' + (g_score[1] % 10)); buf[1] = 0;
    text_draw(g_fbw / 2 + g_fbw / 14, hud_y, s, buf, 4);

    // g_events is this-tick-only, so gate the banner on the score itself — otherwise
    // it would blink off the very next frame instead of staying up at match point.
    if (g_score[0] >= WINSCORE || g_score[1] >= WINSCORE) {
        const char *msg = g_score[0] >= WINSCORE ? "LEFT WINS" : "RIGHT WINS";
        // Sized off fbw, not fbh: a 10-glyph banner is wide before it's tall, and
        // sizing off height alone once ran it clean off both edges of the court.
        int ss = g_fbw / 100; if (ss < 1) ss = 1;
        text_draw(g_fbw / 2 - text_width(msg, ss) / 2, g_fbh / 2 - ss * 3, ss, msg, 5);
    }
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ull;
    h = h * 1099511628211ull ^ (uint32_t)g_ball.x;
    h = h * 1099511628211ull ^ (uint32_t)g_ball.y;
    h = h * 1099511628211ull ^ (uint32_t)g_pad_y[0];
    h = h * 1099511628211ull ^ (uint32_t)g_pad_y[1];
    h = h * 1099511628211ull ^ g_score[0];
    h = h * 1099511628211ull ^ g_score[1];
    return h;
}

const Game game_rally = { "rally", init, tick, audio, draw, checksum };
