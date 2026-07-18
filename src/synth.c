// synth.c — 2-op FM + classic waveforms + a tiny tracker. Zero samples, zero files.
// Audio runs on its own thread; the sim only throws events in one direction and
// never reads synth state → determinism stays uncontaminated.
#include "synth.h"
#include "core.h"
#include <math.h>

Instr g_instr[NINSTR];

enum { ENV_OFF, ENV_A, ENV_D, ENV_S, ENV_R };
typedef struct {
    uint8_t  instr, stage;
    uint32_t phase, mphase, step;    // 16.16 fixed-point phase
    int32_t  env, envstep;           // 0..1024<<10
    uint16_t vel;
    uint32_t lfsr;                   // for noise
} Voice;
static Voice g_v[NCHAN];

void synth_init(void) {
    tables_init();   // the synth uses this table too, but core owns it
    for (int i = 0; i < NCHAN; i++) { g_v[i].stage = ENV_OFF; g_v[i].lfsr = 0xACE1u + i; }

    // Eight instruments. These numbers are the "sound file" — one instrument is 12 bytes.
    //                wave      ratio index    a      d     s     r    duty
    g_instr[0] = (Instr){ W_SQUARE, 0,  0,    200,  3000,  600, 4000, 128 }; // lead
    g_instr[1] = (Instr){ W_SINE,   2,  900,  100,  6000,  200, 3000, 128 }; // FM bell
    g_instr[2] = (Instr){ W_TRI,    0,  0,    300, 12000,  800, 8000, 128 }; // bass
    g_instr[3] = (Instr){ W_NOISE,  0,  0,     50,  1800,    0,  600, 128 }; // percussion
    g_instr[4] = (Instr){ W_SINE,   6, 1800,   20,  1200,    0,  800, 128 }; // FM metallic hit
    g_instr[5] = (Instr){ W_SQUARE, 0,  0,     10,   900,    0,  400,  64 }; // jump
    g_instr[6] = (Instr){ W_SAW,    1,  400,  400,  8000,  500, 5000, 128 }; // pad
    g_instr[7] = (Instr){ W_SINE,   0,  0,    800, 20000,  900,12000, 128 }; // pure tone
}

static uint32_t note_step(int midi) {
    // frequency = 440 × 2^((midi-69)/12), converted to a 16.16 phase increment
    float hz = 440.f * powf(2.f, (midi - 69) / 12.f);
    return (uint32_t)(hz * 65536.f / SR);
}

void synth_note(int ch, int instr, int midi, int vel) {
    // 🔴 A bound, not a mask. `ch & (NCHAN - 1)` is only a modulo while NCHAN is a power of
    // two — it was 4, it is 6, and &5 quietly sends channel 2 to voice 0 and channel 3 to
    // voice 1. The song would have come out mangled and I'd have blamed the song.
    if (ch < 0 || ch >= NCHAN) return;
    Voice *v = &g_v[ch];
    v->instr = instr & (NINSTR - 1);   // NINSTR is 8 and the mask is honest there
    v->step = note_step(midi);
    v->phase = v->mphase = 0;
    // Clamped here so that no game can ever mix its way past int16. See VEL_MAX.
    if (vel > VEL_MAX) vel = VEL_MAX;
    if (vel < 0) vel = 0;
    v->vel = vel;
    v->stage = ENV_A;
    v->env = 0;
    Instr *in = &g_instr[v->instr];
    v->envstep = in->a ? (1024 << 10) / in->a : (1024 << 10);
}

void synth_off(int ch) {
    if (ch < 0 || ch >= NCHAN) return;      // a bound, not a mask — see synth_note
    Voice *v = &g_v[ch];
    if (v->stage == ENV_OFF) return;
    v->stage = ENV_R;
    Instr *in = &g_instr[v->instr];
    v->envstep = in->r ? -(v->env / (int32_t)in->r) - 1 : -(1024 << 10);
}

