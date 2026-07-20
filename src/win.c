// win.c — the Windows platform layer, the twin of mac.c. Same five questions: give me a window,
// give me memory that reaches the screen, what key is down, what time is it, should I stop.
// Win32 for the window (GDI blits the framebuffer), waveOut for sound, and that's the whole port.
// Cross-compiled from the Mac with `zig cc -target x86_64-windows-gnu` — see build-win.sh.
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "synth.h"
#include "game.h"
#include "g3d.h"
#include "net.h"          // deterministic lockstep networking (winsock2 fenced inside net.c)
#include "games.gen.h"   // the cartridge roster, generated from games/*.c by tools/gen-games.sh

const Game *g_switch_to;

static uint32_t g_rgba[MAXFBW * MAXFBH];   // g_pal is 0xAARRGGBB; a BI_RGB DIB reads it as RRGGBB
static uint8_t  g_keys[256];               // by Windows virtual-key code
static int      g_running;
static HWND     g_hwnd;

// ---- display: palette indices -> RGB, stretched into the client area, pixels kept sharp -----
static void blit(HDC hdc) {
    for (int i = 0; i < g_fbw * g_fbh; i++) g_rgba[i] = g_pal[g_fb[i]];
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_fbw;
    bi.bmiHeader.biHeight = -g_fbh;         // negative = top-down, so row 0 is the top
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    RECT rc; GetClientRect(g_hwnd, &rc);
    SetStretchBltMode(hdc, COLORONCOLOR);   // nearest-neighbour: a 1994 pixel stays a pixel
    StretchDIBits(hdc, 0, 0, rc.right, rc.bottom, 0, 0, g_fbw, g_fbh,
                  g_rgba, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            // Edge, not level: WM_KEYDOWN auto-repeats, so only the DOWN transition (key was up)
            // pulses the one-frame latches the shell consumes — same contract mac.c gets from
            // its `t==10 && !g_keys[kc]` guard. Tab toggles the camera; Esc is the two-stage back
            // (route it through g_esc; the main loop decides console-vs-quit, never an instakill).
            if (w == VK_TAB    && !g_keys[VK_TAB])    g_view_toggle = 1;
            if (w == VK_ESCAPE && !g_keys[VK_ESCAPE]) g_esc = 1;
            if (w < 256) g_keys[w] = 1;
            return 0;
        case WM_KEYUP: case WM_SYSKEYUP:
            if (w < 256) g_keys[w] = 0;
            return 0;
        case WM_PAINT: { PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps); blit(hdc); EndPaint(h, &ps); return 0; }
        case WM_CLOSE: case WM_DESTROY: g_running = 0; return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ---- audio: a thread keeps waveOut fed, computing samples on demand. No file, no decoder. -----
#define AUD_BUFS   4
#define AUD_FRAMES 1024
static HWAVEOUT   g_wo;
static WAVEHDR    g_hdr[AUD_BUFS];
static int16_t    g_abuf[AUD_BUFS][AUD_FRAMES * 2];

static DWORD WINAPI audio_thread(LPVOID p) {
    (void)p;
    for (int b = 0; b < AUD_BUFS; b++) {
        g_hdr[b].lpData = (LPSTR)g_abuf[b];
        g_hdr[b].dwBufferLength = AUD_FRAMES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_wo, &g_hdr[b], sizeof(WAVEHDR));
        synth_render(g_abuf[b], AUD_FRAMES);
        waveOutWrite(g_wo, &g_hdr[b], sizeof(WAVEHDR));
    }
    while (g_running) {
        for (int b = 0; b < AUD_BUFS; b++) {
            if (g_hdr[b].dwFlags & WHDR_DONE) {
                synth_render(g_abuf[b], AUD_FRAMES);
                g_hdr[b].dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_wo, &g_hdr[b], sizeof(WAVEHDR));
            }
        }
        Sleep(2);
    }
    return 0;
}

static void audio_start(void) {
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = SR;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = SR * 4;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    CreateThread(0, 0, audio_thread, 0, 0, 0);
}

// Keyboard -> input for two characters. Character A reads WASD + Space + E; character B reads the
// arrows + Enter + Right-Shift or '/'. Same local co-op mac.c establishes; the sim never asks.
static void read_input(Input in[2]) {
    // Dual-stick, mirroring mac.c: WASD is P1's LEFT stick (W/S forward-back, A/D STRAFE); the
    // arrows are P1's RIGHT stick (LOOK: left/right yaw, up/down pitch) AND double as P2's move.
    in[0] = (Input){ (int8_t)(g_keys['D'] - g_keys['A']),            // strafe
                     (int8_t)(g_keys['W'] - g_keys['S']),            // forward/back
                     (int8_t)(g_keys[VK_RIGHT] - g_keys[VK_LEFT]),   // look yaw
                     (int8_t)(g_keys[VK_UP] - g_keys[VK_DOWN]),      // look pitch
                     g_keys[VK_SPACE], g_keys['E'] };
    in[1] = (Input){ (int8_t)(g_keys[VK_RIGHT] - g_keys[VK_LEFT]),   // P2 move x
                     (int8_t)(g_keys[VK_UP] - g_keys[VK_DOWN]),      // P2 move y
                     0, 0,                                           // P2 has no look stick on the keys
                     g_keys[VK_RETURN],
                     (uint8_t)(g_keys[VK_RSHIFT] | g_keys[VK_OEM_2]) };
}

