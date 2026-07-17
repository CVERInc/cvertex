// svg2poly — SVG → a const Shape baked into the binary. Runs at build time, never ships in the game.
//
// 🔴 The engine never learns what SVG is. SVG is an authoring format, not a runtime
// one — the full spec is a monster (bezier curves / stroke / transform / clip-path /
// CSS), and implementing it would eat the whole floppy. So it's chewed up here
// instead: beziers get flattened here, colours get clustered here, and the engine
// only ever receives "a string of points + a palette index."
//
// Several SVGs in, ONE header out, and they SHARE:
//   - one palette. A tracer clusters each drawing on its own, so the same white comes
//     out #fcfcfb here and #fdfdfc there — measured ΔE 0.35, when the eye can't see a
//     difference below about 2.3. Left alone that's 43 colours for what is really 8,
//     and it breaks the whole point of an indexed framebuffer: edit one table and the
//     game changes season. Colours within ΔE_MERGE become one entry.
//   - one scale. Normalizing each view to its own bounding box would make the
//     character grow and shrink as it turns.
// ΔE is CIE76 on purpose: CIEDE2000 was measured WORSE under noise in a sibling
// project (it de-weights lightness, which is exactly what separates these). Don't retry it.
//
//   cc -O2 -o svg2poly tools/svg2poly.c -lm
//   ./svg2poly hero 16 0:v0.svg 45:v1.svg 90:v2.svg 135:v3.svg 225:v4.svg ... > out.h
//              ^name ^base   ^degrees:file, ascending, 0..359
//   Pass --mirror to cover only 0..180 and let the engine reflect the far half. That is
//   ONLY correct for a symmetric design; anything asymmetric must draw the full turn.
//
// 🔴 --key RRGGBB drops a background colour, and the source art MUST use one.
// Tracing art on a transparent background silently composites it onto WHITE, so white
// artwork merges with the background, becomes one region, and the tracer discards it as
// background. A white-haired character then arrives with a hole where her hair was —
// and on a white page it still LOOKS right, because you're seeing the page through her.
// Key on a colour the art can't contain (magenta) and drop it here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAXPT   200000
#define MAXCONT 4096
#define MAXFILL 512
#define MAXPAL  64

static double px[MAXPT], py[MAXPT];
static int    np;
static int    clen[MAXCONT], nc;                 // point count per contour
static struct { int c0, ncont, pal; } fl[MAXFILL];
static struct { int f0, nf, c0, nc, p0, np; } vw[16]; static int nvw;
static int    nf;
static unsigned pal[MAXPAL]; static int npal;
static double pal_lab[MAXPAL][3];
static int    pal_n[MAXPAL];          // how many source colours merged into this entry
#define DE_MERGE 3.0                  // below the eye's threshold (~2.3), safely the same colour

static void to_lab(unsigned rgb, double *L) {
    double c[3] = { ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0 };
    for (int i = 0; i < 3; i++) c[i] = c[i] <= 0.04045 ? c[i] / 12.92 : pow((c[i] + 0.055) / 1.055, 2.4);
    double X = (c[0]*0.4124 + c[1]*0.3576 + c[2]*0.1805) / 0.95047;
    double Y =  c[0]*0.2126 + c[1]*0.7152 + c[2]*0.0722;
    double Z = (c[0]*0.0193 + c[1]*0.1192 + c[2]*0.9505) / 1.08883;
    double f[3], t[3] = { X, Y, Z };
    for (int i = 0; i < 3; i++) f[i] = t[i] > 0.008856 ? cbrt(t[i]) : 7.787 * t[i] + 16.0 / 116.0;
    L[0] = 116 * f[1] - 16; L[1] = 500 * (f[0] - f[1]); L[2] = 200 * (f[1] - f[2]);
}

// Flattening tolerance: squared control-point-to-chord distance / squared chord length. Smaller = finer.
static double TOL = 0.02;

static void emit(double x, double y) { if (np < MAXPT) { px[np] = x; py[np] = y; np++; } }

