// rocks.c — ASTEROIDS. A triangular ship in the middle of a field that wraps on
// itself, rough polygon rocks drifting through it, and nothing round anywhere:
// every shape here, ship, rock, bullet, is poly_fill and g_sin and nothing else.
//
// The whole game is one idea, twice. A rock is a circle of points with each
// radius jittered — that's "rough polygon" — and it's built once, at spawn, not
// reshuffled every frame: a rock that re-jitters each tick doesn't drift, it
// boils. Frozen shape, live position: the same split every vector game in this
// engine makes between what a thing IS and where a thing IS RIGHT NOW.
//
// Momentum is the other half. Rotation just turns the ship; only jump adds
// velocity, along whatever the nose is pointing at that instant, and nothing
// ever subtracts from it. Let go of jump mid-turn and the ship keeps sliding
// the OLD direction while pointing a NEW one — the moment that makes Asteroids
// Asteroids, and the one thing an arcade cabinet with a d-pad could never do,
// because a d-pad has no "let go and coast."
#include "core.h"
#include "game.h"
#include "text.h"
#include "synth.h"

#define FP 16                          // 1 virtual pixel = 1<<FP

// ---- tuning -------------------------------------------------------------
#define ROT_STEP     16                 // angle units (of 1024 = one turn) per frame held
#define THRUST_ACC 7000                 // FP16 units/frame^2 while jump is held
#define MAXV       220000               // FP16 units/frame speed cap, per axis
#define BULLET_SPD 380000                // FP16 units/frame, added to ship velocity
#define BULLET_LIFE  45
#define MAXBUL        6
#define MAXROCK      48
#define NRVERT       10                 // points per rock outline
#define SHIP_R        9                 // collision radius
#define BULLET_R      2
#define INVULN_F     90                 // frames of blink-and-can't-die after a hit
#define WAVE_BASE     4
#define WAVE_MAX      7

static const int32_t RBASE[3]    = { 34, 19, 11 };          // large, medium, small
static const int32_t SCORE[3]    = { 20, 50, 100 };
static const int32_t BASE_SPD[3] = { 45000, 70000, 100000 };
static const int32_t JIT_SPD[3]  = { 20000, 30000, 40000 };

// ---- state ----------------------------------------------------------------
typedef struct {
    int32_t x, y, vx, vy;   // FP16
    int16_t ang;             // 0..1023
    uint8_t invuln;
    uint8_t prev_act;
    uint8_t thrusting;       // draw-only: was jump held this tick
} Ship;

typedef struct {
    int32_t x, y, vx, vy;    // FP16
    int16_t ang, spin;        // visual tumble only — never read by collision
    uint8_t size;              // 0 large, 1 medium, 2 small
    uint8_t alive;
    uint8_t jit[NRVERT];       // per-vertex radius, 65..100 (percent of RBASE)
} Rock;

typedef struct { int32_t x, y, vx, vy; uint8_t life, alive; } Bullet;

static Ship   g_ship;
static Rock   g_rock[MAXROCK];
static Bullet g_bul[MAXBUL];
static uint32_t g_frame;
static uint32_t g_score;
static int      g_lives;
static int      g_over;
static int      g_wave;
static uint32_t g_rng;
static uint8_t  g_events;
static int      g_lasthit;   // size of the rock last destroyed, for audio pitch
#define EV_FIRE    1
#define EV_ROCKHIT 2
#define EV_SHIPHIT 4
#define EV_THRUST  8

enum { COL_BG, COL_SHIP, COL_FLAME, COL_BULLET, COL_ROCK_L, COL_ROCK_M, COL_ROCK_S,
       COL_LABEL, COL_VALUE, COL_OVERBG, COL_OVERTX };