// One lockstep frame — the twin of mac.c's net_step. Exchange my input+checksum for the peer's,
// place both inputs by role, and every NET_CHECK_EVERY frames trip the desync wire if the two
// checksums have drifted. Returns non-zero when the peer is gone.
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
    // Built with -mwindows, so a double-click opens no console — just the game window. But if this
    // WAS launched from a terminal, attach to it so --headless and --help still print there (the
    // cross-platform determinism check needs its stdout). No parent console = nothing attaches.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    int rw = 640, rh = 360;
    const char *runmode = 0; int modearg = 0;
    const char *keys = 0;
    const Game *g = &game_menu;
    const Game *const *games = GEN_GAMES;   // whatever cartridges live in games/, in folder order
    #define NGAMES GEN_NGAMES
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--res") && a + 2 < argc) { rw = atoi(argv[a+1]); rh = atoi(argv[a+2]); a += 2; }
        else if (!strcmp(argv[a], "--keys") && a + 1 < argc) { keys = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--game") && a + 1 < argc) {
            g = 0;
            if (!strcmp(argv[a+1], "menu")) g = &game_menu;
            for (int k = 0; k < NGAMES; k++) if (!strcmp(argv[a+1], games[k]->name)) g = games[k];
            if (!g) { fprintf(stderr, "cvertex: no game called '%s'\n", argv[a+1]); return 1; }
            a++;
        }
        else if (argv[a][0] == '-' && a + 1 < argc) { runmode = argv[a]; modearg = atoi(argv[a+1]); a++; }
    }
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

    // --headless N: the same determinism harness mac.c has, so a Windows build's sim can be
    // checked against the Mac's — same input, same checksum, or the port changed the game.
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

    WNDCLASS wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = "cvertex";
    RegisterClass(&wc);

    RECT r = { 0, 0, g_fbw * 2, g_fbh * 2 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindow("cvertex", g->name, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          0, 0, wc.hInstance, 0);
    ShowWindow(g_hwnd, SW_SHOW);

    synth_init();
    audio_start();

    // Fixed 60Hz timestep off the high-resolution counter — the sim consumes a fixed dt so it
    // stays deterministic, decoupled from how fast the machine can actually draw.
    timeBeginPeriod(1);
    LARGE_INTEGER freq, next; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&next);
    LONGLONG step = freq.QuadPart / 60;

    // Lockstep bookkeeping (only live once a menu co-op or --host/--join stands the net up).
    uint64_t mysum = net_active() ? g->checksum() : 0;
    long     net_frame = 0;

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
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
        // The two-stage Esc, mirroring mac.c: the menu eats g_esc in its own tick to start the
        // CRT power-off, so a latch still set means a real cartridge ignored it — swap back to the
        // console, flagged as a return so the insert plays in reverse. g_quit ends the loop only
        // after the menu's power-off has finished. Windows Esc no longer instakills.
        if (g_esc) {
            if (g != &game_menu) { g_menu_return = 1; g_switch_to = &game_menu; }
            g_esc = 0;
        }
        if (g_quit) break;
        if (g_switch_to) {
            g = g_switch_to; g_switch_to = 0;
            music_play(0, 0, 0, 0);
            g3d_light(0, 0, 0);   // and back to the headlamp — a cartridge's light is content, like its song
            // Menu-driven co-op (g_coop: 1 HOST / 2 JOIN), the command-line-free twin of --host/--join.
            // 🔴 SEAM (same as mac.c): JOIN has no on-screen IP entry yet, so it targets 127.0.0.1 —
            // fine for a two-window test on one box; remote co-op still needs the --join <ip> flag.
            int was_active = net_active();
            if (g_coop && !was_active) {
                int ok = (g_coop == 1) ? net_host(NET_DEFAULT_PORT)
                                       : net_join("127.0.0.1", NET_DEFAULT_PORT);
                if (ok != 0) fprintf(stderr, "[net] menu co-op setup failed — staying solo.\n");
                g_coop = 0;
            }
            g->init();
            memset(g_keys, 0, sizeof g_keys);
            SetWindowText(g_hwnd, g->name);
            if (net_active()) { mysum = g->checksum(); net_frame = was_active ? net_frame + 1 : 0; }
            continue;
        }
        g->audio();
        g->draw();
        HDC hdc = GetDC(g_hwnd); blit(hdc); ReleaseDC(g_hwnd, hdc);

        if (net_active()) { mysum = g->checksum(); net_frame++; }

        next.QuadPart += step;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        LONGLONG wait = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (wait > 0) Sleep((DWORD)wait); else next = now;
    }
    return 0;
}
