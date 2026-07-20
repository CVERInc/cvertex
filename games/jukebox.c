// jukebox.c — a cartridge that is an album. Browse the tracks, hit play, and watch the FM synth
// drive a live LED spectrum. audio × palette, a little jukebox. The songs are public-domain
// classical (and originals), arranged into the tracker table by tools/jukebox_build.py — see
// games/jukebox_arr/*.json. Nothing here is copyrighted: the compositions are PD, the arrangements
// and the code are ours (MIT). See Notes/音樂授權筆記 for why that matters.
//
// The controls follow the picture: the track list is vertical, so UP/DOWN walks it (hold to
// scroll); the album is a tab, so LEFT/RIGHT turns it. Browsing never touches the music —
// SPACE plays the highlighted track (or stops it), and when a track ends the next one on its
// shelf drops in, because an album is something you put on, not twelve things you queue by hand.
//
// 🔴 Determinism: the visualiser reads synth_level() — the audio thread's live loudness — but ONLY
// inside draw(), never tick() or checksum(). So the sim stays pure (headless replays identical) and
// the bars are free to dance off a thread the sim never touches.
#include <stdio.h>
#include <stdlib.h>   // getenv, for the JUKE_DEMO visualiser-in-headless dev hook
#include "core.h"
#include "game.h"
#include "synth.h"
#include "text.h"
#include "jukebox_songs.h"

