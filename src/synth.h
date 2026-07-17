// synth.h — the synth. No samples, no files — sound is computed.
#ifndef SYNTH_H
#define SYNTH_H
#include <stdint.h>

#define SR       44100
#define NCHAN    4
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
void music_start(void);   // the tracker's own demo tune — a game must ask for it

#endif
