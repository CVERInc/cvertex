// shape.c — draw a flat vector graphic. The whole file is about 30 lines, because
// the real work (flattening curves, clustering colours) already happened at build
// time — runtime is left with nothing but coordinate multiplies.
#include "shape.h"
#include "core.h"
#include "g3d.h"

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
            // normalized coordinate (longest edge 32768) → pixel. One multiply, one shift, no division.
            t[p * 2]     = (int16_t)(cx + (((int32_t)src[p * 2]     * h) >> 15));
            t[p * 2 + 1] = (int16_t)(cy + (((int32_t)src[p * 2 + 1] * h) >> 15));
        }
        poly_fill_n(t, &s->lens[f->len_off], f->nc, f->ci);
    }
}

// ---- extrusion --------------------------------------------------------------

// Shape space (±16384, y down) → world (16.16, y up) → rotate → project.
static void project_pts(const int16_t *src, int n, int ax, int ay, int az,
                        int32_t wx, int32_t wy, int32_t wz,
                        int32_t size, int32_t z0, int flip, int16_t *ox, int16_t *oy) {
    for (int p = 0; p < n; p++) {
        int32_t x =  (int32_t)(((int64_t)src[p * 2]     * size) >> 15);
        int32_t y = -(int32_t)(((int64_t)src[p * 2 + 1] * size) >> 15);  // SVG y goes down, the world's goes up
        int32_t z = z0;
        if (flip) x = -x;
        g3d_rot(&x, &y, &z, ax, ay, az);
        g3d_project(x + wx, y + wy, z + wz, &ox[p], &oy[p]);
    }
}

int32_t world_per_px(int32_t wz) { return (int32_t)(((int64_t)wz << 16) / (180 << 16)); }

void shape_draw3d(const Shape *s, int32_t wx, int32_t wy, int32_t wz,
                  int ax, int ay, int az, int32_t size, int32_t depth, int flip) {
    static int16_t fx[MAXXFORM], fy[MAXXFORM], bx[MAXXFORM], by[MAXXFORM];
    if (!s->nf) return;
    int32_t zf = -depth / 2, zb = depth / 2;   // front is nearer the camera

    // ---- back face + side walls, both from the silhouette.
    const Fill *sil = &s->f[0];
    int n = 0;
    for (int c = 0; c < sil->nc; c++) n += s->lens[sil->len_off + c];
    if (n <= MAXXFORM) {
        const int16_t *src = &s->pts[sil->pt_off * 2];
        project_pts(src, n, ax, ay, az, wx, wy, wz, size, zf, flip, fx, fy);
        project_pts(src, n, ax, ay, az, wx, wy, wz, size, zb, flip, bx, by);

        // The back of a paper cutout is the cutout: one flat silhouette in the outline
        // colour. Drawn first, so the walls and the artwork land on top of it.
        {
            static int16_t t[MAXXFORM * 2];
            for (int p = 0; p < n; p++) { t[p * 2] = bx[p]; t[p * 2 + 1] = by[p]; }
            poly_fill_n(t, &s->lens[sil->len_off], sil->nc, s->pal_base);
        }

        int base = 0;
        for (int c = 0; c < sil->nc; c++) {
            int len = s->lens[sil->len_off + c];
            for (int i = 0; i < len; i++) {
                int p = base + i, q = base + (i + 1) % len;
                int16_t quad[8] = { fx[p], fy[p], fx[q], fy[q], bx[q], by[q], bx[p], by[p] };
                // Cull by screen winding: half these walls face away and would only
                // fight the front face for the same pixels.
                int32_t cr = (int32_t)(quad[2] - quad[0]) * (quad[5] - quad[1])
                           - (int32_t)(quad[4] - quad[0]) * (quad[3] - quad[1]);
                if (cr <= 0) continue;
                poly_fill(quad, 4, s->pal_base);   // the outline colour: a drawn line, given thickness
            }
            base += len;
        }
    }

    // ---- front face: the artwork itself, flat, riding the same transform.
    for (uint16_t i = 0; i < s->nf; i++) {
        const Fill *f = &s->f[i];
        int m = 0;
        for (int c = 0; c < f->nc; c++) m += s->lens[f->len_off + c];
        if (m > MAXXFORM) continue;
        project_pts(&s->pts[f->pt_off * 2], m, ax, ay, az, wx, wy, wz, size, zf, flip, fx, fy);
        static int16_t t[MAXXFORM * 2];
        for (int p = 0; p < m; p++) { t[p * 2] = fx[p]; t[p * 2 + 1] = fy[p]; }
        poly_fill_n(t, &s->lens[f->len_off], f->nc, f->ci);
    }
}