// ---- palette (the cartridge owns it while it's the running game) --------------------------------
static void ramp(int base, uint32_t rgb) {
    int br = (rgb >> 16) & 255, bg = (rgb >> 8) & 255, bb = rgb & 255;
    for (int i = 0; i < 8; i++) {
        int m = 60 + i * 26; int r = br * m / 255, g = bg * m / 255, b = bb * m / 255;
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        g_pal[base + i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}
static void init_palette(void) {
    g_pal[0] = 0xFF0B0A12;   // background
    g_pal[1] = 0xFF6E6A80;   // dim text
    g_pal[2] = 0xFFF4F3FA;   // bright text
    g_pal[3] = 0xFF8B5CF6;   // accent (album / playing) — the console purple
    g_pal[4] = 0xFF22D3EE;   // cyan sub-accent (composer / PD tag)
    ramp(16, 0xFF34D399);    // 16..23 meter LOW  — green
    ramp(24, 0xFFF6C453);    // 24..31 meter MID  — amber
    ramp(32, 0xFFEF4444);    // 32..39 meter HIGH — red
    // The unlit furniture: meter sockets, their bay outlines, and the progress trough all live in
    // this ramp. It used to sit at 0x2A2740, which on a real screen was barely above the
    // background — the empty bays and the un-played part of the timeline both read as nothing at
    // all. Lifted so the hardware is visible when it is idle, which is the whole point of drawing
    // a socket: you should see the slot before anything fills it.
    ramp(40, 0xFF4A4570);
}

// ---- state ---------------------------------------------------------------------------------------
static int      g_album, g_sel;      // the cursor: which shelf, which row
static int      g_play;              // JUKE index that should be sounding, -1 = stopped. Sim-owned:
                                     // tick decides it, audio() only relays it to the tracker.
static uint32_t g_frame, g_pfrm;     // g_pfrm = frames the current track has been playing
static int8_t   g_px, g_py, g_pj;    // input edge latches
static int16_t  g_hold;              // frames the browse axis has been held, for the scroll repeat
// One meter column per music channel, derived from NCHAN so the display can never quietly
// disagree with the synth again — widening the engine widens the spectrum.
#define METER_N (NCHAN - 1)
static int32_t  g_barf[METER_N];
static inline int lx_of(int x0, int c, int cell) { return x0 + c * cell; }     // smoothed meter heights, (0..8)<<8, VISUAL ONLY
// The audio-side latch: the last index actually handed to the tracker. Bookkeeping for audio(),
// NOT sim state — never hashed, so a headless run (which never calls audio) checksums the same.
static int      g_handed;

// tracks in an album, in shelf order (JUKE is already sorted classics-then-originals)
static int album_count(int album) {
    int n = 0; for (int i = 0; i < JUKE_N; i++) if (JUKE[i].album == album) n++; return n;
}
static int resolve(int album, int sel) {            // JUKE index of the sel-th track in `album`, or -1
    int k = 0;
    for (int i = 0; i < JUKE_N; i++) if (JUKE[i].album == album) { if (k == sel) return i; k++; }
    return -1;
}
static int song_frames(int idx) {                   // track length in 60Hz frames, rounded up
    return (JUKE[idx].rows * 60 + JUKE[idx].rps - 1) / JUKE[idx].rps;
}
static int next_in_album(int idx) {                 // the shelf order continues, and wraps
    int a = JUKE[idx].album, k = 0;
    for (int i = 0; i < JUKE_N; i++) if (JUKE[i].album == a) { if (i == idx) break; k++; }
    return resolve(a, (k + 1) % album_count(a));
}

static void init(void) {
    init_palette();
    g_album = 0; g_sel = 0; g_frame = 0; g_pfrm = 0;
    g_play = resolve(0, 0);                         // an album starts playing when you put it on
    g_px = g_py = g_pj = 0; g_hold = 0;
    g_handed = -2;                                  // force the first reconcile, even to "stopped"
    for (int i = 0; i < METER_N; i++) g_barf[i] = 0;
}

// ---- tick: pure. UP/DOWN walks the list (hold to scroll), LEFT/RIGHT turns the album, SPACE
// plays or stops the highlighted track. Browsing moves only the cursor — never the music.
static void tick(const Input in[2]) {
    g_frame++;
    Input p = input_1p(in);

    int cnt = album_count(g_album); if (cnt < 1) cnt = 1;
    int step = 0;                                   // browse with repeat: step on the press,
    if (p.y) {                                      // then autoscroll after a beat
        if (p.y != g_py) { step = 1; g_hold = 0; }
        else { g_hold++; if (g_hold >= 15 && (g_hold - 15) % 5 == 0) step = 1; }
    } else g_hold = 0;
    if (step) g_sel = (g_sel + (p.y < 0 ? 1 : cnt - 1)) % cnt;   // down = next row

    if (p.x && !g_px) {                                          // turn the album tab
        g_album = (g_album + (p.x > 0 ? 1 : ALBUM_N - 1)) % ALBUM_N;
        int c2 = album_count(g_album); if (g_sel >= c2) g_sel = c2 ? c2 - 1 : 0;
    }
    if (p.jump && !g_pj) {                                       // play / stop the highlighted track
        int cur = resolve(g_album, g_sel);
        if (cur >= 0) { g_play = (g_play == cur) ? -1 : cur; g_pfrm = 0; }
    }
    // album playback: when the track runs out, the next one on its shelf drops in
    if (g_play >= 0 && (int)++g_pfrm >= song_frames(g_play)) {
        g_play = next_in_album(g_play); g_pfrm = 0;
    }
    g_px = p.x; g_py = p.y; g_pj = p.jump;
}

// ---- audio: relay tick's verdict to the tracker. music_play only when the target changes,
// or it restarts the song every frame. This is the ONLY place the jukebox makes noise.
static void audio(void) {
    if (g_play == g_handed) return;
    if (g_play < 0) music_play(0, 0, 0, 0);
    else {
        const JukeTrack *t = &JUKE[g_play];
        music_play(t->data, t->rows, t->instr, t->rps);
        if (getenv("JUKE_LOG"))
            fprintf(stderr, "[juke] play #%d '%s' rows=%d rps=%d instr=%d,%d,%d,%d,%d,%d,%d,%d\n",
                    g_play, t->title, t->rows, t->rps, t->instr[0], t->instr[1], t->instr[2], t->instr[3],
                    t->instr[4], t->instr[5], t->instr[6], t->instr[7]);
    }
    g_handed = g_play;
}

// ---- draw: Now Playing + the album's track list + an LED spectrum that dances to the synth -----
static const char *ALBUM_NAME[ALBUM_N] = { "CLASSICS", "ORIGINALS" };

static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t ci) {
    int16_t p[6] = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2 };
    poly_fill(p, 3, ci);
}

