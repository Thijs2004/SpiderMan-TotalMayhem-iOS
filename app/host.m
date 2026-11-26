// Spider-Man: Total Mayhem on iOS: boots the Android runtime (libspiderman.so + recompiled engine) and plays
// the part of the Android activity / GLSurfaceView / renderer (see the decompiled Spiderman, GameRenderer
// and GameGLSurfaceView classes).
#import <UIKit/UIKit.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES1/gl.h>
#import <OpenGLES/ES1/glext.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#include "elfload.h"
#include "bionic.h"
#include "jni.h"
#import "settings.h"

extern char elf_host_base[], elf_aot_base[];   // the baked ELF images (gen/host.s, gen/aot.s)
extern int smtm_surface_width, smtm_surface_height;
void java_setup(const char *dataDir);
void arm32_install(uintptr_t hostBase);
void sound_setup(const char *dataDir, int bgmCount);
int java_video_completed(void);
void java_set_video_completed(int done);
void input_frame(void);

static char g_dataDir[1024], g_docs[1024], g_lib[1024], g_libdir[1024];

// ---------------------------------------------------------------- paths and properties
const char *bionic_path(const char *p) {
    static __thread char buf[1024];
    if (!p) return p;
    static const struct { const char *from; int which; } maps[] = {
        {"/sdcard/Android/data/com.redmemories.spidermantotalmayhem/files/SpiderManTotalMayhemRevived", 0},
        {"/sdcard/gameloft/games/GloftSMHP", 0},
        {"/sdcard/Android/data/com.redmemories.spidermantotalmayhem/files", 1},
        {"/data/data/com.redmemories.spidermantotalmayhem", 2},
        {"/data/user/0/com.redmemories.spidermantotalmayhem", 2},
        {"/data/data/com.gameloft.android.GAND.GloftSMHP.ML", 2},
        {"/data/app/smtm/lib/arm64", 3},
    };
    for (size_t i = 0; i < sizeof maps / sizeof maps[0]; i++) {
        size_t n = strlen(maps[i].from);
        if (!strncmp(p, maps[i].from, n) && (p[n] == 0 || p[n] == '/')) {
            const char *base = maps[i].which == 0 ? g_dataDir : maps[i].which == 1 ? g_docs : maps[i].which == 2 ? g_lib : g_libdir;
            snprintf(buf, sizeof buf, "%s%s", base, p + n);
            return buf;
        }
    }
    return p;
}
const char *bionic_property(const char *name) {
    const char *env = getenv("SMTM_PROPS");   // e.g. "debug.smhp.aotcheck=1;..."
    if (env) {
        const char *p = strstr(env, name);
        if (p && p[strlen(name)] == '=') {
            static char v[92];
            const char *s = p + strlen(name) + 1, *e = strchr(s, ';');
            size_t n = e ? (size_t)(e - s) : strlen(s);
            if (n > 91) n = 91;
            memcpy(v, s, n);
            v[n] = 0;
            return v;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- natives
typedef void (*V0)(void *, void *);
typedef void (*VI)(void *, void *, int);
typedef void (*VII)(void *, void *, int, int);
typedef void (*VIIII)(void *, void *, int, int, int, int);
typedef void (*VSSSS)(void *, void *, void *, void *, void *, void *);
typedef int (*I0)(void *, void *);

static void *native(const char *cls, const char *name) {
    void *p = jni_native(cls, name);
    if (!p) elf_log("native %s.%s missing", cls, name);
    return p;
}
#define GAME "com/gameloft/android/GAND/GloftSMHP/ML/"

static void prepare_data(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    NSString *lib = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES)[0];
    strlcpy(g_docs, docs.fileSystemRepresentation, sizeof g_docs);
    strlcpy(g_lib, lib.fileSystemRepresentation, sizeof g_lib);
    NSString *data = [docs stringByAppendingPathComponent:@"SpiderManTotalMayhemRevived"];
    const char *dev = getenv("SMTM_DATA");   // simulator: use the extracted data on the Mac directly
    if (dev) data = @(dev);
    else if (![fm fileExistsAtPath:[data stringByAppendingPathComponent:@"configs.pack"]]) {
        // First launch: clone the bundled data (APFS clone, no extra space).
        NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"SpiderManTotalMayhemRevived"];
        [fm createDirectoryAtPath:data withIntermediateDirectories:YES attributes:nil error:nil];
        for (NSString *f in [fm contentsOfDirectoryAtPath:bundled error:nil]) {
            NSError *err;
            if (![fm copyItemAtPath:[bundled stringByAppendingPathComponent:f] toPath:[data stringByAppendingPathComponent:f] error:&err])
                elf_log("copy %s failed: %s", f.UTF8String, err.localizedDescription.UTF8String);
        }
    }
    strlcpy(g_dataDir, data.fileSystemRepresentation, sizeof g_dataDir);
    // The first builds had no sound player and reported volume 0, which the game saved into settings.dat:
    // drop that file once so the options start at their defaults again (progress is in other files).
    if (![NSUserDefaults.standardUserDefaults boolForKey:@"SoundSettingsReset1"]) {
        [fm removeItemAtPath:[data stringByAppendingPathComponent:@"settings.dat"] error:nil];
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"SoundSettingsReset1"];
    }
    strlcpy(g_libdir, [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"lib"].fileSystemRepresentation, sizeof g_libdir);
    elf_log("data %s", g_dataDir);
}

