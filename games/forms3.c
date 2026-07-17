// forms3.c — the same question in three dimensions, where being flat finally means
// something.
//
// In 2D, flatness bought almost nothing: a wall is thin in X, so a door in it has to be a
// gap in Y, and you get under it by being short. Nothing is ever thin enough in X to slip
// through a wall sideways, because sideways is the axis a wall has no thickness in. "Turn
// sideways and slip through the crack" needs a third axis to turn into.
//
// Here it has one. A form is thin ACROSS its facing, so walking at a narrow slot lines your
// thin axis up with it automatically — the player turns sideways without ever being told
// to, because walking somewhere is already aiming.
//
// Still not a game, still no level format. One question, made answerable.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"

#define FP 8                          // 1 unit = 1/256 world unit
// The numbers below read as centimetres and land as world units: W(110) is a character
// 1.1 units tall, next to a cube that was 1. The first pass read them as whole units and
// built a 110-unit person on an 1800-unit floor — the camera ended up inside the ground.
#define W(u) ((int32_t)((int32_t)(u) * 256 / 100))

// ---- forms ------------------------------------------------------------------
// len is along facing, across is perpendicular to it. Flat is thin ACROSS, so it threads a
// slot by walking at it; long is short, so it goes under things.
enum { F_UPRIGHT, F_FLAT, F_LONG, NFORM };
static const struct { int16_t len, height, across; } FORM[NFORM] = {
    { 60, 110, 60 },    // upright: fits nothing special
    { 60, 110, 16 },    // flat: punched — thin across
    { 150, 30, 60 },    // long: swallowed — low
};

// Facing snaps to four directions, which is the whole reason the AABB stays an AABB. A
// free angle would need an oriented box and a separating-axis test; four directions need a
// swap. The mechanic doesn't get better for the extra maths.
enum { D_EAST, D_NORTH, D_WEST, D_SOUTH };
static const int8_t DIR[4][2] = { {1,0}, {0,1}, {-1,0}, {0,-1} };   // x, z

static void extent(int form, int dir, int32_t *ex, int32_t *ey, int32_t *ez) {
    int along = FORM[form].len, across = FORM[form].across;
    int alongX = (dir == D_EAST || dir == D_WEST);
    *ex = W(alongX ? along : across) / 2;
    *ez = W(alongX ? across : along) / 2;
    *ey = W(FORM[form].height);
}

// ---- the world --------------------------------------------------------------
// Sizes come from FORM[], never from taste. Two doors, either side of the start, because
// in a line only the form that beats the first one ever meets the second.
typedef struct { int32_t x, y, z, sx, sy, sz; } Box;   // centre + half-extents
#define FLOOR_Y W(0)
static const Box SOLID[] = {
    { W(0), W(60), W(0), W(700), W(60), W(300) },          // the floor: its top face at y=0

    // WEST: a wall whose underside sits 40 up. Only LONG (height 30) gets beneath it.
    { W(-400), W(-40 - 155), W(0), W(30), W(155), W(300) },

    // EAST: a wall split by a vertical slot 24 wide. Only FLAT (across 16) threads it.
    { W(400), W(-160), W(-162), W(30), W(160), W(150) },
    { W(400), W(-160), W( 162), W(30), W(160), W(150) },
};
#define NSOLID (int)(sizeof SOLID / sizeof SOLID[0])

// ---- actors -----------------------------------------------------------------
typedef struct { int32_t x, y, z, vy; uint8_t form, dir, grounded; } Actor;
static Actor g_act[2];
static uint64_t g_checksum;
static uint8_t g_events;
#define EV_JUMP 1
#define EV_MORPH 2

static int hits(int32_t cx, int32_t cy, int32_t cz, int form, int dir) {
    int32_t ex, ey, ez;
    extent(form, dir, &ex, &ey, &ez);
    for (int i = 0; i < NSOLID; i++) {
        const Box *s = &SOLID[i];
        if (cx + ex <= s->x - s->sx || cx - ex >= s->x + s->sx) continue;
        if (cz + ez <= s->z - s->sz || cz - ez >= s->z + s->sz) continue;
        if (cy <= s->y - s->sy || cy - ey >= s->y + s->sy) continue;   // cy is the feet
        return 1;
    }
    return 0;
}

