// menu.c — the console shell, in 3D. A rack of cartridges floats in front of you, the one in
// front turned to face you and rocking gently to catch the light; pick it and (next stage) the
// lens tips down to a console on the floor and the cart dives in. Underneath it's still the
// picker — it sets g_switch_to — wearing a machine.
//
// The wobble, the sheen ramp and the eases are title.c's, which read them out of cubeconjure.
#include <stdlib.h>   // getenv, for the turntable dev instrument
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"
#include "g3d.h"

#define U (1 << 16)

enum { P_BOOT, P_SHELF, P_INSERT };
static int g_sel, g_cool, g_phase, g_ins, g_boot;
static int32_t g_scroll;                 // smoothed shelf index (16.16), chasing g_sel
static uint32_t g_frame;

#define BOOT_LEN 104          // frames the "CVERTEX" fly-in runs before the shelf takes over
#define FLY      48           // frames one letter spends in flight
#define STAG      7           // frames between successive letters launching

static const Game *const *g_list;
static int g_n;
void menu_populate(const Game *const *list, int n) { g_list = list; g_n = n; }

#define INS_LEN 46
#define SEAT    8            // frames before the end that the cart hits home
#define MAXCART 32

// ---- the cartridge mesh: a chamfered slab, not a cube. Eight-sided cross-section (corners cut
// at 45, the 90s idea of "moulded"), extruded to a shallow depth: a front face, a back, and a
// ring of narrow bevels catching light as it turns. One template; per-cart copies just recolour
// the label band, the way title.c gives each cubie its own faces. --------------------------------
#define CV 36                            // 8 front + 8 back + 4 label + 8 grip + 8 connector
#define CT 54                            // 6 front + 6 back + 16 sides + 2 label + 12 grip + 12 connector
static V3   cart_v[MAXCART][CV];
static Tri  cart_t[MAXCART][CT];
static Mesh cart_m[MAXCART];
static V3   tmpl_v[CV];
static Tri  tmpl_t[CT];
static void box_into(V3 *v, Tri *t, int vbase, int32_t ox, int32_t oy, int32_t oz,
                     int32_t hx, int32_t hy, int32_t hz, uint8_t ci);

