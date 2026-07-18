// racer.c — a 3D track winding into the distance, and a car that steers to stay on it.
//
// The whole road is one deterministic curve, authored once as a sequence of {straight,
// turn, hill} commands and baked into a table of world-space nodes at init(). Nothing
// about the track depends on the player: the same nodes exist whether the car is on
// them or not. The car is a point that lives in the track's OWN coordinate frame —
// a distance travelled along the curve, plus a sideways offset from its centreline —
// which is the same trick OutRun and Virtua Racing used to make a curving road cheap to
// drive on: steering doesn't turn the car, it changes how far from the middle it sits.
//
// The camera reads the curve at a point behind the car and looks the way the road is
// pointing there, so it swings through a turn a beat after the car does — a chase, not
// a rail.
#include "core.h"
#include "g3d.h"
#include "game.h"
#include "synth.h"
#include "text.h"

#define U (1 << 16)
#define FX(v)  ((int32_t)((int64_t)(v) * 65536 / 100))     // v in hundredths of a unit
#define FXK(v) ((int32_t)((int64_t)(v) * 65536 / 1000))    // v in thousandths of a unit
static int32_t mul(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 16); }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- box meshes, built once at init ------------------------------------------------
// Every solid thing in this game — car, wheels, pylons, the finish gate — is one of
// these, the same runtime-box trick every cartridge here uses.
#define MAXBOX 128
static V3   bv[MAXBOX][8];
static Tri  bt[MAXBOX][12];
static Mesh bm[MAXBOX];
static int  nbox;
static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
                                 {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1} };
static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
static const int8_t FN[6][3]  = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };

static int box(int32_t sx, int32_t sy, int32_t sz, uint8_t ci) {
    if (nbox >= MAXBOX) return 0;
    int i = nbox++;
    for (int v = 0; v < 8; v++) {
        bv[i][v].x = VP[v][0] * sx; bv[i][v].y = VP[v][1] * sy; bv[i][v].z = VP[v][2] * sz;
    }
    for (int f = 0; f < 6; f++)
        for (int k = 0; k < 2; k++) {
            Tri *t = &bt[i][f * 2 + k];
            t->a = FQ[f][0]; t->b = FQ[f][1 + k]; t->c = FQ[f][2 + k];
            t->ci = ci;
            t->nx = (int16_t)(FN[f][0] * 32767); t->ny = (int16_t)(FN[f][1] * 32767);
            t->nz = (int16_t)(FN[f][2] * 32767);
        }
    bm[i].v = bv[i]; bm[i].nv = 8; bm[i].t = bt[i]; bm[i].nt = 12;
    return i;
}

// ---- palette ------------------------------------------------------------------------
#define P_GND    8    //  8..15  asphalt
#define P_STRIPE 16   // 16..23  the dashed centre line
#define P_DIRT   24   // 24..31  shoulder — off the paved surface, not yet a wall
#define P_WALL   32   // 32..39  the rumble wall
#define P_POLE   40   // 40..47  roadside pylons
#define P_BODY   48   // 48..55  the car
#define P_CAB    56   // 56..63  its cockpit, dim
#define P_WHEEL  64   // 64..71  tyres, near black
#define P_GATE   72   // 72..79  the finish gate

static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int k = 60 + i * 28;
        g_pal[base + i] = 0xFF000000u
            | ((uint32_t)(((rgb >> 16) & 255) * k / 255) << 16)
            | ((uint32_t)(((rgb >> 8) & 255) * k / 255) << 8)
            | (uint32_t)((rgb & 255) * k / 255);
    }
}

// ---- the track ------------------------------------------------------------------
// Authored as commands, not noise: a straight, then a sweep, then a hairpin, so the
// shape of the course is a decision, not a seed. dhead is a heading delta per node (of
// 1024 = a full turn); dy is a height delta per node, both applied for `len` nodes.
// dhead > 0 bends toward +X, which projects to screen-right; dhead < 0 bends left.
typedef struct { int16_t len, dhead, dy; } TSeg;
static const TSeg TRACK[] = {
    { 50,   0,  0 },   // start, dead straight — get up to speed
    { 35,   6,  0 },   // gentle right sweep
    { 15,   0,  0 },
    { 40,  -5,  3 },   // left sweep, climbing
    { 20,   0, -3 },   // straight, descending back down
    { 18,  14,  0 },   // a sharp right hairpin
    { 25,   0,  0 },
    { 22, -13,  0 },   // a sharp left, the other way
    { 45,   0,  4 },   // long straight, climbing a hill
    { 20,   0, -4 },   // down the other side
    { 25,   7,  0 },   // S-curve...
    { 25,  -7,  0 },   // ...and back
    { 40,   0,  0 },   // the finish straight
};
#define NTSEG (int)(sizeof TRACK / sizeof TRACK[0])
#define TN 381                          // 1 + sum of every TRACK[].len above (50+35+15+40+20+18+25+22+45+20+25+25+40=380)