// ---- small maths ------------------------------------------------------------
// 🔴 Not rand(), not time — a game's own LCG, advanced only by decisions the sim
// already made (a wave clearing, a rock taking a hit). Same inputs, same shots,
// same shattering, forever.
static uint32_t rng_next(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static void rot(int32_t x, int32_t y, int16_t a, int32_t *ox, int32_t *oy) {
    int32_t co = g_sin[(a + 256) & 1023], si = g_sin[a & 1023];
    *ox = (int32_t)(((int64_t)x * co - (int64_t)y * si) >> 15);
    *oy = (int32_t)(((int64_t)x * si + (int64_t)y * co) >> 15);
}

static int32_t wrapfp(int32_t v, int32_t maxfp) {
    if (v < 0) v += maxfp; else if (v >= maxfp) v -= maxfp;
    return v;
}

// Is a virtual-pixel coordinate close enough to an edge that its shape needs a
// second copy drawn on the OTHER side, so a rock crossing the seam reads as one
// object wrapping, not two halves vanishing?
static int wrap_off(int32_t v, int32_t vmax, int margin, int32_t *out) {
    if (v < margin) { *out = vmax; return 1; }
    if (v > vmax - margin) { *out = -vmax; return 1; }
    return 0;
}

// Virtual pixels -> real framebuffer pixels. The sim never knows g_fbw/g_fbh.
static void scale_pts(int16_t *p, int n) {
    for (int i = 0; i < n; i++) {
        p[2 * i]     = (int16_t)((int32_t)p[2 * i]     * g_fbw / VW);
        p[2 * i + 1] = (int16_t)((int32_t)p[2 * i + 1] * g_fbh / VH);
    }
}

static int hit(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t rad) {
    int32_t dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy <= rad * rad;
}

// ---- spawning ---------------------------------------------------------------
static void spawn_rock(int32_t x, int32_t y, uint8_t size) {
    for (int i = 0; i < MAXROCK; i++) {
        if (g_rock[i].alive) continue;
        Rock *r = &g_rock[i];
        int a = (int)(rng_next() & 1023);
        int32_t spd = BASE_SPD[size] + (int32_t)(rng_next() % (uint32_t)JIT_SPD[size]);
        int32_t co = g_sin[(a + 256) & 1023], si = g_sin[a & 1023];
        r->x = x; r->y = y;
        r->vx = (int32_t)(((int64_t)co * spd) >> 15);
        r->vy = (int32_t)(((int64_t)si * spd) >> 15);
        r->ang = (int16_t)(rng_next() & 1023);
        r->spin = (int16_t)(1 + (rng_next() % 4));   // always positive: sign never matters, one fewer edge case
        for (int k = 0; k < NRVERT; k++) r->jit[k] = (uint8_t)(65 + (rng_next() % 36));
        r->size = size;
        r->alive = 1;
        return;
    }
}

// A ring around the centre, far enough out that the ship never spawns inside
// one. Angles are spread evenly then jittered, so a wave never opens with two
// rocks stacked on the same bearing.
static void spawn_wave(int n) {
    for (int i = 0; i < n; i++) {
        int a = (int)(((i * 1024) / n + (int)(rng_next() & 63)) & 1023);
        int32_t radius = 140 + (int32_t)(rng_next() % 90);
        int32_t co = g_sin[(a + 256) & 1023], si = g_sin[a & 1023];
        int32_t cx = VW / 2 + (int32_t)(((int64_t)co * radius) >> 15);
        int32_t cy = VH / 2 + (int32_t)(((int64_t)si * radius) >> 15);
        if (cx < 20) cx = 20; if (cx > VW - 20) cx = VW - 20;
        if (cy < 20) cy = 20; if (cy > VH - 20) cy = VH - 20;
        spawn_rock(cx << FP, cy << FP, 0);
    }
}

// ---- the game ---------------------------------------------------------------
static void init(void) {
    tables_init();
    g_frame = 0; g_score = 0; g_lives = 3; g_over = 0; g_wave = 0;
    g_events = 0; g_lasthit = 0;
    g_rng = 0x9E3779B9u;   // a constant, never the clock
    for (int i = 0; i < MAXROCK; i++) g_rock[i].alive = 0;
    for (int i = 0; i < MAXBUL; i++) g_bul[i].alive = 0;
    g_ship = (Ship){ .x = (VW / 2) << FP, .y = (VH / 2) << FP, .ang = 768 };   // 768 = facing up
    spawn_wave(WAVE_BASE);

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[COL_BG]      = 0xFF10121C;   // deep space, not quite black
    g_pal[COL_SHIP]    = 0xFFEAF6FF;
    g_pal[COL_FLAME]   = 0xFFFFB347;
    g_pal[COL_BULLET]  = 0xFFFFD24D;
    g_pal[COL_ROCK_L]  = 0xFF8A7F6B;
    g_pal[COL_ROCK_M]  = 0xFFA79C86;
    g_pal[COL_ROCK_S]  = 0xFFC9BFA6;   // smaller rocks read lighter — a size cue by eye, not just count
    g_pal[COL_LABEL]   = 0xFF7C82A6;
    g_pal[COL_VALUE]   = 0xFFEDEFF7;
    g_pal[COL_OVERBG]  = 0xFF15171F;
    g_pal[COL_OVERTX]  = 0xFFFF5C5C;
}

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0;
    Ship *s = &g_ship;

    // Single-player: merge both pads so WASD and the arrow keys both drive.
    // (input_1p isn't declared in this tree's game.h yet, so inlined here.)
    Input p;
    p.x    = in[0].x    ? in[0].x    : in[1].x;
    p.y    = in[0].y    ? in[0].y    : in[1].y;
    p.jump = (uint8_t)(in[0].jump | in[1].jump);
    p.act  = (uint8_t)(in[0].act  | in[1].act);

    if (g_over) {
        // 🔴 Same debounce as fire: a held Space must not spin up game after
        // game the instant it ends. init() then re-arms the same edge check.
        if (p.jump && !s->prev_act) init();
        s->prev_act = p.jump;
        return;
    }

    {
        s->ang = (int16_t)(((uint16_t)s->ang + (uint16_t)(p.x * ROT_STEP)) & 1023u);

        s->thrusting = p.y > 0;
        if (s->thrusting) {
            int32_t dx = g_sin[(s->ang + 256) & 1023], dy = g_sin[s->ang & 1023];
            s->vx += (int32_t)(((int64_t)dx * THRUST_ACC) >> 15);
            s->vy += (int32_t)(((int64_t)dy * THRUST_ACC) >> 15);
            if (s->vx >  MAXV) s->vx =  MAXV;  if (s->vx < -MAXV) s->vx = -MAXV;
            if (s->vy >  MAXV) s->vy =  MAXV;  if (s->vy < -MAXV) s->vy = -MAXV;
        }
        // 🔴 Edge, not level — debounce the fire button: a verb that reads
        // "held" fires every frame it's held. One press, one bullet. Space
        // (p.jump) is the primary action button: it fires.
        int pressed = p.jump && !s->prev_act;
        s->prev_act = p.jump;
        if (pressed) {
            for (int i = 0; i < MAXBUL; i++) {
                if (g_bul[i].alive) continue;
                int32_t dx = g_sin[(s->ang + 256) & 1023], dy = g_sin[s->ang & 1023];
                int32_t nx, ny; rot(15, 0, s->ang, &nx, &ny);
                Bullet *b = &g_bul[i];
                b->x = s->x + (nx << FP); b->y = s->y + (ny << FP);
                b->vx = s->vx + (int32_t)(((int64_t)dx * BULLET_SPD) >> 15);
                b->vy = s->vy + (int32_t)(((int64_t)dy * BULLET_SPD) >> 15);
                b->life = BULLET_LIFE; b->alive = 1;
                g_events |= EV_FIRE;
                break;
            }
        }

        s->x = wrapfp(s->x + s->vx, (int32_t)VW << FP);
        s->y = wrapfp(s->y + s->vy, (int32_t)VH << FP);
        if (s->invuln > 0) s->invuln--;

        for (int i = 0; i < MAXBUL; i++) {
            Bullet *b = &g_bul[i]; if (!b->alive) continue;
            b->x = wrapfp(b->x + b->vx, (int32_t)VW << FP);
            b->y = wrapfp(b->y + b->vy, (int32_t)VH << FP);
            if (--b->life == 0) b->alive = 0;
        }
        for (int i = 0; i < MAXROCK; i++) {
            Rock *r = &g_rock[i]; if (!r->alive) continue;
            r->x = wrapfp(r->x + r->vx, (int32_t)VW << FP);
            r->y = wrapfp(r->y + r->vy, (int32_t)VH << FP);
            r->ang = (int16_t)(((uint16_t)r->ang + (uint16_t)r->spin) & 1023u);
        }

        // Bullet vs rock: one bullet spends itself on the first rock it touches.
        for (int bi = 0; bi < MAXBUL; bi++) {
            Bullet *b = &g_bul[bi]; if (!b->alive) continue;
            for (int ri = 0; ri < MAXROCK; ri++) {
                Rock *r = &g_rock[ri]; if (!r->alive) continue;
                if (!hit(b->x >> FP, b->y >> FP, r->x >> FP, r->y >> FP, RBASE[r->size] + BULLET_R)) continue;
                b->alive = 0;
                g_score += (uint32_t)SCORE[r->size];
                g_events |= EV_ROCKHIT; g_lasthit = r->size;
                if (r->size < 2) { spawn_rock(r->x, r->y, (uint8_t)(r->size + 1));
                                   spawn_rock(r->x, r->y, (uint8_t)(r->size + 1)); }
                r->alive = 0;
                break;
            }
        }
        // Ship vs rock: the rock survives the ship, never the other way round.
        if (s->invuln == 0) {
            for (int ri = 0; ri < MAXROCK; ri++) {
                Rock *r = &g_rock[ri]; if (!r->alive) continue;
                if (!hit(s->x >> FP, s->y >> FP, r->x >> FP, r->y >> FP, RBASE[r->size] + SHIP_R)) continue;
                g_events |= EV_SHIPHIT;
                if (--g_lives <= 0) { g_over = 1; }
                else {
                    s->x = (VW / 2) << FP; s->y = (VH / 2) << FP;
                    s->vx = 0; s->vy = 0; s->ang = 768; s->invuln = INVULN_F;
                }
                break;
            }
        }
        // The field cleared: a new wave, a little bigger, so the game never just stops.
        int any = 0;
        for (int i = 0; i < MAXROCK; i++) if (g_rock[i].alive) { any = 1; break; }
        if (!any) {
            g_wave++;
            int n = WAVE_BASE + g_wave; if (n > WAVE_MAX) n = WAVE_MAX;
            spawn_wave(n);
        }
    }
}

