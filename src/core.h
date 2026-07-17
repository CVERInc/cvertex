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
// can only cross in a straight line has doors you cannot reach: forms3's slot sits at z=0
// and nothing could ever walk to it.
typedef struct {
    int8_t  x, y;      // ground plane. In 2D games y is unused; in a menu it's the list.
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
