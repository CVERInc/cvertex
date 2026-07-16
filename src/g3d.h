// g3d.h — 定點數 3D。一顆軟體 Cx4。
// 沒有浮點、沒有 GPU、沒有貼圖。旋轉查表、投影一次除法、平塗填色走既有的 rasterizer。
#ifndef G3D_H
#define G3D_H
#include <stdint.h>

typedef struct { int32_t x, y, z; } V3;              // 16.16 定點，1<<16 = 1 單位

// 一個三角形 10 bytes：三個頂點索引 + 調色盤基色 + 模型空間法線（Q15）。
// 法線存起來不是偷懶，是 90 年代的正解——單位法線經過旋轉仍是單位法線，
// 所以打光只要旋轉它、取 z，不必開根號。
typedef struct { uint8_t a, b, c, ci; int16_t nx, ny, nz; } Tri;

typedef struct { const V3 *v; int nv; const Tri *t; int nt; } Mesh;

extern const Mesh g_cube;

// ax/ay/az：0..1023 的角度（直接是 g_sin 的索引）。tz：鏡頭距離，16.16。
void g3d_draw(const Mesh *m, int ax, int ay, int az, int32_t tz);

#endif
