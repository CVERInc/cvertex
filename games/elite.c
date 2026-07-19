// elite.c — open-space flight combat.
//
// No rail, no track: the player is a point in free space that only ever points
// somewhere and cruises forward. Steering is the whole interface — up/down pitches,
// left/right yaws — and the camera IS the ship, so g3d_scene's own Cam does the work
// tube/racer do by hand-rotating the scene: Cam.pos is the ship, Cam.ax/ay/az is where
// it's looking, and g3d_unrot inside g3d_scene turns world space into view space from
// there. Nothing else in this file rotates a "scene"; the engine already has a camera,
// so the camera is used.
//
// Everything that isn't the player is either fixed in world space (the starfield, the
// station) or a small function of the frame counter (enemy drift and tumble), which
// means enemy position never needs to be stored — tick() and draw() both call the same
// enemy_pos()/enemy_rot(), so there is exactly one definition of "where enemy i is".
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
static int32_t mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

// ---- tuning -------------------------------------------------------------------
#define ENEMY_N     6
#define MAX_PBOLT   16
#define MAX_EBOLT   8
#define MAX_SHARD   32
#define STAR_N      70
#define MAXINST     (STAR_N + 1 + ENEMY_N + MAX_PBOLT + MAX_EBOLT + MAX_SHARD + 8)

#define CRUISE      (U * 22 / 100)     // constant forward speed: steer, don't throttle
#define YAW_RATE    5
#define PITCH_RATE  5
#define ROLL_AMOUNT 60                 // visual bank into a turn, table units

#define FIRE_CD       8
#define BOLT_SPEED    (U * 95 / 100)
#define BOLT_LIFE     40
#define LATERAL_OFF   (U * 35 / 100)   // twin bolts start this far apart...
#define CONVERGE_DIST (U * 8)          // ...and meet on the centreline by here
#define HIT_R         (U * 14 / 10)

#define RAM_R          (U * 16 / 10)
#define PLAYER_HIT_R   (U * 12 / 10)
#define FIRE_RANGE     (U * 45)
#define TRAVEL_FRAMES  45
#define ENEMY_FIRE_CD  90

#define RESPAWN_FRAMES  90
#define SHARD_PER_KILL  8
#define SHARD_LIFE      30
#define SHARD_SPEED     (U * 12 / 10)

#define INVULN_FRAMES     60
#define SHAKE_FRAMES      18
#define FLASH_FRAMES      10
#define DOCK_FLASH_FRAMES 60

#define STATION_SPIN_RATE 1
#define RADAR_RANGE        (U * 60)

static const V3 STATION_POS = { 0, 0, U * 45 };
#define HOLE_H   (U * 2)                // the docking slot's own half-width/height
#define BEAM_T   (U * 6 / 10)
#define BEAM_OFF (HOLE_H + BEAM_T)
#define RING_D   (U * 6 / 10)
#define DOCK_HX  (U * 17 / 10)          // slightly smaller than HOLE_H: fly THROUGH it
#define DOCK_HY  (U * 17 / 10)
#define DOCK_HZ  (U * 3)

// ---- palette (base colours; the renderer adds 0..7 shade from the normal) ------
enum {
    PAL_ENEMY_HULL = 8, PAL_ENEMY_ACCENT = 16, PAL_BOLT_PLAYER = 24, PAL_BOLT_ENEMY = 32,
    PAL_STAR = 40, PAL_STATION_FRAME = 48, PAL_STATION_HULL = 56, PAL_SHARD = 64,
};
enum {
    HUD_WHITE = 80, HUD_DIM, HUD_GREEN, HUD_RED, HUD_BLUE, HUD_YELLOW, HUD_PANEL, HUD_OVERLAY,
};

// ---- events, for audio() to read without re-deciding anything -----------------
enum { EV_FIRE = 1, EV_KILL = 2, EV_HIT = 4, EV_EFIRE = 8, EV_DOCK = 16 };
static uint8_t g_events;

// ---- state ----------------------------------------------------------------------
static V3  g_pos;
static int g_ax, g_ay, g_az;           // pitch, yaw, (visual) roll
static int g_lives, g_score, g_over;
static uint32_t g_frame;
static int g_fire_cd, g_invuln_t, g_shake_t, g_flash_t, g_dock_flash_t, g_dock_was_in;
static uint8_t g_prev_jump;

