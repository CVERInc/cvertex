// game.h — the contract between the engine and whatever it happens to be running.
//
// The engine is core/g3d/shape/synth plus a platform layer. It knows about pixels,
// polygons, palettes, sound and time. It knows nothing about gravity, or a character, or
// a rule. Everything on THAT side of the line is a game, and games are swappable —
// otherwise every experiment means surgery on the engine, and the engine slowly becomes
// a pile of whichever game was most recent.
//
// The split is not tidiness. The engine will outlive every game built on it.
#ifndef GAME_H
#define GAME_H
#include <stdint.h>
#include "core.h"

typedef struct {
    const char *name;

    void (*init)(void);

    // 🔴 Pure. No clock, no random, no reading the synth, no touching the display.
    // Same inputs from the same init must give the same state, forever, on any machine,
    // at any resolution. Replays and lockstep co-op are downstream of this one rule.
    void (*tick)(const Input in[2]);

    // Called right after tick, on the same thread. This is where a game is allowed to
    // make noise: tick decides that something happened, audio decides what it sounds
    // like. Keeping them apart is what stops the audio thread reaching determinism.
    void (*audio)(void);

    void (*draw)(void);

    // For --headless: the state, hashed. If this changes when it shouldn't, something
    // that isn't allowed to has got into tick.
    uint64_t (*checksum)(void);

    // Who made this cartridge — shown on the shelf. cvertex is a community console, so a
    // contributor signs their work here. Optional: NULL reads as anonymous. It goes LAST so a
    // cartridge written before this field existed still compiles — its author is simply NULL —
    // and adding it never disturbs the positional initializers already out there.
    const char *author;
} Game;

// A game may ask to be replaced by another. The platform layer honours it between frames
// and re-inits — which is the whole of "switching games", and it's a fact about the
// platform, not about menus. Set it and stop caring; you won't get another tick.
extern const Game *g_switch_to;

// The other half of the two-stage Esc, and both platform facts like g_switch_to. On Esc inside
// a game the platform swaps back to the menu AND raises g_menu_return, which the menu reads once
// in init() to enter its reverse-insert — the cart rising back OUT of the console slot — instead
// of the cold boot fly-in, landing on the shelf at the very cart you had selected. On Esc inside
// the menu the menu plays its CRT power-off and then raises g_quit, which the platform reads to
// stop the run. (win.c / lnx.c must route Esc and honour g_quit the same way to match.)
extern int g_menu_return;   // platform -> menu: we returned from a game; play the insert in reverse
extern int g_quit;          // menu -> platform: the CRT power-off finished; stop the loop

// Every game defines a `const Game game_<name>` in its own games/<name>.c. The platform layer does
// NOT name them here — tools/gen-games.sh scans games/*.c and writes the roster into games.gen.h, so
// dropping a .c in the folder is all it takes to add a cartridge. The one exception is the shell
// itself, which the platform reaches for by name as the default screen:
extern const Game game_menu;
void menu_populate(const Game *const *list, int n);

// A single-player game reads one control scheme, but a player reaches for WASD or the arrows without
// thinking. Merge both pads into one so either drives, with Space OR Enter as the action button —
// so no single-player cartridge has to care which half of the keyboard the player picked.
static inline Input input_1p(const Input in[2]) {
    Input p;
    p.x    = in[0].x ? in[0].x : in[1].x;
    p.y    = in[0].y ? in[0].y : in[1].y;
    // The look axis rides along the same "either pad drives" rule as move — a single-scheme
    // game that wants a look stick gets it from whichever half filled it. Keyboard never fills
    // in[1].rx/ry (there is no second look on the keys), so in practice this is in[0]'s look.
    p.rx   = in[0].rx ? in[0].rx : in[1].rx;
    p.ry   = in[0].ry ? in[0].ry : in[1].ry;
    p.jump = in[0].jump | in[1].jump;      // Space or Enter — the action button
    p.act  = in[0].act  | in[1].act;
    return p;
}

// 🔴 Any HUD a game paints at the top must clear this band. The window's title bar crowds the top
// edge, so a score drawn at y=0 reads as jammed under the chrome (or hidden by it). Put top-left
// HUD at hud_top() and below — never in the rows above it. Every cartridge follows this, so the
// rule lives here and not in anyone's memory.
static inline int hud_top(void) { return g_fbh * 8 / 100; }

#endif
