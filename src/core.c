// core.c — the framebuffer and the rasterizer. Plain C, zero dependencies.
//
// Everything here is engine: pixels, a palette, a sine table, filling polygons. Nothing
// here knows what a character is, or what gravity is, or what the rules are. That side
// of the line lives in games/ — see game.h for why.
#include "core.h"

// Q15 sine, one full turn in 1024 steps. BAKED, not computed: every rotation, every projection and
// every angle the simulation ever takes comes out of this table, so "the same input gives the same run
// on any machine" was quietly resting on libm's sinf being bit-identical everywhere — an assumption
// nobody had ever tested, and one that costs nothing to remove. These are the exact values the old
// sinf loop produced (verified value-for-value, all 1024), now a compile-time constant: 2 KB of
// read-only data, no libm, no startup work, and a determinism argument with one fewer thing to trust.
const int16_t g_sin[1024] = {
         0,   201,   402,   603,   804,  1005,  1206,  1406,  1607,  1808,  2009,  2209,
      2410,  2610,  2811,  3011,  3211,  3411,  3611,  3811,  4011,  4210,  4409,  4608,
      4807,  5006,  5205,  5403,  5601,  5799,  5997,  6195,  6392,  6589,  6786,  6982,
      7179,  7375,  7571,  7766,  7961,  8156,  8351,  8545,  8739,  8932,  9126,  9319,
      9511,  9703,  9895, 10087, 10278, 10469, 10659, 10849, 11038, 11227, 11416, 11604,
     11792, 11980, 12166, 12353, 12539, 12724, 12909, 13094, 13278, 13462, 13645, 13827,
     14009, 14191, 14372, 14552, 14732, 14911, 15090, 15268, 15446, 15623, 15799, 15975,
     16150, 16325, 16499, 16672, 16845, 17017, 17189, 17360, 17530, 17699, 17868, 18036,
     18204, 18371, 18537, 18702, 18867, 19031, 19194, 19357, 19519, 19680, 19840, 20000,
     20159, 20317, 20474, 20631, 20787, 20942, 21096, 21249, 21402, 21554, 21705, 21855,
     22004, 22153, 22301, 22448, 22594, 22739, 22883, 23027, 23169, 23311, 23452, 23592,
     23731, 23869, 24006, 24143, 24278, 24413, 24546, 24679, 24811, 24942, 25072, 25201,
     25329, 25456, 25582, 25707, 25831, 25954, 26077, 26198, 26318, 26437, 26556, 26673,
     26789, 26905, 27019, 27132, 27244, 27355, 27466, 27575, 27683, 27790, 27896, 28001,
     28105, 28208, 28309, 28410, 28510, 28608, 28706, 28802, 28897, 28992, 29085, 29177,
     29268, 29358, 29446, 29534, 29621, 29706, 29790, 29873, 29955, 30036, 30116, 30195,
     30272, 30349, 30424, 30498, 30571, 30643, 30713, 30783, 30851, 30918, 30984, 31049,
     31113, 31175, 31236, 31297, 31356, 31413, 31470, 31525, 31580, 31633, 31684, 31735,
     31785, 31833, 31880, 31926, 31970, 32014, 32056, 32097, 32137, 32176, 32213, 32249,
     32284, 32318, 32350, 32382, 32412, 32441, 32468, 32495, 32520, 32544, 32567, 32588,
     32609, 32628, 32646, 32662, 32678, 32692, 32705, 32717, 32727, 32736, 32744, 32751,
     32757, 32761, 32764, 32766, 32767, 32766, 32764, 32761, 32757, 32751, 32744, 32736,
     32727, 32717, 32705, 32692, 32678, 32662, 32646, 32628, 32609, 32588, 32567, 32544,
     32520, 32495, 32468, 32441, 32412, 32382, 32350, 32318, 32284, 32249, 32213, 32176,
     32137, 32097, 32056, 32014, 31970, 31926, 31880, 31833, 31785, 31735, 31684, 31633,
     31580, 31525, 31470, 31413, 31356, 31297, 31236, 31175, 31113, 31049, 30984, 30918,
     30851, 30783, 30713, 30643, 30571, 30498, 30424, 30349, 30272, 30195, 30116, 30036,
     29955, 29873, 29790, 29706, 29621, 29534, 29446, 29358, 29268, 29177, 29085, 28992,
     28897, 28802, 28706, 28608, 28510, 28410, 28309, 28208, 28105, 28001, 27896, 27790,
     27683, 27575, 27466, 27355, 27244, 27132, 27019, 26905, 26789, 26673, 26556, 26437,
     26318, 26198, 26077, 25954, 25831, 25707, 25582, 25456, 25329, 25201, 25072, 24942,
     24811, 24679, 24546, 24413, 24278, 24143, 24006, 23869, 23731, 23592, 23452, 23311,
     23169, 23027, 22883, 22739, 22594, 22448, 22301, 22153, 22004, 21855, 21705, 21554,
     21402, 21249, 21096, 20942, 20787, 20631, 20474, 20317, 20159, 20000, 19840, 19680,
     19519, 19357, 19194, 19031, 18867, 18702, 18537, 18371, 18204, 18036, 17868, 17699,
     17530, 17360, 17189, 17017, 16845, 16672, 16499, 16325, 16150, 15975, 15799, 15623,
     15446, 15268, 15090, 14911, 14732, 14552, 14372, 14191, 14009, 13827, 13645, 13462,
     13278, 13094, 12909, 12724, 12539, 12353, 12166, 11980, 11792, 11604, 11416, 11227,
     11038, 10849, 10659, 10469, 10278, 10087,  9895,  9703,  9511,  9319,  9126,  8932,
      8739,  8545,  8351,  8156,  7961,  7766,  7571,  7375,  7179,  6982,  6786,  6589,
      6392,  6195,  5997,  5799,  5601,  5403,  5205,  5006,  4807,  4608,  4409,  4210,
      4011,  3811,  3611,  3411,  3211,  3011,  2811,  2610,  2410,  2209,  2009,  1808,
      1607,  1406,  1206,  1005,   804,   603,   402,   201,     0,  -201,  -402,  -603,
      -804, -1005, -1206, -1406, -1607, -1808, -2009, -2209, -2410, -2610, -2811, -3011,
     -3211, -3411, -3611, -3811, -4011, -4210, -4409, -4608, -4807, -5006, -5205, -5403,
     -5601, -5799, -5997, -6195, -6392, -6589, -6786, -6982, -7179, -7375, -7571, -7766,
     -7961, -8156, -8351, -8545, -8739, -8932, -9126, -9319, -9511, -9703, -9895,-10087,
    -10278,-10469,-10659,-10849,-11038,-11227,-11416,-11604,-11792,-11980,-12166,-12353,
    -12539,-12724,-12909,-13094,-13278,-13462,-13645,-13827,-14009,-14191,-14372,-14552,
    -14732,-14911,-15090,-15268,-15446,-15623,-15799,-15975,-16150,-16325,-16499,-16672,
    -16845,-17017,-17189,-17360,-17530,-17699,-17868,-18036,-18204,-18371,-18537,-18702,
    -18867,-19031,-19194,-19357,-19519,-19680,-19840,-20000,-20159,-20317,-20474,-20631,
    -20787,-20942,-21096,-21249,-21402,-21554,-21705,-21855,-22004,-22153,-22301,-22448,
    -22594,-22739,-22883,-23027,-23169,-23311,-23452,-23592,-23731,-23869,-24006,-24143,
    -24278,-24413,-24546,-24679,-24811,-24942,-25072,-25201,-25329,-25456,-25582,-25707,
    -25831,-25954,-26077,-26198,-26318,-26437,-26556,-26673,-26789,-26905,-27019,-27132,
    -27244,-27355,-27466,-27575,-27683,-27790,-27896,-28001,-28105,-28208,-28309,-28410,
    -28510,-28608,-28706,-28802,-28897,-28992,-29085,-29177,-29268,-29358,-29446,-29534,
    -29621,-29706,-29790,-29873,-29955,-30036,-30116,-30195,-30272,-30349,-30424,-30498,
    -30571,-30643,-30713,-30783,-30851,-30918,-30984,-31049,-31113,-31175,-31236,-31297,
    -31356,-31413,-31470,-31525,-31580,-31633,-31684,-31735,-31785,-31833,-31880,-31926,
    -31970,-32014,-32056,-32097,-32137,-32176,-32213,-32249,-32284,-32318,-32350,-32382,
    -32412,-32441,-32468,-32495,-32520,-32544,-32567,-32588,-32609,-32628,-32646,-32662,
    -32678,-32692,-32705,-32717,-32727,-32736,-32744,-32751,-32757,-32761,-32764,-32766,
    -32767,-32766,-32764,-32761,-32757,-32751,-32744,-32736,-32727,-32717,-32705,-32692,
    -32678,-32662,-32646,-32628,-32609,-32588,-32567,-32544,-32520,-32495,-32468,-32441,
    -32412,-32382,-32350,-32318,-32284,-32249,-32213,-32176,-32137,-32097,-32056,-32014,
    -31970,-31926,-31880,-31833,-31785,-31735,-31684,-31633,-31580,-31525,-31470,-31413,
    -31356,-31297,-31236,-31175,-31113,-31049,-30984,-30918,-30851,-30783,-30713,-30643,
    -30571,-30498,-30424,-30349,-30272,-30195,-30116,-30036,-29955,-29873,-29790,-29706,
    -29621,-29534,-29446,-29358,-29268,-29177,-29085,-28992,-28897,-28802,-28706,-28608,
    -28510,-28410,-28309,-28208,-28105,-28001,-27896,-27790,-27683,-27575,-27466,-27355,
    -27244,-27132,-27019,-26905,-26789,-26673,-26556,-26437,-26318,-26198,-26077,-25954,
    -25831,-25707,-25582,-25456,-25329,-25201,-25072,-24942,-24811,-24679,-24546,-24413,
    -24278,-24143,-24006,-23869,-23731,-23592,-23452,-23311,-23169,-23027,-22883,-22739,
    -22594,-22448,-22301,-22153,-22004,-21855,-21705,-21554,-21402,-21249,-21096,-20942,
    -20787,-20631,-20474,-20317,-20159,-20000,-19840,-19680,-19519,-19357,-19194,-19031,
    -18867,-18702,-18537,-18371,-18204,-18036,-17868,-17699,-17530,-17360,-17189,-17017,
    -16845,-16672,-16499,-16325,-16150,-15975,-15799,-15623,-15446,-15268,-15090,-14911,
    -14732,-14552,-14372,-14191,-14009,-13827,-13645,-13462,-13278,-13094,-12909,-12724,
    -12539,-12353,-12166,-11980,-11792,-11604,-11416,-11227,-11038,-10849,-10659,-10469,
    -10278,-10087, -9895, -9703, -9511, -9319, -9126, -8932, -8739, -8545, -8351, -8156,
     -7961, -7766, -7571, -7375, -7179, -6982, -6786, -6589, -6392, -6195, -5997, -5799,
     -5601, -5403, -5205, -5006, -4807, -4608, -4409, -4210, -4011, -3811, -3611, -3411,
     -3211, -3011, -2811, -2610, -2410, -2209, -2009, -1808, -1607, -1406, -1206, -1005,
      -804,  -603,  -402,  -201
};