static void draw(void) {
    fb_clear(0);
    int s = g_fbh / 180; if (s < 1) s = 1;
    int cx = g_fbw / 2;

    // --- Now Playing (top): the track that is SOUNDING, which browsing does not disturb --------
    text_draw(cx - text_width("JUKEBOX", s * 2) / 2, hud_top(), s * 2, "JUKEBOX", 3);
    if (g_play >= 0) {
        const JukeTrack *t = &JUKE[g_play];
        // A long title used to run out to the frame edges. Drop it a size when it will not fit
        // inside 78% of the width, so the header keeps its margins whatever the track is called.
        int ts = s; if (text_width(t->title, ts) > g_fbw * 78 / 100 && ts > 1) ts--;
        text_draw(cx - text_width(t->title, ts) / 2, g_fbh * 17 / 100, ts, t->title, 2);
        // composer, trimmed before any "(arr...)" or ", arr..." and capped, then a PD tag
        char sub[48]; int p = 0;
        for (const char *q = t->composer; *q && *q != '(' && *q != ',' && p < 30; q++) sub[p++] = *q;
        while (p > 0 && sub[p - 1] == ' ') p--;
        // only tag it if the arrangement did not already say so — "E. GRIEG - PD" was coming out
        // as "E. GRIEG - PD - PD" because both sides added it.
        int has_pd = (p >= 2 && sub[p-2] == 'P' && sub[p-1] == 'D');
        if (!has_pd) { const char *tag = " - PD"; for (const char *q = tag; *q && p < 46; q++) sub[p++] = *q; }
        sub[p] = 0;
        text_draw(cx - text_width(sub, s) / 2, g_fbh * 22 / 100, s, sub, 4);
        // elapsed / total, and a progress line the track slides along
        int total = song_frames(g_play), el = (int)g_pfrm;
        char tm[20];
        snprintf(tm, sizeof tm, "%d:%02d/%d:%02d", el / 3600, el / 60 % 60, total / 3600, total / 60 % 60);
        text_draw(cx - text_width(tm, s) / 2, g_fbh * 29 / 100, s, tm, 1);
        int bw = g_fbw * 44 / 100, bx = cx - bw / 2, by = g_fbh * 35 / 100, bh = 2 * s;
        int fill = (int)((int64_t)bw * el / (total ? total : 1));
        int16_t trough[8] = { (int16_t)bx, (int16_t)by, (int16_t)(bx + bw), (int16_t)by,
                              (int16_t)(bx + bw), (int16_t)(by + bh), (int16_t)bx, (int16_t)(by + bh) };
        poly_fill(trough, 4, 42);
        if (fill > 0) {
            int16_t done[8] = { (int16_t)bx, (int16_t)by, (int16_t)(bx + fill), (int16_t)by,
                                (int16_t)(bx + fill), (int16_t)(by + bh), (int16_t)bx, (int16_t)(by + bh) };
            poly_fill(done, 4, 3);
        }
    } else {
        text_draw(cx - text_width("STOPPED", s) / 2, g_fbh * 17 / 100, s, "STOPPED", 1);
    }

    // --- the LED spectrum: one column per music channel, stacked segments. A real meter
    // attacks instantly and falls back slowly. 🔴 VISUAL ONLY: synth_level() is the live audio thread,
    // read here in draw and nowhere else, so the sim (tick/checksum) stays pure and deterministic.
    static int demo = -1; if (demo < 0) demo = getenv("JUKE_DEMO") ? 1 : 0;   // headless visualiser check
    int barTop = g_fbh * 37 / 100, barBot = g_fbh * 51 / 100;
    int segH = (barBot - barTop) / 8;
    // All sixteen slots are always drawn: showing only the channels a song happens to use made the
    // block change width from track to track. An idle bay is simply its eight sockets in grey —
    // that segmented read IS the look, and a frame drawn around it only muddied what the sockets
    // were already saying.
    int span = g_fbw * 80 / 100;
    int cell = span / METER_N, gap = cell / 3, colW = cell - gap;
    int x0 = cx - (METER_N * cell - gap) / 2;
    for (int c = 0; c < METER_N; c++) {
        int lvl = synth_level(c);                          // ~0..170
        if (demo) lvl = 30 + ((int)((g_frame / 3 + c * 5) % 12) * 12);   // a fake dancing pattern
        int32_t target = (int32_t)(lvl * 8 / 150); if (target > 8) target = 8;
        target <<= 8;
        if (target > g_barf[c]) g_barf[c] = target;                        // snap up
        else { g_barf[c] -= 0x50; if (g_barf[c] < 0) g_barf[c] = 0; }      // fall back slowly
        int lit = g_barf[c] >> 8;
        int lx = lx_of(x0, c, cell);
        for (int seg = 0; seg < 8; seg++) {
            int base = (seg >= lit) ? 40 : (seg >= 6 ? 32 : seg >= 3 ? 24 : 16);  // socket, or by height
            uint8_t ci = (uint8_t)(base + (seg >= lit ? 1 : 5));                   // socket dim, lit bright
            int sbot = barBot - seg * segH, stop = sbot - (segH - 2 * s);
            int16_t pts[8] = { (int16_t)lx, (int16_t)stop, (int16_t)(lx + colW), (int16_t)stop,
                               (int16_t)(lx + colW), (int16_t)sbot, (int16_t)lx, (int16_t)sbot };
            poly_fill(pts, 4, ci);
        }
    }

    // --- album tab (LEFT/RIGHT turns it — the side arrows say so) --------------------------------
    int ay = g_fbh * 58 / 100, aw = text_width(ALBUM_NAME[g_album], s);
    text_draw(cx - aw / 2, ay, s, ALBUM_NAME[g_album], 3);
    int am = ay + 3 * s;                                    // arrow midline, glyphs are 7s tall
    tri(cx - aw / 2 - 8 * s, am, cx - aw / 2 - 5 * s, am - 3 * s, cx - aw / 2 - 5 * s, am + 3 * s, 1);
    tri(cx + aw / 2 + 8 * s, am, cx + aw / 2 + 5 * s, am - 3 * s, cx + aw / 2 + 5 * s, am + 3 * s, 1);

    // --- the track list: a 3-row window, cursor kept centred, ▶ on the sounding track. Three and
//     not five because the space it gives back is what lets every line above keep its margins.
    int cnt = album_count(g_album);
    if (cnt == 0) {
        text_draw(cx - text_width("NO TRACKS YET", s) / 2, g_fbh * 70 / 100, s, "NO TRACKS YET", 1);
    }
    int start = g_sel - 1;
    if (start > cnt - 3) start = cnt - 3;
    if (start < 0) start = 0;
    for (int row = 0; row < 3 && start + row < cnt; row++) {
        int i = start + row, idx = resolve(g_album, i);
        const char *tt = idx >= 0 ? JUKE[idx].title : "";
        int ty = g_fbh * 66 / 100 + row * 10 * s, tw = text_width(tt, s);   // 9s pitch: 7s glyph + real leading
        text_draw(cx - tw / 2, ty, s, tt, i == g_sel ? 2 : 1);
        if (idx == g_play)                                   // ▶ the one you can hear
            tri(cx - tw / 2 - 8 * s, ty, cx - tw / 2 - 8 * s, ty + 6 * s, cx - tw / 2 - 4 * s, ty + 3 * s, 3);
    }
    // Two short lines, not one long one: the single 46-character hint spanned nearly the whole
    // width and sat at 94% — ink against three edges at once. Split and lifted, it stays inside
    // the same margins the title now respects.
    const char *h1 = "UP DOWN BROWSE    LEFT RIGHT ALBUM", *h2 = "SPACE PLAY";
    text_draw(cx - text_width(h1, s) / 2, g_fbh * 85 / 100, s, h1, 1);
    text_draw(cx - text_width(h2, s) / 2, g_fbh * 90 / 100, s, h2, 1);
}

static uint64_t checksum(void) {
    uint64_t h = 1469598103934665603ull;
    h = h * 31 + (uint32_t)g_album; h = h * 31 + (uint32_t)g_sel;
    h = h * 31 + (uint32_t)g_play;  h = h * 31 + g_pfrm;
    h = h * 31 + g_frame;
    return h;
}

const Game game_jukebox = { "jukebox", init, tick, audio, draw, checksum, "CVER Inc." };
