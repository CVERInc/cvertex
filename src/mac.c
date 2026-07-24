// mac.c — the macOS platform layer. Plain C, calls the objc runtime directly, no
// Objective-C compiler involved.
// It only answers five questions: give me a window, give me memory that reaches the
// screen, what key is down, what time is it, should I stop.
#include <objc/objc.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CoreGraphics.h>
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"
#include "version.h"   // CVERTEX_VERSION — the --version flag, same constant the boot screen shows
#include "synth.h"
#include "game.h"
#include "g3d.h"
#include "net.h"          // deterministic lockstep networking (opt-in via --host/--join)
#include "games.gen.h"   // the cartridge roster, generated from games/*.c by tools/gen-games.sh

// A game asked to be replaced. The platform owns this because switching games is what a
// platform does; a game just points at the next one.
const Game *g_switch_to;
// g_menu_return / g_quit (the two-stage Esc's platform facts) live in core.c beside g_esc, so
// every platform (win.c / lnx.c) links even before it routes Esc — menu.c uses them on all builds.

#define SEL_(s) sel_registerName(s)
#define CLS_(s) ((id)objc_getClass(s))
#define MSG(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

static uint32_t g_rgba[MAXFBW * MAXFBH];
static uint8_t  g_keys[128];
static int      g_running;   // see synth.c's 16KB lesson: assign the initial value at runtime, never give it a non-zero initializer

// framebuffer (palette indices) → RGBA, then blit into the window. That's the whole of "display."
static void fbview_draw(id self, SEL _cmd, CGRect dirty) {
    (void)self; (void)_cmd; (void)dirty;
    for (int i = 0; i < g_fbw * g_fbh; i++) g_rgba[i] = g_pal[g_fb[i]];
    if (g_present_fx) g_present_fx(g_rgba, g_fbw, g_fbh);   // the light chip, if a cartridge armed it

    id nsctx = MSG(id)(CLS_("NSGraphicsContext"), SEL_("currentContext"));
    if (!nsctx) return;
    CGContextRef ctx = MSG(CGContextRef)(nsctx, SEL_("CGContext"));
    if (!ctx) return;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(0, g_rgba, (size_t)g_fbw * g_fbh * 4, 0);
    CGImageRef img = CGImageCreate(g_fbw, g_fbh, 8, 32, g_fbw * 4, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, 0, 0, kCGRenderingIntentDefault);

    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);  // pixels need to stay sharp
    CGRect box = CGContextGetClipBoundingBox(ctx);
    CGContextDrawImage(ctx, box, img);

    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

// Audio: the OS asks for samples, we compute them on demand. No file, no decoder.
static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 frames,
                          AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    synth_render((int16_t *)io->mBuffers[0].mData, frames);
    return noErr;
}

static void audio_start(void) {
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                    kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(0, &d);
    if (!comp) return;
    AudioUnit au;
    if (AudioComponentInstanceNew(comp, &au) != noErr) return;
    AudioStreamBasicDescription f = { 0 };
    f.mSampleRate = SR;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4;
    f.mBytesPerPacket = 4;
    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    AURenderCallbackStruct cb = { render_cb, 0 };
    AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioUnitInitialize(au);
    AudioOutputUnitStart(au);
}