// One axis at a time. The oldest trick there is, and still the one that stops a corner
// from wedging you.
static void step(Actor *a, int32_t dx, int32_t dy, int32_t dz) {
    if (dx && !hits(a->x + dx, a->y, a->z, a->form, a->dir)) a->x += dx;
    if (dz && !hits(a->x, a->y, a->z + dz, a->form, a->dir)) a->z += dz;
    if (dy) {
        if (!hits(a->x, a->y + dy, a->z, a->form, a->dir)) a->y += dy;
        else { if (dy > 0) a->grounded = 1; a->vy = 0; }
    }
}

static void init(void) {
    tables_init();
    g_act[0] = (Actor){ 0, FLOOR_Y, W(-60), 0, F_UPRIGHT, D_EAST, 0 };
    g_act[1] = (Actor){ 0, FLOOR_Y, W( 60), 0, F_UPRIGHT, D_EAST, 0 };
    g_checksum = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF1A1C2C;
    // A shade ramp is a multiplier, so a dark base can only ever produce dark. The first
    // pass used (58,46,74) against a (26,28,44) sky: it rendered perfectly and looked like
    // an empty screen, and I went hunting for a renderer bug through three passes.
    for (int i = 0; i < 8; i++) {
        int m = 60 + i * 28;
        g_pal[8 + i] = 0xFF000000u | ((uint32_t)(150 * m / 255) << 16)
                     | ((uint32_t)(130 * m / 255) << 8) | (uint32_t)(190 * m / 255);
    }
    for (int k = 0; k < 2; k++) {            // the two actors
        uint32_t c = k ? 0xFF41A6F6 : 0xFFEF7D57;
        for (int i = 0; i < 8; i++) {
            int m = 40 + i * 26;
            g_pal[16 + k * 8 + i] = 0xFF000000u
                | ((uint32_t)(((c >> 16) & 255) * m / 255) << 16)
                | ((uint32_t)(((c >> 8) & 255) * m / 255) << 8)
                | (uint32_t)((c & 255) * m / 255);
        }
    }
}

static void tick(const Input in[2]) {
    g_events = 0;
    for (int i = 0; i < 2; i++) {
        Actor *a = &g_act[i];

        // Cycling your own form is a TEST AFFORDANCE and a lie about the design: the whole
        // point is that someone else does this to you. It's here so "does shape decide
        // passage" can be answered before "who decides your shape" exists.
        if (in[i].act) {            // E / right-shift
            uint8_t want = (uint8_t)((a->form + 1) % NFORM);
            if (!hits(a->x, a->y, a->z, want, a->dir)) { a->form = want; g_events |= EV_MORPH; }
            // Refusing to grow inside a wall isn't politeness — it's the only thing keeping
            // a shape change from being a teleport through solid matter.
        }

        // Two axes on the ground. Facing follows whichever you pushed hardest, and that's
        // the whole aiming interface: walking at a slot already lines your thin side up
        // with it, so a flat character threads it without ever being told to turn.
        int32_t dx = in[i].x * W(3), dz = in[i].y * W(3);
        if (in[i].x || in[i].y) {
            int ax = in[i].x < 0 ? -in[i].x : in[i].x;
            int az = in[i].y < 0 ? -in[i].y : in[i].y;
            if (ax >= az) a->dir = in[i].x > 0 ? D_EAST : D_WEST;
            else          a->dir = in[i].y > 0 ? D_NORTH : D_SOUTH;
        }

        if (in[i].jump && a->grounded) { a->vy = -W(11); a->grounded = 0; g_events |= EV_JUMP; }
        a->vy += W(1) / 2;
        if (a->vy > W(14)) a->vy = W(14);

        a->grounded = 0;
        step(a, dx, 0, dz);
        step(a, 0, a->vy, 0);

        if (a->y > W(600)) { a->x = 0; a->y = FLOOR_Y; a->z = W(-60) + i * W(120); a->vy = 0; }
        g_checksum = g_checksum * 31 + (uint32_t)a->x + (uint32_t)a->y + (uint32_t)a->z + a->form;
    }
}

