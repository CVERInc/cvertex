// core.h — engine core. Zero dependencies, only stdint.
#ifndef CORE_H
#define CORE_H
#include <stdint.h>

// The framebuffer is sized at RUNTIME. It lives in __bss — zerofill — so reserving the
// largest sane buffer costs exactly nothing on disk, and the art is vector, so there is
// no resolution it stops being correct at.
//
// That makes chunky-pixels vs sharp-vectors a number, not an architecture: run at 640x360
// and scale up and you get a deliberate 1994 pixel grid; run at the display's own
// resolution and the same polygons come out sharp. Same art, same binary, one integer.
// 🔴 The simulation lives here, in virtual pixels, and this NEVER changes with the
// framebuffer. If gameplay were measured in real pixels, the same inputs would produce
// different physics at a different resolution — and a replay recorded at 640x360 would
// desync at 1920x1080, taking lockstep co-op down with it. Determinism has to survive
// the display, so the display is not allowed to be part of it.
#define VW 640
#define VH 360

#define MAXFBW 3840
#define MAXFBH 2160
extern int g_fbw, g_fbh;
void fb_resize(int w, int h);
#define MAXPTS 256   // max intersections on one scanline. A traced outline crosses many times — don't size this by hand-drawn-polygon intuition.

// Palette-indexed framebuffer: one byte per pixel.
extern uint8_t  g_fb[MAXFBW * MAXFBH];
extern uint32_t g_pal[256];   // 0xAARRGGBB

// Sine lookup table. The synth uses it for waveforms, 3D uses it for rotation — one
// table, because that's the same thing underneath.
// 🔴 It lives in core, not synth: sharing it is a dividend, but ownership has to be
// explicit, or 3D silently depends on audio's init order (learned the hard way:
// --dump skipped synth_init, and the whole cube collapsed to a point).
extern int16_t g_sin[1024];   // Q15: ±32767 = ±1.0
void tables_init(void);       // called by sim_init; the synth depends on it too

// Input: one struct per character. What the platform layer hands a game, and the only way
// a game's state is allowed to change.
//
// 🔴 x and y are the GROUND. This struct was born 2D — x, and a y that meant "up" — and a
// 3D game needs two axes to stand on, so jump had to stop being one of them. A world you
// can only cross in a straight line has doors you cannot reach: a slot at z=0 sits off the
// line, and nothing could ever walk to it.
typedef struct {
    int8_t  x, y;      // ground plane / LEFT stick. In 2D games y is unused; in a menu it's the list.
    // 🔴 The LOOK axis, appended AFTER x,y so it maps 1:1 to a gamepad's RIGHT stick — and,
    // just as important, so every cartridge written against the old {x,y,jump,act} shape still
    // compiles and behaves untouched: it reads x,y,jump,act and never names rx,ry, so adding
    // these two bytes can't perturb it. A dual-stick game drives MOVE from x,y and LOOK
    // from rx,ry; the keyboard fills rx,ry from the arrow keys (see the platform read_input).
    int8_t  rx, ry;    // look plane / RIGHT stick. rx = yaw (turn), ry = pitch (up/down).
    uint8_t jump;      // space / return
    uint8_t act;       // the game's own verb — morph, grab, whatever it has
} Input;

// Polygon scanline fill (even-odd). pts = x0,y0,x1,y1,... 16.0 fixed point.
void poly_fill(const int16_t *pts, int n, uint8_t ci);

// Multi-contour version: lens[] holds the point count per contour. The even-odd rule
// makes holes fall out for free — a scanline sees every contour's edges at once,
// crossing into the outer contour counts 1, crossing into the inner one counts 2 →
// not filled. No "this is a hole" flag needed, no winding test needed either: the
// rule does the arithmetic itself.
void poly_fill_n(const int16_t *pts, const uint16_t *lens, int nc, uint8_t ci);

void fb_clear(uint8_t ci);

// A camera distance a game may let the command line override, so the near plane can
// actually be driven into. It's here rather than improvised per test because every
// throwaway harness written today broke; the ones with names never did.
extern int32_t g_dev_camz;   // 0 = the game decides

// ---- pointer ----------------------------------------------------------------
// The mouse, in FRAMEBUFFER pixels (g_fb's top-left origin), plus a button bitfield
// (bit0 = left, bit1 = right, bit2 = MIDDLE). The platform layer fills these; a cartridge that
// wants a pointer — the box editor — reads them. 🔴 Every existing game and the whole sim ignore
// these, so a pointer no one reads changes no determinism and no checksum: adding it is
// invisible to everything that came before. In --ppm/--headless the harness can drive them
// from a scripted track (--mouse), the pointer's answer to --keys.
extern int     g_mx, g_my;
extern uint8_t g_mbtn;

