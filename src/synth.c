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
    uint32_t vphase;                 // the vibrato LFO's own phase
    int32_t  env, envstep;           // 0..1024<<10
    uint16_t vel;
    uint32_t lfsr;                   // for noise
} Voice;
static Voice g_v[NCHAN];

// ---- the stage: where each voice stands, and how much of it goes into the room ------------------
// 🔴 All zero-init (__bss — see the note on the tracker's state below); synth_init fills in the
// defaults at startup rather than an initializer doing it on disk.
static int32_t g_panl[NCHAN], g_panr[NCHAN];   // Q15 per-side gains, from an equal-gain pan law
static int32_t g_send[NCHAN];                  // Q15 of the voice into the delay line

void synth_pan(int ch, int32_t q15) {
    if (ch < 0 || ch >= NCHAN) return;
    if (q15 < -32768) q15 = -32768;
    if (q15 >  32767) q15 =  32767;
    // 🔴 A 0 dB pan law: centre is FULL on both sides and panning only ever turns the far side
    // DOWN. Every other law (equal-power, or anything that halves the centre) changes how loud the
    // existing games are the moment stereo arrives, and worse, a law that can exceed unity on one
    // side would break the anti-clip arithmetic VEL_MAX was derived from. Here no side ever gets
    // more than the mono mix used to, so the headroom proof survives the stage.
    g_panl[ch] = 32767 - (q15 > 0 ? q15 : 0);
    g_panr[ch] = 32767 + (q15 < 0 ? q15 : 0);
}
void synth_echo(int ch, int32_t send) {
    if (ch < 0 || ch >= NCHAN) return;
    g_send[ch] = send < 0 ? 0 : send > 32767 ? 32767 : send;
}

// ---- the echo: one delay line, fed by the sends, feeding itself back through a lowpass ---------
// 150 ms and 40% back is a room, not a cathedral. The lowpass in the loop is the whole character:
// each pass loses its top end, so the tail turns to shadow instead of ringing on as a copy — the
// cheapest honest way to sound like somewhere. int16 and __bss: 13 KB of RAM, nothing on disk.
#define ECHO_LEN  (SR * 150 / 1000)
#define ECHO_FB   13107            // Q15 ≈ 0.40 feedback
#define ECHO_LP   13107            // Q15 ≈ 0.40 one-pole coefficient in the loop
#define ECHO_RET  18022            // Q15 ≈ 0.55 return level. 🔴 The tail rides the SAME headroom
                                   // the dry mix does — sends and return are kept small on purpose
                                   // so a loud passage doesn't hand the mixing job to the clamp.
static int16_t g_echo[ECHO_LEN];
static int     g_echo_i;
static int32_t g_echo_lp;