#define SEG_LEN     FX(260)             // world distance between nodes
#define ROAD_HALF   FX(140)             // half the paved width
#define EDGE_W      FX(45)              // shoulder beyond the paved edge
#define STRIPE_HALF FX(12)
#define WALL_H      FX(55)
#define CAR_MARGIN  FX(35)              // how much of the car's width counts against the road

static V3     trk_p[TN];                // centreline, world space, 16.16
static int32_t trk_head[TN];            // heading, angle units, UNWRAPPED (no &1023 baked in)

static void dirvec(int32_t head, int32_t *fx, int32_t *fz) {
    int32_t x = 0, y = 0, z = U; g3d_rot(&x, &y, &z, 0, (int)head, 0); *fx = x; *fz = z;
}
static void rightvec(int32_t head, int32_t *rx, int32_t *rz) {
    int32_t x = U, y = 0, z = 0; g3d_rot(&x, &y, &z, 0, (int)head, 0); *rx = x; *rz = z;
}
static void rightN(int32_t head, int16_t *nx, int16_t *nz) {
    int32_t x = 32767, y = 0, z = 0; g3d_rot(&x, &y, &z, 0, (int)head, 0);
    *nx = (int16_t)x; *nz = (int16_t)z;
}

static void build_track(void) {
    int32_t head = 0; V3 p = { 0, 0, 0 };
    trk_p[0] = p; trk_head[0] = 0;
    int idx = 0;
    for (int s = 0; s < NTSEG; s++) {
        for (int k = 0; k < TRACK[s].len; k++) {
            head += TRACK[s].dhead;
            int32_t fx, fz; dirvec(head, &fx, &fz);
            p.x += fx; p.z += fz; p.y += FX(TRACK[s].dy);
            idx++;
            trk_p[idx] = p; trk_head[idx] = head;
        }
    }
    // idx is exactly TN-1 here; TN was chosen to match TRACK[] above.
}

#define FINISH_S ((int64_t)(TN - 2) * SEG_LEN)

// Sample the curve at arclength s: a node index plus a linear blend to the next one.
// Track nodes are close enough together (SEG_LEN) that a straight-line blend of both
// position and heading is indistinguishable from the true curve.
static void track_at(int64_t s, V3 *pos, int32_t *head) {
    if (s < 0) s = 0;
    int64_t maxs = (int64_t)(TN - 2) * SEG_LEN;
    if (s > maxs) s = maxs;
    int ni = (int)(s / SEG_LEN);
    if (ni > TN - 2) ni = TN - 2;
    if (ni < 0) ni = 0;
    int32_t t = (int32_t)(s - (int64_t)ni * SEG_LEN);
    int32_t frac = (int32_t)(((int64_t)t << 16) / SEG_LEN);
    const V3 *a = &trk_p[ni], *b = &trk_p[ni + 1];
    pos->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * frac) >> 16);
    pos->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * frac) >> 16);
    pos->z = a->z + (int32_t)(((int64_t)(b->z - a->z) * frac) >> 16);
    *head  = trk_head[ni] + (int32_t)(((int64_t)(trk_head[ni + 1] - trk_head[ni]) * frac) >> 16);
}

// ---- the road mesh, built once: 8 lateral points per node, 7 coloured bands between
// them, laid out as one long ribbon that follows every curve and hill in the table.
#define RD_PTS 8
static const int32_t PROF_DX[RD_PTS] = {
    -(ROAD_HALF + EDGE_W), -(ROAD_HALF + EDGE_W), -ROAD_HALF, -STRIPE_HALF,
     STRIPE_HALF,  ROAD_HALF,  ROAD_HALF + EDGE_W,  ROAD_HALF + EDGE_W,
};
static const int32_t PROF_DY[RD_PTS] = { WALL_H, 0, 0, 0, 0, 0, 0, WALL_H };