static void build_template(void) {
    const int32_t w = U * 60 / 100, h = U * 78 / 100, c = U * 6 / 100, d = U * 30 / 100;
    // eight cross-section points, corners chamfered by c, clockwise from top-right
    int32_t px[8] = {  w,      w - c, -(w - c), -w,     -w,        -(w - c),  w - c,  w      };
    int32_t py[8] = {  h - c,  h,      h,        h - c, -(h - c),  -h,       -h,    -(h - c) };
    for (int i = 0; i < 8; i++) {
        tmpl_v[i]     = (V3){ px[i], py[i],  d };     // front ring (+z faces the player... -z here)
        tmpl_v[i + 8] = (V3){ px[i], py[i], -d };     // back ring
    }
    int nt = 0;                                       // Tri is { a,b,c, ci, nx,ny,nz }
    // 🔴 The normal is the ONE source of truth for culling and light (g3d.c). The ring at +d is the
    // FAR face — its outward normal is +z (away from the camera), so it culls. The ring at -d is the
    // NEAR face the player sees — normal -z. Getting these backwards culls the near wall instead: you
    // then see THROUGH the cartridge to its far wall, with the label floating over a hollow gap.
    for (int i = 1; i < 7; i++)                        // +d ring: far face, normal +z (away, culled)
        tmpl_t[nt++] = (Tri){ (uint16_t)0, (uint16_t)i, (uint16_t)(i + 1), 0, 0, 0, 32767 };
    for (int i = 1; i < 7; i++)                        // -d ring: near face, normal -z (toward camera)
        tmpl_t[nt++] = (Tri){ (uint16_t)8, (uint16_t)(8 + i + 1), (uint16_t)(8 + i), 0, 0, 0, -32767 };
    for (int i = 0; i < 8; i++) {                     // the bevel ring
        int j = (i + 1) & 7;
        int32_t ex = py[i] - py[j], ey = px[j] - px[i];          // outward edge normal, in xy
        int32_t mag = ex < 0 ? -ex : ex, my = ey < 0 ? -ey : ey; mag = mag > my ? mag : my;
        if (mag == 0) mag = 1;
        int16_t nx = (int16_t)((int64_t)ex * 30000 / mag), ny = (int16_t)((int64_t)ey * 30000 / mag);
        tmpl_t[nt++] = (Tri){ (uint16_t)i, (uint16_t)(i + 8), (uint16_t)(j + 8), 0, nx, ny, 0 };
        tmpl_t[nt++] = (Tri){ (uint16_t)i, (uint16_t)(j + 8), (uint16_t)j,       0, nx, ny, 0 };
    }
    // the label sticker: a raised rect on the camera-facing face, upper-centre — the cue that
    // says "cartridge", not "gem". Same face the front fan showed, a hair proud so it can't fight.
    int32_t lw = w * 66 / 100, lt = h * 60 / 100, lb = -h * 42 / 100, lz = -(d + U * 4 / 100);
    tmpl_v[16] = (V3){ -lw, lb, lz }; tmpl_v[17] = (V3){ lw, lb, lz };
    tmpl_v[18] = (V3){ lw, lt, lz };  tmpl_v[19] = (V3){ -lw, lt, lz };
    tmpl_t[nt++] = (Tri){ 16, 17, 18, 0, 0, 0, -32767 };
    tmpl_t[nt++] = (Tri){ 16, 18, 19, 0, 0, 0, -32767 };
    // a narrow grip tab dead-centre on top — a pull-notch, not a full-width lid (that read as a jar)
    // and not a wide rail (that read as a crimped bag). Verts 20..27, tris 30..41.
    box_into(&tmpl_v[20], &tmpl_t[30], 20, 0, h + U * 1 / 100, 0, w * 44 / 100, U * 6 / 100, d * 84 / 100, 8);
    // the connector foot at the bottom — the darker edge that slots into the console. THE thing that
    // says "cartridge, and this end goes in", and it gives the shape a top and a bottom. Verts 28..35.
    box_into(&tmpl_v[28], &tmpl_t[42], 28, 0, -h - U * 3 / 100, 0, w * 66 / 100, U * 7 / 100, d * 74 / 100, 8);
}

// an 8-shade ramp from one rgb, at base — title.c's, so flat faces read as a lit gloss.
static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 45 + i * 28;
        int r = (int)((rgb >> 16) & 255) * m / 255, gg = (int)((rgb >> 8) & 255) * m / 255, b = (int)(rgb & 255) * m / 255;
        if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
    }
}
// tech palette: the body is one dark gunmetal for every cart, the connector a darker metal, and the
// only colour is the label — a neon "screen" that tells the carts apart. Reads as console hardware,
// not candy.
static const uint32_t NEON_RGB[6] = { 0xFF22D3EE, 0xFF41A6F6, 0xFF8B5CF6, 0xFFEC4899, 0xFF34D399, 0xFFF6C453 };

static void build_carts(void) {
    build_template();
    ramp(8,  0xFF12151E);                             // 8..15  connector (near-black metal)
    ramp(16, 0xFF39415C);                             // 16..23 body (blue gunmetal)
    for (int c = 0; c < 6; c++) ramp(24 + c * 8, NEON_RGB[c]);   // 24..71 six neon label ramps
    int n = g_n < MAXCART ? g_n : MAXCART;
    for (int i = 0; i < n; i++) {
        for (int v = 0; v < CV; v++) cart_v[i][v] = tmpl_v[v];
        for (int t = 0; t < CT; t++) {
            cart_t[i][t] = tmpl_t[t];
            cart_t[i][t].ci = (uint8_t)((t == 28 || t == 29) ? 24 + (i % 6) * 8   // neon label
                                        : (t >= 42)          ? 8                  // connector foot
                                        :                      16);              // gunmetal body + grip
        }
        cart_m[i].v = cart_v[i]; cart_m[i].nv = CV; cart_m[i].t = cart_t[i]; cart_m[i].nt = CT;
    }
}

