// maze.c — first-person maze + chaser. 3D Monster Maze.
//
// The camera stands INSIDE the grid, at eye height, looking straight down a corridor —
// receding walls in perspective, not a plan seen from above. Turning is a quarter-turn of
// the camera's own yaw; walking is a slide from one cell centre to the next. Nothing here
// is 2D dressed up: the corridors are boxes, the monster is boxes, the exit is a boxes,
// and the only thing standing in for "you" is where the lens is.
//
// The monster hunts by BFS over the same wall grid the player collides with — one source
// of truth for "can I get there," read by both the player's footsteps and the monster's
// path. When it shares your corridor with a clear line of sight, the HUD says so; the
// dread itself is just perspective doing its job — the same box gets bigger, faster, the
// nearer it gets, because that is what boxes do in a real lens.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
static int32_t mulU(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }

// ---- the maze -----------------------------------------------------------------
// A perfect maze (recursive backtracker), baked at build time so every run starts from
// the same corridors — determinism the same way every other rule here is determinism.
#define MW 19
#define MH 15
static const char *const MAZE[MH] = {
    "###################",
    "#...#...........#.#",
    "###.#####.###.#.#.#",
    "#.#.#...#...#.#...#",
    "#.#.#.#.#####.#####",
    "#.#...#...#...#...#",
    "#.#######.#.###.#.#",
    "#.......#.#.....#.#",
    "#####.#.#.#######.#",
    "#.....#.#.......#.#",
    "#.#############.#.#",
    "#.......#.....#.#.#",
    "#.#####.#.###.#.#.#",
    "#.....#.....#.....#",
    "###################",
};
#define START_COL 1
#define START_ROW 1
#define EXIT_COL 17
#define EXIT_ROW 13
#define MON_COL  9
#define MON_ROW  7

static int is_wall(int c, int r) {
    if (c < 0 || c >= MW || r < 0 || r >= MH) return 1;
    return MAZE[r][c] == '#';
}

// A straight look down a corridor: only true along a shared row or column, and only if
// nothing walled crosses it. It's the same test a flashlight would fail.
static int has_los(int c0, int r0, int c1, int r1) {
    if (c0 == c1 && r0 != r1) {
        int step = (r1 > r0) ? 1 : -1;
        for (int r = r0 + step; r != r1; r += step) if (is_wall(c0, r)) return 0;
        return 1;
    }
    if (r0 == r1 && c0 != c1) {
        int step = (c1 > c0) ? 1 : -1;
        for (int c = c0 + step; c != c1; c += step) if (is_wall(c, r0)) return 0;
        return 1;
    }
    return c0 == c1 && r0 == r1;
}

// BFS over the wall grid: the monster's one source of pathing truth, walked fresh every
// time it needs a new cell so it always closes on wherever the player actually is.
static int bfs_step(int fc, int fr, int tc, int tr, int *nc, int *nr) {
    static int8_t from_dir[MH][MW];
    static int16_t qc[MW * MH], qr[MW * MH];
    static const int DC[4] = { 1, 0, -1, 0 }, DR[4] = { 0, 1, 0, -1 };
    for (int r = 0; r < MH; r++) for (int c = 0; c < MW; c++) from_dir[r][c] = -1;
    int qh = 0, qt = 0;
    qc[qt] = (int16_t)fc; qr[qt] = (int16_t)fr; qt++;
    from_dir[fr][fc] = -2;
    int found = (fc == tc && fr == tr);
    while (qh < qt && !found) {
        int c = qc[qh], r = qr[qh]; qh++;
        for (int d = 0; d < 4; d++) {
            int nc2 = c + DC[d], nr2 = r + DR[d];
            if (is_wall(nc2, nr2) || from_dir[nr2][nc2] != -1) continue;
            from_dir[nr2][nc2] = (int8_t)d;
            qc[qt] = (int16_t)nc2; qr[qt] = (int16_t)nr2; qt++;
            if (nc2 == tc && nr2 == tr) { found = 1; break; }
        }
    }
    if (!found || (fc == tc && fr == tr)) return 0;
    int c = tc, r = tr, firstd = -1, guard = 0;
    while (!(c == fc && r == fr) && guard++ < MW * MH) {
        int d = from_dir[r][c];
        firstd = d;
        c -= DC[d]; r -= DR[d];
    }
    if (firstd < 0) return 0;
    *nc = fc + DC[firstd]; *nr = fr + DR[firstd];
    return 1;
}

