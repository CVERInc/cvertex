// g3d.c — fixed-point 3D. Not one float anywhere in this file.
#include "g3d.h"
#include "core.h"
#include "mesh_torus.h"

// Focal length tracks the framebuffer, so the field of view is the same picture at
// any resolution — only the sampling gets finer.
#define PROJ  (g_fbh)
#define NEAR  (1 << 14)        // near plane

static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
#define SIN(a) g_sin[(a) & 1023]
#define COS(a) g_sin[((a) + 256) & 1023]

// Rotate Y, then X, then Z. Separate rotations cost less code than composing a
// matrix, and they're easier to read.
// Where the segment a->b crosses the near plane, and the point there.
static void near_cut(const P3 *a, const P3 *b, P3 *out) {
    int64_t d = (int64_t)b->z - a->z;
    int64_t t = d ? (((int64_t)(NEAR - a->z) << 16) / d) : 0;   // 16.16 along the edge
    out->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * t) >> 16);
    out->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * t) >> 16);
    out->z = NEAR;
}

static void fill_tri(const P3 *v, uint8_t ci) {
    int16_t p[6];
    uint32_t w[3];
    for (int i = 0; i < 3; i++) {
        g3d_project(v[i].x, v[i].y, v[i].z, &p[i * 2], &p[i * 2 + 1]);
        int32_t z = v[i].z > NEAR ? v[i].z : NEAR;
        w[i] = (uint32_t)(((int64_t)1 << 30) / z);
    }
    tri_fill_z(p, w, ci);
}

// How far outside the frame a vertex may land before we have to cut it away.
//
// 🔴 Screen coordinates are int16. A wall standing right beside the lens is barely any
// distance away and enormously far to the side, so x * PROJ / z runs into five figures and
// WRAPS — and a wall fifteen cells behind your shoulder reappears smeared across the middle
// of the screen, painting the whole view one flat colour. That is a picture with no bug
// visible in it anywhere: every triangle is correct, every normal is correct, and the frame
// is a solid rectangle. Cutting at a generous boundary well outside the frame keeps the
// numbers inside int16 while leaving everything that was already drawable untouched.
#define GUARD 8192

// Does this vertex project inside the guard box? z is always >= NEAR here, so no division.
static int in_guard(const P3 *v) {
    int64_t gz = (int64_t)GUARD * v->z;
    int64_t px = (int64_t)v->x * PROJ, py = (int64_t)v->y * PROJ;
    return px <= gz && -px <= gz && py <= gz && -py <= gz;
}

// Where edge a->b crosses one guard plane. da/db are the signed distances the caller
// already computed; a is the vertex on the inside, so the rounding leans the same way
// near_cut's does.
static void guard_cut(const P3 *a, const P3 *b, int64_t da, int64_t db, P3 *out) {
    int64_t den = da - db;
    int64_t t = den ? ((da << 16) / den) : 0;
    out->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * t) >> 16);
    out->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * t) >> 16);
    out->z = a->z + (int32_t)(((int64_t)(b->z - a->z) * t) >> 16);
}

// Signed distance to guard plane `p`: left, right, bottom, top. Inside is >= 0.
static int64_t guard_d(int p, const P3 *v) {
    int64_t gz = (int64_t)GUARD * v->z;
    switch (p) {
        case 0:  return gz + (int64_t)v->x * PROJ;
        case 1:  return gz - (int64_t)v->x * PROJ;
        case 2:  return gz + (int64_t)v->y * PROJ;
        default: return gz - (int64_t)v->y * PROJ;
    }
}

