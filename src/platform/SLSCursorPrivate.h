/*
 * macOS SkyLight private cursor API — extern declarations.
 *
 * SLSGetGlobalCursorData returns the *system-wide* current cursor bitmap,
 * which is what RDP clients want to render. The public equivalent,
 * [NSCursor currentSystemCursor], reads only the calling process's
 * cursor — a background daemon with no key window therefore never sees
 * anything but the default arrow, so text I-beams, hands, resize
 * cursors, busy spinners, etc. never reach the client.
 *
 * Symbols live in /System/Library/PrivateFrameworks/SkyLight.framework
 * (loaded transitively via CoreGraphics). We resolve them at runtime
 * with dlsym(RTLD_DEFAULT, ...) rather than link-time, so a future
 * macOS that renames or removes them just falls back to NSCursor
 * instead of crashing at launch.
 */

#pragma once

#import <CoreGraphics/CoreGraphics.h>
#include <stdint.h>

typedef int (*SLSMainConnectionIDFn)(void);
typedef int (*SLSGetCurrentCursorSeedFn)(int cid);
typedef CGError (*SLSGetGlobalCursorDataSizeFn)(int cid, size_t *size);
typedef CGError (*SLSGetGlobalCursorDataFn)(int cid,
                                            void *data,
                                            size_t *size,
                                            int *row_bytes,
                                            CGRect *bounds,
                                            CGPoint *hot_spot,
                                            int *bits_per_pixel,
                                            int *components,
                                            int *bits_per_component);
