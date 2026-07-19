// well.c — Rim-of-the-Well: a Tempest homage told straight, with a camera that never
// moves and a tube that does all the work.
//
// The camera sits at the mouth and looks straight down the throat, forever. It never
// dollies, never turns — every ounce of "3D in motion" here comes from the geometry
// itself: a funnel that recedes and tapers (so far is small and near is huge, the way
// perspective actually behaves, not a trick of it), and from things riding that funnel's
// surface — the player's claw circling the rim, enemies climbing its walls, bolts falling
// away into the bottom. You orbit the mouth of a well. You do not fly through it.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"
#include <math.h>

#define U (1 << 16)

// ---- the well -----------------------------------------------------------------
// A ring of lanes, receding: NRING circles of NLANE points each, the near ring wide and
// the far one narrow — a funnel, not a cylinder, so depth reads even before anything
// moves in it. One mesh, built once; nothing about its shape changes at runtime.
#define NLANE   16
#define NSEG    10
#define NRING   (NSEG + 1)
#define LANEW   (1024 / NLANE)          // 64 table-units per lane; divides 1024 exactly
#define R_NEAR  (U * 17 / 5)            // 3.4 units — the mouth, right in the camera's face
#define R_FAR   (U * 6 / 10)            // 0.6 units — the bottom, far and small
#define Z_FAR   (U * 16)                // 16 units of throat between them
#define OBJ_IN  (U / 8)                 // things ride slightly inside the wall, not on it

#define WELL_NV (NRING * NLANE + 1)     // +1: the centre point of the floor at the bottom
#define WELL_NT (NSEG * NLANE * 2 + NLANE)

static V3  well_v[WELL_NV];
static Tri well_t[WELL_NT];
static Mesh well_m;

static int32_t ring_r(int s) { return R_NEAR - (int32_t)((int64_t)(R_NEAR - R_FAR) * s / NSEG); }
static int32_t ring_z(int s) { return (int32_t)((int64_t)Z_FAR * s / NSEG); }

// A point on the well's own surface (or anything riding it): angle in table units
// (0..1023, one lap of the rim), a radius, a depth. The camera looks straight down +Z,
// so the rim's circle lives in the XY plane and Z alone is "how deep".
static void well_pt(int ang, int32_t r, int32_t z, V3 *p) {
    ang &= 1023;
    p->x = (int32_t)(((int64_t)r * g_sin[(ang + 256) & 1023]) >> 15);
    p->y = (int32_t)(((int64_t)r * g_sin[ang]) >> 15);
    p->z = z;
}
static int32_t well_r(int32_t z) {                       // the taper, continuous in z
    if (z < 0) z = 0; if (z > Z_FAR) z = Z_FAR;
    return R_NEAR - (int32_t)((int64_t)(R_NEAR - R_FAR) * z / Z_FAR);
}

static int16_t q15(float v) { if (v > 1) v = 1; if (v < -1) v = -1; return (int16_t)(v * 32767.f); }

#define WELL_A   8     // one lane colour, 8 shades
#define WELL_B  16     // the alternate lane colour — the checkerboard IS the lane grid
#define WELL_CAP 24    // the floor at the bottom

