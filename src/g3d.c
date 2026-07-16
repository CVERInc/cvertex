// g3d.c — 定點數 3D。整個檔案沒有一個 float。
#include "g3d.h"
#include "core.h"

#define PROJ  180              // 焦距（像素）
#define NEAR  (1 << 14)        // 近平面

static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
#define SIN(a) g_sin[(a) & 1023]
#define COS(a) g_sin[((a) + 256) & 1023]

// 繞 Y → X → Z 依序轉。分開轉比組矩陣省碼，而且看得懂。
static void rot(int32_t *px, int32_t *py, int32_t *pz, int ax, int ay, int az) {
    int32_t x = *px, y = *py, z = *pz, t;
    int32_t s = SIN(ay), c = COS(ay);
    t = mul15(x, c) + mul15(z, s);  z = mul15(z, c) - mul15(x, s);  x = t;
    s = SIN(ax); c = COS(ax);
    t = mul15(y, c) - mul15(z, s);  z = mul15(z, c) + mul15(y, s);  y = t;
    s = SIN(az); c = COS(az);
    t = mul15(x, c) - mul15(y, s);  y = mul15(y, c) + mul15(x, s);  x = t;
    *px = x; *py = y; *pz = z;
}

#define MAXV 64
#define MAXT 128

void g3d_draw(const Mesh *m, int ax, int ay, int az, int32_t tz) {
    int32_t vx[MAXV], vy[MAXV], vz[MAXV];
    int16_t sx[MAXV], sy[MAXV];

    // ---- 頂點：旋轉 → 平移 → 投影
    for (int i = 0; i < m->nv && i < MAXV; i++) {
        int32_t x = m->v[i].x, y = m->v[i].y, z = m->v[i].z;
        rot(&x, &y, &z, ax, ay, az);
        z += tz;
        vx[i] = x; vy[i] = y; vz[i] = z;
        int32_t zz = z < NEAR ? NEAR : z;               // 投影用的安全 z
        sx[i] = (int16_t)(FBW / 2 + (int32_t)(((int64_t)x * PROJ) / zz));
        sy[i] = (int16_t)(FBH / 2 - (int32_t)(((int64_t)y * PROJ) / zz));
    }

    // ---- 深度排序（畫家演算法）：遠的先畫
    int order[MAXT], n = 0;
    int32_t depth[MAXT];
    for (int i = 0; i < m->nt && i < MAXT; i++) {
        const Tri *t = &m->t[i];
        // ⚠️ 近平面：任一頂點在鏡頭後面就整片丟掉。這是 spike 的偷吃步——
        // 真正的裁切要把三角形切開重組。物件不穿過鏡頭時看不出差別。
        if (vz[t->a] < NEAR || vz[t->b] < NEAR || vz[t->c] < NEAR) continue;
        // 背面剔除：螢幕空間叉積。
        // ⚠️ 取負號不是筆誤：螢幕 y 向下（sy = FBH/2 - y）翻掉了手性，
        // 世界空間的正面在螢幕上是順時針。取錯邊的話剪影一模一樣、只有打光會露餡。
        int32_t cx = (int32_t)(sx[t->b] - sx[t->a]) * (sy[t->c] - sy[t->a])
                   - (int32_t)(sx[t->c] - sx[t->a]) * (sy[t->b] - sy[t->a]);
        if (cx >= 0) continue;
        depth[n] = vz[t->a] + vz[t->b] + vz[t->c];
        order[n++] = i;
    }
    for (int a = 1; a < n; a++) {                        // 插入排序，由遠到近
        int oi = order[a]; int32_t d = depth[a]; int b = a - 1;
        while (b >= 0 && depth[b] < d) { depth[b + 1] = depth[b]; order[b + 1] = order[b]; b--; }
        depth[b + 1] = d; order[b + 1] = oi;
    }

    // ---- 填色
    for (int k = 0; k < n; k++) {
        const Tri *t = &m->t[order[k]];
        // 打光：把模型法線用同一個旋轉轉過去，取 z。單位法線轉完還是單位法線，
        // 所以不用正規化、不用開根號——這就是把法線存進資料裡的理由。
        int32_t nx = t->nx, ny = t->ny, nz = t->nz;
        rot(&nx, &ny, &nz, ax, ay, az);
        int shade = (-nz * 8) >> 15;                     // 光從鏡頭來
        if (shade < 0) shade = 0;
        if (shade > 7) shade = 7;
        int16_t p[6] = { sx[t->a], sy[t->a], sx[t->b], sy[t->b], sx[t->c], sy[t->c] };
        poly_fill(p, 3, (uint8_t)(t->ci + shade));
    }
}

// ---- 一顆立方體。8 個頂點 + 12 個三角形 = 這個模型 216 bytes。
#define U (1 << 15)
static const V3 cube_v[8] = {
    { -U, -U, -U }, {  U, -U, -U }, {  U,  U, -U }, { -U,  U, -U },
    { -U, -U,  U }, {  U, -U,  U }, {  U,  U,  U }, { -U,  U,  U },
};
#define N 32767
static const Tri cube_t[12] = {
    { 0, 1, 2, 8,  0, 0, -N }, { 0, 2, 3, 8,  0, 0, -N },   // 前
    { 5, 4, 7, 8,  0, 0,  N }, { 5, 7, 6, 8,  0, 0,  N },   // 後
    { 4, 0, 3, 8, -N, 0,  0 }, { 4, 3, 7, 8, -N, 0,  0 },   // 左
    { 1, 5, 6, 8,  N, 0,  0 }, { 1, 6, 2, 8,  N, 0,  0 },   // 右
    { 4, 5, 1, 8,  0,-N,  0 }, { 4, 1, 0, 8,  0,-N,  0 },   // 下
    { 3, 2, 6, 8,  0, N,  0 }, { 3, 6, 7, 8,  0, N,  0 },   // 上
};
const Mesh g_cube = { cube_v, 8, cube_t, 12 };