static ElfImage *g_host;
static void boot_runtime(void) {
    // The revival's settings, as Settings.apply passes them to the engine (0 fps = the screen's rate).
    char v[16];
    snprintf(v, sizeof v, "%d", setting_fps() == 120 ? 0 : setting_fps());
    setenv("DEBUG_SMHP_MAXFPS", v, 1);
    setenv("DEBUG_SMHP_RES", setting_screen().UTF8String, 1);
    snprintf(v, sizeof v, "%.2f", setting_resolution() / 100.0);
    setenv("DEBUG_SMHP_SCALE", v, 1);
    prepare_data();
    ElfImage *aot = elf_register("libspidermanaot.so", elf_aot_base);
    g_host = elf_register("libspiderman.so", elf_host_base);
    elf_link(aot, bionic_resolve);
    elf_link(g_host, bionic_resolve);
    arm32_install(elf_base(g_host));   // dynarmic's JIT entry -> our ARMv5 interpreter
    elf_run_init(aot);
    elf_run_init(g_host);
    java_setup(g_dataDir);
    sound_setup(g_dataDir, 24);
    int (*onload)(void *, void *) = elf_sym(g_host, "JNI_OnLoad");
    elf_log("JNI_OnLoad -> %#x", onload(jni_vm(), NULL));
}

// ---------------------------------------------------------------- GL view
@interface GameView : UIView
@end
@implementation GameView
+ (Class)layerClass { return CAMetalLayer.class; }   // GL frames are shown through Metal (present.m)
@end

@interface GameVC : UIViewController
- (void)setActive:(BOOL)active;
@end
void pause_audio(int paused);
int present_init(CAMetalLayer *layer, EAGLContext *ctx, int w, int h);
void present_begin(void);
void present_end(void);
@implementation GameVC {
    EAGLContext *_ctx;
    GLuint _fb, _color, _depth;
    GLint _w, _h;
    CADisplayLink *_link;
    BOOL _started;
    UILabel *_fpsLabel;
    int _frames;
    CFTimeInterval _fpsSince;
    AVPlayer *_video;
    AVPlayerLayer *_videoLayer;
    NSMutableArray<UITouch *> *_touches;
}

- (void)loadView {
    GameView *v = [GameView new];
    v.multipleTouchEnabled = YES;
    v.contentScaleFactor = UIScreen.mainScreen.nativeScale * setting_resolution() / 100.0;   // Resolution setting
    v.layer.opaque = YES;
    self.view = v;
    _touches = [NSMutableArray array];
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    if (!_ctx && self.view.bounds.size.width > self.view.bounds.size.height) [self setupGL];
}

