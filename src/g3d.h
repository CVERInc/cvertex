// g3d.h — fixed-point 3D. A software Cx4.
// No floats, no GPU, no textures. Rotation is a table lookup, projection is one divide, flat fills ride the existing rasterizer.
#ifndef G3D_H
#define G3D_H
#include <stdint.h>

typedef struct { int32_t x, y, z; } V3;              // 16.16 fixed point, 1<<16 = 1 unit

// A triangle is 10 bytes: three vertex indices + a palette base colour + a
// model-space normal (Q15). Storing the normal isn't laziness, it's the '90s-correct
// move — a unit normal survives a rotation as a unit normal, so lighting only has to
// rotate it and take z, never a square root.
typedef struct { uint16_t a, b, c; uint8_t ci; int16_t nx, ny, nz; } Tri;

typedef struct { const V3 *v; int nv; const Tri *t; int nt; } Mesh;

extern const Mesh g_cube;
extern const Mesh g_torus;

// ax/ay/az: angle 0..1023 (a direct index into g_sin). tz: camera distance, 16.16.
void g3d_draw(const Mesh *m, int ax, int ay, int az, int32_t tz);

// One mesh, placed. Several of these make a scene.
typedef struct {
    const Mesh *m;
    V3   pos;            // world position, 16.16
    int  ax, ay, az;     // rotation
    int32_t scale;       // 16.16; 1<<16 = the mesh's own size
} Inst;

// 🔴 Draw a whole scene at once, because a painter's algorithm sorted per MESH is not
// sorted. Draw two meshes one after the other and every triangle of the second lands on
// top of the first, whatever the depth says. The sort has to see the entire scene, so
// the scene has to be a list, not a sequence of calls.
void g3d_scene(const Inst *inst, int ninst, int32_t camz);

// The transform, exposed so flat vector shapes ride the SAME pipeline as meshes
// instead of growing a second one. Anything that can produce three coordinates gets
// to be 3D — that is the whole trick behind extrusion.
void g3d_rot(int32_t *x, int32_t *y, int32_t *z, int ax, int ay, int az);
void g3d_project(int32_t x, int32_t y, int32_t z, int16_t *sx, int16_t *sy);

#endif
