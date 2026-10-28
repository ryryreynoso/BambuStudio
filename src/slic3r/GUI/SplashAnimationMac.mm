#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "SplashAnimationMac.hpp"

#include <algorithm>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

// Same geometry and colours as scripts/icon/make_icon.swift, in its 1024 pt design units (y up).
const CGFloat DESIGN_SCALE = 0.42;
const CGFloat WIDTH = 420, HEIGHT = 500, ART_BOTTOM = 150;
const CGFloat LAYER_H = 40, LAYER_GAP = 7, BASE_Y = 262, LAYER_X = 302, LAYER_W = 420;
const unsigned LAYER_COLOURS[] = {0x7b4dff, 0x3d7bff, 0x1fc8e8, 0x2ed47a, 0xc6e83a, 0xffc93a, 0xff8a3a, 0xff4d6d};
const int LAYERS = 8;

// Timing of one loop, in seconds: each layer, then a pause on the finished stack, then a fade.
const double LAYER_TIME = 0.26, HOLD_TIME = 0.6, FADE_TIME = 0.35;
const double CYCLE = LAYERS * LAYER_TIME + HOLD_TIME + FADE_TIME;

CGFloat X(CGFloat x) { return WIDTH / 2 + (x - 512) * DESIGN_SCALE; }
CGFloat Y(CGFloat y) { return ART_BOTTOM + (y - 216) * DESIGN_SCALE; }
CGFloat D(CGFloat d) { return d * DESIGN_SCALE; }

CGColorRef rgb(unsigned hex, CGFloat alpha = 1)
{
    return [[NSColor colorWithSRGBRed:((hex >> 16) & 0xff) / 255.0 green:((hex >> 8) & 0xff) / 255.0 blue:(hex & 0xff) / 255.0 alpha:alpha] CGColor];
}

NSNumber *at(double seconds) { return @(seconds / CYCLE); }

CALayer *rounded(CGRect frame, unsigned colour, CGFloat radius)
{
    CALayer *layer        = [CALayer layer];
    layer.frame           = frame;
    layer.backgroundColor = rgb(colour);
    layer.cornerRadius    = radius;
    return layer;
}

CATextLayer *text_layer(NSString *text, CGFloat size, NSFontWeight weight, unsigned colour, CGFloat y, CGFloat scale)
{
    CATextLayer *layer    = [CATextLayer layer];
    layer.string          = text;
    layer.font            = (__bridge CFTypeRef)[NSFont systemFontOfSize:size weight:weight];
    layer.fontSize        = size;
    layer.foregroundColor = rgb(colour);
    layer.alignmentMode   = kCAAlignmentCenter;
    layer.truncationMode  = kCATruncationEnd;
    layer.contentsScale   = scale;
    layer.frame           = CGRectMake(20, y, WIDTH - 40, size * 1.4);
    return layer;
}

void repeat_forever(CALayer *layer, NSArray<CAAnimation *> *animations, NSString *key)
{
    CAAnimationGroup *group = [CAAnimationGroup animation];
    group.animations        = animations;
    group.duration          = CYCLE;
    group.repeatCount       = HUGE_VALF;
    group.removedOnCompletion = NO;
    [layer addAnimation:group forKey:key];
}

// The nozzle, drawn around its tip: (0, 0) is the top of the layer being printed.
CALayer *make_nozzle()
{
    CALayer *nozzle = [CALayer layer];
    nozzle.bounds   = CGRectMake(-D(70), -D(6), D(140), D(251));
    nozzle.anchorPoint = CGPointMake(0.5, D(6) / D(251));

    CALayer *filament = rounded(CGRectMake(-D(9), -D(6), D(18), D(50)), LAYER_COLOURS[0], D(9));
    filament.name     = @"filament";
    [nozzle addSublayer:filament];

    CAShapeLayer *tip = [CAShapeLayer layer];
    CGMutablePathRef path = CGPathCreateMutable();
    CGPathMoveToPoint(path, nullptr, -D(46), D(122));
    CGPathAddLineToPoint(path, nullptr, D(46), D(122));
    CGPathAddLineToPoint(path, nullptr, D(12), D(40));
    CGPathAddLineToPoint(path, nullptr, -D(12), D(40));
    CGPathCloseSubpath(path);
    tip.path      = path;
    tip.fillColor = rgb(0xaab0c8);
    CGPathRelease(path);
    [nozzle addSublayer:tip];

    [nozzle addSublayer:rounded(CGRectMake(-D(70), D(120), D(140), D(125)), 0xd9dcea, D(22))];
    [nozzle addSublayer:rounded(CGRectMake(-D(70), D(175), D(140), D(22)), 0x8a91ad, 0)];
    return nozzle;
}

} // namespace

