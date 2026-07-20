// synth.h — the synth. No samples, no files — sound is computed.
#ifndef SYNTH_H
#define SYNTH_H
#include <stdint.h>

#define SR       44100
#define NCHAN    17     // sixteen for a song, and the last one always belongs to sound effects.
                        // 🔴 Next-gen: SFC's eight was a hardware limit, and we run on 2020s
                        // silicon. Eight taught good arranging; sixteen lets a real orchestral or
                        // piano score keep its inner voices when it comes down into the tracker,
                        // instead of throwing half the chord away. The last channel is still SFX's
                        // alone — the game's voice is never stolen by the music, however big it gets.

// The loudest a note may be. It used to be 255*4/NCHAN — a hard division that guaranteed even
// every voice at full blast could not clip, by making each one quieter as the count grew. At
// sixteen voices that math starves the sound (VEL_MAX would fall to 60). So the ceiling moved
// from per-voice division to a master SOFT-LIMITER (synth_render): voices play at a healthy fixed
// level, the mix stays linear through normal playing, and only a genuine full-orchestra tutti
// leans into a smooth limiter instead of a hard clip. Loud AND many, the way a real mixer does it
// — not many-but-thin. (This is also why the eight-voice era's −3 dB drop is gone.)
#define VEL_MAX  160
// What an UNACCENTED note plays at. The pair (VEL_SOFT, VEL_MAX) is the dynamic range of every
// song in the machine: a table with no accents in it plays flat at VEL_SOFT, and marking a row
// pushes it to the ceiling. Kept next to VEL_MAX because they are one decision, not two.
#define VEL_SOFT (VEL_MAX * 5 / 8)
#define NINSTR   8

// Voice: 2-op FM (that AdLib/OPL2 flavour) plus the classic waveforms.
enum { W_SINE, W_SQUARE, W_TRI, W_SAW, W_NOISE };

typedef struct {
    uint8_t  wave;        // carrier waveform
    uint8_t  mod_ratio;   // modulator frequency = carrier × ratio/4 (the soul of FM)
    uint16_t mod_index;   // modulation depth; 0 = plain waveform, no FM
    uint16_t a, d, s, r;  // ADSR: a/d/r are sample counts, s is a sustain level 0..1024
    uint8_t  duty;        // square wave duty cycle 0..255
    // Vibrato, the thing a held note needs so it stops sounding like a switch left on. Both LAST
    // in the struct so every instrument written before they existed still compiles — and reads as
    // depth 0, which is exactly the old behaviour. (Same reasoning as Game's `author` field.)
    uint8_t  vib_rate;    // ~64 is 5 Hz, ~255 is 20 Hz
    uint8_t  vib_depth;   // 0 = dead steady; 255 ≈ ±3% of the pitch, a third of a semitone
} Instr;

extern Instr g_instr[NINSTR];

void synth_init(void);
void synth_note(int ch, int instr, int midi, int vel);  // triggered by the game thread
void synth_off(int ch);

// ---- the stage a voice stands on -------------------------------------------------------------
// Two knobs that shape SPACE rather than sound, because five voices mixed to one point in the
// middle of your head is the one thing about this synth that a real console never did.
//
// pan: −32768 hard left … 0 centre … +32767 hard right. synth_init fans the music channels out
// across the stereo field and leaves the SFX channel dead centre, so a game gets a stereo mix
// without asking; a game that wants its own layout says so.
void synth_pan(int ch, int32_t q15);
// echo send: 0 = dry … 32767 = the whole voice into the delay line. There is ONE echo — a delay
// with feedback and a lowpass in the loop, which is what a 90s console could build and what makes
// a room. Not a reverb: no early reflections, no diffusion, nothing that needs a convolution.
// synth_init gives the music channels a little and the SFX channel none (the game talking should
// land in front of the music, not behind it).
void synth_echo(int ch, int32_t send);
void synth_render(int16_t *out, int frames);            // called by the audio thread
// A channel's live loudness, ~0..170, for VISUALS only (a music visualiser, a pulsing world).
// Reads the audio thread's envelope without a lock: a visualiser wants "roughly now", and this
// never touches the sim, so a torn read costs at most one slightly-stale bar. Never hash it.
int  synth_level(int ch);
// A song is a table of `rows` x NCHAN bytes. A byte is:
//   0          hold — nothing happens on this row
//   255        note-off (a rest made real; without it a sustaining instrument hangs forever)
//   1..108     a MIDI pitch, played at VEL_SOFT
//   |0x80      the same pitch ACCENTED, played at VEL_MAX
// 🔴 255 is tested BEFORE the accent mask, or it decodes as "accented pitch 127" and a rest
// becomes the loudest note in the song. Pitches stop at 108, so the two never collide.
// One byte still carries the whole row: widening the cell to fit a velocity would have doubled
// every table on disk to express what one bit expresses.
// rowinstr says which instrument each channel plays. The last channel is never read — it
// belongs to sound effects, which are the game talking, not the song.
//
// 🔴 The engine has no song of its own, the same way it has no font. It used to carry a
// demo tune that nothing ever played: an engine with a tune in it is an engine with an
// opinion about music.
// rps = rows per second. Tempo belongs to the song for the same reason the notes do — the
// engine used to hardcode eight rows a second, which is an engine with an opinion about how
// fast music goes.
void music_play(const uint8_t *song, int rows, const uint8_t *rowinstr, int rps);

#endif