typedef struct {
    uint8_t active;
    int ax, ay;                          // orientation at fire time, for rendering
    int32_t ocx, ocy, ocz;               // origin (player position at fire time)
    int32_t fx, fy, fz;                  // forward unit vector at fire time
    int32_t offx, offy, offz;            // lateral offset vector (full, at dist=0)
    int32_t dist;
    int life;
    int32_t cx, cy, cz;                  // current position, filled in by tick()
} Bolt;
static Bolt g_pbolt[MAX_PBOLT];

typedef struct { uint8_t active; int32_t x, y, z, vx, vy, vz; int life; } EBolt;
static EBolt g_ebolt[MAX_EBOLT];

typedef struct { uint8_t active; int32_t x, y, z, vx, vy, vz; int life; } Shard;
static Shard g_shard[MAX_SHARD];

typedef struct { uint8_t alive; int respawn_t, fire_cd; } Enemy;
static Enemy g_enemy[ENEMY_N];

static const V3 ENEMY_INIT[ENEMY_N] = {
    {  U * 8,  U * 2, U * 15 }, { -U * 10, -U * 3, U * 22 }, {  U * 4,  U * 6, U * 38 },
    { -U * 14, U * 4, U * 50 }, {  U * 12, -U * 5, U * 65 }, { -U * 6,  U * 3, U * 85 },
};

// Position/rotation are pure functions of (index, frame) — no state to desync, and
// tick()'s collision checks and draw()'s rendering read the exact same place.
static void enemy_pos(int i, uint32_t f, V3 *p) {
    V3 b = ENEMY_INIT[i];
    int a1 = (int)((f * (uint32_t)(3 + i) + (uint32_t)i * 191u) & 1023);
    int a2 = (int)((f * (uint32_t)(2 + i) + (uint32_t)i * 97u + 300u) & 1023);
    int a3 = (int)((f * (uint32_t)(4 + i) + (uint32_t)i * 53u + 600u) & 1023);
    int32_t amp = U * 6;
    p->x = b.x + ((amp * g_sin[a1]) >> 15);
    p->y = b.y + (((amp / 2) * g_sin[a2]) >> 15);
    p->z = b.z + ((amp * g_sin[a3]) >> 15);
}
static void enemy_rot(int i, uint32_t f, int *ax, int *ay, int *az) {
    *ax = (int)((f * (uint32_t)(5 + i) + (uint32_t)i * 11u) & 1023);
    *ay = (int)((f * (uint32_t)(7 + i) + (uint32_t)i * 29u) & 1023);
    *az = (int)((f * (uint32_t)(3 + i) + (uint32_t)i * 17u) & 1023);
}

static int dist2_lt(int32_t dx, int32_t dy, int32_t dz, int32_t r) {
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
    int64_t rr = (int64_t)r * r;
    return d2 < rr;
}

static int find_free_pbolt(void) { for (int i = 0; i < MAX_PBOLT; i++) if (!g_pbolt[i].active) return i; return -1; }
static int find_free_ebolt(void) { for (int i = 0; i < MAX_EBOLT; i++) if (!g_ebolt[i].active) return i; return -1; }
static int find_free_shard(void) { for (int i = 0; i < MAX_SHARD; i++) if (!g_shard[i].active) return i; return -1; }

static void spawn_shards(int32_t cx, int32_t cy, int32_t cz) {
    for (int k = 0; k < SHARD_PER_KILL; k++) {
        int slot = find_free_shard();
        if (slot < 0) continue;
        Shard *s = &g_shard[slot];
        int a  = (k * 1024 / SHARD_PER_KILL) & 1023;
        int el = (k * 307) & 1023;
        int32_t dirx = g_sin[a], dirz = g_sin[(a + 256) & 1023], diry = g_sin[el] / 2;
        s->active = 1; s->life = SHARD_LIFE;
        s->x = cx; s->y = cy; s->z = cz;
        s->vx = (int32_t)(((int64_t)dirx * SHARD_SPEED) >> 15);
        s->vy = (int32_t)(((int64_t)diry * SHARD_SPEED) >> 15);
        s->vz = (int32_t)(((int64_t)dirz * SHARD_SPEED) >> 15);
    }
}

static void do_player_hit(void) {
    g_lives--; g_invuln_t = INVULN_FRAMES; g_shake_t = SHAKE_FRAMES; g_flash_t = FLASH_FRAMES;
    g_events |= EV_HIT;
    if (g_lives <= 0) { g_lives = 0; g_over = 1; }
}

