// g3d.c — fixed-point 3D. Not one float anywhere in this file.
#include "g3d.h"
#include "core.h"
#include "mesh_torus.h"

#define PROJ  180              // focal length (pixels)
#define NEAR  (1 << 14)        // near plane

static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
#define SIN(a) g_sin[(a) & 1023]
#define COS(a) g_sin[((a) + 256) & 1023]

// Rotate Y, then X, then Z. Separate rotations cost less code than composing a
// matrix, and they're easier to read.
void g3d_rot(int32_t *px, int32_t *py, int32_t *pz, int ax, int ay, int az) {
    int32_t x = *px, y = *py, z = *pz, t;
    int32_t s = SIN(ay), c = COS(ay);
    t = mul15(x, c) + mul15(z, s);  z = mul15(z, c) - mul15(x, s);  x = t;
    s = SIN(ax); c = COS(ax);
    t = mul15(y, c) - mul15(z, s);  z = mul15(z, c) + mul15(y, s);  y = t;
    s = SIN(az); c = COS(az);
    t = mul15(x, c) - mul15(y, s);  y = mul15(y, c) + mul15(x, s);  x = t;
    *px = x; *py = y; *pz = z;
}

// One projection formula, used by meshes and shapes alike. Two copies would drift.
void g3d_project(int32_t x, int32_t y, int32_t z, int16_t *sx, int16_t *sy) {
    int32_t zz = z < NEAR ? NEAR : z;                   // clamped z, safe to divide by
    *sx = (int16_t)(FBW / 2 + (int32_t)(((int64_t)x * PROJ) / zz));
    *sy = (int16_t)(FBH / 2 - (int32_t)(((int64_t)y * PROJ) / zz));
}

#define MAXV 2048
#define MAXT 4096

void g3d_draw(const Mesh *m, int ax, int ay, int az, int32_t tz) {
    int32_t vx[MAXV], vy[MAXV], vz[MAXV];
    int16_t sx[MAXV], sy[MAXV];

    // ---- vertices: rotate → translate → project
    for (int i = 0; i < m->nv && i < MAXV; i++) {
        int32_t x = m->v[i].x, y = m->v[i].y, z = m->v[i].z;
        g3d_rot(&x, &y, &z, ax, ay, az);
        z += tz;
        vx[i] = x; vy[i] = y; vz[i] = z;
        g3d_project(x, y, z, &sx[i], &sy[i]);
    }

    // ---- culling + depth sort (painter's algorithm): far triangles draw first
    //
    // 🔴 Culling and lighting are only allowed one source of truth: the model normal.
    // Culling used to read a screen-space cross product while lighting read the
    // normal — two independent ideas of "which way is this facing," with nothing
    // forcing them to agree. The cube's normals were hand-typed (facing out) so
    // it happened to work; the torus's normals came out of the winding order
    // (facing in) and it broke: the shape looked completely normal but the
    // lighting was dead. That's the hardest kind of bug — it looks correct.
    // Now both paths read rn[]: flip a model's normals and the whole object turns
    // inside out, which is a mistake you can see and understand in a modelling tool.
    int order[MAXT], n = 0;
    int32_t depth[MAXT];
    static int16_t rnz[MAXT];                            // rotated normal z, kept for lighting
    for (int i = 0; i < m->nt && i < MAXT; i++) {
        const Tri *t = &m->t[i];
        // ⚠️ Near plane: drop the whole triangle if any vertex is behind the camera.
        // This is spike's shortcut — real clipping would slice the triangle and
        // rebuild it. Invisible as long as geometry never crosses the camera.
        if (vz[t->a] < NEAR || vz[t->b] < NEAR || vz[t->c] < NEAR) continue;

        int32_t nx = t->nx, ny = t->ny, nz = t->nz;
        g3d_rot(&nx, &ny, &nz, ax, ay, az);
        // Backface cull: normal · (face centroid − camera). The camera sits at the
        // view-space origin, so dotting the centroid directly is enough. This is
        // exact under perspective (not the orthographic nz-only approximation) and
        // cheaper than a cross product.
        int32_t cx = (vx[t->a] + vx[t->b] + vx[t->c]) / 3;
        int32_t cy = (vy[t->a] + vy[t->b] + vy[t->c]) / 3;
        int32_t cz = (vz[t->a] + vz[t->b] + vz[t->c]) / 3;
        int64_t dot = (int64_t)nx * cx + (int64_t)ny * cy + (int64_t)nz * cz;
        if (dot >= 0) continue;                          // facing away from camera
        rnz[n] = (int16_t)nz;
        depth[n] = vz[t->a] + vz[t->b] + vz[t->c];
        order[n++] = i;
    }
    for (int a = 1; a < n; a++) {                        // insertion sort, far to near
        int oi = order[a]; int32_t d = depth[a]; int b = a - 1;
        while (b >= 0 && depth[b] < d) { depth[b + 1] = depth[b]; order[b + 1] = order[b]; b--; }
        depth[b + 1] = d; order[b + 1] = oi;
    }

    // ---- fill
    for (int k = 0; k < n; k++) {
        const Tri *t = &m->t[order[k]];
        // Lighting: run the model normal through the same rotation, take z. A unit
        // normal is still a unit normal after rotating, so no normalizing, no square
        // root — that's the whole reason the normal is stored in the data.
        int32_t nx = t->nx, ny = t->ny, nz = t->nz;
        g3d_rot(&nx, &ny, &nz, ax, ay, az);
        int shade = (-nz * 8) >> 15;                     // light comes from the camera
        if (shade < 0) shade = 0;
        if (shade > 7) shade = 7;
        int16_t p[6] = { sx[t->a], sy[t->a], sx[t->b], sy[t->b], sx[t->c], sy[t->c] };
        poly_fill(p, 3, (uint8_t)(t->ci + shade));
    }
}

// ---- A cube. 8 vertices + 12 triangles = 216 bytes for this model.
#define U (1 << 15)
static const V3 cube_v[8] = {
    { -U, -U, -U }, {  U, -U, -U }, {  U,  U, -U }, { -U,  U, -U },
    { -U, -U,  U }, {  U, -U,  U }, {  U,  U,  U }, { -U,  U,  U },
};
#define N 32767
static const Tri cube_t[12] = {
    { 0, 1, 2, 8,  0, 0, -N }, { 0, 2, 3, 8,  0, 0, -N },   // front
    { 5, 4, 7, 8,  0, 0,  N }, { 5, 7, 6, 8,  0, 0,  N },   // back
    { 4, 0, 3, 8, -N, 0,  0 }, { 4, 3, 7, 8, -N, 0,  0 },   // left
    { 1, 5, 6, 8,  N, 0,  0 }, { 1, 6, 2, 8,  N, 0,  0 },   // right
    { 4, 5, 1, 8,  0,-N,  0 }, { 4, 1, 0, 8,  0,-N,  0 },   // bottom
    { 3, 2, 6, 8,  0, N,  0 }, { 3, 6, 7, 8,  0, N,  0 },   // top
};
const Mesh g_cube = { cube_v, 8, cube_t, 12 };