static void audio(void) {
    if (g_events & EV_FIRE)    synth_note(NCHAN - 1, 5, 76, 130);
    if (g_events & EV_ROCKHIT) synth_note(NCHAN - 1, 3, (uint8_t)(46 + g_lasthit * 6), 190);
    if (g_events & EV_SHIPHIT) synth_note(NCHAN - 1, 4, 38, 220);
}

// ---- draw -------------------------------------------------------------------
static void draw_rock(const Rock *r) {
    int16_t local[NRVERT * 2];
    for (int k = 0; k < NRVERT; k++) {
        int a = (int)(((k * 1024) / NRVERT + r->ang) & 1023);
        int32_t co = g_sin[(a + 256) & 1023], si = g_sin[a & 1023];
        int32_t rr = RBASE[r->size] * r->jit[k] / 100;
        local[2 * k]     = (int16_t)((rr * co) >> 15);
        local[2 * k + 1] = (int16_t)((rr * si) >> 15);
    }
    int32_t cx = r->x >> FP, cy = r->y >> FP;
    int32_t exx, exy; int nex = wrap_off(cx, VW, RBASE[r->size] + 4, &exx);
    int32_t oxs[2] = { 0, 0 }; int nox = 1; if (nex) { oxs[1] = exx; nox = 2; }
    int ney = wrap_off(cy, VH, RBASE[r->size] + 4, &exy);
    int32_t oys[2] = { 0, 0 }; int noy = 1; if (ney) { oys[1] = exy; noy = 2; }
    uint8_t ci = (uint8_t)(r->size == 0 ? COL_ROCK_L : r->size == 1 ? COL_ROCK_M : COL_ROCK_S);
    for (int ix = 0; ix < nox; ix++) for (int iy = 0; iy < noy; iy++) {
        int16_t p[NRVERT * 2];
        for (int k = 0; k < NRVERT; k++) {
            p[2 * k]     = (int16_t)(local[2 * k] + cx + oxs[ix]);
            p[2 * k + 1] = (int16_t)(local[2 * k + 1] + cy + oys[iy]);
        }
        scale_pts(p, NRVERT);
        poly_fill(p, NRVERT, ci);
    }
}

