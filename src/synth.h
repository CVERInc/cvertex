// synth.h — 合成器。沒有取樣、沒有檔案，聲音是算出來的。
#ifndef SYNTH_H
#define SYNTH_H
#include <stdint.h>

#define SR       44100
#define NCHAN    4
#define NINSTR   8

// 音色：2-op FM（AdLib/OPL2 那味）＋古典波形。
enum { W_SINE, W_SQUARE, W_TRI, W_SAW, W_NOISE };

typedef struct {
    uint8_t  wave;        // 載波波形
    uint8_t  mod_ratio;   // 調變器頻率 = 載波 × ratio/4（FM 的靈魂）
    uint16_t mod_index;   // 調變深度，0 = 純波形不做 FM
    uint16_t a, d, s, r;  // ADSR：a/d/r 是 sample 數，s 是 0..1024 的持續位準
    uint8_t  duty;        // 方波工作週期 0..255
} Instr;

extern Instr g_instr[NINSTR];

void synth_init(void);
void synth_note(int ch, int instr, int midi, int vel);  // 遊戲執行緒觸發
void synth_off(int ch);
void synth_render(int16_t *out, int frames);            // 音訊執行緒呼叫
void music_start(void);

#endif
