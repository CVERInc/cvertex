// obj2mesh — .obj → 一顆烘進 binary 的 const mesh。build 時跑，不進遊戲。
//
// 為什麼烘進 binary 而不是讀檔：沒有載入器、沒有檔案格式、沒有 I/O、沒有錯誤處理。
// const 資料住 __TEXT，本來就有整頁空著。零執行期成本。
//
// 平塗打光只要「面法線」，所以 .obj 裡的 vn 一律忽略、法線一律由幾何算——
// 這也順便讓任何工具吐出來的 obj 都能吃，不管它寫不寫 vn。
//
//   cc -O2 -o obj2mesh tools/obj2mesh.c -lm
//   ./obj2mesh oga.obj oga > src/mesh_oga.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAXV 8192
#define MAXT 16384

static double vx[MAXV], vy[MAXV], vz[MAXV];
static int ta[MAXT], tb[MAXT], tc[MAXT], tm[MAXT];
static int nv, nt;

// usemtl 名字 → 調色盤基色。名字裡有數字就用它，否則依出現順序給坡道。
static char mtl[64][64];
static int nmtl, cur_mtl;

static int mtl_index(const char *name) {
    for (int i = 0; i < nmtl; i++) if (!strcmp(mtl[i], name)) return i;
    if (nmtl < 64) { snprintf(mtl[nmtl], 64, "%s", name); return nmtl++; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "用法: obj2mesh <in.obj> <名字> [每個材質的階數=8]\n"); return 1; }
    const char *name = argv[2];
    int shades = argc > 3 ? atoi(argv[3]) : 8;

    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "obj2mesh: 開不了 %s\n", argv[1]); return 1; }

    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "v ", 2)) {
            if (nv >= MAXV) { fprintf(stderr, "obj2mesh: 頂點超過 %d\n", MAXV); return 1; }
            sscanf(line + 2, "%lf %lf %lf", &vx[nv], &vy[nv], &vz[nv]); nv++;
        } else if (!strncmp(line, "usemtl ", 7)) {
            char m[64]; sscanf(line + 7, "%63s", m); cur_mtl = mtl_index(m);
        } else if (!strncmp(line, "f ", 2)) {
            // 吃 "f a b c"、"f a/b c/d ..."、"f a//b ..."，並把 n 邊形扇形三角化
            int idx[64], n = 0;
            char *p = line + 2;
            while (*p && n < 64) {
                while (*p == ' ' || *p == '\t') p++;
                if (*p < '0' || *p > '9') { if (*p == '-') {} else break; }
                int v = atoi(p);
                if (v < 0) v = nv + v; else v -= 1;      // obj 是 1-based；負數是相對索引
                idx[n++] = v;
                while (*p && *p != ' ' && *p != '\t') p++;
            }
            for (int i = 1; i + 1 < n; i++) {
                if (nt >= MAXT) { fprintf(stderr, "obj2mesh: 三角形超過 %d\n", MAXT); return 1; }
                ta[nt] = idx[0]; tb[nt] = idx[i]; tc[nt] = idx[i + 1]; tm[nt] = cur_mtl; nt++;
            }
        }
    }
    fclose(f);
    if (!nv || !nt) { fprintf(stderr, "obj2mesh: %s 裡沒有幾何\n", argv[1]); return 1; }

    // 置中並縮放到 ±0.5 單位（= ±(1<<15)）。模型自己多大不重要，擺放交給引擎。
    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
    for (int i = 0; i < nv; i++) {
        double v[3] = { vx[i], vy[i], vz[i] };
        for (int k = 0; k < 3; k++) { if (v[k] < lo[k]) lo[k] = v[k]; if (v[k] > hi[k]) hi[k] = v[k]; }
    }
    double ext = 0;
    for (int k = 0; k < 3; k++) if (hi[k] - lo[k] > ext) ext = hi[k] - lo[k];
    if (ext <= 0) ext = 1;
    double s = 65536.0 / ext;   // 最長邊 → 1.0 單位

    printf("// 由 obj2mesh 從 %s 產生。別手改——改模型再跑一次 build。\n", argv[1]);
    printf("// %d 頂點 / %d 三角形 / %d 材質\n", nv, nt, nmtl ? nmtl : 1);
    printf("static const V3 %s_v[%d] = {\n", name, nv);
    for (int i = 0; i < nv; i++) {
        long X = lrint((vx[i] - (lo[0] + hi[0]) / 2) * s);
        long Y = lrint((vy[i] - (lo[1] + hi[1]) / 2) * s);
        long Z = lrint((vz[i] - (lo[2] + hi[2]) / 2) * s);
        printf("  {%ld,%ld,%ld},%s", X, Y, Z, (i % 4 == 3) ? "\n" : "");
    }
    printf("\n};\nstatic const Tri %s_t[%d] = {\n", name, nt);
    for (int i = 0; i < nt; i++) {
        // 面法線 = (b-a) × (c-a)，正規化到 Q15。這是這個工具唯一真正的工作。
        double ux = vx[tb[i]] - vx[ta[i]], uy = vy[tb[i]] - vy[ta[i]], uz = vz[tb[i]] - vz[ta[i]];
        double wx = vx[tc[i]] - vx[ta[i]], wy = vy[tc[i]] - vy[ta[i]], wz = vz[tc[i]] - vz[ta[i]];
        double nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        double L = sqrt(nx * nx + ny * ny + nz * nz);
        if (L < 1e-12) L = 1;                       // 退化面：法線給 0，打光會是最暗階
        int NX = (int)lrint(nx / L * 32767), NY = (int)lrint(ny / L * 32767), NZ = (int)lrint(nz / L * 32767);
        printf("  {%d,%d,%d,%d,%d,%d,%d},\n", ta[i], tb[i], tc[i], 8 + tm[i] * shades, NX, NY, NZ);
    }
    printf("};\nconst Mesh g_%s = { %s_v, %d, %s_t, %d };\n", name, name, nv, name, nt);

    fprintf(stderr, "obj2mesh: %s → %d 頂點 %d 三角形，%d bytes 的模型資料\n",
            argv[1], nv, nt, nv * 12 + nt * 14);
    return 0;
}
