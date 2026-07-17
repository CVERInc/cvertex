// title.c — cubeconjure's start-screen show, running in cvertex.
//
// The choreography isn't invented here. It's read out of cubeconjure's Cube3DView:
//
//   C wobbling gently (5s) -> explode into the cube -> the cube self-spins while dancing
//   a fixed sequence -> explode back to the C -> repeat
//
// and the dance is K moves out and the same K undone, so it always lands on exactly the
// start cube, so the explosion always yields exactly the same C.
//
// None of the hard parts are here. cubeconjure's colorPlan() solves an assignment — which
// cubie goes to which slot in the C, and how it's turned so the right sticker faces out —
// and its solver walks a real cube through the dance. Both are constants, so both are in
// cplan.h. There is no cube model in this engine, no quaternions, and no solver: the slots
// are a table and the colours are a lookup.
//
// It's a test as much as a title. 27 instances is the smallest scene that can prove
// g3d_scene sorts across meshes rather than within them, and it answers a bigger question:
// whether something built in SceneKit fits in here at all.
//
// A pure function of the frame counter. No clock, no random.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "cplan.h"

#define N 27
#define U (1 << 16)
#define GAP  (U * 104 / 100)     // 1.04 centre-to-centre — cubeconjure's own spacing
#define HALF (U * 95 / 200)      // 0.95 wide in that gap, so the seams read

// cubeconjure's palette, read out of its source rather than remembered.
#define PAL_STICKER 8            // 6 colours x 8 shades: 8..55
#define PAL_PLASTIC 56           // + 8: 56..63
static const uint32_t STICKER_RGB[6] = {
    0xFFF5F5F8,   // U
    0xFFF23A35,   // R
    0xFF33C759,   // F
    0xFFFFD62E,   // D
    0xFFFF9519,   // L
    0xFF338CFA,   // B
};
#define PLASTIC_RGB 0xFF26262C

// cubeconjure's face order is its SCNBox material order: 0 +Z, 1 +X, 2 -Z, 3 -X, 4 +Y,
// 5 -Y. Ours is 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z — and our Z points the other way, so
// its +Z sticker lands on our -Z face.
static const uint8_t MY_FACE[6] = { 5, 0, 4, 1, 2, 3 };