// Kept as the engine's "tables are ready" call — the table above is now built by the compiler, so
// there is nothing left to do here. Callers (every game, and the synth) stay unchanged.
void tables_init(void) { }

uint8_t  g_fb[MAXFBW * MAXFBH];
int g_fbw, g_fbh;
int32_t g_dev_camz;
int      g_mx, g_my;   // pointer, in framebuffer pixels (top-left origin)
uint8_t  g_mbtn;       // bit0 = left button, bit1 = right button, bit2 = middle button
int      g_wheel;     // wheel notches accumulated since a tool read it; + = up/zoom-in. UI only, never hashed
int      g_digit = -1; // number-row pulse: 0..9 the frame its key goes down, else -1. UI only, never hashed
uint8_t  g_view_toggle;// one-frame edge: platform pulses (Tab / --view), a game consumes it
uint8_t  g_help_toggle;// one-frame edge: platform pulses ('/' keycode 44), a game toggles its manual
uint8_t  g_debug_toggle;// one-frame edge: platform pulses (F3), a game toggles its dev debug overlay
uint8_t  g_esc;        // one-frame edge: platform pulses (Esc / CV_ESC_AT), the shell routes it
int      g_menu_return;   // platform -> menu: returned from a game, play the insert in reverse (all platforms link)
int      g_quit;          // menu -> platform: the CRT power-off finished, stop the loop
// The OPTIONS panel's settings. 🔴 Zero-init IS the default for every one — off / solo /
// windowed / first-person — so the synth.c non-zero-initializer lesson holds (these live in
// __bss, nothing on disk) AND "untouched == today's behaviour" is true by construction. The
// menu raises g_crt_off to 1 at init() (the collapse plays by default); mac.c sets g_fullscreen
// from the --fullscreen flag. Both are runtime assignments, never data-segment initializers.
int      g_gentle, g_coop, g_fullscreen, g_crt_off, g_cam_chase;