static V3  road_v[TN * RD_PTS];
static Tri road_t[(TN - 1) * 7 * 2];

static void build_road(void) {
    for (int i = 0; i < TN; i++) {
        int32_t rx, rz; rightvec(trk_head[i], &rx, &rz);
        for (int j = 0; j < RD_PTS; j++) {
            V3 *v = &road_v[i * RD_PTS + j];
            v->x = trk_p[i].x + mul(rx, PROF_DX[j]);
            v->y = trk_p[i].y + PROF_DY[j];
            v->z = trk_p[i].z + mul(rz, PROF_DX[j]);
        }
    }
    int nt = 0;
    for (int i = 0; i < TN - 1; i++) {
        int16_t rnx, rnz; rightN(trk_head[i], &rnx, &rnz);
        for (int k = 0; k < 7; k++) {
            uint8_t ci; int16_t nx = 0, ny = 32767, nz = 0;
            if (k == 0)      { ci = P_WALL; nx = rnx;            ny = 0; nz = rnz; }
            else if (k == 6) { ci = P_WALL; nx = (int16_t)-rnx;  ny = 0; nz = (int16_t)-rnz; }
            else if (k == 1 || k == 5) ci = P_DIRT;
            else if (k == 3) ci = ((i / 6) & 1) ? P_STRIPE : P_GND;   // the dashes
            else ci = P_GND;                                          // k == 2, 4
            int a = i * RD_PTS + k, b = i * RD_PTS + k + 1;
            int c = (i + 1) * RD_PTS + k + 1, d = (i + 1) * RD_PTS + k;
            road_t[nt++] = (Tri){ (uint16_t)a, (uint16_t)b, (uint16_t)c, ci, nx, ny, nz };
            road_t[nt++] = (Tri){ (uint16_t)a, (uint16_t)c, (uint16_t)d, ci, nx, ny, nz };
        }
    }
}

// ---- roadside pylons, positioned once (the track never moves) -----------------------
#define POLE_STEP 10
#define NPOLE (2 * (TN / POLE_STEP + 1))
static V3 pole_pos[NPOLE];
static int npole;
static int pole_mesh;

static void build_poles(void) {
    npole = 0;
    for (int i = 0; i < TN; i += POLE_STEP) {
        int32_t rx, rz; rightvec(trk_head[i], &rx, &rz);
        int32_t off = ROAD_HALF + EDGE_W + FX(30);
        if (npole < NPOLE) {
            V3 *p = &pole_pos[npole++];
            p->x = trk_p[i].x + mul(rx, off); p->y = trk_p[i].y + FX(80); p->z = trk_p[i].z + mul(rz, off);
        }
        if (npole < NPOLE) {
            V3 *p = &pole_pos[npole++];
            p->x = trk_p[i].x - mul(rx, off); p->y = trk_p[i].y + FX(80); p->z = trk_p[i].z - mul(rz, off);
        }
    }
}

// ---- the finish gate: two pillars and a beam, sat at the last node ------------------
static V3 gate_pos[3]; static int gate_head;
static int gate_pillar, gate_beam;
static void build_gate(void) {
    int n = TN - 1;
    int32_t rx, rz; rightvec(trk_head[n], &rx, &rz);
    gate_head = trk_head[n];
    gate_pos[0] = (V3){ trk_p[n].x + mul(rx, ROAD_HALF), trk_p[n].y + FX(110), trk_p[n].z + mul(rz, ROAD_HALF) };
    gate_pos[1] = (V3){ trk_p[n].x - mul(rx, ROAD_HALF), trk_p[n].y + FX(110), trk_p[n].z - mul(rz, ROAD_HALF) };
    gate_pos[2] = (V3){ trk_p[n].x, trk_p[n].y + FX(230), trk_p[n].z };
}

