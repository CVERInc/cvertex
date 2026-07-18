// missile.c — Missile Command. Enemy fire streaks down out of the dark; you never touch
// it directly. All you have is a crosshair and a button that spends an interceptor to
// buy a circle of sky for a few frames. The whole game is aiming that circle early
// enough to matter.
//
// Everything moves at a CONSTANT SPEED along a straight line computed once, at launch —
// not "ease toward the target every frame", which would make an interceptor's flight
// time depend on floating error instead of on distance. Distance decides a flight-time-
// in-frames up front (ttl), the missile ticks it down, and it detonates on ttl==0
// regardless of where fixed-point drift left it. That's what keeps a slow interceptor and
// a far shot honest against each other run after run.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define FP 8                          // 1 px = 1<<FP units — sub-pixel motion, integer math
#define GROUND_Y (VH - 30)
#define N_CITIES 6
#define MAX_ENEMY 24
#define MAX_INT   6
#define MAX_BLAST 6
#define BLAST_GROW 9                  // frames 0 -> max radius
#define BLAST_HOLD 16                 // frames max radius -> gone
#define BLAST_R    24                 // virtual px
#define CROSS_SPD  (3 << FP)
#define INT_SPEED  6                  // px/frame, interceptor
#define ENEMY_BASE_SPEED 1            // px/frame at wave 1 (fixed-point below adds a fraction)

// ---- integer square root — called once per launch, never per frame -----------------
static int32_t isqrt32(int32_t v) {
    if (v <= 0) return 0;
    int32_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

typedef struct { int32_t x, y, vx, vy; int32_t tx, ty; int16_t ox, oy; int16_t ttl; uint8_t active, city; } Missile;
// city: which city index this enemy is aimed at (valid only while active)
// ox,oy: the launch point, kept around only so the trail has something to start from

// kill: only an interceptor's blast destroys other missiles and scores. A ground impact
// gets the same fireball drawn for it, but it is the player's loss, not a second kill —
// without this flag a city dying next to a crowd would score points for dying.
typedef struct { int16_t x, y, r, t; uint8_t active, kill; } Blast;

static Missile g_enemy[MAX_ENEMY];
static Missile g_intc[MAX_INT];
static Blast   g_blast[MAX_BLAST];
static uint8_t g_city[N_CITIES];      // 1 = standing
static int32_t g_cx, g_cy;            // crosshair, FP
static uint32_t g_score, g_wave, g_lcg;
static int16_t  g_wave_remaining, g_wave_active, g_spawn_timer, g_spawn_base;
static uint8_t  g_over, g_prev_act;
static uint8_t  g_events;
#define EV_LAUNCH 1
#define EV_BLAST  2
#define EV_KILL   4
#define EV_LOST   8
static uint64_t g_checksum;

static uint32_t rnd(uint32_t n) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (n ? (g_lcg >> 16) % n : 0);
}

static int32_t city_x(int i) {
    int32_t margin = 60, span = VW - 2 * margin;
    return margin + span * i / (N_CITIES - 1);
}

static void new_wave(void) {
    g_wave++;
    g_wave_remaining = (int16_t)(5 + g_wave * 2 + (int16_t)rnd(3));
    g_spawn_base = (int16_t)(34 - (int16_t)g_wave * 2);
    if (g_spawn_base < 10) g_spawn_base = 10;
    g_spawn_timer = (int16_t)(g_spawn_base / 2);
}

static void init(void) {
    tables_init();
    g_lcg = 2463534242u;
    g_score = 0; g_wave = 0;
    for (int i = 0; i < N_CITIES; i++) g_city[i] = 1;
    for (int i = 0; i < MAX_ENEMY; i++) g_enemy[i].active = 0;
    for (int i = 0; i < MAX_INT; i++)   g_intc[i].active = 0;
    for (int i = 0; i < MAX_BLAST; i++) g_blast[i].active = 0;
    g_cx = (VW / 2) << FP; g_cy = (VH / 3) << FP;
    g_over = 0; g_prev_act = 0;
    g_events = 0;
    g_wave_active = 0;
    new_wave();
    g_checksum = 0;

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0]  = 0xFF0D0C1D;   // sky
    g_pal[1]  = 0xFF3A2E4A;   // ground band
    g_pal[2]  = 0xFF41A6F6;   // city, standing
    g_pal[3]  = 0xFF2A6EBB;   // city roofline
    g_pal[4]  = 0xFFEF7D57;   // enemy trail
    g_pal[5]  = 0xFFF6C15C;   // enemy head
    g_pal[6]  = 0xFF6DE080;   // interceptor trail
    g_pal[7]  = 0xFFEAF6EF;   // interceptor head
    g_pal[8]  = 0xFFEF7D57;   // blast outer
    g_pal[9]  = 0xFFFFF3B0;   // blast inner
    g_pal[10] = 0xFFF5F5F8;   // crosshair
    g_pal[11] = 0xFF241E30;   // rubble
    g_pal[12] = 0xFFF5F5F8;   // text bright
    g_pal[13] = 0xFF6B6480;   // text dim
    g_pal[14] = 0xFF9AA0C0;   // silo
}

