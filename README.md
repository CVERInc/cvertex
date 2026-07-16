# cvertex

> **Fits an entire 3D engine on a floppy disk.** cvertex (C + vertex) draws worlds
> out of shapes instead of bitmaps, so a game, its music, and the engine itself
> land in 1,474,560 bytes — with room to spare.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Language: C](https://img.shields.io/badge/Language-C11-orange.svg)
![Dependencies: none](https://img.shields.io/badge/Dependencies-none-brightgreen.svg)

---

## What & why

A 3.5" HD floppy holds 1,474,560 bytes. That number is the whole design brief.

It rules out the usual answers before you write a line: no engine runtime, no
textures, no sampled audio, no asset loader, no parser. What survives is
geometry, a palette, and arithmetic — which turns out to be enough for a
shaded, rotating 3D world with a soundtrack.

The constraint isn't nostalgia. Storage was never what stopped 1994 from
shipping full 3D — silicon was. A SNES needed an extra chip in the cartridge to
draw wireframes at all. The floppy still fits; the hardware caught up. cvertex
is what you get when you take the old budget and the new machine.

Current cost, macOS, everything below included:

| | binary | machine code |
|---|---|---|
| Empty Cocoa shell (control) | 33,592 | 0 |
| + rasterizer, sim, platform layer, input | 35,256 | 2,996 |
| + FM synth, tracker, music | 35,976 | 5,036 |
| + full 3D pipeline | **36,248** | **6,352** |
| Share of one floppy | **2.45%** | |

Almost all of that is Mach-O container overhead, not engine. The 3D pipeline —
rotation, projection, backface culling, depth sorting, flat-shaded lighting —
costs 1,316 bytes.

## Features

- **Palette-indexed software rendering.** One byte per pixel, 256 colours, no GPU.
- **Fixed-point 3D.** Not one float in the pipeline. Deterministic on every machine.
- **Synthesized audio.** 2-op FM plus classic waveforms. No wav, no ogg, no decoder.
- **Deterministic simulation.** Same inputs, same frame, forever. Replays and
  lockstep multiplayer come free.
- **Local co-op from day one.** The simulation never learns where input came from.
- **Zero dependencies.** A C compiler is the whole toolchain.

## Quick start

```sh
./build.sh && ./cvertex
```

A/D/W drives one character, arrow keys drive the other, Esc quits. Every build
prints its size against the floppy budget.

```sh
./cvertex --headless 3600   # same inputs must give the same checksum
./cvertex --dump 40         # print the framebuffer as ASCII
```

## How it works

### The palette is the foundation

Ordinary pixels store *what colour is here* — R235 G71 B71, four bytes. Palette
pixels store *which number is here* — one byte, and a 256-entry table says what
each number means.

Saving three bytes a pixel is the small win. The real one is that you can change
what number 2 *means* without touching a single pixel:

| To do this | Do this | Pixels touched |
|---|---|---|
| Fade out | Walk all 256 entries toward black | 0 |
| Flash on hit | Set that material's entry white for a frame | 0 |
| Dusk lighting | Bias the whole table orange | 0 |
| Shade a 3D face | Give each material 8 consecutive entries — one hue, eight brightnesses — and pick one by the surface normal | 0 |

Light and effects aren't computed, they're a 1 KB table you edit. That is why
the 3D pipeline is 1,316 bytes: it doesn't calculate light, it looks it up.

It also explains the art the engine wants — flat fills, hard edges, no
gradients. A gradient puts hundreds of near-identical skin tones on one face;
256 slots can't hold them and wouldn't gain anything if they could. Here the
constraint and the implementation are the same decision.

### 3D is a software Cx4

In 1994 Rockman X2 rendered wireframes with a Capcom Cx4 — a 20 MHz fixed-point
DSP whose instruction set literally included `Draw wireframe` and
`Transform Lines`. This is that, in software.

Rotation reads the same sine table the synth uses. Projection is one divide.
Triangles go through the same `poly_fill` the 2D path uses. Face normals live in
the model, because a unit normal stays a unit normal through a rotation — so
lighting never normalizes and never takes a square root.

Culling and lighting share one source of truth: the model normal. Flip a
model's normals and the object turns inside out, which is a mistake you can see
and fix in your modelling tool.

### Sound is arithmetic, not a file

2-op FM (a modulator offsetting a carrier's phase — the AdLib/OPL2 voice) plus
square, triangle, saw and LFSR noise, each with an ADSR envelope.

An instrument is 12 bytes. The demo song is a 32×4 table — 64 bytes. For scale,
a three-minute MP3 is roughly two floppies.

The simulation only emits events; the platform layer turns those into notes. The
simulation doesn't know the synth exists, so the audio thread can't touch
determinism.

### The platform layer answers five questions

Give me a window. Give me memory that reaches the screen. What key is down. What
time is it. Should I stop. The macOS layer is plain C calling the Objective-C
runtime directly — no Objective-C compiler involved.

Frameworks are dylibs on macOS, so linking one costs about 72 bytes. Audio is
effectively free.

### Assets are baked, not loaded

`tools/obj2mesh` turns any `.obj` into a `const` C array at build time. No
loader, no file format, no I/O, no error handling — and `const` data lives in a
`__TEXT` page that was already there.

Vertex normals in the file are ignored: flat shading wants face normals, so they
are computed from geometry. Any tool's `.obj` works, whether or not it writes
normals.

## Size discipline

Two rules earn their place in the budget.

**Static variables are zero-initialized; assign at runtime.** A non-zero
initializer creates `__data`, and macOS aligns segments to 16 KB — so four bytes
of initial value drag 16,384 bytes into the file. Zero-initialized data lives in
`__bss` and costs nothing on disk. The 320×180 framebuffer is free.

**Measure, don't guess.** Every build prints its size. The first synth clipped
through its whole range and sounded merely "a bit dirty"; the meter said peak
32768, RMS 23772, and the mix gain was off by three bits.

## Status

Early. It runs, and the shape of it is settled.

- macOS only. The Windows and Linux platform layers aren't written yet.
- Near-plane handling drops any triangle with a vertex behind the camera rather
  than clipping it properly. Invisible until geometry crosses the lens.
- Depth sorting is per-triangle painter's algorithm, so interpenetrating
  geometry can sort wrong.
- `--dump` and other development paths still compile into the binary.

## Development

```
src/core.c    framebuffer, polygon rasterizer, deterministic sim, sine table
src/g3d.c     fixed-point 3D pipeline
src/synth.c   FM synth, oscillators, tracker
src/mac.c     macOS platform layer
tools/        build-time asset converters
```

## License

MIT © CVER Inc.