// The scroll wheel, ACCUMULATED in notches since a tool last read it: + is up (zoom in), − is
// down (zoom out). The platform layer adds to it (every notch counts at least ±1, so a trackpad's
// fine deltas still register); a tool — the editor's Maya-style camera — reads it and resets it to
// 0 each frame. Like the pointer it is UI state, never in the Input struct, never hashed, so no
// existing game or sim sees it. In --ppm/--headless the harness drives it from --scroll (u/d/.).
extern int     g_wheel;

// The number row (1..9, 0) as a ONE-FRAME digit pulse: the digit 0..9 on the frame its key goes
// down, else -1. The platform layer resets it to -1 each frame and sets it on the keydown edge (a
// held key can't strobe). Like the pointer and wheel it is UI state — never in the Input struct,
// never hashed — so no game or deterministic sim sees it unless it deliberately reaches for it. A
// cartridge can read it for a debug shortcut (e.g. a dev-gated jump); a plain game never touches it.
extern int     g_digit;

// ---- view toggle ------------------------------------------------------------
// A one-frame edge latch, exactly the mouse's bargain in a single bit: the platform layer
// PULSES it (mac.c sets it to 1 on a Tab keydown edge, keycode 48; --view pulses it from a
// scripted track), and a cartridge that wants a view/camera cycle CONSUMES it — reads it,
// acts, clears it back to 0. 🔴 Every existing game and the whole sim ignore it, so a latch
// no one reads changes no determinism and no checksum: adding it is invisible to everything
// that came before, same as the pointer. It is deliberately NOT part of the Input struct —
// Input is the sim contract, and a camera choice is a draw-side comfort setting, not a move.
extern uint8_t g_view_toggle;

// One-frame edge latch for the '/' key (keycode 44, which is also '?' with shift). The platform
// pulses it on the keydown edge; a cartridge consumes it to toggle an on-demand help/manual overlay.
// Same bargain as the view toggle — draw-side comfort, never in the Input struct, never hashed.
extern uint8_t g_help_toggle;

// One-frame edge latch for a DEBUG-overlay toggle (F3, the Minecraft convention). Same bargain as the
// view/help latches — the platform pulses it on the keydown edge, a cartridge consumes it to flip an
// on-demand developer overlay (position/angle/time/weather — the recipe to reproduce a screenshot).
// Draw-side comfort, never in the Input struct, never hashed: it steers no deterministic sim.
extern uint8_t g_debug_toggle;

// ---- Esc: the two-stage back button -----------------------------------------
// A one-frame edge latch, the same bargain as the view toggle: the platform PULSES it on an
// Esc keydown edge (mac.c keycode 53; --ppm/--dump can pulse it from CV_ESC_AT for headless
// renders). What it MEANS is decided by who consumes it: the menu reads it in its own tick and
// starts its CRT power-off; a real game ignores it, so the platform sees it still set after the
// tick and swaps back to the menu (raising g_menu_return). Not in Input — a back button is a
// shell gesture, never a move the deterministic sim is allowed to see.
extern uint8_t g_esc;

// ---- pre-game settings: the console's OPTIONS panel -------------------------
// The shell's OPTIONS screen (Space on the shelf) writes these; the launched game and the
// platform read them. 🔴 Every one defaults to today's behaviour, so a run that never opens
// OPTIONS is byte-identical to before — determinism and every cartridge's self-checks are untouched
// unless the player deliberately turns a knob. These are shell/comfort settings, deliberately
// NOT part of the Input sim contract (same reasoning as g_esc / g_view_toggle): a preference
// chosen BEFORE the game starts, read once, never a per-frame move the deterministic sim sees.
extern int g_gentle;      // GENTLE MODE: a cartridge's tamed creatures never starve-revert and never cannibalise (0 = off = today's rule)
extern int g_coop;        // CO-OP at launch: 0 SOLO, 1 HOST, 2 JOIN — the platform stands up lockstep net on the next game
extern int g_fullscreen;  // FULLSCREEN want: the platform toggles the live window to match when it changes (0 = windowed)
extern int g_crt_off;     // CRT POWER-OFF flourish on Esc-quit: 1 = the tube collapse plays, 0 = quit straight to black
extern int g_cam_chase;   // camera default a 3D game may read: 0 = first-person (today), 1 = chase

// ---- depth ------------------------------------------------------------------
// 🔴 A painter's algorithm sorted per triangle is not a depth test, it's a guess that
// usually agrees with one. Two triangles whose average depths cross swap places, and a
// far face paints over a near one for a frame — which reads as colours flickering, not
// as a sorting bug, which is why it survived so long.
//
// So: a real depth buffer. It's __bss, so it costs nothing on disk at any resolution,
// and it's period-correct — Quake shipped one.
//
// 1/z is what's interpolated, not z: only the reciprocal is linear in screen space.
// Larger means nearer.
extern uint32_t g_zb[MAXFBW * MAXFBH];
void zb_clear(void);

// A depth-tested triangle. p = x,y screen and w = (1<<30)/z per vertex.
void tri_fill_z(const int16_t *xy, const uint32_t *w, uint8_t ci);

#endif