void fb_resize(int w, int h) {
    if (w > MAXFBW) w = MAXFBW;
    if (h > MAXFBH) h = MAXFBH;
    g_fbw = w; g_fbh = h;
}
uint32_t g_pal[256];
void (*g_present_fx)(uint32_t *rgba, int w, int h);   // the light-chip socket; NULL = base console. See core.h.

void fb_clear(uint8_t ci) {
    for (int i = 0; i < g_fbw * g_fbh; i++) g_fb[i] = ci;
}

// Scanline fill: for each scanline, find every edge intersection, sort, fill pairs.
// Even-odd rule. Integer math, no floats → deterministic.
void poly_fill_n(const int16_t *pts, const uint16_t *lens, int nc, uint8_t ci) {
    int total = 0;
    for (int c = 0; c < nc; c++) total += lens[c];
    if (total < 3) return;

    int miny = pts[1], maxy = pts[1];
    for (int i = 1; i < total; i++) {
        int y = pts[i * 2 + 1];
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (miny < 0) miny = 0;
    if (maxy > g_fbh - 1) maxy = g_fbh - 1;

    int xs[MAXPTS];
    for (int y = miny; y <= maxy; y++) {
        int cnt = 0;
        int base = 0;
        for (int c = 0; c < nc; c++) {              // each contour closes on its own; no edges cross between contours
            int n = lens[c];
            for (int i = 0; i < n && cnt < MAXPTS; i++) {
                int p = base + i, q = base + (i + 1) % n;
                int y0 = pts[p * 2 + 1], y1 = pts[q * 2 + 1];
                if (y0 == y1) continue;
                // half-open [min,max) → a shared vertex isn't counted twice
                int ymin = y0 < y1 ? y0 : y1;
                int ymax = y0 < y1 ? y1 : y0;
                if (y < ymin || y >= ymax) continue;
                int x0 = pts[p * 2], x1 = pts[q * 2];
                xs[cnt++] = x0 + (int)((int32_t)(y - y0) * (x1 - x0) / (y1 - y0));
            }
            base += n;
        }
        // insertion sort (cnt is small)
        for (int a = 1; a < cnt; a++) {
            int v = xs[a], b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < cnt; a += 2) {
            int L = xs[a], R = xs[a + 1];
            if (R < 0 || L > g_fbw - 1) continue;
            if (L < 0) L = 0;
            if (R > g_fbw - 1) R = g_fbw - 1;
            uint8_t *row = &g_fb[y * g_fbw];
            for (int x = L; x <= R; x++) row[x] = ci;
        }
    }
}

void poly_fill(const int16_t *pts, int n, uint8_t ci) {
    uint16_t one = (uint16_t)n;
    poly_fill_n(pts, &one, 1, ci);
}


// ---- depth-buffered triangles ------------------------------------------------

uint32_t g_zb[MAXFBW * MAXFBH];

void zb_clear(void) {
    for (int i = 0; i < g_fbw * g_fbh; i++) g_zb[i] = 0;   // 0 = infinitely far
}

// Scanline fill with a per-pixel depth test. Only 1/z is linear in screen space, so
// that's what's interpolated — the alternative is a depth test that's subtly wrong in
// exactly the places perspective matters.
void tri_fill_z(const int16_t *xy, const uint32_t *w, uint8_t ci) {
    int x0 = xy[0], y0 = xy[1], x1 = xy[2], y1 = xy[3], x2 = xy[4], y2 = xy[5];
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (miny < 0) miny = 0;
    if (maxy > g_fbh - 1) maxy = g_fbh - 1;
    if (miny > maxy) return;

    // Edge function denominator: twice the signed area. Zero = degenerate, nothing to do.
    int32_t area = (int32_t)(x1 - x0) * (y2 - y0) - (int32_t)(x2 - x0) * (y1 - y0);
    if (area == 0) return;

    for (int y = miny; y <= maxy; y++) {
        int xs[3], ws_i = 0;
        int32_t xl = 0, xr = 0;
        // Find this scanline's span from the three edges.
        int cnt = 0; int px[3];
        int vx[3] = { x0, x1, x2 }, vy[3] = { y0, y1, y2 };
        for (int e = 0; e < 3 && cnt < 2; e++) {
            int a = e, b = (e + 1) % 3;
            int ya = vy[a], yb = vy[b];
            if (ya == yb) continue;
            int lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
            if (y < lo || y >= hi) continue;
            px[cnt++] = vx[a] + (int)((int32_t)(y - ya) * (vx[b] - vx[a]) / (yb - ya));
        }
        if (cnt < 2) continue;
        xl = px[0] < px[1] ? px[0] : px[1];
        xr = px[0] < px[1] ? px[1] : px[0];
        if (xr < 0 || xl > g_fbw - 1) continue;
        if (xl < 0) xl = 0;
        if (xr > g_fbw - 1) xr = g_fbw - 1;

        uint8_t *row = &g_fb[y * g_fbw];
        uint32_t *zrow = &g_zb[y * g_fbw];
        for (int x = xl; x <= xr; x++) {
            // Barycentric from edge functions — exact, and it gives the 1/z weights.
            int32_t b0 = (int32_t)(x1 - x) * (y2 - y) - (int32_t)(x2 - x) * (y1 - y);
            int32_t b1 = (int32_t)(x2 - x) * (y0 - y) - (int32_t)(x0 - x) * (y2 - y);
            int32_t b2 = area - b0 - b1;
            int64_t ww = ((int64_t)b0 * w[0] + (int64_t)b1 * w[1] + (int64_t)b2 * w[2]) / area;
            if (ww <= 0) continue;
            uint32_t d = (uint32_t)ww;
            if (d > zrow[x]) { zrow[x] = d; row[x] = ci; }
        }
        (void)xs; (void)ws_i;
    }
}
