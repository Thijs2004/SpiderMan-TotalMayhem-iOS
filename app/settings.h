#pragma once
#import <UIKit/UIKit.h>

int setting_fps(void);          // 30, 60 or 120
int setting_resolution(void);   // 100, 75 or 50 (%)
NSString *setting_screen(void); // wide, fit, stretch
BOOL setting_fps_counter(void);
BOOL setting_controller(void);
BOOL setting_tilt(void);

@interface SettingsVC : UIViewController
@property(nonatomic) BOOL gameRunning;
@property(nonatomic, copy) void (^onDone)(void);
@end