static void bez(double x0,double y0,double x1,double y1,double x2,double y2,double x3,double y3,int d) {
    double dx = x3-x0, dy = y3-y0;
    double d1 = fabs((x1-x3)*dy - (y1-y3)*dx), d2 = fabs((x2-x3)*dy - (y2-y3)*dx);
    if (d > 16 || (d1+d2)*(d1+d2) <= TOL*(dx*dx+dy*dy)) { emit(x3, y3); return; }
    double x01=(x0+x1)/2, y01=(y0+y1)/2, x12=(x1+x2)/2, y12=(y1+y2)/2, x23=(x2+x3)/2, y23=(y2+y3)/2;
    double xa=(x01+x12)/2, ya=(y01+y12)/2, xb=(x12+x23)/2, yb=(y12+y23)/2;
    double xm=(xa+xb)/2, ym=(ya+yb)/2;
    bez(x0,y0,x01,y01,xa,ya,xm,ym,d+1);
    bez(xm,ym,xb,yb,x23,y23,x3,y3,d+1);
}

// Nearest existing entry within DE_MERGE, else a new one. The entry keeps a running
// mean in Lab so a cluster settles on its centre rather than on whichever view came first.
static int keydrops;
// The tracer's clustering nudges the key colour a little, so match by distance, not equality.
static int dropped_key(unsigned rgb, unsigned key) {
    int dr = (int)((rgb>>16)&255) - (int)((key>>16)&255);
    int dg = (int)((rgb>>8)&255)  - (int)((key>>8)&255);
    int db = (int)(rgb&255)       - (int)(key&255);
    return dr*dr + dg*dg + db*db < 60*60;
}

static int palidx(unsigned rgb) {
    double L[3]; to_lab(rgb, L);
    int best = -1; double bd = 1e30;
    for (int i = 0; i < npal; i++) {
        double d = 0;
        for (int k = 0; k < 3; k++) { double e = L[k] - pal_lab[i][k]; d += e * e; }
        d = sqrt(d);
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && bd < DE_MERGE) {
        for (int k = 0; k < 3; k++)
            pal_lab[best][k] = (pal_lab[best][k] * pal_n[best] + L[k]) / (pal_n[best] + 1);
        pal_n[best]++;
        return best;
    }
    if (npal < MAXPAL) {
        pal[npal] = rgb;
        for (int k = 0; k < 3; k++) pal_lab[npal][k] = L[k];
        pal_n[npal] = 1;
        return npal++;
    }
    return 0;
}

// Pull the next number out of a d="" string (skip commas/whitespace, stop at a command letter)
static const char *num(const char *p, double *out) {
    while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p || (!(*p >= '0' && *p <= '9') && *p != '-' && *p != '+' && *p != '.')) return NULL;
    char *e; *out = strtod(p, &e); return e;
}

static void parse_d(const char *d) {
    double cx = 0, cy = 0, sx = 0, sy = 0;   // current / subpath start
    char cmd = 0;
    int cstart = np;
    const char *p = d;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (!*p) break;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { cmd = *p++; }
        double a[6];
        switch (cmd) {
        case 'M': case 'm': {
            const char *q = num(p, &a[0]); if (!q) { p++; break; } p = num(q, &a[1]);
            if (np > cstart && nc < MAXCONT) { clen[nc++] = np - cstart; }  // close out the previous contour
            cstart = np;
            if (cmd == 'm') { cx += a[0]; cy += a[1]; } else { cx = a[0]; cy = a[1]; }
            sx = cx; sy = cy; emit(cx, cy);
            cmd = (cmd == 'M') ? 'L' : 'l';                                 // subsequent coordinates are an implicit lineto
            break; }
        case 'L': case 'l': {
            const char *q = num(p, &a[0]); if (!q) { cmd = 0; break; } p = num(q, &a[1]);
            if (cmd == 'l') { cx += a[0]; cy += a[1]; } else { cx = a[0]; cy = a[1]; }
            emit(cx, cy); break; }
        case 'H': case 'h': {
            const char *q = num(p, &a[0]); if (!q) { cmd = 0; break; } p = q;
            cx = (cmd == 'h') ? cx + a[0] : a[0]; emit(cx, cy); break; }
        case 'V': case 'v': {
            const char *q = num(p, &a[0]); if (!q) { cmd = 0; break; } p = q;
            cy = (cmd == 'v') ? cy + a[0] : a[0]; emit(cx, cy); break; }
        case 'C': case 'c': {
            const char *q = p; int ok = 1;
            for (int i = 0; i < 6; i++) { q = num(q, &a[i]); if (!q) { ok = 0; break; } }
            if (!ok) { cmd = 0; break; }
            p = q;
            if (cmd == 'c') for (int i = 0; i < 6; i += 2) { a[i] += cx; a[i+1] += cy; }
            bez(cx, cy, a[0], a[1], a[2], a[3], a[4], a[5], 0);
            cx = a[4]; cy = a[5]; break; }
        case 'Z': case 'z':
            cx = sx; cy = sy;
            if (np > cstart && nc < MAXCONT) { clen[nc++] = np - cstart; cstart = np; }
            cmd = 0; break;
        default: p++; break;
        }
    }
    if (np > cstart && nc < MAXCONT) clen[nc++] = np - cstart;
}

