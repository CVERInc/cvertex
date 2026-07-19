// marble.c — isometric marble roll. A faceted ball with real momentum, crossing a
// course of platforms and ramps that float over nothing.
//
// The camera never turns. It sits at a fixed pitch and a fixed 45-degree yaw and only
// ever TRANSLATES, chasing the ball — that fixity is the whole trick that makes a
// perspective renderer read as an oblique isometric one: the geometry keeps the same
// silhouette everywhere on screen, so ramps and edges read as consistent angles instead
// of a lens hunting around. rx/ry/rz into g3d_scene stay zero for the same reason: this
// is a camera game, not a turntable.
//
// Momentum lives entirely in tick(). The marble doesn't have a "roll" verb — it has
// velocity, friction shaves it every frame, and a ramp's own slope keeps pushing it
// downhill whether or not a key is held. Overshoot is the feature: let go mid-corner and
// it keeps going, exactly the way a real marble would embarrass you.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"
#include <math.h>

#define U (1 << 16)
// Course numbers read as hundredths of a world unit — TU(140) is "1.40 units" — so a
// tile's footprint and a ramp's rise are both small integers instead of raw 16.16 noise.
#define TU(v) ((int32_t)((int64_t)(v) * U / 100))

// ---- a face normal that can't be told a lie -------------------------------------
// Every custom mesh below (the ramps, the ball) is built at init() from raw vertices,
// not hand-typed like a cube's six axis-aligned faces — so there is no table of "this
// face points +X" to copy from. Compute the normal instead, and settle which way is
// "out" by checking it against the shape's own centre: a convex solid's face centroid,
// seen from its centre, is always on the outside. Flip if it isn't. This is the one
// piece of arithmetic every mesh below depends on, so getting it wrong here would have
// made every ramp and the ball itself look hollow.
static void set_normal(Tri *t, const V3 *v, int32_t ccx, int32_t ccy, int32_t ccz) {
    float ax = (float)(v[t->b].x - v[t->a].x), ay = (float)(v[t->b].y - v[t->a].y), az = (float)(v[t->b].z - v[t->a].z);
    float bx = (float)(v[t->c].x - v[t->a].x), by = (float)(v[t->c].y - v[t->a].y), bz = (float)(v[t->c].z - v[t->a].z);
    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    float mcx = (float)((int64_t)v[t->a].x + v[t->b].x + v[t->c].x) / 3.f - (float)ccx;
    float mcy = (float)((int64_t)v[t->a].y + v[t->b].y + v[t->c].y) / 3.f - (float)ccy;
    float mcz = (float)((int64_t)v[t->a].z + v[t->b].z + v[t->c].z) / 3.f - (float)ccz;
    if (nx * mcx + ny * mcy + nz * mcz < 0.f) { nx = -nx; ny = -ny; nz = -nz; }
    float len = sqrtf(nx * nx + ny * ny + nz * nz); if (len < 1.f) len = 1.f;
    t->nx = (int16_t)(nx / len * 32767.f);
    t->ny = (int16_t)(ny / len * 32767.f);
    t->nz = (int16_t)(nz / len * 32767.f);
}

// ---- the course -------------------------------------------------------------------
// One straight run in X, a turn, one straight run in Z: the shape a 45-degree camera
// wants, because it puts both world axes on screen as the two diagonals at once. axis 0
// is flat, 1 ramps along X (y0 at the low-x edge, y1 at the high-x edge), 2 ramps along
// Z the same way. Every tile's edge value matches its neighbour's, by hand — that's what
// makes a ramp read as ground instead of a floating wedge next to a floor.
typedef struct { int32_t cx, cz, hx, hz, y0, y1; uint8_t axis, goal; } TileDef;
#define NTILES 9
static const TileDef TILES[NTILES] = {
    { TU(   0), TU(   0), TU(140), TU(140), TU(  0), TU(  0), 0, 0 },   // start pad
    { TU( 260), TU(   0), TU(120), TU(110), TU(  0), TU(110), 1, 0 },   // ramp up +x
    { TU( 520), TU(   0), TU(140), TU(110), TU(110), TU(110), 0, 0 },   // flat, elevated
    { TU( 770), TU(   0), TU(110), TU( 55), TU(110), TU(110), 0, 0 },   // narrow bridge
    { TU( 830), TU( 130), TU( 90), TU(130), TU(110), TU(220), 2, 0 },   // ramp up +z, turning
    { TU( 830), TU( 400), TU(140), TU(140), TU(220), TU(220), 0, 0 },   // turn plaza
    { TU( 890), TU( 700), TU( 55), TU(160), TU(220), TU(220), 0, 0 },   // narrow bridge
    { TU( 890), TU(1010), TU(130), TU(150), TU(220), TU(320), 2, 0 },   // ramp up +z, final
    { TU( 890), TU(1340), TU(220), TU(180), TU(320), TU(320), 0, 1 },   // GOAL pad
};
#define NARROW_MAX TU(70)   // a tile thinner than this on its cross axis reads as a warning

