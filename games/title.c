// title.c — the title sequence, as a game with no input.
//
// A cube of 27 cubies bursts apart, spells a letter,
// bursts again, and packs itself back into a cube that scrambles.
//
// It exists as a test as much as a title: 27 instances is the smallest scene that can
// prove g3d_scene sorts across meshes rather than within them, and the burst puts near
// cubies in front of far ones at every angle.
//
// Nothing here reads a clock or a random number — the whole sequence is a pure function
// of the frame counter, so it replays identically and it can't desync.
#include "core.h"
#include "g3d.h"
#include "game.h"

#define N 27
#define U (1 << 16)          // one world unit
#define GAP (U * 11 / 10)    // cubie spacing: a little air between them

// The 27 cubies of a 3x3x3, in x-major order.
static void home(int i, V3 *p) {
    int x = i % 3 - 1, y = (i / 3) % 3 - 1, z = i / 9 - 1;
    p->x = x * GAP; p->y = y * GAP; p->z = z * GAP;
}

// The letter, drawn once as grid cells: a C, five wide and five tall, two layers deep.
// 14 cells in front, 13 behind = 27, which is what a Rubik's cube happens to have.
static const int8_t C_CELLS[N][3] = {
    // front layer
    {-1, 2, 0}, {0, 2, 0}, {1, 2, 0}, {2, 2, 0},
    {-2, 1, 0}, {-1, 1, 0},
    {-2, 0, 0}, {-1, 0, 0},
    {-2,-1, 0}, {-1,-1, 0},
    {-1,-2, 0}, {0,-2, 0}, {1,-2, 0}, {2,-2, 0},
    // back layer, one cubie short
    {-1, 2,-1}, {0, 2,-1}, {1, 2,-1},
    {-2, 1,-1}, {-1, 1,-1},
    {-2, 0,-1}, {-1, 0,-1},
    {-2,-1,-1}, {-1,-1,-1},
    {-1,-2,-1}, {0,-2,-1}, {1,-2,-1}, {2,-2,-1},
};

// A deterministic scatter: same cubie, same direction, every run, forever.
static void burst_dir(int i, V3 *p) {
    uint32_t h = (uint32_t)i * 2654435761u;
    int a = (int)(h & 1023), b = (int)((h >> 10) & 1023);
    int32_t sy = g_sin[b], cy = g_sin[(b + 256) & 1023];
    p->x = ((int64_t)g_sin[a] * cy) >> 15;
    p->y = ((int64_t)g_sin[(a + 256) & 1023] * cy) >> 15;
    p->z = sy;
}

static int32_t lerp(int32_t a, int32_t b, int t) { return a + (int32_t)(((int64_t)(b - a) * t) >> 10); }

// Ease in and out, 0..1024 in, 0..1024 out. Smoothstep in fixed point.
static int ease(int t) {
    if (t < 0) t = 0; else if (t > 1024) t = 1024;
    int32_t x = t;
    return (int)(((int64_t)x * x * (3 * 1024 - 2 * x)) >> 21);
}

// Phase lengths, in frames.
#define T_CUBE   90
#define T_OUT1   45
#define T_C     120
#define T_OUT2   45
#define T_IN     50
#define T_SCRAM 200
#define T_TOTAL (T_CUBE + T_OUT1 + T_C + T_OUT2 + T_IN + T_SCRAM)