static const int8_t FN[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                 {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// The 27 cubies, built at runtime: each needs its OWN face colours, and 27 const meshes
// would be 27 copies of the same eight vertices.
static V3   cub_v[N][8];
static Tri  cub_t[N][12];
static Mesh cub_m[N];

static void build_cubies(void) {
    static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
    for (int i = 0; i < N; i++) {
        for (int f = 0; f < 6; f++)
            for (int k = 0; k < 2; k++) {
                Tri *t = &cub_t[i][f * 2 + k];
                t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k];
            }
        cub_m[i].v = cub_v[i]; cub_m[i].nv = 8;
        cub_m[i].t = cub_t[i]; cub_m[i].nt = 12;
    }
}

// Repaint from a baked state. This is the whole of "the cube has colours".
static void paint(int state) {
    for (int i = 0; i < N; i++)
        for (int f = 0; f < 6; f++) {
            uint8_t c = CUBE_STATES[state][i][f];
            uint8_t ci = c ? (uint8_t)(PAL_STICKER + (c - 1) * 8) : PAL_PLASTIC;
            cub_t[i][MY_FACE[f] * 2].ci = ci;
            cub_t[i][MY_FACE[f] * 2 + 1].ci = ci;
        }
}

// cubeconjure's numbering: ((x+1)*3 + (y+1))*3 + (z+1). Our Z is flipped, so z negates.
static void home(int i, V3 *p) {
    int x = i / 9 - 1, y = (i / 3) % 3 - 1, z = i % 3 - 1;
    p->x = x * GAP; p->y = y * GAP; p->z = -z * GAP;
}

static const CSlot *slot_of(int cubie) {
    for (int i = 0; i < N; i++) if (C_PLAN[i].cubie == cubie) return &C_PLAN[i];
    return &C_PLAN[0];
}

// A slot's orientation is a fixed cube symmetry (Q15, det +1), so it's applied to the
// mesh's own vertices and normals rather than asking g3d for a second rotation stage it
// doesn't have. Exact: the entries are only ever 0 or +/-1.
static void applym(const int16_t *m, int32_t *x, int32_t *y, int32_t *z) {
    int32_t X = *x, Y = *y, Z = *z;
    *x = (int32_t)(((int64_t)m[0]*X + (int64_t)m[1]*Y + (int64_t)m[2]*Z) >> 15);
    *y = (int32_t)(((int64_t)m[3]*X + (int64_t)m[4]*Y + (int64_t)m[5]*Z) >> 15);
    *z = (int32_t)(((int64_t)m[6]*X + (int64_t)m[7]*Y + (int64_t)m[8]*Z) >> 15);
}

static void orient(int i, const int16_t *m) {
    for (int v = 0; v < 8; v++) {
        int32_t x = VP[v][0] * HALF, y = VP[v][1] * HALF, z = VP[v][2] * HALF;
        if (m) applym(m, &x, &y, &z);
        cub_v[i][v].x = x; cub_v[i][v].y = y; cub_v[i][v].z = z;
    }
    for (int f = 0; f < 6; f++) {
        int32_t nx = FN[f][0] * 32767, ny = FN[f][1] * 32767, nz = FN[f][2] * 32767;
        if (m) applym(m, &nx, &ny, &nz);
        for (int k = 0; k < 2; k++) {
            cub_t[i][f*2+k].nx = (int16_t)nx;
            cub_t[i][f*2+k].ny = (int16_t)ny;
            cub_t[i][f*2+k].nz = (int16_t)nz;
        }
    }
}

static int32_t lerp(int32_t a, int32_t b, int t) { return a + (int32_t)(((int64_t)(b - a) * t) >> 10); }
static int ease(int t) {                       // smoothstep, 0..1024
    if (t < 0) t = 0; else if (t > 1024) t = 1024;
    return (int)(((int64_t)t * t * (3 * 1024 - 2 * t)) >> 21);
}

// Frames at 60Hz. The durations are cubeconjure's own.
#define T_ROCK   300     // the C wobbles ~5s
#define T_OUT     78     // explode into the cube (1.3s)
#define T_MOVE    30     // one dance move (0.5s)
#define T_DANCE  (T_MOVE * 12)
#define T_IN      78     // explode back to the C (1.3s)
#define T_TOTAL  (T_ROCK + T_OUT + T_DANCE + T_IN)

static uint32_t g_frame;

static void title_draw(uint32_t frame, int32_t camz) {
    static Inst inst[N];
    int t = (int)(frame % T_TOTAL);

    int state = 0;
    if (t >= T_ROCK + T_OUT && t < T_ROCK + T_OUT + T_DANCE)
        state = (t - T_ROCK - T_OUT) / T_MOVE;
    paint(state);

    // The C's gentle 3-axis float, straight out of startCRock(): small angles at
    // different rates so it never pauses or repeats stiffly, faded in over 0.6s so it
    // picks up seamlessly from the just-formed upright C. Radians there, table units
    // here: 1 rad = 1024/2pi = 163.
    int rx = 0, ry = 0, rz = 0, spin = 0;
    if (t >= T_ROCK) { int u = T_ROCK - 1;
        rx = (int)(((int64_t)26 * g_sin[(u * 1684 / 1000) & 1023] * 1024) >> 25);
        ry = (int)(((int64_t)33 * g_sin[((u * 1222 / 1000) + 212) & 1023] * 1024) >> 25);
        rz = (int)(((int64_t)20 * g_sin[((u * 2173 / 1000) + 342) & 1023] * 1024) >> 25); }
    if (t < T_ROCK) {
        int rampv = t < 36 ? t * 1024 / 36 : 1024;
        rx = (int)(((int64_t)26 * g_sin[(t * 1684 / 1000) & 1023] * rampv) >> 25);
        ry = (int)(((int64_t)33 * g_sin[((t * 1222 / 1000) + 212) & 1023] * rampv) >> 25);
        rz = (int)(((int64_t)20 * g_sin[((t * 2173 / 1000) + 342) & 1023] * rampv) >> 25);
    } else if (t >= T_ROCK + T_OUT && t < T_ROCK + T_OUT + T_DANCE) {
        spin = t - T_ROCK - T_OUT;
    }

    for (int i = 0; i < N; i++) {
        const CSlot *s = slot_of(i);
        V3 h; home(i, &h);
        V3 cp = { s->x * (U / 1024), s->y * (U / 1024), s->z * (U / 1024) };
        V3 p; int ax = 0, ay = 0, az = 0;
        const int16_t *mat = 0;

        if (t < T_ROCK) {                                   // the C, wobbling
            p = cp; mat = s->m;
        } else if (t < T_ROCK + T_OUT) {                    // C -> cube
            int e = ease((t - T_ROCK) * 1024 / T_OUT);
            p.x = lerp(cp.x, h.x, e); p.y = lerp(cp.y, h.y, e); p.z = lerp(cp.z, h.z, e);
            mat = (e < 512) ? s->m : 0;                     // the symmetry snaps at halfway
        } else if (t < T_ROCK + T_OUT + T_DANCE) {          // the cube, dancing
            p = h;
            int mi = (t - T_ROCK - T_OUT) / T_MOVE;
            int ph = ((t - T_ROCK - T_OUT) % T_MOVE) * 1024 / T_MOVE;
            int in_layer = 0;
            for (int k = 0; k < 9; k++) if (DANCE_LAYER[mi][k] == i) in_layer = 1;
            if (in_layer) {
                int a = DANCE_QT[mi] * 256 * ease(ph) / 1024;
                int axi = DANCE_AXIS[mi];
                if (axi == 2) a = -a;                       // our Z points the other way
                int32_t sn = g_sin[a & 1023], co = g_sin[(a + 256) & 1023];
                int32_t px = h.x, py = h.y, pz = h.z, tmp;
                if (axi == 0)      { tmp = ((int64_t)py*co - (int64_t)pz*sn) >> 15;
                                     pz  = ((int64_t)pz*co + (int64_t)py*sn) >> 15; py = tmp; ax = a; }
                else if (axi == 1) { tmp = ((int64_t)px*co + (int64_t)pz*sn) >> 15;
                                     pz  = ((int64_t)pz*co - (int64_t)px*sn) >> 15; px = tmp; ay = a; }
                else               { tmp = ((int64_t)px*co - (int64_t)py*sn) >> 15;
                                     py  = ((int64_t)py*co + (int64_t)px*sn) >> 15; px = tmp; az = a; }
                p.x = px; p.y = py; p.z = pz;
            }
        } else {                                            // cube -> C
            int e = ease((t - T_ROCK - T_OUT - T_DANCE) * 1024 / T_IN);
            p.x = lerp(h.x, cp.x, e); p.y = lerp(h.y, cp.y, e); p.z = lerp(h.z, cp.z, e);
            mat = (e < 512) ? 0 : s->m;
        }

        orient(i, mat);
        inst[i].m = &cub_m[i];
        inst[i].pos = p;
        inst[i].ax = ax; inst[i].ay = ay; inst[i].az = az;
        inst[i].scale = U;
    }

    // The wobble and the self-spin are the SCENE's, not each cubie's — cubeconjure turns
    // its root node and the children follow. Rotating 27 cubies instead only looks right
    // while the angle is small.
    int sx = rx, sy = ry, sz = rz;
    if (t >= T_ROCK && t < T_ROCK + T_OUT) {                 // ease the wobble out
        int e = ease((t - T_ROCK) * 1024 / T_OUT);
        sx = (int)((int64_t)rx * (1024 - e) >> 10);
        sy = (int)((int64_t)ry * (1024 - e) >> 10);
        sz = (int)((int64_t)rz * (1024 - e) >> 10);
    } else if (t >= T_ROCK + T_OUT && t < T_ROCK + T_OUT + T_DANCE) {
        sx = 0; sy = spin; sz = 0;
    } else if (t >= T_ROCK + T_OUT + T_DANCE) {              // the explosion eases it upright
        int e = ease((t - T_ROCK - T_OUT - T_DANCE) * 1024 / T_IN);
        sx = 0; sy = (int)((int64_t)spin * (1024 - e) >> 10); sz = 0;
    }
    g3d_scene(inst, N, camz, sx, sy, sz);
}

// ---- the game ---------------------------------------------------------------

static void init(void) {
    tables_init();
    g_frame = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;
    for (int c = 0; c < 6; c++) ramp(PAL_STICKER + c * 8, STICKER_RGB[c]);
    ramp(PAL_PLASTIC, PLASTIC_RGB);
    build_cubies();
}
static void tick(const Input in[2]) { (void)in; g_frame++; }
static void audio(void) {}
static void draw(void) { fb_clear(0); title_draw(g_frame, 13 << 16); }
static uint64_t checksum(void) { return g_frame; }

const Game game_title = { "title", init, tick, audio, draw, checksum };