static int32_t osc(Voice *v, Instr *in, uint32_t ph) {
    switch (in->wave) {
    case W_SINE:   return g_sin[(ph >> 6) & 1023];
    case W_SQUARE: return (((ph >> 8) & 255) < in->duty) ? 32767 : -32768;
    case W_TRI: {  int32_t t = (ph >> 7) & 511;
                   return (t < 256 ? t - 128 : 383 - t) * 256; }
    case W_SAW:    return (int32_t)((ph >> 8) & 65535) - 32768;
    case W_NOISE:  v->lfsr = (v->lfsr >> 1) ^ (-(int32_t)(v->lfsr & 1) & 0xB400u);
                   return (int32_t)(v->lfsr & 65535) - 32768;
    }
    return 0;
}

static void music_tick(void);
static int g_tickcount, g_ticklen;   // the tracker's clock; zero-init, so __bss

void synth_render(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        if (g_ticklen && --g_tickcount <= 0) { music_tick(); g_tickcount = g_ticklen; }

        int32_t mix = 0;
        for (int c = 0; c < NCHAN; c++) {
            Voice *v = &g_v[c];
            if (v->stage == ENV_OFF) continue;
            Instr *in = &g_instr[v->instr];

            // ---- FM: the modulator's output offsets the carrier's phase. Two lines of code, the whole of 1993.
            uint32_t ph = v->phase;
            if (in->mod_index) {
                int32_t m = g_sin[(v->mphase >> 6) & 1023];
                ph += (uint32_t)((int64_t)m * in->mod_index >> 8);
                v->mphase += (uint32_t)((uint64_t)v->step * in->mod_ratio >> 2);
            }
            int32_t s = osc(v, in, ph);
            v->phase += v->step;

            // ---- envelope
            v->env += v->envstep;
            switch (v->stage) {
            case ENV_A: if (v->env >= (1024 << 10)) {
                            v->env = 1024 << 10; v->stage = ENV_D;
                            v->envstep = in->d ? -(((1024 - in->s) << 10) / (int32_t)in->d) - 1 : 0;
                        } break;
            case ENV_D: if (v->env <= ((int32_t)in->s << 10)) {
                            v->env = (int32_t)in->s << 10; v->stage = ENV_S; v->envstep = 0;
                        } break;
            case ENV_R: if (v->env <= 0) { v->env = 0; v->stage = ENV_OFF; } break;
            }
            // The >>20 is derived, not guessed: one voice at full blast =
            // 32767×1024×255>>20 ≈ 8.2k, so all four channels have to peak at once
            // just to reach full scale → normal playing never clips.
            mix += (int32_t)((int64_t)s * (v->env >> 10) * v->vel >> 20);
        }
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out[i * 2] = out[i * 2 + 1] = (int16_t)mix;
    }
}

// ---- tiny tracker ---------------------------------------------------
// A song is a table, and the table belongs to whoever is playing it. This holds a pointer
// and a length; the notes live in the game.
//
// 🔴 None of these may get a non-zero initializer: that creates __data, and macOS aligns
// segments to 16 KB, so four bytes of initial value drag a whole page into the file.
// Zero-init lives in __bss, which is zerofill and costs nothing on disk.
static const uint8_t *g_song, *g_rowinstr;
static int g_rows, g_row, g_playing;

void music_play(const uint8_t *song, int rows, const uint8_t *rowinstr, int rps) {
    if (!song || rows <= 0 || rps <= 0) { g_playing = 0; return; }
    g_song = song; g_rows = rows; g_rowinstr = rowinstr;
    g_ticklen = SR / rps;
    g_tickcount = 1;
    g_row = rows - 1; g_playing = 1;
}

static void music_tick(void) {
    if (!g_playing) return;
    g_row = (g_row + 1) % g_rows;
    for (int c = 0; c < NCHAN - 1; c++) {   // the last channel belongs to sound effects
        uint8_t n = g_song[g_row * NCHAN + c];
        if (n) synth_note(c, g_rowinstr[c], n, VEL_MAX);
    }
}