- (void)setupGL {
    _ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES1];
    [EAGLContext setCurrentContext:_ctx];
    CGFloat sc = self.view.contentScaleFactor;
    _w = (GLint)lround(self.view.bounds.size.width * sc) & ~1;
    _h = (GLint)lround(self.view.bounds.size.height * sc) & ~1;
    if (present_init((CAMetalLayer *)self.view.layer, _ctx, _w, _h)) elf_log("FATAL: presenter setup failed");
    smtm_surface_width = _w;
    smtm_surface_height = _h;
    elf_log("GL ES1 surface %dx%d, screen up to %ld Hz, low power %d", _w, _h, (long)UIScreen.mainScreen.maximumFramesPerSecond,
            NSProcessInfo.processInfo.isLowPowerModeEnabled);
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(frame)];
    int fps = setting_fps();
    _link.preferredFrameRateRange = fps == 120 ? CAFrameRateRangeMake(80, 120, 120) : CAFrameRateRangeMake(fps, fps, fps);
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

// GameRenderer.onSurfaceCreated
- (void)surfaceCreated {
    void *env = jni_env();
    void *renderer = jni_object(jni_class(GAME "GameRenderer"));
    ((V0)native(GAME "GameRenderer", "nativeRendererGetJNIEnv"))(env, renderer);
    ((V0)native(GAME "Spiderman", "nativeGameInit"))(env, jni_class(GAME "Spiderman"));
    // GLMediaPlayer.init: nativeInit and the sound counts
    void *mp = jni_class(GAME "GLMediaPlayer");
    ((V0)native(GAME "GLMediaPlayer", "nativeInit"))(env, mp);
    elf_log("sounds: total %d, bgm %d, sfx %d, vfx %d",
            ((I0)native(GAME "GLMediaPlayer", "nativeGetTotalSounds"))(env, mp),
            ((I0)native(GAME "GLMediaPlayer", "nativeGetBGMCount"))(env, mp),
            ((I0)native(GAME "GLMediaPlayer", "nativeGetSFXCount"))(env, mp),
            ((I0)native(GAME "GLMediaPlayer", "nativeGetVFXCount"))(env, mp));
    ((VII)native(GAME "GameRenderer", "nativeResize"))(env, jni_class(GAME "GameRenderer"), _w, _h);
    ((VII)native(GAME "GameRenderer", "nativeRendererInit"))(env, jni_class(GAME "GameRenderer"), 1, 0);
    elf_log("renderer initialised");
    [self playLogo];
}

- (void)frame {
    [EAGLContext setCurrentContext:_ctx];
    CFTimeInterval t0 = CACurrentMediaTime();
    present_begin();
    if (!_started) {
        _started = YES;
        boot_runtime();
        // Spiderman.onCreate -> nativeGetInfo, onStart
        void *env = jni_env();
        ((VSSSS)native(GAME "Spiderman", "nativeGetInfo"))(env, jni_class(GAME "Spiderman"), jni_string("US"),
                                                             jni_string("000000000000000"), jni_string("samsung_SM-S948B"), jni_string("17"));
        [self surfaceCreated];
        return;
    }
    input_frame();
    if (java_video_completed()) ((V0)native(GAME "GameRenderer", "nativeUpdateAndRender"))(jni_env(), jni_class(GAME "GameRenderer"));
    CFTimeInterval t1 = CACurrentMediaTime();
    present_end();
    CFTimeInterval t2 = CACurrentMediaTime();
    [self scriptedTaps];
    [self countFrame];
    [self timeGame:t1 - t0 present:t2 - t1];
}

- (void)setActive:(BOOL)active {
    if (!_started) return;
    void *env = jni_env();
    _link.paused = !active;
    if (_video) { if (active) [_video play]; else [_video pause]; }
    if (!active) {
        JValue none[1] = {{0}};
        JMethod *m = jni_method(jni_class(GAME "GLMediaPlayer"), "pauseAll", "()V", true);
        if (m->impl) m->impl(NULL, none);
        ((VI)native(GAME "GameGLSurfaceView", "nativePause"))(env, jni_class(GAME "GameGLSurfaceView"), 0);
        pause_audio(1);
    } else {
        pause_audio(0);
        ((VI)native(GAME "GameGLSurfaceView", "nativeResume"))(env, jni_class(GAME "GameGLSurfaceView"), 0);
    }
}

