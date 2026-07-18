// menu.c — pick a game.
//
// The picker is itself a game, which is the answer the Game contract was already
// suggesting: the platform layer runs one of these, and there's no reason the one it runs
// can't be the one that chooses. The engine gained exactly one thing for this — a game may
// ask to be replaced — and that's a fact about the platform, not about menus.
//
// The font used to live here, under a note saying it would move to src/ the moment a
// second game wanted text. The runner wanted text. It moved.
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// ---- the menu ---------------------------------------------------------------

static int g_sel, g_cool;
static uint32_t g_frame;

static const Game *const *g_list;
static int g_n;
void menu_populate(const Game *const *list, int n) { g_list = list; g_n = n; }

static void init(void) {
    tables_init();
    g_frame = 0; g_cool = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;   // background
    g_pal[1] = 0xFF4A4458;   // an unselected name
    g_pal[2] = 0xFFF5F5F8;   // the selected one
    g_pal[3] = 0xFFEF7D57;   // the caret
    g_pal[4] = 0xFF41A6F6;   // the title
    g_pal[5] = 0xFF2A2C3E;   // the selected row's bar
}

static void tick(const Input in[2]) {
    g_frame++;
    // A list is a list, so up and down are the whole interface — and left/right do the
    // same thing, because someone will try them.
    int y = in[0].y + in[1].y, x = in[0].x + in[1].x;
    int mv = y ? (y > 0 ? -1 : 1) : (x ? (x > 0 ? 1 : -1) : 0);
    if (g_cool > 0) { g_cool--; return; }
    if (mv) { g_sel = (g_sel + mv + g_n) % g_n; g_cool = 10; }
    if ((in[0].jump || in[1].jump) && g_n) g_switch_to = g_list[g_sel];
}

static void audio(void) {}

static void draw(void) {
    fb_clear(0);
    int s = g_fbh / 180;                 // one scale for any resolution
    if (s < 1) s = 1;
    int cx = g_fbw / 2;

    text_draw(cx - text_width("CVERTEX", s * 3) / 2, g_fbh / 6, s * 3, "CVERTEX", 4);

    int top = g_fbh / 2 - g_n * 12 * s / 2;
    for (int i = 0; i < g_n; i++) {
        int y = top + i * 12 * s;
        if (i == g_sel) {
            for (int py = y - 2 * s; py < y + 9 * s; py++)
                for (int px = cx - 60 * s; px < cx + 60 * s; px++)
                    if (px >= 0 && px < g_fbw && py >= 0 && py < g_fbh)
                        g_fb[py * g_fbw + px] = 5;
            // A caret that blinks, so a still screenshot still says which one is live.
            if ((g_frame / 20) & 1) text_put(cx - 52 * s, y, s, '-', 3);
        }
        text_draw(cx - text_width(g_list[i]->name, s) / 2, y, s,
             g_list[i]->name, (uint8_t)(i == g_sel ? 2 : 1));
    }

    text_draw(cx - text_width("ARROWS OR WASD   SPACE TO START   ESC QUITS", s) / 2,
         g_fbh - g_fbh / 6, s, "ARROWS OR WASD   SPACE TO START   ESC QUITS", 1);
}

static uint64_t checksum(void) { return (uint64_t)g_sel; }

const Game game_menu = { "menu", init, tick, audio, draw, checksum };
