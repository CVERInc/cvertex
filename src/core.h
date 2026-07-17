// core.h — engine core. Zero dependencies, only stdint.
#ifndef CORE_H
#define CORE_H
#include <stdint.h>

// 640x360, not 320x180. The framebuffer lives in __bss — zerofill — so its size costs
// exactly nothing on disk, and the art is vector, so there is no upper resolution to
// respect. Retro instinct says 320x180; the budget says it never had to.
#define FBW 640
#define FBH 360
#define MAXPTS 256   // max intersections on one scanline. A traced outline crosses many times — don't size this by hand-drawn-polygon intuition.

// Palette-indexed framebuffer: one byte per pixel.
extern uint8_t  g_fb[FBW * FBH];
extern uint32_t g_pal[256];   // 0xAARRGGBB

// Sine lookup table. The synth uses it for waveforms, 3D uses it for rotation — one
// table, because that's the same thing underneath.
// 🔴 It lives in core, not synth: sharing it is a dividend, but ownership has to be
// explicit, or 3D silently depends on audio's init order (learned the hard way:
// --dump skipped synth_init, and the whole cube collapsed to a point).
extern int16_t g_sin[1024];   // Q15: ±32767 = ±1.0
void tables_init(void);       // called by sim_init; the synth depends on it too

// Input: one struct per character. State changes only through this struct.
typedef struct { int8_t x, y; uint8_t act; } Input;

// Events the sim emits; the platform layer consumes them into sound. The sim doesn't
// know the synth exists → the audio thread can't contaminate determinism.
extern uint8_t  g_events;
extern uint32_t g_frame;   // frame count. Read this to animate — never the clock.
#define EV_JUMP_A 1
#define EV_JUMP_B 2
#define EV_LAND_A 4
#define EV_LAND_B 8

// Deterministic simulation: same input sequence → same frame. No random, no floats,
// no clock reads.
void sim_init(void);
void sim_tick(const Input in[2]);
void sim_draw(void);

// Polygon scanline fill (even-odd). pts = x0,y0,x1,y1,... 16.0 fixed point.
void poly_fill(const int16_t *pts, int n, uint8_t ci);

// Multi-contour version: lens[] holds the point count per contour. The even-odd rule
// makes holes fall out for free — a scanline sees every contour's edges at once,
// crossing into the outer contour counts 1, crossing into the inner one counts 2 →
// not filled. No "this is a hole" flag needed, no winding test needed either: the
// rule does the arithmetic itself.
void poly_fill_n(const int16_t *pts, const uint16_t *lens, int nc, uint8_t ci);

void fb_clear(uint8_t ci);

#endif
