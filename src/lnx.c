// lnx.c — the Linux platform layer, the twin of mac.c and win.c. Same five questions: give me a
// window, give me memory that reaches the screen, what key is down, what time is it, should I stop.
// Xlib for the window (an XImage blits the framebuffer), ALSA for sound, and that's the whole port.
// X11 and ALSA are system libraries here exactly as Cocoa is on macOS and Win32 is on Windows —
// no third-party dependency. Build with build-linux.sh.
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "core.h"
#include "synth.h"
#include "game.h"
#include "g3d.h"
#include "net.h"          // deterministic lockstep networking (BSD sockets, shared with mac.c)
#include "games.gen.h"   // the cartridge roster, generated from games/*.c by tools/gen-games.sh

const Game *g_switch_to;

// ---- our own compact key state. X keysyms are large sparse values, so we fold the handful we care
// about into a small dense array the way win.c indexes by virtual-key code.
enum { K_A, K_D, K_W, K_S, K_SPACE, K_E,
       K_LEFT, K_RIGHT, K_UP, K_DOWN, K_RETURN, K_RSHIFT, K_SLASH,
       K_TAB, K_ESCAPE, K_F3, NKEYS };
static uint8_t g_keys[NKEYS];
static int     g_running;

// ---- display state
static Display *g_dpy;
static Window   g_win;
static GC       g_gc;
static Visual  *g_visual;
static int      g_depth;
static XImage  *g_img;
static uint32_t *g_px;              // window-sized pixel buffer the XImage draws from
static int      g_win_w, g_win_h;   // current window size (framebuffer is scaled into it)
static int      g_rs, g_gs, g_bs;   // shift for red/green/blue, taken from the visual's masks
static int      g_le;               // host byte order: 1 = little-endian
static Atom     g_wmdelete;

static int mask_shift(unsigned long m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }

// Build (or rebuild, on resize) the XImage that carries pixels to the screen.
static void make_image(void) {
    if (g_img) { g_img->data = 0; XDestroyImage(g_img); }   // detach our buffer first; we own it
    free(g_px);
    g_px = malloc((size_t)g_win_w * g_win_h * 4);
    g_img = XCreateImage(g_dpy, g_visual, g_depth, ZPixmap, 0, (char *)g_px, g_win_w, g_win_h, 32, 0);
    // We compose pixel words in host order, so tell X to read them in host order regardless of the
    // server's own — otherwise a big-endian display would swap our channels.
    g_img->byte_order = g_le ? LSBFirst : MSBFirst;
}

// ---- display: palette indices -> RGB, nearest-neighbour scaled into the window, pixels kept sharp.
// g_pal is 0xAARRGGBB; the X visual is usually a BGRA/32 TrueColor, so we recompose each channel into
// the visual's own masks instead of assuming a layout.
static void present(void) {
    for (int y = 0; y < g_win_h; y++) {
        int sy = y * g_fbh / g_win_h;
        const uint8_t *srow = &g_fb[sy * g_fbw];
        uint32_t *drow = &g_px[(size_t)y * g_win_w];
        for (int x = 0; x < g_win_w; x++) {
            uint32_t c = g_pal[srow[x * g_fbw / g_win_w]];
            drow[x] = (((c >> 16) & 0xFF) << g_rs) | (((c >> 8) & 0xFF) << g_gs) | ((c & 0xFF) << g_bs);
        }
    }
    XPutImage(g_dpy, g_win, g_gc, g_img, 0, 0, 0, 0, g_win_w, g_win_h);
}

// ---- audio: a thread keeps ALSA fed, computing samples on demand. No file, no decoder. -----
#define AUD_FRAMES 1024
static snd_pcm_t *g_pcm;

static void *audio_thread(void *p) {
    (void)p;
    int16_t buf[AUD_FRAMES * 2];
    while (g_running) {
        synth_render(buf, AUD_FRAMES);
        snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, buf, AUD_FRAMES);
        if (n == -EPIPE) snd_pcm_prepare(g_pcm);        // xrun (underrun): reset and keep going
        else if (n < 0)  snd_pcm_recover(g_pcm, (int)n, 1);
    }
    return 0;
}