static void pump_events(id app, id mode) {
    id past = MSG(id)(CLS_("NSDate"), SEL_("distantPast"));
    g_digit = -1;   // the digit pulse lives for exactly the frame its key went down
    for (;;) {
        id ev = MSG(id, unsigned long long, id, id, BOOL)(app,
            SEL_("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ULL, past, mode, YES);
        if (!ev) break;
        unsigned long t = MSG(unsigned long)(ev, SEL_("type"));

        // 🔴 A modifier is not a key. Shift, control and friends never produce a keyDown —
        // they produce flagsChanged, and a loop that only reads keyDown simply never hears
        // them. Right Shift was bound to a player action for a while and could not have
        // worked once. The device-dependent bits are what separate left from right; the
        // documented ones don't.
        if (t == 12) {  // NSEventTypeFlagsChanged
            unsigned long m = MSG(unsigned long)(ev, SEL_("modifierFlags"));
            g_keys[60] = (m & 0x0004) ? 1 : 0;   // right shift
            g_keys[56] = (m & 0x0002) ? 1 : 0;   // left shift
            continue;
        }

        // The pointer. Types 1..7 are the left/right mouse (Down/Up, MouseMoved, the two Dragged
        // variants); 25..27 are the OTHER-button mouse (middle, for the editor's Maya camera).
        // locationInWindow is window-space POINTS with a bottom-left origin; the blit scales the
        // g_fbw×g_fbh image to fill the (non-resizable) content view at exactly 2×, and g_fb's
        // origin is top-left — so halve, and flip y. Buttons set/clear bits. We do NOT consume the
        // event: the window still wants it for title-bar drags.
        if ((t >= 1 && t <= 7) || (t >= 25 && t <= 27)) {
            CGPoint p = MSG(CGPoint)(ev, SEL_("locationInWindow"));
            int fx = (int)(p.x * 0.5), fy = (int)((double)g_fbh - p.y * 0.5);
            if (fx < 0) fx = 0; else if (fx > g_fbw - 1) fx = g_fbw - 1;
            if (fy < 0) fy = 0; else if (fy > g_fbh - 1) fy = g_fbh - 1;
            g_mx = fx; g_my = fy;
            if      (t == 1) g_mbtn |= 1;              // LeftMouseDown
            else if (t == 2) g_mbtn &= (uint8_t)~1;    // LeftMouseUp
            else if (t == 3) g_mbtn |= 2;              // RightMouseDown
            else if (t == 4) g_mbtn &= (uint8_t)~2;    // RightMouseUp
            else if (t == 25 || t == 26) {             // OtherMouseDown/Up — only the MIDDLE (button 2)
                long bn = MSG(long)(ev, SEL_("buttonNumber"));
                if (bn == 2) { if (t == 25) g_mbtn |= 4; else g_mbtn &= (uint8_t)~4; }
            }
            // t == 27 (OtherMouseDragged) falls through with position updated — a middle-drag orbit.
            // fall through: forward the event so the rest of AppKit still sees it
        }

        // The scroll wheel — zoom for the editor's camera. Accumulate notches; any nonzero delta
        // registers at least ±1 so a trackpad's fine scrolling still zooms. UI only, never hashed.
        if (t == 22) {  // NSEventTypeScrollWheel
            double dy = MSG(double)(ev, SEL_("scrollingDeltaY"));
            int d = (int)dy; if (d == 0 && dy != 0.0) d = (dy > 0.0) ? 1 : -1;
            g_wheel += d;
        }

        if (t == 10 || t == 11) {  // NSEventTypeKeyDown / KeyUp
            unsigned short kc = MSG(unsigned short)(ev, SEL_("keyCode"));
            // Tab (keycode 48) is the view/camera cycle — a single edge pulse into the
            // draw-side latch a cartridge consumes. Fire ONLY on the up->down transition
            // (g_keys[48] still 0), so macOS key-repeat's stream of keyDowns can't strobe
            // the camera while Tab is held. It's not in the Input struct on purpose: the
            // camera is a comfort choice, not a move the deterministic sim ever sees.
            if (kc == 48 && t == 10 && !g_keys[48]) g_view_toggle = 1;
            // Esc (keycode 53) is the two-stage back button now, not a hard quit. Pulse the
            // latch on the keydown EDGE only (like Tab) so key-repeat can't strobe it; the
            // routing in the main loop decides what it means (menu → power-off, game → back to
            // the console). See core.h / game.h.
            if (kc == 53 && t == 10 && !g_keys[53]) g_esc = 1;
            // '/' (keycode 44, and '?' with shift) → a one-frame edge pulse; a cartridge toggles its
            // on-demand manual. Edge-only so a held key can't strobe it open/shut.
            if (kc == 44 && t == 10 && !g_keys[44]) g_help_toggle = 1;
            // F3 (keycode 99) → the dev debug overlay, edge-only like the rest (Minecraft's binding).
            if (kc == 99 && t == 10 && !g_keys[99]) g_debug_toggle = 1;
            // Number row 1..9,0 → a one-frame digit pulse on the keydown EDGE (like Tab/Esc, so a held
            // key can't strobe). A cartridge can read g_digit for a dev-gated debug shortcut; it's never
            // in the Input struct, so it steers no deterministic sim.
            if (t == 10 && !g_keys[kc]) switch (kc) {
                case 18: g_digit = 1; break; case 19: g_digit = 2; break; case 20: g_digit = 3; break;
                case 21: g_digit = 4; break; case 23: g_digit = 5; break; case 22: g_digit = 6; break;
                case 26: g_digit = 7; break; case 28: g_digit = 8; break; case 25: g_digit = 9; break;
                case 29: g_digit = 0; break;
            }
            if (kc < 128) g_keys[kc] = (t == 10);

            // Don't pass it on. A keyDown that reaches the end of the responder chain
            // unhandled makes AppKit beep — and holding a key down beeps once per repeat.
            // The game has already taken it; forwarding it as well only asks Cocoa to
            // find someone else who cares, and the beep is Cocoa saying nobody did.
            //
            // Command-modified keys still go through, so the menu's equivalents work.
            unsigned long mods = MSG(unsigned long)(ev, SEL_("modifierFlags"));
            if (!(mods & (1UL << 20))) continue;   // NSEventModifierFlagCommand
        }
        MSG(void, id)(app, SEL_("sendEvent:"), ev);
    }
}

// Keyboard → input for two characters. Local co-op is established right here: character A
// reads WASD, character B reads the arrows. The sim doesn't know or care where input comes
// from.
//
// 🔴 y was hard-coded to 0 for weeks. Nothing on a keyboard could ever produce it, so the
// menu's up/down did nothing and a game's morph key never once fired — while --keys set y
// happily, so every test passed. A test rig that can reach somewhere the player can't is
// a test rig that verifies a game nobody is playing.
static void read_input(Input in[2]) {
    // Dual-stick on one keyboard. WASD is player 1's LEFT stick (MOVE): W/S forward-back,
    // A/D strafe. The arrows are player 1's RIGHT stick (LOOK): left/right yaw, up/down pitch.
    // Jump and act get their own keys, because a 3D game needs both ground axes and can't
    // spend one on jumping. This maps 1:1 to a Bluetooth gamepad's two analog sticks later.
    //
    // 🔴 The arrows do DOUBLE DUTY, on purpose: they fill player 1's LOOK (in[0].rx/ry) AND
    // player 2's MOVE (in[1].x/y). A single-player dual-stick game (WASD move + arrows
    // look) reads in[0]'s four axes and ignores in[1]; a local-2P game (WASD vs arrows)
    // reads in[1].x/y and ignores the look fields. Each cartridge reads only what it wants, so
    // both schemes live on the same keyboard with no key fighting over two meanings at once.
    in[0] = (Input){ (int8_t)(g_keys[2] - g_keys[0]),        // D - A   (P1 strafe)
                     (int8_t)(g_keys[13] - g_keys[1]),       // W - S   (P1 forward/back)
                     (int8_t)(g_keys[124] - g_keys[123]),    // -> - <- (P1 look yaw)
                     (int8_t)(g_keys[126] - g_keys[125]),    // up - down (P1 look pitch)
                     g_keys[49],                             // space
                     g_keys[14] };                           // E
    in[1] = (Input){ (int8_t)(g_keys[124] - g_keys[123]),          // -> - <-   (P2 move x)
                     (int8_t)(g_keys[126] - g_keys[125]),          // up - down (P2 move y)
                     0, 0,                                         // P2 has no look stick on the keys
                     g_keys[36],                                   // return
                     (uint8_t)(g_keys[60] | g_keys[44]) };         // right shift or /
}

// One lockstep frame. `local` is THIS machine's player, already merged to a single Input.
// We send it (plus our pre-tick checksum) and block for the peer's, then assemble the SAME
// in[2] on both ends — host's input in [0], joiner's in [1] — regardless of who we are. The
// caller ticks with in[2] and then refreshes *mysum with g->checksum().
//
// *mysum is the state checksum as of the END of the previous frame (or just after init for
// frame 0). Both peers exchange that same quantity, so a mismatch means the sims have
// already diverged — the desync tripwire. We don't crash; lockstep's job is to notice.
#define NET_CHECK_EVERY 30   // compare checksums this often (they ride every packet regardless)
static int net_step(uint64_t *mysum, long frame, Input local, Input in[2]) {
    Input remote; uint64_t rsum;
    if (net_exchange(&local, &remote, *mysum, &rsum)) {
        fprintf(stderr, "[net] peer disconnected at frame %ld — stopping network play.\n", frame);
        return -1;
    }
    if (net_role() == 0) { in[0] = local;  in[1] = remote; }   // host drives P1
    else                 { in[0] = remote; in[1] = local;  }   // joiner drives P2
    if ((frame % NET_CHECK_EVERY) == 0 && *mysum != rsum)
        fprintf(stderr, "[net] DESYNC at frame %ld (local=%llu remote=%llu)\n",
                frame, (unsigned long long)*mysum, (unsigned long long)rsum);
    return 0;
}

int main(int argc, char **argv) {
    g_running = 1;

    // Resolution is an argument, because it is a look, not an architecture. 640x360
    // scaled up is a 1994 pixel grid; the display's own resolution is the same polygons,
    // sharp. Same art, same binary.
    int rw = 640, rh = 360, fullscreen = 0;
    const char *runmode = 0; int modearg = 0;
    // The engine runs a game; which one is an argument. Nothing below this line knows
    // what the game is about.
    const Game *g = &game_menu;   // no argument = the list, not somebody's test level
    // Scripted input, one character per frame. This is the dividend a deterministic sim
    // was always going to pay and I kept not collecting: the same string produces the
    // same run, so a question like "does a flat shape fit through that gap" gets an
    // answer instead of an opinion. It's also, exactly, a replay.
    const char *keys = 0;
    // The pointer's answer to --keys: a scripted mouse track for --ppm/--headless, since
    // neither a headless render nor a test harness has a real mouse. "x,y,btn;x,y,btn;..."
    // one entry per frame (btn: bit0=left, bit1=right); the last entry repeats once it runs
    // out. This is what makes the box editor verifiable without a live hand on the mouse.
    const char *mousearg = 0;
    // The Tab key's answer to --keys: a scripted view-toggle track for --ppm/--headless,
    // since neither a headless render nor a test harness can press Tab. One char per frame;
    // 't' (or 'T') PULSES the view-toggle latch that frame, anything else leaves it be.
    // Kept a SEPARATE channel from --keys on purpose: a camera cycle must never steal a
    // frame of movement, so the SAME --keys drives a byte-identical sim whether the camera
    // is toggled or not — which is exactly how the draw-side/sim split gets proven.
    const char *viewarg = 0;
    // The RIGHT stick's answer to --keys: a scripted LOOK track for --ppm/--headless, since
    // neither a headless render nor a test harness can feel an analog stick (the same reason
    // --mouse and --view exist). One char per frame, driving player 1's look field in[0].rx/ry:
    //   .  idle      l/r  look left / right (yaw)      u/d  look up / down (PITCH)
    // A SEPARATE channel from --keys so a self-check can drive LOOK in isolation without also
    // waking player 2 (whom the real arrow keys would) — exactly what verifies pitch headlessly.
    const char *lookarg = 0;
    // The scroll wheel's answer to --keys: one char per frame — 'u' zooms in a notch, 'd' out, any
    // other char idles — so the editor's Maya zoom is renderable/verifiable headlessly with --ppm.
    const char *scrollarg = 0;
    // Networking is strictly opt-in: with neither --host nor --join, none of this is touched
    // and the engine runs exactly as it always has.
    int net_want = 0, net_is_host = 0, net_port = NET_DEFAULT_PORT;
    const char *net_ip = 0;
    const Game *const *games = GEN_GAMES;   // whatever cartridges live in games/, in folder order
    #define NGAMES GEN_NGAMES
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--res") && a + 2 < argc) { rw = atoi(argv[a+1]); rh = atoi(argv[a+2]); a += 2; }
        else if (!strcmp(argv[a], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[a], "--camz") && a + 1 < argc) { g_dev_camz = (int32_t)(atof(argv[a+1]) * 65536); a++; }
        else if (!strcmp(argv[a], "--keys") && a + 1 < argc) { keys = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--mouse") && a + 1 < argc) { mousearg = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--view") && a + 1 < argc) { viewarg = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--look") && a + 1 < argc) { lookarg = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--scroll") && a + 1 < argc) { scrollarg = argv[a+1]; a++; }
        else if (!strcmp(argv[a], "--host")) {   // listen, become player 1; port is optional
            net_want = 1; net_is_host = 1;
            if (a + 1 < argc && argv[a+1][0] >= '0' && argv[a+1][0] <= '9') { net_port = atoi(argv[a+1]); a++; }
        }
        else if (!strcmp(argv[a], "--join") && a + 1 < argc) {   // connect, become player 2
            net_want = 1; net_is_host = 0; net_ip = argv[a+1]; a++;
            if (a + 1 < argc && argv[a+1][0] >= '0' && argv[a+1][0] <= '9') { net_port = atoi(argv[a+1]); a++; }
        }
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
        else if (!strcmp(argv[a], "--version")) { printf("cvertex %s\n", CVERTEX_VERSION); return 0; }
        else if (!strcmp(argv[a], "--help") || !strcmp(argv[a], "-h")) {
            printf("cvertex %s — a game engine that draws worlds out of shapes\n\n", CVERTEX_VERSION);
            printf("  --game <name>     which game to run (default: the menu)\n");
            printf("                    available:");
            for (int k = 0; k < NGAMES; k++) printf(" %s", games[k]->name);
            printf("\n");
            printf("  --host [port]     lockstep co-op: listen, be player 1 (default port %d)\n", NET_DEFAULT_PORT);
            printf("  --join <ip> [port] lockstep co-op: connect to a host, be player 2\n");
            printf("  --res <w> <h>     framebuffer size (default: 640 360)\n");
            printf("  --fullscreen\n");
            printf("  --camz <units>    override a game's camera distance (dev)\n");
            printf("  --keys <string>   scripted input, one char per frame, player 1:\n");
            printf("                      . idle  l/r/u/d move  j jump  a act\n");
            printf("                      uppercase does the same for player 2\n");
            printf("  --mouse <string>  scripted pointer for --ppm/--headless, one entry per frame:\n");
            printf("                      \"x,y,btn;x,y,btn;...\"  btn: bit0=left bit1=right\n");
            printf("  --view <string>   scripted view-toggle (Tab) for --ppm/--headless, one char per frame:\n");
            printf("                      't' pulses the camera cycle that frame, anything else idles\n");
            printf("  --look <string>   scripted RIGHT-stick LOOK for --ppm/--headless, one char per frame:\n");
            printf("                      . idle  l/r look yaw left/right  u/d look pitch up/down (player 1)\n");
            printf("  --scroll <string> scripted scroll WHEEL for --ppm/--headless, one char per frame:\n");
            printf("                      . idle  u zoom in (wheel up)  d zoom out (wheel down)\n\n");
            printf("  --headless <n>    run n frames, print checksums, no window\n");
            printf("  --dump <n>        run n frames, print the framebuffer as ASCII\n");
            printf("  --ppm <n>         run n frames, write the framebuffer to stdout as a PPM\n");
            printf("  --version         print the version and exit\n\n");
            printf("  Controls vary by game: A/D/W + Space/E for one player, the arrows + Enter for a second. Esc quits.\n");
            return 0;
        }
        else if (argv[a][0] == 0x2D && a + 1 < argc) { runmode = argv[a]; modearg = atoi(argv[a+1]); a++; }
    }
    // One char of the script -> one frame of input.
    #define SCRIPT(f) do { \
        in[0] = (Input){ 0, 0, 0, 0, 0, 0 }; in[1] = (Input){ 0, 0, 0, 0, 0, 0 }; \
        if (keys && (size_t)(f) < strlen(keys)) { \
            char c = keys[f]; int p = (c >= 'A' && c <= 'Z') ? 1 : 0; \
            char lc = (char)(p ? c - 'A' + 'a' : c); \
            if (lc == 'l') in[p].x = -1; else if (lc == 'r') in[p].x = 1; \
            else if (lc == 'u') in[p].y = 1; else if (lc == 'd') in[p].y = -1; \
            else if (lc == 'j') in[p].jump = 1; else if (lc == 'a') in[p].act = 1; \
        } \
        if (lookarg && (size_t)(f) < strlen(lookarg)) { \
            char c = lookarg[f]; \
            if (c == 'l') in[0].rx = -1; else if (c == 'r') in[0].rx = 1; \
            else if (c == 'u') in[0].ry = 1; else if (c == 'd') in[0].ry = -1; \
        } \
    } while (0)

    // Parse the scripted mouse track once, into parallel arrays. In bss (static), so it costs
    // nothing on disk and doesn't touch the stack.
    static int mouse_x[8192], mouse_y[8192]; static uint8_t mouse_b[8192];
    int mouse_n = 0;
    if (mousearg) {
        const char *s = mousearg;
        while (*s && mouse_n < 8192) {
            char *e;
            long x = strtol(s, &e, 10); if (e == s) break; s = e; if (*s == ',') s++;
            long y = strtol(s, &e, 10);                     s = e; if (*s == ',') s++;
            long b = strtol(s, &e, 10);                     s = e;
            mouse_x[mouse_n] = (int)x; mouse_y[mouse_n] = (int)y; mouse_b[mouse_n] = (uint8_t)b;
            mouse_n++;
            while (*s && *s != ';') s++; if (*s == ';') s++;
        }
    }
    // Drive the pointer globals from the track for frame f (last entry repeats when it runs out).
    #define MOUSE(f) do { if (mouse_n) { int _i = (f) < mouse_n ? (int)(f) : mouse_n - 1; \
        g_mx = mouse_x[_i]; g_my = mouse_y[_i]; g_mbtn = mouse_b[_i]; } } while (0)

    // Pulse the view-toggle latch for frame f when the --view track says 't' there. The game
    // consumes (clears) it inside its own tick, so a non-'t' frame simply leaves it at 0 —
    // no reset needed here. Unlike --mouse the track does NOT repeat past its end: a toggle
    // is an event, not a held state, so silence past the string means "no more toggles."
    #define VIEW(f) do { if (viewarg && (size_t)(f) < strlen(viewarg) && \
        (viewarg[f] == 't' || viewarg[f] == 'T')) g_view_toggle = 1; } while (0)

    // Drive the accumulated wheel for frame f from the --scroll track: 'u' adds a notch up (zoom
    // in), 'd' a notch down. Like the real wheel it ACCUMULATES (+=), so the tool reading-and-
    // resetting each frame sees exactly one frame's worth; silence past the string means no scroll.
    #define SCROLL(f) do { if (scrollarg && (size_t)(f) < strlen(scrollarg)) { \
        char _c = scrollarg[f]; if (_c == 'u' || _c == 'U') g_wheel += 1; \
        else if (_c == 'd' || _c == 'D') g_wheel -= 1; } } while (0)

    menu_populate(games, NGAMES);
    fb_resize(rw, rh);
    // Dev hook: CV_MENU_RETURN=1 boots the menu straight into its reverse-insert, so the
    // "cart rising back out of the slot" animation is renderable headlessly with --ppm without
    // first having to launch and Esc out of a game. Same spirit as CVX_TURNTABLE.
    if (getenv("CV_MENU_RETURN")) g_menu_return = 1;
    g->init();

    // Opt-in networking: connect the two peers now, after init so the first checksum we
    // exchange describes the same freshly-initialised state on both ends. --host blocks here
    // until a peer joins. On failure we bail rather than silently drop to single-player.
    if (net_want) {
        int ok = net_is_host ? net_host(net_port) : net_join(net_ip, net_port);
        if (ok != 0) return 1;
    }

    // --headless N over the net: two processes on one machine (or a LAN) each run N frames in
    // lockstep, feeding only their OWN player, and print the final checksum. Because the host
    // supplies slot [0] and the joiner slot [1] — the SAME split a single-process --headless
    // uses — a synced pair prints the SAME sim_checksum as a solo run. That is the honest,
    // scriptable proof of lockstep. CV_NET_TRACE=1 in the env dumps every frame's checksum.
    if (net_active() && runmode && !strcmp(runmode, "--headless")) {
        int n = modearg;
        uint64_t mysum = g->checksum();
        const char *trace = getenv("CV_NET_TRACE");
        for (int f = 0; f < n; f++) {
            Input in[2];
            if (keys) SCRIPT(f);
            else { in[0] = (Input){ (int8_t)((f / 17) % 3 - 1), 0, 0, 0, (uint8_t)((f % 23) == 0), 0 };
                   in[1] = (Input){ (int8_t)((f / 11) % 3 - 1), 0, 0, 0, (uint8_t)((f % 31) == 0), 0 }; }
            Input local = (net_role() == 0) ? in[0] : in[1];   // I only own my own player
            if (net_step(&mysum, f, local, in)) break;
            MOUSE(f); VIEW(f); SCROLL(f);
            g->tick(in);
            mysum = g->checksum();
            // Test hook (this networked-headless harness only): set CV_NET_DESYNC_AT=<frame>
            // on ONE peer to corrupt the checksum it reports, proving the tripwire actually
            // fires. It perturbs only the reported checksum, never the sim.
            { const char *d = getenv("CV_NET_DESYNC_AT");
              if (d && f == atoi(d)) mysum ^= 1ull; }
            if (trace) fprintf(stderr, "f=%d sum=%llu\n", f, (unsigned long long)mysum);
        }
        g->draw();
        uint64_t ink = 0;
        for (int i = 0; i < g_fbw * g_fbh; i++) ink = ink * 3 + g_fb[i];
        printf("game=%s frames=%d sim_checksum=%llu fb_checksum=%llu\n", g->name, n, g->checksum(), ink);
        net_close();
        return 0;
    }

    // --headless N: no window, run N frames, print the checksum. Used to verify determinism.
    if (runmode && !strcmp(runmode, "--headless")) {
        int n = modearg;
        for (int f = 0; f < n; f++) {
            Input in[2];
            if (keys) SCRIPT(f);
            else { in[0] = (Input){ (int8_t)((f / 17) % 3 - 1), 0, 0, 0, (uint8_t)((f % 23) == 0), 0 };
                   in[1] = (Input){ (int8_t)((f / 11) % 3 - 1), 0, 0, 0, (uint8_t)((f % 31) == 0), 0 }; }
            MOUSE(f); VIEW(f); SCROLL(f);
            g->tick(in);
        }
        g->draw();
        uint64_t ink = 0;
        for (int i = 0; i < g_fbw * g_fbh; i++) ink = ink * 3 + g_fb[i];
        printf("game=%s frames=%d sim_checksum=%llu fb_checksum=%llu\n", g->name, n, g->checksum(), ink);
        return 0;
    }

    // --ppm N: run N frames, dump the framebuffer to stdout as a PPM. The ASCII dump
    // proves the geometry is right; this one proves it looks good — two different
    // things. A developer's eyes only, doesn't ship in a release build.
    if (runmode && !strcmp(runmode, "--ppm")) {
        int n = modearg;
        Input in[2];
        // CV_ESC_AT=<frame> pulses the Esc latch at that frame so the shell's power-off (in the
        // menu) or return-to-console routing is renderable headlessly. Dev hook, --ppm only.
        const char *escat = getenv("CV_ESC_AT"); int esc_at = escat ? atoi(escat) : -1;
        for (int f = 0; f < n; f++) { SCRIPT(f); MOUSE(f); VIEW(f); SCROLL(f); if (f == esc_at) g_esc = 1; g->tick(in); }
        g->draw();
        for (int i = 0; i < g_fbw * g_fbh; i++) g_rgba[i] = g_pal[g_fb[i]];
        if (g_present_fx) g_present_fx(g_rgba, g_fbw, g_fbh);   // screenshots see exactly what the window sees
        printf("P6\n%d %d\n255\n", g_fbw, g_fbh);
        for (int i = 0; i < g_fbw * g_fbh; i++) {
            uint32_t c = g_rgba[i];
            putchar((c >> 16) & 255); putchar((c >> 8) & 255); putchar(c & 255);
        }
        return 0;
    }

    // --dump N: run N frames, print the framebuffer as ASCII. A developer's eyes only, doesn't ship in a release build.
    if (runmode && !strcmp(runmode, "--dump")) {
        int n = modearg;
        Input in[2];
        const char *escat = getenv("CV_ESC_AT"); int esc_at = escat ? atoi(escat) : -1;
        for (int f = 0; f < n; f++) { SCRIPT(f); MOUSE(f); VIEW(f); SCROLL(f); if (f == esc_at) g_esc = 1; g->tick(in); }
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

    id app = MSG(id)(CLS_("NSApplication"), SEL_("sharedApplication"));
    MSG(void, long)(app, SEL_("setActivationPolicy:"), 0);

    // A menu bar with one item in it. Cmd-Q isn't a key the OS handles for you — it's a
    // menu item's key equivalent, so an app with no menu has no Cmd-Q, and nobody thinks
    // to check because every other Mac app has one. Ten lines, and the window closes the
    // way a window is supposed to.
    {
        id bar = MSG(id)(MSG(id)(CLS_("NSMenu"), SEL_("alloc")), SEL_("init"));
        id barItem = MSG(id)(MSG(id)(CLS_("NSMenuItem"), SEL_("alloc")), SEL_("init"));
        MSG(void, id)(bar, SEL_("addItem:"), barItem);
        MSG(void, id)(app, SEL_("setMainMenu:"), bar);

        id menu = MSG(id)(MSG(id)(CLS_("NSMenu"), SEL_("alloc")), SEL_("init"));
        id title = MSG(id, const char *)(CLS_("NSString"), SEL_("stringWithUTF8String:"), "Quit cvertex");
        id key   = MSG(id, const char *)(CLS_("NSString"), SEL_("stringWithUTF8String:"), "q");
        id quit  = MSG(id, id, SEL, id)(MSG(id)(CLS_("NSMenuItem"), SEL_("alloc")),
                       SEL_("initWithTitle:action:keyEquivalent:"), title, SEL_("terminate:"), key);
        MSG(void, id)(menu, SEL_("addItem:"), quit);
        MSG(void, id)(barItem, SEL_("setSubmenu:"), menu);
    }

    Class vc = objc_allocateClassPair((Class)objc_getClass("NSView"), "FBView", 0);
    class_addMethod(vc, SEL_("drawRect:"), (IMP)fbview_draw, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    objc_registerClassPair(vc);

    CGRect r = { { 0, 0 }, { g_fbw * 2, g_fbh * 2 } };
    id win = MSG(id)(CLS_("NSWindow"), SEL_("alloc"));
    win = MSG(id, CGRect, unsigned long, unsigned long, BOOL)(win,
        SEL_("initWithContentRect:styleMask:backing:defer:"), r, 1 | 2 | 4, 2, NO);
    id title = MSG(id, const char *)(CLS_("NSString"), SEL_("stringWithUTF8String:"), g->name);
    MSG(void, id)(win, SEL_("setTitle:"), title);

    id view = MSG(id)((id)vc, SEL_("alloc"));
    view = MSG(id, CGRect)(view, SEL_("initWithFrame:"), r);
    MSG(void, id)(win, SEL_("setContentView:"), view);
    if (fullscreen) {
        // NSWindowCollectionBehaviorFullScreenPrimary, then ask for the toggle.
        MSG(void, unsigned long)(win, SEL_("setCollectionBehavior:"), 1UL << 7);
        MSG(void, id)(win, SEL_("toggleFullScreen:"), 0);
    }
    MSG(void, id)(win, SEL_("makeKeyAndOrderFront:"), 0);
    MSG(void, BOOL)(app, SEL_("activateIgnoringOtherApps:"), YES);
    MSG(void)(app, SEL_("finishLaunching"));

    id mode = MSG(id, const char *)(CLS_("NSString"), SEL_("stringWithUTF8String:"), "kCFRunLoopDefaultMode");

    synth_init();
    audio_start();
    // 🔴 No music_start() here. The synth is a capability; a song is content, and the
    // platform layer starting one meant the engine had an opinion about what you'd hear —
    // the same mistake as an engine that knows what a jump is. A game that wants music
    // starts it.

    // Fixed timestep. The sim always consumes a fixed dt, decoupled from wall-clock time → determinism.
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t step = 16666667ULL * tb.denom / tb.numer;  // 60Hz
    uint64_t next = mach_absolute_time();

    // Lockstep bookkeeping, only alive when networked. mysum is the state at the end of the
    // last frame, which is what we hand the peer at the start of the next exchange.
    long     net_frame = 0;
    uint64_t mysum = net_active() ? g->checksum() : 0;

    // FULLSCREEN as a live OPTIONS setting: g_fullscreen mirrors the window's current state, seeded
    // from the --fullscreen flag. When the panel flips it, the value diverges from fs_applied and we
    // ask the real NSWindow to toggle — a genuine runtime path on the window that already exists, no
    // rebuild. (The panel writes g_fullscreen; the platform, which owns the window, applies it here.)
    g_fullscreen = fullscreen;
    int fs_applied = fullscreen;

    while (g_running) {
        pump_events(app, mode);
        if (MSG(BOOL)(win, SEL_("isVisible")) == NO) break;
        if (g_fullscreen != fs_applied) {
            MSG(void, unsigned long)(win, SEL_("setCollectionBehavior:"), 1UL << 7);
            MSG(void, id)(win, SEL_("toggleFullScreen:"), 0);
            fs_applied = g_fullscreen;
        }

        Input in[2];
        if (net_active()) {
            // Read the LOCAL keyboard and merge WASD+arrows into this player's one Input, so
            // whichever half of the keyboard they reach for drives them. The exchange puts it
            // in slot [0] (host) or [1] (joiner) and fills the other slot from the peer.
            Input raw[2]; read_input(raw);
            Input local = input_1p(raw);
            if (net_step(&mysum, net_frame, local, in)) { net_close(); continue; }
        } else {
            read_input(in);
        }
        g->tick(in);
        // The two-stage Esc, routed here where the platform knows which cartridge is current.
        // The menu consumes g_esc inside its OWN tick (to start its CRT power-off), so if the
        // latch is still set the current cartridge is a real game that ignored it — swap back to
        // the console, flagged as a return so the menu plays its insert in reverse. mac.c only
        // routes and swaps; the menu owns both animations.
        if (g_esc) {
            if (g != &game_menu) { g_menu_return = 1; g_switch_to = &game_menu; }
            g_esc = 0;
        }
        if (g_quit) break;   // the menu's CRT power-off has finished collapsing the tube
        if (g_switch_to) {
            // Silence first, then hand over. Stopping the music is the platform's job —
            // otherwise whatever the last game was playing follows you into the next one,
            // and every game would have to remember to shut the previous one up. Asking for
            // music is the game's job, in init(), where a song is content like any other.
            //
            // Lockstep survives a game switch for free: both peers fed the SAME in[2] to the
            // menu, so both pick the same cartridge and re-init identically. Just re-baseline
            // the checksum to the fresh state.
            g = g_switch_to; g_switch_to = 0;
            music_play(0, 0, 0, 0);
            g3d_light(0, 0, 0);   // and back to the headlamp — a cartridge's light is content, like its song
            g_present_fx = 0;     // and eject the light chip — post-processing is content too, must not follow out
            // CO-OP chosen in the OPTIONS panel (g_coop: 1 HOST / 2 JOIN): stand up the lockstep net
            // now, as the cart plugs in — the menu-driven equivalent of the --host/--join flags, so a
            // player never has to touch the command line. HOST blocks here until a peer joins, exactly
            // like --host. 🔴 SEAM: JOIN has no on-screen IP entry yet, so it targets 127.0.0.1 — enough
            // for a two-window test on one machine; remote co-op still needs --join <ip>. On failure we
            // print and fall through to solo rather than dropping a dead socket into the loop.
            int was_active = net_active();
            if (g_coop && !was_active) {
                int ok = (g_coop == 1) ? net_host(NET_DEFAULT_PORT)
                                       : net_join("127.0.0.1", NET_DEFAULT_PORT);
                if (ok != 0) fprintf(stderr, "[net] menu co-op setup failed — staying solo.\n");
                g_coop = 0;   // consumed: a later Esc-back to the menu must not reconnect a live socket
            }
            g->init();
            memset(g_keys, 0, sizeof g_keys);
            // A net that was already live keeps counting frames; one just established starts at 0 so
            // both peers begin the fresh game on the same frame index.
            if (net_active()) { mysum = g->checksum(); net_frame = was_active ? net_frame + 1 : 0; }
            continue;
        }
        g->audio();
        g->draw();
        MSG(void, BOOL)(view, SEL_("setNeedsDisplay:"), YES);
        MSG(void)(view, SEL_("displayIfNeeded"));

        if (net_active()) { mysum = g->checksum(); net_frame++; }

        next += step;
        uint64_t now = mach_absolute_time();
        if (next > now) mach_wait_until(next); else next = now;
    }
    return 0;
}