// ---- launch a missile down a straight line at a constant speed, arriving in exactly
// the number of frames the distance calls for -----------------------------------------
static void fire(Missile *m, int32_t sx, int32_t sy, int32_t tx, int32_t ty, int32_t speed, int city) {
    int32_t dx = tx - sx, dy = ty - sy;
    int32_t dist = isqrt32(dx * dx + dy * dy);
    if (dist < 1) dist = 1;
    int32_t ttl = (dist + speed - 1) / speed;
    if (ttl < 1) ttl = 1;
    m->x = sx << FP; m->y = sy << FP;
    m->ox = (int16_t)sx; m->oy = (int16_t)sy;
    m->tx = tx << FP; m->ty = ty << FP;
    m->vx = (dx << FP) / ttl;
    m->vy = (dy << FP) / ttl;
    m->ttl = (int16_t)ttl;
    m->active = 1;
    m->city = (uint8_t)city;
}

static void spawn_blast(int32_t x, int32_t y, uint8_t kill) {
    for (int i = 0; i < MAX_BLAST; i++)
        if (!g_blast[i].active) {
            g_blast[i] = (Blast){ (int16_t)x, (int16_t)y, 0, 0, 1, kill };
            return;
        }
    // no free slot: steal the oldest-looking one rather than lose the detonation silently
    g_blast[0] = (Blast){ (int16_t)x, (int16_t)y, 0, 0, 1, kill };
}

static int any_city_alive(void) {
    for (int i = 0; i < N_CITIES; i++) if (g_city[i]) return 1;
    return 0;
}

