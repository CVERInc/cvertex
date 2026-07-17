// shape.h — a flat vector graphic: a stack of fills, drawn back to front.
// Data comes from tools/svg2poly, baked into the binary (same reasoning as obj2mesh: no loader).
#ifndef SHAPE_H
#define SHAPE_H
#include <stdint.h>

// One fill. It may have more than one contour — an outline plus its holes — the
// even-odd rule makes the holes work on their own.
// Offsets, not pointers: a pointer is 8 bytes, an offset is 2, and offsets need no relocation.
typedef struct {
    uint16_t pt_off;    // point index into pts[] (not a byte offset)
    uint16_t len_off;   // index into lens[]
    uint8_t  nc;        // number of contours
    uint8_t  ci;        // palette index
} Fill;

typedef struct {
    const int16_t  *pts;    // x,y interleaved. Coordinates are normalized: longest edge = 32768 (±16384, centered on the origin)
    const uint16_t *lens;   // point count per contour
    const Fill     *f;      // draw order, back to front (= SVG document order)
    uint16_t        nf;
    const uint32_t *pal;    // this shape's colours, 0xAARRGGBB
    uint8_t         npal, pal_base;
} Shape;

void shape_install_palette(const Shape *s);            // load pal into g_pal[pal_base..]
void shape_draw(const Shape *s, int cx, int cy, int h); // h = desired height (pixels)

// Extrude the shape along Z and put it in the 3D world: the silhouette grows walls,
// the artwork stays flat on the front face. No triangulation anywhere — the points go
// through g3d's transform and land in poly_fill, which only ever wanted screen
// coordinates and never cared where they came from. Concave outlines, holes and all
// 26 fills come along for free.
//
// The silhouette is f[0]: a tracer emits the backmost fill first, and for line art
// that fill IS the outline. Art with a drawn outline hands us its own silhouette.
//
// wx/wy/wz = world position (16.16); wz is distance from the camera.
// size = height in world units (16.16), depth = thickness (16.16).
// flip = mirror horizontally (a character facing the other way costs no art).
void shape_draw3d(const Shape *s, int32_t wx, int32_t wy, int32_t wz,
                  int ax, int ay, int az, int32_t size, int32_t depth, int flip);

// Screen pixels → world units at a given camera distance. The sim thinks in screen
// space (gameplay is 2D); this is the one place that opinion meets the 3D camera.
int32_t world_per_px(int32_t wz);

#endif
