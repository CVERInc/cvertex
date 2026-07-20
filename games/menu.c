// menu.c — the console shell, in 3D. A rack of cartridges floats in front of you, the one in
// front turned to face you and rocking gently to catch the light; pick it and (next stage) the
// lens tips down to a console on the floor and the cart dives in. Underneath it's still the
// picker — it sets g_switch_to — wearing a machine.
//
// The wobble, the sheen ramp and the eases come out of cubeconjure.
#include <stdlib.h>   // getenv, for the turntable dev instrument
#include <string.h>   // memcpy, for the CRT power-off tube collapse
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"
#include "g3d.h"

#define U (1 << 16)

// P_RETURN and P_POWEROFF are the two-stage Esc's screens (see game.h). RETURN is the insert
// played in reverse — the cart rising back out of the console after you Esc'd out of a game —
// and POWEROFF is the CRT tube collapse the console does before the whole program quits.
// P_OPTIONS is the console's OPTIONS/SETTINGS screen — Space on the shelf opens it (Space used to
// free-spin the cart; that INSPECT toy is retired from Space, the turntable dev instrument below is
// unaffected). It's a vertical list scrolled with Up/Down and changed with Left/Right; Space or Esc
// backs out to the shelf. The settings it writes are the shared globals in core.h (g_gentle, g_coop…).
enum { P_BOOT, P_SHELF, P_INSERT, P_RETURN, P_POWEROFF, P_OPTIONS };
static int g_sel, g_cool, g_phase, g_ins, g_boot;
static int g_ret, g_poff;                // reverse-insert countdown; CRT power-off frame counter
static int g_flip, g_fliptarget, g_prev_up;   // flip the selected cart over (Up) to read its developer
static int g_spin_x, g_spin_y, g_prev_insp;   // (retired) INSPECT free-spin state; kept so the idle-decay math below no-ops harmlessly
static int g_prev_jump;                  // Space rising-edge latch: open OPTIONS from the shelf / back out of it
// OPTIONS panel: the rows, the cursor, and the Left/Right + Up/Down edge latches.
enum { OPT_GENTLE, OPT_COOP, OPT_FULLSCREEN, OPT_CAMERA, OPT_CRT, OPT_BACK, OPT_N };
static int g_opt_sel, g_opt_prev_x, g_opt_prev_y;
static int32_t g_scroll;                 // smoothed shelf index (16.16), chasing g_sel
static uint32_t g_frame;

#define BOOT_LEN 104          // frames the "CVERTEX" fly-in runs before the shelf takes over
#define FLY      48           // frames one letter spends in flight
#define STAG      7           // frames between successive letters launching

static const Game *const *g_list;
static int g_n;
void menu_populate(const Game *const *list, int n) { g_list = list; g_n = n; }

#define INS_LEN 140          // total P_INSERT frames: a long, admirable dive + a seated HOLD beat
#define SEAT    44           // frames the seated HOLD lasts — also = frames before the end the cart hits home,
                             //   so the DIVE (INS_LEN-SEAT = 96 frames) descends, then it lingers plugged in
#define RET_LEN 72           // P_RETURN frames: the reverse-insert, the cart rising back out of the slot
#define POFF_V   16          // CRT power-off: frames the picture squashes vertically to a bright line
#define POFF_H   24          // ...then the line shrinks horizontally to a dot by here...
#define POFF_LEN 32          // ...then the dot fades to black; the platform quits when g_poff hits this
#define MAXCART 32

// ---- the cartridge mesh: a chamfered slab, not a cube — and LANDSCAPE, lying on its long edge the
// way a Famicom/Game Boy cart does, wider than it is tall. Eight-sided cross-section (corners cut at
// 45, the 90s idea of "moulded"), extruded to a shallow depth: a front face, a back, and a ring of
// narrow bevels catching light as it turns. The connector foot runs along the BOTTOM long edge — the
// edge that plugs down into the console's slot. On the near face it wears the moulded language of a
// coveted 90s cart, all in the CONSOLE's palette so the two read as one product family: a proud PURPLE
// screen BEZEL ringing the neon label, four dark corner SCREWS (through-drilled so heads show front AND
// back), a brass BADGE chip above the screen, a brass CONTACT band across the connector, and a moulded
// grip RAIL down each flank. One template; per-cart copies just recolour the label band. --------------
// Box layout after the rings+label (verts 0..19, tris 0..29): each box is 8 verts / 12 tris via box_into.
//  20 connector | 28 bezel-top | 36 bezel-bot | 44 bezel-L | 52 bezel-R | 60 screwTL | 68 screwTR
//  76 screwBL | 84 screwBR | 92 badge | 100 contacts
//  tris: 30 conn | 42 bez-T | 54 bez-B | 66 bez-L | 78 bez-R | 90 sTL | 102 sTR | 114 sBL | 126 sBR
//        138 badge | 150 contacts
#define CV 116                           // 8 front + 8 back + 4 label + 11 boxes*8 + 8 chamfer ring
#define CT 178                           // 6 front + 6 back + 16 sides + 2 label + 11 boxes*12 + 16 chamfer
static V3   cart_v[MAXCART][CV];
static Tri  cart_t[MAXCART][CT];
static Mesh cart_m[MAXCART];
static V3   tmpl_v[CV];
static Tri  tmpl_t[CT];
// label plane metrics, published so the 3D name/developer text can land ON the same rectangle the
// neon sticker occupies — front name on the -z label, developer mirrored on the +z back.
static int32_t g_lw, g_lt, g_lb, g_ld;
static void box_into(V3 *v, Tri *t, int vbase, int32_t ox, int32_t oy, int32_t oz,
                     int32_t hx, int32_t hy, int32_t hz, uint8_t ci);

// The eight points of a chamfered rectangle: half-size (hx,hy), corners cut by fc, centred at
// y = oy. Same silhouette the cart body uses, borrowed here to frame the screen.
static void chamf8(int32_t *px, int32_t *py, int32_t hx, int32_t hy, int32_t fc, int32_t oy) {
    int32_t x[8] = {  hx,  hx - fc, -(hx - fc), -hx,  -hx, -(hx - fc),  hx - fc,  hx      };
    int32_t y[8] = {  hy - fc,  hy,  hy,  hy - fc,  -(hy - fc), -hy, -hy, -(hy - fc) };
    for (int i = 0; i < 8; i++) { px[i] = x[i]; py[i] = y[i] + oy; }
}

