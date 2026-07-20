// ribbon.c — Ribbon Road in the Void. A narrow checkered strip of road, floating in
// empty space, undulating gently as it recedes to the horizon. A ball auto-rolls along
// it, bouncing on every landing; you only ever steer sideways and time a jump.
//
// The world doesn't move under the ball the way a treadmill does — the ball's Z is
// fixed at the origin and the RIBBON scrolls toward the camera, tile by tile, exactly
// the trick the engine already trusts ("How far the world has come at us"). It buys a
// camera that never has to chase a moving target in Z: it only has to lean with the
// ball's X and bob with its Y, which is what makes it read as a chase camera glued to
// something alive instead of a camera doing arithmetic.
//
// Every tile is the same box, in three colours (two checker shades, one hazard amber):
// one shared vertex buffer, three baked triangle sets, the same trick the console shell uses for
// 27 cubies that only ever differ in colour. A tile that's a GAP is not drawn at all —
// missing geometry is the honest way to say "there is no floor here," and the rails
// either side of the ribbon stop exactly where the gap starts, so the void is never
// ambiguous about where it begins.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)

// ---- geometry -----------------------------------------------------------------
#define TILE_LEN    (U * 3)
#define TILE_HALFZ  (TILE_LEN / 2)
#define TILE_HALFY  (U / 10)
#define RIB_HALF    (U * 11 / 10)         // half-width of the ribbon
#define BALL_R      (U / 2)

// The ribbon's own gentle hills — it floats, it doesn't lie flat. A long, slow sine so
// it reads as terrain and not as noise: about one crest every fifty-odd tiles.
#define HILL_AMP    (U * 7 / 5)
#define HILL_STEP   18

#define RAIL_HALFX  (U / 12)
#define RAIL_HALFY  (U / 6)

// ---- physics --------------------------------------------------------------------
#define GRAV        (U * 3 / 1000)
#define BOUNCE_V    (U * 3 / 100)         // the idle bounce: too short to clear a gap
#define JUMP_V      (U * 12 / 100)        // a deliberate Space: plenty of hang time
#define START_SPEED (U * 5 / 100)
#define MAX_SPEED   (U * 12 / 100)
#define SPEED_RAMP  3
#define STEER_ACCEL (U * 5 / 100)
#define STEER_MAXV  (U * 8 / 100)
#define FALL_Y      (-(U * 5))            // well below any legal surface height

#define CAM_HEIGHT  (U * 3)
#define CAM_DIST    (U * 6)

#define NVIEW       18                    // tiles rendered ahead of the ball
#define NSTAR       20
#define STAR_SPAN   (U * 30)
#define STAR_CYCLE  (U * 260)
#define STAR_PARDIV 5                     // stars drift slower than the road: parallax

// ---- palette layout ---------------------------------------------------------------
#define P_CHKA 8     // checker light
#define P_CHKB 16    // checker dark
#define P_WARN 24    // hazard amber, one tile before every gap
#define P_RAIL 32    // the edge rails, glowing
#define P_BALL 40    // the ball
#define P_HURT 48    // the ball, flashing after a fall
#define P_STAR 56    // distant debris
#define P_TXT  64
#define P_TXT2 65
#define P_TXT3 66

enum { T_SAFE, T_WARN, T_GAP };
enum { EV_JUMP = 1, EV_BOUNCE = 2, EV_FALL = 4, EV_OVER = 8 };

// ---- deterministic track --------------------------------------------------------
// lowbias32: shift, multiply, shift, multiply, shift. A Weyl-sequence hash (i * const)
// is regular in its low bits; this finaliser folds the high bits back down so nearby
// tile indices don't fall into a visible period.
static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Gaps only ever land on idx % 3 == 0 — a structural minimum spacing of three tiles,
// which guarantees no two gaps are ever adjacent and every gap has a warning tile and
// a landing tile right next to it. No sequential state, no precomputed table: the
// track is a pure function of where you are, same as everything else here.
static int is_gap_tile(int64_t idx) {
    if (idx < 8) return 0;                              // a tutorial stretch, always solid
    uint32_t h = mix32((uint32_t)idx ^ 0xA53Fu);
    int64_t diff = idx > 500 ? 500 : idx;
    int threshold = 15 + (int)(diff * 30 / 500);         // 15%..45% as the run goes on
    return (int)(h % 100) < threshold;
}
static int tile_kind(int64_t idx) {
    if (idx % 3 == 0) return is_gap_tile(idx) ? T_GAP : T_SAFE;
    if (idx % 3 == 2) return is_gap_tile(idx + 1) ? T_WARN : T_SAFE;   // telegraph the gap ahead
    return T_SAFE;
}
static int32_t hill_y(int64_t idx) {
    int ang = (int)((idx * HILL_STEP) & 1023);
    return (int32_t)(((int64_t)HILL_AMP * g_sin[ang]) >> 15);
}
// The tile's top surface at a fractional position within it — interpolated, so the
// ball's ride over the hill is smooth instead of stair-stepping once per tile.
static int32_t track_top(int64_t idx, int32_t fracNum) {
    int32_t y0 = hill_y(idx), y1 = hill_y(idx + 1);
    int32_t y = y0 + (int32_t)(((int64_t)(y1 - y0) * fracNum) / TILE_LEN);
    return y + TILE_HALFY;
}