static int32_t surf_y(const TileDef *d, int32_t x, int32_t z) {
    if (d->axis == 1) {
        int32_t span = 2 * d->hx; if (span <= 0) span = 1;
        return d->y0 + (int32_t)(((int64_t)(x - (d->cx - d->hx)) * (d->y1 - d->y0)) / span);
    }
    if (d->axis == 2) {
        int32_t span = 2 * d->hz; if (span <= 0) span = 1;
        return d->y0 + (int32_t)(((int64_t)(z - (d->cz - d->hz)) * (d->y1 - d->y0)) / span);
    }
    return d->y0;
}
static int find_tile(int32_t x, int32_t z) {
    for (int i = 0; i < NTILES; i++) {
        const TileDef *d = &TILES[i];
        if (x >= d->cx - d->hx && x <= d->cx + d->hx && z >= d->cz - d->hz && z <= d->cz + d->hz) return i;
    }
    return -1;
}

// ---- palette ------------------------------------------------------------------------
#define NORMAL_BASE 8
#define WARN_BASE   16
#define GOAL_BASE   24
#define BALL_BASE   32

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255, g = (int)((rgb >> 8) & 255) * m / 255, b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
static uint8_t tile_ci(int i) {
    const TileDef *d = &TILES[i];
    if (d->goal) return GOAL_BASE;
    if (d->hx < NARROW_MAX || d->hz < NARROW_MAX) return WARN_BASE;
    return NORMAL_BASE;
}

// ---- tile meshes, built once at init ------------------------------------------------
// A tile is eight vertices, same topology as a box, except the top four aren't all the
// same height — that's the entire difference between a platform and a ramp. Sides and
// the bottom stay exactly axis-aligned planes regardless (a face at fixed x or fixed z
// is planar no matter what the top does), so only the top quad's normal is ever not a
// straight up. World coordinates are baked straight into the vertices — every Inst
// below sits at pos (0,0,0) — because a sloped quad has no single centre to offset from.
static V3  tv[NTILES][8];
static Tri tt[NTILES][12];
static Mesh tm[NTILES];

static void build_tile(int i) {
    const TileDef *d = &TILES[i];
    int32_t lo = d->y0 < d->y1 ? d->y0 : d->y1;
    int32_t yb = lo - TU(30);                    // flat underside, well below the low edge
    int32_t yA = surf_y(d, d->cx - d->hx, d->cz - d->hz);   // corner x-,z-
    int32_t yB = surf_y(d, d->cx + d->hx, d->cz - d->hz);   // corner x+,z-
    int32_t yC = surf_y(d, d->cx + d->hx, d->cz + d->hz);   // corner x+,z+
    int32_t yD = surf_y(d, d->cx - d->hx, d->cz + d->hz);   // corner x-,z+

    V3 *v = tv[i];
    v[0] = (V3){ d->cx - d->hx, yb, d->cz - d->hz };
    v[1] = (V3){ d->cx + d->hx, yb, d->cz - d->hz };
    v[2] = (V3){ d->cx + d->hx, yB, d->cz - d->hz };
    v[3] = (V3){ d->cx - d->hx, yA, d->cz - d->hz };
    v[4] = (V3){ d->cx - d->hx, yb, d->cz + d->hz };
    v[5] = (V3){ d->cx + d->hx, yb, d->cz + d->hz };
    v[6] = (V3){ d->cx + d->hx, yC, d->cz + d->hz };
    v[7] = (V3){ d->cx - d->hx, yD, d->cz + d->hz };

    uint8_t ci = tile_ci(i);
    Tri *t = tt[i];
    int n = 0;
    #define QUAD(a,b,c,d4) do { t[n]=(Tri){(a),(b),(c),ci,0,0,0}; n++; t[n]=(Tri){(a),(c),(d4),ci,0,0,0}; n++; } while (0)
    QUAD(1,5,6,2);   // +x side
    QUAD(4,0,3,7);   // -x side
    QUAD(3,2,6,7);   // top (the sloped one, on a ramp)
    QUAD(4,5,1,0);   // bottom
    QUAD(5,4,7,6);   // +z side
    QUAD(0,1,2,3);   // -z side
    #undef QUAD

    int32_t ccx = d->cx, ccz = d->cz;
    int32_t ccy = (yb + (yA + yB + yC + yD) / 4) / 2;
    for (int k = 0; k < n; k++) set_normal(&t[k], v, ccx, ccy, ccz);
    tm[i].v = v; tm[i].nv = 8; tm[i].t = t; tm[i].nt = n;
}