static void build_template(void) {
    const int32_t w = U * 90 / 100, h = U * 56 / 100, c = U * 7 / 100, d = U * 38 / 100;   // landscape (thicker = holds like an object)
    // fc: the FRONT CHAMFER, a 45° ring between the face and the flank (verts 108.., tris 162..).
    // The old cross-section chamfer only cut the silhouette's corners — every ring face still
    // pointed dead sideways (nz=0), so under any light the flanks were one flat tone. A real
    // chamfer strip carries normals halfway between face and flank, which is exactly the band
    // the key light runs along as the cart wobbles. Same depth as c so the moulding reads as one.
    const int32_t fc = U * 7 / 100;
    // eight cross-section points, corners chamfered by c, clockwise from top-right
    int32_t px[8] = {  w,      w - c, -(w - c), -w,     -w,        -(w - c),  w - c,  w      };
    int32_t py[8] = {  h - c,  h,      h,        h - c, -(h - c),  -h,       -h,    -(h - c) };
    for (int i = 0; i < 8; i++) {
        tmpl_v[i]     = (V3){ px[i], py[i],  d };     // far ring (+z, away from the player)
        tmpl_v[i + 8] = (V3){ px[i], py[i], -(d - fc) };   // near OUTER ring — the chamfer's break line
    }
    int nt = 0;                                       // Tri is { a,b,c, ci, nx,ny,nz }
    // 🔴 The normal is the ONE source of truth for culling and light (g3d.c). The ring at +d is the
    // FAR face — its outward normal is +z (away from the camera), so it culls. The ring at -d is the
    // NEAR face the player sees — normal -z. Getting these backwards culls the near wall instead: you
    // then see THROUGH the cartridge to its far wall, with the label floating over a hollow gap.
    for (int i = 1; i < 7; i++)                        // +d ring: far face, normal +z (away, culled)
        tmpl_t[nt++] = (Tri){ (uint16_t)0, (uint16_t)i, (uint16_t)(i + 1), 0, 0, 0, 32767 };
    for (int i = 1; i < 7; i++)                        // near face fan, on the INSET ring at -d (verts 108..115)
        tmpl_t[nt++] = (Tri){ (uint16_t)108, (uint16_t)(108 + i + 1), (uint16_t)(108 + i), 0, 0, 0, -32767 };
    for (int i = 0; i < 8; i++) {                     // the bevel ring
        int j = (i + 1) & 7;
        int32_t ex = py[i] - py[j], ey = px[j] - px[i];          // outward edge normal, in xy
        int32_t mag = ex < 0 ? -ex : ex, my = ey < 0 ? -ey : ey; mag = mag > my ? mag : my;
        if (mag == 0) mag = 1;
        int16_t nx = (int16_t)((int64_t)ex * 30000 / mag), ny = (int16_t)((int64_t)ey * 30000 / mag);
        tmpl_t[nt++] = (Tri){ (uint16_t)i, (uint16_t)(i + 8), (uint16_t)(j + 8), 0, nx, ny, 0 };
        tmpl_t[nt++] = (Tri){ (uint16_t)i, (uint16_t)(j + 8), (uint16_t)j,       0, nx, ny, 0 };
    }
    // the label sticker: a wide rect on the camera-facing face, centred a touch high — the cue that
    // says "cartridge", not "gem". Same face the front fan showed, a hair proud so it can't fight. Its
    // metrics are published so the 3D name text can be laid across exactly this rectangle.
    int32_t lw = w * 82 / 100, lt = h * 56 / 100, lb = -h * 50 / 100;
    g_lw = lw; g_lt = lt; g_lb = lb; g_ld = d;
    // The screen QUAD is bigger than the text's label box and sits a touch further back than the
    // bezel's front, so the frame overlaps the glass edges — the screen tucks UNDER the moulding
    // the way a real one does, no gap between picture and frame. It stays a rectangle (a screen is a
    // rectangle); the pretty chamfered 口 is the frame, and it presses onto the screen's corners.
    // The text metrics above are left at the label box, so the name doesn't grow with the glass.
    int32_t sw = lw + U * 6 / 100, st = lt + U * 6 / 100, sb = lb - U * 6 / 100, sz = -(d + U * 2 / 100);
    tmpl_v[16] = (V3){ -sw, sb, sz }; tmpl_v[17] = (V3){ sw, sb, sz };
    tmpl_v[18] = (V3){ sw, st, sz };  tmpl_v[19] = (V3){ -sw, st, sz };
    tmpl_t[nt++] = (Tri){ 16, 17, 18, 0, 0, 0, -32767 };
    tmpl_t[nt++] = (Tri){ 16, 18, 19, 0, 0, 0, -32767 };
    // ---- the moulded detail that turns a plain slab into a coveted cart. All in box_into geometry,
    // recoloured per-element in build_carts, sharing the CONSOLE's palette so the family reads as one.
    int32_t cy = (lt + lb) / 2;                             // vertical centre of the label
    // the connector foot along the BOTTOM long edge — the darker edge that plugs into the console
    // slot. THE thing that says "cartridge, and THIS edge goes in", giving the slab an up and a down.
    // Wide and thin so it mates with the console's horizontal mouth. Verts 20..27, tris 30..41.
    box_into(&tmpl_v[20], &tmpl_t[30], 20, 0, -h - U * 3 / 100, 0, w * 68 / 100, U * 7 / 100, d * 80 / 100, 8);
    // BEZEL: ONE closed frame — a 口, not a 井. The four rails it replaced overran each other so
    // eight rail-ends stuck out at the corners; this is a single moulded surround with an outer edge
    // and an inner window, both chamfered, extruded proud of the face. The ring's TOP faces the lens;
    // its INNER wall is the rim the screen shines on (Package A lights the whole rim now, not four
    // separate bars). Verts 28..59, tris 42..89 — the same budget the rails used.
    //   28..35 outer-proud   36..43 inner-proud   44..51 outer-base   52..59 inner-base
    //   tris 42..57 top ring   58..73 inner wall (glows)   74..89 outer wall
    int32_t bhz = U * 4 / 100, zt = -d - bhz, zb = -d;     // proud toward the lens, base on the face
    int32_t half_h = (lt - lb) / 2, bfc = U * 4 / 100;
    int32_t oxp[8], oyp[8], ixp[8], iyp[8];
    chamf8(oxp, oyp, lw + U * 9 / 100, half_h + U * 9 / 100, bfc, cy);   // outer edge
    chamf8(ixp, iyp, lw + U * 2 / 100, half_h + U * 2 / 100, bfc, cy);   // inner window (screen shows through)
    for (int i = 0; i < 8; i++) {
        tmpl_v[28 + i] = (V3){ oxp[i], oyp[i], zt };
        tmpl_v[36 + i] = (V3){ ixp[i], iyp[i], zt };
        tmpl_v[44 + i] = (V3){ oxp[i], oyp[i], zb };
        tmpl_v[52 + i] = (V3){ ixp[i], iyp[i], zb };
    }
    int bt = 42;
    for (int i = 0; i < 8; i++) {                          // top ring: faces the lens (-z)
        int j = (i + 1) & 7;
        tmpl_t[bt++] = (Tri){ (uint16_t)(28 + i), (uint16_t)(28 + j), (uint16_t)(36 + j), 0, 0, 0, -32767 };
        tmpl_t[bt++] = (Tri){ (uint16_t)(28 + i), (uint16_t)(36 + j), (uint16_t)(36 + i), 0, 0, 0, -32767 };
    }
    for (int i = 0; i < 8; i++) {                          // inner wall: normal points INWARD, the lit rim
        int j = (i + 1) & 7;
        int32_t ex = iyp[i] - iyp[j], ey = ixp[j] - ixp[i];
        int32_t mag = ex < 0 ? -ex : ex, my = ey < 0 ? -ey : ey; mag = mag > my ? mag : my; if (!mag) mag = 1;
        int16_t nx = (int16_t)(-(int64_t)ex * 30000 / mag), ny = (int16_t)(-(int64_t)ey * 30000 / mag);
        tmpl_t[bt++] = (Tri){ (uint16_t)(36 + i), (uint16_t)(52 + i), (uint16_t)(52 + j), 0, nx, ny, 0 };
        tmpl_t[bt++] = (Tri){ (uint16_t)(36 + i), (uint16_t)(52 + j), (uint16_t)(36 + j), 0, nx, ny, 0 };
    }
    for (int i = 0; i < 8; i++) {                          // outer wall: normal points OUTWARD
        int j = (i + 1) & 7;
        int32_t ex = oyp[i] - oyp[j], ey = oxp[j] - oxp[i];
        int32_t mag = ex < 0 ? -ex : ex, my = ey < 0 ? -ey : ey; mag = mag > my ? mag : my; if (!mag) mag = 1;
        int16_t nx = (int16_t)((int64_t)ex * 30000 / mag), ny = (int16_t)((int64_t)ey * 30000 / mag);
        tmpl_t[bt++] = (Tri){ (uint16_t)(28 + i), (uint16_t)(44 + i), (uint16_t)(44 + j), 0, nx, ny, 0 };
        tmpl_t[bt++] = (Tri){ (uint16_t)(28 + i), (uint16_t)(44 + j), (uint16_t)(28 + j), 0, nx, ny, 0 };
    }
    // corner SCREWS: dark rods drilled through the whole slab (oz=0, depth d), so a tiny head stands
    // proud on the near face AND the far face — very '90s, and it de-bares the back. Verts 60..91.
    int32_t sx = w - c - U * 8 / 100, sy = h - c - U * 6 / 100, srz = d + U * 2 / 100;
    box_into(&tmpl_v[60], &tmpl_t[90],   60, -sx,  sy, 0, U * 3 / 100, U * 3 / 100, srz, 8);   // TL
    box_into(&tmpl_v[68], &tmpl_t[102],  68,  sx,  sy, 0, U * 3 / 100, U * 3 / 100, srz, 8);   // TR
    box_into(&tmpl_v[76], &tmpl_t[114],  76, -sx, -sy, 0, U * 3 / 100, U * 3 / 100, srz, 8);   // BL
    box_into(&tmpl_v[84], &tmpl_t[126],  84,  sx, -sy, 0, U * 3 / 100, U * 3 / 100, srz, 8);   // BR
    // brand BADGE: a small brass emblem chip on the top margin, centred above the screen. Verts 92..99.
    box_into(&tmpl_v[92], &tmpl_t[138],  92, 0, lt + U * 12 / 100, -d - U * 3 / 100, U * 11 / 100, U * 4 / 100, U * 3 / 100, 8);
    // brass CONTACTS: a gold band across the connector's near face — the edge that mates with the slot,
    // the one warm-metal note that says "this plugs in." Verts 100..107.
    box_into(&tmpl_v[100], &tmpl_t[150], 100, 0, -h - U * 5 / 100, -(d * 80 / 100) - U * 1 / 100, w * 58 / 100, U * 3 / 100, U * 2 / 100, 8);
    // the front chamfer itself: an inset copy of the cross-section carries the face at -d, and
    // sixteen quad-halves bridge it to the outer ring at -(d-fc). Normals are the flank's xy
    // direction folded 45° toward the lens (×0.707, z=-23170) — hand-set like every other normal
    // here, because the normal is the one source of truth for culling AND light (see the 🔴 above).
    // Appended at the tail so every box index and build_carts recolour range stays put; the new
    // tris fall through build_carts' final else into the glossy body ramp, which is the intent.
    for (int i = 0; i < 8; i++) {
        int32_t ix = px[i] > 0 ? px[i] - fc : px[i] + fc;
        int32_t iy = py[i] > 0 ? py[i] - fc : py[i] + fc;
        tmpl_v[108 + i] = (V3){ ix, iy, -d };
    }
    for (int i = 0; i < 8; i++) {
        int j = (i + 1) & 7;
        int32_t ex = py[i] - py[j], ey = px[j] - px[i];          // outward edge normal, in xy
        int32_t mag = ex < 0 ? -ex : ex, my = ey < 0 ? -ey : ey; mag = mag > my ? mag : my;
        if (mag == 0) mag = 1;
        int16_t nx = (int16_t)((int64_t)ex * 21213 / mag), ny = (int16_t)((int64_t)ey * 21213 / mag);
        tmpl_t[162 + i * 2]     = (Tri){ (uint16_t)(i + 8), (uint16_t)(108 + i), (uint16_t)(108 + j), 0, nx, ny, -23170 };
        tmpl_t[162 + i * 2 + 1] = (Tri){ (uint16_t)(i + 8), (uint16_t)(108 + j), (uint16_t)(j + 8),   0, nx, ny, -23170 };
    }
    // (The flank GRIP rails are gone: being body-coloured they read as no decoration at all, just a
    // pair of thin same-colour standoffs whose shaded edges flickered as dark cracks — "不明黑線" — as
    // the cart wobbled, and aliased into slivers edge-on during a flip. The coveted read comes from the
    // dark bezel, brass badge + contacts, drilled screws and the neon screen; the rails only added noise.)
}

