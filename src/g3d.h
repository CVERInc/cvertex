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

// The light, a direction TOWARD the source, ~unit Q15, in view space. The default is the
// headlamp — light from the lens, (0,0,-32768) — which is what every game gets if it never
// calls this, and what (0,0,0) restores. A headlamp puts every camera-facing surface on the
// flat top of the cosine, where a wobble moves nothing; a game that wants light to CRAWL
// points this off-axis in init() so the same faces land on the steep part of the curve.
// The platform resets it on every game switch — a cartridge's taste in light is content,
// like its palette, and must not follow you into the next game. Draw-side only: shading
// never feeds the sim, so this cannot touch determinism.
void g3d_light(int32_t lx, int32_t ly, int32_t lz);

// One mesh, placed. Several of these make a scene.
typedef struct {
    const Mesh *m;
    V3   pos;            // world position, 16.16
    int  ax, ay, az;     // rotation
    int32_t scale;       // 16.16; 1<<16 = the mesh's own size
} Inst;

// A camera: somewhere to stand and somewhere to look. Until this existed the world was
// something placed in front of a lens at a fixed distance — you could turn the world, but
// you could not go anywhere. A camera is the difference between a display and a place.
typedef struct {
    V3  pos;             // where it stands, world units
    int ax, ay, az;      // where it looks
} Cam;

// 🔴 Draw a whole scene at once, because a painter's algorithm sorted per MESH is not
// sorted. Draw two meshes one after the other and every triangle of the second lands on
// top of the first, whatever the depth says. The sort has to see the entire scene, so
// the scene has to be a list, not a sequence of calls.
//
// rx/ry/rz rotate the SCENE — every instance's position orbits and its mesh turns with
// it. Without this there is no way to say "the whole thing spins": rotating each Inst
// instead spins 27 objects on the spot, which looks close enough while the angle is small
// and falls apart the moment an instance also has a rotation of its own.
void g3d_scene(const Inst *inst, int ninst, const Cam *cam, int rx, int ry, int rz);

// The inverse of g3d_rot. A camera doesn't move the world, it moves the point of view —
// which is the same arithmetic backwards, and backwards means the axes come off in the
// opposite order too. Getting only the signs right and not the order gives you a rotation
// that's correct on one axis at a time and wrong the moment you use two.
// Camera world->view: undoes yaw, THEN pitch, so the two never mix into roll. Use this for a
// camera's orientation; g3d_unrot stays the exact inverse of g3d_rot, which is what objects want.
void g3d_view(int32_t *x, int32_t *y, int32_t *z, int ax, int ay, int az);
void g3d_unrot(int32_t *x, int32_t *y, int32_t *z, int ax, int ay, int az);
// The exact inverse of g3d_view (view orientation -> world). With the camera position added back, it
// turns a screen pixel + depth into the world point it came from — for world-anchored ground textures.
void g3d_unview(int32_t *x, int32_t *y, int32_t *z, int ax, int ay, int az);

// The transform, exposed so flat vector shapes ride the SAME pipeline as meshes
// instead of growing a second one. Anything that can produce three coordinates gets
// to be 3D — that is the whole trick behind extrusion.
void g3d_rot(int32_t *x, int32_t *y, int32_t *z, int ax, int ay, int az);
void g3d_project(int32_t x, int32_t y, int32_t z, int16_t *sx, int16_t *sy);

// A view-space triangle, ready to be clipped and drawn. Positions, not indices, because
// clipping invents vertices that no index can name.
typedef struct { int32_t x, y, z; } P3;

// Clip one triangle against the near plane and draw what survives. A triangle with a
// vertex behind the lens can't be projected — z goes to zero and through it — so it has
// to be cut, not dropped. Dropping it is invisible right up until the camera moves
// through the world, and then the world has holes in it.
void g3d_tri(const P3 *v, uint8_t ci);

#endif