static void reset_game(void) {
    g_pos = (V3){ 0, 0, 0 };
    g_ax = g_ay = g_az = 0;
    g_lives = 3; g_score = 0; g_over = 0; g_frame = 0;
    g_fire_cd = g_invuln_t = g_shake_t = g_flash_t = g_dock_flash_t = g_dock_was_in = 0;
    g_prev_jump = 0;
    for (int i = 0; i < MAX_PBOLT; i++) g_pbolt[i].active = 0;
    for (int i = 0; i < MAX_EBOLT; i++) g_ebolt[i].active = 0;
    for (int i = 0; i < MAX_SHARD; i++) g_shard[i].active = 0;
    for (int i = 0; i < ENEMY_N; i++) { g_enemy[i].alive = 1; g_enemy[i].respawn_t = 0; g_enemy[i].fire_cd = 20 + i * 15; }
}

// ---- tick -------------------------------------------------------------------
static void tick(const Input in[2]) {
    g_events = 0;

    // WASD and the arrows both drive: sum the two controllers, clamp to one step.
    int ix = in[0].x + in[1].x; if (ix > 1) ix = 1; if (ix < -1) ix = -1;
    int iy = in[0].y + in[1].y; if (iy > 1) iy = 1; if (iy < -1) iy = -1;
    uint8_t jump = (uint8_t)(in[0].jump | in[1].jump);

    if (g_over) {
        if (jump && !g_prev_jump) reset_game();
        g_prev_jump = jump;
        return;
    }

    // ---- orientation: up/down pitches, left/right yaws --------------------
    g_ay += ix * YAW_RATE;
    g_ax -= iy * PITCH_RATE;             // g3d_rot's X term: +ax tips forward=Y-down,
    g_ax &= 1023; g_ay &= 1023;          // so nose-up (iy>0) has to SUBTRACT.
    int target_roll = -ix * ROLL_AMOUNT;
    g_az += (target_roll - g_az) >> 3;   // bank into the turn, ease back out of it

    // ---- fly: constant cruise along wherever the nose is pointing ---------
    int32_t fx = 0, fy = 0, fz = U;
    g3d_rot(&fx, &fy, &fz, g_ax, g_ay, 0);
    g_pos.x += mul(fx, CRUISE); g_pos.y += mul(fy, CRUISE); g_pos.z += mul(fz, CRUISE);

    // ---- fire: twin bolts, converging to the centreline --------------------
    if (g_fire_cd > 0) g_fire_cd--;
    if (jump && g_fire_cd <= 0) {
        g_fire_cd = FIRE_CD;
        g_events |= EV_FIRE;
        int32_t rx = U, ry = 0, rz = 0;
        g3d_rot(&rx, &ry, &rz, g_ax, g_ay, 0);
        for (int side = -1; side <= 1; side += 2) {
            int slot = find_free_pbolt();
            if (slot < 0) continue;
            Bolt *b = &g_pbolt[slot];
            b->active = 1; b->dist = 0; b->life = BOLT_LIFE; b->ax = g_ax; b->ay = g_ay;
            b->ocx = g_pos.x; b->ocy = g_pos.y; b->ocz = g_pos.z;
            b->fx = fx; b->fy = fy; b->fz = fz;
            b->offx = mul(rx, side * LATERAL_OFF);
            b->offy = mul(ry, side * LATERAL_OFF);
            b->offz = mul(rz, side * LATERAL_OFF);
            b->cx = b->ocx + b->offx; b->cy = b->ocy + b->offy; b->cz = b->ocz + b->offz;
        }
    }

    // ---- player bolts: advance, converge, and check enemies ----------------
    for (int k = 0; k < MAX_PBOLT; k++) {
        Bolt *b = &g_pbolt[k];
        if (!b->active) continue;
        b->dist += BOLT_SPEED; b->life--;
        if (b->life <= 0) { b->active = 0; continue; }
        int32_t cd = b->dist < CONVERGE_DIST ? b->dist : CONVERGE_DIST;
        int32_t factor = 1024 - (int32_t)(((int64_t)cd * 1024) / CONVERGE_DIST);   // 1024..0
        b->cx = b->ocx + mul(b->fx, b->dist) + ((b->offx * factor) >> 10);
        b->cy = b->ocy + mul(b->fy, b->dist) + ((b->offy * factor) >> 10);
        b->cz = b->ocz + mul(b->fz, b->dist) + ((b->offz * factor) >> 10);

        for (int e = 0; e < ENEMY_N; e++) {
            if (!g_enemy[e].alive) continue;
            V3 ep; enemy_pos(e, g_frame, &ep);
            if (dist2_lt(b->cx - ep.x, b->cy - ep.y, b->cz - ep.z, HIT_R)) {
                g_enemy[e].alive = 0; g_enemy[e].respawn_t = RESPAWN_FRAMES;
                g_score += 100;
                spawn_shards(ep.x, ep.y, ep.z);
                g_events |= EV_KILL;
                b->active = 0;
                break;
            }
        }
    }

    // ---- enemies: respawn timer, ram check, return fire ---------------------
    for (int e = 0; e < ENEMY_N; e++) {
        Enemy *en = &g_enemy[e];
        if (!en->alive) {
            if (en->respawn_t > 0 && --en->respawn_t == 0) en->alive = 1;
            continue;
        }
        V3 ep; enemy_pos(e, g_frame, &ep);
        int32_t dx = g_pos.x - ep.x, dy = g_pos.y - ep.y, dz = g_pos.z - ep.z;

        if (g_invuln_t <= 0 && dist2_lt(dx, dy, dz, RAM_R)) {
            en->alive = 0; en->respawn_t = RESPAWN_FRAMES;
            spawn_shards(ep.x, ep.y, ep.z);
            do_player_hit();
            continue;
        }
        if (en->fire_cd > 0) { en->fire_cd--; }
        else if (dist2_lt(dx, dy, dz, FIRE_RANGE)) {
            en->fire_cd = ENEMY_FIRE_CD;
            int slot = find_free_ebolt();
            if (slot >= 0) {
                EBolt *bo = &g_ebolt[slot];
                bo->active = 1; bo->life = TRAVEL_FRAMES + 15;
                bo->x = ep.x; bo->y = ep.y; bo->z = ep.z;
                bo->vx = dx / TRAVEL_FRAMES; bo->vy = dy / TRAVEL_FRAMES; bo->vz = dz / TRAVEL_FRAMES;
                g_events |= EV_EFIRE;
            }
        }
    }

    // ---- enemy bolts: advance, check the player -----------------------------
    for (int k = 0; k < MAX_EBOLT; k++) {
        EBolt *bo = &g_ebolt[k];
        if (!bo->active) continue;
        bo->x += bo->vx; bo->y += bo->vy; bo->z += bo->vz; bo->life--;
        if (bo->life <= 0) { bo->active = 0; continue; }
        if (g_invuln_t <= 0 && dist2_lt(bo->x - g_pos.x, bo->y - g_pos.y, bo->z - g_pos.z, PLAYER_HIT_R)) {
            bo->active = 0;
            do_player_hit();
        }
    }

    // ---- shards: fly apart, fade ---------------------------------------------
    for (int k = 0; k < MAX_SHARD; k++) {
        Shard *s = &g_shard[k];
        if (!s->active) continue;
        s->x += s->vx; s->y += s->vy; s->z += s->vz;
        if (--s->life <= 0) s->active = 0;
    }

    // ---- the station: slot check, edge-triggered so one pass = one bonus -----
    {
        int32_t dx = g_pos.x - STATION_POS.x, dy = g_pos.y - STATION_POS.y, dz = g_pos.z - STATION_POS.z;
        int spin = (int)((g_frame * STATION_SPIN_RATE) & 1023);
        g3d_unrot(&dx, &dy, &dz, 0, spin, 0);
        int in_slot = dx > -DOCK_HX && dx < DOCK_HX && dy > -DOCK_HY && dy < DOCK_HY && dz > -DOCK_HZ && dz < DOCK_HZ;
        if (in_slot && !g_dock_was_in) { g_score += 500; g_dock_flash_t = DOCK_FLASH_FRAMES; g_events |= EV_DOCK; }
        g_dock_was_in = in_slot;
    }

    if (g_invuln_t > 0) g_invuln_t--;
    if (g_shake_t > 0) g_shake_t--;
    if (g_flash_t > 0) g_flash_t--;
    if (g_dock_flash_t > 0) g_dock_flash_t--;

    g_frame++;
    g_prev_jump = jump;
}

