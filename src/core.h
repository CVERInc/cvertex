// core.h — 引擎核心。零依賴，只用 stdint。
#ifndef CORE_H
#define CORE_H
#include <stdint.h>

#define FBW 320
#define FBH 180
#define MAXPTS 256   // 一條掃描線最多幾個交點。追蹤出來的輪廓穿很多次，別用手寫多邊形的直覺抓。

// 調色盤索引 framebuffer：一個 byte 一個 pixel。
extern uint8_t  g_fb[FBW * FBH];
extern uint32_t g_pal[256];   // 0xAARRGGBB

// 正弦查表。合成器拿它做波形、3D 拿它做旋轉——同一張表，因為那本來就是同一件事。
// 🔴 它住在 core 而不是 synth：共用是紅利，但誰擁有它必須明確，否則 3D 會偷偷
// 依賴音效的初始化順序（踩過：--dump 沒呼叫 synth_init，整顆立方體塌成一個點）。
extern int16_t g_sin[1024];   // Q15：±32767 = ±1.0
void tables_init(void);       // sim_init 會呼叫；合成器也依賴它

// 輸入：每個角色一份。狀態只由這個結構改變。
typedef struct { int8_t x, y; uint8_t act; } Input;

// sim 吐出的事件，平台層消費後才發聲。sim 自己不認識合成器 → 音訊執行緒污染不到確定性。
extern uint8_t  g_events;
extern uint32_t g_frame;   // 幀數。要動畫就讀它，別讀時鐘。
#define EV_JUMP_A 1
#define EV_JUMP_B 2
#define EV_LAND_A 4
#define EV_LAND_B 8

// 確定性模擬：同樣的輸入序列 → 同樣的畫面。無隨機、無浮點、不讀時鐘。
void sim_init(void);
void sim_tick(const Input in[2]);
void sim_draw(void);

// 多邊形掃描填色（even-odd）。pts = x0,y0,x1,y1,... 16.0 定點。
void poly_fill(const int16_t *pts, int n, uint8_t ci);

// 多輪廓版本：lens[] 是每個輪廓的點數。even-odd 規則讓「洞」自然發生——
// 掃描線同時看到所有輪廓的邊，穿進外輪廓算 1 次、穿進內輪廓算第 2 次 → 不填。
// 不需要任何「這是洞」的旗標，也不需要判斷纏繞方向：規則自己會算。
void poly_fill_n(const int16_t *pts, const uint16_t *lens, int nc, uint8_t ci);

void fb_clear(uint8_t ci);

#endif