// ---- the car, a few boxes on a rigid frame ------------------------------------------
typedef struct { int32_t x, y, z, sx, sy, sz; uint8_t mat; } Part;
enum { PM_BODY, PM_CAB, PM_WHEEL };
#define NPART 6
static const Part CAR[NPART] = {
    {   0,  30,   0,  35,  16,  70, PM_BODY  },   // chassis
    {   0,  60, -14,  22,  14,  30, PM_CAB   },   // cockpit, toward the back
    { -37,  16,  52,   9,  16,  16, PM_WHEEL },   // front left
    {  37,  16,  52,   9,  16,  16, PM_WHEEL },   // front right
    { -37,  16, -52,   9,  16,  16, PM_WHEEL },   // rear left
    {  37,  16, -52,   9,  16,  16, PM_WHEEL },   // rear right
};
static int car_mesh[NPART];

static void build_car(void) {
    for (int i = 0; i < NPART; i++) {
        uint8_t ci = CAR[i].mat == PM_BODY ? P_BODY : (CAR[i].mat == PM_CAB ? P_CAB : P_WHEEL);
        car_mesh[i] = box(FX(CAR[i].sx), FX(CAR[i].sy), FX(CAR[i].sz), ci);
    }
}

// ---- game state -----------------------------------------------------------------
typedef enum { RACE, WON, CRASHED, TIMEOUT } RState;
static int64_t  g_s;          // arclength travelled, 16.16
static int32_t  g_off, g_voff;   // lateral offset & its rate, 16.16
static int32_t  g_speed;
static uint32_t g_frame, g_timer, g_offframes;
static RState   g_state;
static uint64_t g_checksum;
static V3       g_car_pos; static int32_t g_car_head;
static uint8_t  g_events;
#define EV_WALL  1
#define EV_SCRAPE 2
#define EV_WIN   4
#define EV_CRASH 8
#define EV_TIME  16

#define SPEED_MIN FX(9)
#define SPEED_MAX FX(42)
#define BOOST_MAX FX(50)
#define ACCEL     FXK(2)
#define BRAKE     FXK(20)
#define SCRAPE_DECEL FXK(18)
#define WALL_HIT  FX(9)
#define STEER_ACCEL 110
#define MAXVOFF     1500
#define CRASH_FRAMES 90
#define TIME_LIMIT   6000

static void reset_state(void) {
    g_s = 0; g_off = 0; g_voff = 0; g_speed = SPEED_MIN;
    g_timer = 0; g_offframes = 0; g_state = RACE; g_events = 0;
    V3 p; int32_t h; track_at(0, &p, &h);
    g_car_pos = p; g_car_head = h;
}

static void init(void) {
    tables_init();
    nbox = 0;
    build_track();
    build_road();
    build_poles();
    build_gate();
    build_car();
    pole_mesh = box(FX(8), FX(80), FX(8), P_POLE);
    gate_pillar = box(FX(14), FX(110), FX(14), P_GATE);
    gate_beam = box(ROAD_HALF + FX(20), FX(15), FX(14), P_GATE);

    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF6FC6E8;   // a bright day sky
    g_pal[1] = 0xFF2A2C3E;   // dim HUD text
    g_pal[2] = 0xFFF5F5F8;   // bright HUD text
    g_pal[3] = 0xFFEF7D57;   // accent
    g_pal[4] = 0xFFE8302E;   // warning
    // The road is lit by the same view-Z rule as everything else, but it's nearly
    // horizontal under a shallow chase-cam pitch, so it never reaches the top of its
    // ramp — the base colours below are chosen bright enough to still read as asphalt,
    // not shadow, at the low shade that's all a flat surface ever earns here.
    ramp(P_GND,    0xFF9A97A6);
    ramp(P_STRIPE, 0xFFFCF3B0);
    ramp(P_DIRT,   0xFFAE9C55);
    ramp(P_WALL,   0xFFD8433F);
    ramp(P_POLE,   0xFFEFEFEF);
    ramp(P_BODY,   0xFFE8302E);
    ramp(P_CAB,    0xFF33333F);
    ramp(P_WHEEL,  0xFF1E1E1E);
    ramp(P_GATE,   0xFFF2C230);

    g_frame = 0; g_checksum = 0;
    reset_state();
}