static void title_draw(uint32_t frame, int32_t camz) {
    static Inst inst[N];
    int t = (int)(frame % T_TOTAL);
    int spin = (int)((frame * 2) & 1023);

    for (int i = 0; i < N; i++) {
        V3 h, c, d;
        home(i, &h);
        c.x = C_CELLS[i][0] * GAP; c.y = C_CELLS[i][1] * GAP; c.z = C_CELLS[i][2] * GAP;
        burst_dir(i, &d);

        V3 p = h;
        int ax = 0, ay = spin, az = 0;
        int32_t scale = U;
        int u = t;

        if (u < T_CUBE) {
            // packed and turning
        } else if ((u -= T_CUBE) < T_OUT1) {
            int e = ease(u * 1024 / T_OUT1);
            p.x = lerp(h.x, h.x + d.x * 5, e);
            p.y = lerp(h.y, h.y + d.y * 5, e);
            p.z = lerp(h.z, h.z + d.z * 5, e);
            ax = (int)(u * 9); az = (int)(u * 5);
        } else if ((u -= T_OUT1) < T_C) {
            // fly in from the burst and settle into the letter
            int e = ease(u * 1024 / (T_C / 2));
            p.x = lerp(h.x + d.x * 5, c.x, e);
            p.y = lerp(h.y + d.y * 5, c.y, e);
            p.z = lerp(h.z + d.z * 5, c.z, e);
            ax = (int)((T_OUT1 * 9) * (1024 - e) >> 10);
            az = (int)((T_OUT1 * 5) * (1024 - e) >> 10);
            ay = (int)((int64_t)spin * (1024 - e) >> 10);   // the letter faces front
        } else if ((u -= T_C) < T_OUT2) {
            int e = ease(u * 1024 / T_OUT2);
            p.x = lerp(c.x, c.x + d.x * 6, e);
            p.y = lerp(c.y, c.y + d.y * 6, e);
            p.z = lerp(c.z, c.z + d.z * 6, e);
            ax = (int)(u * 11); az = (int)(u * 7);
        } else if ((u -= T_OUT2) < T_IN) {
            int e = ease(u * 1024 / T_IN);
            p.x = lerp(c.x + d.x * 6, h.x, e);
            p.y = lerp(c.y + d.y * 6, h.y, e);
            p.z = lerp(c.z + d.z * 6, h.z, e);
            ax = (int)((T_OUT2 * 11) * (1024 - e) >> 10);
            az = (int)((T_OUT2 * 7) * (1024 - e) >> 10);
            ay = (int)((int64_t)spin * e >> 10);
        } else {
            // packed again, and a layer keeps turning: the cube scrambling itself
            u -= T_IN;
            int turn = u / 40, phase = (u % 40) * 1024 / 40;
            int e = ease(phase);
            int axis = turn % 3, slice = (turn / 3) % 3 - 1;
            int in_slice = (axis == 0 && i % 3 - 1 == slice)
                        || (axis == 1 && (i / 3) % 3 - 1 == slice)
                        || (axis == 2 && i / 9 - 1 == slice);
            if (in_slice) {
                int a = 256 * e >> 10;                     // a quarter turn
                int32_t s = g_sin[a & 1023], co = g_sin[(a + 256) & 1023];
                int32_t px = h.x, py = h.y, pz = h.z, tmp;
                if (axis == 0) { tmp = ((int64_t)py * co - (int64_t)pz * s) >> 15;
                                 pz  = ((int64_t)pz * co + (int64_t)py * s) >> 15; py = tmp; ax = a; }
                else if (axis == 1) { tmp = ((int64_t)px * co + (int64_t)pz * s) >> 15;
                                 pz  = ((int64_t)pz * co - (int64_t)px * s) >> 15; px = tmp; }
                else { tmp = ((int64_t)px * co - (int64_t)py * s) >> 15;
                       py  = ((int64_t)py * co + (int64_t)px * s) >> 15; px = tmp; az = a; }
                p.x = px; p.y = py; p.z = pz;
                if (axis == 1) ay = spin + a; else ay = spin;
            }
        }

        inst[i].m = &g_cube;
        inst[i].pos = p;
        inst[i].ax = ax; inst[i].ay = ay; inst[i].az = az;
        inst[i].scale = scale;
    }

    g3d_scene(inst, N, camz);
}


// ---- the game -------------------------------------------------------------

static uint32_t g_frame;

static void init(void) { tables_init(); g_frame = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;
    for (int i = 0; i < 8; i++) {
        int r = 30 + i * 26, g = 90 + i * 22, b = 120 + i * 18;
        g_pal[8 + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
static void tick(const Input in[2]) { (void)in; g_frame++; }
static void audio(void) {}
static void draw(void) { fb_clear(0); title_draw(g_frame, 12 << 16); }
static uint64_t checksum(void) { return g_frame; }

const Game game_title = { "title", init, tick, audio, draw, checksum };