// an 8-shade ramp from one rgb, at base, so flat faces read as a lit gloss.
static void ramp(int base, uint32_t rgb) {
    for (int i = 0; i < 8; i++) {
        int m = 45 + i * 28;
        int r = (int)((rgb >> 16) & 255) * m / 255, gg = (int)((rgb >> 8) & 255) * m / 255, b = (int)(rgb & 255) * m / 255;
        if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
    }
}
// A glossier ramp than ramp(): a darker floor for more contrast, and the top two shades bloom toward
// WHITE — a specular hotspot, not just a brighter tint. As a face turns to the camera the renderer's
// normal-shade lands on those top shades, so a bright highlight crawls across the surface as the cart
// wobbles — cubeconjure's obvious sheen, on a slab that only rocks instead of tumbling.
static void ramp_gloss(int base, uint32_t rgb) {
    int br = (int)((rgb >> 16) & 255), bg = (int)((rgb >> 8) & 255), bb = (int)(rgb & 255);
    for (int i = 0; i < 8; i++) {
        int m = 24 + i * 26;                                   // darker floor than ramp() -> wider tonal range
        int r = br * m / 255, g = bg * m / 255, b = bb * m / 255;
        if (i >= 6) {                                          // top two shades bloom to a near-white specular hotspot
            int s = (i - 5) * 98;                              // 98, 196
            r += (255 - r) * s / 255; g += (255 - g) * s / 255; b += (255 - b) * s / 255;
        }
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        g_pal[base + i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}
// An EMITTER's ramp: all eight steps the same colour. The renderer still adds its light-driven
// shade and every shade lands on the same value, so the surface holds its brightness however the
// cart turns. This is emission spelled in the only vocabulary a palette engine has — and it is the
// physics we were getting wrong: a screen does not go dim because you angled it away from the room's
// lamp. Same trick as the baked shadow on the console, pointed the other way.
static void ramp_flat(int base, uint32_t rgb, int m) {
    int r = (int)((rgb >> 16) & 255) * m / 255, g = (int)((rgb >> 8) & 255) * m / 255, b = (int)(rgb & 255) * m / 255;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    for (int i = 0; i < 8; i++) g_pal[base + i] = c;
}
// tech palette: the body is one dark gunmetal for every cart, the connector a darker metal, and the
// only colour is the label — a neon "screen" that tells the carts apart. Reads as console hardware,
// not candy.
static const uint32_t NEON_RGB[6] = { 0xFF22D3EE, 0xFF41A6F6, 0xFF8B5CF6, 0xFFEC4899, 0xFF34D399, 0xFFF6C453 };
#define LABEL_M 213          // the screens' brightness — ramp()'s old step 6: lit, not blown out
#define GLOW    176          // 176..223: six ramps of what each screen throws onto its own bezel
#define GLOW_M  112          // ~44% of the neon. A wall this close to a lit screen catches a lot;
                             // at 30% it read as a slightly-off shade of bezel rather than as light.

static void build_carts(void) {
    build_template();
    ramp(8,  0xFF12151E);                             // 8..15  connector (near-black metal)
    ramp_gloss(16, 0xFF9A9488);                       // 16..23 body — warm moulded grey with a white specular hotspot so the sheen crawls
    // 24..71 six neon label ramps, and 176..223 the glow each throws on its bezel — all FLAT, because
    // both are the screen: one is the tube, the other is what the tube lights up. See ramp_flat.
    for (int c = 0; c < 6; c++) ramp_flat(24 + c * 8, NEON_RGB[c], LABEL_M);
    for (int c = 0; c < 6; c++) ramp_flat(GLOW + c * 8, NEON_RGB[c], GLOW_M);
    ramp_flat(136, 0xFF0A0912, LABEL_M);              // 136..143 name ink — flat too, so the ink's contrast
                                                      // against its screen is the same from every angle
    ramp(144, 0xFF141019);                            // 144..151 developer ink — DARK now: the body's specular sheen goes near-white, so white ink vanished on it; dark ink reads on every body shade
    ramp_gloss(152, 0xFFC9A24B);                       // 152..159 brass — badge + connector contacts, a specular gold that glints as the cart wobbles
    ramp(160, 0xFF262433);                             // 160..167 screen BEZEL — a neutral dark moulded-plastic frame. NOT the console purple: the purple label (TUBE) is 0xFF8B5CF6, exactly the old bezel colour, so purple-on-purple vanished. A dark frame makes every neon screen pop. 🔴 160 not 128: the console's fins own 128, and build_console() runs after build_carts() — sharing that slot silently painted the bezel with the fins' colour.
    // the screws reuse the connector's near-black metal (8); the bezel is its own dark plastic (128),
    // so a bright screen always reads against it no matter which of the six neon colours a cart wears.
    int n = g_n < MAXCART ? g_n : MAXCART;
    for (int i = 0; i < n; i++) {
        for (int v = 0; v < CV; v++) cart_v[i][v] = tmpl_v[v];
        for (int t = 0; t < CT; t++) {
            cart_t[i][t] = tmpl_t[t];
            uint8_t ci;
            if      (t == 28 || t == 29)  ci = (uint8_t)(24 + (i % 6) * 8);   // neon label
            else if (t >= 30 && t < 42)   ci = 8;                            // connector foot (near-black metal)
            else if (t >= 42 && t < 90)   ci = 160;                          // screen bezel (neutral dark plastic — frames any neon screen)
            else if (t >= 90 && t < 138)  ci = 8;                            // corner screws (same dark metal)
            else if (t >= 138 && t < 162) ci = 152;                          // brass badge + contacts
            else                          ci = 16;                           // body + bevel ring
            cart_t[i][t].ci = ci;
        }
        // The rim the screen shines on: the bezel frame's whole INNER wall (tris 58..73). Without
        // this the emissive label reads as a sticker — a bright rectangle that lights nothing is a
        // print, not a screen. The single-frame bezel means the glow is one continuous rim now,
        // chamfered corners and all, instead of four separate bars.
        uint8_t gi = (uint8_t)(GLOW + (i % 6) * 8);
        for (int t = 58; t <= 73; t++) cart_t[i][t].ci = gi;
        cart_m[i].v = cart_v[i]; cart_m[i].nv = CV; cart_m[i].t = cart_t[i]; cart_m[i].nt = CT;
    }
}

// ---- the console it dives into. Not two boxes any more but a chunky, tiered, injection-moulded
// deck a 90s kid would covet: a wide warm-grey body stepped up through a plinth to a raised top deck,
// a recessed dark cartridge MOUTH framed by a moulded bezel (four rails around a real rectangular
// hole, so the cart's connector edge visibly drops INTO it), a purple accent stripe, a red power LED
// and a row of cooling fins across the back. All boxes + bevelled tiers + the fake-AO ramp; no
// bitmap anywhere. Twelve boxes, so 96 verts and 144 tris. -----------------------------------------
static V3   con_v[96];
static Tri  con_t[144];
static Mesh con_m;
// The console's reflection on the black-glass floor — a dark, dim, upside-down twin built once
// from con_m. It only comes into frame during the dive, when the camera tips down toward the
// floor; on the level shelf it sits below the bottom edge and never draws. The magazine-cover
// touch the insert deserves, for the price of one mirrored mesh.
static V3   con_refl_v[96];
static Tri  con_refl_t[144];
static Mesh con_refl_m;
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
    ramp(72,  0xFFB6B2A4);   // 72..79  body — warm moulded grey (the coveted '90s off-white-grey)
    ramp(80,  0xFF0A0912);   // 80..87  the dark cartridge mouth
    ramp(96,  0xFF5C584E);   // 96..103 base plinth — a darker footing under the body
    ramp(104, 0xFFCAC6B8);   // 104..111 deck bezel rails — a shade lighter, catches the top light
    ramp(112, 0xFF8B5CF6);   // 112..119 accent stripe — the one bold colour, a purple flash
    ramp(120, 0xFFEF4444);   // 120..127 power LED — a hot red pinprick
    ramp(128, 0xFF33312B);   // 128..135 cooling fins — dark moulded ridges
    // Stepped tiers give the moulded silhouette: base 175 wide > body 160 > deck 145, each shorter in
    // depth too, so the eye reads cast plastic, not a stacked pair of cubes.
    box_into(&con_v[0],  &con_t[0],   0,  0, -U * 42 / 100,  0,          U * 175 / 100, U * 13 / 100, U * 105 / 100, 96);   // 0 base plinth
    box_into(&con_v[8],  &con_t[12],  8,  0,  0,             0,          U * 160 / 100, U * 30 / 100, U * 95 / 100,  72);   // 1 main body
    // The bezel: four rails ring a rectangular hole (opening ±95 in x, ±34 in z). The even-odd of real
    // geometry — a frame with a gap — is what makes it a SLOT and not a painted-on dark rectangle.
    // Slot opening enlarged ~14% to match the redesigned cart: the hole reads as sized for THIS
    // cartridge, not a token gap. Rails' inner faces sit at ±108 x / ±0.41 z, framing the wider mouth;
    // outer edge ±158 still tucks inside the ±160 body. Bump the mouth to match, or the cart plugs
    // into a frame bigger than its own pit.
    box_into(&con_v[16], &con_t[24],  16, -U * 133 / 100, U * 38 / 100, 0,           U * 25 / 100,  U * 8 / 100,  U * 96 / 100, 104);   // 2 left rail
    box_into(&con_v[24], &con_t[36],  24,  U * 133 / 100, U * 38 / 100, 0,           U * 25 / 100,  U * 8 / 100,  U * 96 / 100, 104);   // 3 right rail
    box_into(&con_v[32], &con_t[48],  32,  0,             U * 38 / 100, -U * 663 / 1000, U * 108 / 100, U * 8 / 100, U * 255 / 1000, 104); // 4 front rail
    box_into(&con_v[40], &con_t[60],  40,  0,             U * 38 / 100,  U * 663 / 1000, U * 108 / 100, U * 8 / 100, U * 255 / 1000, 104); // 5 back rail
    // The mouth: a dark pit whose top (16+26=42) sits BELOW the bezel top (38+8=46), so you look down
    // into a recess. The cart's connector edge descends into exactly this box — widened with the frame.
    box_into(&con_v[48], &con_t[72],  48,  0,  U * 16 / 100, 0,          U * 108 / 100,  U * 26 / 100, U * 39 / 100, 80);    // 6 dark mouth
    box_into(&con_v[56], &con_t[84],  56,  0,  U * 12 / 100, -U * 97 / 100, U * 140 / 100, U * 5 / 100, U * 3 / 100, 112);  // 7 accent stripe (front)
    box_into(&con_v[64], &con_t[96],  64, -U * 120 / 100, -U * 13 / 100, -U * 98 / 100, U * 7 / 100, U * 6 / 100, U * 3 / 100, 120); // 8 power LED (front)
    box_into(&con_v[72], &con_t[108], 72,  0,  U * 47 / 100, U * 45 / 100, U * 70 / 100, U * 3 / 100, U * 3 / 100, 128);   // 9  fin
    box_into(&con_v[80], &con_t[120], 80,  0,  U * 47 / 100, U * 58 / 100, U * 70 / 100, U * 3 / 100, U * 3 / 100, 128);   // 10 fin
    box_into(&con_v[88], &con_t[132], 88,  0,  U * 47 / 100, U * 71 / 100, U * 70 / 100, U * 3 / 100, U * 3 / 100, 128);   // 11 fin
    // Baked contact shadow — the maintainer's "烤陰影 via palette": faces sitting in an occluded
    // pocket get a ramp DARKER than the light alone would give, the way a lightmap texture bakes AO.
    // The same trick used for concave corners elsewhere in this tree (a second darker ramp), brought to the console
    // so its tiers read as genuinely stacked, not painted flat. box_into's face order is
    // +X,-X,+Y(top),-Y,+Z,-Z, two tris each, so a box's TOP face is tris [4],[5] off its base.
    ramp(168, 0xFF302E2A);                                  // 168..175 baked shadow — a deep neutral grey. 🔴 168 (not 136): the cart's name ink owns 136 and co-draws with the console in the dive; every menu material must be disjoint.
    con_t[4].ci  = con_t[5].ci  = 168;   // base plinth TOP: the ring around the body sits in the body's contact shadow
    con_t[16].ci = con_t[17].ci = 168;   // main body TOP (the deck): under the bezel + mouth furniture, an occluded well
    con_m.v = con_v; con_m.nv = 96; con_m.t = con_t; con_m.nt = 144;

    // The reflection: the same console mirrored about its base plane and painted one dim colour.
    // The base plinth sits centred at y=-0.42U with half-height 0.13U, so its underside — the
    // plane it stands on — is y=-0.55U; a vertex reflects to y' = 2·(-0.55U) − y. A reflection
    // inverts a normal's vertical component, so ny flips (nx,nz keep) and culling stays honest.
    // 224..231 is a dark ramp of its own (176..223 belong to the carts' glow), dim enough that the
    // twin reads as a reflection on black glass and not a second console.
    ramp(224, 0xFF241F33);
    int32_t plane = -U * 110 / 100;                        // 2 × the base plane, precomputed
    for (int i = 0; i < 96; i++) {
        con_refl_v[i] = con_v[i];
        con_refl_v[i].y = plane - con_v[i].y;
    }
    for (int i = 0; i < 144; i++) {
        con_refl_t[i] = con_t[i];
        con_refl_t[i].ny = (int16_t)(-con_t[i].ny);
        con_refl_t[i].ci = 224;
    }
    con_refl_m.v = con_refl_v; con_refl_m.nv = 96; con_refl_m.t = con_refl_t; con_refl_m.nt = 144;
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

// ---- the game's NAME and DEVELOPER, as real geometry ON the selected cart. Not a screen overlay:
// every lit pixel of the 5x7 font becomes a tiny quad in the cart's LOCAL space on the label plane,
// collected into one dynamic Mesh and drawn as an extra Inst that SHARES the selected cart's exact
// transform. So g3d rotates it, lights it and culls it for free — the name rides the cart's wobble
// and skew, and disappears the instant the cart turns its back. Name on the -z (neon) face; developer
// mirrored on the +z (back) face so it reads the right way round once the cart is flipped over. -----
#define TMAXV 6000
#define TMAXT 3000
static V3   txt_v[TMAXV];
static Tri  txt_t[TMAXT];
static Mesh txt_m;                       // the SELECTED cart's flip side: BY <author>
static int  g_txt_sel = -1;              // which cart's credit is currently baked

// Every cart wears its own name, so the shelf reads like a shelf of labelled boxes instead of one
// legible cart flanked by anonymous slabs. A name never changes at runtime, so each is baked once
// into its own little mesh at init — the same deal the cart meshes get — and the rack then draws all
// of them for nothing. Pools live in __bss: no disk cost, and the longest name in the roster fills
// about a quarter of one. An over-long name simply stops emitting (px_quad's guard) rather than
// scribbling into the next cart's.
#define NMV 2048
#define NMT 1024
static V3   nam_v[MAXCART][NMV];
static Tri  nam_t[MAXCART][NMT];
static Mesh nam_m[MAXCART];

// The mesh currently under construction. The text builder fills whichever one it is pointed at.
static Mesh *tb_m; static V3 *tb_v; static Tri *tb_t; static int tb_maxv, tb_maxt;
static void tb_begin(Mesh *m, V3 *v, Tri *t, int maxv, int maxt) {
    tb_m = m; tb_v = v; tb_t = t; tb_maxv = maxv; tb_maxt = maxt;
    m->v = v; m->t = t; m->nv = 0; m->nt = 0;
}

// the 5x7 font, one byte per column, bit0 = top — the subset the names need (caps, digits, - .),
// copied from text.c. Adding a glyph is adding five numbers.
static const struct { char c; uint8_t col[5]; } FONT[] = {
    { 'A', { 0x7E,0x11,0x11,0x11,0x7E } }, { 'B', { 0x7F,0x49,0x49,0x49,0x36 } },
    { 'C', { 0x3E,0x41,0x41,0x41,0x22 } }, { 'D', { 0x7F,0x41,0x41,0x22,0x1C } },
    { 'E', { 0x7F,0x49,0x49,0x49,0x41 } }, { 'F', { 0x7F,0x09,0x09,0x09,0x01 } },
    { 'G', { 0x3E,0x41,0x49,0x49,0x7A } }, { 'H', { 0x7F,0x08,0x08,0x08,0x7F } },
    { 'I', { 0x00,0x41,0x7F,0x41,0x00 } }, { 'J', { 0x20,0x40,0x41,0x3F,0x01 } },
    { 'K', { 0x7F,0x08,0x14,0x22,0x41 } }, { 'L', { 0x7F,0x40,0x40,0x40,0x40 } },
    { 'M', { 0x7F,0x02,0x0C,0x02,0x7F } }, { 'N', { 0x7F,0x04,0x08,0x10,0x7F } },
    { 'O', { 0x3E,0x41,0x41,0x41,0x3E } }, { 'P', { 0x7F,0x09,0x09,0x09,0x06 } },
    { 'Q', { 0x3E,0x41,0x51,0x21,0x5E } }, { 'R', { 0x7F,0x09,0x19,0x29,0x46 } },
    { 'S', { 0x46,0x49,0x49,0x49,0x31 } }, { 'T', { 0x01,0x01,0x7F,0x01,0x01 } },
    { 'U', { 0x3F,0x40,0x40,0x40,0x3F } }, { 'V', { 0x1F,0x20,0x40,0x20,0x1F } },
    { 'W', { 0x7F,0x20,0x18,0x20,0x7F } }, { 'X', { 0x63,0x14,0x08,0x14,0x63 } },
    { 'Y', { 0x03,0x04,0x78,0x04,0x03 } }, { 'Z', { 0x61,0x51,0x49,0x45,0x43 } },
    { '0', { 0x3E,0x51,0x49,0x45,0x3E } }, { '1', { 0x00,0x42,0x7F,0x40,0x00 } },
    { '2', { 0x42,0x61,0x51,0x49,0x46 } }, { '3', { 0x21,0x41,0x45,0x4B,0x31 } },
    { '4', { 0x18,0x14,0x12,0x7F,0x10 } }, { '5', { 0x27,0x45,0x45,0x45,0x39 } },
    { '6', { 0x3C,0x4A,0x49,0x49,0x30 } }, { '7', { 0x01,0x71,0x09,0x05,0x03 } },
    { '8', { 0x36,0x49,0x49,0x49,0x36 } }, { '9', { 0x06,0x49,0x49,0x29,0x1E } },
    { '-', { 0x08,0x08,0x08,0x08,0x08 } }, { '.', { 0x00,0x60,0x60,0x00,0x00 } },
};
#define NFONT (int)(sizeof FONT / sizeof FONT[0])

// one lit-pixel quad, flat on the label plane at z, facing ±z (nz picks front/back).
static void px_quad(int32_t x, int32_t y, int32_t z, int32_t sz, int16_t nz, uint8_t ci) {
    if (tb_m->nv + 4 > tb_maxv || tb_m->nt + 2 > tb_maxt) return;
    int v = tb_m->nv;
    tb_v[v + 0] = (V3){ x,      y,      z };
    tb_v[v + 1] = (V3){ x + sz, y,      z };
    tb_v[v + 2] = (V3){ x + sz, y + sz, z };
    tb_v[v + 3] = (V3){ x,      y + sz, z };
    tb_t[tb_m->nt++] = (Tri){ (uint16_t)v, (uint16_t)(v + 1), (uint16_t)(v + 2), ci, 0, 0, nz };
    tb_t[tb_m->nt++] = (Tri){ (uint16_t)v, (uint16_t)(v + 2), (uint16_t)(v + 3), ci, 0, 0, nz };
    tb_m->nv += 4;
}

// lay a caps string across the label, sized so the whole word fills availw; centre its 7-row block
// on cyc. mirror flips x for the back face so the developer reads correctly once the cart is flipped.
static void emit_text(const char *s, int32_t z, int16_t nz, int mirror, uint8_t ci, int32_t availw, int32_t cyc) {
    int L = 0; for (const char *p = s; *p && L < 20; p++) L++;
    if (L == 0) return;
    int32_t pp = availw / (6 * L);                 // pixel pitch: word fills the label width
    if (pp < U / 200) pp = U / 200;
    int32_t sz = pp * 82 / 100;                    // a hair of gap between pixels -> reads as a grid
    int32_t startx = -((6 * L - 1) * pp) / 2;
    int32_t topy = cyc + pp * 7 / 2;
    int k = 0;
    for (const char *p = s; *p && k < 20; p++, k++) {
        char c = *p; if (c >= 'a' && c <= 'z') c -= 32;
        const uint8_t *col = 0;
        for (int g = 0; g < NFONT; g++) if (FONT[g].c == c) { col = FONT[g].col; break; }
        if (!col) continue;                        // space / unknown -> a blank cell (k still advances)
        for (int cx = 0; cx < 5; cx++) for (int r = 0; r < 7; r++) if (col[cx] & (1 << r)) {
            int32_t bx = startx + (k * 6 + cx) * pp;
            int32_t by = topy - (r + 1) * pp;      // row 0 is the top of the glyph
            if (mirror) bx = -bx - sz;
            px_quad(bx, by, z, sz, nz, ci);
        }
    }
}

// Every cart's name, on its own label. Baked once — names are fixed at build time.
static void build_names(void) {
    int n = g_n < MAXCART ? g_n : MAXCART;
    int32_t availw = g_lw * 2 * 92 / 100;          // fill most of the label width
    int32_t cyc    = (g_lt + g_lb) / 2;            // vertical centre of the label
    for (int i = 0; i < n; i++) {
        tb_begin(&nam_m[i], nam_v[i], nam_t[i], NMV, NMT);
        emit_text(g_list[i]->name, -(g_ld + U * 7 / 100), -32767, 0, 136, availw, cyc);  // dark ink on neon
    }
}

// The credit on the flip side, for the selected cart only — you can only turn one cart over, and
// only the one you are holding. Re-baked when the selection moves.
static void build_dev(const char *author) {
    tb_begin(&txt_m, txt_v, txt_t, TMAXV, TMAXT);
    int32_t availw = g_lw * 2 * 92 / 100;
    int32_t cyc    = (g_lt + g_lb) / 2;
    static char dev[40];
    int j = 0; dev[j++] = 'B'; dev[j++] = 'Y'; dev[j++] = ' ';
    const char *a = (author && author[0]) ? author : "ANONYMOUS";
    for (int i = 0; a[i] && j < 38; i++) dev[j++] = a[i];
    dev[j] = 0;
    emit_text(dev, (g_ld + U * 7 / 100), 32767, 1, 144, availw, cyc);      // BACK: BY <author>, light ink, mirrored
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

// Turn the currently-selected OPTIONS row by `dir` (-1 Left / +1 Right). Each row writes its shared
// global directly, so the value on screen IS the value the game/platform will read — no apply step,
// no copy to drift. Two-state rows toggle (either direction flips); CO-OP is a 3-way SOLO/HOST/JOIN.
static void options_change(int dir) {
    switch (g_opt_sel) {
        case OPT_GENTLE:     g_gentle = !g_gentle; break;                 // a survival cartridge's taming dial, at runtime
        case OPT_COOP:       g_coop = (g_coop + dir + 3) % 3; break;      // 0 SOLO / 1 HOST / 2 JOIN — platform stands up net at launch
        case OPT_FULLSCREEN: g_fullscreen = !g_fullscreen; break;        // platform toggles the live window to match
        case OPT_CAMERA:     g_cam_chase = !g_cam_chase; break;          // a 3D game's default view
        case OPT_CRT:        g_crt_off = !g_crt_off; break;              // whether Esc-quit plays the tube collapse
        case OPT_BACK:       g_phase = P_SHELF; g_cool = 4; break;       // Left/Right on BACK also returns to the shelf
    }
}

static void init(void) {
    tables_init();
    g_frame = 0; g_cool = 0; g_ins = 0; g_boot = 0;
    g_ret = 0; g_poff = 0;
    g_flip = g_fliptarget = g_prev_up = 0;
    g_spin_x = g_spin_y = g_prev_insp = 0;
    g_prev_jump = 0;
    g_opt_sel = g_opt_prev_x = g_opt_prev_y = 0;
    // The OPTIONS settings default at runtime (never a data-segment initializer). g_crt_off = 1 means
    // the tube-collapse plays on Esc-quit — today's behaviour; the panel can switch it off. g_gentle /
    // g_coop / g_cam_chase keep their zero (off / solo / first-person) default; g_fullscreen is owned by
    // the platform (the --fullscreen flag), so the menu reads it but doesn't stamp it here.
    g_crt_off = 1;
    g_scroll = (int32_t)g_sel << 16;                 // g_sel is a persisting static → the shelf lands where we left it
    for (int i = 0; i < 256; i++) g_pal[i] = 0xFF000000;
    g_pal[0] = 0xFF0C0B14;   // deep background
    g_pal[1] = 0xFF9A96AC;   // secondary text — readable grey (the old 0xFF4A4458 was too dark on the near-black bg: the footer hint and OPTIONS' unselected rows both vanished)
    g_pal[2] = 0xFFF5F5F8;   // bright text (also the CRT power-off's hot scanline)
    g_pal[3] = 0xFF171526;   // OPTIONS panel plate — a shade off the background so it reads as a raised screen
    g_pal[4] = 0xFF41A6F6;   // title
    g_pal[5] = 0xFF2B2740;   // OPTIONS selected-row bar
    g_pal[6] = 0xFF8B5CF6;   // OPTIONS panel frame — the console's purple accent
    g_shake = 0;
    // The showroom key light: up-left-front of the lens, ~40° off axis. The engine's default
    // headlamp parks every camera-facing surface on the flat top of the cosine, where the
    // wobble can't move it a single ramp step (measured: shades 3..6 of the body ramp never
    // rendered at all — the 8-step gloss was a 2-tone). Off-axis, the same faces sit on the
    // STEEP part of the curve, so the existing wobble sweeps the highlight across the slab.
    // The platform resets this on every game switch; it is the menu's light, not the world's.
    g3d_light(-13000, 15500, -25500);
    build_carts();
    build_names();
    build_console();
    build_letters();
    // Returning from a game (Esc-in-game) enters the reverse-insert instead of the cold boot
    // fly-in: the cart is seated in the console, then rises back out to the shelf at g_sel. The
    // platform raised g_menu_return just before this init(); consume it so a later cold launch
    // still boots normally.
    if (g_menu_return) { g_menu_return = 0; g_phase = P_RETURN; g_ret = RET_LEN; }
    else                 g_phase = P_BOOT;
}

static void tick(const Input in[2]) {
    g_frame++;
    // The shell's own Esc: the platform pulses g_esc, and here — inside the menu — it means
    // "turn the console off." Start the CRT power-off from wherever we are; a second Esc while
    // it's already collapsing is ignored. (In a game Esc never reaches here: that cartridge
    // doesn't read g_esc, so the platform routes it to a return-to-menu instead.)
    if (g_esc) {
        g_esc = 0;
        // Inside OPTIONS, Esc is the "back" gesture — it closes the panel to the shelf, it does NOT
        // power the console off. Everywhere else Esc powers off: normally that's the CRT tube collapse,
        // but if the player switched CRT POWER-OFF off in OPTIONS we quit straight to black instead.
        if (g_phase == P_OPTIONS) { g_phase = P_SHELF; g_cool = 4; beep(4, 58, 90); }
        else if (g_phase != P_POWEROFF) {
            beep(3, 45, 200);
            if (g_crt_off) { g_phase = P_POWEROFF; g_poff = 0; }
            else g_quit = 1;
        }
    }
    if (g_phase == P_POWEROFF) {
        // Count the collapse; when the tube has gone dark, tell the platform to stop. tick stays
        // pure of g_running — quitting is the platform's call, exactly like a game swap.
        if (++g_poff >= POFF_LEN) g_quit = 1;
        return;
    }
    if (g_phase == P_RETURN) {
        // The reverse-insert: run the counter down and land on the shelf at the same cart. No
        // seat chunk, no g_switch_to — this is the cart LEAVING, not plugging in.
        if (--g_ret <= 0) { g_phase = P_SHELF; g_cool = 6; }
        return;
    }
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
    if (g_phase == P_OPTIONS) {
        // The console's OPTIONS screen. Up/Down move the cursor (rising edge, so a held key steps
        // once), Left/Right change the highlighted row's value, Space toggles back out to the shelf.
        // Esc-back is handled above. Browsing/flipping/inserting are all suspended while it's open.
        int ox = in[0].x + in[1].x;
        int oy = in[0].y + in[1].y;
        int jump = (in[0].jump || in[1].jump);
        int ud = (oy > 0) ? -1 : (oy < 0) ? 1 : 0;               // Up = toward the top of the list
        if (ud && !g_opt_prev_y) { g_opt_sel = (g_opt_sel + ud + OPT_N) % OPT_N; beep(5, 72, 70); }
        g_opt_prev_y = (ud != 0);
        int lr = (ox > 0) ? 1 : (ox < 0) ? -1 : 0;
        if (lr && !g_opt_prev_x) { options_change(lr); beep(4, 70, 70); }
        g_opt_prev_x = (lr != 0);
        if (jump && !g_prev_jump) { g_phase = P_SHELF; g_cool = 4; beep(4, 58, 90); }   // Space = back to the shelf
        g_prev_jump = jump;
        return;
    }

    int32_t target = (int32_t)g_sel << 16;
    g_scroll += (target - g_scroll) >> 3;
    int x = in[0].x + in[1].x;
    int y = in[0].y + in[1].y;
    // SPACE now opens the OPTIONS panel (rising edge, so a held key opens it once). It used to hold-to
    // -INSPECT the cart in a free-spin; that toy is retired from Space — the turntable dev instrument
    // (CVX_TURNTABLE) still exists for judging the mesh. The spin state decays to idle here so the old
    // math no-ops cleanly now that nothing ever winds it up.
    int jump = (in[0].jump || in[1].jump);
    if (jump && !g_prev_jump && g_n) { g_phase = P_OPTIONS; g_opt_sel = 0; beep(4, 78, 90); g_prev_jump = jump; return; }
    g_prev_jump = jump;
    g_spin_x -= g_spin_x >> 3; if (g_spin_x > -8 && g_spin_x < 8) g_spin_x = 0;   // ease the (now unused) spin back to the
    g_spin_y -= g_spin_y >> 3; if (g_spin_y > -8 && g_spin_y < 8) g_spin_y = 0;   //   gentle idle wobble
    int mv = x ? (x > 0 ? 1 : -1) : 0;                           // left/right browse the shelf
    if (g_cool > 0) g_cool--;
    else if (mv) { g_sel = (g_sel + mv + g_n) % g_n; g_cool = 9; g_fliptarget = 0; beep(5, 74, 80); }  // a new cart shows its front
    // Up flips the selected cartridge over — like turning a real cart to read the back — to show
    // who made it. Press again to flip it back. Toggle on the rising edge so a held key doesn't spin.
    int up = (in[0].y > 0) || (in[1].y > 0);
    if (up && !g_prev_up) { g_fliptarget = g_fliptarget ? 0 : 1024; beep(4, 66, 70); }
    g_prev_up = up;
    g_flip += (g_fliptarget - g_flip) >> 3;
    // Down PLUNGES the cart down into the console — the insert. (Space used to do this; it's now the
    // inspect toy, and Down is the honest gesture: you push the cart DOWN into the slot.)
    if ((in[0].y < 0 || in[1].y < 0) && g_n) { g_phase = P_INSERT; g_ins = INS_LEN; beep(3, 40, 200); }
}

static void audio(void) {}

// ---- shelf rack placement. The selected cart stands prominent up front; the rest recede into a
// row behind it. These four numbers ARE the anti-clip geometry: RACK_STEP is the x-gap between
// neighbours (they must not overlap each other) and RACK_ZBACK sinks the whole non-selected rack
// away from the camera so the big front cart can never intersect one. Placement lives in one helper
// so the (temporary) overlap ruler measures EXACTLY what draw() renders — same wobble, same scale.
#define RACK_STEP   300            // x spacing multiplier: px = off * RACK_STEP >> 7  (per-cart gap)
#define RACK_ZBACK  (U * 60 / 100) // non-selected carts pushed this far back (+z, away from the lens)
#define RACK_FZ     (-U * 30 / 100)// the selected cart leans forward toward you
#define RACK_FSCALE (U * 140 / 100)
#define RACK_BSCALE (U * 82 / 100) // the rack is a touch smaller too, reinforcing the recede
// The rack FLOATS above the console rather than sitting at the world origin: the console owns the
// floor, and lifting the shelf is what drops the console's deck into the bottom of the shelf framing
// without moving the carts off centre (the camera rides at RACK_Y too). Every cart shifts equally,
// so the anti-clip sweep above is untouched.
#define YFLOOR      (U * 178 / 100)// the floor the console stands on
#define RACK_Y      (U * 80 / 100) // how far the rack floats above the world origin. Tuned by eye:
                                   // high enough that only the console's top deck breaks into the
                                   // bottom of the shelf framing (a sliver, not a second subject,
                                   // and it stays clear of the controls hint), low enough that the
                                   // camera reaches the slot in one unhurried move.
// These four numbers were tuned by a cart-vs-cart AABB overlap ruler (an exhaustive sweep of every
// wobble phase × flip state × browse/scroll position): STEP=300 gives 2.34U of x-spacing so no two
// neighbours interpenetrate, and ZBACK sinks the rack 0.60U behind the leaning front cart. Peak
// inter-cart overlap measured 0 across ~15M pair-checks (was 0.77U before). See git history for the
// scaffold. Placement lives here in one helper so what draws is exactly what was measured.
static void place_cart(int i, int sel, uint32_t frame, int32_t scroll, int flip,
                       int spinx, int spiny, Inst *out) {
    int32_t off = ((int32_t)i << 16) - scroll;
    int front = (i == sel);
    uint32_t p = frame + (uint32_t)i * 137;
    int amp = front ? 48 : 16;   // the selected cart tilts more so its edges/top catch light — reads as a held object
    int ax = (int)(((int64_t)amp * g_sin[(p * 168 / 100) & 1023]) >> 15);
    int ay = (int)(((int64_t)(amp + 6) * g_sin[((p * 122 / 100) + 212) & 1023]) >> 15);
    int az = (int)(((int64_t)(amp - 6) * g_sin[((p * 217 / 100) + 342) & 1023]) >> 15);
    int32_t px = (int32_t)(((int64_t)off * RACK_STEP) >> 7);
    int sx = front ? spinx : 0, sy = front ? spiny : 0;
    out->m = &cart_m[i];
    out->pos = (V3){ px, RACK_Y, front ? RACK_FZ : RACK_ZBACK };
    out->ax = (ax + sx) & 1023;
    out->ay = (ay + (front ? flip / 2 : 0) + sy) & 1023;
    out->az = az & 1023;
    out->scale = front ? RACK_FSCALE : RACK_BSCALE;
}

// ---- the world -----------------------------------------------------------------------------------
// ONE place, always: the console standing on the floor with the rack of carts floating above it. A
// phase changes where the CAMERA stands and whether the selected cart is still on the shelf or in
// flight to the slot — it never changes who is in the room. That is the whole difference between a
// move and a cut, and it is why the shelf framing deliberately keeps the console's deck in the
// bottom of frame: the player can always see where a cart is about to go.
//
// (This used to be two scenes. The shelf drew a rack with no console in it; the insert drew a
// console with no rack in it, from its own camera, and the cut was disguised by starting the dive
// at the same screen position the shelf had just left. It worked, and it cost us the room — the
// console could vanish from the build entirely and nothing on the shelf noticed for a month.)
#define SEAT_Y   (-YFLOOR + U * 90 / 100)   // the cart, connector seated inside the mouth
#define SLOT_Y   (-YFLOOR + U * 42 / 100)   // the bezel mouth — what the focus lines converge on
#define SEAT_SCALE (U * 110 / 100)          // a seated cart reads smaller than the one held up front

// The camera: t = 0 is the shelf framing, t = 1024 is settled over the slot. Everything between is
// the same move played at whatever rate the phase asks for.
// Where the move ENDS is not a free choice. Rack and console are only ~2U apart in a frame that
// spans more than that at this distance, so no framing centres the console and also loses the shelf
// entirely — tip far enough to drop the rack out and the console slides down with it. So the end
// pose is tuned to put the console on the axis and skim the rack's underside along the very top
// edge: the shelf thins to a line as the camera tips, still overhead, still the same room. Lens at
// 1.30U tipped 88 (31°) lands the console at 0.05 of a half-frame and the rack at 0.48 — a hair
// inside the edge, which is a choice, where a third of a cart would have been an accident.
static void cam_at(Cam *c, int t) {
    c->pos.x = 0;
    c->pos.y = lerp(RACK_Y, U * 130 / 100, t);
    c->pos.z = lerp(-U * 46 / 10, -U * 58 / 10, t);   // pull BACK — the console sits further off
    c->ax = lerp(0, 88, t);                           // tip down — enough to see the floor, and the slot
    c->ay = c->az = 0;
}

// Angles are mod 1024, so lerping a wobble of 1000 (which is −24) toward 0 the naive way takes the
// cart most of a turn the wrong way round. Go the short way.
static int ang_lerp(int a, int b, int t) {
    int d = (b - a) & 1023;
    if (d > 512) d -= 1024;
    return (a + (int)(((int64_t)d * t) >> 10)) & 1023;
}

// The insert flight. Position, rotation AND scale lerp together, so the object the player was
// looking at is the object that lands in the slot: it stops wobbling, squares up, shrinks to seated
// size and descends, all as one movement. (The old cut had to hide a teleport across all four.)
static void fly_cart(Inst *c, int t) {
    int e = ease_in(t);
    c->pos.x = lerp(c->pos.x, 0, e);
    c->pos.y = lerp(c->pos.y, SEAT_Y, e);
    c->pos.z = lerp(c->pos.z, 0, e);
    c->ax = ang_lerp(c->ax, 0, e);
    c->ay = ang_lerp(c->ay, 0, e);
    c->az = ang_lerp(c->az, 0, e);
    c->scale = lerp(c->scale, SEAT_SCALE, e);
}

// Everything that exists, in world space. `fly` < 0 leaves the selected cart on the shelf with its
// neighbours; 0..1024 is its progress into the slot.
static int compose_world(Inst *inst, int fly) {
    int ni = 0;
    int n = g_n < MAXCART ? g_n : MAXCART;
    // the console, dead-centre on the floor (world x = 0) so it projects to screen centre
    inst[ni].m = &con_m; inst[ni].pos = (V3){ 0, -YFLOOR, 0 };
    inst[ni].ax = inst[ni].ay = inst[ni].az = 0; inst[ni].scale = U; ni++;
    // its reflection, same place — the mesh already carries the mirror. Off-screen on the level
    // shelf, it rises into frame only as the dive tips the camera toward the floor.
    inst[ni].m = &con_refl_m; inst[ni].pos = (V3){ 0, -YFLOOR, 0 };
    inst[ni].ax = inst[ni].ay = inst[ni].az = 0; inst[ni].scale = U; ni++;
    // The rack: carts in a row along x, the selected one at the origin — dead centre, biggest,
    // leaning at you. Each rocks on three axes at its own phase so the light crawls over the bevels.
    int f_seen = 0; Inst front = inst[0];
    for (int i = 0; i < n; i++) {
        int32_t off = ((int32_t)i << 16) - g_scroll;
        if (off < -4 * U || off > 4 * U) continue;
        // one placement, shared with the overlap ruler: the selected cart forward and big, the rest
        // stepped apart in x and sunk back in z so no two carts ever interpenetrate (proven, not eyed).
        place_cart(i, g_sel, g_frame, g_scroll, g_flip, g_spin_x, g_spin_y, &inst[ni]);
        if (i == g_sel) {
            if (fly >= 0) fly_cart(&inst[ni], fly);   // this one is leaving
            f_seen = 1; front = inst[ni];
        }
        // the name, sharing this cart's exact transform — so g3d rotates, lights and culls it for
        // free: every label rides its own cart's wobble, and the selected one rides the whole flight
        // into the slot. It turns away with the cart, because it is ON the cart.
        Inst c = inst[ni++];
        inst[ni] = c; inst[ni].m = &nam_m[i]; ni++;
    }
    // the credit, on the selected cart's back — visible only once the flip has turned it toward you,
    // which the backface cull decides on its own.
    if (f_seen && g_n) { inst[ni] = front; inst[ni].m = &txt_m; ni++; }
    return ni;
}

// The world, drawn. `camt` slides the camera, `fly` flies the selected cart, `fint` is the focus
// lines' intensity (0 = don't). The seat shake rides on g_shake, which only the forward insert sets.
// The tubes breathe. One slow sine (~0.5 Hz) drives both the screens and the glow they spill, with
// the spill swinging wider than the source — a lit surface shows a brightness wobble more than the
// light does. It is the difference between a screen that is on and a screen that is merely bright.
// 🔴 Palette writes belong to draw(): tick never sees a colour, so the sim stays pure.
static void breathe(void) {
    int s = g_sin[(g_frame * 9) & 1023];                     // Q15
    int lm = LABEL_M + (int)(((int64_t)s * 8) >> 15);        // ±4% on the tube
    int gm = GLOW_M  + (int)(((int64_t)s * 9) >> 15);        // ±12% on what it lights
    for (int c = 0; c < 6; c++) {
        ramp_flat(24 + c * 8, NEON_RGB[c], lm);
        ramp_flat(GLOW + c * 8, NEON_RGB[c], gm);
    }
}

static void draw_world(int camt, int fly, int fint) {
    fb_clear(0);
    breathe();
    Cam cam; cam_at(&cam, camt);
    if (g_shake > 0) {                                    // the seat kicks the lens
        uint32_t r = g_frame * 2654435761u + 1u;
        cam.pos.x += (int32_t)(((int64_t)((int)(r & 255) - 128) * g_shake) >> 8);
        cam.pos.y += (int32_t)(((int64_t)((int)((r >> 8) & 255) - 128) * g_shake) >> 8);
    }
    static Inst inst[MAXCART * 2 + 4];   // every cart brings its name along
    int ni = compose_world(inst, fly);
    g3d_scene(inst, ni, &cam, 0, 0, 0);
    if (fint <= 0) return;
    // Converge the focus lines on the ACTUAL insertion point: project the slot mouth through the same
    // (shaken) camera, so the sunburst locks onto where the cart seats — not a guessed screen fraction.
    int32_t ix = 0, iy = SLOT_Y, iz = 0;
    ix -= cam.pos.x; iy -= cam.pos.y; iz -= cam.pos.z;
    g3d_view(&ix, &iy, &iz, cam.ax, cam.ay, cam.az);   // same transform g3d_scene uses, or the lines miss
    int16_t fsx, fsy; g3d_project(ix, iy, iz, &fsx, &fsy);
    focus_lines(fsx, fsy, g_fbh * 42 / 100, fint, 2);     // big clear centre, lines at the rim
    // (No "READING" caption — the cart diving into the slot with the focus lines says it on its own.)
}

// One frame of the insert, forward (P_INSERT) or reversed (P_RETURN). `t` is the dive frame: 0 = the
// cart still on the shelf, `dive` = seated home, beyond that the seated HOLD. Run t backwards and
// the same curves lift the cart back out and hand it to the shelf. `fx` is the forward-only
// flourish — the focus lines; the reverse plays clean and quiet.
static void draw_dive(int t, int fx) {
    int dive = INS_LEN - SEAT;                            // descent frames; the rest is the seated HOLD
    int half = dive * 62 / 100;                           // camera settles by mid-dive, then holds
    // The focus lines build as the cart approaches, FLASH on the seat chunk, then fade with the shake.
    int fint = !fx ? 0
             : (t < half) ? 0
             : (g_shake > 0) ? 230
             : (t < dive) ? (t - half) * 230 / (dive - half)
             : 0;
    draw_world(ease_io(clamp01(t * 1024 / half)), clamp01(t * 1024 / dive), fint);
}

// The CRT power-off: the classic tube collapse. The picture already in g_fb squashes vertically into
// a bright horizontal line, that line pinches horizontally to a dot, the dot fades to black — over
// POFF_LEN frames. It works straight on g_fb (through a scratch copy, since the vertical squash reads
// source rows it is about to overwrite), so --ppm sees exactly what the window does. g_pal[2] is the
// near-white the shelf already uses for bright text — reused here as the hot scanline glow.
static uint8_t crt_buf[MAXFBW * MAXFBH];
static void crt_off(int f) {
    int cx = g_fbw / 2, cy = g_fbh / 2;
    if (f >= POFF_H) {                                    // the tail: a bright dot collapsing to nothing
        fb_clear(0);
        int prog = clamp01((f - POFF_H) * 1024 / (POFF_LEN - POFF_H));
        int rad = 3 - 3 * prog / 1024;
        for (int y = -rad; y <= rad; y++) for (int x = -rad; x <= rad; x++)
            if (x * x + y * y <= rad * rad) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && px < g_fbw && py >= 0 && py < g_fbh) g_fb[py * g_fbw + px] = 2;
            }
        return;
    }
    int32_t vs, hs;                                       // 16.16 scales: U = full size, shrinking to a sliver
    if (f < POFF_V) { int e = ease_io(clamp01(f * 1024 / POFF_V)); vs = lerp(U, U * 4 / 100, e); hs = U; }
    else { vs = U * 4 / 100; int e = ease_io(clamp01((f - POFF_V) * 1024 / (POFF_H - POFF_V))); hs = lerp(U, U * 2 / 100, e); }
    memcpy(crt_buf, g_fb, (size_t)g_fbw * g_fbh);
    fb_clear(0);
    int bandh = (int)(((int64_t)g_fbh * vs) >> 16) / 2;   // half-height / half-width of the surviving band
    int bandw = (int)(((int64_t)g_fbw * hs) >> 16) / 2;
    for (int y = cy - bandh; y <= cy + bandh; y++) {
        if (y < 0 || y >= g_fbh) continue;
        int32_t sy = g_fbh / 2 + (int32_t)(((int64_t)(y - cy) << 16) / vs);   // inverse-map this row to a source row
        if (sy < 0 || sy >= g_fbh) continue;
        int base = y * g_fbw, sbase = (int)sy * g_fbw;
        for (int x = cx - bandw; x <= cx + bandw; x++) {
            if (x < 0 || x >= g_fbw) continue;
            int32_t sx = g_fbw / 2 + (int32_t)(((int64_t)(x - cx) << 16) / hs);
            if (sx < 0 || sx >= g_fbw) continue;
            g_fb[base + x] = crt_buf[sbase + (int)sx];
        }
    }
    // the hot scanline through the middle: it spans the band's width, so as the horizontal stage
    // pinches the band in, the bright line becomes the bright dot.
    for (int y = cy - 1; y <= cy + 1; y++) {
        if (y < 0 || y >= g_fbh) continue;
        int base = y * g_fbw;
        for (int x = cx - bandw; x <= cx + bandw; x++)
            if (x >= 0 && x < g_fbw) g_fb[base + x] = 2;
    }
}

// The shelf proper: the rack of carts, the CVERTEX wordmark, the controls hint. Factored out so the
// CRT power-off can render the live shelf and then collapse it in place.
static void draw_shelf(void) {
    int s = g_fbh / 180; if (s < 1) s = 1;
    int cx = g_fbw / 2;
    draw_world(0, -1, 0);          // the same room, from the shelf's chair; nobody in flight
    // The title clears a safe top margin — 10*s worked out to a fixed 5.5% of height at any
    // resolution, which crowded the top edge (and, windowed, the title bar). A clean 7% band keeps
    // ink off the bleed. Same reasoning as game.h's hud_top(): text near an edge reads as spilling.
    text_draw(cx - text_width("CVERTEX", s * 2) / 2, g_fbh * 7 / 100, s * 2, "CVERTEX", 4);
    // The controls hint, two centred rows. As one line (~69 chars) it was 826px wide on a 640px
    // framebuffer — both ends ran off-screen (BROWSE clipped left, ESC POWER clipped right). Split so
    // each row fits with margin at any sane resolution, and drawn in the readable secondary grey.
    static const char *hintA = "ARROWS BROWSE    UP FLIP    DOWN INSERT";
    static const char *hintB = "SPACE OPTIONS    ESC POWER OFF";
    // Lifted clear of the bottom edge: the console's deck now breaks into frame down there, and grey
    // hint text laid over grey moulding is the one place this screen stops being instantly readable.
    text_draw(cx - text_width(hintA, s) / 2, g_fbh - 37 * s, s, hintA, 1);
    text_draw(cx - text_width(hintB, s) / 2, g_fbh - 25 * s, s, hintB, 1);   // ~9% off the bottom, matching the title's top margin
}

// The OPTIONS panel: a raised plate in the console's palette, a vertical list of settings with the
// cursor row barred and its label/value in bright ink, the rest dim. Palette + geometry + text only,
// exactly like the shelf — a poly_fill rect for the plate and frame, text_draw for every label.
static const char *opt_value(int row) {
    switch (row) {
        case OPT_GENTLE:     return g_gentle ? "ON" : "OFF";
        case OPT_COOP:       return g_coop == 1 ? "HOST" : g_coop == 2 ? "JOIN" : "SOLO";
        case OPT_FULLSCREEN: return g_fullscreen ? "ON" : "OFF";
        case OPT_CAMERA:     return g_cam_chase ? "CHASE" : "FIRST-PERSON";
        case OPT_CRT:        return g_crt_off ? "ON" : "OFF";
        default:             return "";
    }
}
static void draw_options(void) {
    static const char *label[OPT_N] = { "GENTLE MODE", "CO-OP", "FULLSCREEN", "CAMERA", "CRT POWER-OFF", "BACK" };
    fb_clear(0);
    int s = g_fbh / 180; if (s < 1) s = 1;
    int cx = g_fbw / 2;
    // the plate — a purple-framed screen centred on the display
    int pw = g_fbw * 62 / 100, ph = g_fbh * 66 / 100;
    int px0 = cx - pw / 2, py0 = g_fbh / 2 - ph / 2;
    int16_t frame[8] = { (int16_t)px0, (int16_t)py0, (int16_t)(px0 + pw), (int16_t)py0,
                         (int16_t)(px0 + pw), (int16_t)(py0 + ph), (int16_t)px0, (int16_t)(py0 + ph) };
    poly_fill(frame, 4, 6);                                  // frame (purple accent)
    int b = 2 * s;
    int16_t plate[8] = { (int16_t)(px0 + b), (int16_t)(py0 + b), (int16_t)(px0 + pw - b), (int16_t)(py0 + b),
                         (int16_t)(px0 + pw - b), (int16_t)(py0 + ph - b), (int16_t)(px0 + b), (int16_t)(py0 + ph - b) };
    poly_fill(plate, 4, 3);                                  // plate fill
    text_draw(cx - text_width("OPTIONS", s * 2) / 2, py0 + 5 * s, s * 2, "OPTIONS", 4);
    int rowh = ph * 11 / 100;
    int rx = px0 + 8 * s;
    int y0 = py0 + ph * 24 / 100;
    for (int i = 0; i < OPT_N; i++) {
        int y = y0 + i * rowh;
        int sel = (i == g_opt_sel);
        if (sel) {                                          // the highlight bar behind the cursor row
            int16_t bar[8] = { (int16_t)(px0 + b), (int16_t)(y - 2 * s), (int16_t)(px0 + pw - b), (int16_t)(y - 2 * s),
                               (int16_t)(px0 + pw - b), (int16_t)(y + 9 * s), (int16_t)(px0 + b), (int16_t)(y + 9 * s) };
            poly_fill(bar, 4, 5);
        }
        text_draw(rx, y, s, label[i], sel ? 2 : 1);
        const char *v = opt_value(i);
        if (v[0]) text_draw(px0 + pw - 8 * s - text_width(v, s), y, s, v, sel ? 2 : 1);
    }
    text_draw(cx - text_width("ARROWS MOVE   LEFT RIGHT CHANGE   SPACE BACK", s) / 2,
              py0 + ph + 4 * s, s, "ARROWS MOVE   LEFT RIGHT CHANGE   SPACE BACK", 1);
}

static void draw(void) {
    // Re-bake the selected cart's name/developer geometry only when the selection changes — shared by
    // the shelf and both console animations, so bake it once here for every path below.
    if (g_n && g_txt_sel != g_sel) { build_dev(g_list[g_sel]->author); g_txt_sel = g_sel; }

    if (g_phase == P_BOOT) {
        fb_clear(0);
        int s = g_fbh / 180; if (s < 1) s = 1;
        int cx = g_fbw / 2;
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

    if (g_phase == P_OPTIONS) { draw_options(); return; }                                   // the console's OPTIONS screen
    if (g_phase == P_INSERT) { draw_dive(INS_LEN - g_ins, 1); return; }                    // the forward dive
    if (g_phase == P_RETURN) { draw_dive((INS_LEN - SEAT) * g_ret / RET_LEN, 0); return; } // the reverse-insert

    // Turntable dev instrument (CVX_TURNTABLE=1): the selected cart alone, centred, spinning one
    // full turn every 128 frames with a slight downward tilt — so `--ppm` at frames 0/16/32/48…
    // gives front / 3-4 / side / back views. This is the "accurate eye": a mesh has to be judged
    // from more than the one angle that happens to hide its holes and its upside-down.
    if (getenv("CVX_TURNTABLE")) {
        fb_clear(0);
        int s = g_fbh / 180; if (s < 1) s = 1;
        static Inst inst[1];
        Cam cam = { { 0, 0, -U * 42 / 10 }, 0, 0, 0 };
        int ang = (int)((g_frame * 8) & 1023);
        inst[0].m = &cart_m[g_sel]; inst[0].pos = (V3){ 0, 0, 0 };
        inst[0].ax = 44; inst[0].ay = ang; inst[0].az = 0; inst[0].scale = U * 150 / 100;
        g3d_scene(inst, 1, &cam, 0, 0, 0);
        text_draw(4, 4, s, g_list[g_sel]->name, 2);
        return;
    }

    // The CRT power-off renders the live shelf, then collapses it in place; everything else is the shelf.
    if (g_phase == P_POWEROFF) { draw_shelf(); crt_off(g_poff); return; }
    draw_shelf();
}

static uint64_t checksum(void) { return (uint64_t)((g_sel << 4) | g_phase); }

const Game game_menu = { "menu", init, tick, audio, draw, checksum };
