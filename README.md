# cvertex

> **Draws a world out of shapes instead of bitmaps.** cvertex (C + vertex) is a game
> engine you can read in an afternoon: a software rasterizer, a fixed-point 3D pipeline,
> an FM synth and a deterministic simulation, in a few thousand lines of C with no
> dependencies at all.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Language: C](https://img.shields.io/badge/Language-C11-orange.svg)
![Dependencies: none](https://img.shields.io/badge/Dependencies-none-brightgreen.svg)

---

## What & why

There is no engine runtime here, no textures, no sampled audio, no asset loader and no
parser. What's left is geometry, a palette, and arithmetic — which turns out to be enough
for a shaded, rotating 3D world with a soundtrack.

Every technique in it is 1994's: palette indices, scanline fills, fixed point, a
painter's algorithm. Not one bit is new. That was never what stopped 1994 — a SNES needed
an extra chip in the cartridge to draw wireframes at all, and Star Fox ran at fifteen
frames a second because of the silicon, not because anybody was short of ideas.

The silicon caught up. So the same arithmetic, on one core with no GPU:

| | |
|---|---|
| 320x180 | 18,047 fps |
| 640x360 | 12,774 fps |
| 1920x1080 | 2,138 fps |
| **3840x2160** | **540 fps** |

A 486 could not fill 8.3M pixels. This does it 540 times a second with nine frames of
headroom left over — running code its authors would have recognised on sight.

What that buys, beyond the number: an engine small enough to hold in your head. You can
read all of it, so you can trust all of it, and you can change any of it without
wondering what else knows about it. It isn't a big claim — this is all of it:

| | |
|---|---|
| `src/core.c` | framebuffer, palette, scanline fill |
| `src/g3d.c` | the fixed-point 3D pipeline |
| `src/shape.c` | vector art: extrusion, turnarounds |
| `src/synth.c` | FM synth, oscillators, tracker |
| `src/mac.c` | the macOS platform layer |
| `src/win.c` | the Windows platform layer |
| `src/lnx.c` | the Linux platform layer |
| `src/game.h` | the line between the engine and a game |
| `games/*.c` | one cartridge each; the folder is the roster |

The engine's machine code currently rounds to about eleven kilobytes, but that number is
`./build.sh`'s to report, not this file's to remember. A linked binary is much larger and
says much less: some 33 KB of it is Mach-O container overhead before a line of ours, and
baked artwork outweighs all the code. Neither is the engine.

## Features

- **Palette-indexed software rendering.** One byte per pixel, 256 colours, no GPU.
- **Fixed-point 3D.** Not one float in the pipeline. Deterministic on every machine.
- **Synthesized audio.** 2-op FM plus classic waveforms. No wav, no ogg, no decoder.
- **Deterministic simulation.** Same inputs, same frame, forever — at any resolution,
  because gameplay lives in a virtual space the display can't reach. Replays and lockstep
  multiplayer come free.
- **Vector characters.** Drawings in, polygons out, extruded into the 3D world. Turn to
  the drawn view nearest the facing, then keep rotating the rest of the way.
- **Chunky or sharp is a number.** 640x360 scaled up is a 1994 pixel grid; the display's
  own resolution is the same polygons, sharp. Same art, same binary.
- **Local co-op from day one.** The simulation never learns where input came from.
- **A game is one file.** Drop a `.c` in `games/`, rebuild, it's on the shelf. The folder is
  the roster; nothing is wired by hand.
- **Zero dependencies.** A C compiler is the whole toolchain.

## Quick start

The whole toolchain is a C compiler. Pick your platform (A–Z, no favourites):

**Linux** — X11 + ALSA (`sudo apt install gcc libx11-dev libasound2-dev`, or your distro's equivalent):

```sh
sh build-linux.sh && ./cvertex
```

**macOS** — clang (`xcode-select --install`):

```sh
./build.sh && ./cvertex
tools/bundle.sh && open cvertex.app   # ...or the same thing, double-clickable
```

**Windows** — [zig](https://ziglang.org) and Git for Windows, then in Git Bash:

```sh
sh build-win.sh            # produces cvertex.exe — double-click to play
```

The menu opens: arrows or WASD to browse the shelf, Space to start, Esc to quit.

> The Linux layer is new but well-exercised: it compiles native on real Linux (x86_64 **and**
> aarch64), and its headless simulation comes out **byte-identical across both architectures** and
> to the macOS/Windows builds — the same inputs, the same checksum, so the port never touches the
> games. The one piece still unconfirmed is the interactive desktop window (colours, keyboard,
> audio); if something's off there, that's a very welcome first pull request.

Controls vary by game, but the pattern holds: A/D/W + Space is one player, the arrow keys + Enter
are a second where a game wants two. Every build prints its size — the number is worth watching,
and watching it is free.

```sh
./cvertex --res 1920 1080   # any resolution; the art is vector
./cvertex --fullscreen
./cvertex --headless 3600   # same inputs must give the same checksum, at any --res
./cvertex --dump 40         # framebuffer as ASCII
./cvertex --ppm 40          # framebuffer as a PPM on stdout
```

## Add a game

The `games/` folder is the cartridge library. Drop a `.c` in it, rebuild, and it is on the
shelf — there is no array to edit, no source list to append to, nothing named twice.

A game is one file that fills in the contract from `game.h` and names itself once:

```c
// games/pong.c
#include "core.h"
#include "game.h"

static void init(void)  { /* set up g_pal, your state */ }
static void tick(const Input in[2]) { /* pure: no clock, no rand, no drawing */ }
static void audio(void) { /* make noise about what tick decided */ }
static void draw(void)  { /* palette-indexed pixels and polygons */ }
static uint64_t checksum(void) { return /* your state, hashed */; }

const Game game_pong = { "pong", init, tick, audio, draw, checksum };
```

```sh
./build.sh && ./cvertex --game pong
```

> ⚠️ **A cartridge appears at BUILD time, not at runtime.** After you add or change a `.c` you
> **must rebuild** — `./build.sh` — because the engine is compiled, not a hot-loader. Opening an
> already-built `cvertex` will not show your new game; always `./build.sh && ./cvertex`.
>
> Name the struct **`game_<yourname>`** to match its `"<yourname>"` string (e.g. `game_pong` /
> `"pong"`). A generic name like `game_my_game` works alone but collides at link time the moment a
> second contributor leaves theirs the same — so make it yours.

`build.sh` runs `tools/gen-games.sh`, which scans `games/*.c` for each `const Game game_X`
and writes the roster into `src/games.gen.h`. It is an emulator reading its ROM folder,
done at build time — the `.c` is the ROM, and building is the cartridge going in. The one
name the folder doesn't decide is `menu`: that file is the shell, not a cartridge.

The whole toolchain is a C compiler. `./build.sh` uses the system `clang`; `./build-win.sh`
cross-compiles a Windows `.exe` with [zig](https://ziglang.org) (`brew install zig`). If you
can compile a C file, you can add a cartridge.

For the full contract — the runtime-resolution coordinate model, the determinism rule, input,
sound, and a checklist — see **[docs/AUTHORING.md](docs/AUTHORING.md)**. It ends with a system
prompt you can paste into an AI assistant so it writes cartridges that fit the engine instead of
guessing at its API.

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

Light and effects aren't computed, they're a 1 KB table you edit. That is why the 3D
pipeline is 1,316 bytes: it doesn't calculate light, it looks it up.

It also decides the art the engine wants — flat fills, hard edges, no gradients. A
gradient puts hundreds of near-identical skin tones on one face; 256 slots can't hold
them and wouldn't gain anything if they could. The look and the implementation are the
same decision, which is why the look is coherent rather than applied.

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

`tools/obj2mesh` turns any `.obj` into a `const` C array at build time; `tools/svg2poly`
does the same for vector art. No loader, no file format, no I/O, no error handling — and
`const` data lives in a `__TEXT` page that was already there.

The engine never learns what SVG is. SVG is an authoring format: the full spec is a
monster, and an engine that grew one would stop being readable. Curves flatten and colours
cluster at build time; the engine receives points and a palette index.

Vertex normals in an `.obj` are ignored — flat shading wants face normals, so they come
from geometry, which also means any tool's output works.

### Drawings become characters

A character is drawn from several angles, traced to polygons, and extruded: the
silhouette grows walls, the artwork stays flat on the front. Nothing is triangulated —
`poly_fill` only ever wanted screen coordinates and never cared where they came from, so
shape points ride the same transform as meshes.

`turn_pick` picks the drawn view nearest the facing and hands the leftover angle back, so
the chosen view keeps rotating the rest of the way. Doom snapped and you could see it
snap; here the drawing changes on a step and the motion doesn't. A gap in the sheet
costs accuracy, not correctness.

Three things about this are load-bearing, and each was a bug first:

- **Every view carries its own angle.** Assuming even spacing put the front view at 180°.
  An artist draws the angles a character needs, not the angles a formula wants. Which
  angle reads as screen-right is a property of the art too — read the sheet.
- **Mirroring is opt-in, never automatic.** Reflecting one half is free and it is wrong
  for any design that isn't symmetric: a scar, a satchel, a parting all jump sides, and it
  looks completely normal until someone notices. Bytes are the cheapest thing here.
- **Quantize the source before tracing.** A flat-coloured illustration turned out to hold
  3,004 distinct colours; about eleven were the artist's and the rest were anti-aliasing.
  A tracer can't tell those apart, so every blended edge pixel became a real, thin,
  spurious fill. Snapping the source to its own flat colours first (nearest in CIE76 Lab)
  took it to 14. And trace on a key colour, never on transparency: transparent composites
  to white, white artwork merges with it, and the tracer discards it as background — a
  white-haired character arrives with a hole where her hair was, which still looks right
  on a white page because you are seeing the page through her.

## Size discipline

Small isn't an accident, and it isn't free. Two rules keep it.

**Static variables are zero-initialized; assign at runtime.** A non-zero
initializer creates `__data`, and macOS aligns segments to 16 KB — so four bytes
of initial value drag 16,384 bytes into the file. Zero-initialized data lives in
`__bss` and costs nothing on disk. The framebuffer is free at any size — up to 4K.

**Measure, don't guess.** Every build prints its size. The first synth clipped
through its whole range and sounded merely "a bit dirty"; the meter said peak
32768, RMS 23772, and the mix gain was off by three bits.

## Status

Early. It runs, and the shape of it is settled.

- macOS, Windows, and Linux — a platform layer each (Cocoa, Win32, X11+ALSA), no third-party
  deps. The Linux layer (`build-linux.sh`) is compiled and run on real Linux (x86_64 and aarch64),
  its headless sim byte-identical across both; only the interactive desktop window is still to be
  confirmed.
- The engine ships with no artwork. `tools/svg2poly` bakes a character in; without one
  it draws placeholder polygons.

- `--dump` and other development paths still compile into the binary.

## Development

```
src/core.c    framebuffer, polygon rasterizer, deterministic sim, sine table
src/g3d.c     fixed-point 3D pipeline
src/synth.c   FM synth, oscillators, tracker
src/mac.c     macOS platform layer (Cocoa + CoreAudio)
src/win.c     Windows platform layer (Win32 + GDI + waveOut)
src/lnx.c     Linux platform layer (X11 + ALSA)
games/*.c     one cartridge each; the folder is the roster
tools/        build-time asset converters; gen-games.sh writes the roster
```

## License

MIT © CVER Inc.