SplashAnimationMac::SplashAnimationMac(const std::string &title, const std::string &version)
{
    NSRect   frame  = NSMakeRect(0, 0, WIDTH, HEIGHT);
    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    window.opaque          = NO;
    window.backgroundColor = [NSColor clearColor];
    window.hasShadow       = YES;
    window.level           = NSFloatingWindowLevel;
    window.releasedWhenClosed = NO;
    [window center];

    const CGFloat scale = window.backingScaleFactor;
    NSView  *view = [[[NSView alloc] initWithFrame:frame] autorelease];
    CALayer *root = [CALayer layer];
    root.frame    = frame;
    view.layer    = root; // layer-hosting: the window server runs the animations below
    view.wantsLayer = YES;
    window.contentView = view;

    // Tile: the icon's navy-to-plum gradient with rounded corners.
    CAGradientLayer *tile = [CAGradientLayer layer];
    tile.frame        = frame;
    tile.colors       = @[(__bridge id) rgb(0x2b1f52), (__bridge id) rgb(0x141a33)];
    tile.startPoint   = CGPointMake(0.5, 1);
    tile.endPoint     = CGPointMake(0.5, 0);
    tile.cornerRadius = 36;
    tile.masksToBounds = YES;
    [root addSublayer:tile];

    // Build plate.
    [tile addSublayer:rounded(CGRectMake(X(220), Y(216), D(584), D(32)), 0x3a4170, D(17))];

    // Layers: even ones print left to right, odd ones right to left, like a real perimeter pass.
    std::vector<CGPoint> nozzle_points;
    std::vector<double>  nozzle_times;
    NSMutableArray      *filament_colours = [NSMutableArray array];
    NSMutableArray      *filament_times   = [NSMutableArray array];
    for (int i = 0; i < LAYERS; ++i) {
        const bool    rightward = i % 2 == 0;
        const CGFloat y         = BASE_Y + i * (LAYER_H + LAYER_GAP);
        const double  start = i * LAYER_TIME, end = start + LAYER_TIME;

        CALayer *layer     = rounded(CGRectMake(0, 0, D(LAYER_W), D(LAYER_H)), LAYER_COLOURS[i], D(20));
        layer.anchorPoint  = CGPointMake(rightward ? 0 : 1, 0);
        layer.position     = CGPointMake(X(rightward ? LAYER_X : LAYER_X + LAYER_W), Y(y));
        layer.masksToBounds = YES;
        CALayer *highlight = rounded(CGRectMake(D(10), D(LAYER_H * 0.55), D(LAYER_W - 20), D(LAYER_H * 0.28)), 0xffffff, D(8));
        highlight.opacity  = 0.18;
        [layer addSublayer:highlight];
        [tile addSublayer:layer];

        CAKeyframeAnimation *grow = [CAKeyframeAnimation animationWithKeyPath:@"bounds.size.width"];
        grow.values   = @[@0, @0, @(D(LAYER_W)), @(D(LAYER_W)), @(D(LAYER_W)), @0];
        grow.keyTimes = @[@0, at(start), at(end), at(CYCLE - FADE_TIME), at(CYCLE - 0.01), @1];
        CAKeyframeAnimation *fade = [CAKeyframeAnimation animationWithKeyPath:@"opacity"];
        // Hidden until its pass starts (a zero-width rounded layer still draws a sliver).
        fade.values   = @[@0, @0, @1, @1, @0, @0];
        fade.keyTimes = @[@0, at(start), at(start), at(CYCLE - FADE_TIME), at(CYCLE - 0.01), @1];
        repeat_forever(layer, @[grow, fade], @"print");

        const CGFloat top = y + LAYER_H;
        nozzle_points.push_back(CGPointMake(X(rightward ? LAYER_X : LAYER_X + LAYER_W), Y(top)));
        nozzle_times.push_back(start);
        nozzle_points.push_back(CGPointMake(X(rightward ? LAYER_X + LAYER_W : LAYER_X), Y(top)));
        nozzle_times.push_back(end);
        [filament_colours addObject:(__bridge id) rgb(LAYER_COLOURS[i])];
        [filament_times addObject:at(start)];
    }

    // Nozzle: follows each pass, lifts off the finished part, and drops back for the next loop.
    CALayer *nozzle = make_nozzle();
    nozzle.position = nozzle_points.front();
    [tile addSublayer:nozzle];
    const CGPoint parked = CGPointMake(X(LAYER_X + LAYER_W / 2), nozzle_points.back().y + D(70));
    nozzle_points.push_back(parked);
    nozzle_times.push_back(LAYERS * LAYER_TIME + HOLD_TIME * 0.6);
    nozzle_points.push_back(parked);
    nozzle_times.push_back(CYCLE - FADE_TIME);
    nozzle_points.push_back(nozzle_points.front());
    nozzle_times.push_back(CYCLE);

    NSMutableArray *values = [NSMutableArray array], *times = [NSMutableArray array], *timing = [NSMutableArray array];
    for (size_t i = 0; i < nozzle_points.size(); ++i) {
        [values addObject:[NSValue valueWithPoint:NSPointFromCGPoint(nozzle_points[i])]];
        [times addObject:at(nozzle_times[i])];
        if (i + 1 < nozzle_points.size())
            // Steady speed along each pass; eased lift off the part and return.
            [timing addObject:[CAMediaTimingFunction functionWithName:i + 1 < LAYERS * 2 ? kCAMediaTimingFunctionLinear : kCAMediaTimingFunctionEaseInEaseOut]];
    }
    CAKeyframeAnimation *move = [CAKeyframeAnimation animationWithKeyPath:@"position"];
    move.values          = values;
    move.keyTimes        = times;
    move.timingFunctions = timing;
    repeat_forever(nozzle, @[move], @"move");

    for (CALayer *part in nozzle.sublayers)
        if ([part.name isEqualToString:@"filament"]) {
            [filament_colours addObject:filament_colours.firstObject];
            [filament_times addObject:@1];
            CAKeyframeAnimation *colour = [CAKeyframeAnimation animationWithKeyPath:@"backgroundColor"];
            colour.values          = filament_colours;
            colour.keyTimes        = filament_times;
            colour.calculationMode = kCAAnimationDiscrete;
            CAKeyframeAnimation *ooze = [CAKeyframeAnimation animationWithKeyPath:@"opacity"];
            ooze.values   = @[@1, @1, @0, @0, @1];
            ooze.keyTimes = @[@0, at(LAYERS * LAYER_TIME), at(LAYERS * LAYER_TIME + 0.1), at(CYCLE - 0.05), @1];
            repeat_forever(part, @[colour, ooze], @"colour");
        }

    // Title, version and the loading step.
    [tile addSublayer:text_layer([NSString stringWithUTF8String:title.c_str()], 26, NSFontWeightBold, 0xffffff, 92, scale)];
    [tile addSublayer:text_layer([NSString stringWithUTF8String:version.c_str()], 13, NSFontWeightRegular, 0x9aa0c0, 70, scale)];
    CATextLayer *status = text_layer(@"", 12, NSFontWeightRegular, 0x9aa0c0, 28, scale);
    [tile addSublayer:status];

    [window orderFrontRegardless];
    [window display];
    [CATransaction flush];

    m_window  = window;
    m_status  = [status retain];
    m_started = CACurrentMediaTime();
}

void SplashAnimationMac::set_text(const std::string &text)
{
    CATextLayer *status = (CATextLayer *) m_status;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    status.string = [NSString stringWithUTF8String:text.c_str()];
    [CATransaction commit];
    // Push it to the window server now: the main thread is about to be busy.
    [CATransaction flush];
}

SplashAnimationMac::~SplashAnimationMac()
{
    NSWindow    *window = (NSWindow *) m_window;
    CATextLayer *status = (CATextLayer *) m_status;
    // Let the stack finish before fading, even when loading was quick; the app carries on meanwhile.
    const double wait = std::max(0., LAYERS * LAYER_TIME + 0.3 - (CACurrentMediaTime() - m_started));
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (wait * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext *context) {
            context.duration = 0.3;
            window.animator.alphaValue = 0;
        } completionHandler:^{
            [window orderOut:nil];
            [status release];
            [window release];
        }];
    });
}

} // namespace GUI
} // namespace Slic3r