// Built once, at init, the same way core.c bakes g_sin with sinf: a one-time table, not a
// per-frame computation, so purity in tick() is untouched. A real cross-product normal per
// quad, not a radial guess — the funnel actually tilts (3.4 units down to 0.6), so the true
// normal carries that tilt and the wall shades in a gradient down the throat instead of
// reading as a flat painted cylinder.
static void build_well(void) {
    int vi = 0;
    for (int s = 0; s <= NSEG; s++) {
        int32_t r = ring_r(s), z = ring_z(s);
        for (int l = 0; l < NLANE; l++) well_pt(l * LANEW, r, z, &well_v[vi++]);
    }
    int center = vi;
    well_v[vi++] = (V3){ 0, 0, Z_FAR };

    int ti = 0;
    for (int s = 0; s < NSEG; s++) {
        for (int l = 0; l < NLANE; l++) {
            int l1 = (l + 1) % NLANE;
            int i00 = s * NLANE + l,       i10 = s * NLANE + l1;
            int i01 = (s + 1) * NLANE + l, i11 = (s + 1) * NLANE + l1;
            V3 *p00 = &well_v[i00], *p10 = &well_v[i10], *p01 = &well_v[i01];
            float ex1 = (float)(p10->x - p00->x), ey1 = (float)(p10->y - p00->y), ez1 = (float)(p10->z - p00->z);
            float ex2 = (float)(p01->x - p00->x), ey2 = (float)(p01->y - p00->y), ez2 = (float)(p01->z - p00->z);
            float nx = ey1 * ez2 - ez1 * ey2, ny = ez1 * ex2 - ex1 * ez2, nz = ex1 * ey2 - ey1 * ex2;
            float len = sqrtf(nx*nx + ny*ny + nz*nz); if (len < 1.f) len = 1.f;
            nx /= len; ny /= len; nz /= len;
            // The camera stands INSIDE the tube: the visible face is the inner one, so
            // force the normal to point back toward the axis, not away from it.
            if (nx * (float)p00->x + ny * (float)p00->y > 0) { nx = -nx; ny = -ny; nz = -nz; }
            uint8_t ci = (uint8_t)(((l + s) & 1) ? WELL_B : WELL_A);
            int16_t qx = q15(nx), qy = q15(ny), qz = q15(nz);
            well_t[ti++] = (Tri){ (uint16_t)i00, (uint16_t)i10, (uint16_t)i11, ci, qx, qy, qz };
            well_t[ti++] = (Tri){ (uint16_t)i00, (uint16_t)i11, (uint16_t)i01, ci, qx, qy, qz };
        }
    }
    for (int l = 0; l < NLANE; l++) {                     // the bottom: a floor, so a
        int l1 = (l + 1) % NLANE;                         // "well" has one to look down at
        int i0 = NSEG * NLANE + l, i1 = NSEG * NLANE + l1;
        well_t[ti++] = (Tri){ (uint16_t)center, (uint16_t)i1, (uint16_t)i0,
                               WELL_CAP, 0, 0, (int16_t)-32767 };   // faces straight back at the lens
    }
    well_m = (Mesh){ well_v, WELL_NV, well_t, WELL_NT };
}

// ---- a small glossy octahedron -------------------------------------------------
// One shape, three colours: the claw, the two enemy kinds, and the bolt are all this same
// eight-facet gem, scaled and tinted differently, spinning slowly so its flat facets sweep
// through the (camera-relative) light one at a time — that sweep IS the gloss.
static const int8_t OCV[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
static const uint8_t OCF[8][3] = { {0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5} };
static const int8_t ONRM[8][3] = { {1,1,1},{-1,1,1},{-1,-1,1},{1,-1,1},{1,1,-1},{-1,1,-1},{-1,-1,-1},{1,-1,-1} };
#define OCN 18918   // 32767 / sqrt(3), rounded — the exact octahedron facet normal

static void build_octa(V3 *v, Tri *t, uint8_t ci) {
    for (int i = 0; i < 6; i++) {
        v[i].x = OCV[i][0] * U; v[i].y = OCV[i][1] * U; v[i].z = OCV[i][2] * U;
    }
    for (int f = 0; f < 8; f++) {
        t[f].a = OCF[f][0]; t[f].b = OCF[f][1]; t[f].c = OCF[f][2]; t[f].ci = ci;
        t[f].nx = (int16_t)(ONRM[f][0] * OCN);
        t[f].ny = (int16_t)(ONRM[f][1] * OCN);
        t[f].nz = (int16_t)(ONRM[f][2] * OCN);
    }
}

#define PAL_PLAYER  32   // 32..39
#define PAL_ENEMY_A 40   // 40..47  the common climber
#define PAL_ENEMY_B 48   // 48..55  the fast one
#define PAL_BOLT    56   // 56..63

static V3  ov_player[6], ov_ea[6], ov_eb[6], ov_bolt[6];
static Tri ot_player[8], ot_ea[8], ot_eb[8], ot_bolt[8];
static Mesh m_player, m_ea, m_eb, m_bolt;

static void build_shapes(void) {
    build_well();
    build_octa(ov_player, ot_player, PAL_PLAYER); m_player = (Mesh){ ov_player, 6, ot_player, 8 };
    build_octa(ov_ea, ot_ea, PAL_ENEMY_A);         m_ea     = (Mesh){ ov_ea, 6, ot_ea, 8 };
    build_octa(ov_eb, ot_eb, PAL_ENEMY_B);         m_eb     = (Mesh){ ov_eb, 6, ot_eb, 8 };
    build_octa(ov_bolt, ot_bolt, PAL_BOLT);        m_bolt   = (Mesh){ ov_bolt, 6, ot_bolt, 8 };
}

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ---- gameplay state -------------------------------------------------------------
#define MAXENEMY 20
#define MAXBOLT   8

typedef struct { int32_t z; int16_t ang; uint8_t alive, kind; } Enemy;
typedef struct { int32_t z; int16_t ang; uint8_t alive; } Bolt;

static int32_t g_pang;                  // player's angle on the rim, table units
static int     g_lane;                  // nearest lane, for firing and for being grabbed
static int     g_lives, g_score, g_wave;
static uint32_t g_frame, g_rng;
static int     g_fire_cd, g_pjump;

static Enemy g_en[MAXENEMY];
static Bolt  g_bolt[MAXBOLT];
static int   g_spawn_left, g_spawn_timer;
static int32_t g_climb;                 // this wave's climb speed

enum { ST_PLAY, ST_HIT, ST_CLEAR, ST_OVER };
static int g_state, g_state_t;

#define EV_FIRE 1
#define EV_KILL 2
#define EV_GRAB 4
#define EV_CLEAR 8
static uint8_t g_events;

static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }

