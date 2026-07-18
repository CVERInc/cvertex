// synth.h — the synth. No samples, no files — sound is computed.
#ifndef SYNTH_H
#define SYNTH_H
#include <stdint.h>

#define SR       44100
#define NCHAN    6      // five for a song, and the last one always belongs to sound effects

// 🔴 The loudest a note may be, and it is a division rather than a preference. One voice at
// full blast is 32767 x 1024 x vel >> 20 = 8159 at vel 255, so int16 holds almost exactly
// four of them. Every channel after the fourth spends the same headroom: four channels'
// worth, shared NCHAN ways. Past this the clamp starts doing the mixing, which is the long
// way of spelling distortion. synth_note enforces it, so no game can get this wrong.
#define VEL_MAX  (255 * 4 / NCHAN)
#define NINSTR   8

// Voice: 2-op FM (that AdLib/OPL2 flavour) plus the classic waveforms.
enum { W_SINE, W_SQUARE, W_TRI, W_SAW, W_NOISE };

typedef struct {
    uint8_t  wave;        // carrier waveform
    uint8_t  mod_ratio;   // modulator frequency = carrier × ratio/4 (the soul of FM)
    uint16_t mod_index;   // modulation depth; 0 = plain waveform, no FM
    uint16_t a, d, s, r;  // ADSR: a/d/r are sample counts, s is a sustain level 0..1024
    uint8_t  duty;        // square wave duty cycle 0..255
} Instr;

extern Instr g_instr[NINSTR];

void synth_init(void);
void synth_note(int ch, int instr, int midi, int vel);  // triggered by the game thread
void synth_off(int ch);
void synth_render(int16_t *out, int frames);            // called by the audio thread
// A song is a table of `rows` x NCHAN bytes: 0 = hold, otherwise a MIDI pitch. rowinstr
// says which instrument each channel plays. The last channel is never read — it belongs to
// sound effects, which are the game talking, not the song.
//
// 🔴 The engine has no song of its own, the same way it has no font. It used to carry a
// demo tune that nothing ever played: an engine with a tune in it is an engine with an
// opinion about music.
// rps = rows per second. Tempo belongs to the song for the same reason the notes do — the
// engine used to hardcode eight rows a second, which is an engine with an opinion about how
// fast music goes.
void music_play(const uint8_t *song, int rows, const uint8_t *rowinstr, int rps);

#endif
