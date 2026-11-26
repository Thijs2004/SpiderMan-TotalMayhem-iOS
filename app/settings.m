// Settings (the revival's Settings + SettingsActivity on Android): frame rate, resolution, screen mode, FPS
// counter, controller, tilt. Opened from the home-screen icon (long-press -> Settings) or from inside the game.
#import <UIKit/UIKit.h>
#import "settings.h"

static NSUserDefaults *D(void) { return NSUserDefaults.standardUserDefaults; }

int setting_fps(void) { NSInteger v = [D() integerForKey:@"fps"]; return v == 30 || v == 120 ? (int)v : 60; }
int setting_resolution(void) { NSInteger v = [D() integerForKey:@"resolution"]; return v == 75 || v == 50 ? (int)v : 100; }
NSString *setting_screen(void) { NSString *s = [D() stringForKey:@"screen"]; return [@[@"fit", @"stretch"] containsObject:s] ? s : @"wide"; }
BOOL setting_fps_counter(void) { return [D() boolForKey:@"fpsCounter"]; }
BOOL setting_controller(void) { return [D() objectForKey:@"controller"] ? [D() boolForKey:@"controller"] : YES; }
BOOL setting_tilt(void) { return [D() objectForKey:@"tilt"] ? [D() boolForKey:@"tilt"] : YES; }

@implementation SettingsVC {
    UISegmentedControl *_fps, *_res, *_screen;
    UISwitch *_counter, *_controller, *_tilt;
    int _startFps, _startRes;
    NSString *_startScreen;
}

- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
- (BOOL)prefersStatusBarHidden { return YES; }

static UILabel *label(NSString *t, CGFloat size, UIFontWeight w) {
    UILabel *l = [UILabel new];
    l.text = t;
    l.font = [UIFont systemFontOfSize:size weight:w];
    l.textColor = UIColor.whiteColor;
    l.numberOfLines = 0;
    return l;
}

- (UIView *)row:(NSString *)title detail:(NSString *)detail control:(UIView *)control {
    UIStackView *texts = [[UIStackView alloc] initWithArrangedSubviews:@[label(title, 17, UIFontWeightSemibold)]];
    texts.axis = UILayoutConstraintAxisVertical;
    if (detail) {
        UILabel *d = label(detail, 13, UIFontWeightRegular);
        d.textColor = [UIColor colorWithWhite:1 alpha:0.6];
        [texts addArrangedSubview:d];
    }
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[texts, control]];
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 16;
    [control setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [control setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    return row;
}

static UISegmentedControl *segments(NSArray *items, NSUInteger selected) {
    UISegmentedControl *s = [[UISegmentedControl alloc] initWithItems:items];
    s.selectedSegmentIndex = selected;
    s.selectedSegmentTintColor = [UIColor colorWithRed:0.85 green:0.1 blue:0.12 alpha:1];
    [s setTitleTextAttributes:@{NSForegroundColorAttributeName : UIColor.whiteColor} forState:UIControlStateNormal];
    return s;
}
static UISwitch *toggle(BOOL on) {
    UISwitch *s = [UISwitch new];
    s.on = on;
    s.onTintColor = [UIColor colorWithRed:0.85 green:0.1 blue:0.12 alpha:1];
    return s;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.07 green:0.07 blue:0.09 alpha:1];
    _startFps = setting_fps();
    _startRes = setting_resolution();
    _startScreen = setting_screen();
    _fps = segments(@[@"30", @"60", @"120"], _startFps == 30 ? 0 : _startFps == 120 ? 2 : 1);
    _res = segments(@[@"100%", @"75%", @"50%"], _startRes == 75 ? 1 : _startRes == 50 ? 2 : 0);
    _screen = segments(@[@"Wide", @"Original", @"Stretched"], [_startScreen isEqual:@"fit"] ? 1 : [_startScreen isEqual:@"stretch"] ? 2 : 0);
    _counter = toggle(setting_fps_counter());
    _controller = toggle(setting_controller());
    _tilt = toggle(setting_tilt());

    UILabel *title = label(@"Settings", 28, UIFontWeightBold);
    NSArray *rows = @[
        title,
        [self row:@"Frame rate" detail:@"120 uses the full ProMotion refresh rate" control:_fps],
        [self row:@"Resolution" detail:@"Lower is faster, with a slightly softer image" control:_res],
        [self row:@"Screen" detail:@"Wide fills the screen; Original keeps the old proportions" control:_screen],
        [self row:@"FPS counter" detail:nil control:_counter],
        [self row:@"Controller" detail:@"A jump, X punch, Y web, B spider-sense, R super, L action/rescue, Menu pause" control:_controller],
        [self row:@"Tilt" detail:@"Use the accelerometer like the original" control:_tilt],
    ];
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:rows];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 18;
    stack.translatesAutoresizingMaskIntoConstraints = NO;

    UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration *cfg = [UIButtonConfiguration filledButtonConfiguration];
    cfg.title = self.gameRunning ? @"Done" : @"Play";
    cfg.baseBackgroundColor = [UIColor colorWithRed:0.85 green:0.1 blue:0.12 alpha:1];
    cfg.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    cfg.contentInsets = NSDirectionalEdgeInsetsMake(10, 28, 10, 28);
    done.configuration = cfg;
    [done addTarget:self action:@selector(done) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:done];

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    [scroll addSubview:stack];
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:24],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-24],
        [scroll.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-24],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.trailingAnchor],
    ]];
}

- (void)done {
    int fps = _fps.selectedSegmentIndex == 0 ? 30 : _fps.selectedSegmentIndex == 2 ? 120 : 60;
    int res = _res.selectedSegmentIndex == 1 ? 75 : _res.selectedSegmentIndex == 2 ? 50 : 100;
    NSString *screen = _screen.selectedSegmentIndex == 1 ? @"fit" : _screen.selectedSegmentIndex == 2 ? @"stretch" : @"wide";
    [D() setInteger:fps forKey:@"fps"];
    [D() setInteger:res forKey:@"resolution"];
    [D() setObject:screen forKey:@"screen"];
    [D() setBool:_counter.on forKey:@"fpsCounter"];
    [D() setBool:_controller.on forKey:@"controller"];
    [D() setBool:_tilt.on forKey:@"tilt"];
    BOOL needsRestart = self.gameRunning && (fps != _startFps || res != _startRes || ![screen isEqual:_startScreen]);
    if (!needsRestart) { if (self.onDone) self.onDone(); return; }
    // Frame rate, resolution and screen mode are set when the engine starts (like the revival, which
    // restarted the game after saving): close the game so it opens with them next time.
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"Restart needed"
        message:@"Frame rate, resolution and screen apply when the game starts. The game closes now; open it again to play with the new settings. Your progress is saved."
        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Close game" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { exit(0); }]];
    [a addAction:[UIAlertAction actionWithTitle:@"Later" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { if (self.onDone) self.onDone(); }]];
    [self presentViewController:a animated:YES completion:nil];
}
@end
