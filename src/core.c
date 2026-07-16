// core.c — framebuffer + 多邊形 rasterizer + 確定性模擬。純 C，零依賴。
#include "core.h"
#include "g3d.h"
#include <math.h>

int16_t g_sin[1024];

void tables_init(void) {
    for (int i = 0; i < 1024; i++)
        g_sin[i] = (int16_t)(sinf((float)i * 6.2831853f / 1024.f) * 32767.f);
}

uint8_t  g_fb[FBW * FBH];
uint32_t g_pal[256];

void fb_clear(uint8_t ci) {
    for (int i = 0; i < FBW * FBH; i++) g_fb[i] = ci;
}

// 掃描線填色：每條掃描線求所有邊的交點，排序，成對填。
// even-odd 規則。整數運算，無浮點 → 確定性。
void poly_fill(const int16_t *pts, int n, uint8_t ci) {
    if (n < 3) return;
    int miny = pts[1], maxy = pts[1];
    for (int i = 1; i < n; i++) {
        int y = pts[i * 2 + 1];
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (miny < 0) miny = 0;
    if (maxy > FBH - 1) maxy = FBH - 1;

    int xs[MAXPTS];
    for (int y = miny; y <= maxy; y++) {
        int cnt = 0;
        for (int i = 0; i < n && cnt < MAXPTS; i++) {
            int j  = (i + 1) % n;
            int y0 = pts[i * 2 + 1], y1 = pts[j * 2 + 1];
            if (y0 == y1) continue;
            // 半開區間 [min,max) → 頂點不重複計數
            int ymin = y0 < y1 ? y0 : y1;
            int ymax = y0 < y1 ? y1 : y0;
            if (y < ymin || y >= ymax) continue;
            int x0 = pts[i * 2], x1 = pts[j * 2];
            xs[cnt++] = x0 + (int)((int32_t)(y - y0) * (x1 - x0) / (y1 - y0));
        }
        // 插入排序（cnt 很小）
        for (int a = 1; a < cnt; a++) {
            int v = xs[a], b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < cnt; a += 2) {
            int L = xs[a], R = xs[a + 1];
            if (R < 0 || L > FBW - 1) continue;
            if (L < 0) L = 0;
            if (R > FBW - 1) R = FBW - 1;
            uint8_t *row = &g_fb[y * FBW];
            for (int x = L; x <= R; x++) row[x] = ci;
        }
    }
}

// ---- 確定性模擬 ----------------------------------------------------
// 定點數：1 單位 = 1/256 pixel。整數運算 → 位元完全可重現。
#define FP 8
typedef struct { int32_t x, y, vx, vy; uint8_t grounded; } Actor;
static Actor g_act[2];
uint64_t g_checksum;   // 決定性自我檢查

void sim_init(void) {
    tables_init();
    g_act[0] = (Actor){ 80 << FP, 100 << FP, 0, 0, 0 };
    g_act[1] = (Actor){ 240 << FP, 100 << FP, 0, 0, 0 };
    g_checksum = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;  // 背景
    g_pal[1] = 0xFF5D275D;  // 地面
    g_pal[2] = 0xFFEF7D57;  // 角色 A
    g_pal[3] = 0xFF41A6F6;  // 角色 B

    // 8..15＝一個材質的八階明暗坡。3D 打光不改像素，只是往坡上挑一格——
    // 這就是調色盤索引的紅利：光影是查表，不是運算。
    for (int i = 0; i < 8; i++) {
        int r = 30 + i * 26, g = 90 + i * 22, b = 120 + i * 18;
        g_pal[8 + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    g_frame = 0;
}

uint32_t g_frame;

#define GROUND (150 << FP)

uint8_t g_events;

void sim_tick(const Input in[2]) {
    g_events = 0;
    g_frame++;   // 旋轉角度由幀數決定，不讀時鐘 → 確定性不破
    for (int i = 0; i < 2; i++) {
        Actor *a = &g_act[i];
        a->vx = in[i].x * (2 << FP) / 2;
        if (in[i].act && a->grounded) {
            a->vy = -(5 << FP); a->grounded = 0;
            g_events |= (EV_JUMP_A << i);
        }
        a->vy += (1 << FP) / 4;                 // 重力
        a->x += a->vx;
        a->y += a->vy;
        if (a->y >= GROUND) {
            if (!a->grounded) g_events |= (EV_LAND_A << i);
            a->y = GROUND; a->vy = 0; a->grounded = 1;
        }
        if (a->x < (8 << FP))         a->x = 8 << FP;
        if (a->x > ((FBW - 8) << FP)) a->x = (FBW - 8) << FP;
        g_checksum = g_checksum * 31 + (uint32_t)a->x + (uint32_t)a->y;
    }
}

void sim_draw(void) {
    fb_clear(0);

    // 3D 在 2D 後面轉。這就是「渲染全 3D、玩法留 2D」的最小證明。
    g3d_draw(&g_cube, (int)(g_frame * 3 / 2), (int)(g_frame * 2), 0, 5 << 16);

    int16_t ground[8] = { 0, 158, FBW, 158, FBW, FBH, 0, FBH };
    poly_fill(ground, 4, 1);

    // 兩個角色，各自一顆多邊形（spike 用手寫形狀代替 motifmint 資產）
    for (int i = 0; i < 2; i++) {
        int cx = g_act[i].x >> FP, cy = g_act[i].y >> FP;
        int16_t body[12] = {
            cx,     cy - 12,
            cx + 7, cy - 4,
            cx + 5, cy + 8,
            cx,     cy + 4,
            cx - 5, cy + 8,
            cx - 7, cy - 4,
        };
        poly_fill(body, 6, 2 + i);
    }
}
