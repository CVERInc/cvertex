// shape.c — 畫一個平面向量圖形。整個檔案 30 行左右，因為真正的工作
// (曲線攤平、顏色聚類) 在 build 時就做完了，執行期只剩座標乘法。
#include "shape.h"
#include "core.h"

#define MAXXFORM 4096

void shape_install_palette(const Shape *s) {
    for (int i = 0; i < s->npal; i++) g_pal[s->pal_base + i] = s->pal[i];
}

void shape_draw(const Shape *s, int cx, int cy, int h) {
    static int16_t t[MAXXFORM * 2];
    for (uint16_t i = 0; i < s->nf; i++) {
        const Fill *f = &s->f[i];
        int n = 0;
        for (int c = 0; c < f->nc; c++) n += s->lens[f->len_off + c];
        if (n > MAXXFORM) continue;

        const int16_t *src = &s->pts[f->pt_off * 2];
        for (int p = 0; p < n; p++) {
            // 正規化座標（最長邊 32768）→ 像素。一個乘法一個位移，沒有除法。
            t[p * 2]     = (int16_t)(cx + (((int32_t)src[p * 2]     * h) >> 15));
            t[p * 2 + 1] = (int16_t)(cy + (((int32_t)src[p * 2 + 1] * h) >> 15));
        }
        poly_fill_n(t, &s->lens[f->len_off], f->nc, f->ci);
    }
}