// ---- runtime meshes ---------------------------------------------------------------
static const int8_t  BOX_VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                      {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t BOX_FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t  BOX_FN[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static void fill_box_verts(V3 *v, int32_t sx, int32_t sy, int32_t sz) {
    for (int i = 0; i < 8; i++) {
        v[i].x = BOX_VP[i][0] * sx; v[i].y = BOX_VP[i][1] * sy; v[i].z = BOX_VP[i][2] * sz;
    }
}
static void fill_box_tris(Tri *t, uint8_t ci) {
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *o = &t[f * 2 + k];
            o->a = BOX_FQ[f][0]; o->b = BOX_FQ[f][1 + k]; o->c = BOX_FQ[f][2 + k]; o->ci = ci;
            o->nx = (int16_t)(BOX_FN[f][0] * 32767);
            o->ny = (int16_t)(BOX_FN[f][1] * 32767);
            o->nz = (int16_t)(BOX_FN[f][2] * 32767);
        }
}

// The tile: one box shape, three baked colourings sharing the same eight verts.
static V3   tile_v[8];
static Tri  tile_t[3][12];
static Mesh tile_m[3];
static void build_tiles(void) {
    fill_box_verts(tile_v, RIB_HALF, TILE_HALFY, TILE_HALFZ);
    static const uint8_t cis[3] = { P_CHKA, P_CHKB, P_WARN };
    for (int v = 0; v < 3; v++) {
        fill_box_tris(tile_t[v], cis[v]);
        tile_m[v].v = tile_v; tile_m[v].nv = 8; tile_m[v].t = tile_t[v]; tile_m[v].nt = 12;
    }
}

static V3 rail_v[8]; static Tri rail_t[12]; static Mesh rail_m;
static void build_rail(void) {
    fill_box_verts(rail_v, RAIL_HALFX, RAIL_HALFY, TILE_HALFZ);
    fill_box_tris(rail_t, P_RAIL);
    rail_m.v = rail_v; rail_m.nv = 8; rail_m.t = rail_t; rail_m.nt = 12;
}

static V3 star_v[8]; static Tri star_t[12]; static Mesh star_m;
static V3 star_base[NSTAR];
static void build_stars(void) {
    fill_box_verts(star_v, U / 12, U / 12, U / 12);
    fill_box_tris(star_t, P_STAR);
    star_m.v = star_v; star_m.nv = 8; star_m.t = star_t; star_m.nt = 12;
    for (int i = 0; i < NSTAR; i++) {
        uint32_t hx = mix32((uint32_t)i * 7 + 11), hy = mix32((uint32_t)i * 13 + 29),
                 hz = mix32((uint32_t)i * 19 + 3);
        star_base[i].x = (int32_t)(hx % (STAR_SPAN * 2)) - STAR_SPAN;
        star_base[i].y = (int32_t)(hy % STAR_SPAN) + U * 2;
        star_base[i].z = (int32_t)(hz % STAR_CYCLE);
    }
}

// The ball: a lat/long sphere, built once, in two colourings that share one vertex
// buffer — normal and a hurt-flash red — the same variant trick as the tiles.
#define BALL_SEG   10
#define BALL_RINGS 6
#define BALL_NV    ((BALL_RINGS + 1) * BALL_SEG)
#define BALL_NT    (BALL_RINGS * BALL_SEG * 2)
static V3   ball_v[BALL_NV];
static Tri  ball_t[2][BALL_NT];
static Mesh ball_m[2];
static void build_ball(int32_t r) {
    int nv = 0;
    for (int i = 0; i <= BALL_RINGS; i++) {
        int lat = (i * 512) / BALL_RINGS - 256;
        int32_t sy = g_sin[lat & 1023], cy = g_sin[(lat + 256) & 1023];
        for (int j = 0; j < BALL_SEG; j++) {
            int lon = (j * 1024) / BALL_SEG;
            int32_t cx = g_sin[(lon + 256) & 1023], sz = g_sin[lon & 1023];
            int32_t px = (int32_t)(((int64_t)r * cy >> 15) * cx >> 15);
            int32_t py = (int32_t)((int64_t)r * sy >> 15);
            int32_t pz = (int32_t)(((int64_t)r * cy >> 15) * sz >> 15);
            ball_v[nv++] = (V3){ px, py, pz };
        }
    }
    int nt = 0;
    static const uint8_t cis[2] = { P_BALL, P_HURT };
    for (int i = 0; i < BALL_RINGS; i++)
        for (int j = 0; j < BALL_SEG; j++) {
            int jn = (j + 1) % BALL_SEG;
            int a = i * BALL_SEG + j, b = i * BALL_SEG + jn,
                c = (i + 1) * BALL_SEG + j, d = (i + 1) * BALL_SEG + jn;
            V3 nb = ball_v[c];                             // a sphere's normal is its own position
            int16_t nx = (int16_t)(((int64_t)nb.x << 15) / (r ? r : 1));
            int16_t ny = (int16_t)(((int64_t)nb.y << 15) / (r ? r : 1));
            int16_t nz = (int16_t)(((int64_t)nb.z << 15) / (r ? r : 1));
            for (int v = 0; v < 2; v++) {
                ball_t[v][nt]     = (Tri){ (uint16_t)a, (uint16_t)c, (uint16_t)d, cis[v], nx, ny, nz };
                ball_t[v][nt + 1] = (Tri){ (uint16_t)a, (uint16_t)d, (uint16_t)b, cis[v], nx, ny, nz };
            }
            nt += 2;
        }
    for (int v = 0; v < 2; v++) {
        ball_m[v].v = ball_v; ball_m[v].nv = nv; ball_m[v].t = ball_t[v]; ball_m[v].nt = nt;
    }
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

// ---- game state ---------------------------------------------------------------
typedef struct { int32_t x, y, vx, vy; } Ball;
static Ball     g_ball;
static int64_t  g_dist;
static int32_t  g_speed;
static int      g_lives, g_over;
static int32_t  g_best;
static uint32_t g_frame;
static uint8_t  g_events, g_prev_jump, g_hurt;
static uint64_t g_checksum;

static void lose_life(int64_t idx) {
    g_lives--;
    g_events |= EV_FALL;
    if (g_lives <= 0) {
        g_over = 1; g_events |= EV_OVER;
        int32_t d = (int32_t)(g_dist >> 16);
        if (d > g_best) g_best = d;
        return;
    }
    int64_t nidx = idx;
    while (tile_kind(nidx) == T_GAP) nidx++;               // never lands you back in the void
    g_dist = nidx * (int64_t)TILE_LEN;
    g_ball.x = 0; g_ball.vx = 0;
    g_ball.y = hill_y(nidx) + TILE_HALFY + BALL_R; g_ball.vy = 0;
    g_hurt = 30;
}

static void ball_physics(int8_t steer, uint8_t jumpHeld) {
    if (steer) g_ball.vx += steer * STEER_ACCEL;
    else       g_ball.vx = (int32_t)((int64_t)g_ball.vx * 3 / 4);       // friction
    if (g_ball.vx >  STEER_MAXV) g_ball.vx =  STEER_MAXV;
    if (g_ball.vx < -STEER_MAXV) g_ball.vx = -STEER_MAXV;
    g_ball.x += g_ball.vx;
    int32_t xlim = RIB_HALF * 3;
    if (g_ball.x >  xlim) g_ball.x =  xlim;
    if (g_ball.x < -xlim) g_ball.x = -xlim;

    g_ball.vy -= GRAV;
    g_ball.y  += g_ball.vy;

    int64_t idx = g_dist / TILE_LEN;
    int32_t fracNum = (int32_t)(g_dist - idx * (int64_t)TILE_LEN);
    int32_t top = track_top(idx, fracNum);
    int kind = tile_kind(idx);
    int onRibbon = (g_ball.x <= RIB_HALF && g_ball.x >= -RIB_HALF);
    int canLand = (kind != T_GAP) && onRibbon;
    int32_t restY = top + BALL_R;

    // The fall has to fire the instant the ball reaches the height it would have
    // landed at, not some absolute depth below it — a small idle bounce only dips a
    // fraction of a unit, and the ribbon's own hills range further than that. Waiting
    // for an absolute FALL_Y let the ball glide clean through a gap: it dipped, never
    // got deep enough to trip the threshold, and came back up on the far side having
    // "crossed" nothing but empty space it was never touching.
    if (g_ball.vy <= 0 && g_ball.y <= restY) {
        if (canLand) {
            g_ball.y = restY;
            if (jumpHeld) { g_ball.vy = JUMP_V;   g_events |= EV_JUMP; }
            else          { g_ball.vy = BOUNCE_V; g_events |= EV_BOUNCE; }
        } else {
            lose_life(idx);
            return;
        }
    }
    if (g_ball.y < FALL_Y) lose_life(idx);   // safety net: still catches a stray plunge
}

static void reset_run(void) {
    g_dist = 0; g_speed = START_SPEED;
    g_ball.x = 0; g_ball.vx = 0;
    g_ball.y = hill_y(0) + TILE_HALFY + BALL_R; g_ball.vy = 0;
    g_lives = 3; g_over = 0; g_hurt = 0;
    g_checksum = 0;
}

static void init(void) {
    tables_init();
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF05050F;                    // the void
    ramp(P_CHKA, 0xFFB8C4E8);
    ramp(P_CHKB, 0xFF3A4270);
    ramp(P_WARN, 0xFFFF7A33);
    ramp(P_RAIL, 0xFF35E0D8);
    ramp(P_BALL, 0xFFFFC24D);
    ramp(P_HURT, 0xFFFF3355);
    ramp(P_STAR, 0xFF7C6FD6);
    g_pal[P_TXT]  = 0xFFF5F5F8;
    g_pal[P_TXT2] = 0xFFFFD62E;
    g_pal[P_TXT3] = 0xFFFF5A4D;

    build_tiles(); build_rail(); build_ball(BALL_R); build_stars();

    g_frame = 0; g_prev_jump = 0; g_best = 0;
    reset_run();
}

static void tick(const Input in[2]) {
    g_frame++; g_events = 0;
    if (g_hurt) g_hurt--;

    int8_t steer = (int8_t)(in[0].x + in[1].x);
    if (steer > 1) steer = 1; if (steer < -1) steer = -1;
    uint8_t jump  = (uint8_t)(in[0].jump || in[1].jump);
    uint8_t jedge = (uint8_t)(jump && !g_prev_jump);
    g_prev_jump = jump;

    if (g_over) { if (jedge) reset_run(); return; }

    g_speed += SPEED_RAMP;
    if (g_speed > MAX_SPEED) g_speed = MAX_SPEED;
    g_dist += g_speed;

    ball_physics(steer, jump);

    g_checksum = g_checksum * 31 + (uint32_t)g_ball.x + (uint32_t)g_ball.y * 7
               + (uint32_t)(g_dist & 0xffffffffu) + (uint32_t)g_lives * 13 + (uint32_t)g_over;
}

static void audio(void) {
    if (g_events & EV_JUMP)        synth_note(NCHAN - 1, 4, 74, 190);
    else if (g_events & EV_BOUNCE) synth_note(NCHAN - 1, 3, 64, 90);
    if (g_events & EV_FALL)        synth_note(NCHAN - 1, 5, 45, 200);
    if (g_events & EV_OVER)        synth_note(NCHAN - 1, 5, 33, 220);
}

// ---- HUD ------------------------------------------------------------------------
static int digits(int32_t v, char *b) {
    int i = 0;
    if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0;
    return i;
}

static void draw_hud(void) {
    int s = g_fbh / 180; if (s < 1) s = 1;
    char buf[12];

    text_draw(4 * s, hud_top(), s, "DIST", (uint8_t)P_TXT);
    digits((int32_t)(g_dist >> 16), buf);
    text_draw(4 * s + text_width("DIST", s) + 6 * s, hud_top(), s, buf, (uint8_t)P_TXT);

    text_draw(4 * s, hud_top() + 9 * s, s, "LIVES", (uint8_t)P_TXT);
    digits(g_lives, buf);
    text_draw(4 * s + text_width("LIVES", s) + 6 * s, hud_top() + 9 * s, s, buf, (uint8_t)P_TXT2);

    if (g_best > 0) {
        const char *lbl = "BEST";
        text_draw(g_fbw - 4 * s - text_width(lbl, s) - 6 * s - text_width("999999", s),
                  4 * s, s, lbl, (uint8_t)P_TXT);
        digits(g_best, buf);
        text_draw(g_fbw - 4 * s - text_width(buf, s), hud_top(), s, buf, (uint8_t)P_TXT);
    }

    if (g_over) {
        int cx = g_fbw / 2, cy = g_fbh / 2;
        text_draw(cx - text_width("GAME OVER", s * 3) / 2, cy - 16 * s, s * 3, "GAME OVER", (uint8_t)P_TXT3);
        digits((int32_t)(g_dist >> 16), buf);
        char full[24]; int p = 0;
        const char *lbl = "DIST ";
        while (lbl[p]) { full[p] = lbl[p]; p++; }
        for (int k = 0; buf[k]; k++) full[p++] = buf[k];
        full[p] = 0;
        text_draw(cx - text_width(full, s) / 2, cy + 2 * s, s, full, (uint8_t)P_TXT);
        text_draw(cx - text_width("SPACE TO RESTART", s) / 2, cy + 16 * s, s, "SPACE TO RESTART", (uint8_t)P_TXT);
    } else if (g_hurt) {
        text_draw(g_fbw / 2 - text_width("OOPS", s * 2) / 2, g_fbh / 3, s * 2, "OOPS", (uint8_t)P_TXT3);
    }
}

// ---- draw -------------------------------------------------------------------------
#define MAXINST 128
static void draw(void) {
    fb_clear(0);
    static Inst inst[MAXINST];
    int n = 0;

    int64_t idx0 = g_dist / TILE_LEN;
    for (int64_t i = idx0 - 1; i <= idx0 + NVIEW && n < MAXINST - 4; i++) {
        if (i < 0) continue;
        int kind = tile_kind(i);
        int32_t z = (int32_t)(i * (int64_t)TILE_LEN - g_dist) + TILE_HALFZ;
        int32_t y = hill_y(i);
        if (kind != T_GAP) {
            int variant = (kind == T_WARN) ? 2 : (int)(i & 1);
            inst[n].m = &tile_m[variant]; inst[n].pos = (V3){ 0, y, z };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;

            inst[n].m = &rail_m; inst[n].pos = (V3){  RIB_HALF, y + TILE_HALFY + RAIL_HALFY, z };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
            inst[n].m = &rail_m; inst[n].pos = (V3){ -RIB_HALF, y + TILE_HALFY + RAIL_HALFY, z };
            inst[n].ax = inst[n].ay = inst[n].az = 0; inst[n].scale = U; n++;
        }
    }

    for (int i = 0; i < NSTAR && n < MAXINST - 1; i++) {
        int64_t sz = ((int64_t)star_base[i].z - g_dist / STAR_PARDIV) % STAR_CYCLE;
        if (sz < 0) sz += STAR_CYCLE;
        sz -= STAR_CYCLE / 2;
        inst[n].m = &star_m; inst[n].pos = (V3){ star_base[i].x, star_base[i].y, (int32_t)sz };
        inst[n].ax = (int)((g_frame * 3 + (uint32_t)i * 37) & 1023);
        inst[n].ay = (int)((g_frame * 2 + (uint32_t)i * 13) & 1023);
        inst[n].az = 0;
        inst[n].scale = U + (int32_t)((i % 3) * U / 4);
        n++;
    }

    // The ball: spinning about its own forward roll (tied to distance travelled, so it
    // never slips against the ground) plus a bank tied to steering speed.
    int64_t rollRaw = (g_dist * 1024) / 205887;                 // 205887 ~= pi * BALL_R * 2 / U... circumference
    int roll = (int)(rollRaw & 1023);
    int32_t bank = -(int32_t)(((int64_t)g_ball.vx * 40) / (STEER_MAXV ? STEER_MAXV : 1));
    if (bank >  40) bank =  40; if (bank < -40) bank = -40;
    inst[n].m = &ball_m[g_hurt ? 1 : 0];
    inst[n].pos = (V3){ g_ball.x, g_ball.y, 0 };
    inst[n].ax = roll; inst[n].ay = 0; inst[n].az = (int)bank;
    inst[n].scale = U; n++;

    Cam cam;
    cam.pos.x = (int32_t)(((int64_t)g_ball.x * 6) / 10);
    cam.pos.y = g_ball.y + CAM_HEIGHT;
    cam.pos.z = -CAM_DIST;
    cam.ax = 46; cam.ay = 0;
    int32_t camBank = -(int32_t)(((int64_t)g_ball.vx * 20) / (STEER_MAXV ? STEER_MAXV : 1));
    if (camBank >  20) camBank =  20; if (camBank < -20) camBank = -20;
    cam.az = (int)camBank;

    g3d_scene(inst, n, &cam, 0, 0, 0);
    draw_hud();
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_ribbon = { "ribbon", init, tick, audio, draw, checksum };
