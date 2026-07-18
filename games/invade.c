// invaders.c — a grid that comes down.
//
// The whole shape of Space Invaders in one sentence: the grid gets closer every time it
// turns around. Nothing about it needs 3D or a mesh — it's a formation, a cannon, and two
// kinds of bullet, which is all a flat 2D game is ever made of.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- the world (virtual pixels, VW x VH from core.h) ------------------------
#define ROWS 5
#define COLS 8
#define NINV (ROWS * COLS)
#define CELL_W 34
#define CELL_H 22
#define INV_W  16
#define INV_H  10
#define BASE_X 70
#define BASE_Y 58   // clears the HUD line (drawn at g_fbh*10/100) at any resolution
// The grid's leftmost column sits at BASE_X + grid_x; MAXX is how far it may drift right
// before the rightmost column would leave room to turn.
#define MAXX (VW - 2 * BASE_X - ((COLS - 1) * CELL_W + INV_W))
#define STEP 6      // horizontal march, one step per move-tick
#define DROP 8      // how far the formation drops each time it turns

#define PLAYER_W 22
#define PLAYER_H 10
#define PLAYER_Y (VH - 34)
#define PLAYER_SPEED 3

#define PBULLET_SPEED 8
#define EBULLET_SPEED 4
#define MAXEB 4
#define START_LIVES 3

typedef struct { int32_t x, y; uint8_t active; } Bullet;

static uint8_t g_alive[ROWS][COLS];
static int32_t g_grid_x, g_grid_y;
static int8_t  g_dir;             // -1 left, +1 right
static int32_t g_move_timer;
static int     g_alive_count;

static int32_t g_player_x;
static uint8_t g_lives;
static uint32_t g_score, g_hi;

static Bullet g_pbullet;
static Bullet g_ebullet[MAXEB];
static int32_t g_espawn_timer;
static uint32_t g_rng;

static uint8_t g_over, g_won;
static uint64_t g_checksum;
static uint8_t g_events;
#define EV_FIRE  1
#define EV_KILL  2
#define EV_HIT   4
#define EV_OVER  8
#define EV_WIN  16

static uint32_t lcg(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng; }

static void init(void) {
    tables_init();
    uint32_t hi = g_hi;   // a high score survives the run that just ended
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            g_alive[r][c] = 1;
    g_alive_count = NINV;
    g_grid_x = 0; g_grid_y = 0; g_dir = 1;
    g_move_timer = 30;

    g_player_x = VW / 2 - PLAYER_W / 2;
    g_lives = START_LIVES;
    g_score = 0; g_hi = hi;

    g_pbullet.active = 0;
    for (int i = 0; i < MAXEB; i++) g_ebullet[i].active = 0;
    g_espawn_timer = 40;
    g_rng = 0x9E3779B9u;

    g_over = 0; g_won = 0;
    g_checksum = 0;
    g_events = 0;

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0]  = 0xFF0B0E1A;   // sky
    g_pal[2]  = 0xFF3DDC5A;   // player cannon
    g_pal[3]  = 0xFF7DE3FF;   // player bullet
    g_pal[4]  = 0xFFFF5C5C;   // enemy bullet
    g_pal[5]  = 0xFFEDEDF2;   // HUD text
    g_pal[6]  = 0xFFEF7D57;   // invader rows 0-1 (worth most)
    g_pal[7]  = 0xFF41A6F6;   // invader rows 2-3
    g_pal[8]  = 0xFFB13E53;   // invader row 4 (worth least)
    g_pal[9]  = 0xFF2A2C3E;   // end-screen bar
    g_pal[10] = 0xFFF5F5F8;   // end-screen headline
}

// row score & colour: the front rank is worth the least, the back rank the most — the
// ones that take longest to reach pay off best.
static int row_score(int r)  { return (r + 1) * 10; }
static uint8_t row_ci(int r) { return (uint8_t)(r < 2 ? 6 : r < 4 ? 7 : 8); }

static int32_t inv_x(int c) { return BASE_X + g_grid_x + c * CELL_W; }
static int32_t inv_y(int r) { return BASE_Y + g_grid_y + r * CELL_H; }

static void kill_invader(int r, int c) {
    g_alive[r][c] = 0;
    g_alive_count--;
    g_score += (uint32_t)row_score(r);
    if (g_score > g_hi) g_hi = g_score;
    g_events |= EV_KILL;
    if (g_alive_count == 0) { g_over = 1; g_won = 1; g_events |= EV_WIN; }
}

