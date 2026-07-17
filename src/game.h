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
} Game;

// Every game defines one of these; the platform layer picks between them.
extern const Game game_vikings;
extern const Game game_title;

#endif
