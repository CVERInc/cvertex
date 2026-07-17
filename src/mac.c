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
#include "synth.h"
#include "game.h"

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
    for (;;) {
        id ev = MSG(id, unsigned long long, id, id, BOOL)(app,
            SEL_("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0ULL, past, mode, YES);
        if (!ev) break;
        unsigned long t = MSG(unsigned long)(ev, SEL_("type"));
        if (t == 10 || t == 11) {  // NSEventTypeKeyDown / KeyUp
            unsigned short kc = MSG(unsigned short)(ev, SEL_("keyCode"));
            if (kc < 128) g_keys[kc] = (t == 10);
            if (kc == 53) g_running = 0;  // Esc

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

// Keyboard → input for two characters. Local co-op is established right here:
// character A reads A/D/W, character B reads ←/→/↑. The sim doesn't know or care where input comes from.
static void read_input(Input in[2]) {
    in[0] = (Input){ (int8_t)(g_keys[2] - g_keys[0]), 0, g_keys[13] };      // D-A, W
    in[1] = (Input){ (int8_t)(g_keys[124] - g_keys[123]), 0, g_keys[126] }; // →-←, ↑
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
    const Game *g = &game_vikings;
    static const Game *const games[] = { &game_vikings, &game_title };
    #define NGAMES (int)(sizeof games / sizeof games[0])
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--res") && a + 2 < argc) { rw = atoi(argv[a+1]); rh = atoi(argv[a+2]); a += 2; }
        else if (!strcmp(argv[a], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[a], "--game") && a + 1 < argc) {
            g = 0;
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
            printf("  --game <name>     which game to run (default: %s)\n", game_vikings.name);
            printf("                    available:");
            for (int k = 0; k < NGAMES; k++) printf(" %s", games[k]->name);
            printf("\n");
            printf("  --res <w> <h>     framebuffer size (default: 640 360)\n");
            printf("  --fullscreen\n\n");
            printf("  --headless <n>    run n frames, print checksums, no window\n");
            printf("  --dump <n>        run n frames, print the framebuffer as ASCII\n");
            printf("  --ppm <n>         run n frames, write the framebuffer to stdout as a PPM\n\n");
            printf("  vikings: A/D/W and the arrow keys drive one character each. Esc quits.\n");
            return 0;
        }
        else if (argv[a][0] == 0x2D && a + 1 < argc) { runmode = argv[a]; modearg = atoi(argv[a+1]); a++; }
    }
    fb_resize(rw, rh);
    g->init();

    // --headless N: no window, run N frames, print the checksum. Used to verify determinism.
    if (runmode && !strcmp(runmode, "--headless")) {
        int n = modearg;
        for (int f = 0; f < n; f++) {
            Input in[2] = { { (f / 17) % 3 - 1, 0, (f % 23) == 0 },
                            { (f / 11) % 3 - 1, 0, (f % 31) == 0 } };
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
        Input in[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
        for (int f = 0; f < n; f++) g->tick(in);
        g->draw();
        printf("P6\n%d %d\n255\n", g_fbw, g_fbh);
        for (int i = 0; i < g_fbw * g_fbh; i++) {
            uint32_t c = g_pal[g_fb[i]];
            putchar((c >> 16) & 255); putchar((c >> 8) & 255); putchar(c & 255);
        }
        return 0;
    }

    // --dump N: run N frames, print the framebuffer as ASCII. A developer's eyes only, doesn't ship in a release build.
    if (runmode && !strcmp(runmode, "--dump")) {
        int n = modearg;
        Input in[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
        for (int f = 0; f < n; f++) g->tick(in);
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
    music_start();

    // Fixed timestep. The sim always consumes a fixed dt, decoupled from wall-clock time → determinism.
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t step = 16666667ULL * tb.denom / tb.numer;  // 60Hz
    uint64_t next = mach_absolute_time();

    while (g_running) {
        pump_events(app, mode);
        if (MSG(BOOL)(win, SEL_("isVisible")) == NO) break;

        Input in[2]; read_input(in);
        g->tick(in);
        g->audio();
        g->draw();
        MSG(void, BOOL)(view, SEL_("setNeedsDisplay:"), YES);
        MSG(void)(view, SEL_("displayIfNeeded"));

        next += step;
        uint64_t now = mach_absolute_time();
        if (next > now) mach_wait_until(next); else next = now;
    }
    return 0;
}