static void draw_bullet(const Bullet *b) {
    int32_t cx = b->x >> FP, cy = b->y >> FP;
    int16_t p[8] = { (int16_t)(cx - 2), (int16_t)cy, (int16_t)cx, (int16_t)(cy - 2),
                      (int16_t)(cx + 2), (int16_t)cy, (int16_t)cx, (int16_t)(cy + 2) };
    scale_pts(p, 4);
    poly_fill(p, 4, COL_BULLET);
}

static void draw_ship(void) {
    const Ship *s = &g_ship;
    int32_t cx = s->x >> FP, cy = s->y >> FP;
    static const int16_t base[3][2] = { { 15, 0 }, { -9, -8 }, { -9, 8 } };

    if (s->thrusting) {
        int32_t fx, fy, lx, ly, rx, ry;
        rot(-17, 0, s->ang, &fx, &fy);
        rot(-9, -4, s->ang, &lx, &ly);
        rot(-9,  4, s->ang, &rx, &ry);
        int16_t fp[6] = { (int16_t)(cx + fx), (int16_t)(cy + fy),
                          (int16_t)(cx + lx), (int16_t)(cy + ly),
                          (int16_t)(cx + rx), (int16_t)(cy + ry) };
        scale_pts(fp, 3);
        poly_fill(fp, 3, COL_FLAME);
    }

    int32_t local[3][2];
    for (int k = 0; k < 3; k++) rot(base[k][0], base[k][1], s->ang, &local[k][0], &local[k][1]);
    int32_t exx, exy; int nex = wrap_off(cx, VW, SHIP_R + 8, &exx);
    int32_t oxs[2] = { 0, 0 }; int nox = 1; if (nex) { oxs[1] = exx; nox = 2; }
    int ney = wrap_off(cy, VH, SHIP_R + 8, &exy);
    int32_t oys[2] = { 0, 0 }; int noy = 1; if (ney) { oys[1] = exy; noy = 2; }
    for (int ix = 0; ix < nox; ix++) for (int iy = 0; iy < noy; iy++) {
        int16_t p[6];
        for (int k = 0; k < 3; k++) {
            p[2 * k]     = (int16_t)(local[k][0] + cx + oxs[ix]);
            p[2 * k + 1] = (int16_t)(local[k][1] + cy + oys[iy]);
        }
        scale_pts(p, 3);
        poly_fill(p, 3, COL_SHIP);
    }
}