// ---- the ball: a faceted UV sphere, built once, radius U so Inst.scale IS its radius.
#define SLICES 10
#define RINGS   5
#define BALLV (2 + RINGS * SLICES)
#define BALLT (SLICES * 2 + (RINGS - 1) * SLICES * 2)
static V3  ballv[BALLV];
static Tri ballt[BALLT];
static Mesh ballm;

static void build_ball(void) {
    const float PI = 3.14159265358979323846f;
    const int NP = 0, SP = 1;
    ballv[NP] = (V3){ 0,  U, 0 };
    ballv[SP] = (V3){ 0, -U, 0 };
    for (int r = 0; r < RINGS; r++) {
        float phi = PI * (float)(r + 1) / (float)(RINGS + 1);
        float y = cosf(phi), rad = sinf(phi);
        for (int s = 0; s < SLICES; s++) {
            float th = 2.f * PI * (float)s / (float)SLICES;
            ballv[2 + r * SLICES + s] = (V3){ (int32_t)(rad * cosf(th) * U), (int32_t)(y * U), (int32_t)(rad * sinf(th) * U) };
        }
    }
    #define RING(r,s) (uint16_t)(2 + (r) * SLICES + ((s) % SLICES))
    int n = 0;
    for (int s = 0; s < SLICES; s++) { ballt[n] = (Tri){ (uint16_t)NP, RING(0,s), RING(0,s+1), BALL_BASE, 0,0,0 }; n++; }
    for (int r = 0; r < RINGS - 1; r++)
        for (int s = 0; s < SLICES; s++) {
            ballt[n] = (Tri){ RING(r,s),   RING(r,s+1), RING(r+1,s+1), BALL_BASE, 0,0,0 }; n++;
            ballt[n] = (Tri){ RING(r,s),   RING(r+1,s+1), RING(r+1,s), BALL_BASE, 0,0,0 }; n++;
        }
    for (int s = 0; s < SLICES; s++) { ballt[n] = (Tri){ (uint16_t)SP, RING(RINGS-1,s+1), RING(RINGS-1,s), BALL_BASE, 0,0,0 }; n++; }
    #undef RING
    for (int k = 0; k < n; k++) set_normal(&ballt[k], ballv, 0, 0, 0);
    ballm.v = ballv; ballm.nv = BALLV; ballm.t = ballt; ballm.nt = n;
}

// ---- simulation state ---------------------------------------------------------------
typedef struct { int32_t x, y, z, vx, vy, vz; int spinx, spinz; uint8_t grounded; } Ball;
static Ball g_ball;
static int g_lives;
static int32_t g_timer;
static uint8_t g_over, g_win, g_prev_jump;
static uint32_t g_frame;
static uint8_t g_events;
#define EV_FALL     1
#define EV_TIMEOUT  2
#define EV_OVER     4
#define EV_WIN      8
#define EV_RESTART 16

#define R           TU(35)
#define ACC         (U * 9  / 1000)
#define MAXSPD      (U * 45 / 1000)
#define RAMP_ACC    (U * 4  / 1000)   // must stay well under ACC or a ramp becomes a wall
#define GRAVITY     (U * 55 / 1000)
#define VOID_Y      (-TU(900))
#define TIME_LIMIT  (60 * 40)     // one life's countdown, ~40s at 60Hz
#define LIVES_START 3
#define SPIN_K      457           // angle units per (16.16 unit of roll), ~2*pi*R physically