// ---- the console it dives into: two boxes, a wide grey body and a dark slot on its top face ----
static V3   con_v[16];
static Tri  con_t[24];
static Mesh con_m;
static int32_t g_shake;                  // camera-shake energy on the seat, decaying (draw side)

// write a box's 8 verts at v[] and 12 tris at t[]; vbase is where these verts land in the final
// mesh, so a box added after others points at its OWN vertices, not the first box's.
static void box_into(V3 *v, Tri *t, int vbase, int32_t ox, int32_t oy, int32_t oz,
                     int32_t hx, int32_t hy, int32_t hz, uint8_t ci) {
    static const int8_t VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
    static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
    static const int8_t FN[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int i = 0; i < 8; i++) v[i] = (V3){ VP[i][0] * hx + ox, VP[i][1] * hy + oy, VP[i][2] * hz + oz };
    for (int f = 0; f < 6; f++) for (int k = 0; k < 2; k++) {
        Tri *tt = &t[f * 2 + k];
        tt->a = (uint16_t)(FQ[f][0] + vbase); tt->b = (uint16_t)(FQ[f][1 + k] + vbase); tt->c = (uint16_t)(FQ[f][2 + k] + vbase);
        tt->ci = ci;
        tt->nx = (int16_t)(FN[f][0] * 32767); tt->ny = (int16_t)(FN[f][1] * 32767); tt->nz = (int16_t)(FN[f][2] * 32767);
    }
}
static void build_console(void) {
    ramp(72, 0xFF3A3D4C);                                     // 72..79 console body
    ramp(80, 0xFF0A0912);                                     // 80..87 the dark slot
    box_into(&con_v[0], &con_t[0], 0, 0, 0, 0, U * 150 / 100, U * 30 / 100, U * 78 / 100, 72);   // body
    box_into(&con_v[8], &con_t[12], 8, 0, U * 30 / 100, 0, U * 42 / 100, U * 6 / 100, U * 12 / 100, 80); // slot in the top
    con_m.v = con_v; con_m.nv = 16; con_m.t = con_t; con_m.nt = 24;
}

// ---- the CVERTEX logo, in 3D. Each letter is a handful of rectangular BARS — vertical, horizontal
// or diagonal — extruded to a shallow depth, so the whole word is real geometry that catches light
// and turns, not a flat blit. bar_into is box_into with its cross-section rotated in the xy-plane by
// `ang`, which is the whole trick: a rotated box is a diagonal stroke, and diagonal strokes are what
// V, X and R's leg are made of. ---------------------------------------------------------------------
#define NLET 7
#define LV   48              // 6 bars max * 8 verts
#define LT   72              // 6 bars max * 12 tris
static V3   let_v[NLET][LV];
static Tri  let_t[NLET][LT];
static Mesh let_m[NLET];

