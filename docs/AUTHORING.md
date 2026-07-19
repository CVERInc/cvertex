# Writing a cartridge

> A game is one file. Drop `games/yours.c` in the folder, rebuild, and it's on the
> menu — no wiring, no registration, nothing to edit anywhere else. This is how you
> write that file so it behaves like the ones already shipped.

You can read the whole engine in an afternoon, and you should: the four headers a
cartridge includes *are* the contract, and they say more than this document does. This
page is the map, not the territory — when it disagrees with a header, the header wins.

- `core.h` — framebuffer, palette, polygons, the depth buffer, the input struct.
- `game.h` — the five functions a cartridge is, the determinism rule, `input_1p`, `hud_top`.
- `synth.h` — the FM voice, `synth_note`, how a song and a sound effect differ.
- `text.h` — five columns of bits and a blitter. No layout, no fonts.

If you are handing this to an AI, hand it the four headers too. Most cartridges that
"almost work" are cartridges written against a guessed API instead of the real one.

---

## The whole of it

A cartridge is one `const Game`, and a `Game` is five functions:

```c
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"

// --- state: file-scope, and every byte of it `static` (see rule 6) ---
static int32_t g_x, g_y;      // the player, in VIRTUAL pixels — see rule 2
static uint32_t g_events;     // things that happened this tick, for audio() to voice

enum { EV_HIT = 1 };

static void init(void) {
    tables_init();                                   // rule 1: always first
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[1] = 0xFF3AA0FF;                            // 0xAARRGGBB — rule 7
    g_x = VW / 2; g_y = VH / 2;                       // centre of the VIRTUAL screen
    g_events = 0;
}

static void tick(const Input raw[2]) {
    Input in = input_1p(raw);                         // rule 3
    g_events = 0;
    g_x += in.x * 2;                                  // move in virtual pixels
    g_y += in.y * 2;
    if (in.jump) g_events |= EV_HIT;
}

static void audio(void) {                             // rule 5: sound lives here
    if (g_events & EV_HIT) synth_note(NCHAN - 1, 0, 72, 150);
}

static void draw(void) {
    fb_clear(0);                                      // always first
    // Author in virtual space, then scale to the real framebuffer at the last moment:
    int16_t px = (int16_t)((int32_t)g_x * g_fbw / VW);
    int16_t py = (int16_t)((int32_t)g_y * g_fbh / VH);
    int16_t box[8] = { px-8,py-8, px+8,py-8, px+8,py+8, px-8,py+8 };
    poly_fill(box, 4, 1);

    int s = g_fbh / 180; if (s < 1) s = 1;            // scale HUD text to resolution
    text_draw(g_fbw / 2 - text_width("HELLO", s) / 2, hud_top(), s, "HELLO", 1);
}

static uint64_t checksum(void) {                      // rule 8: the WHOLE state
    uint64_t h = 1469598103934665603ull;
    #define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    MIX(g_x); MIX(g_y); MIX(g_events);
    #undef MIX
    return h;
}

const Game game_hello = { "hello", init, tick, audio, draw, checksum };
```

That compiles, centres on any screen, takes WASD *or* the arrow keys, and makes a
noise. Everything below is why each line is the way it is.

---

## The eight rules that bite

**1. Read the headers, and let `init` set the world up.**
`init()`'s first line is `tables_init()` — the shared sine/rotation table is what 3D and
the synth both stand on, and nothing works before it exists. Then blank the palette
(`g_pal[i] = 0xFF000000`) and set the colours you want. `draw()`'s first line is
`fb_clear(...)`. These aren't ceremony; a cartridge that skips them inherits whatever
the last one left behind.

**2. The simulation lives in virtual pixels. Scale to the framebuffer only when you draw.**
This is the one that produces the most "it won't centre" grief, so it gets the most words.

