// The Java side of the Android app, natively: the classes and methods the runtime calls through JNI.
// Anything not defined here is logged by jni.c when called ("java: ... not implemented").
#import <Foundation/Foundation.h>
#include "jni.h"
#include "elfload.h"

#define GAME "com/gameloft/android/GAND/GloftSMHP/ML/"
static char g_data[1024];
static int g_videoDone = 1;

int java_video_completed(void) { return g_videoDone; }
void java_set_video_completed(int done) { g_videoDone = done; }

static JValue ret_i(int v) { JValue r = {0}; r.i = v; return r; }
static JValue ret_l(void *v) { JValue r = {0}; r.l = v; return r; }

static JValue Revived_dataPath(JObject *self, const JValue *a) { (void)self; (void)a; return ret_l(jni_string(g_data)); }
static JValue Revived_dataPath1(JObject *self, const JValue *a) {
    (void)self;
    char buf[1200];
    snprintf(buf, sizeof buf, "%s%s", g_data, jni_cstr(a[0].l) ?: "");
    return ret_l(jni_string(buf));
}
static JValue Revived_fixPath(JObject *self, const JValue *a) {
    (void)self;
    const char *p = jni_cstr(a[0].l);
    const char *old = "/sdcard/gameloft/games/GloftSMHP";
    if (p && !strncmp(p, old, strlen(old))) {
        char buf[1200];
        snprintf(buf, sizeof buf, "%s%s", g_data, p + strlen(old));
        return ret_l(jni_string(buf));
    }
    return ret_l(a[0].l);
}
static JValue zero(JObject *self, const JValue *a) { (void)self; (void)a; return ret_i(0); }
static JValue one(JObject *self, const JValue *a) { (void)self; (void)a; return ret_i(1); }
static JValue isVideoCompleted(JObject *self, const JValue *a) { (void)self; (void)a; return ret_i(g_videoDone); }

static JValue detectPhoneLang(JObject *self, const JValue *a) {
    (void)self; (void)a;
    // Gameloft order: 0 English, 1 French, 2 German, 3 Italian, 4 Spanish (others fall back to English)
    NSString *lang = NSLocale.preferredLanguages.firstObject ?: @"en";
    if ([lang hasPrefix:@"fr"]) return ret_i(1);
    if ([lang hasPrefix:@"de"]) return ret_i(2);
    if ([lang hasPrefix:@"it"]) return ret_i(3);
    if ([lang hasPrefix:@"es"]) return ret_i(4);
    return ret_i(0);
}

void java_setup(const char *dataDir) {
    strlcpy(g_data, dataDir, sizeof g_data);
    jni_define("com/redmemories/smhp/Revived", "dataPath", "()Ljava/lang/String;", true, Revived_dataPath);
    jni_define("com/redmemories/smhp/Revived", "dataPath", "(Ljava/lang/String;)Ljava/lang/String;", true, Revived_dataPath1);
    jni_define("com/redmemories/smhp/Revived", "fixPath", "(Ljava/lang/String;)Ljava/lang/String;", true, Revived_fixPath);
    jni_define(GAME "GLMediaPlayer", "isVideoCompleted", "()I", true, isVideoCompleted);
    jni_define(GAME "MyVideoView", "isVideoCompleted", "()I", true, isVideoCompleted);
    jni_define(GAME "GLMediaPlayer", "detectPhoneLang", "()I", true, detectPhoneLang);
    jni_define(GAME "GLMediaPlayer", "detectPhoneManufacturer", "()I", true, zero);
    jni_define(GAME "GLMediaPlayer", "isSilentMode", "()Z", true, zero);
    jni_define(GAME "Spiderman", "isWifiAlive", "()I", true, one);
    jni_define(GAME "Spiderman", "getDeviceName", "()I", true, zero);
    jni_define(GAME "GameRenderer", "getTypePhone", "()I", true, zero);
}
