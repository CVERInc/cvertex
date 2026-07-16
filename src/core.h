// core.h — 引擎核心。零依賴，只用 stdint。
#ifndef CORE_H
#define CORE_H
#include <stdint.h>

#define FBW 320
#define FBH 180
#define MAXPTS 64

// 調色盤索引 framebuffer：一個 byte 一個 pixel。
extern uint8_t  g_fb[FBW * FBH];
extern uint32_t g_pal[256];   // 0xAARRGGBB

// 輸入：每個角色一份。狀態只由這個結構改變。
typedef struct { int8_t x, y; uint8_t act; } Input;

// 確定性模擬：同樣的輸入序列 → 同樣的畫面。無隨機、無浮點、不讀時鐘。
void sim_init(void);
void sim_tick(const Input in[2]);
void sim_draw(void);

// 多邊形掃描填色（even-odd）。pts = x0,y0,x1,y1,... 16.0 定點。
void poly_fill(const int16_t *pts, int n, uint8_t ci);
void fb_clear(uint8_t ci);

#endif