static void tick(const Input in[2]) {
    g_events = 0;
    Input p = input_1p(in);               // WASD and the arrows both drive; Space is the button
    uint8_t act = p.jump;
    uint8_t act_edge = (uint8_t)(act && !g_prev_act);
    g_prev_act = act;

    if (g_over) {
        if (act_edge) init();
        return;
    }

    // crosshair: steady speed, digital in, clamped to the sky above the ground band
    g_cx += (int32_t)p.x * CROSS_SPD;
    g_cy -= (int32_t)p.y * CROSS_SPD;   // +y input = up = smaller screen y
    if (g_cx < (8 << FP)) g_cx = 8 << FP;
    if (g_cx > ((VW - 8) << FP)) g_cx = (VW - 8) << FP;
    if (g_cy < (8 << FP)) g_cy = 8 << FP;
    if (g_cy > ((GROUND_Y - 8) << FP)) g_cy = (GROUND_Y - 8) << FP;

    // launch: spend one interceptor flying to where the crosshair is RIGHT NOW
    if (act_edge) {
        for (int i = 0; i < MAX_INT; i++)
            if (!g_intc[i].active) {
                fire(&g_intc[i], VW / 2, GROUND_Y, g_cx >> FP, g_cy >> FP, INT_SPEED, 0);
                g_events |= EV_LAUNCH;
                break;
            }
    }

    // spawn enemy fire: denser waves come from LCG-driven count and pacing, not a clock
    if (g_wave_remaining > 0) {
        if (g_spawn_timer > 0) g_spawn_timer--;
        else if (any_city_alive()) {
            int slot = -1;
            for (int i = 0; i < MAX_ENEMY; i++) if (!g_enemy[i].active) { slot = i; break; }
            if (slot >= 0) {
                int c;
                do { c = (int)rnd(N_CITIES); } while (!g_city[c]);
                int32_t speed = ENEMY_BASE_SPEED + (int32_t)(g_wave / 4);
                if (speed > 3) speed = 3;
                fire(&g_enemy[slot], (int32_t)rnd(VW), 0, city_x(c), GROUND_Y, speed, c);
                g_wave_remaining--;
                g_wave_active++;
                int var = g_spawn_base / 2; if (var < 1) var = 1;
                g_spawn_timer = (int16_t)(g_spawn_base / 2 + (int16_t)rnd((uint32_t)var));
            }
        }
    } else if (g_wave_active == 0 && any_city_alive()) {
        new_wave();
    }

    // enemy missiles: fly the fixed line, land on arrival
    for (int i = 0; i < MAX_ENEMY; i++) {
        Missile *m = &g_enemy[i];
        if (!m->active) continue;
        m->x += m->vx; m->y += m->vy; m->ttl--;
        if (m->ttl <= 0) {
            if (g_city[m->city]) { g_city[m->city] = 0; g_events |= EV_LOST; }
            spawn_blast(m->tx >> FP, m->ty >> FP, 0);   // ground impact: fireball only, no kill
            m->active = 0; g_wave_active--;
        }
    }

    // interceptors: fly the fixed line, detonate on arrival
    for (int i = 0; i < MAX_INT; i++) {
        Missile *m = &g_intc[i];
        if (!m->active) continue;
        m->x += m->vx; m->y += m->vy; m->ttl--;
        if (m->ttl <= 0) {
            spawn_blast(m->tx >> FP, m->ty >> FP, 1);   // player's intercept: this one kills
            m->active = 0;
            g_events |= EV_BLAST;
        }
    }

    // blasts: grow, hold near max, fade — and eat anything they cover meanwhile
    for (int i = 0; i < MAX_BLAST; i++) {
        Blast *b = &g_blast[i];
        if (!b->active) continue;
        b->t++;
        if (b->t <= BLAST_GROW) b->r = (int16_t)(BLAST_R * b->t / BLAST_GROW);
        else if (b->t <= BLAST_GROW + BLAST_HOLD)
            b->r = (int16_t)(BLAST_R - BLAST_R * (b->t - BLAST_GROW) / BLAST_HOLD);
        else { b->active = 0; continue; }
        if (b->r < 2 || !b->kill) continue;
        for (int j = 0; j < MAX_ENEMY; j++) {
            Missile *m = &g_enemy[j];
            if (!m->active) continue;
            int32_t dx = (m->x >> FP) - b->x, dy = (m->y >> FP) - b->y;
            if (dx * dx + dy * dy <= (int32_t)b->r * b->r) {
                m->active = 0; g_wave_active--;
                g_score += 100 + g_wave * 10;
                g_events |= EV_KILL;
            }
        }
    }

    if (!any_city_alive()) g_over = 1;

    g_checksum = g_checksum * 1000003u + g_score + g_wave * 97 + (uint32_t)(g_cx + g_cy);
    for (int i = 0; i < N_CITIES; i++) g_checksum = g_checksum * 31 + g_city[i];
    for (int i = 0; i < MAX_ENEMY; i++) if (g_enemy[i].active) g_checksum = g_checksum * 31 + (uint32_t)(g_enemy[i].x + g_enemy[i].y);
    for (int i = 0; i < MAX_INT; i++)   if (g_intc[i].active)  g_checksum = g_checksum * 31 + (uint32_t)(g_intc[i].x + g_intc[i].y);
}

static void audio(void) {
    if (g_events & EV_LAUNCH) synth_note(NCHAN - 1, 5, 72, 130);
    if (g_events & EV_BLAST)  synth_note(NCHAN - 1, 3, 48, 200);
    if (g_events & EV_KILL)   synth_note(NCHAN - 1, 4, 84, 180);
    if (g_events & EV_LOST)   synth_note(NCHAN - 1, 3, 36, 220);
}

