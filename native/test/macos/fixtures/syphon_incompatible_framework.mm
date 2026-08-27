#import <Foundation/Foundation.h>

// A framework that registers SyphonMetalServer without the selectors the
// daemon calls -- a Syphon too old or too new for this build. It is a
// different failure from a framework that registers nothing at all, and an
// operator told the wrong one goes looking in the wrong place.
@interface SyphonMetalServer : NSObject
- (void)stop;
@end

@implementation SyphonMetalServer
- (void)stop {
}
@end
