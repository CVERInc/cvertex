// svg2poly — SVG → a const Shape baked into the binary. Runs at build time, never ships in the game.
//
// 🔴 The engine never learns what SVG is. SVG is an authoring format, not a runtime
// one — the full spec is a monster (bezier curves / stroke / transform / clip-path /
// CSS), and implementing it would eat the whole floppy. So it's chewed up here
// instead: beziers get flattened here, colours get clustered here, and the engine
// only ever receives "a string of points + a palette index."
//
//   cc -O2 -o svg2poly tools/svg2poly.c -lm
//   ./svg2poly hero.svg hero 16 > src/shape_hero.h
//                       ^name ^palette base index
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
static int    nf;
static unsigned pal[MAXPAL]; static int npal;

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

static int palidx(unsigned rgb) {
    for (int i = 0; i < npal; i++) if (pal[i] == rgb) return i;
    if (npal < MAXPAL) { pal[npal] = rgb; return npal++; }
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
    if (argc < 3) { fprintf(stderr, "usage: svg2poly <in.svg> <name> [palette base=16] [tolerance=0.02]\n"); return 1; }
    const char *name = argv[2];
    int base = argc > 3 ? atoi(argv[3]) : 16;
    if (argc > 4) TOL = atof(argv[4]);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "svg2poly: can't open %s\n", argv[1]); return 1; }
    static char buf[1 << 22];
    long n = (long)fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);

    // Walk each <path> in document order = back-to-front draw order (this is how a tracer emits them)
    const char *p = buf;
    static char d[1 << 20], fill[64];
    while ((p = strstr(p, "<path")) != NULL) {
        const char *end = strchr(p, '>'); if (!end) break;
        long len = end - p; if (len > (long)sizeof d - 1) len = sizeof d - 1;
        static char tag[1 << 20];
        memcpy(tag, p, len); tag[len] = 0;
        p = end;

        if (!attr(tag, "d", d, sizeof d)) continue;
        unsigned rgb = 0;
        if (attr(tag, "fill", fill, sizeof fill) && fill[0] == '#') {
            if (strlen(fill) == 7) rgb = (unsigned)strtoul(fill + 1, NULL, 16);
            else if (strlen(fill) == 4) {                     // #abc → #aabbcc
                unsigned v = (unsigned)strtoul(fill + 1, NULL, 16);
                rgb = ((v & 0xF00) * 0x1100) | ((v & 0x0F0) * 0x110) | ((v & 0x00F) * 0x11);
            }
        }
        if (!strcmp(fill, "none")) continue;

        int c0 = nc, p0 = np;
        parse_d(d);
        if (nc == c0) continue;                               // produced no contours
        (void)p0;
        if (nf < MAXFILL) { fl[nf].c0 = c0; fl[nf].ncont = nc - c0; fl[nf].pal = palidx(rgb); nf++; }
    }
    if (!np) { fprintf(stderr, "svg2poly: no paths in %s\n", argv[1]); return 1; }

    // Center + normalize: longest edge → 32768 (±16384)
    double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
    for (int i = 0; i < np; i++) {
        if (px[i] < lo[0]) lo[0] = px[i]; if (px[i] > hi[0]) hi[0] = px[i];
        if (py[i] < lo[1]) lo[1] = py[i]; if (py[i] > hi[1]) hi[1] = py[i];
    }
    double ext = (hi[0]-lo[0] > hi[1]-lo[1]) ? hi[0]-lo[0] : hi[1]-lo[1];
    if (ext <= 0) ext = 1;
    double s = 32768.0 / ext, mx = (lo[0]+hi[0])/2, my = (lo[1]+hi[1])/2;

    printf("// generated by svg2poly from %s. Don't hand-edit — change the artwork and rebuild.\n", argv[1]);
    printf("// %d points / %d contours / %d fills / %d colours\n", np, nc, nf, npal);
    printf("static const int16_t %s_pts[%d] = {\n", name, np * 2);
    for (int i = 0; i < np; i++)
        printf("%d,%d,%s", (int)lrint((px[i]-mx)*s), (int)lrint((py[i]-my)*s), (i % 8 == 7) ? "\n" : "");
    printf("\n};\nstatic const uint16_t %s_lens[%d] = {\n", name, nc);
    for (int i = 0; i < nc; i++) printf("%d,%s", clen[i], (i % 16 == 15) ? "\n" : "");
    printf("\n};\nstatic const Fill %s_f[%d] = {\n", name, nf);
    int ptoff = 0;
    for (int i = 0; i < nf; i++) {
        printf("  {%d,%d,%d,%d},\n", ptoff, fl[i].c0, fl[i].ncont, base + fl[i].pal);
        for (int c = 0; c < fl[i].ncont; c++) ptoff += clen[fl[i].c0 + c];
    }
    printf("};\nstatic const uint32_t %s_pal[%d] = {\n", name, npal);
    for (int i = 0; i < npal; i++) printf("  0xFF%06X,\n", pal[i]);
    printf("};\nconst Shape g_%s = { %s_pts, %s_lens, %s_f, %d, %s_pal, %d, %d };\n",
           name, name, name, name, nf, name, npal, base);

    fprintf(stderr, "svg2poly: %s → %d points / %d contours / %d fills / %d colours → %d bytes\n",
            argv[1], np, nc, nf, npal, np * 4 + nc * 2 + nf * 6 + npal * 4);
    return 0;
}
