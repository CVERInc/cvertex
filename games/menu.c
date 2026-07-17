// menu.c — pick a game.
//
// The picker is itself a game, which is the answer the Game contract was already
// suggesting: the platform layer runs one of these, and there's no reason the one it runs
// can't be the one that chooses. The engine gained exactly one thing for this — a game may
// ask to be replaced — and that's a fact about the platform, not about menus.
//
// The font lives here, not in the engine. Text is a capability every game eventually
// wants, and the moment a second one asks, it earns a move to src/. Until then an engine
// with a font in it is an engine with an opinion about letters.
#include "core.h"
#include "game.h"
#include "synth.h"
#include <string.h>

// 5x7, one byte per column, bit 0 = top. Only what the names need; adding a glyph is
// adding five numbers.
static const struct { char c; uint8_t col[5]; } GLYPH[] = {
    { 'A', { 0x7E,0x11,0x11,0x11,0x7E } }, { 'B', { 0x7F,0x49,0x49,0x49,0x36 } },
    { 'C', { 0x3E,0x41,0x41,0x41,0x22 } }, { 'D', { 0x7F,0x41,0x41,0x22,0x1C } },
    { 'E', { 0x7F,0x49,0x49,0x49,0x41 } }, { 'F', { 0x7F,0x09,0x09,0x09,0x01 } },
    { 'G', { 0x3E,0x41,0x49,0x49,0x7A } }, { 'H', { 0x7F,0x08,0x08,0x08,0x7F } },
    { 'I', { 0x00,0x41,0x7F,0x41,0x00 } }, { 'J', { 0x20,0x40,0x41,0x3F,0x01 } },
    { 'K', { 0x7F,0x08,0x14,0x22,0x41 } }, { 'L', { 0x7F,0x40,0x40,0x40,0x40 } },
    { 'M', { 0x7F,0x02,0x0C,0x02,0x7F } }, { 'N', { 0x7F,0x04,0x08,0x10,0x7F } },
    { 'O', { 0x3E,0x41,0x41,0x41,0x3E } }, { 'P', { 0x7F,0x09,0x09,0x09,0x06 } },
    { 'Q', { 0x3E,0x41,0x51,0x21,0x5E } }, { 'R', { 0x7F,0x09,0x19,0x29,0x46 } },
    { 'S', { 0x46,0x49,0x49,0x49,0x31 } }, { 'T', { 0x01,0x01,0x7F,0x01,0x01 } },
    { 'U', { 0x3F,0x40,0x40,0x40,0x3F } }, { 'V', { 0x1F,0x20,0x40,0x20,0x1F } },
    { 'W', { 0x7F,0x20,0x18,0x20,0x7F } }, { 'X', { 0x63,0x14,0x08,0x14,0x63 } },
    { 'Y', { 0x03,0x04,0x78,0x04,0x03 } }, { 'Z', { 0x61,0x51,0x49,0x45,0x43 } },
    { '0', { 0x3E,0x51,0x49,0x45,0x3E } }, { '1', { 0x00,0x42,0x7F,0x40,0x00 } },
    { '2', { 0x42,0x61,0x51,0x49,0x46 } }, { '3', { 0x21,0x41,0x45,0x4B,0x31 } },
    { '4', { 0x18,0x14,0x12,0x7F,0x10 } }, { '5', { 0x27,0x45,0x45,0x45,0x39 } },
    { '6', { 0x3C,0x4A,0x49,0x49,0x30 } }, { '7', { 0x01,0x71,0x09,0x05,0x03 } },
    { '8', { 0x36,0x49,0x49,0x49,0x36 } }, { '9', { 0x06,0x49,0x49,0x29,0x1E } },
    { '-', { 0x08,0x08,0x08,0x08,0x08 } }, { '.', { 0x00,0x60,0x60,0x00,0x00 } },
};
#define NGLYPH (int)(sizeof GLYPH / sizeof GLYPH[0])

static void put(int x, int y, int s, char c, uint8_t ci) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (int g = 0; g < NGLYPH; g++) {
        if (GLYPH[g].c != c) continue;
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (GLYPH[g].col[col] & (1 << row))
                    for (int dy = 0; dy < s; dy++)
                        for (int dx = 0; dx < s; dx++) {
                            int px = x + col * s + dx, py = y + row * s + dy;
                            if (px >= 0 && px < g_fbw && py >= 0 && py < g_fbh)
                                g_fb[py * g_fbw + px] = ci;
                        }
        return;
    }
}

static void text(int x, int y, int s, const char *str, uint8_t ci) {
    for (const char *p = str; *p; p++, x += 6 * s) put(x, y, s, *p, ci);
}
static int width(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }

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
    // The list is a list, so up and down are the whole interface.
    int mv = (in[0].y || in[1].y) ? -1 : 0;
    if (in[0].x || in[1].x) mv = (in[0].x + in[1].x) > 0 ? 1 : -1;
    if (g_cool > 0) { g_cool--; return; }
    if (mv) {
        g_sel = (g_sel + mv + g_n) % g_n;
        g_cool = 10;
    }
    if ((in[0].act || in[1].act) && g_n) g_switch_to = g_list[g_sel];
}

static void audio(void) {}

static void draw(void) {
    fb_clear(0);
    int s = g_fbh / 180;                 // one scale for any resolution
    if (s < 1) s = 1;
    int cx = g_fbw / 2;

    text(cx - width("CVERTEX", s * 3) / 2, g_fbh / 6, s * 3, "CVERTEX", 4);

    int top = g_fbh / 2 - g_n * 12 * s / 2;
    for (int i = 0; i < g_n; i++) {
        int y = top + i * 12 * s;
        if (i == g_sel) {
            for (int py = y - 2 * s; py < y + 9 * s; py++)
                for (int px = cx - 60 * s; px < cx + 60 * s; px++)
                    if (px >= 0 && px < g_fbw && py >= 0 && py < g_fbh)
                        g_fb[py * g_fbw + px] = 5;
            // A caret that blinks, so a still screenshot still says which one is live.
            if ((g_frame / 20) & 1) put(cx - 52 * s, y, s, '-', 3);
        }
        text(cx - width(g_list[i]->name, s) / 2, y, s,
             g_list[i]->name, (uint8_t)(i == g_sel ? 2 : 1));
    }

    text(cx - width("ARROWS  W TO START  ESC QUITS", s) / 2, g_fbh - g_fbh / 6, s,
         "ARROWS  W TO START  ESC QUITS", 1);
}

static uint64_t checksum(void) { return (uint64_t)g_sel; }

const Game game_menu = { "menu", init, tick, audio, draw, checksum };