static void start_wave(int w) {
    g_wave = w;
    for (int i = 0; i < MAXENEMY; i++) g_en[i].alive = 0;
    for (int i = 0; i < MAXBOLT; i++) g_bolt[i].alive = 0;
    g_spawn_left = 6 + w * 2;
    g_spawn_timer = 40;
    g_climb = U / 44 + (int32_t)(w * (U / 260));
    if (g_climb > U / 10) g_climb = U / 10;
}

static void reset_game(void) {
    g_pang = 0; g_lane = 0;
    g_lives = 3; g_score = 0;
    g_fire_cd = 0; g_pjump = 0;
    g_state = ST_PLAY; g_state_t = 0;
    start_wave(1);
}

static void init(void) {
    tables_init();
    build_shapes();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF090C16;   // the void beyond the well
    g_pal[1] = 0xFFE8ECF8;   // HUD primary text
    g_pal[2] = 0xFF5CE1FF;   // HUD accent
    g_pal[3] = 0xFFFF3B30;   // warning / hit flash
    ramp(WELL_A,      0xFF178FA8);
    ramp(WELL_B,      0xFF6A35C9);
    ramp(WELL_CAP,    0xFF2A1E52);
    ramp(PAL_PLAYER,  0xFFFFD23F);
    ramp(PAL_ENEMY_A, 0xFFFF3B30);
    ramp(PAL_ENEMY_B, 0xFFFF9519);
    ramp(PAL_BOLT,    0xFFBFF6FF);
    g_rng = 0x9E3779B9u;
    g_frame = 0;
    reset_game();
}

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0;

    int x = in[0].x + in[1].x; if (x > 1) x = 1; if (x < -1) x = -1;
    int jump = in[0].jump || in[1].jump;

    if (g_state == ST_OVER) {
        if (jump && !g_pjump) reset_game();
        g_pjump = jump;
        return;
    }

    // Orbit the mouth. Wrapping is free: the angle is already mod 1024, one lap.
    if (x) g_pang = (g_pang + x * 10 + 1024) & 1023;
    g_lane = ((g_pang + LANEW / 2) / LANEW) % NLANE;

    if (g_state == ST_HIT) {
        if (--g_state_t <= 0) {
            g_state = g_lives > 0 ? ST_PLAY : ST_OVER;
        }
        g_pjump = jump;
        return;
    }
    if (g_state == ST_CLEAR) {
        if (--g_state_t <= 0) { g_state = ST_PLAY; start_wave(g_wave + 1); }
        g_pjump = jump;
        return;
    }

    // ---- fire: down the lane you're standing in, right now -------------------
    if (g_fire_cd > 0) g_fire_cd--;
    if (jump && !g_pjump && g_fire_cd == 0) {
        for (int i = 0; i < MAXBOLT; i++) if (!g_bolt[i].alive) {
            g_bolt[i] = (Bolt){ 0, (int16_t)(g_lane * LANEW + LANEW / 2), 1 };
            g_fire_cd = 8;
            g_events |= EV_FIRE;
            break;
        }
    }
    g_pjump = jump;

    // ---- bolts fall away down the throat --------------------------------------
    for (int i = 0; i < MAXBOLT; i++) if (g_bolt[i].alive) {
        g_bolt[i].z += Z_FAR / 16;
        if (g_bolt[i].z >= Z_FAR) g_bolt[i].alive = 0;
    }

    // ---- spawn: enemies climb out of the bottom -------------------------------
    if (g_spawn_left > 0) {
        if (--g_spawn_timer <= 0) {
            int slot = -1;
            for (int i = 0; i < MAXENEMY; i++) if (!g_en[i].alive) { slot = i; break; }
            if (slot >= 0) {
                int lane = (int)((rnd() >> 24) % NLANE);
                int kind = (int)((rnd() >> 16) & 1);
                g_en[slot] = (Enemy){ Z_FAR, (int16_t)(lane * LANEW + LANEW / 2), 1, (uint8_t)kind };
                g_spawn_left--;
                g_spawn_timer = 30 - g_wave * 2; if (g_spawn_timer < 12) g_spawn_timer = 12;
            } else {
                g_spawn_timer = 6;   // no room yet — try again shortly, don't lose the spawn
            }
        }
    }

    // ---- enemies climb; reaching the rim grabs you -----------------------------
    for (int i = 0; i < MAXENEMY; i++) if (g_en[i].alive) {
        int32_t speed = g_climb + (g_en[i].kind ? g_climb / 2 : 0);   // kind 1 is the fast one
        g_en[i].z -= speed;
        if (g_en[i].z <= 0) {
            g_en[i].alive = 0;
            g_lives--;
            g_events |= EV_GRAB;
            g_state = ST_HIT; g_state_t = 24;
        }
    }

    // ---- a bolt in the same lane, near enough in depth, kills -------------------
    for (int i = 0; i < MAXBOLT; i++) if (g_bolt[i].alive) {
        for (int j = 0; j < MAXENEMY; j++) if (g_en[j].alive && g_en[j].ang == g_bolt[i].ang) {
            int32_t d = g_bolt[i].z - g_en[j].z; if (d < 0) d = -d;
            if (d < U * 3 / 4) {
                g_en[j].alive = 0; g_bolt[i].alive = 0;
                g_score += 10 + g_wave;
                g_events |= EV_KILL;
                break;
            }
        }
    }

    if (g_state == ST_PLAY && g_spawn_left == 0) {
        int any = 0;
        for (int i = 0; i < MAXENEMY; i++) if (g_en[i].alive) { any = 1; break; }
        if (!any) { g_state = ST_CLEAR; g_state_t = 50; g_events |= EV_CLEAR; }
    }
}

