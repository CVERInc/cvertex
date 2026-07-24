// fx.c — screen-space bloom. See fx.h for why a palette console gets a light chip at all.
#include "fx.h"
#include "core.h"     // MAXFBW, MAXFBH, g_fb, g_pal
#include <stdint.h>
#include <string.h>

static int s_thresh   = 255;   // luminance gate; 255 = OFF, bloom purely by the emissive mask (day/night safe)
static int s_radius   = 4;     // blur half-width, px
static int s_strength = 150;   // additive gain, 0..256

static uint8_t s_emis[256];    // per-index glow strength, 0 = never blooms. Set by fx_bloom_emissive.
static int     s_have_emis;    // any index marked? — lets a mask-less game fall back to luminance

void fx_bloom_set(int thresh, int radius, int strength) {
    s_thresh = thresh; s_radius = radius; s_strength = strength;
}

static int s_dof_str;          // depth-of-field aperture gain, 0 = off
void fx_dof_set(int strength) { s_dof_str = strength; }

void fx_bloom_emissive(const uint8_t mask[256]) {
    if (!mask) { memset(s_emis, 0, sizeof s_emis); s_have_emis = 0; return; }
    memcpy(s_emis, mask, sizeof s_emis);
    s_have_emis = 0;
    for (int i = 0; i < 256; i++) if (s_emis[i]) { s_have_emis = 1; break; }
}

// Two scratch planes, packed 0x00RRGGBB. __bss (zerofill) — reserving them costs the
// binary nothing, which is the whole reason a size-bound console can afford scratch at all.
static uint32_t s_a[MAXFBW * MAXFBH];
static uint32_t s_b[MAXFBW * MAXFBH];

// One separable box pass, src -> dst. horiz=1 blurs along rows, else down columns. A running
// sum makes it O(pixels), independent of radius — the '90s move, a box is a poor man's gaussian
// and two of them stacked is a decent one. Edges clamp-extend so the frame border doesn't darken.
static void box_pass(uint32_t *dst, const uint32_t *src, int w, int h, int r, int horiz) {
    int n     = horiz ? w : h;   // length of a line
    int lines = horiz ? h : w;   // how many lines
    int win   = 2 * r + 1;
    for (int L = 0; L < lines; L++) {
        int base = horiz ? L * w : L;
        int step = horiz ? 1 : w;
        int sr = 0, sg = 0, sb = 0;
        for (int k = -r; k <= r; k++) {                     // prime the window centred on i=0
            int idx = k < 0 ? 0 : k >= n ? n - 1 : k;
            uint32_t c = src[base + idx * step];
            sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
        }
        for (int i = 0; i < n; i++) {
            dst[base + i * step] = ((uint32_t)(sr / win) << 16) | ((uint32_t)(sg / win) << 8) | (uint32_t)(sb / win);
            int out = i - r;      out = out < 0 ? 0 : out >= n ? n - 1 : out;   // leaves the window
            int in  = i + r + 1;  in  = in  < 0 ? 0 : in  >= n ? n - 1 : in;    // enters it
            uint32_t co = src[base + out * step], ci = src[base + in * step];
            sr += (int)((ci >> 16) & 255) - (int)((co >> 16) & 255);
            sg += (int)((ci >>  8) & 255) - (int)((co >>  8) & 255);
            sb += (int)( ci        & 255) - (int)( co        & 255);
        }
    }
}

void fx_bloom(uint32_t *rgba, int w, int h) {
    int px = w * h;
    // Bright-pass. Two ways in: the emissive mask (exact — the pixel's palette index says whether
    // it's a light source, so daylight sand is spared and a night flame isn't), and, if no mask is
    // set, a luminance gate as a generic fallback. The mask wins where present; the index is right
    // there in g_fb because the framebuffer is still palette-indexed.
    for (int i = 0; i < px; i++) {
        uint32_t c = rgba[i];
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        int k = 0;
        if (s_have_emis) {
            k = s_emis[g_fb[i]];                          // declared glow strength for this colour
        } else if (s_thresh < 255) {
            int lum = (77 * r + 150 * g + 29 * b) >> 8;   // fallback: brightness gate with a 32-wide knee
            if (lum > s_thresh) { k = (lum - s_thresh) * 8; if (k > 255) k = 255; }
        }
        if (!k) { s_a[i] = 0; continue; }
        s_a[i] = ((uint32_t)(r * k / 255) << 16) | ((uint32_t)(g * k / 255) << 8) | (uint32_t)(b * k / 255);
    }
    // Two separable iterations ≈ a gaussian: h, v, h, v — ping-ponging a<->b, landing in s_a.
    box_pass(s_b, s_a, w, h, s_radius, 1);
    box_pass(s_a, s_b, w, h, s_radius, 0);
    box_pass(s_b, s_a, w, h, s_radius, 1);
    box_pass(s_a, s_b, w, h, s_radius, 0);
    // Composite: add the blurred light back, clamped. Additive is what light does — two glows
    // that overlap are brighter, and a bright source blows its own halo toward white.
    for (int i = 0; i < px; i++) {
        uint32_t o = rgba[i], glow = s_a[i];
        int r = (int)((o >> 16) & 255) + (int)((((glow >> 16) & 255) * s_strength) >> 8);
        int g = (int)((o >>  8) & 255) + (int)((((glow >>  8) & 255) * s_strength) >> 8);
        int b = (int)( o        & 255) + (int)((( glow        & 255) * s_strength) >> 8);
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        rgba[i] = (o & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    // 🔭 DEPTH OF FIELD, last — a camera lens, the thing that reads a blocky world as a MINIATURE (the
    // whole "world in your palm"). g_zb holds inverse depth (w ≈ 1/z, 0 = sky), and a thin lens's circle
    // of confusion is ∝ |1/z − 1/focus| — which is exactly |w − focus_w|, so the blur amount falls right
    // out of the depth buffer with no division. Autofocus on the centre pixel: whatever you look AT is
    // sharp, everything nearer or farther melts. A palette PPU has no per-pixel depth to do this with.
    if (s_dof_str > 0) {
        uint32_t fw = g_zb[(size_t)(h / 2) * w + (w / 2)];    // focus depth = what the crosshair rests on
        for (int i = 0; i < px; i++) s_a[i] = rgba[i] & 0x00FFFFFFu;   // a blurred copy of the whole frame…
        box_pass(s_b, s_a, w, h, 3, 1);
        box_pass(s_a, s_b, w, h, 3, 0);
        box_pass(s_b, s_a, w, h, 3, 1);
        box_pass(s_a, s_b, w, h, 3, 0);
        for (int i = 0; i < px; i++) {
            int coc = (int)g_zb[i] - (int)fw; if (coc < 0) coc = -coc;   // distance from the focal plane, in inverse-depth
            int m = (coc * s_dof_str) >> 10; if (m > 256) m = 256;       // …blended in by how far out of focus this pixel is
            if (!m) continue;
            uint32_t o = rgba[i], bl = s_a[i];
            int r = (int)((o >> 16) & 255) + ((((int)((bl >> 16) & 255) - (int)((o >> 16) & 255)) * m) >> 8);
            int g = (int)((o >>  8) & 255) + ((((int)((bl >>  8) & 255) - (int)((o >>  8) & 255)) * m) >> 8);
            int b = (int)( o        & 255) + ((((int)( bl        & 255) - (int)( o        & 255)) * m) >> 8);
            rgba[i] = (o & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}
