// forms.c — shape decides where you fit.
//
// Not a game yet, and deliberately not a level format either: that would presume a
// design that doesn't exist. This is the one question the design actually rests on —
// can a flattened character pass where an upright one can't? — made playable, so it can
// be answered by looking instead of by arguing.
//
// The whole idea it's testing: you can't change your own shape. Someone punches you flat
// or swallows you long, and the shape you end up in decides which gaps are yours. Which
// means the person deciding your shape is deciding your route, and they get nothing out
// of choosing well.
//
// Collision lives here, in a game, not in the engine. An engine that knew what a wall was
// would be a game. If a second game ever wants this, that's when it earns a move.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"

#define FP 8                       // 1 unit = 1/256 of a virtual pixel
#define CAMZ    (5 << 16)
#define PROJ_V  VH

// ---- forms ------------------------------------------------------------------
// A form is a size. That's the entire mechanic: the gap doesn't check who you are, it
// checks how wide you are.
enum { F_UPRIGHT, F_FLAT, F_LONG, NFORM };
static const struct { int16_t w, h; const char *name; } FORM[NFORM] = {
    { 18, 34, "upright" },   // the shape you're born in: fits nothing special
    {  5, 34, "flat"    },   // punched: thin enough for a door gap
    { 44,  9, "long"    },   // swallowed: low enough for a floor slot
};

// ---- the world --------------------------------------------------------------
// Hand-placed solids, because a level FORMAT presumes we know what a level is. Four
// obstacles, each answering one question.
typedef struct { int16_t x, y, w, h; } Box;
// Each obstacle admits exactly ONE form, and the numbers come from FORM[] rather than
// from taste: a gap sized by eye is a gap nobody fits through. The first draft had a slot
// at y=300..309 for a shape that stands at 307..316 — a door in mid-air, and all three
// forms bounced off it, which for one hopeful minute looked like a collision bug.
// Two doors, one either side of the start, because in a line they'd be a sequence and
// only the form that beats the FIRST one ever gets to meet the second. Each admits
// exactly one shape, and the sizes come from FORM[] rather than from taste — the first
// draft had a slot at y=300..309 for a shape that stands at 307..316, a door in mid-air
// that all three forms bounced off, which for a hopeful minute looked like a collision bug.
// 🔴 In a side-scroller, width and height buy different doors, and only one of them is
// the door you pictured. A wall is thin in X, so a gap in it has to be a gap in Y — you
// get under it by being SHORT. Nothing is ever thin enough in X to slip through a wall
// sideways, because sideways is the axis the wall has no thickness in.
//
// So being flat only helps going DOWN: a narrow shaft in the floor. "Turn sideways and
// slip through the crack" is a 3D thought — in 2D there is no crack to turn into. Worth
// remembering when the design decides how 3D it wants to be.
//
// Two doors, one either side of the start, because in a line only the form that beats the
// FIRST one ever meets the second. Sizes come from FORM[], not from taste: the first
// draft had a slot at y=300..309 for a shape that stands at 307..316 — a door in mid-air —
// and the second put a gap behind a pillar, where nothing could reach it. Both times all
// three forms bounced, and both times it looked briefly like a collision bug.
#define FLOOR_Y 316
static const Box SOLID[] = {
    {   0, FLOOR_Y, 450, 44 },                      // floor, left of the shaft
    { 459, FLOOR_Y, VW - 459, 44 },                 // floor, right of it — the gap is 9 wide
    {   0, FLOOR_Y + 44, VW, 16 },                  // the bottom of the shaft, so it's a room not a void
    { 150,   0, 24, FLOOR_Y - 9 },                  // LEFT: a 9px slot along the floor — LONG (h=9) only
};
#define NSOLID (int)(sizeof SOLID / sizeof SOLID[0])

// ---- actors -----------------------------------------------------------------
typedef struct { int32_t x, y, vx, vy; uint8_t form, grounded, prev_act; } Actor;
static Actor g_act[2];
static uint64_t g_checksum;
static uint8_t g_events;
#define EV_JUMP 1
#define EV_MORPH 2
#define EV_BLOCK 4

static int overlaps(int32_t cx, int32_t cy, int form) {
    int w = FORM[form].w, h = FORM[form].h;
    int l = (cx >> FP) - w / 2, r = l + w;
    int t = (cy >> FP) - h, b = cy >> FP;
    for (int i = 0; i < NSOLID; i++) {
        const Box *s = &SOLID[i];
        if (r > s->x && l < s->x + s->w && b > s->y && t < s->y + s->h) return 1;
    }
    return 0;
}