static void audio(void) {
    if (g_events & EV_FIRE)  synth_note(NCHAN - 1, 5, 80, 140);
    if (g_events & EV_EFIRE) synth_note(NCHAN - 1, 5, 60, 100);
    if (g_events & EV_KILL)  synth_note(NCHAN - 1, 4, 48, 200);
    if (g_events & EV_HIT)   synth_note(NCHAN - 1, 3, 36, 220);
    if (g_events & EV_DOCK)  synth_note(NCHAN - 1, 4, 72, 180);
}

// ---- meshes, built once at init ----------------------------------------------
// One generic box builder, appending into a growing (V,T) pair — a compound mesh is
// just several boxes whose vertices/triangles land in the same arrays. Known-good
// vertex/face/normal tables (outward normals, verified by every other box in this
// engine), so the risk of an inside-out face is in the CALLER's numbers, not these.
static void add_box(V3 *V, Tri *T, int *nv, int *nt,
                     int32_t cx, int32_t cy, int32_t cz,
                     int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                     {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
    static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
    static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    int base = *nv;
    for (int v = 0; v < 8; v++) {
        V[base + v].x = cx + VP[v][0] * sx;
        V[base + v].y = cy + VP[v][1] * sy;
        V[base + v].z = cz + VP[v][2] * sz;
    }
    int tb = *nt;
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &T[tb + f * 2 + k];
            t->a = (uint16_t)(base + FQ[f][0]); t->b = (uint16_t)(base + FQ[f][1 + k]); t->c = (uint16_t)(base + FQ[f][2 + k]);
            t->ci = ci;
            t->nx = (int16_t)(FN[f][0] * 32767); t->ny = (int16_t)(FN[f][1] * 32767); t->nz = (int16_t)(FN[f][2] * 32767);
        }
    *nv += 8; *nt += 12;
}

