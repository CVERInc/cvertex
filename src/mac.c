// mac.c — macOS 平台層。純 C，直接呼叫 objc runtime，不用 Objective-C 編譯器。
// 它只回答五個問題：給我視窗、給我一塊貼到螢幕的記憶體、鍵盤按了什麼、現在幾點、該收工了嗎。
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

#define SEL_(s) sel_registerName(s)
#define CLS_(s) ((id)objc_getClass(s))
#define MSG(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

static uint32_t g_rgba[FBW * FBH];
static uint8_t  g_keys[128];
static int      g_running;   // 見 synth.c 的 16KB 教訓：初值在執行期賦，別寫非零初始化
extern uint64_t g_checksum;

// framebuffer(調色盤索引) → RGBA，然後貼進視窗。整個「顯示」就這樣。
static void fbview_draw(id self, SEL _cmd, CGRect dirty) {
    (void)self; (void)_cmd; (void)dirty;
    for (int i = 0; i < FBW * FBH; i++) g_rgba[i] = g_pal[g_fb[i]];

    id nsctx = MSG(id)(CLS_("NSGraphicsContext"), SEL_("currentContext"));
    if (!nsctx) return;
    CGContextRef ctx = MSG(CGContextRef)(nsctx, SEL_("CGContext"));
    if (!ctx) return;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(0, g_rgba, sizeof(g_rgba), 0);
    CGImageRef img = CGImageCreate(FBW, FBH, 8, 32, FBW * 4, cs,
        kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, 0, 0, kCGRenderingIntentDefault);

    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);  // 像素要銳利
    CGRect box = CGContextGetClipBoundingBox(ctx);
    CGContextDrawImage(ctx, box, img);

    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

// 音訊：作業系統來要 sample，我們就算給它。沒有檔案，沒有解碼器。
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

// sim 吐事件 → 這裡翻譯成聲音。sim 不知道有音效這回事。
static void play_events(void) {
    if (g_events & (EV_JUMP_A | EV_JUMP_B)) synth_note(NCHAN - 1, 5, (g_events & EV_JUMP_A) ? 84 : 79, 180);
    if (g_events & (EV_LAND_A | EV_LAND_B)) synth_note(NCHAN - 1, 4, 48, 140);
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
        }
        MSG(void, id)(app, SEL_("sendEvent:"), ev);
    }
}

// 鍵盤 → 兩個角色的輸入。本地雙人合作在這裡就成立了：
// 角色 A 讀 A/D/W，角色 B 讀 ←/→/↑。sim 不知道輸入從哪來，也不在乎。
static void read_input(Input in[2]) {
    in[0] = (Input){ (int8_t)(g_keys[2] - g_keys[0]), 0, g_keys[13] };      // D-A, W
    in[1] = (Input){ (int8_t)(g_keys[124] - g_keys[123]), 0, g_keys[126] }; // →-←, ↑
}

int main(int argc, char **argv) {
    g_running = 1;
    sim_init();

    // --headless N：不開視窗，跑 N 幀，印出 checksum。用來驗確定性。
    if (argc > 2 && !strcmp(argv[1], "--headless")) {
        int n = atoi(argv[2]);
        for (int f = 0; f < n; f++) {
            Input in[2] = { { (f / 17) % 3 - 1, 0, (f % 23) == 0 },
                            { (f / 11) % 3 - 1, 0, (f % 31) == 0 } };
            sim_tick(in);
        }
        sim_draw();
        uint64_t ink = 0;
        for (int i = 0; i < FBW * FBH; i++) ink = ink * 3 + g_fb[i];
        printf("frames=%d sim_checksum=%llu fb_checksum=%llu\n", n, g_checksum, ink);
        return 0;
    }

    id app = MSG(id)(CLS_("NSApplication"), SEL_("sharedApplication"));
    MSG(void, long)(app, SEL_("setActivationPolicy:"), 0);

    Class vc = objc_allocateClassPair((Class)objc_getClass("NSView"), "FBView", 0);
    class_addMethod(vc, SEL_("drawRect:"), (IMP)fbview_draw, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    objc_registerClassPair(vc);

    CGRect r = { { 0, 0 }, { FBW * 3, FBH * 3 } };
    id win = MSG(id)(CLS_("NSWindow"), SEL_("alloc"));
    win = MSG(id, CGRect, unsigned long, unsigned long, BOOL)(win,
        SEL_("initWithContentRect:styleMask:backing:defer:"), r, 1 | 2 | 4, 2, NO);
    id title = MSG(id, const char *)(CLS_("NSString"), SEL_("stringWithUTF8String:"), "spike");
    MSG(void, id)(win, SEL_("setTitle:"), title);

    id view = MSG(id)((id)vc, SEL_("alloc"));
    view = MSG(id, CGRect)(view, SEL_("initWithFrame:"), r);
    MSG(void, id)(win, SEL_("setContentView:"), view);
    MSG(void, id)(win, SEL_("makeKeyAndOrderFront:"), 0);
    MSG(void, BOOL)(app, SEL_("activateIgnoringOtherApps:"), YES);
    MSG(void)(app, SEL_("finishLaunching"));

    id mode = MSG(id, const char *)(CLS_("NSString"), SEL_("stringWithUTF8String:"), "kCFRunLoopDefaultMode");

    synth_init();
    audio_start();
    music_start();

    // 固定時間步長。sim 永遠吃固定的 dt，跟真實時間解耦 → 確定性。
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    const uint64_t step = 16666667ULL * tb.denom / tb.numer;  // 60Hz
    uint64_t next = mach_absolute_time();

    while (g_running) {
        pump_events(app, mode);
        if (MSG(BOOL)(win, SEL_("isVisible")) == NO) break;

        Input in[2]; read_input(in);
        sim_tick(in);
        play_events();
        sim_draw();
        MSG(void, BOOL)(view, SEL_("setNeedsDisplay:"), YES);
        MSG(void)(view, SEL_("displayIfNeeded"));

        next += step;
        uint64_t now = mach_absolute_time();
        if (next > now) mach_wait_until(next); else next = now;
    }
    return 0;
}