// Grab an attribute out of a <path> tag
static int attr(const char *tag, const char *name, char *out, int cap) {
    char pat[32]; snprintf(pat, sizeof pat, "%s=\"", name);
    const char *s = strstr(tag, pat); if (!s) return 0;
    s += strlen(pat);
    const char *e = strchr(s, '"'); if (!e) return 0;
    int n = (int)(e - s); if (n >= cap) n = cap - 1;
    memcpy(out, s, n); out[n] = 0; return 1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: svg2poly <name> <palette base> <view.svg>...   (views ordered 0..180 degrees)\n");
        return 1;
    }
    const char *name = argv[1];
    int base = atoi(argv[2]);
    int mirror = 0, argstart = 3;
    long key = -1;
    for (int a = 3; a < argc; a++) {
        if (!strcmp(argv[a], "--mirror")) { mirror = 1; if (a + 1 > argstart) argstart = a + 1; }
        else if (!strcmp(argv[a], "--key") && a + 1 < argc) { key = strtol(argv[a+1], NULL, 16); if (a + 2 > argstart) argstart = a + 2; }
    }

    static char buf[1 << 22], d[1 << 20], fill[64], tag[1 << 20];
    static int deg[16];
    for (int a = argstart; a < argc && nvw < 16; a++) {
        const char *spec = argv[a], *colon = strchr(spec, ':');
        if (!colon) { fprintf(stderr, "svg2poly: expected <degrees>:<file>, got %s\n", spec); return 1; }
        deg[nvw] = atoi(spec);
        int limit = mirror ? 180 : 359;
        if (deg[nvw] < 0 || deg[nvw] > limit) { fprintf(stderr, "svg2poly: %d out of 0..%d\n", deg[nvw], limit); return 1; }
        if (nvw && deg[nvw] <= deg[nvw-1]) { fprintf(stderr, "svg2poly: angles must ascend\n"); return 1; }
        FILE *f = fopen(colon + 1, "rb");
        if (!f) { fprintf(stderr, "svg2poly: can't open %s\n", colon + 1); return 1; }
        long n = (long)fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);

        vw[nvw].f0 = nf; vw[nvw].c0 = nc; vw[nvw].p0 = np;
        const char *p = buf;
        while ((p = strstr(p, "<path")) != NULL) {
            const char *end = strchr(p, '>'); if (!end) break;
            long len = end - p; if (len > (long)sizeof tag - 1) len = sizeof tag - 1;
            memcpy(tag, p, len); tag[len] = 0;
            p = end;
            if (!attr(tag, "d", d, sizeof d)) continue;
            unsigned rgb = 0;
            fill[0] = 0;
            if (attr(tag, "fill", fill, sizeof fill) && fill[0] == '#') {
                if (strlen(fill) == 7) rgb = (unsigned)strtoul(fill + 1, NULL, 16);
                else if (strlen(fill) == 4) {
                    unsigned v = (unsigned)strtoul(fill + 1, NULL, 16);
                    rgb = ((v & 0xF00) * 0x1100) | ((v & 0x0F0) * 0x110) | ((v & 0x00F) * 0x11);
                }
            }
            if (!strcmp(fill, "none")) continue;
            if (key >= 0 && dropped_key(rgb, (unsigned)key)) { keydrops++; continue; }
            int c0 = nc;
            parse_d(d);
            if (nc == c0) continue;
            if (nf < MAXFILL) { fl[nf].c0 = c0; fl[nf].ncont = nc - c0; fl[nf].pal = palidx(rgb); nf++; }
        }
        vw[nvw].nf = nf - vw[nvw].f0; vw[nvw].nc = nc - vw[nvw].c0; vw[nvw].np = np - vw[nvw].p0;
        if (!vw[nvw].nf) { fprintf(stderr, "svg2poly: no paths in %s\n", colon + 1); return 1; }
        nvw++;
    }

    // ONE scale and ONE centre across every view. Per-view normalization would make the
    // character grow, shrink and drift as it turns.
    double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
    for (int i = 0; i < np; i++) {
        if (px[i] < lo[0]) lo[0] = px[i]; if (px[i] > hi[0]) hi[0] = px[i];
        if (py[i] < lo[1]) lo[1] = py[i]; if (py[i] > hi[1]) hi[1] = py[i];
    }
    double ext = (hi[0]-lo[0] > hi[1]-lo[1]) ? hi[0]-lo[0] : hi[1]-lo[1];
    if (ext <= 0) ext = 1;
    double sc = 32768.0 / ext, mx = (lo[0]+hi[0])/2, my = (lo[1]+hi[1])/2;

    printf("// generated by svg2poly. Don't hand-edit — change the art and rebuild.\n");
    printf("// %d views / %d points / %d contours / %d fills / %d shared colours\n", nvw, np, nc, nf, npal);
    printf("static const uint32_t %s_pal[%d] = {\n", name, npal);
    for (int i = 0; i < npal; i++) printf("  0xFF%06X,\n", pal[i]);
    printf("};\n");

    for (int v = 0; v < nvw; v++) {
        printf("static const int16_t %s_%d_pts[%d] = {\n", name, v, vw[v].np * 2);
        for (int i = vw[v].p0; i < vw[v].p0 + vw[v].np; i++)
            printf("%d,%d,%s", (int)lrint((px[i]-mx)*sc), (int)lrint((py[i]-my)*sc), (i % 8 == 7) ? "\n" : "");
        printf("\n};\nstatic const uint16_t %s_%d_lens[%d] = {\n", name, v, vw[v].nc);
        for (int i = vw[v].c0; i < vw[v].c0 + vw[v].nc; i++) printf("%d,%s", clen[i], (i % 16 == 15) ? "\n" : "");
        printf("\n};\nstatic const Fill %s_%d_f[%d] = {\n", name, v, vw[v].nf);
        int ptoff = 0;
        for (int i = vw[v].f0; i < vw[v].f0 + vw[v].nf; i++) {
            printf("  {%d,%d,%d,%d},\n", ptoff, fl[i].c0 - vw[v].c0, fl[i].ncont, base + fl[i].pal);
            for (int c = 0; c < fl[i].ncont; c++) ptoff += clen[fl[i].c0 + c];
        }
        printf("};\nstatic const Shape %s_%d = { %s_%d_pts, %s_%d_lens, %s_%d_f, %d, %s_pal, %d, %d };\n",
               name, v, name, v, name, v, name, v, vw[v].nf, name, npal, base);
    }
    printf("static const Shape *const %s_views[%d] = {", name, nvw);
    for (int v = 0; v < nvw; v++) printf(" &%s_%d,", name, v);
    printf(" };\nstatic const uint16_t %s_angles[%d] = {", name, nvw);
    for (int v = 0; v < nvw; v++) printf(" %d,", deg[v] * 1024 / 360);   // degrees -> g3d units
    printf(" };\nconst Turn g_%s = { %s_views, %s_angles, %d, %d };\n", name, name, name, nvw, mirror);

    fprintf(stderr, "svg2poly: %d views%s -> %d points / %d fills / %d shared colours -> %d bytes\n",
            nvw, mirror ? " (+mirrored half)" : " (full turn)", np, nf, npal, np * 4 + nc * 2 + nf * 6 + npal * 4);
    if (key >= 0) fprintf(stderr, "svg2poly: dropped %d key-coloured fills\n", keydrops);
    else fprintf(stderr, "svg2poly: NOTE no --key given. If the art was traced on a transparent or\n"
                         "          white background, white artwork may be missing and you won't see it\n"
                         "          on a light page.\n");
    return 0;
}
