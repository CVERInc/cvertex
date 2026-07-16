// synth.c — 2-op FM + 古典波形 + 微型 tracker。零取樣、零檔案。
// 音訊在自己的執行緒跑；sim 只單向丟事件進來，永不讀合成器狀態 → 確定性不受污染。
#include "synth.h"
#include "core.h"
#include <math.h>

Instr g_instr[NINSTR];

enum { ENV_OFF, ENV_A, ENV_D, ENV_S, ENV_R };
typedef struct {
    uint8_t  instr, stage;
    uint32_t phase, mphase, step;    // 16.16 定點相位
    int32_t  env, envstep;           // 0..1024<<10
    uint16_t vel;
    uint32_t lfsr;                   // 雜訊用
} Voice;
static Voice g_v[NCHAN];

void synth_init(void) {
    tables_init();   // 合成器也吃這張表，但擁有者是 core
    for (int i = 0; i < NCHAN; i++) { g_v[i].stage = ENV_OFF; g_v[i].lfsr = 0xACE1u + i; }

    // 八個音色。這些數字就是「音效檔」——一個音色 12 bytes。
    //                wave      ratio index    a      d     s     r    duty
    g_instr[0] = (Instr){ W_SQUARE, 0,  0,    200,  3000,  600, 4000, 128 }; // 主旋律
    g_instr[1] = (Instr){ W_SINE,   2,  900,  100,  6000,  200, 3000, 128 }; // FM 鐘聲
    g_instr[2] = (Instr){ W_TRI,    0,  0,    300, 12000,  800, 8000, 128 }; // 貝斯
    g_instr[3] = (Instr){ W_NOISE,  0,  0,     50,  1800,    0,  600, 128 }; // 打擊
    g_instr[4] = (Instr){ W_SINE,   6, 1800,   20,  1200,    0,  800, 128 }; // FM 金屬撞擊
    g_instr[5] = (Instr){ W_SQUARE, 0,  0,     10,   900,    0,  400,  64 }; // 跳躍
    g_instr[6] = (Instr){ W_SAW,    1,  400,  400,  8000,  500, 5000, 128 }; // 襯底
    g_instr[7] = (Instr){ W_SINE,   0,  0,    800, 20000,  900,12000, 128 }; // 純音
}

static uint32_t note_step(int midi) {
    // 頻率 = 440 × 2^((midi-69)/12)，轉成 16.16 相位增量
    float hz = 440.f * powf(2.f, (midi - 69) / 12.f);
    return (uint32_t)(hz * 65536.f / SR);
}

void synth_note(int ch, int instr, int midi, int vel) {
    Voice *v = &g_v[ch & (NCHAN - 1)];
    v->instr = instr & (NINSTR - 1);
    v->step = note_step(midi);
    v->phase = v->mphase = 0;
    v->vel = vel;
    v->stage = ENV_A;
    v->env = 0;
    Instr *in = &g_instr[v->instr];
    v->envstep = in->a ? (1024 << 10) / in->a : (1024 << 10);
}

void synth_off(int ch) {
    Voice *v = &g_v[ch & (NCHAN - 1)];
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
static int g_tickcount = 0;
#define TICK_SAMPLES (SR / 8)   // 每秒 8 個 tracker row（125 BPM 感）

void synth_render(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        if (--g_tickcount <= 0) { music_tick(); g_tickcount = TICK_SAMPLES; }

        int32_t mix = 0;
        for (int c = 0; c < NCHAN; c++) {
            Voice *v = &g_v[c];
            if (v->stage == ENV_OFF) continue;
            Instr *in = &g_instr[v->instr];

            // ---- FM：調變器的輸出去偏移載波的相位。兩行程式碼，整個 1993 年。
            uint32_t ph = v->phase;
            if (in->mod_index) {
                int32_t m = g_sin[(v->mphase >> 6) & 1023];
                ph += (uint32_t)((int64_t)m * in->mod_index >> 8);
                v->mphase += (uint32_t)((uint64_t)v->step * in->mod_ratio >> 2);
            }
            int32_t s = osc(v, in, ph);
            v->phase += v->step;

            // ---- 包絡
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
            // >>20 是算出來的不是喊出來的：單 voice 滿檔 = 32767×1024×255>>20 ≈ 8.2k，
            // 四軌同時滿檔才剛好貼到滿刻度 → 正常演奏永不削波。
            mix += (int32_t)((int64_t)s * (v->env >> 10) * v->vel >> 20);
        }
        if (mix >  32767) mix =  32767;
        if (mix < -32768) mix = -32768;
        out[i * 2] = out[i * 2 + 1] = (int16_t)mix;
    }
}

// ---- 微型 tracker ---------------------------------------------------
// 一首曲子就是一張表。這是「音樂檔」——下面整首歌 64 bytes。
#define ROWS 32
static const uint8_t g_song[ROWS][NCHAN] = {
//   旋律 貝斯 打擊 襯底      (0 = 不動，其餘 = MIDI 音高)
    { 76,  40,  60,  64 }, { 0, 0, 0, 0 }, { 79, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 83,  40,  60,  0 },  { 0, 0, 0, 0 }, { 79, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 81,  45,  60,  69 }, { 0, 0, 0, 0 }, { 78, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 74,  45,  60,  0 },  { 0, 0, 0, 0 }, { 78, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 76,  38,  60,  62 }, { 0, 0, 0, 0 }, { 79, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 84,  38,  60,  0 },  { 0, 0, 0, 0 }, { 83, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 81,  43,  60,  67 }, { 0, 0, 0, 0 }, { 76, 0, 60, 0 }, { 0, 0, 0, 0 },
    { 79,  43,  60,  0 },  { 0, 0, 0, 0 }, { 0,  0, 60, 0 }, { 0, 0, 0, 0 },
};
// 🔴 這兩個永遠不准寫成非零的初始值：非零初始化 → __data → 硬拖一整頁 16KB 進檔案。
// 零初始化住 __bss（zerofill，不佔硬碟）。初值一律在執行期賦。
static int g_row, g_playing;
static const uint8_t g_rowinstr[NCHAN] = { 1, 2, 3, 6 };  // 各軌用哪個音色

void music_start(void) { g_row = ROWS - 1; g_playing = 1; }

static void music_tick(void) {
    if (!g_playing) return;
    g_row = (g_row + 1) % ROWS;
    for (int c = 0; c < NCHAN - 1; c++) {   // 最後一軌保留給音效
        uint8_t n = g_song[g_row][c];
        if (n) synth_note(c, g_rowinstr[c], n, 200);
    }
}
