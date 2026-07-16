// shape.h — 一個平面向量圖形：一疊填色，由後往前畫。
// 資料由 tools/svg2poly 產生，烘進 binary（同 obj2mesh 的理由：沒有載入器）。
#ifndef SHAPE_H
#define SHAPE_H
#include <stdint.h>

// 一塊填色。輪廓可能不只一個——外框加它的洞——even-odd 讓洞自己成立。
// 用 offset 不用指標：指標一顆 8 bytes，offset 2 bytes，而且不需要重定位。
typedef struct {
    uint16_t pt_off;    // 進 pts[] 的點索引（不是 byte）
    uint16_t len_off;   // 進 lens[] 的索引
    uint8_t  nc;        // 幾個輪廓
    uint8_t  ci;        // 調色盤索引
} Fill;

typedef struct {
    const int16_t  *pts;    // x,y 交錯。座標已正規化：最長邊 = 32768（±16384，中心在原點）
    const uint16_t *lens;   // 每個輪廓的點數
    const Fill     *f;      // 由後往前的繪製順序（＝SVG 的文件順序）
    uint16_t        nf;
    const uint32_t *pal;    // 這個圖形要的顏色，0xAARRGGBB
    uint8_t         npal, pal_base;
} Shape;

void shape_install_palette(const Shape *s);            // 把 pal 灌進 g_pal[pal_base..]
void shape_draw(const Shape *s, int cx, int cy, int h); // h = 想要的高度（像素）

#endif