static V3 enemy_v[32]; static Tri enemy_t[48]; static Mesh enemy_m;
static V3 station_v[48]; static Tri station_t[72]; static Mesh station_m;
static V3 boltp_v[8]; static Tri boltp_t[12]; static Mesh bolt_p_m;
static V3 bolte_v[8]; static Tri bolte_t[12]; static Mesh bolt_e_m;
static V3 star_v[8]; static Tri star_t[12]; static Mesh star_m;
static V3 shard_v[8]; static Tri shard_t[12]; static Mesh shard_m;
static V3 star_pos[STAR_N];

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

static void build_meshes(void) {
    int nv, nt;

    // A little fighter: a hull, two wings, and a bright cockpit accent.
    nv = nt = 0;
    add_box(enemy_v, enemy_t, &nv, &nt, 0, 0, 0, U * 30 / 100, U * 15 / 100, U * 70 / 100, PAL_ENEMY_HULL);
    add_box(enemy_v, enemy_t, &nv, &nt, -U * 55 / 100, 0, -U * 10 / 100, U * 35 / 100, U * 5 / 100, U * 30 / 100, PAL_ENEMY_HULL);
    add_box(enemy_v, enemy_t, &nv, &nt,  U * 55 / 100, 0, -U * 10 / 100, U * 35 / 100, U * 5 / 100, U * 30 / 100, PAL_ENEMY_HULL);
    add_box(enemy_v, enemy_t, &nv, &nt, 0, U * 18 / 100, U * 55 / 100, U * 8 / 100, U * 12 / 100, U * 15 / 100, PAL_ENEMY_ACCENT);
    enemy_m.v = enemy_v; enemy_m.nv = nv; enemy_m.t = enemy_t; enemy_m.nt = nt;

    // The station: a square docking ring (fly through the hole) plus an offset
    // control tower, so it reads as a structure, not a box, as it slowly turns.
    nv = nt = 0;
    add_box(station_v, station_t, &nv, &nt, 0,  BEAM_OFF, 0, HOLE_H + BEAM_T, BEAM_T, RING_D, PAL_STATION_FRAME);
    add_box(station_v, station_t, &nv, &nt, 0, -BEAM_OFF, 0, HOLE_H + BEAM_T, BEAM_T, RING_D, PAL_STATION_FRAME);
    add_box(station_v, station_t, &nv, &nt, -BEAM_OFF, 0, 0, BEAM_T, HOLE_H, RING_D, PAL_STATION_FRAME);
    add_box(station_v, station_t, &nv, &nt,  BEAM_OFF, 0, 0, BEAM_T, HOLE_H, RING_D, PAL_STATION_FRAME);
    add_box(station_v, station_t, &nv, &nt, BEAM_OFF + U * 2, 0, U * 15 / 10, U * 13 / 10, U * 13 / 10, U * 13 / 10, PAL_STATION_HULL);
    add_box(station_v, station_t, &nv, &nt, BEAM_OFF + U, 0, U * 8 / 10, U * 9 / 10, U * 25 / 100, U * 25 / 100, PAL_STATION_HULL);
    station_m.v = station_v; station_m.nv = nv; station_m.t = station_t; station_m.nt = nt;

    nv = nt = 0; add_box(boltp_v, boltp_t, &nv, &nt, 0, 0, 0, U * 3 / 100, U * 3 / 100, U * 35 / 100, PAL_BOLT_PLAYER);
    bolt_p_m.v = boltp_v; bolt_p_m.nv = nv; bolt_p_m.t = boltp_t; bolt_p_m.nt = nt;

    nv = nt = 0; add_box(bolte_v, bolte_t, &nv, &nt, 0, 0, 0, U * 7 / 100, U * 7 / 100, U * 7 / 100, PAL_BOLT_ENEMY);
    bolt_e_m.v = bolte_v; bolt_e_m.nv = nv; bolt_e_m.t = bolte_t; bolt_e_m.nt = nt;

    nv = nt = 0; add_box(star_v, star_t, &nv, &nt, 0, 0, 0, U * 4 / 100, U * 4 / 100, U * 4 / 100, PAL_STAR);
    star_m.v = star_v; star_m.nv = nv; star_m.t = star_t; star_m.nt = nt;

    nv = nt = 0; add_box(shard_v, shard_t, &nv, &nt, 0, 0, 0, U * 8 / 100, U * 8 / 100, U * 8 / 100, PAL_SHARD);
    shard_m.v = shard_v; shard_m.nv = nv; shard_m.t = shard_t; shard_m.nt = nt;

    // A fixed starfield, scattered on shells of varying radius so both near and far
    // stars are on screen at once — the whole reason flying through it reads as flying.
    for (int i = 0; i < STAR_N; i++) {
        int a1 = (i * 167) & 1023, a2 = (i * 281 + 512) & 1023;
        int32_t r = U * (30 + (i * 37) % 160);
        star_pos[i].x = (int32_t)(((int64_t)r * g_sin[a1]) >> 15);
        star_pos[i].y = (int32_t)(((int64_t)r * g_sin[(a1 + 256) & 1023]) >> 16);
        star_pos[i].z = (int32_t)(((int64_t)r * g_sin[a2]) >> 15);
    }
}

