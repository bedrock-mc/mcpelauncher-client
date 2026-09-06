#include "macos_dock.h"
#import <AppKit/AppKit.h>

void macos_set_dock_icon(const char *iconPath) {
    @autoreleasepool {
        NSImage *image = [[NSImage alloc] initWithContentsOfFile:[NSString stringWithUTF8String:iconPath]];
        if(image)
            [NSApp setApplicationIconImage:image];
    }
}
