// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Ben Kelly
// obscam: reads raw 356x292 bayer_grbg8 frames on stdin and sends them to the OBS Virtual Camera.
// It needs the camera extension from OBS 30 or later. The OBS app does not need to run.
// Usage: ./legocam | ./obscam
#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CoreMediaIO.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#include <libswscale/swscale.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define W 356
#define H 292
#define OBS_DEVICE_UID CFSTR("7626645E-4425-469E-9D8B-97E0FA59AC75") /* from the OBS source */

static OSStatus get_prop(CMIOObjectID obj, CMIOObjectPropertySelector sel, void *out, UInt32 size, UInt32 *used)
{
    CMIOObjectPropertyAddress a = { sel, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
    return CMIOObjectGetPropertyData(obj, &a, 0, NULL, size, used, out);
}

static void queue_altered(CMIOStreamID stream, void *token, void *refcon)
{
    (void)stream, (void)token, (void)refcon;
}

static CMIOObjectID find_obs_device(void)
{
    CMIOObjectID devs[64];
    UInt32 used = 0;
    if (get_prop(kCMIOObjectSystemObject, kCMIOHardwarePropertyDevices, devs, sizeof devs, &used))
        return 0;
    for (UInt32 i = 0; i < used / sizeof devs[0]; i++) {
        CFStringRef uid = NULL;
        UInt32 n;
        if (get_prop(devs[i], kCMIODevicePropertyDeviceUID, &uid, sizeof uid, &n) || !uid)
            continue;
        Boolean match = CFEqual(uid, OBS_DEVICE_UID);
        CFRelease(uid);
        if (match)
            return devs[i];
    }
    return 0;
}

/* Camera streams have direction 1. The sink, which takes frames in, has direction 0. */
static CMIOStreamID find_sink(CMIOObjectID dev)
{
    CMIOStreamID streams[8];
    UInt32 used = 0;
    if (get_prop(dev, kCMIODevicePropertyStreams, streams, sizeof streams, &used))
        return 0;
    for (UInt32 i = 0; i < used / sizeof streams[0]; i++) {
        UInt32 dir = 1, n;
        if (!get_prop(streams[i], kCMIOStreamPropertyDirection, &dir, sizeof dir, &n) && dir == 0)
            return streams[i];
    }
    return 0;
}

int main(void)
{
    @autoreleasepool {
        if (isatty(STDIN_FILENO)) {
            fprintf(stderr, "usage: ./legocam | ./obscam\n");
            return 1;
        }
        signal(SIGINT, SIG_IGN); /* exit on EOF, after legocam turns the camera off */

        CMIOObjectID dev = find_obs_device();
        CMIOStreamID sink = dev ? find_sink(dev) : 0;
        if (!sink) {
            fprintf(stderr, "obscam: OBS Virtual Camera not found. Install OBS 30 or later, "
                            "open it once, and allow its camera extension.\n");
            return 1;
        }
        CMSimpleQueueRef queue = NULL;
        if (CMIOStreamCopyBufferQueue(sink, queue_altered, NULL, &queue) || CMIODeviceStartStream(dev, sink)) {
            fprintf(stderr, "obscam: cannot start the OBS Virtual Camera sink stream\n");
            return 1;
        }

        NSDictionary *attrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_422YpCbCr8),
            (id)kCVPixelBufferWidthKey : @W,
            (id)kCVPixelBufferHeightKey : @H,
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        };
        CVPixelBufferPoolRef pool = NULL;
        CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &pool);
        struct SwsContext *sws = sws_getContext(W, H, AV_PIX_FMT_BAYER_GRBG8, W, H, AV_PIX_FMT_UYVY422,
                                                SWS_BILINEAR, NULL, NULL, NULL);
        if (!pool || !sws) {
            fprintf(stderr, "obscam: setup failed\n");
            return 1;
        }

        static uint8_t bayer[W * H];
        CMFormatDescriptionRef fmt = NULL;
        while (fread(bayer, 1, sizeof bayer, stdin) == sizeof bayer) {
            CVPixelBufferRef pb = NULL;
            if (CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess)
                continue;
            CVPixelBufferLockBaseAddress(pb, 0);
            const uint8_t *src[4] = { bayer };
            int src_stride[4] = { W };
            uint8_t *dst[4] = { CVPixelBufferGetBaseAddress(pb) };
            int dst_stride[4] = { (int)CVPixelBufferGetBytesPerRow(pb) };
            sws_scale(sws, src, src_stride, 0, H, dst, dst_stride);
            CVPixelBufferUnlockBaseAddress(pb, 0);

            if (!fmt)
                CMVideoFormatDescriptionCreateForImageBuffer(NULL, pb, &fmt);
            CMSampleTimingInfo timing = { .presentationTimeStamp = CMClockGetTime(CMClockGetHostTimeClock()) };
            CMSampleBufferRef sb = NULL;
            if (!CMSampleBufferCreateForImageBuffer(NULL, pb, true, NULL, NULL, fmt, &timing, &sb)
                && CMSimpleQueueEnqueue(queue, sb))
                CFRelease(sb); /* queue full: drop this frame */
            CVPixelBufferRelease(pb);
        }
        CMIODeviceStopStream(dev, sink);
    }
    return 0;
}