void synth_init(void) {
    tables_init();   // the synth uses this table too, but core owns it
    for (int i = 0; i < NCHAN; i++) { g_v[i].stage = ENV_OFF; g_v[i].lfsr = 0xACE1u + i; }

    // The default stage: the music channels fanned evenly across the field, the last one (sound
    // effects) dead centre and dry, because the game talking should land in front of the music.
    //
    // 🔴 The fan ALTERNATES sides — ch0 a touch left, ch1 a touch right, ch2 further left, and so on
    // — instead of sweeping left-to-right across all sixteen. A song that uses only its first few
    // channels (most do: lead, harmony, arp, bass...) would otherwise pile up on one side, because
    // the arranger fills channels in order and a straight sweep puts the low indices all left. This
    // way ANY prefix of channels stays balanced. It is still a gentle lean, not a hard pan, and the
    // engine still holds no opinion about which channel is the melody — a cartridge that knows its
    // roles overrides with synth_pan().
    int nm = NCHAN - 1;
    for (int i = 0; i < nm; i++) {
        int32_t mag = 3500 + (int32_t)(i / 2) * 2600;   // spreads outward in pairs
        if (mag > 24000) mag = 24000;
        synth_pan(i, (i & 1) ? mag : -mag);
        synth_echo(i, 8192);       // Q15 0.25
    }
    synth_pan(NCHAN - 1, 0);
    synth_echo(NCHAN - 1, 0);
    for (int i = 0; i < ECHO_LEN; i++) g_echo[i] = 0;
    g_echo_i = 0; g_echo_lp = 0;

    // Eight instruments. These numbers are the "sound file" — one instrument is 14 bytes.
    // The two trailing columns are vibrato (rate, depth); a 0 depth is a dead-steady pitch, which
    // is what every instrument had before it existed. Only the two that hold long notes get any:
    // percussion and the one-shots have nothing to wobble.
    //                wave      ratio index    a      d     s     r    duty  vib
    g_instr[0] = (Instr){ W_SQUARE, 0,  0,    200,  3000,  600, 4000, 128,  58, 34 }; // lead
    g_instr[1] = (Instr){ W_SINE,   2,  900,  100,  6000,  200, 3000, 128 }; // FM bell
    g_instr[2] = (Instr){ W_TRI,    0,  0,    300, 12000,  800, 8000, 128 }; // bass
    // Percussion. The decay used to be 1800 samples — 41 ms, which is a TICK, not a drum: at a
    // twelve-row-per-second groove the hits are 160 ms apart, so each one died long before the next
    // and the rhythm section had no weight to it. 4600 samples is ~105 ms, enough noise body to read
    // as the era's explosive drum without smearing into the following beat. Attack stays at 50 (1 ms)
    // because the snap at the front is what makes it hit.
    g_instr[3] = (Instr){ W_NOISE,  0,  0,     50,  4600,    0,  600, 128 }; // percussion
    g_instr[4] = (Instr){ W_SINE,   6, 1800,   20,  1200,    0,  800, 128 }; // FM metallic hit
    g_instr[5] = (Instr){ W_SQUARE, 0,  0,     10,   900,    0,  400,  64 }; // jump
    g_instr[6] = (Instr){ W_SAW,    1,  400,  400,  8000,  500, 5000, 128,  42, 46 }; // pad
    g_instr[7] = (Instr){ W_SINE,   0,  0,    800, 20000,    0,12000, 128 }; // pure tone — decays to silence (s=0), so a one-shot (a game-over sting, a distant hoot) fades instead of droning forever with no note-off
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
    v->phase = v->mphase = v->vphase = 0;   // every note starts from the top of its own vibrato
    // Clamped here so that no game can ever mix its way past int16. See VEL_MAX.
    if (vel > VEL_MAX) vel = VEL_MAX;
    if (vel < 0) vel = 0;
    v->vel = vel;
    v->stage = ENV_A;
    v->env = 0;
    Instr *in = &g_instr[v->instr];
    v->envstep = in->a ? (1024 << 10) / in->a : (1024 << 10);
}

