#include "environment.h"
#import <Foundation/Foundation.h>

namespace detail {

    std::string getMacCacheDir()
    {
        @autoreleasepool {
            NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
                NSCachesDirectory, NSUserDomainMask, YES);
            NSString* path = [dirs firstObject];
            return path ? std::string([path UTF8String]) : "";
        }
    }

    std::string getMacConfigDir()
    {
        @autoreleasepool {
            NSArray<NSString*>* dirs = NSSearchPathForDirectoriesInDomains(
                NSApplicationSupportDirectory, NSUserDomainMask, YES);
            NSString* path = [dirs firstObject];
            return path ? std::string([path UTF8String]) : "";
        }
    }

} // namespace detail