// Axis at a time, which is the oldest trick in the book and still the right one: resolve
// x, then y, and a corner can never wedge you.
static void move_axis(Actor *a, int32_t dx, int32_t dy) {
    if (dx) {
        if (!overlaps(a->x + dx, a->y, a->form)) a->x += dx;
        else { a->vx = 0; g_events |= EV_BLOCK; }
    }
    if (dy) {
        if (!overlaps(a->x, a->y + dy, a->form)) a->y += dy;
        else {
            if (dy > 0) a->grounded = 1;
            a->vy = 0;
        }
    }
}

static void init(void) {
    tables_init();
    g_act[0] = (Actor){ 300 << FP, FLOOR_Y << FP, 0, 0, F_UPRIGHT, 0, 0 };
    g_act[1] = (Actor){ 340 << FP, FLOOR_Y << FP, 0, 0, F_UPRIGHT, 0, 0 };
    g_checksum = 0;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0]  = 0xFF1A1C2C;   // sky
    g_pal[1]  = 0xFF3A2E4A;   // solids
    g_pal[2]  = 0xFFEF7D57;   // actor A
    g_pal[3]  = 0xFF41A6F6;   // actor B
    g_pal[4]  = 0xFF5D5578;   // a solid's lit edge
}

static void tick(const Input in[2]) {
    g_events = 0;
    for (int i = 0; i < 2; i++) {
        Actor *a = &g_act[i];

        // Cycling your own form is a TEST AFFORDANCE and a lie about the design: the
        // whole point is that you can't do this to yourself. It's here so the question
        // "does shape decide passage" can be answered before the answer to "who decides
        // your shape" exists.
        // 🔴 Edge, not level. act is "held", and a verb that reads it fires every frame — hold
        // the key for a second and the form cycles sixty times. Whether a verb repeats is a
        // design question (holding jump is bunny-hopping, which some games want), so the
        // platform can't answer it and the game has to.
        int pressed = in[i].act && !a->prev_act;
        a->prev_act = in[i].act;
        if (pressed) {
            uint8_t want = (uint8_t)((a->form + 1) % NFORM);
            if (!overlaps(a->x, a->y, want)) { a->form = want; g_events |= EV_MORPH; }
            // Refusing to grow inside a wall is not politeness, it's the only thing
            // stopping a shape change from being a teleport through solid matter.
        }

        a->vx = in[i].x * (2 << FP);
        if (in[i].jump && a->grounded) { a->vy = -(5 << FP); a->grounded = 0; g_events |= EV_JUMP; }
        a->vy += (1 << FP) / 4;
        if (a->vy > (6 << FP)) a->vy = 6 << FP;

        a->grounded = 0;
        move_axis(a, a->vx, 0);
        move_axis(a, 0, a->vy);

        if (a->x < (8 << FP))        a->x = 8 << FP;
        if (a->x > ((VW - 8) << FP)) a->x = (VW - 8) << FP;
        if (a->y > ((VH + 40) << FP)) { a->x = 300 << FP; a->y = FLOOR_Y << FP; a->vy = 0; }

        g_checksum = g_checksum * 31 + (uint32_t)a->x + (uint32_t)a->y + a->form;
    }
}

static void audio(void) {
    if (g_events & EV_JUMP)  synth_note(NCHAN - 1, 5, 84, 170);
    if (g_events & EV_MORPH) synth_note(NCHAN - 1, 4, 60, 150);
}

static void quad(int x, int y, int w, int h, uint8_t ci) {
    int16_t p[8] = { (int16_t)x, (int16_t)y, (int16_t)(x + w), (int16_t)y,
                     (int16_t)(x + w), (int16_t)(y + h), (int16_t)x, (int16_t)(y + h) };
    // The sim thinks in virtual pixels; the framebuffer may be any size.
    for (int i = 0; i < 8; i += 2) {
        p[i]     = (int16_t)((int32_t)p[i] * g_fbw / VW);
        p[i + 1] = (int16_t)((int32_t)p[i + 1] * g_fbh / VH);
    }
    poly_fill(p, 4, ci);
}

static void draw(void) {
    fb_clear(0);
    for (int i = 0; i < NSOLID; i++) {
        quad(SOLID[i].x, SOLID[i].y, SOLID[i].w, SOLID[i].h, 1);
        quad(SOLID[i].x, SOLID[i].y, SOLID[i].w, 2, 4);
    }
    for (int i = 0; i < 2; i++) {
        int w = FORM[g_act[i].form].w, h = FORM[g_act[i].form].h;
        quad((g_act[i].x >> FP) - w / 2, (g_act[i].y >> FP) - h, w, h, (uint8_t)(2 + i));
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_forms = { "forms", init, tick, audio, draw, checksum };