The framebuffer is sized at *runtime* — the player picks 640×360, or 1920×1080, or the
display's native size, from the command line. `g_fbw` and `g_fbh` are those numbers, and
they are not known when you write the file. So you never write a literal like `160` for
"the middle"; the middle is `g_fbw / 2`, and it moves.

But physics measured in real pixels would run differently at different resolutions — and
a replay recorded at one size would desync at another, taking lockstep co-op down with
it. So the simulation is measured in a *fixed* virtual space, `VW`×`VH` (640×360), that
never changes. You move the player in virtual pixels; at draw time you map virtual → real
with `* g_fbw / VW` and `* g_fbh / VH`. Centre is `VW/2, VH/2` in the sim and lands dead
centre on any monitor.

Hardcode `160` and your game renders into the top-left corner of a 640-wide buffer and
nowhere near the middle of a 1920-wide one. No amount of nudging the constant fixes that,
because the constant is the bug — the width isn't a constant. `games/invade.c` is the
shortest cartridge that does this correctly; read its `draw`.

**3. Single-player reads `input_1p`, never `in[0]` directly.**
A player reaches for WASD or the arrows without thinking about it. `input_1p(raw)` merges
both halves of the keyboard into one `Input`, with Space *or* Enter as the action button
(the `jump` field). Read `raw[0]` straight and half your players' keys do nothing. (A
genuine two-player cartridge is the only reason to touch `raw[0]` and `raw[1]` apart.)

**4. Top-of-screen HUD starts at `hud_top()`.**
The window's title bar crowds the very top edge, so text at `y = 0` reads as jammed under
the chrome or hidden by it. `hud_top()` is the first row that clears it. Put your score,
timer, lives at `hud_top()` and below — never above.

**5. `tick` is pure; sound happens in `audio`.**
The same inputs from the same `init` must produce the same state, forever, on every
machine, at every resolution. That is what makes replays and networked co-op possible,
and it is not negotiable. So inside `tick`: no clock, no `rand()`, no floating point, no
reading the synth, no touching the framebuffer. Need randomness? Hash your frame counter.
Need noise? `tick` records *what happened* in a variable (a bitmask of events works well);
`audio()` runs right after, reads that variable, and calls `synth_note`. Keeping the two
apart is what stops the audio thread from reaching into determinism.

**6. Everything is `static`. The one exported symbol is `const Game game_<name>`.**
Every cartridge is compiled into the same binary, so two files that both define a
non-`static` `g_score` either collide at link time or, worse, silently *share* one
variable. Mark every piece of file-scope state `static`. The single symbol the engine is
allowed to see is the `Game` struct at the bottom, and its name must match the file:
`games/foo.c` ends with `const Game game_foo = { ... }`.

**7. Colours are palette indices, 0–255 — not 32-bit values.**
`poly_fill`, `text_draw` and friends take a colour *index* (`ci`). You define what each
index looks like in `init` by writing a 32-bit `0xAARRGGBB` into `g_pal[index]`. Passing
`0xFFFFFFFF` where an index is expected asks for palette entry 255, not white. The flat,
poster-like look is the palette doing its job; a small, deliberately-chosen set of colours
reads as art direction, a hundred arbitrary ones as noise.

**8. `checksum` hashes the *whole* state.**
`--headless` runs the game with no window and hashes what `checksum` returns each frame,
which is how a desync is caught. Hash every field that `tick` can change — positions,
velocities, timers, scores, both players. Hash only one field and a bug in everything else
sails straight past the one test built to catch it.

---

## Sound, concretely

Six channels. Five belong to a song; the last one, `NCHAN - 1`, is always the sound-effect
channel — the game talking over the music. To fire a one-shot effect:

```c
synth_note(NCHAN - 1, instrument, midi_pitch, velocity);
```

`instrument` is an index into `g_instr[NINSTR]` (2-op FM voices — see `synth.h` for the
`Instr` fields, and the shipped cartridges for voices that sound like something). A short,
bright note on a `W_SQUARE` reads as a pickup; noise on a low pitch reads as a hit. Velocity
is clamped for you, so you cannot blow the mix. Music, if you want it, is a byte table handed
to `music_play` — but a game with only well-placed effects and no song at all is completely
normal.