static void raw_tri(const P3 *v, uint8_t ci) {
    // The overwhelmingly common case: the triangle is already inside the guard box, so it
    // takes the same path it always took and comes out pixel for pixel the same.
    if (in_guard(&v[0]) && in_guard(&v[1]) && in_guard(&v[2])) { fill_tri(v, ci); return; }

    // Sutherland-Hodgman against the four sides. Each plane can add at most one vertex, so a
    // triangle comes out of all four with seven at the very most.
    P3 poly[8], tmp[8];
    int n = 3;
    poly[0] = v[0]; poly[1] = v[1]; poly[2] = v[2];

    for (int p = 0; p < 4 && n >= 3; p++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const P3 *cur = &poly[i], *prev = &poly[(i + n - 1) % n];
            int64_t dc = guard_d(p, cur), dp = guard_d(p, prev);
            if ((dc >= 0) != (dp >= 0)) {
                if (dp >= 0) guard_cut(prev, cur, dp, dc, &tmp[m++]);
                else         guard_cut(cur, prev, dc, dp, &tmp[m++]);
            }
            if (dc >= 0) tmp[m++] = *cur;
        }
        n = m;
        for (int i = 0; i < n; i++) poly[i] = tmp[i];
    }

    for (int i = 1; i + 1 < n; i++) {
        P3 t[3] = { poly[0], poly[i], poly[i + 1] };
        fill_tri(t, ci);
    }
}

// Sutherland-Hodgman against one plane, which for a triangle has only four outcomes:
// all in, all out, one in (a smaller triangle), or two in (a quad = two triangles).
void g3d_tri(const P3 *v, uint8_t ci) {
    const P3 *in[3], *out[3];
    int ni = 0, no = 0;
    for (int i = 0; i < 3; i++) {
        if (v[i].z >= NEAR) in[ni++] = &v[i]; else out[no++] = &v[i];
    }
    if (no == 0) { raw_tri(v, ci); return; }
    if (ni == 0) return;

    if (ni == 1) {
        P3 t[3] = { *in[0] };
        near_cut(in[0], out[0], &t[1]);
        near_cut(in[0], out[1], &t[2]);
        raw_tri(t, ci);
    } else {
        P3 c0, c1;
        near_cut(in[0], out[0], &c0);
        near_cut(in[1], out[0], &c1);
        P3 a[3] = { *in[0], *in[1], c1 };
        P3 b[3] = { *in[0], c1, c0 };
        raw_tri(a, ci);
        raw_tri(b, ci);
    }
}

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
    *sx = (int16_t)(g_fbw / 2 + (int32_t)(((int64_t)x * PROJ) / zz));
    *sy = (int16_t)(g_fbh / 2 - (int32_t)(((int64_t)y * PROJ) / zz));
}

// World -> view for a CAMERA, which is not the same thing as undoing an object's rotation.
//
// g3d_unrot peels the angles off in the order Z, X, Y, so the pitch comes off around the WORLD's
// X axis. That is the exact inverse of g3d_rot and it is right for objects — but a camera has to
// undo its YAW first, so that the pitch then happens around the axis the camera is actually
// looking along. Get it backwards and the two rotations mix into ROLL: with yaw and pitch both
// non-zero the horizon tips instead of the view looking up and down. (They agree, and the bug
// hides, whenever either angle is zero — which is why it went unnoticed until a first-person
// camera turned its head and then looked up.)
//
// So: undo yaw, then pitch, then roll. Kept as its own function rather than a fix inside
// g3d_unrot, because g3d_unrot's object semantics are correct and several cartridges depend on
// them — the editor's two-axis pick ray among them. This one is only for camera orientation.
void g3d_view(int32_t *px, int32_t *py, int32_t *pz, int ax, int ay, int az) {
    int32_t x = *px, y = *py, z = *pz, t;
    int32_t s = SIN(-ay), c = COS(-ay);                              // 1. yaw: face the camera down +Z
    t = mul15(x, c) + mul15(z, s);  z = mul15(z, c) - mul15(x, s);  x = t;
    s = SIN(-ax); c = COS(-ax);                                      // 2. pitch, about the camera's own X
    t = mul15(y, c) - mul15(z, s);  z = mul15(z, c) + mul15(y, s);  y = t;
    s = SIN(-az); c = COS(-az);                                      // 3. roll last
    t = mul15(x, c) - mul15(y, s);  y = mul15(y, c) + mul15(x, s);  x = t;
    *px = x; *py = y; *pz = z;
}