static void tick_formation(void) {
    // Fewer invaders left, faster the survivors march — the classic squeeze.
    int interval = 6 + g_alive_count * 24 / NINV;
    if (--g_move_timer > 0) return;
    g_move_timer = interval;

    int would_cross = (g_dir > 0) ? (g_grid_x + STEP > MAXX) : (g_grid_x - STEP < 0);
    if (would_cross) {
        g_grid_y += DROP;
        g_dir = (int8_t)-g_dir;
    } else {
        g_grid_x += g_dir * STEP;
    }

    // The formation reached the cannon — invaded, game over regardless of lives left.
    int32_t bottom = inv_y(ROWS - 1) + INV_H;
    if (bottom >= PLAYER_Y && !g_over) { g_over = 1; g_won = 0; g_events |= EV_OVER; }
}

static void tick_pbullet(const Input p) {
    if (p.jump && !g_pbullet.active) {
        g_pbullet.active = 1;
        g_pbullet.x = g_player_x + PLAYER_W / 2 - 1;
        g_pbullet.y = PLAYER_Y - 4;
        g_events |= EV_FIRE;
    }
    if (!g_pbullet.active) return;
    g_pbullet.y -= PBULLET_SPEED;
    if (g_pbullet.y < 0) { g_pbullet.active = 0; return; }
    for (int r = 0; r < ROWS && g_pbullet.active; r++)
        for (int c = 0; c < COLS && g_pbullet.active; c++) {
            if (!g_alive[r][c]) continue;
            int32_t ix = inv_x(c), iy = inv_y(r);
            if (g_pbullet.x >= ix && g_pbullet.x < ix + INV_W &&
                g_pbullet.y >= iy && g_pbullet.y < iy + INV_H) {
                kill_invader(r, c);
                g_pbullet.active = 0;
            }
        }
}

static void spawn_ebullet(void) {
    if (g_alive_count == 0) return;
    int col = (int)(lcg() >> 16) % COLS;
    int found = -1;
    for (int k = 0; k < COLS; k++) {
        int c = (col + k) % COLS;
        for (int r = ROWS - 1; r >= 0; r--)
            if (g_alive[r][c]) { found = c; break; }
        if (found >= 0) { col = found; break; }
    }
    if (found < 0) return;
    int row = -1;
    for (int r = ROWS - 1; r >= 0; r--) if (g_alive[r][col]) { row = r; break; }
    if (row < 0) return;
    for (int i = 0; i < MAXEB; i++) {
        if (g_ebullet[i].active) continue;
        g_ebullet[i].active = 1;
        g_ebullet[i].x = inv_x(col) + INV_W / 2;
        g_ebullet[i].y = inv_y(row) + INV_H;
        return;
    }
}

static void tick_ebullets(void) {
    int interval = 15 + g_alive_count * 45 / NINV;
    if (--g_espawn_timer <= 0) { g_espawn_timer = interval; spawn_ebullet(); }

    for (int i = 0; i < MAXEB; i++) {
        Bullet *b = &g_ebullet[i];
        if (!b->active) continue;
        b->y += EBULLET_SPEED;
        if (b->y > VH) { b->active = 0; continue; }
        if (b->x >= g_player_x && b->x < g_player_x + PLAYER_W &&
            b->y >= PLAYER_Y && b->y < PLAYER_Y + PLAYER_H + 6) {
            b->active = 0;
            g_events |= EV_HIT;
            if (g_lives > 0) g_lives--;
            if (g_lives == 0 && !g_over) { g_over = 1; g_won = 0; g_events |= EV_OVER; }
        }
    }
}

static void tick(const Input in[2]) {
    g_events = 0;
    Input p = input_1p(in);

    if (g_over) {
        // Frozen on the result. Space starts clean — same button that fired the gun.
        if (p.jump) init();
        return;
    }

    g_player_x += p.x * PLAYER_SPEED;
    if (g_player_x < 4) g_player_x = 4;
    if (g_player_x > VW - 4 - PLAYER_W) g_player_x = VW - 4 - PLAYER_W;

    tick_formation();
    if (!g_over) tick_pbullet(p);
    if (!g_over) tick_ebullets();

    g_checksum = g_checksum * 31 + (uint32_t)g_player_x + g_score + g_grid_x * 3u
               + (uint32_t)g_grid_y * 7u + g_alive_count + g_lives;
}