static void respawn(void) {
    g_ball.x = TILES[0].cx; g_ball.z = TILES[0].cz;
    g_ball.y = TILES[0].y0 + R;
    g_ball.vx = g_ball.vy = g_ball.vz = 0;
    g_ball.grounded = 1;
    g_timer = TIME_LIMIT;
}
static void reset_game(void) {
    g_lives = LIVES_START; g_over = 0; g_win = 0;
    g_ball.spinx = g_ball.spinz = 0;
    respawn();
}

static void init(void) {
    tables_init();
    build_ball();
    for (int i = 0; i < NTILES; i++) build_tile(i);

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF07070F;             // the void
    g_pal[1] = 0xFFF5F5F8;             // hud text
    g_pal[2] = 0xFFF0C24A;             // gold: win / good news
    g_pal[3] = 0xFFD9463A;             // red: lives lost / game over
    ramp(NORMAL_BASE, 0xFF4A9F6E);     // course, mossy green
    ramp(WARN_BASE,   0xFFB8543A);     // narrow tiles, rust — read this as "careful"
    ramp(GOAL_BASE,   0xFFE8C24A);     // the goal pad, gold
    ramp(BALL_BASE,   0xFFE0483C);     // the marble itself, warm red

    reset_game();
    g_frame = 0;
}

static void tick(const Input in[2]) {
    g_events = 0;
    g_frame++;
    Input p = in[0];                   // single player: one marble, one set of hands
    uint8_t jpress = p.jump && !g_prev_jump;
    g_prev_jump = p.jump;

    if (g_over || g_win) {
        if (jpress) { reset_game(); g_events |= EV_RESTART; }
        return;
    }

    // ---- horizontal: input + a ramp's own downhill pull + friction, in that order ----
    int t0 = find_tile(g_ball.x, g_ball.z);
    if (t0 >= 0) {
        const TileDef *d = &TILES[t0];
        if (d->axis == 1) g_ball.vx += (d->y1 > d->y0 ? -1 : 1) * RAMP_ACC;
        else if (d->axis == 2) g_ball.vz += (d->y1 > d->y0 ? -1 : 1) * RAMP_ACC;
    }
    g_ball.vx += (int32_t)p.x * ACC;
    g_ball.vz += (int32_t)p.y * ACC;
    g_ball.vx -= g_ball.vx >> 6;        // rolling friction — never quite stops on its own
    g_ball.vz -= g_ball.vz >> 6;
    if (g_ball.vx >  MAXSPD) g_ball.vx =  MAXSPD; else if (g_ball.vx < -MAXSPD) g_ball.vx = -MAXSPD;
    if (g_ball.vz >  MAXSPD) g_ball.vz =  MAXSPD; else if (g_ball.vz < -MAXSPD) g_ball.vz = -MAXSPD;
    g_ball.x += g_ball.vx;
    g_ball.z += g_ball.vz;

    // A rolling ball's spin is its own displacement divided by its own radius — free,
    // convincing, and it's what tells the eye "momentum" instead of "sliding puck."
    g_ball.spinx = (g_ball.spinx + ((g_ball.vz * SPIN_K) >> 16)) & 1023;
    g_ball.spinz = (g_ball.spinz - ((g_ball.vx * SPIN_K) >> 16)) & 1023;

    // ---- vertical: on a tile, ride its surface; off every tile, you're in the void ----
    uint8_t lost = 0;
    int t1 = find_tile(g_ball.x, g_ball.z);
    if (t1 >= 0) {
        const TileDef *d = &TILES[t1];
        g_ball.y = surf_y(d, g_ball.x, g_ball.z) + R;
        g_ball.vy = 0; g_ball.grounded = 1;
        if (d->goal) { g_win = 1; g_events |= EV_WIN; }
    } else {
        g_ball.grounded = 0;
        g_ball.vy -= GRAVITY;
        g_ball.y += g_ball.vy;
        if (g_ball.y < VOID_Y) { lost = 1; g_events |= EV_FALL; }
    }

    if (!lost && !g_win) {
        g_timer--;
        if (g_timer <= 0) { lost = 1; g_events |= EV_TIMEOUT; }
    }
    if (lost) {
        g_lives--;
        if (g_lives <= 0) { g_over = 1; g_events |= EV_OVER; }
        else respawn();
    }
}