// g3d_rot turns about Y, then X, then Z. Undoing that means Z, then X, then Y, each
// negated — reverse the order as well as the signs, or two axes at once come out wrong.
void g3d_unrot(int32_t *px, int32_t *py, int32_t *pz, int ax, int ay, int az) {
    int32_t x = *px, y = *py, z = *pz, t;
    int32_t s = SIN(-az), c = COS(-az);
    t = mul15(x, c) - mul15(y, s);  y = mul15(y, c) + mul15(x, s);  x = t;
    s = SIN(-ax); c = COS(-ax);
    t = mul15(y, c) - mul15(z, s);  z = mul15(z, c) + mul15(y, s);  y = t;
    s = SIN(-ay); c = COS(-ay);
    t = mul15(x, c) + mul15(z, s);  z = mul15(z, c) - mul15(x, s);  x = t;
    *px = x; *py = y; *pz = z;
}

// The exact inverse of g3d_view: view space -> world orientation. g3d_view applies yaw⁻, pitch⁻, roll⁻
// (in that order); undoing it means the inverse of each, in REVERSE order — roll, pitch, yaw. With the
// camera position added back afterwards, this turns a screen pixel + its depth into the WORLD point it
// came from, so a draw pass can lay a world-anchored ground texture that stays put as the camera
// moves instead of swimming with the screen.
void g3d_unview(int32_t *px, int32_t *py, int32_t *pz, int ax, int ay, int az) {
    int32_t x = *px, y = *py, z = *pz, t;
    int32_t s = SIN(-az), c = COS(-az);                              // roll⁻¹ (undo the roll g3d_view did last, first)
    t = mul15(x, c) + mul15(y, s);  y = mul15(y, c) - mul15(x, s);  x = t;
    s = SIN(-ax); c = COS(-ax);                                      // pitch⁻¹
    t = mul15(y, c) + mul15(z, s);  z = mul15(z, c) - mul15(y, s);  y = t;
    s = SIN(-ay); c = COS(-ay);                                      // yaw⁻¹
    t = mul15(x, c) - mul15(z, s);  z = mul15(z, c) + mul15(x, s);  x = t;
    *px = x; *py = y; *pz = z;
}

#define MAXV 2048
#define MAXT 4096

// The light (see g3d.h). The zero vector MEANS the headlamp and is decoded at use, so the
// state zero-inits into __bss — a non-zero initializer here would be four bytes of __data
// dragging a 16 KB page into the file (synth.c's rule, same floppy). The headlamp constant
// is -32768 and not -32767 so the default is BIT-identical to the old hardwired shade:
// nz * -32768 * 8 >> 30 == -nz * 8 >> 15 exactly, power-of-two shifts all the way down.
// Every game that never asks for a light renders the same pixels it always did.
static int32_t g_lx, g_ly, g_lz;
void g3d_light(int32_t lx, int32_t ly, int32_t lz) { g_lx = lx; g_ly = ly; g_lz = lz; }
static int light_shade(int32_t nx, int32_t ny, int32_t nz) {
    int64_t d = (g_lx | g_ly | g_lz)
              ? (int64_t)nx * g_lx + (int64_t)ny * g_ly + (int64_t)nz * g_lz
              : (int64_t)nz * -32768;
    int shade = (int)(d * 8 >> 30);
    return shade < 0 ? 0 : shade > 7 ? 7 : shade;
}