// GLMediaPlayer.loadMovie(logo.mp4) / MyVideoView: the Gameloft logo before the game, tap to skip.
- (void)playLogo {
    NSString *path = [@(bionic_path("/sdcard/gameloft/games/GloftSMHP")) stringByAppendingPathComponent:@"logo.mp4"];
    if (getenv("SMTM_TAPS") || ![NSFileManager.defaultManager fileExistsAtPath:path]) return;   // tests skip it
    java_set_video_completed(0);
    _video = [AVPlayer playerWithURL:[NSURL fileURLWithPath:path]];
    _videoLayer = [AVPlayerLayer playerLayerWithPlayer:_video];
    _videoLayer.frame = self.view.bounds;
    _videoLayer.videoGravity = AVLayerVideoGravityResizeAspect;
    _videoLayer.backgroundColor = UIColor.blackColor.CGColor;
    [self.view.layer addSublayer:_videoLayer];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(logoDone) name:AVPlayerItemDidPlayToEndTimeNotification object:_video.currentItem];
    [_video play];
}
- (void)logoDone {
    if (!_video) return;
    [_video pause];
    [_videoLayer removeFromSuperlayer];
    _video = nil;
    _videoLayer = nil;
    java_set_video_completed(1);
}

// Every 5 s: real refresh rate, and how long the game and the presenting take per frame.
- (void)timeGame:(double)game present:(double)pres {
    static double sumG, sumP, maxG, since;
    static int n;
    sumG += game; sumP += pres; n++;
    if (game > maxG) maxG = game;
    double now = CACurrentMediaTime();
    if (!since) since = now;
    if (now - since >= 5) {
        elf_log("frames: %.1f/s, game %.2f ms avg (max %.1f), present %.2f ms avg, link %.0f-%.0f Hz",
                n / (now - since), sumG / n * 1000, maxG * 1000, sumP / n * 1000,
                _link.preferredFrameRateRange.minimum, _link.preferredFrameRateRange.maximum);
        sumG = sumP = maxG = 0; n = 0; since = now;
    }
}

- (void)countFrame {
    if (!setting_fps_counter()) { if (_fpsLabel) { [_fpsLabel removeFromSuperview]; _fpsLabel = nil; } return; }
    if (!_fpsLabel) {
        _fpsLabel = [UILabel new];
        _fpsLabel.font = [UIFont monospacedDigitSystemFontOfSize:14 weight:UIFontWeightBold];
        _fpsLabel.textColor = UIColor.greenColor;
        _fpsLabel.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
        _fpsLabel.textAlignment = NSTextAlignmentCenter;
        _fpsLabel.layer.cornerRadius = 6;
        _fpsLabel.clipsToBounds = YES;
        UIEdgeInsets in = self.view.safeAreaInsets;
        _fpsLabel.frame = CGRectMake(MAX(in.left, 12) + 4, 8, 70, 24);
        _fpsLabel.text = @"-- fps";
        [self.view addSubview:_fpsLabel];
        _fpsSince = CACurrentMediaTime();
        _frames = 0;
    }
    _frames++;
    CFTimeInterval now = CACurrentMediaTime();
    if (now - _fpsSince >= 0.5) {
        _fpsLabel.text = [NSString stringWithFormat:@"%.0f fps", _frames / (now - _fpsSince)];
        _frames = 0;
        _fpsSince = now;
    }
}

// Testing in the simulator (no touch screen there): SMTM_TAPS="frame:x,y;frame:x,y" taps at fractions of the
// screen (0..1) at the given frame numbers.
- (void)scriptedTaps {
    static long frame;
    static const char *taps;
    frame++;
    if (!taps) taps = getenv("SMTM_TAPS") ?: "";
    for (const char *p = taps; *p;) {
        long f = strtol(p, (char **)&p, 10);
        if (*p != ':') break;
        double x = strtod(p + 1, (char **)&p);
        if (*p != ',') break;
        double y = strtod(p + 1, (char **)&p);
        if (f == frame || f + 3 == frame) {
            VIIII t = (VIIII)native(GAME "GameGLSurfaceView", "nativeOnTouch");
            t(jni_env(), jni_class(GAME "GameGLSurfaceView"), f == frame ? 1 : 0, (int)(x * _w), (int)(y * _h), 0);
            elf_log("test tap %s at %.2f,%.2f (frame %ld)", f == frame ? "down" : "up", x, y, frame);
        }
        if (*p == ';') p++;
    }
}