static void audio(void) {
    if (g_events & EV_FIRE) synth_note(NCHAN - 1, 0, 72, 150);
    if (g_events & EV_KILL) synth_note(NCHAN - 1, 4, 64, 200);
    if (g_events & EV_HIT)  synth_note(NCHAN - 1, 3, 40, 220);
    if (g_events & EV_OVER) synth_note(NCHAN - 1, 6, 36, 200);
    if (g_events & EV_WIN)  synth_note(NCHAN - 1, 1, 76, 220);
}

// ---- drawing ------------------------------------------------------------------
static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t pt[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                      (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    // Sim thinks in virtual pixels (VW x VH); the framebuffer may be any size.
    for (int i = 0; i < 8; i += 2) {
        pt[i]     = (int16_t)((int32_t)pt[i] * g_fbw / VW);
        pt[i + 1] = (int16_t)((int32_t)pt[i + 1] * g_fbh / VH);
    }
    poly_fill(pt, 4, ci);
}

static void draw_player(void) {
    quad((int)g_player_x, PLAYER_Y + 4, PLAYER_W, PLAYER_H - 4, 2);
    quad((int)g_player_x + PLAYER_W / 2 - 3, PLAYER_Y, 6, 6, 2);
}

static void draw_invaders(void) {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            if (!g_alive[r][c]) continue;
            int32_t x = inv_x(c), y = inv_y(r);
            uint8_t ci = row_ci(r);
            quad((int)x, (int)y, INV_W, INV_H, ci);
            quad((int)x + 3, (int)y + 3, 2, 2, 0);           // eyes: two notches punched in
            quad((int)x + INV_W - 5, (int)y + 3, 2, 2, 0);
        }
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

// 🔴 Rule 2: nothing important in the top ~8% of the frame — a window's title bar sits
// there. The HUD is pinned at g_fbh*10/100, comfortably below that band at any resolution.
static void draw_hud(void) {
    int s = g_fbh / 180; if (s < 1) s = 1;
    int y = g_fbh * 10 / 100;
    int x = 8 * s;

    text_draw(x, y, s, "SCORE", 5);
    num(x + text_width("SCORE ", s), y, s, g_score, 5);

    int lx = g_fbw / 2 - text_width("LIVES 0", s) / 2;
    text_draw(lx, y, s, "LIVES", 5);
    num(lx + text_width("LIVES ", s), y, s, g_lives, 5);

    const char *hi = "HI";
    int hx = g_fbw - x - text_width("HI 000000", s);
    text_draw(hx, y, s, hi, 5);
    num(hx + text_width("HI ", s), y, s, g_hi, 5);
}

static void draw_end(void) {
    int s = g_fbh / 180; if (s < 1) s = 1;
    int cx = g_fbw / 2, cy = g_fbh / 2;

    for (int py = cy - 26 * s; py < cy + 30 * s; py++)
        for (int px = 0; px < g_fbw; px++)
            if (py >= 0 && py < g_fbh) g_fb[py * g_fbw + px] = 9;

    const char *head = g_won ? "YOU WIN" : "GAME OVER";
    text_draw(cx - text_width(head, s * 3) / 2, cy - 20 * s, s * 3, head, 10);

    char b[24] = "SCORE ";
    char digs[12]; digits(g_score, digs);
    int n = 6; for (int i = 0; digs[i]; i++) b[n++] = digs[i]; b[n] = 0;
    text_draw(cx - text_width(b, s * 2) / 2, cy + 2 * s, s * 2, b, 5);

    text_draw(cx - text_width("SPACE TO RESTART", s) / 2, cy + 20 * s, s, "SPACE TO RESTART", 5);
}

static void draw(void) {
    fb_clear(0);
    draw_invaders();
    draw_player();
    if (g_pbullet.active) quad((int)g_pbullet.x, (int)g_pbullet.y, 2, 6, 3);
    for (int i = 0; i < MAXEB; i++)
        if (g_ebullet[i].active) quad((int)g_ebullet[i].x, (int)g_ebullet[i].y, 2, 6, 4);
    draw_hud();
    if (g_over) draw_end();
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_invade = { "invade", init, tick, audio, draw, checksum };