// ---- drawing: everything is authored in the VW x VH virtual grid, then scaled to
// whatever the real framebuffer size is — never hardcode a resolution here. ------------
static void vscale(int16_t *p, int n) {
    for (int i = 0; i < n * 2; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i]     * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1]  * g_fbh / VH);
    }
}
static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    vscale(p, 4);
    poly_fill(p, 4, ci);
}
static void thick_line(int x0, int y0, int x1, int y1, int hw, uint8_t ci) {
    int32_t dx = x1 - x0, dy = y1 - y0;
    int32_t len = isqrt32(dx * dx + dy * dy); if (len < 1) len = 1;
    int32_t nx = -dy * hw / len, ny = dx * hw / len;
    int16_t p[8] = { (int16_t)(x0 + nx), (int16_t)(y0 + ny), (int16_t)(x1 + nx), (int16_t)(y1 + ny),
                     (int16_t)(x1 - nx), (int16_t)(y1 - ny), (int16_t)(x0 - nx), (int16_t)(y0 - ny) };
    vscale(p, 4);
    poly_fill(p, 4, ci);
}
static void circle(int cx, int cy, int r, uint8_t ci) {
    if (r < 1) return;
    enum { NS = 16 };
    int16_t p[NS * 2];
    for (int k = 0; k < NS; k++) {
        int a = k * 1024 / NS;
        p[k * 2]     = (int16_t)(cx + ((int32_t)r * g_sin[a & 1023] >> 15));
        p[k * 2 + 1] = (int16_t)(cy + ((int32_t)r * g_sin[(a + 256) & 1023] >> 15));
    }
    vscale(p, NS);
    poly_fill(p, NS, ci);
}
static int digits(uint32_t v, char *b) {
    int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int s, uint32_t v, uint8_t ci) {
    char b[12]; digits(v, b);
    text_draw(x, y, s, b, ci);
}

static void draw(void) {
    fb_clear(0);
    // ground band
    quad(0, GROUND_Y, VW, VH - GROUND_Y, 1);

    // launch silo, decorative, always there
    int16_t silo[6] = { VW / 2 - 10, GROUND_Y, VW / 2, GROUND_Y - 14, VW / 2 + 10, GROUND_Y };
    vscale(silo, 3);
    poly_fill(silo, 3, 14);

    // cities: a standing skyline, or a low mound of rubble
    for (int i = 0; i < N_CITIES; i++) {
        int cx = (int)city_x(i);
        if (g_city[i]) {
            quad(cx - 16, GROUND_Y - 12, 32, 12, 2);
            quad(cx - 12, GROUND_Y - 20, 8, 8, 3);
            quad(cx + 2,  GROUND_Y - 17, 8, 5, 3);
        } else {
            quad(cx - 16, GROUND_Y - 4, 32, 4, 11);
        }
    }

    // enemy missiles: trail from spawn point to current head
    for (int i = 0; i < MAX_ENEMY; i++) {
        Missile *m = &g_enemy[i];
        if (!m->active) continue;
        int hx = (int)(m->x >> FP), hy = (int)(m->y >> FP);
        thick_line(m->ox, m->oy, hx, hy, 1, 4);
        circle(hx, hy, 3, 5);
    }

    // interceptors: trail from the silo to the current head
    for (int i = 0; i < MAX_INT; i++) {
        Missile *m = &g_intc[i];
        if (!m->active) continue;
        int hx = (int)(m->x >> FP), hy = (int)(m->y >> FP);
        thick_line(VW / 2, GROUND_Y, hx, hy, 1, 6);
        circle(hx, hy, 2, 7);
    }

    // blasts
    for (int i = 0; i < MAX_BLAST; i++) {
        Blast *b = &g_blast[i];
        if (!b->active || b->r < 1) continue;
        circle(b->x, b->y, b->r, 8);
        circle(b->x, b->y, b->r * 3 / 5, 9);
    }

    // crosshair
    int hx = (int)(g_cx >> FP), hy = (int)(g_cy >> FP);
    quad(hx - 7, hy - 1, 15, 2, 10);
    quad(hx - 1, hy - 7, 2, 15, 10);

    // HUD — kept clear of the top ~8% of the frame, which the window's title bar covers
    int s = g_fbh / 200; if (s < 1) s = 1;
    int hudy = g_fbh * 8 / 100;
    text_draw(6 * s, hudy, s, "SCORE", 13);
    num(6 * s, hudy + 8 * s, s, g_score, 12);
    int wx = g_fbw - text_width("WAVE 00", s) - 6 * s;
    text_draw(wx, hudy, s, "WAVE", 13);
    num(wx + text_width("WAVE ", s), hudy, s, g_wave, 12);

    if (g_over) {
        int cx2 = g_fbw / 2, cy2 = g_fbh / 2;
        text_draw(cx2 - text_width("GAME OVER", s * 3) / 2, cy2 - 14 * s, s * 3, "GAME OVER", 12);
        text_draw(cx2 - text_width("SPACE TO RESTART", s) / 2, cy2 + 16 * s, s, "SPACE TO RESTART", 13);
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_missile = { "missile", init, tick, audio, draw, checksum };