static void bar_into(V3 *v, Tri *t, int vbase, int32_t cx, int32_t cy,
                     int32_t hx, int32_t hy, int32_t dz, int ang, uint8_t ci) {
    int32_t co = g_sin[(ang + 256) & 1023], sn = g_sin[ang & 1023];      // Q15
    static const int8_t  VP[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
    static const uint8_t FQ[6][4] = { {1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},{5,4,7,6},{0,1,2,3} };
    static const int8_t  FN[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
    for (int i = 0; i < 8; i++) {
        int32_t lx = VP[i][0] * hx, ly = VP[i][1] * hy;                  // local rectangle corner
        int32_t rx = (int32_t)(((int64_t)lx * co - (int64_t)ly * sn) >> 15);
        int32_t ry = (int32_t)(((int64_t)lx * sn + (int64_t)ly * co) >> 15);
        v[i] = (V3){ cx + rx, cy + ry, VP[i][2] * dz };
    }
    for (int f = 0; f < 6; f++) for (int k = 0; k < 2; k++) {
        Tri *tt = &t[f * 2 + k];
        tt->a = (uint16_t)(FQ[f][0] + vbase); tt->b = (uint16_t)(FQ[f][1 + k] + vbase); tt->c = (uint16_t)(FQ[f][2 + k] + vbase);
        tt->ci = ci;
        int32_t nx = FN[f][0], ny = FN[f][1];                            // rotate the side normal too
        tt->nx = (int16_t)((int64_t)nx * co - (int64_t)ny * sn);
        tt->ny = (int16_t)((int64_t)nx * sn + (int64_t)ny * co);
        tt->nz = (int16_t)(FN[f][2] * 32767);
    }
}

// strokes in a normalised cell: x,y in hundredths of a unit, tall letters. {cx,cy,halfx,halfy,ang}.
static const struct { char c; int nb; int16_t b[6][5]; } GLYPH[6] = {
    { 'C', 3, { {-47,0,15,100,0}, {0,85,62,15,0}, {0,-85,62,15,0} } },
    { 'V', 2, { {-31,0,15,104,49}, {31,0,15,104,975} } },
    { 'E', 4, { {-47,0,15,100,0}, {0,85,62,15,0}, {-3,0,52,15,0}, {0,-85,62,15,0} } },
    { 'R', 5, { {-47,0,15,100,0}, {-5,85,50,15,0}, {47,55,15,45,0}, {-5,12,50,15,0}, {24,-45,13,63,70} } },
    { 'T', 2, { {0,85,62,15,0}, {0,-10,15,95,0} } },
    { 'X', 2, { {0,0,14,117,934}, {0,0,14,117,90} } },
};
static const char WORD[NLET + 1] = "CVERTEX";
#define LDZ (U * 26 / 100)

static void build_letters(void) {
    ramp(88, 0xFF5AB0FF);                                               // 88..95 the glossy logo blue
    for (int i = 0; i < NLET; i++) {
        const int16_t (*b)[5] = 0; int nb = 0;
        for (int g = 0; g < 6; g++) if (GLYPH[g].c == WORD[i]) { b = GLYPH[g].b; nb = GLYPH[g].nb; break; }
        int vb = 0, tb = 0;
        for (int s = 0; s < nb; s++) {
            bar_into(&let_v[i][vb], &let_t[i][tb], vb,
                     b[s][0] * U / 100, b[s][1] * U / 100, b[s][2] * U / 100, b[s][3] * U / 100,
                     LDZ, b[s][4], 88);
            vb += 8; tb += 12;
        }
        let_m[i] = (Mesh){ let_v[i], vb, let_t[i], tb };
    }
}

static int clamp01(int t) { return t < 0 ? 0 : (t > 1024 ? 1024 : t); }
static int ease_io(int t) { t = clamp01(t); return (int)(((int64_t)t * t * (3 * 1024 - 2 * t)) >> 20); }
static int ease_in(int t) { t = clamp01(t); return (int)(((int64_t)t * t) >> 10); }
static int ease_out(int t) { t = clamp01(t); return 1024 - ease_in(1024 - t); }
static int32_t lerp(int32_t a, int32_t b, int e) { return a + (int32_t)(((int64_t)(b - a) * e) >> 10); }

// Comic focus lines: a ragged sunburst converging on a point, drawn straight into the framebuffer
// over the 3D. Manga's whole vocabulary for "this, RIGHT here" — it rides the dive and peaks on
// the chunk. Each spoke leaves the centre clear by a jittered radius and runs off the edge.
static void focus_lines(int fx, int fy, int rin, int intensity, uint8_t ci) {
    if (intensity <= 0) return;
    int diag = g_fbw + g_fbh;
    int n = 40 + intensity / 6;
    for (int k = 0; k < n; k++) {
        int ang = (k * 1024 / n + (int)((g_frame * 7 + k * 101) % 21)) & 1023;
        int32_t co = g_sin[(ang + 256) & 1023], sn = g_sin[ang & 1023];
        int r0 = rin + (int)((k * 37 + g_frame) % 60);        // ragged inner edge
        for (int r = r0; r < diag; r += 1) {
            int x = fx + (int)(((int64_t)co * r) >> 15);
            int y = fy + (int)(((int64_t)sn * r) >> 15);
            if (x < 0 || x >= g_fbw || y < 0 || y >= g_fbh) break;
            g_fb[y * g_fbw + x] = ci;                          // thin spoke
            if (r > rin + 120 && x + 1 < g_fbw) g_fb[y * g_fbw + x + 1] = ci;   // thickens outward
        }
    }
}

static void beep(int wave, int midi, int vel) { synth_note(NCHAN - 1, wave, midi, vel); }

static void init(void) {
    tables_init();
    g_frame = 0; g_cool = 0; g_phase = P_BOOT; g_ins = 0; g_boot = 0;
    g_scroll = (int32_t)g_sel << 16;
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0C0B14;   // deep background
    g_pal[1] = 0xFF4A4458;   // dim text
    g_pal[2] = 0xFFF5F5F8;   // bright text
    g_pal[4] = 0xFF41A6F6;   // title
    g_shake = 0;
    build_carts();
    build_console();
    build_letters();
}

static void tick(const Input in[2]) {
    g_frame++;
    if (g_phase == P_BOOT) {
        g_boot++;
        for (int i = 0; i < NLET; i++)                        // a rising chime as each letter seats
            if (g_boot == i * STAG + FLY) beep(4, 55 + i * 3, 110);   // instr 4: FM ping, sustain 0
        int mv = in[0].x + in[1].x + in[0].y + in[1].y;       // any nudge skips the intro
        if (mv || g_boot >= BOOT_LEN) { g_phase = P_SHELF; g_cool = 6; }
        return;
    }
    if (g_phase == P_INSERT) {
        if (g_ins == SEAT) { g_shake = U * 34 / 100; beep(3, 30, 220); }   // the chunk, as it seats
        if (g_shake > 0) g_shake -= (g_shake >> 3) + U / 100;
        if (g_shake < 0) g_shake = 0;
        if (--g_ins <= 0 && g_n) g_switch_to = g_list[g_sel];
        return;
    }
    int32_t target = (int32_t)g_sel << 16;
    g_scroll += (target - g_scroll) >> 3;
    int y = in[0].y + in[1].y, x = in[0].x + in[1].x;
    int mv = x ? (x > 0 ? 1 : -1) : (y ? (y > 0 ? -1 : 1) : 0);
    if (g_cool > 0) g_cool--;
    else if (mv) { g_sel = (g_sel + mv + g_n) % g_n; g_cool = 9; beep(5, 74, 80); }   // instr 5: short square tick, sustain 0
    if ((in[0].jump || in[1].jump) && g_n) { g_phase = P_INSERT; g_ins = INS_LEN; beep(3, 40, 200); }
}

static void audio(void) {}

static void draw(void) {
    fb_clear(0);
    int s = g_fbh / 180; if (s < 1) s = 1;
    int cx = g_fbw / 2;
    int n = g_n < MAXCART ? g_n : MAXCART;

    if (g_phase == P_BOOT) {
        // CVERTEX assembles out of the depth: each letter launches a few frames after the last,
        // rushing in from far away, spinning, and easing to a dead stop face-on. The stagger makes
        // it read left-to-right; the spin ending exactly at zero makes it land, not drift.
        Cam cam = { { 0, 0, -U * 50 / 10 }, 0, 0, 0 };
        static Inst bi[NLET];
        int32_t SPC = U * 88 / 100, ZSET = U * 30 / 100, STARTZ = U * 9;
        for (int i = 0; i < NLET; i++) {
            int te = (g_boot - i * STAG) * 1024 / FLY;
            int e = ease_out(te);
            int32_t z = lerp(STARTZ, ZSET, e);
            int turns = 2 + (i & 1);
            int spin = (((1024 - clamp01(te)) * turns)) & 1023;
            // once seated (e -> 1) a slow three-axis rock fades in, so the extrusion breathes and
            // the light crawls over the bevels — TITLE's wobbling C, spread across a word.
            uint32_t p = g_frame + (uint32_t)i * 90;
            int axw = (int)(((int64_t)28 * e * g_sin[(p * 2) & 1023]) >> 25);
            int ayw = (int)(((int64_t)60 * e * g_sin[((p * 3) + 212) & 1023]) >> 25);
            int azw = (int)(((int64_t)24 * e * g_sin[((p * 4) + 342) & 1023]) >> 25);
            bi[i].m = &let_m[i];
            bi[i].pos = (V3){ (int32_t)((i - 3) * SPC), 0, z };
            bi[i].ax = axw & 1023; bi[i].ay = ayw & 1023; bi[i].az = (spin + azw) & 1023;
            bi[i].scale = U * 60 / 100;
        }
        g3d_scene(bi, NLET, &cam, 0, 0, 0);
        // speed streaks toward the end, then a clean logo
        text_draw(cx - text_width("PRESS ANY KEY", s) / 2, g_fbh - 16 * s, s, "PRESS ANY KEY", 1);
        return;
    }

    static Inst inst[MAXCART + 1];
    int ni = 0;
    int32_t Yfloor = U * 178 / 100;
    Cam cam = { { 0, 0, -U * 46 / 10 }, 0, 0, 0 };

    if (g_phase == P_INSERT) {
        // The dive: the lens tips down and rises to look at a console on the floor, and the chosen
        // cart plunges into its slot — ease in, so it's slow-then-fast, a real drop.
        int t = INS_LEN - g_ins;
        int em = ease_io(clamp01(t * 1024 / (INS_LEN * 52 / 100)));   // camera settles by mid-dive, then holds
        cam.pos.y = lerp(0, U * 58 / 100, em);
        cam.pos.z = lerp(-U * 46 / 10, -U * 58 / 10, em);     // pull BACK — the console sits further off
        cam.ax = lerp(0, 66, em);                             // a gentle tilt down — enough to see the floor
        if (g_shake > 0) {                                    // the seat kicks the lens
            uint32_t r = g_frame * 2654435761u + 1u;
            cam.pos.x += (int32_t)(((int64_t)((int)(r & 255) - 128) * g_shake) >> 8);
            cam.pos.y += (int32_t)(((int64_t)((int)((r >> 8) & 255) - 128) * g_shake) >> 8);
        }
        // the console dead-centre on the floor (world x = 0) so it projects to screen centre
        int32_t slot_y = -Yfloor + U * 30 / 100;              // the slot mouth: where the cart seats
        inst[ni].m = &con_m; inst[ni].pos = (V3){ 0, -Yfloor, 0 };
        inst[ni].ax = inst[ni].ay = inst[ni].az = 0; inst[ni].scale = U; ni++;
        int ed = ease_in(t * 1024 / INS_LEN);
        int32_t cy = lerp(U * 70 / 100, slot_y + U * 52 / 100, ed);   // dive from high above into the slot
        inst[ni].m = &cart_m[g_sel]; inst[ni].pos = (V3){ 0, cy, 0 };
        inst[ni].ax = inst[ni].ay = inst[ni].az = 0; inst[ni].scale = U * 110 / 100; ni++;
        g3d_scene(inst, ni, &cam, 0, 0, 0);

        // Converge the focus lines on the ACTUAL insertion point: project the slot mouth through the
        // same (shaken) camera, so the sunburst locks onto where the cart seats — not a guessed screen
        // fraction that drifts the moment the camera moves. This is why it looked "off centre" before.
        int32_t ix = 0, iy = slot_y, iz = 0;
        ix -= cam.pos.x; iy -= cam.pos.y; iz -= cam.pos.z;
        g3d_unrot(&ix, &iy, &iz, cam.ax, cam.ay, cam.az);
        int16_t fsx, fsy; g3d_project(ix, iy, iz, &fsx, &fsy);
        int half = INS_LEN * 45 / 100;
        int fint = (t < half) ? 0 : ((g_shake > 0) ? 230 : (t - half) * 150 / (INS_LEN - SEAT - half));
        focus_lines(fsx, fsy, g_fbh * 42 / 100, fint, 2);     // big clear centre, lines at the rim
        text_draw(cx - text_width("READING", s * 2) / 2, 12 * s, s * 2, "READING", g_shake > 0 ? 4 : 2);
        return;
    }

    // Turntable dev instrument (CVX_TURNTABLE=1): the selected cart alone, centred, spinning one
    // full turn every 128 frames with a slight downward tilt — so `--ppm` at frames 0/16/32/48…
    // gives front / 3-4 / side / back views. This is the "accurate eye": a mesh has to be judged
    // from more than the one angle that happens to hide its holes and its upside-down.
    if (getenv("CVX_TURNTABLE")) {
        int ang = (int)((g_frame * 8) & 1023);
        inst[0].m = &cart_m[g_sel]; inst[0].pos = (V3){ 0, 0, 0 };
        inst[0].ax = 44; inst[0].ay = ang; inst[0].az = 0; inst[0].scale = U * 150 / 100;
        cam.pos = (V3){ 0, 0, -U * 42 / 10 };
        g3d_scene(inst, 1, &cam, 0, 0, 0);
        text_draw(4, 4, s, g_list[g_sel]->name, 2);
        return;
    }

    // The rack: carts in a row along x, the selected one at the origin — dead centre, biggest,
    // leaning at you. Each rocks on three axes at its own phase (title.c's wobble) so the light
    // crawls over the bevels and the front label gleams.
    for (int i = 0; i < n; i++) {
        int32_t off = ((int32_t)i << 16) - g_scroll;
        if (off < -4 * U || off > 4 * U) continue;
        int front = (i == g_sel);
        uint32_t p = g_frame + (uint32_t)i * 137;
        int amp = front ? 30 : 16;
        int ax = (int)(((int64_t)amp * g_sin[(p * 168 / 100) & 1023]) >> 15);
        int ay = (int)(((int64_t)(amp + 6) * g_sin[((p * 122 / 100) + 212) & 1023]) >> 15);
        int az = (int)(((int64_t)(amp - 6) * g_sin[((p * 217 / 100) + 342) & 1023]) >> 15);
        int32_t px = (int32_t)(((int64_t)off * 175) >> 7);
        inst[ni].m = &cart_m[i];
        inst[ni].pos = (V3){ px, 0, front ? -U * 30 / 100 : 0 };
        inst[ni].ax = ax & 1023; inst[ni].ay = ay & 1023; inst[ni].az = az & 1023;
        inst[ni].scale = front ? (U * 130 / 100) : (U * 90 / 100);
        ni++;
    }
    g3d_scene(inst, ni, &cam, 0, 0, 0);

    text_draw(cx - text_width("CVERTEX", s * 2) / 2, 10 * s, s * 2, "CVERTEX", 4);
    if (g_n) text_draw(cx - text_width(g_list[g_sel]->name, s * 2) / 2, g_fbh - 34 * s, s * 2, g_list[g_sel]->name, 2);
    text_draw(cx - text_width("SPACE INSERTS   ARROWS BROWSE", s) / 2, g_fbh - 14 * s, s, "SPACE INSERTS   ARROWS BROWSE", 1);
}

static uint64_t checksum(void) { return (uint64_t)((g_sel << 4) | g_phase); }

const Game game_menu = { "menu", init, tick, audio, draw, checksum };