static void audio(void) {
    if (g_events & EV_FIRE)  synth_note(NCHAN - 1, 3, 74, 110);
    if (g_events & EV_KILL)  synth_note(NCHAN - 1, 4, 60, 170);
    if (g_events & EV_GRAB)  synth_note(NCHAN - 1, 5, 40, 220);
    if (g_events & EV_CLEAR) synth_note(NCHAN - 1, 5, 76, 200);
}

// ---- draw -------------------------------------------------------------------
static void num(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i-1-j]; b[i-1-j] = t; }
    b[i] = 0;
    text_draw(x, y, sc, b, ci);
}

#define MAXINST (1 + 1 + MAXENEMY + MAXBOLT)
static Inst g_inst[MAXINST];

static void draw(void) {
    fb_clear(0);
    int n = 0;

    g_inst[n++] = (Inst){ &well_m, {0,0,0}, 0,0,0, U };

    // The claw, riding the rim, slightly proud of the mouth so it silhouettes against the
    // well instead of blending into it, tumbling gently for the gloss.
    {
        V3 p; well_pt((int)g_pang, R_NEAR, U / 5, &p);
        int ay = (int)((g_frame * 5) & 1023);
        g_inst[n++] = (Inst){ &m_player, p, 0, ay, 0, U / 7 };
    }

    for (int i = 0; i < MAXENEMY; i++) if (g_en[i].alive) {
        V3 p; well_pt(g_en[i].ang, well_r(g_en[i].z) - OBJ_IN, g_en[i].z, &p);
        int ax = (int)(((g_frame + (uint32_t)i * 37) * 9) & 1023);
        int ay = (int)(((g_frame + (uint32_t)i * 61) * 13) & 1023);
        int32_t sc = g_en[i].kind ? U / 9 : U / 7;
        g_inst[n++] = (Inst){ g_en[i].kind ? &m_eb : &m_ea, p, ax, ay, 0, sc };
    }

    for (int i = 0; i < MAXBOLT; i++) if (g_bolt[i].alive) {
        V3 p; well_pt(g_bolt[i].ang, well_r(g_bolt[i].z) - OBJ_IN, g_bolt[i].z, &p);
        int az = (int)((g_frame * 40) & 1023);
        g_inst[n++] = (Inst){ &m_bolt, p, 0, 0, az, U / 14 };
    }

    // The camera never moves and never turns: it stands at the mouth and looks straight
    // down the throat, forever. The whole scene gets a slow roll instead — the well itself
    // turning under a fixed lens, which is the one camera move Tempest never had to make.
    Cam cam = { { 0, 0, -(U * 72 / 10) }, 0, 0, 0 };
    int rz = (int)((g_frame * 3 / 2) & 1023);
    g3d_scene(g_inst, n, &cam, 0, 0, rz);

    // ---- HUD: top strip, clear of the well ------------------------------------
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    num(6 * sc, hud_top(), sc, g_score, 2);
    text_draw(g_fbw - 60 * sc, hud_top(), sc, "WAVE", 1);
    num(g_fbw - 24 * sc, hud_top(), sc, g_wave, 2);
    for (int i = 0; i < 3; i++) {
        uint8_t ci = (uint8_t)(i < g_lives ? PAL_PLAYER + 5 : 8);
        int lx = g_fbw / 2 - 24 * sc + i * 16 * sc, ly = 5 * sc;
        for (int py = ly; py < ly + 8 * sc; py++)
            for (int px = lx; px < lx + 10 * sc; px++)
                if (px >= 0 && px < g_fbw && py >= 0 && py < g_fbh) g_fb[py * g_fbw + px] = ci;
    }

    if (g_state == ST_HIT) {
        text_draw(g_fbw/2 - text_width("GRABBED", sc*2)/2, g_fbh/2 - 6*sc, sc*2, "GRABBED", 3);
    } else if (g_state == ST_CLEAR) {
        text_draw(g_fbw/2 - text_width("WELL CLEARED", sc*2)/2, g_fbh/2 - 6*sc, sc*2, "WELL CLEARED", 2);
    } else if (g_state == ST_OVER) {
        int cx = g_fbw/2, cy = g_fbh/2;
        text_draw(cx - text_width("WELL LOST", sc*3)/2, cy - 16*sc, sc*3, "WELL LOST", 3);
        text_draw(cx - text_width("SPACE TO DIVE AGAIN", sc)/2, cy + 16*sc, sc, "SPACE TO DIVE AGAIN", 1);
    }
}

static uint64_t checksum(void) {
    uint64_t h = (uint64_t)g_frame * 1000003u + (uint64_t)g_pang * 31u
                + (uint64_t)g_score * 17u + (uint64_t)g_lives * 13u
                + (uint64_t)g_wave * 7u + (uint64_t)g_state;
    for (int i = 0; i < MAXENEMY; i++) if (g_en[i].alive)
        h = h * 31u + (uint32_t)g_en[i].z + (uint32_t)g_en[i].ang * 3u + g_en[i].kind;
    for (int i = 0; i < MAXBOLT; i++) if (g_bolt[i].alive)
        h = h * 31u + (uint32_t)g_bolt[i].z + (uint32_t)g_bolt[i].ang * 5u;
    return h;
}

const Game game_well = { "well", init, tick, audio, draw, checksum };
