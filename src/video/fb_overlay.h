/**
 * Frametime overlay for the framebuffer window. See fb_overlay.c.
 *
 * Sampling is called from the flip (nv2a_pb_exec.c), drawing from whichever
 * presenter hands the finished frame to the screen. All functions are cheap
 * no-ops while the overlay is off, which is the default.
 */
#ifndef XBOX_FB_OVERLAY_H
#define XBOX_FB_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void xbox_OverlayInit(void);
int  xbox_OverlayEnabled(void);
void xbox_OverlayToggle(void);

/* One presented frame: ms spent waiting for the GPU before the flip, and the
 * guest's cumulative draw counters (the overlay reports their per-frame
 * deltas). */
void xbox_OverlayFlip(double wait_ms, uint32_t draws, uint32_t vertices);

/* How long the present itself took (framebuffer copy plus any external
 * presenter), recorded against the frame the preceding Flip started. */
void xbox_OverlayPresentDone(void);

/* Blend the panel into a 32-bit 0x00RRGGBB image; stride is in pixels. */
void xbox_OverlayComposite(uint32_t *px, int width, int height, int stride);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FB_OVERLAY_H */