static void tick(const Input in[2]) {
    g_frame++;
    g_events = 0;

    // WASD and the arrows both drive the one car.
    int8_t px = (int8_t)clampi(in[0].x + in[1].x, -1, 1);
    int8_t py = (int8_t)clampi(in[0].y + in[1].y, -1, 1);
    uint8_t jump = (uint8_t)(in[0].jump || in[1].jump);

    if (g_state != RACE) {
        if (jump) reset_state();
        g_checksum = g_checksum * 31u + g_frame;
        return;
    }

    g_voff += px * STEER_ACCEL;
    g_voff = clampi(g_voff, -MAXVOFF, MAXVOFF);
    g_voff = (int32_t)(((int64_t)g_voff * 94) / 100);      // grip: bleeds off with no input
    g_off += g_voff;

    int32_t limit = ROAD_HALF + EDGE_W - CAR_MARGIN;
    int hitwall = 0;
    if (g_off > limit)  { g_off = limit;  g_voff = 0; hitwall = 1; }
    if (g_off < -limit) { g_off = -limit; g_voff = 0; hitwall = 1; }

    int offroad = g_off > ROAD_HALF - CAR_MARGIN || g_off < -(ROAD_HALF - CAR_MARGIN);

    int32_t target = SPEED_MAX + (py > 0 ? BOOST_MAX - SPEED_MAX : 0);
    if (hitwall) { g_speed -= WALL_HIT; g_events |= EV_WALL; }
    if (offroad) {
        g_speed -= SCRAPE_DECEL; g_offframes++;
        if ((g_frame & 3) == 0) g_events |= EV_SCRAPE;
    } else {
        g_offframes = 0;
        if (g_speed < target) g_speed += ACCEL;
    }
    if (py < 0) g_speed -= BRAKE;
    g_speed = clampi(g_speed, SPEED_MIN, BOOST_MAX);

    g_s += g_speed;
    g_timer++;

    V3 pos; int32_t head; track_at(g_s, &pos, &head);
    int32_t rx, rz; rightvec(head, &rx, &rz);
    g_car_pos.x = pos.x + mul(rx, g_off);
    g_car_pos.y = pos.y;
    g_car_pos.z = pos.z + mul(rz, g_off);
    g_car_head = head;

    if (g_s >= FINISH_S)             { g_state = WON;     g_events |= EV_WIN; }
    else if (g_offframes > CRASH_FRAMES) { g_state = CRASHED; g_events |= EV_CRASH; }
    else if (g_timer > TIME_LIMIT)   { g_state = TIMEOUT; g_events |= EV_TIME; }

    g_checksum = g_checksum * 1000003u + (uint32_t)g_s + (uint32_t)g_off
               + (uint32_t)g_speed + (uint32_t)g_state + g_frame;
}

static void audio(void) {
    if (g_events & EV_WALL)   synth_note(NCHAN - 1, 3, 38, 200);
    if (g_events & EV_SCRAPE) synth_note(NCHAN - 1, 4, 64, 90);
    if (g_events & EV_WIN)    synth_note(NCHAN - 1, 5, 84, 190);
    if (g_events & EV_CRASH)  synth_note(NCHAN - 1, 3, 30, 220);
    if (g_events & EV_TIME)   synth_note(NCHAN - 1, 3, 33, 200);
}