static void life_icon(int x, int y, int sc, uint8_t ci) {
    int16_t p[6] = { (int16_t)x, (int16_t)(y - 6 * sc),
                      (int16_t)(x - 4 * sc), (int16_t)(y + 5 * sc),
                      (int16_t)(x + 4 * sc), (int16_t)(y + 5 * sc) };
    poly_fill(p, 3, ci);
}

static int digits(uint32_t v, char *b) {
    int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    // 🔴 The window's title bar covers roughly the top 8% of the frame — keep
    // the HUD clear of it rather than tucked in the very top rows.
    int top = g_fbh * 8 / 100;
    text_draw(6 * sc, top, sc, "SCORE", COL_LABEL);
    char buf[12]; digits(g_score, buf);
    text_draw(6 * sc, top + 8 * sc, sc * 2, buf, COL_VALUE);

    for (int i = 0; i < g_lives; i++)
        life_icon(g_fbw - 10 * sc - i * 12 * sc, top + 6 * sc, sc, COL_SHIP);

    if (g_over) {
        int bh = 34 * sc, by = g_fbh / 2 - bh / 2;
        int16_t band[8] = { 0, (int16_t)by, (int16_t)g_fbw, (int16_t)by,
                             (int16_t)g_fbw, (int16_t)(by + bh), 0, (int16_t)(by + bh) };
        poly_fill(band, 4, COL_OVERBG);
        const char *msg = "GAME OVER";
        int w = text_width(msg, sc * 3);
        text_draw(g_fbw / 2 - w / 2, g_fbh / 2 - 12 * sc, sc * 3, msg, COL_OVERTX);
        const char *sub = "SPACE TO RESTART";
        int sw = text_width(sub, sc);
        text_draw(g_fbw / 2 - sw / 2, g_fbh / 2 + 8 * sc, sc, sub, COL_LABEL);
    }
}

static void draw(void) {
    fb_clear(COL_BG);
    for (int i = 0; i < MAXROCK; i++) if (g_rock[i].alive) draw_rock(&g_rock[i]);
    for (int i = 0; i < MAXBUL; i++)  if (g_bul[i].alive)  draw_bullet(&g_bul[i]);
    if (!g_over && (g_ship.invuln == 0 || ((g_frame >> 2) & 1))) draw_ship();
    hud();
}

static uint64_t checksum(void) {
    uint64_t h = (uint64_t)g_frame * 1000003ull + (uint64_t)g_score * 97ull
               + (uint64_t)g_lives * 13ull + (uint64_t)g_over * 29ull + (uint64_t)g_wave * 7ull;
    h = h * 31 + (uint32_t)g_ship.x; h = h * 31 + (uint32_t)g_ship.y;
    h = h * 31 + (uint16_t)g_ship.ang;
    for (int i = 0; i < MAXROCK; i++) {
        const Rock *r = &g_rock[i]; if (!r->alive) continue;
        h = h * 31 + (uint32_t)r->x; h = h * 31 + (uint32_t)r->y; h = h * 31 + r->size;
    }
    for (int i = 0; i < MAXBUL; i++) {
        const Bullet *b = &g_bul[i]; if (!b->alive) continue;
        h = h * 31 + (uint32_t)b->x; h = h * 31 + (uint32_t)b->y;
    }
    return h;
}

const Game game_rocks = { "rocks", init, tick, audio, draw, checksum };