// dir 0..3 -> which way that faces in world XZ, read out of the SAME table g3d rotates
// the camera with, so "the corridor the player is looking down" and "the corridor the
// engine draws" never disagree.
static void dir_delta(int dir, int *dc, int *dr) {
    int a = dir * 256;
    int32_t sx = g_sin[a & 1023];
    int32_t sz = g_sin[(a + 256) & 1023];
    *dc = sx > 0 ? 1 : (sx < 0 ? -1 : 0);
    *dr = sz > 0 ? 1 : (sz < 0 ? -1 : 0);
}

// ---- world scale ----------------------------------------------------------------
#define CELL   (2 * U)
#define WALLH  (3 * U)
#define EYE    (U + U / 2)
#define FLOORH (U / 16)
#define CEILH  (U / 16)
#define PSTEP_F 8      // frames to slide one cell
#define PTURN_F 8      // frames to swing 90 degrees (32 * 8 = 256)
#define MSTEP_F 12     // the monster is slower per cell than the player

// ---- palette ----------------------------------------------------------------
#define WALL_CI       8
#define FLOORA_CI    16
#define FLOORB_CI    24
#define CEIL_CI      32
#define EXITFLOOR_CI 40
#define EXITMARK_CI  48
#define BODY_CI      56
#define HEAD_CI      64
#define EYE_CI       72
#define TX_WHITE 90
#define TX_RED   91
#define TX_GOLD  92
#define TX_GREEN 93
#define TX_DIM   94

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 40 + i * 30;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
// A ramp with a raised floor: even the darkest shade still glows. For eyes.
static void ramp_glow(int base, uint32_t rgb, int minm) {
    for (int i = 0; i < 8; i++) {
        int m = minm + i * (255 - minm) / 8; if (m > 255) m = 255;
        int r = (int)((rgb >> 16) & 255) * m / 255;
        int g = (int)((rgb >> 8) & 255) * m / 255;
        int b = (int)(rgb & 255) * m / 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// ---- runtime meshes -----------------------------------------------------------
// One box builder, several instances of it. Inst carries a uniform scale and a box needs
// three independent extents, so — baking the size into the mesh — the size is baked into the
// vertices once, at init, and every wall / floor tile / ceiling slab reuses that ONE mesh
// through many Insts. 291 placements, 8 meshes.
static const int8_t BOXVP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                     {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t BOXFQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t BOXFN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static void build_box(V3 *v, Tri *t, int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    for (int i = 0; i < 8; i++) {
        v[i].x = BOXVP[i][0] * sx; v[i].y = BOXVP[i][1] * sy; v[i].z = BOXVP[i][2] * sz;
    }
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *tt = &t[f * 2 + k];
            tt->a = BOXFQ[f][0]; tt->b = BOXFQ[f][1 + k]; tt->c = BOXFQ[f][2 + k];
            tt->ci = ci;
            tt->nx = (int16_t)(BOXFN[f][0] * 32767);
            tt->ny = (int16_t)(BOXFN[f][1] * 32767);
            tt->nz = (int16_t)(BOXFN[f][2] * 32767);
        }
}

static V3 wallV[8], floorAV[8], floorBV[8], ceilV[8], exitfV[8], markV[8], bodyV[8], headV[8], eyeV[8];
static Tri wallT[12], floorAT[12], floorBT[12], ceilT[12], exitfT[12], markT[12], bodyT[12], headT[12], eyeT[12];
static Mesh wallM, floorAM, floorBM, ceilM, exitfM, markM, bodyM, headM, eyeM;

static void build_meshes(void) {
    build_box(wallV,  wallT,  CELL / 2, WALLH / 2, CELL / 2, WALL_CI);      wallM  = (Mesh){ wallV,  8, wallT,  12 };
    build_box(floorAV,floorAT,CELL / 2, FLOORH / 2, CELL / 2, FLOORA_CI);   floorAM= (Mesh){ floorAV,8, floorAT,12 };
    build_box(floorBV,floorBT,CELL / 2, FLOORH / 2, CELL / 2, FLOORB_CI);   floorBM= (Mesh){ floorBV,8, floorBT,12 };
    build_box(exitfV, exitfT, CELL / 2, FLOORH / 2, CELL / 2, EXITFLOOR_CI);exitfM = (Mesh){ exitfV, 8, exitfT, 12 };
    build_box(ceilV,  ceilT,  (MW * CELL) / 2, CEILH / 2, (MH * CELL) / 2, CEIL_CI); ceilM = (Mesh){ ceilV, 8, ceilT, 12 };
    build_box(markV,  markT,  U / 5, U / 2, U / 5, EXITMARK_CI);           markM  = (Mesh){ markV,  8, markT,  12 };
    build_box(bodyV,  bodyT,  U * 3 / 8, U * 5 / 8, U * 3 / 8, BODY_CI);   bodyM  = (Mesh){ bodyV,  8, bodyT,  12 };
    build_box(headV,  headT,  U / 4, U / 4, U / 4, HEAD_CI);              headM  = (Mesh){ headV,  8, headT,  12 };
    build_box(eyeV,   eyeT,   U / 20, U / 20, U / 20, EYE_CI);            eyeM   = (Mesh){ eyeV,   8, eyeT,   12 };
}

// ---- game state ---------------------------------------------------------------
static int32_t g_pwx, g_pwz, g_pang;             // player: continuous world pos + yaw
static int g_pcol, g_prow, g_pdir;               // player: logical cell + facing
static uint8_t g_pmoving, g_pturning;
static int g_pmoveT, g_pturnT, g_turnStep;
static int32_t g_pfromX, g_pfromZ, g_ptoX, g_ptoZ;

static int32_t g_mwx, g_mwz, g_mang;             // monster
static int g_mcol, g_mrow;
static uint8_t g_mmoving;
static int g_mmoveT;
static int32_t g_mfromX, g_mfromZ, g_mtoX, g_mtoZ;

static int g_lives, g_state;                     // 0 play, 1 win, 2 game over
static uint32_t g_frame;
static uint64_t g_checksum;
static uint8_t g_events;
#define EV_STEP   1
#define EV_TURN   2
#define EV_CAUGHT 4
#define EV_WIN    8
#define EV_HUNT   16
#define EV_BUMP   32

static uint8_t g_prevX_edge, g_prevJump;
static uint8_t g_wasLOS;

static void reset_game(void) {
    g_pcol = START_COL; g_prow = START_ROW; g_pdir = 0; g_pang = 0;
    g_pwx = START_COL * CELL; g_pwz = START_ROW * CELL;
    g_pmoving = 0; g_pturning = 0; g_pmoveT = 0; g_pturnT = 0;

    g_mcol = MON_COL; g_mrow = MON_ROW;
    g_mwx = MON_COL * CELL; g_mwz = MON_ROW * CELL;
    g_mmoving = 0; g_mmoveT = 0; g_mang = 0;

    g_lives = 3; g_state = 0;
    g_wasLOS = 0;
}

static void init(void) {
    tables_init();
    build_meshes();
    g_frame = 0; g_checksum = 0; g_events = 0;
    g_prevX_edge = 0; g_prevJump = 0;
    reset_game();

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0A0A14;                 // the dark past the torchlight
    ramp(WALL_CI,      0xFF5A6B8C);        // cold stone
    ramp(FLOORA_CI,    0xFF3A3448);
    ramp(FLOORB_CI,    0xFF2A2536);
    ramp(CEIL_CI,      0xFF1A1624);
    ramp(EXITFLOOR_CI, 0xFF2ECC71);        // the way out, unmistakably green
    ramp(EXITMARK_CI,  0xFFFFD24A);        // a floating gold beacon over it
    ramp(BODY_CI,      0xFF3A0A12);        // the thing that hunts you
    ramp(HEAD_CI,      0xFF200610);
    ramp_glow(EYE_CI,  0xFFFF2418, 210);   // eyes that never go fully dark
    g_pal[TX_WHITE] = 0xFFF5F5F8;
    g_pal[TX_RED]   = 0xFFEF4444;
    g_pal[TX_GOLD]  = 0xFFFFD62E;
    g_pal[TX_GREEN] = 0xFF33C759;
    g_pal[TX_DIM]   = 0xFF6B6B7A;
}

static void tick(const Input in[2]) {
    g_events = 0;
    g_frame++;

    Input p;
    int8_t sx = (int8_t)(in[0].x + in[1].x); if (sx > 1) sx = 1; if (sx < -1) sx = -1;
    int8_t sy = (int8_t)(in[0].y + in[1].y); if (sy > 1) sy = 1; if (sy < -1) sy = -1;
    p.x = sx; p.y = sy;
    p.jump = (uint8_t)(in[0].jump || in[1].jump);
    p.act  = (uint8_t)(in[0].act  || in[1].act);

    if (g_state != 0) {
        // Frozen but for the restart edge. Space, once, not held.
        if (p.jump && !g_prevJump) reset_game();
        g_prevJump = p.jump;
        g_prevX_edge = (uint8_t)(p.x != 0);
        g_checksum = g_checksum * 31 + (uint32_t)g_state + g_lives;
        return;
    }
    g_prevJump = p.jump;

    // ---- player: turning, debounced (one quarter turn per press) ----
    if (!g_pturning && !g_pmoving) {
        if (p.x != 0 && !g_prevX_edge) {
            int way = p.x > 0 ? 1 : -1;
            g_pdir = (g_pdir + (way > 0 ? 1 : 3)) & 3;
            g_turnStep = way * 32;
            g_pturning = 1; g_pturnT = 0;
            g_events |= EV_TURN;
        }
    }
    g_prevX_edge = (uint8_t)(p.x != 0);

    if (g_pturning) {
        g_pang = (g_pang + g_turnStep) & 1023;
        g_pturnT++;
        if (g_pturnT >= PTURN_F) { g_pturning = 0; g_pang = (int32_t)(g_pdir * 256); }
    }

    // ---- player: walk, cell by cell, held keys keep advancing ----
    if (!g_pturning && !g_pmoving && p.y != 0) {
        int dc, dr; dir_delta(g_pdir, &dc, &dr);
        int step = p.y > 0 ? 1 : -1;
        int nc = g_pcol + dc * step, nr = g_prow + dr * step;
        if (!is_wall(nc, nr)) {
            g_pfromX = g_pwx; g_pfromZ = g_pwz;
            g_pcol = nc; g_prow = nr;
            g_ptoX = nc * CELL; g_ptoZ = nr * CELL;
            g_pmoving = 1; g_pmoveT = 0;
            g_events |= EV_STEP;
        } else {
            g_events |= EV_BUMP;
        }
    }
    if (g_pmoving) {
        g_pmoveT++;
        int32_t frac = (int32_t)(((int64_t)g_pmoveT << 16) / PSTEP_F);
        g_pwx = g_pfromX + mulU(g_ptoX - g_pfromX, frac);
        g_pwz = g_pfromZ + mulU(g_ptoZ - g_pfromZ, frac);
        if (g_pmoveT >= PSTEP_F) { g_pmoving = 0; g_pwx = g_ptoX; g_pwz = g_ptoZ; }
    }

    // ---- monster: BFS-hunt, one cell at a time, a little slower than the player ----
    if (!g_mmoving) {
        int nc, nr;
        if (bfs_step(g_mcol, g_mrow, g_pcol, g_prow, &nc, &nr)) {
            int dc = nc - g_mcol, dr = nr - g_mrow;
            g_mang = dc > 0 ? 0 : dc < 0 ? 512 : (dr > 0 ? 256 : 768);
            g_mfromX = g_mwx; g_mfromZ = g_mwz;
            g_mcol = nc; g_mrow = nr;
            g_mtoX = nc * CELL; g_mtoZ = nr * CELL;
            g_mmoving = 1; g_mmoveT = 0;
        }
    }
    if (g_mmoving) {
        g_mmoveT++;
        int32_t frac = (int32_t)(((int64_t)g_mmoveT << 16) / MSTEP_F);
        g_mwx = g_mfromX + mulU(g_mtoX - g_mfromX, frac);
        g_mwz = g_mfromZ + mulU(g_mtoZ - g_mfromZ, frac);
        if (g_mmoveT >= MSTEP_F) { g_mmoving = 0; g_mwx = g_mtoX; g_mwz = g_mtoZ; }
    }

    // ---- dread: a shared corridor, nothing walled between ----
    int los = has_los(g_pcol, g_prow, g_mcol, g_mrow);
    if (los && !g_wasLOS) g_events |= EV_HUNT;
    g_wasLOS = (uint8_t)los;

    // ---- catch / win ----
    if (g_mcol == g_pcol && g_mrow == g_prow) {
        g_events |= EV_CAUGHT;
        g_lives--;
        if (g_lives <= 0) { g_state = 2; }
        else {
            int pc = g_pcol, pr = g_prow;
            g_pcol = START_COL; g_prow = START_ROW; g_pdir = 0; g_pang = 0;
            g_pwx = START_COL * CELL; g_pwz = START_ROW * CELL;
            g_pmoving = 0; g_pturning = 0;
            g_mcol = MON_COL; g_mrow = MON_ROW;
            g_mwx = MON_COL * CELL; g_mwz = MON_ROW * CELL;
            g_mmoving = 0;
            (void)pc; (void)pr;
        }
    } else if (g_pcol == EXIT_COL && g_prow == EXIT_ROW) {
        g_state = 1;
        g_events |= EV_WIN;
    }

    g_checksum = g_checksum * 31 + (uint32_t)g_pwx + (uint32_t)g_pwz + (uint32_t)g_pang
               + (uint32_t)g_mwx * 7u + (uint32_t)g_mwz * 7u
               + (uint32_t)g_pcol * 13u + (uint32_t)g_prow * 17u
               + (uint32_t)g_lives * 101u + (uint32_t)g_state * 1009u;
}

static void audio(void) {
    if (g_events & EV_STEP)   synth_note(NCHAN - 1, 4, 38, 60);
    if (g_events & EV_TURN)   synth_note(NCHAN - 1, 3, 66, 40);
    if (g_events & EV_BUMP)   synth_note(NCHAN - 1, 4, 30, 90);
    if (g_events & EV_HUNT)   synth_note(NCHAN - 1, 3, 28, 150);
    if (g_events & EV_CAUGHT) synth_note(NCHAN - 1, 5, 32, 200);
    if (g_events & EV_WIN)    synth_note(NCHAN - 1, 5, 76, 190);
}

// ---- draw -----------------------------------------------------------------
#define MAXINST 512
static Inst g_inst[MAXINST];

static void draw(void) {
    fb_clear(0);
    int n = 0;

    for (int r = 0; r < MH; r++) {
        for (int c = 0; c < MW; c++) {
            if (n >= MAXINST - 8) break;
            if (is_wall(c, r)) {
                g_inst[n].m = &wallM;
                g_inst[n].pos = (V3){ c * CELL, WALLH / 2, r * CELL };
                g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0;
                g_inst[n].scale = 1 << 16;
                n++;
            } else {
                const Mesh *fm = (c == EXIT_COL && r == EXIT_ROW) ? &exitfM
                                : (((c + r) & 1) ? &floorBM : &floorAM);
                g_inst[n].m = fm;
                g_inst[n].pos = (V3){ c * CELL, -FLOORH / 2, r * CELL };
                g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0;
                g_inst[n].scale = 1 << 16;
                n++;
            }
        }
    }

    // The ceiling: one slab over the whole maze, because 285 of them would be the same
    // triangle 285 times.
    g_inst[n].m = &ceilM;
    g_inst[n].pos = (V3){ (MW - 1) * CELL / 2, WALLH + CEILH / 2, (MH - 1) * CELL / 2 };
    g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0;
    g_inst[n].scale = 1 << 16;
    n++;

    // A beacon over the exit: bobbing and spinning, so it reads from three corridors
    // away long before its cell does.
    int32_t bob = g_sin[(g_frame * 6) & 1023] / 6;
    g_inst[n].m = &markM;
    g_inst[n].pos = (V3){ EXIT_COL * CELL, WALLH * 3 / 5 + bob, EXIT_ROW * CELL };
    g_inst[n].ax = 0; g_inst[n].ay = (int)((g_frame * 5) & 1023); g_inst[n].az = 0;
    g_inst[n].scale = 1 << 16;
    n++;

    // The monster: a body, a head, two eyes that never go dark. A small bob so a
    // motionless frame doesn't read as a prop.
    int32_t mbob = g_sin[(g_frame * 14) & 1023] / 10;
    g_inst[n].m = &bodyM;
    g_inst[n].pos = (V3){ g_mwx, U * 5 / 8 + mbob, g_mwz };
    g_inst[n].ax = 0; g_inst[n].ay = g_mang; g_inst[n].az = 0;
    g_inst[n].scale = 1 << 16;
    n++;
    g_inst[n].m = &headM;
    g_inst[n].pos = (V3){ g_mwx, U * 5 / 4 + mbob, g_mwz };
    g_inst[n].ax = 0; g_inst[n].ay = g_mang; g_inst[n].az = 0;
    g_inst[n].scale = 1 << 16;
    n++;
    {
        int32_t s = g_sin[g_mang & 1023], cz = g_sin[(g_mang + 256) & 1023];
        int32_t ex = mulU(U * 3 / 20, s), ez = mulU(U * 3 / 20, cz);
        int32_t ox = mulU(U / 5, cz), oz = -mulU(U / 5, s);   // sideways offset, perp to facing
        g_inst[n].m = &eyeM;
        g_inst[n].pos = (V3){ g_mwx + ex + ox, U * 5 / 4 + mbob + U / 10, g_mwz + ez + oz };
        g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0; g_inst[n].scale = 1 << 16;
        n++;
        g_inst[n].m = &eyeM;
        g_inst[n].pos = (V3){ g_mwx + ex - ox, U * 5 / 4 + mbob + U / 10, g_mwz + ez - oz };
        g_inst[n].ax = g_inst[n].ay = g_inst[n].az = 0; g_inst[n].scale = 1 << 16;
        n++;
    }

    Cam cam = { { g_pwx, EYE, g_pwz }, 0, (int)g_pang, 0 };
    g3d_scene(g_inst, n, &cam, 0, 0, 0);

    // ---- HUD ----
    int s = g_fbh / 180; if (s < 1) s = 1;
    char lv[2] = { (char)('0' + (g_lives < 0 ? 0 : g_lives)), 0 };
    text_draw(4 * s, hud_top(), s, "LIVES", TX_WHITE);
    text_draw(4 * s + text_width("LIVES ", s), hud_top(), s, lv, TX_RED);

    if (g_wasLOS && g_state == 0 && ((g_frame / 8) & 1)) {
        const char *msg = "IT SEES YOU";
        text_draw(g_fbw / 2 - text_width(msg, s) / 2, hud_top(), s, msg, TX_RED);
    }

    text_draw(g_fbw / 2 - text_width("L R TURN   U D WALK", s) / 2, g_fbh - 10 * s, s,
               "L R TURN   U D WALK", TX_DIM);

    if (g_state == 1) {
        const char *m1 = "YOU ESCAPED";
        const char *m2 = "SPACE TO PLAY AGAIN";
        text_draw(g_fbw / 2 - text_width(m1, s * 2) / 2, g_fbh / 2 - 12 * s, s * 2, m1, TX_GREEN);
        text_draw(g_fbw / 2 - text_width(m2, s) / 2, g_fbh / 2 + 8 * s, s, m2, TX_WHITE);
    } else if (g_state == 2) {
        const char *m1 = "CAUGHT";
        const char *m2 = "SPACE TO RETRY";
        text_draw(g_fbw / 2 - text_width(m1, s * 2) / 2, g_fbh / 2 - 12 * s, s * 2, m1, TX_RED);
        text_draw(g_fbw / 2 - text_width(m2, s) / 2, g_fbh / 2 + 8 * s, s, m2, TX_WHITE);
    }
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_maze = { "maze", init, tick, audio, draw, checksum };