static void palette_init(void) {
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF04040C;
    ramp(PAL_ENEMY_HULL,    0xFFE0393B);
    ramp(PAL_ENEMY_ACCENT,  0xFFFFD62E);
    ramp(PAL_BOLT_PLAYER,   0xFF33FFC7);
    ramp(PAL_BOLT_ENEMY,    0xFFFF6A1F);
    ramp(PAL_STAR,          0xFFE8F0FF);
    ramp(PAL_STATION_FRAME, 0xFF9FB4C7);
    ramp(PAL_STATION_HULL,  0xFF6E7A8C);
    ramp(PAL_SHARD,         0xFFFFA347);
    g_pal[HUD_WHITE]   = 0xFFF5F5F8;
    g_pal[HUD_DIM]     = 0xFF7A7E90;
    g_pal[HUD_GREEN]   = 0xFF33FFAA;
    g_pal[HUD_RED]     = 0xFFEF3D3D;
    g_pal[HUD_BLUE]    = 0xFF41A6F6;
    g_pal[HUD_YELLOW]  = 0xFFFFD62E;
    g_pal[HUD_PANEL]   = 0xFF14141F;
    g_pal[HUD_OVERLAY] = 0xFF0A0A14;
}

// ---- HUD ----------------------------------------------------------------------
static void setpx(int x, int y, uint8_t ci) { if (x >= 0 && x < g_fbw && y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = ci; }

static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) { char b[12]; digits(v, b); text_draw(x, y, sc, b, ci); }

