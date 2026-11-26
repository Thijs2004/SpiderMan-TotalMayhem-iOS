// Game controller and tilt, polled once per frame on the game thread. The controller follows the revival's
// Pad class: A jump, X punch, Y web, B spider-sense (back in menus), R1/R2 super, L1/L2 action, left stick or
// d-pad moves (d-pad wins when pressed), Menu/Options = the Android back key (pause). Tilt goes to
// Spiderman.nativeAccelerator in Android's units and axes.
#import <GameController/GameController.h>
#import <CoreMotion/CoreMotion.h>
#include "jni.h"
#include "elfload.h"
#import "settings.h"

#define GAME "com/gameloft/android/GAND/GloftSMHP/ML/"
enum { JUMP = 0, PUNCH = 1, WEB = 2, SENSE = 3, SUPER = 4, ACTION = 5 };

typedef void (*ButtonFn)(void *, void *, int, uint8_t);
typedef void (*StickFn)(void *, void *, float, float);
typedef uint8_t (*HudFn)(void *, void *);
typedef void (*KeyFn)(void *, void *, int);
typedef void (*AccelFn)(void *, void *, float, float, float);

static CMMotionManager *g_motion;
static BOOL g_down[6], g_menu, g_bBack;
static float g_lastX = 2, g_lastY = 2;
static int g_pendingKeyUp = -1;

static void *nat(const char *cls, const char *name) { return jni_native(cls, name); }

static void press(int b, BOOL down) {
    if (g_down[b] == down) return;
    g_down[b] = down;
    ButtonFn f = nat("com/redmemories/smhp/Pad", "nativeButton");
    if (f) f(jni_env(), jni_class("com/redmemories/smhp/Pad"), b, down);
}
static void key(int code, BOOL down) {
    KeyFn f = nat(GAME "Spiderman", down ? "nativeKeyDown" : "nativeKeyUp");
    if (f) f(jni_env(), jni_class(GAME "Spiderman"), code);
}

void input_frame(void) {
    void *env = jni_env();
    if (g_pendingKeyUp >= 0) { key(g_pendingKeyUp, NO); g_pendingKeyUp = -1; }

    if (setting_tilt()) {
        if (!g_motion) {
            g_motion = [CMMotionManager new];
            g_motion.accelerometerUpdateInterval = 1.0 / 60;
            [g_motion startAccelerometerUpdates];
        }
        CMAccelerometerData *a = g_motion.accelerometerData;
        AccelFn f = nat(GAME "Spiderman", "nativeAccelerator");
        // Android reports m/s^2 with the opposite sign of iOS's g values (device axes are the same).
        if (a && f) f(env, jni_class(GAME "Spiderman"), (float)(-a.acceleration.x * 9.81), (float)(-a.acceleration.y * 9.81), (float)(-a.acceleration.z * 9.81));
    } else if (g_motion) {
        [g_motion stopAccelerometerUpdates];
        g_motion = nil;
    }

    GCExtendedGamepad *pad = setting_controller() ? GCController.current.extendedGamepad : nil;
    if (!pad) {
        for (int b = 0; b < 6; b++) press(b, NO);
        if (g_lastX != 0 || g_lastY != 0) {
            StickFn s = nat("com/redmemories/smhp/Pad", "nativeStick");
            if (s && g_lastX != 2) s(env, jni_class("com/redmemories/smhp/Pad"), 0, 0);
            g_lastX = g_lastY = 0;
        }
        return;
    }
    float x = pad.leftThumbstick.xAxis.value, y = -pad.leftThumbstick.yAxis.value;   // Android: y grows downwards
    if (fabsf(x) < 0.15f) x = 0;
    if (fabsf(y) < 0.15f) y = 0;
    float hx = pad.dpad.xAxis.value, hy = -pad.dpad.yAxis.value;
    if (fabsf(hx) > 0.5f || fabsf(hy) > 0.5f) { x = hx; y = hy; }
    if (x != g_lastX || y != g_lastY) {
        StickFn s = nat("com/redmemories/smhp/Pad", "nativeStick");
        if (s) s(env, jni_class("com/redmemories/smhp/Pad"), x, y);
        g_lastX = x; g_lastY = y;
    }
    press(JUMP, pad.buttonA.pressed);
    press(PUNCH, pad.buttonX.pressed);
    press(WEB, pad.buttonY.pressed);
    press(SUPER, pad.rightShoulder.pressed || pad.rightTrigger.value > 0.5f);
    press(ACTION, pad.leftShoulder.pressed || pad.leftTrigger.value > 0.5f);
    // B: spider-sense in game, back in menus (when the HUD is hidden)
    BOOL b = pad.buttonB.pressed;
    if (b && !g_bBack && !g_down[SENSE]) {
        HudFn hud = nat("com/redmemories/smhp/Pad", "nativeHudVisible");
        if (hud && !hud(env, jni_class("com/redmemories/smhp/Pad"))) { g_bBack = YES; key(4, YES); g_pendingKeyUp = 4; }
        else press(SENSE, YES);
    } else if (!b) {
        g_bBack = NO;
        press(SENSE, NO);
    }
    BOOL menu = pad.buttonMenu.pressed || pad.buttonOptions.pressed;
    if (menu && !g_menu) { key(4, YES); g_pendingKeyUp = 4; }
    g_menu = menu;
}