// GameGLSurfaceView.processTouch: action 1 = down, 0 = up, 2 = move; pointer ids stay stable per finger.
- (void)touch:(NSSet<UITouch *> *)touches action:(int)action {
    if (!_started) return;
    if (_video) { if (action == 1) [self logoDone]; return; }   // tap skips the logo
    VIIII f = (VIIII)native(GAME "GameGLSurfaceView", "nativeOnTouch");
    CGFloat s = self.view.contentScaleFactor;
    for (UITouch *t in touches) {
        NSUInteger idx = [_touches indexOfObject:t];
        if (idx == NSNotFound) {
            idx = [_touches indexOfObject:(id)NSNull.null];
            if (idx == NSNotFound) { [_touches addObject:t]; idx = _touches.count - 1; }
            else _touches[idx] = t;
        }
        CGPoint p = [t locationInView:self.view];
        f(jni_env(), jni_class(GAME "GameGLSurfaceView"), action, (int)(p.x * s), (int)(p.y * s), (int)idx);
        if (action == 0) _touches[idx] = (id)NSNull.null;
    }
}
- (void)touchesBegan:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self touch:t action:1]; }
- (void)touchesMoved:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self touch:t action:2]; }
- (void)touchesEnded:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self touch:t action:0]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e { [self touch:t action:0]; }
@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic) UIWindow *window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)opts {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    if ([opts.shortcutItem.type isEqual:@"settings"] || getenv("SMTM_SHOW_SETTINGS")) {
        // Long-press on the icon -> Settings: choose first, then the game starts with them.
        SettingsVC *s = [SettingsVC new];
        __weak SceneDelegate *me = self;
        s.onDone = ^{ me.window.rootViewController = [GameVC new]; };
        self.window.rootViewController = s;
    } else {
        self.window.rootViewController = [GameVC new];
    }
    [self.window makeKeyAndVisible];
}
- (void)windowScene:(UIWindowScene *)scene performActionForShortcutItem:(UIApplicationShortcutItem *)item completionHandler:(void (^)(BOOL))done {
    UIViewController *root = self.window.rootViewController;
    if ([item.type isEqual:@"settings"] && [root isKindOfClass:GameVC.class] && !root.presentedViewController) {
        SettingsVC *s = [SettingsVC new];
        s.gameRunning = YES;
        s.modalPresentationStyle = UIModalPresentationFullScreen;
        [(GameVC *)root setActive:NO];
        __weak UIViewController *r = root;
        s.onDone = ^{ [r dismissViewControllerAnimated:YES completion:^{ [(GameVC *)r setActive:YES]; }]; };
        [root presentViewController:s animated:YES completion:nil];
    }
    done(YES);
}
// GameGLSurfaceView.onWindowFocusChanged / onPause: the game pauses itself and its sounds.
- (void)sceneWillResignActive:(UIScene *)scene {
    UIViewController *r = self.window.rootViewController;
    if ([r isKindOfClass:GameVC.class]) [(GameVC *)r setActive:NO];
}
- (void)sceneDidBecomeActive:(UIScene *)scene {
    UIViewController *r = self.window.rootViewController;
    if ([r isKindOfClass:GameVC.class] && !r.presentedViewController) [(GameVC *)r setActive:YES];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)opts {
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil];   // follow media volume, not the mute switch
    [AVAudioSession.sharedInstance setActive:YES error:nil];
    return YES;
}
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Game" sessionRole:s.role];
    c.delegateClass = SceneDelegate.class;
    return c;
}
- (UIInterfaceOrientationMask)application:(UIApplication *)a supportedInterfaceOrientationsForWindow:(UIWindow *)w {
    return UIInterfaceOrientationMaskLandscape;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(AppDelegate.class)); }
}