int synth_level(int ch) {
    if (ch < 0 || ch >= NCHAN) return 0;
    const Voice *v = &g_v[ch];
    if (v->stage == ENV_OFF) return 0;
    return (int)(((int64_t)(v->env >> 10) * v->vel) >> 10);   // ~0..170
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

// The master bus limiter. Below the knee the mix passes untouched, so normal playing is exactly
// the sum of its voices — no colour, no pumping. Past the knee the excess is bent smoothly toward
// the ceiling by a rational curve (d·H / (d + H), which approaches H and never reaches it), so a
// full-orchestra tutti finds a soft wall instead of the hard clip that used to spell distortion.
// Integer, branch-light, and symmetric. This is what lets sixteen voices be LOUD instead of thin.
#define LIM_KNEE 22000
static int32_t soft_limit(int32_t x) {
    const int32_t H = 32767 - LIM_KNEE;
    if (x >  LIM_KNEE) { int32_t d = x - LIM_KNEE;  return  LIM_KNEE + (int32_t)((int64_t)d * H / (d + H)); }
    if (x < -LIM_KNEE) { int32_t d = -x - LIM_KNEE; return -(LIM_KNEE + (int32_t)((int64_t)d * H / (d + H))); }
    return x;
}

void synth_render(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        if (g_ticklen && --g_tickcount <= 0) { music_tick(); g_tickcount = g_ticklen; }

        int32_t left = 0, right = 0, send = 0;
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
            // Vibrato: nudge the pitch with a slow sine. A held note with a dead-steady pitch is
            // the sound of a switch left on rather than someone playing; this is the cheapest thing
            // that makes eight sustained voices sound like an ensemble instead of an organ.
            uint32_t stp = v->step;
            if (in->vib_depth) {
                v->vphase += (uint32_t)in->vib_rate * 119;      // rate 64 ≈ 5 Hz at SR
                int32_t lfo = g_sin[(v->vphase >> 16) & 1023];  // Q15
                stp += (uint32_t)(((int64_t)stp * lfo * in->vib_depth) >> 28);
            }
            v->phase += stp;

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
            int32_t vs = (int32_t)((int64_t)s * (v->env >> 10) * v->vel >> 20);
            left  += (int32_t)(((int64_t)vs * g_panl[c]) >> 15);   // where this voice stands
            right += (int32_t)(((int64_t)vs * g_panr[c]) >> 15);
            send  += (int32_t)(((int64_t)vs * g_send[c]) >> 15);   // and how much of it goes into the room
        }
        // The delay line. Read what went in ECHO_LEN samples ago, roll it back in through a lowpass
        // so every pass loses more of its top end — the tail turns to shadow instead of ringing on
        // as a copy of itself. The room returns to both sides equally, because a room has no side.
        int32_t wet = g_echo[g_echo_i];
        g_echo_lp += (int32_t)(((int64_t)(wet - g_echo_lp) * ECHO_LP) >> 15);
        int32_t fb = send + (int32_t)(((int64_t)g_echo_lp * ECHO_FB) >> 15);
        if (fb >  32767) fb =  32767;
        if (fb < -32768) fb = -32768;
        g_echo[g_echo_i] = (int16_t)fb;
        if (++g_echo_i >= ECHO_LEN) g_echo_i = 0;
        int32_t ret = (int32_t)(((int64_t)wet * ECHO_RET) >> 15);
        left += ret; right += ret;

        out[i * 2]     = (int16_t)soft_limit(left);    // the master bus: linear until the knee, then
        out[i * 2 + 1] = (int16_t)soft_limit(right);   // a smooth ceiling — see soft_limit
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
    // Stopping the song means going QUIET, not just freezing the tracker: release every voice so
    // nothing drones on. Without this a note still in its sustain (a game-over sting, a held pad)
    // rings forever — the tracker stopped feeding it note-offs, and no one else was going to. This
    // is also what makes the platform's music_play(0,0,0,0) on a game switch a real silence.
    if (!song || rows <= 0 || rps <= 0) {
        g_playing = 0;
        for (int c = 0; c < NCHAN; c++) synth_off(c);
        // and empty the room: silencing the voices while 150 ms of the last game's music is still
        // circulating in the delay line means the next cartridge boots to somebody else's echo.
        for (int i = 0; i < ECHO_LEN; i++) g_echo[i] = 0;
        g_echo_lp = 0;
        return;
    }
    // A new song must not inherit the old one's ringing voices: a sustaining lead or pad has no
    // note-off coming (its song is gone), so it would drone into the new song's opening rest.
    // Release the five music channels; SFX keeps its slot — switching songs is not the game going quiet.
    for (int c = 0; c < NCHAN - 1; c++) synth_off(c);
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
        // 🔴 The note-off test comes FIRST: 255 is 0x7F|0x80, so masking before this would read a
        // rest as an accented pitch 127 — the loudest note in the song, where silence was written.
        if (n == 0xFF) synth_off(c);   // a rest: release the voice instead of holding it
        else if (n) synth_note(c, g_rowinstr[c], n & 0x7F, (n & 0x80) ? VEL_MAX : VEL_SOFT);
    }
}