static void audio_start(void) {
    if (snd_pcm_open(&g_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { g_pcm = 0; return; }
    // 16-bit little-endian, 2 channels, SR, a small (~20ms) latency for a tight period.
    if (snd_pcm_set_params(g_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           2, SR, 1, 20000) < 0) {
        snd_pcm_close(g_pcm); g_pcm = 0; return;
    }
    pthread_t t;
    pthread_create(&t, 0, audio_thread, 0);
    pthread_detach(t);
}

// ---- input: X keysym -> our compact key index, or -1 for a key we don't bind.
static int keyidx(KeySym ks) {
    switch (ks) {
        case XK_a: case XK_A: return K_A;
        case XK_d: case XK_D: return K_D;
        case XK_w: case XK_W: return K_W;
        case XK_s: case XK_S: return K_S;
        case XK_e: case XK_E: return K_E;
        case XK_space:        return K_SPACE;
        case XK_Left:         return K_LEFT;
        case XK_Right:        return K_RIGHT;
        case XK_Up:           return K_UP;
        case XK_Down:         return K_DOWN;
        case XK_Return: case XK_KP_Enter: return K_RETURN;
        case XK_Shift_R:      return K_RSHIFT;
        case XK_slash:        return K_SLASH;
        case XK_Tab:          return K_TAB;
        case XK_Escape:       return K_ESCAPE;
        case XK_F3:           return K_F3;
        default:              return -1;
    }
}

// Keyboard -> input for two characters. Character A reads WASD + Space + E; character B reads the
// arrows + Enter + Right-Shift or '/'. Same local co-op mac.c and win.c establish; the sim never asks.
static void read_input(Input in[2]) {
    // Dual-stick, mirroring mac.c: WASD is P1's LEFT stick (W/S forward-back, A/D STRAFE); the
    // arrows are P1's RIGHT stick (LOOK: left/right yaw, up/down pitch) AND double as P2's move.
    in[0] = (Input){ (int8_t)(g_keys[K_D] - g_keys[K_A]),           // strafe
                     (int8_t)(g_keys[K_W] - g_keys[K_S]),           // forward/back
                     (int8_t)(g_keys[K_RIGHT] - g_keys[K_LEFT]),    // look yaw
                     (int8_t)(g_keys[K_UP] - g_keys[K_DOWN]),       // look pitch
                     g_keys[K_SPACE], g_keys[K_E] };
    in[1] = (Input){ (int8_t)(g_keys[K_RIGHT] - g_keys[K_LEFT]),    // P2 move x
                     (int8_t)(g_keys[K_UP] - g_keys[K_DOWN]),       // P2 move y
                     0, 0,                                          // P2 has no look stick on the keys
                     g_keys[K_RETURN],
                     (uint8_t)(g_keys[K_RSHIFT] | g_keys[K_SLASH]) };
}

static void handle_event(XEvent *e) {
    switch (e->type) {
        case KeyPress: case KeyRelease: {
            KeySym ks = XLookupKeysym(&e->xkey, 0);
            int down = (e->type == KeyPress);
            int idx = keyidx(ks);
            if (idx >= 0) {
                // Edge, mirroring mac.c/win.c: only the up→down transition pulses the one-frame
                // latches. Tab toggles the camera; Esc is the two-stage back (route through g_esc,
                // the main loop decides console-vs-quit — no instakill on Linux either).
                if (down && !g_keys[idx]) {
                    if (idx == K_TAB)    g_view_toggle = 1;
                    if (idx == K_ESCAPE) g_esc = 1;
                    if (idx == K_F3)     g_debug_toggle = 1;   // dev debug overlay
                }
                g_keys[idx] = (uint8_t)down;
            }
            return;
        }
        case ConfigureNotify:
            if (e->xconfigure.width != g_win_w || e->xconfigure.height != g_win_h) {
                g_win_w = e->xconfigure.width; g_win_h = e->xconfigure.height;
                make_image();
            }
            return;
        case ClientMessage:
            if ((Atom)e->xclient.data.l[0] == g_wmdelete) g_running = 0;   // window close box
            return;
    }
}

// One lockstep frame — the twin of mac.c's/win.c's net_step. Exchange my input+checksum for the
// peer's, place both inputs by role, trip the desync wire every NET_CHECK_EVERY frames. Returns
// non-zero when the peer is gone.
#define NET_CHECK_EVERY 30
static int net_step(uint64_t *mysum, long frame, Input local, Input in[2]) {
    Input remote; uint64_t rsum;
    if (net_exchange(&local, &remote, *mysum, &rsum)) {
        fprintf(stderr, "[net] peer disconnected at frame %ld — stopping network play.\n", frame);
        return -1;
    }
    if (net_role() == 0) { in[0] = local;  in[1] = remote; }
    else                 { in[0] = remote; in[1] = local;  }
    if ((frame % NET_CHECK_EVERY) == 0 && *mysum != rsum)
        fprintf(stderr, "[net] DESYNC at frame %ld (local=%llu remote=%llu)\n",
                frame, (unsigned long long)*mysum, (unsigned long long)rsum);
    return 0;
}

int main(int argc, char **argv) {
    g_running = 1;
    int host = 1;
    g_le = (*(char *)&host) == 1;

    int rw = 640, rh = 360, fullscreen = 0;
    const char *runmode = 0; int modearg = 0;
    const char *keys = 0;
    const Game *g = &game_menu;
    const Game *const *games = GEN_GAMES;   // whatever cartridges live in games/, in folder order
    #define NGAMES GEN_NGAMES
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--res") && a + 2 < argc) { rw = atoi(argv[a+1]); rh = atoi(argv[a+2]); a += 2; }
        else if (!strcmp(argv[a], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[a], "--camz") && a + 1 < argc) { g_dev_camz = (int32_t)(atof(argv[a+1]) * 65536); a++; }
        else if (!strcmp(argv[a], "--keys") && a + 1 < argc) { keys = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--game") && a + 1 < argc) {
            g = 0;
            if (!strcmp(argv[a+1], "menu")) g = &game_menu;
            for (int k = 0; k < NGAMES; k++) if (!strcmp(argv[a+1], games[k]->name)) g = games[k];
            if (!g) {
                fprintf(stderr, "cvertex: no game called '%s'. Have:", argv[a+1]);
                for (int k = 0; k < NGAMES; k++) fprintf(stderr, " %s", games[k]->name);
                fprintf(stderr, "\n");
                return 1;
            }
            a++;
        }
        else if (!strcmp(argv[a], "--help") || !strcmp(argv[a], "-h")) {
            printf("cvertex — a game engine that draws worlds out of shapes\n\n");
            printf("  --game <name>     which game to run (default: the menu)\n");
            printf("                    available:");
            for (int k = 0; k < NGAMES; k++) printf(" %s", games[k]->name);
            printf("\n");
            printf("  --res <w> <h>     framebuffer size (default: 640 360)\n");
            printf("  --fullscreen\n");
            printf("  --camz <units>    override a game's camera distance (dev)\n");
            printf("  --keys <string>   scripted input, one char per frame, player 1:\n");
            printf("                      . idle  l/r/u/d move  j jump  a act\n");
            printf("                      uppercase does the same for player 2\n\n");
            printf("  --headless <n>    run n frames, print checksums, no window\n");
            printf("  --dump <n>        run n frames, print the framebuffer as ASCII\n");
            printf("  --ppm <n>         run n frames, write the framebuffer to stdout as a PPM\n\n");
            printf("  P1: A/D/W/S + Space + E.  P2: arrows + Enter + Right-Shift or '/'.  Esc quits.\n");
            return 0;
        }
        else if (argv[a][0] == '-' && a + 1 < argc) { runmode = argv[a]; modearg = atoi(argv[a+1]); a++; }
    }
    // One char of the script -> one frame of input.
    #define SCRIPT(f) do { \
        in[0] = (Input){ 0, 0, 0, 0 }; in[1] = (Input){ 0, 0, 0, 0 }; \
        if (keys && (size_t)(f) < strlen(keys)) { \
            char c = keys[f]; int p = (c >= 'A' && c <= 'Z') ? 1 : 0; \
            char lc = (char)(p ? c - 'A' + 'a' : c); \
            if (lc == 'l') in[p].x = -1; else if (lc == 'r') in[p].x = 1; \
            else if (lc == 'u') in[p].y = 1; else if (lc == 'd') in[p].y = -1; \
            else if (lc == 'j') in[p].jump = 1; else if (lc == 'a') in[p].act = 1; \
        } \
    } while (0)

    menu_populate(games, NGAMES);
    fb_resize(rw, rh);
    g->init();

    // --headless N: the same determinism harness mac.c and win.c have, so a Linux build's sim can be
    // checked against theirs — same input, same checksum, or the port changed the game. No window,
    // so it runs with no X display at all (CI / a headless box). That is why the display is opened
    // only AFTER these runmode checks.
    if (runmode && !strcmp(runmode, "--headless")) {
        int n = modearg;
        for (int f = 0; f < n; f++) {
            Input in[2];
            if (keys) SCRIPT(f);
            else { in[0] = (Input){ (int8_t)((f / 17) % 3 - 1), 0, 0, 0, (uint8_t)((f % 23) == 0), 0 };
                   in[1] = (Input){ (int8_t)((f / 11) % 3 - 1), 0, 0, 0, (uint8_t)((f % 31) == 0), 0 }; }
            g->tick(in);
        }
        g->draw();
        uint64_t ink = 0;
        for (int i = 0; i < g_fbw * g_fbh; i++) ink = ink * 3 + g_fb[i];
        printf("game=%s frames=%d sim_checksum=%llu fb_checksum=%llu\n",
               g->name, n, (unsigned long long)g->checksum(), (unsigned long long)ink);
        return 0;
    }

    // --ppm N: run N frames, dump the framebuffer to stdout as a PPM. The ASCII dump proves the
    // geometry is right; this one proves it looks good. No display needed either.
    if (runmode && !strcmp(runmode, "--ppm")) {
        int n = modearg;
        Input in[2];
        for (int f = 0; f < n; f++) { SCRIPT(f); g->tick(in); }
        g->draw();
        printf("P6\n%d %d\n255\n", g_fbw, g_fbh);
        for (int i = 0; i < g_fbw * g_fbh; i++) {
            uint32_t c = g_pal[g_fb[i]];
            putchar((c >> 16) & 255); putchar((c >> 8) & 255); putchar(c & 255);
        }
        return 0;
    }

    // --dump N: run N frames, print the framebuffer as ASCII. A developer's eyes only.
    if (runmode && !strcmp(runmode, "--dump")) {
        int n = modearg;
        Input in[2];
        for (int f = 0; f < n; f++) { SCRIPT(f); g->tick(in); }
        g->draw();
        const char *ramp = " .:-=+*#%@";
        for (int y = 0; y < g_fbh; y += 8) {
            for (int x = 0; x < g_fbw; x += 4) {
                uint8_t c = g_fb[y * g_fbw + x];
                putchar(c == 0 ? ' ' : (c >= 8 && c < 16) ? ramp[(c - 8) + 1] : '0' + (c % 8));
            }
            putchar('\n');
        }
        return 0;
    }

    // From here on we need a screen. Open the display now, not before — everything above works
    // headless, so CI never touches X.
    g_dpy = XOpenDisplay(0);
    if (!g_dpy) { fprintf(stderr, "cvertex: cannot open X display\n"); return 1; }
    int screen = DefaultScreen(g_dpy);
    g_visual = DefaultVisual(g_dpy, screen);
    g_depth  = DefaultDepth(g_dpy, screen);
    g_rs = mask_shift(g_visual->red_mask);
    g_gs = mask_shift(g_visual->green_mask);
    g_bs = mask_shift(g_visual->blue_mask);

    g_win_w = g_fbw * 2; g_win_h = g_fbh * 2;   // 2x, same "chunky pixel" default the twins use
    g_win = XCreateSimpleWindow(g_dpy, RootWindow(g_dpy, screen), 0, 0, g_win_w, g_win_h, 0,
                                BlackPixel(g_dpy, screen), BlackPixel(g_dpy, screen));
    XStoreName(g_dpy, g_win, g->name);
    XSelectInput(g_dpy, g_win, KeyPressMask | KeyReleaseMask | StructureNotifyMask);
    g_wmdelete = XInternAtom(g_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_dpy, g_win, &g_wmdelete, 1);   // route the close box to us as a ClientMessage
    g_gc = XCreateGC(g_dpy, g_win, 0, 0);
    make_image();
    if (fullscreen) {
        Atom wm_state = XInternAtom(g_dpy, "_NET_WM_STATE", False);
        Atom fs = XInternAtom(g_dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(g_dpy, g_win, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&fs, 1);
    }
    XMapWindow(g_dpy, g_win);
    XFlush(g_dpy);

    synth_init();
    audio_start();

    // Fixed 60Hz timestep off the monotonic clock — the sim consumes a fixed dt so it stays
    // deterministic, decoupled from how fast the machine can actually draw.
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    const long step_ns = 16666667L;   // 1/60 s

    // Lockstep bookkeeping (only live once a menu co-op or --host/--join stands the net up).
    uint64_t mysum = net_active() ? g->checksum() : 0;
    long     net_frame = 0;

    while (g_running) {
        while (XPending(g_dpy)) { XEvent e; XNextEvent(g_dpy, &e); handle_event(&e); }
        if (!g_running) break;

        Input in[2];
        if (net_active()) {
            // Read the LOCAL keyboard, merge WASD+arrows into this player's one Input (input_1p),
            // exchange it with the peer. The result lands in slot [0] (host) or [1] (joiner).
            Input raw[2]; read_input(raw);
            Input local = input_1p(raw);
            if (net_step(&mysum, net_frame, local, in)) { net_close(); continue; }
        } else {
            read_input(in);
        }
        g->tick(in);
        // The two-stage Esc, mirroring mac.c/win.c: the menu eats g_esc in its own tick to start
        // the CRT power-off; a latch still set means a real cartridge ignored it — swap back to the
        // console (return flag → insert plays in reverse). g_quit ends the loop after the power-off.
        if (g_esc) {
            if (g != &game_menu) { g_menu_return = 1; g_switch_to = &game_menu; }
            g_esc = 0;
        }
        if (g_quit) break;
        if (g_switch_to) {
            g = g_switch_to; g_switch_to = 0;
            music_play(0, 0, 0, 0);   // silence the old game before the new one speaks; the platform's job
            g3d_light(0, 0, 0);       // and back to the headlamp — a cartridge's light is content, like its song
            // Menu-driven co-op (g_coop: 1 HOST / 2 JOIN), the command-line-free twin of --host/--join.
            // 🔴 SEAM (same as mac.c): JOIN targets 127.0.0.1 — a two-window test on one box; remote
            // co-op still needs a --join <ip> flag.
            int was_active = net_active();
            if (g_coop && !was_active) {
                int ok = (g_coop == 1) ? net_host(NET_DEFAULT_PORT)
                                       : net_join("127.0.0.1", NET_DEFAULT_PORT);
                if (ok != 0) fprintf(stderr, "[net] menu co-op setup failed — staying solo.\n");
                g_coop = 0;
            }
            g->init();
            memset(g_keys, 0, sizeof g_keys);
            XStoreName(g_dpy, g_win, g->name);
            if (net_active()) { mysum = g->checksum(); net_frame = was_active ? net_frame + 1 : 0; }
            continue;
        }
        g->audio();
        g->draw();
        present();
        XFlush(g_dpy);

        if (net_active()) { mysum = g->checksum(); net_frame++; }

        next.tv_nsec += step_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec || (now.tv_sec == next.tv_sec && now.tv_nsec > next.tv_nsec))
            next = now;   // fell behind: resync rather than sprint to catch up
    }
    return 0;
}