// One radar blip: the contact's position, transformed into the player's YAW-only
// frame (pitch and roll left out on purpose — a scanner that tips with the nose is
// unreadable), then dropped onto a small disc capped to its edge.
static void radar_blip(int rcx, int rcy, int rr, V3 rel, int ay, uint8_t ci) {
    int32_t dx = rel.x, dy = rel.y, dz = rel.z; (void)dy;
    g3d_unrot(&dx, &dy, &dz, 0, ay, 0);
    int32_t sx = (int32_t)(((int64_t)dx * rr) / RADAR_RANGE);
    int32_t sy = (int32_t)(((int64_t)dz * rr) / RADAR_RANGE);
    if (sx > rr) sx = rr; if (sx < -rr) sx = -rr;
    if (sy > rr) sy = rr; if (sy < -rr) sy = -rr;
    int px = rcx + (int)sx, py = rcy - (int)sy;          // forward (+z) reads as UP
    setpx(px, py, ci); setpx(px + 1, py, ci); setpx(px, py + 1, ci); setpx(px + 1, py + 1, ci);
}

static void draw_hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;

    text_draw(6 * sc, hud_top(), sc, "LIVES", HUD_WHITE);
    num(6 * sc + 34 * sc, hud_top(), sc, g_lives, HUD_GREEN);
    text_draw(6 * sc, hud_top() + 9 * sc, sc, "SCORE", HUD_WHITE);
    num(6 * sc + 34 * sc, hud_top() + 9 * sc, sc, g_score, HUD_YELLOW);

    // crosshair
    int cx = g_fbw / 2, cy = g_fbh / 2, cl = 4 * sc;
    for (int i = -cl; i <= cl; i++) { setpx(cx + i, cy, HUD_GREEN); setpx(cx, cy + i, HUD_GREEN); }

    // radar: a small scanner, top right — player at centre, contacts as dots
    int rr = 26 * sc, rx0 = g_fbw - rr * 2 - 8 * sc, ry0 = 8 * sc;
    int rcx = rx0 + rr, rcy = ry0 + rr;
    for (int y = -rr; y <= rr; y++)
        for (int x = -rr; x <= rr; x++)
            if (x * x + y * y <= rr * rr) setpx(rcx + x, rcy + y, HUD_PANEL);
    setpx(rcx, rcy, HUD_WHITE); setpx(rcx + 1, rcy, HUD_WHITE); setpx(rcx, rcy + 1, HUD_WHITE);
    for (int e = 0; e < ENEMY_N; e++) {
        if (!g_enemy[e].alive) continue;
        V3 ep; enemy_pos(e, g_frame, &ep);
        radar_blip(rcx, rcy, rr, (V3){ ep.x - g_pos.x, ep.y - g_pos.y, ep.z - g_pos.z }, g_ay, HUD_RED);
    }
    radar_blip(rcx, rcy, rr,
               (V3){ STATION_POS.x - g_pos.x, STATION_POS.y - g_pos.y, STATION_POS.z - g_pos.z },
               g_ay, HUD_BLUE);

    if (g_dock_flash_t > 0)
        text_draw(g_fbw / 2 - text_width("DOCKED +500", sc * 2) / 2, g_fbh / 2 - 40 * sc, sc * 2, "DOCKED +500", HUD_YELLOW);

    if (g_flash_t > 0) {
        int bt = 3 * sc + 2;
        for (int y = 0; y < g_fbh; y++) for (int x = 0; x < bt; x++) { setpx(x, y, HUD_RED); setpx(g_fbw - 1 - x, y, HUD_RED); }
        for (int x = 0; x < g_fbw; x++) for (int y = 0; y < bt; y++) { setpx(x, y, HUD_RED); setpx(x, g_fbh - 1 - y, HUD_RED); }
    }

    if (g_over) {
        int oy0 = g_fbh / 2 - 30 * sc, oy1 = g_fbh / 2 + 34 * sc;
        for (int y = oy0; y < oy1; y++) for (int x = 0; x < g_fbw; x++) setpx(x, y, HUD_OVERLAY);
        text_draw(cx - text_width("GAME OVER", sc * 3) / 2, g_fbh / 2 - 22 * sc, sc * 3, "GAME OVER", HUD_RED);
        char b[12]; digits(g_score, b);
        int w = text_width("SCORE", sc * 2) + 6 * sc * 2 + text_width(b, sc * 2);
        text_draw(cx - w / 2, g_fbh / 2 + 4 * sc, sc * 2, "SCORE", HUD_WHITE);
        num(cx - w / 2 + text_width("SCORE", sc * 2) + 6 * sc * 2, g_fbh / 2 + 4 * sc, sc * 2, g_score, HUD_YELLOW);
        text_draw(cx - text_width("SPACE TO RESTART", sc) / 2, g_fbh / 2 + 24 * sc, sc, "SPACE TO RESTART", HUD_DIM);
    }
}