static void audio(void) {
    if (g_events & EV_JUMP)  synth_note(NCHAN - 1, 5, 84, 170);
    if (g_events & EV_MORPH) synth_note(NCHAN - 1, 4, 60, 150);
}

// One unit cube, reused by every box: an Inst carries its own scale, so a hundred sizes of
// wall are still one mesh.
static V3  ucv[8];
static Tri uct[12];
static Mesh ucm;
static void build_cube(void) {
    static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                     {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
    static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
    static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int v = 0; v < 8; v++) {
        ucv[v].x = VP[v][0] << 15; ucv[v].y = VP[v][1] << 15; ucv[v].z = VP[v][2] << 15;
    }
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &uct[f * 2 + k];
            t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k];
            t->nx = (int16_t)(FN[f][0] * 32767); t->ny = (int16_t)(FN[f][1] * 32767);
            t->nz = (int16_t)(FN[f][2] * 32767);
        }
    ucm.v = ucv; ucm.nv = 8; ucm.t = uct; ucm.nt = 12;
}

// Inst has one scale, and a box needs three. Bake the size into the mesh instead — the
// alternative is a non-uniform scale in the engine, and no other game has asked for one.
static V3 boxv[NSOLID + 2][8];
static Tri boxt[NSOLID + 2][12];
static Mesh boxm[NSOLID + 2];
static void shape_box(int i, int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                     {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
    for (int v = 0; v < 8; v++) {
        boxv[i][v].x = VP[v][0] * sx; boxv[i][v].y = VP[v][1] * sy; boxv[i][v].z = VP[v][2] * sz;
    }
    for (int t = 0; t < 12; t++) { boxt[i][t] = uct[t]; boxt[i][t].ci = ci; }
    boxm[i].v = boxv[i]; boxm[i].nv = 8; boxm[i].t = boxt[i]; boxm[i].nt = 12;
}

#define S(u) ((int32_t)((int64_t)(u) * 65536 / 256))   // sim units -> 16.16 world

static void draw(void) {
    fb_clear(0);
    static Inst inst[NSOLID + 2];
    int n = 0;

    for (int i = 0; i < NSOLID; i++) {
        shape_box(n, S(SOLID[i].sx), S(SOLID[i].sy), S(SOLID[i].sz), 8);
        inst[n].m = &boxm[n];
        inst[n].pos = (V3){ S(SOLID[i].x), -S(SOLID[i].y), S(SOLID[i].z) };
        inst[n].ax = inst[n].ay = inst[n].az = 0;
        inst[n].scale = 1 << 16;
        n++;
    }
    for (int i = 0; i < 2; i++) {
        int32_t ex, ey, ez;
        extent(g_act[i].form, g_act[i].dir, &ex, &ey, &ez);
        shape_box(n, S(ex), S(ey) / 2, S(ez), (uint8_t)(16 + i * 8));
        inst[n].m = &boxm[n];
        inst[n].pos = (V3){ S(g_act[i].x), -S(g_act[i].y) + S(ey) / 2, S(g_act[i].z) };
        inst[n].ax = inst[n].ay = inst[n].az = 0;
        inst[n].scale = 1 << 16;
        n++;
    }

    // Behind and above, looking down the world. A fixed camera, because a moving one is a
    // design decision and there isn't a design yet.
    // 🔴 Cam.pos is WORLD space: +Y is up. The game thinks +Y is down (gravity is
    // positive), and draw() negates it on the way out — so negating the camera too put it
    // under the floor, looking at the underside of the world.
    Cam cam = { { 0, S(W(240)), -S(W(620)) }, 60, 0, 0 };
    g3d_scene(inst, n, &cam, 0, 0, 0);
}

// A probe that answers in world units. The last three checks read screen pixels and
// couldn't tell "stopped by a wall" from "this box is simply longer" — which is how three
// unreachable doors in a row each looked briefly like a collision bug.
void forms3_probe(int32_t *x, int32_t *y, int32_t *z, int *form) {
    *x = g_act[0].x; *y = g_act[0].y; *z = g_act[0].z; *form = g_act[0].form;
}

static uint64_t checksum(void) { return g_checksum; }

static void init3(void) { build_cube(); init(); }

const Game game_forms3 = { "forms3", init3, tick, audio, draw, checksum };