// ---- HUD: digits, a scaled font, no colon in the glyph set so time is plain seconds ---
static int digits(int32_t v, char *b) {
    int i = 0; if (v <= 0) b[i++] = '0';
    while (v > 0 && i < 10) { b[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; }
    b[i] = 0; return i;
}
static void num(int x, int y, int sc, int32_t v, uint8_t ci) {
    char b[12]; digits(v, b); text_draw(x, y, sc, b, ci);
}

static void hud(void) {
    int sc = g_fbh / 200; if (sc < 1) sc = 1;
    text_draw(6 * sc, hud_top(), sc, "SPEED", 1);
    num(6 * sc + 34 * sc, hud_top(), sc, (int32_t)(((int64_t)g_speed * 1000) >> 16), 2);
    text_draw(6 * sc, hud_top() + 10 * sc, sc, "TIME", 1);
    num(6 * sc + 28 * sc, hud_top() + 10 * sc, sc, (int32_t)(g_timer / 60), 2);
    text_draw(6 * sc, hud_top() + 20 * sc, sc, "DIST", 1);
    num(6 * sc + 28 * sc, hud_top() + 20 * sc, sc, (int32_t)(g_s >> 16), 2);

    if (g_state == RACE) return;
    int cx = g_fbw / 2, cy = g_fbh / 2;
    for (int y = cy - 30 * sc; y < cy + 34 * sc; y++)
        for (int x = 0; x < g_fbw; x++)
            if (y >= 0 && y < g_fbh) g_fb[y * g_fbw + x] = 3;
    const char *msg = g_state == WON ? "FINISH" : g_state == CRASHED ? "WRECKED" : "TIME UP";
    text_draw(cx - text_width(msg, sc * 3) / 2, cy - 22 * sc, sc * 3, msg, 2);
    num(cx - 30 * sc, cy + 3 * sc, sc * 2, (int32_t)(g_timer / 60), 2);
    text_draw(cx + 6 * sc, cy + 3 * sc, sc * 2, "S", 1);
    text_draw(cx - text_width("SPACE TO RACE AGAIN", sc) / 2, cy + 22 * sc, sc, "SPACE TO RACE AGAIN", 1);
}

// How far ahead/behind of the camera's own track-node to draw. Baking the WHOLE track
// into the scene every frame is correct (it's real geometry, real depth-tested) but
// wasteful and, worse, ugly: a thousand distant nodes and pylons all converge on the
// same few pixels at the horizon and the z-buffer can't tell them apart at that
// distance, so they z-fight into a flickering scribble. A window is a crop, not a fix
// for a bug — the far track is still there, just not worth drawing this frame.
#define WIN_BEHIND 3
#define WIN_AHEAD  70

static void draw(void) {
    fb_clear(0);
    static Inst inst[8 + NPOLE + NPART];
    int n = 0;

    int64_t cams = g_s - FX(550); if (cams < 0) cams = 0;
    V3 camp; int32_t camh; track_at(cams, &camp, &camh);

    int niCam = (int)(cams / SEG_LEN);
    niCam = (int)clampi(niCam, 0, TN - 2);
    int winStart = niCam - WIN_BEHIND; if (winStart < 0) winStart = 0;
    int winEnd = niCam + WIN_AHEAD;    if (winEnd > TN - 1) winEnd = TN - 1;
    int segCount = winEnd - winStart;  if (segCount < 0) segCount = 0;

    static Mesh road_sub;
    road_sub.v = road_v; road_sub.nv = TN * RD_PTS;
    road_sub.t = &road_t[winStart * 14]; road_sub.nt = segCount * 14;
    inst[n] = (Inst){ &road_sub, { 0, 0, 0 }, 0, 0, 0, U }; n++;

    for (int i = 0; i < npole; i++) {
        int node = (i / 2) * POLE_STEP;
        if (node < winStart - POLE_STEP || node > winEnd + POLE_STEP) continue;
        inst[n] = (Inst){ &bm[pole_mesh], pole_pos[i], 0, 0, 0, U }; n++;
    }

    if (winEnd >= TN - 2) {   // the gate only exists once it's actually in the window
        inst[n] = (Inst){ &bm[gate_pillar], gate_pos[0], 0, gate_head, 0, U }; n++;
        inst[n] = (Inst){ &bm[gate_pillar], gate_pos[1], 0, gate_head, 0, U }; n++;
        inst[n] = (Inst){ &bm[gate_beam],   gate_pos[2], 0, gate_head, 0, U }; n++;
    }

    // The car: a rigid turn by hand, same trick as every figure in this engine — rotate
    // each part's local offset by the car's heading (plus a lean off the steering) and
    // hand the mesh the same angle, so the body and its wheels turn together.
    int32_t bank = clampi(-g_voff * 6, -3000, 3000);
    for (int i = 0; i < NPART; i++) {
        int32_t ox = FX(CAR[i].x), oy = FX(CAR[i].y), oz = FX(CAR[i].z);
        g3d_rot(&ox, &oy, &oz, 0, g_car_head, bank / 100);
        inst[n].m = &bm[car_mesh[i]];
        inst[n].pos = (V3){ g_car_pos.x + ox, g_car_pos.y + FX(14) + oy, g_car_pos.z + oz };
        inst[n].ax = 0; inst[n].ay = g_car_head; inst[n].az = bank / 100;
        inst[n].scale = U;
        n++;
    }

    Cam cam = { { camp.x, camp.y + FX(230), camp.z }, 38, camh, 0 };
    g3d_scene(inst, n, &cam, 0, 0, 0);

    hud();
}

static uint64_t checksum(void) { return g_checksum; }

const Game game_racer = { "racer", init, tick, audio, draw, checksum };