static void audio(void) {
    if (g_events & EV_FALL)    synth_note(NCHAN - 1, 3, 45, 200);
    if (g_events & EV_TIMEOUT) synth_note(NCHAN - 1, 5, 50, 180);
    if (g_events & EV_OVER)    synth_note(NCHAN - 1, 5, 36, 220);
    if (g_events & EV_WIN)     synth_note(NCHAN - 1, 4, 76, 220);
    if (g_events & EV_RESTART) synth_note(NCHAN - 1, 1, 60, 150);
}

// ---- hud ------------------------------------------------------------------------
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) { char b[12]; digits(v, b); text_draw(x, y, sc, b, ci); }

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "LIVES", 1);
    num(6 * sc + 34 * sc, hud_top(), sc, g_lives, 2);

    int32_t secs = g_timer > 0 ? g_timer / 60 : 0;
    text_draw(g_fbw - 64 * sc, hud_top(), sc, "TIME", (secs <= 5 && !g_over && !g_win) ? 3 : 1);
    num(g_fbw - 28 * sc, hud_top(), sc, secs, (secs <= 5 && !g_over && !g_win) ? 3 : 2);

    if (!g_over && !g_win) return;
    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int y = cy - 30 * sc; y < cy + 34 * sc; y++)
        for (int x = 0; x < g_fbw; x++)
            if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = g_win ? 2 : 3;
    const char *msg = g_win ? "COURSE CLEAR" : "OUT OF LIVES";
    text_draw(cx - text_width(msg, sc * 3) / 2, cy - 22 * sc, sc * 3, msg, 1);
    text_draw(cx - text_width("SPACE TO RESTART", sc) / 2, cy + 16 * sc, sc, "SPACE TO RESTART", 1);
}

// ---- camera + draw --------------------------------------------------------------
// A fixed pitch, a fixed 45-degree yaw, and the offset that pitch/yaw imply — computed
// with g3d_rot itself so it's exactly consistent with how the renderer will later undo
// it, rather than a hand-tuned vector that only happens to look right from one spot.
#define CAM_AX   150     // pitch: steep enough to read as looking down onto the course
#define CAM_AY   128     // yaw: 128/1024 = 45 degrees — the oblique in "isometric"
#define CAMDIST  TU(760)

static void draw(void) {
    fb_clear(0);
    static Inst inst[NTILES + 1];
    int n = 0;
    for (int i = 0; i < NTILES; i++) {
        inst[n].m = &tm[i]; inst[n].pos = (V3){0,0,0};
        inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = 1 << 16;
        n++;
    }
    inst[n].m = &ballm;
    inst[n].pos = (V3){ g_ball.x, g_ball.y, g_ball.z };
    inst[n].ax = g_ball.spinx; inst[n].ay = 0; inst[n].az = g_ball.spinz;
    inst[n].scale = R;
    n++;

    int32_t ox = 0, oy = 0, oz = CAMDIST;
    g3d_rot(&ox, &oy, &oz, CAM_AX, CAM_AY, 0);
    Cam cam = { { g_ball.x - ox, g_ball.y - oy, g_ball.z - oz }, CAM_AX, CAM_AY, 0 };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    hud();
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ULL;
    uint32_t vals[10] = {
        (uint32_t)g_ball.x, (uint32_t)g_ball.y, (uint32_t)g_ball.z,
        (uint32_t)g_ball.vx, (uint32_t)g_ball.vy, (uint32_t)g_ball.vz,
        (uint32_t)g_lives, (uint32_t)g_timer,
        (uint32_t)(g_over | (g_win << 1)),
        (uint32_t)((g_ball.spinx & 0xFFFF) | (g_ball.spinz << 16)),
    };
    for (int i = 0; i < 10; i++) { h ^= vals[i]; h *= 1099511628211ULL; }
    return h;
}

void marble_probe(int32_t *x, int32_t *y, int32_t *z, int32_t *vx, int32_t *vz, int *lives) {
    *x = g_ball.x; *y = g_ball.y; *z = g_ball.z; *vx = g_ball.vx; *vz = g_ball.vz; *lives = g_lives;
}

const Game game_marble = { "marble", init, tick, audio, draw, checksum };
