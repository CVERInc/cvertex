// fx.h — the light chip. An OPT-IN present-time RGBA post-pass, in the spirit of the
// '90s cartridge coprocessor: the base console never pays for it, a cartridge that wants
// it plugs it into the socket (g_present_fx, see core.h) in its init().
//
// Screen-space bloom: bright pixels bleed light into their neighbours. A palette-cell
// compositor — an SFC PPU — fundamentally cannot do this; it places opaque palette cells
// and light stops at the cell edge. Glow that crosses pixel boundaries is the line between
// "SFC" and "impossible SFC", and it costs only CPU, which this console has to spare.
//
// 🔴 Draw-side only. It runs on the RGBA buffer AFTER the indexed framebuffer is hashed
// (fb_checksum reads g_fb, the indices), so it can never reach the sim or determinism.
#ifndef FX_H
#define FX_H
#include <stdint.h>

// The pass itself — assign it to g_present_fx to arm the socket. w/h are the live framebuffer size.
void fx_bloom(uint32_t *rgba, int w, int h);

// thresh: luminance 0..255 where glow begins (only pixels brighter than this bloom). Set 255 to
// turn the luminance path OFF and bloom PURELY by the emissive mask below — which is what a
// day/night world wants, because sunlit sand is as bright as a night flame and a luminance gate
// can't tell them apart. radius: blur half-width in px. strength: additive gain, 0..256.
void fx_bloom_set(int thresh, int radius, int strength);

// The emissive mask: per-palette-INDEX glow, 0..255 (0 = this colour never blooms). The indexed
// framebuffer still knows every pixel's palette index at present time, so the artist declares WHICH
// colours are light sources — the sun disc, the moon, fire, an amber eye — and those bloom in broad
// daylight or pitch dark alike, while diffuse snow and sand never do however bright they render.
// Pass NULL to clear it. This is the palette console's answer to HDR: the emissive channel is the
// palette, not a float. The platform clears it with the socket on every game switch.
void fx_bloom_emissive(const uint8_t mask[256]);

// Depth of field. strength is the aperture gain (0 = OFF). It reads the engine depth buffer (g_zb)
// directly and autofocuses on the centre pixel, so whatever the camera looks at stays sharp while
// everything nearer or farther blurs — the lens trick that reads a blocky world as a held miniature.
// No per-frame setter needed beyond this; focus is found from the depth buffer each present.
void fx_dof_set(int strength);

#endif