---

## How it reaches the menu

`tools/gen-games.sh` scans `games/*.c` at build time, finds every `const Game game_*`, and
writes the roster into `games.gen.h` (a generated file — don't commit it, don't edit it).
The build globs all of `games/`. So the entire act of "adding a game" is: write the file,
rebuild. There is nothing else to touch, and nothing names your cartridge but your cartridge.

---

## The checklist

- [ ] `#include` the four headers; define nothing they already define.
- [ ] `init`: `tables_init()`, then blank and set `g_pal`.
- [ ] `draw`: `fb_clear(...)` first.
- [ ] No literal resolution anywhere. Sim in `VW`×`VH`; scale by `g_fbw`/`g_fbh` at draw.
- [ ] Single player reads `input_1p`; action is the `jump` field.
- [ ] Top HUD at `hud_top()` or below; scale text by `g_fbh`.
- [ ] `tick` pure: no clock, no `rand`, no float, no synth, no framebuffer. Sound in `audio`.
- [ ] Every file-scope name `static` but the final `const Game game_<name>`.
- [ ] Colours are `g_pal` indices; set them as `0xAARRGGBB` in `init`.
- [ ] `checksum` mixes every field `tick` can change.
- [ ] Builds clean, runs `--headless`, shows up on the menu.

---

## A system prompt you can paste into any assistant

> You are writing one game cartridge for the cvertex engine: a single C11 file,
> `games/<name>.c`, no dependencies beyond the engine's own headers. First read the four
> headers you are given — `core.h`, `game.h`, `synth.h`, `text.h` — and treat them as the
> only source of truth for every type and function; do not invent or restate an API from
> memory. Then obey, without exception:
>
> 1. The file includes `core.h`, `game.h`, `synth.h`, `text.h` and defines nothing they
>    already define (not `Input`, not `Game`, not `g_pal`).
> 2. It ends with exactly one exported symbol, `const Game game_<name> = { "<name>", init,
>    tick, audio, draw, checksum };`. Every other file-scope declaration is `static`.
> 3. `init()` starts with `tables_init();`, then sets `g_pal[i]` entries as `0xAARRGGBB`
>    (index 0 is the clear colour). `draw()` starts with `fb_clear(...)`.
> 4. The framebuffer is sized at runtime: `g_fbw`, `g_fbh`. NEVER hardcode a pixel width,
>    height, or centre. Simulate in the fixed virtual space `VW`×`VH` (640×360); at draw
>    time scale points with `* g_fbw / VW` and `* g_fbh / VH`. Screen centre is `VW/2,
>    VH/2` in the simulation.
> 5. For one player, read input via `Input in = input_1p(raw);`. The action button is
>    `in.jump` (Space or Enter). Do not read `raw[0]`/`raw[1]` directly unless the game is
>    genuinely two-player.
> 6. `tick(const Input raw[2])` must be pure and deterministic: no clock, no `rand()`, no
>    floating point, no `<math.h>`, no reading the synth, no writing the framebuffer. Use
>    integer/fixed-point math. For randomness, hash a frame counter.
> 7. Make sound only in `audio()`: `tick` records events in state, `audio()` reads them and
>    calls `synth_note(NCHAN - 1, instr, midi, vel)` on the sound-effect channel.
> 8. Colours passed to `poly_fill`/`text_draw` are palette indices 0–255, never 32-bit
>    colour values.
> 9. Top-of-screen HUD text sits at `hud_top()` or below, with size scaled by `g_fbh`.
> 10. `checksum()` FNV-1a-hashes every field `tick` can change, so `--headless` can catch a
>     desync.
>
> Produce the complete file. If a detail isn't covered by the headers or these rules, copy
> the smallest shipped cartridge that does the same thing rather than guessing.
