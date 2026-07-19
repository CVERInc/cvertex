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
            if (w < 256) g_keys[w] = 1;
            if (w == VK_ESCAPE) g_running = 0;
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
    in[0] = (Input){ (int8_t)(g_keys['D'] - g_keys['A']),
                     (int8_t)(g_keys['W'] - g_keys['S']),
                     g_keys[VK_SPACE], g_keys['E'] };
    in[1] = (Input){ (int8_t)(g_keys[VK_RIGHT] - g_keys[VK_LEFT]),
                     (int8_t)(g_keys[VK_UP] - g_keys[VK_DOWN]),
                     g_keys[VK_RETURN],
                     (uint8_t)(g_keys[VK_RSHIFT] | g_keys[VK_OEM_2]) };
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
            else { in[0] = (Input){ (int8_t)((f / 17) % 3 - 1), 0, (f % 23) == 0, 0 };
                   in[1] = (Input){ (int8_t)((f / 11) % 3 - 1), 0, (f % 31) == 0, 0 }; }
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

    while (g_running) {
        MSG msg;
        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        if (!g_running) break;

        Input in[2]; read_input(in);
        g->tick(in);
        if (g_switch_to) {
            g = g_switch_to; g_switch_to = 0;
            music_play(0, 0, 0, 0);
            g->init();
            memset(g_keys, 0, sizeof g_keys);
            SetWindowText(g_hwnd, g->name);
            continue;
        }
        g->audio();
        g->draw();
        HDC hdc = GetDC(g_hwnd); blit(hdc); ReleaseDC(g_hwnd, hdc);

        next.QuadPart += step;
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        LONGLONG wait = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (wait > 0) Sleep((DWORD)wait); else next = now;
    }
    return 0;
}