static void draw(void) {
    fb_clear(0);

    static Inst inst[MAXINST];
    int n = 0;

    for (int i = 0; i < STAR_N; i++) {
        inst[n].m = &star_m; inst[n].pos = star_pos[i]; inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
    }

    int spin = (int)((g_frame * STATION_SPIN_RATE) & 1023);
    inst[n].m = &station_m; inst[n].pos = STATION_POS; inst[n].ax = 0; inst[n].ay = spin; inst[n].az = 0; inst[n].scale = U; n++;

    for (int i = 0; i < ENEMY_N; i++) {
        if (!g_enemy[i].alive) continue;
        V3 ep; enemy_pos(i, g_frame, &ep);
        int ax, ay, az; enemy_rot(i, g_frame, &ax, &ay, &az);
        inst[n].m = &enemy_m; inst[n].pos = ep; inst[n].ax = ax; inst[n].ay = ay; inst[n].az = az; inst[n].scale = U; n++;
    }

    for (int k = 0; k < MAX_PBOLT; k++) {
        if (!g_pbolt[k].active) continue;
        Bolt *b = &g_pbolt[k];
        inst[n].m = &bolt_p_m; inst[n].pos = (V3){ b->cx, b->cy, b->cz };
        inst[n].ax = b->ax; inst[n].ay = b->ay; inst[n].az = 0; inst[n].scale = U; n++;
    }
    for (int k = 0; k < MAX_EBOLT; k++) {
        if (!g_ebolt[k].active) continue;
        EBolt *bo = &g_ebolt[k];
        inst[n].m = &bolt_e_m; inst[n].pos = (V3){ bo->x, bo->y, bo->z };
        inst[n].ax = (int)((g_frame * 17) & 1023); inst[n].ay = (int)((g_frame * 23) & 1023); inst[n].az = 0;
        inst[n].scale = U; n++;
    }
    for (int k = 0; k < MAX_SHARD; k++) {
        if (!g_shard[k].active) continue;
        Shard *s = &g_shard[k];
        int32_t sc = (int32_t)(((int64_t)U * s->life) / SHARD_LIFE); if (sc < U / 6) sc = U / 6;
        inst[n].m = &shard_m; inst[n].pos = (V3){ s->x, s->y, s->z };
        inst[n].ax = (int)(((SHARD_LIFE - s->life) * 40) & 1023);
        inst[n].ay = (int)(((SHARD_LIFE - s->life) * 61) & 1023);
        inst[n].az = 0; inst[n].scale = sc; n++;
    }

    // camera shake: a jolt on hit, applied only to the render — the player's own
    // ax/ay stay clean so the sim never has to know the screen jittered.
    int jx = 0, jy = 0;
    if (g_shake_t > 0) {
        jx = (g_sin[(g_frame * 131) & 1023] * 4) >> 15;
        jy = (g_sin[(g_frame * 197) & 1023] * 4) >> 15;
    }
    Cam cam = { g_pos, g_ax + jy, g_ay + jx, g_az };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    draw_hud();
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ULL;
    h = h * 1000003 ^ (uint32_t)g_pos.x; h = h * 1000003 ^ (uint32_t)g_pos.y; h = h * 1000003 ^ (uint32_t)g_pos.z;
    h = h * 1000003 ^ (uint32_t)g_ax; h = h * 1000003 ^ (uint32_t)g_ay; h = h * 1000003 ^ (uint32_t)g_az;
    h = h * 1000003 ^ (uint32_t)g_score; h = h * 1000003 ^ (uint32_t)g_lives; h = h * 1000003 ^ g_frame;
    for (int i = 0; i < ENEMY_N; i++) h = h * 1000003 ^ (uint32_t)(g_enemy[i].alive + g_enemy[i].respawn_t * 2);
    for (int i = 0; i < MAX_PBOLT; i++) h = h * 1000003 ^ (uint32_t)g_pbolt[i].active;
    for (int i = 0; i < MAX_EBOLT; i++) h = h * 1000003 ^ (uint32_t)g_ebolt[i].active;
    return h;
}

static void init(void) {
    tables_init();
    build_meshes();
    palette_init();
    reset_game();
}

const Game game_elite = { "elite", init, tick, audio, draw, checksum };