void g3d_draw(const Mesh *m, int ax, int ay, int az, int32_t tz) {
    int32_t vx[MAXV], vy[MAXV], vz[MAXV];
    int16_t sx[MAXV], sy[MAXV];

    // ---- vertices: rotate → translate → project
    for (int i = 0; i < m->nv && i < MAXV; i++) {
        int32_t x = m->v[i].x, y = m->v[i].y, z = m->v[i].z;
        g3d_rot(&x, &y, &z, ax, ay, az);
        z += tz;
        vx[i] = x; vy[i] = y; vz[i] = z;
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
    int n = 0;
    static int keep[MAXT];                // the triangles that survived culling
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
        keep[n++] = i;
    }

    // ---- fill, depth-tested and near-clipped. No sort: see g3d_scene.
    zb_clear();
    for (int k = 0; k < n; k++) {
        const Tri *t = &m->t[keep[k]];
        int32_t nx = t->nx, ny = t->ny, nz = t->nz;
        g3d_rot(&nx, &ny, &nz, ax, ay, az);
        int shade = light_shade(nx, ny, nz);
        P3 v[3] = { { vx[t->a], vy[t->a], vz[t->a] },
                    { vx[t->b], vy[t->b], vz[t->b] },
                    { vx[t->c], vy[t->c], vz[t->c] } };
        g3d_tri(v, (uint8_t)(t->ci + shade));
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

// ---- scenes -----------------------------------------------------------------

#define MAXSV 8192
#define MAXST 16384

static int32_t sv_x[MAXSV], sv_y[MAXSV], sv_z[MAXSV];
static int16_t sv_sx[MAXSV], sv_sy[MAXSV];
static struct { uint16_t a, b, c; uint8_t ci; } st[MAXST];

void g3d_scene(const Inst *inst, int ninst, const Cam *cam, int rx, int ry, int rz) {
    int nv = 0, nt = 0;

    for (int i = 0; i < ninst; i++) {
        const Inst *in = &inst[i];
        const Mesh *m = in->m;
        int base = nv;
        if (nv + m->nv > MAXSV) break;

        for (int v = 0; v < m->nv; v++) {
            int32_t x = (int32_t)(((int64_t)m->v[v].x * in->scale) >> 16);
            int32_t y = (int32_t)(((int64_t)m->v[v].y * in->scale) >> 16);
            int32_t z = (int32_t)(((int64_t)m->v[v].z * in->scale) >> 16);
            g3d_rot(&x, &y, &z, in->ax, in->ay, in->az);
            x += in->pos.x; y += in->pos.y; z += in->pos.z;
            g3d_rot(&x, &y, &z, rx, ry, rz);          // the scene's own turn -> world
            x -= cam->pos.x; y -= cam->pos.y; z -= cam->pos.z;
            g3d_view(&x, &y, &z, cam->ax, cam->ay, cam->az);    // world -> view (camera order)
            sv_x[nv] = x; sv_y[nv] = y; sv_z[nv] = z;
            g3d_project(x, y, z, &sv_sx[nv], &sv_sy[nv]);
            nv++;
        }

        for (int t = 0; t < m->nt && nt < MAXST; t++) {
            const Tri *tr = &m->t[t];
            int a = base + tr->a, b = base + tr->b, c = base + tr->c;
            // No near test here any more: g3d_tri cuts what crosses the lens. Dropping it
            // was invisible until the camera moved through the world, and then the world
            // had holes in it.

            int32_t nx = tr->nx, ny = tr->ny, nz = tr->nz;
            g3d_rot(&nx, &ny, &nz, in->ax, in->ay, in->az);
            g3d_rot(&nx, &ny, &nz, rx, ry, rz);
            g3d_view(&nx, &ny, &nz, cam->ax, cam->ay, cam->az);   // normals ride the same transform,
                                                                 // or lighting/culling desync from the geometry
            int32_t cx = (sv_x[a] + sv_x[b] + sv_x[c]) / 3;
            int32_t cy = (sv_y[a] + sv_y[b] + sv_y[c]) / 3;
            int32_t cz = (sv_z[a] + sv_z[b] + sv_z[c]) / 3;
            if ((int64_t)nx * cx + (int64_t)ny * cy + (int64_t)nz * cz >= 0) continue;

            int shade = light_shade(nx, ny, nz);
            st[nt].a = (uint16_t)a; st[nt].b = (uint16_t)b; st[nt].c = (uint16_t)c;
            st[nt].ci = (uint8_t)(tr->ci + shade);
            nt++;
        }
    }

    // No sort. A painter's algorithm sorted per triangle is a guess that usually agrees
    // with a depth test — until two average depths cross mid-rotation, a far face paints
    // over a near one, and it reads as the colours flickering. The depth buffer decides
    // per pixel, so the order these are drawn in doesn't matter at all.
    zb_clear();
    for (int i = 0; i < nt; i++) {
        P3 v[3] = { { sv_x[st[i].a], sv_y[st[i].a], sv_z[st[i].a] },
                    { sv_x[st[i].b], sv_y[st[i].b], sv_z[st[i].b] },
                    { sv_x[st[i].c], sv_y[st[i].c], sv_z[st[i].c] } };
        g3d_tri(v, st[i].ci);
    }
}
