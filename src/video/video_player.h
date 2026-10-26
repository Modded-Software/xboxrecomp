/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * For a title whose video is a container Windows can already decode, the
 * decoder does not have to be emulated for the video to be watchable. The
 * recompiled title still decides when: it opens the file as its own logic
 * dictates, and the runtime plays the file it opened.
 */

#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

#include <stdint.h>

/* Initialize/shutdown Media Foundation (call once at app start/end) */
int  video_init(void);
void video_shutdown(void);

/* Open a video file for playback. Returns 0 on success. */
int  video_open(const char *path);

/* Advance playback by dt seconds, decode next frame if needed.
 * Returns 1 if a new frame is ready, 0 if unchanged, -1 if finished. */
int  video_update(float dt);

/* Render the current video frame as a fullscreen quad.
 * Call between BeginScene/EndScene. */
void video_render(void);

/* Check if the video has finished playing. */
int  video_is_finished(void);

/* Close the current video and release resources. */
void video_close(void);

/* === Boot sequence state machine === */

/* Boot phases */

/* Get current boot phase */

/* Advance boot state machine. Call once per frame.
 * skip=1 if user pressed a button to skip current video.
 * Returns the new phase. */

/* Render current boot phase (video frame or press-start screen). */


/* Play one video file, start to finish, in its own window on its own thread.
 * Returns 0 if the pump started. Blocking work happens on that thread, so the
 * guest carries on running while the video plays. */
int  xbox_VideoPlayFile(const char *host_path);
int  xbox_VideoIsPlaying(void);

/* Write the current decoded frame to a 24-bit BMP -- evidence that real
 * pixels reached the renderer, which a log line cannot give. */
int  video_dump_frame_bmp(const char *path);

/* Show the guest framebuffer in its own window (RECOMP_FB_WINDOW). Whatever
 * the title renders into guest RAM appears there; nothing else scans it out. */
/* Non-zero while that virtual key is held in the framebuffer window. Always
 * zero when there is no window, which is the right answer: with nothing to
 * focus there is nothing to type into. */
int xbox_FramebufferKeyDown(int vk);

/* Mouse, for the keyboard+mouse mode (RECOMP_KBM). Delta is the movement
 * accumulated since the last read, cleared on read; buttons are 0=L, 1=R,
 * 2=M; wheel is in notches. Capture clips and recentres the cursor so movement
 * is unbounded. All zero when there is no window. */
int xbox_FramebufferMouseDelta(int *dx, int *dy);
/* Return motion the caller could not consume this poll; carried to the next. */
void xbox_FramebufferMousePushback(int dx, int dy);
int xbox_FramebufferMouseButton(int which);
int xbox_FramebufferMouseWheel(void);
void xbox_FramebufferMouseCapture(int on);

void xbox_FramebufferWindowStart(void);
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
/* Rows per displayed row: 2 for a 2x2 anti-aliased surface, else 1. */
void xbox_FramebufferWindowSetAA(uint32_t sy);
int  xbox_FramebufferDumpBmp(const char *path);

/* The framebuffer dimensions the window reads (the title's render resolution).
 * An external presenter needs these to size its swap chain and interpret a
 * flip's pitch (bytes-per-pixel = pitch / width); the window's client rect can
 * differ from the rendered frame. Returns 0 when no window exists yet. */
int  xbox_FramebufferWindowSize(uint32_t *width, uint32_t *height);

/* Optional per-flip present hook. When installed, the pushbuffer executor calls
 * this callback with each finished frame, *in addition to* the built-in GDI
 * framebuffer window (which is suspended, below), so an alternative presenter
 * (e.g. a D3D11/DXGI swap chain feeding ReShade) can own presentation. fb_va is
 * a guest VA: resolve it with xbox_GetMemoryOffset(); pitch is bytes per row.
 * The callback runs synchronously at the flip, so the frame is stable to read.
 * Installing a hook also calls xbox_FramebufferWindowSuspend(1); passing a NULL
 * hook restores the default GDI path. */
typedef void (*xbox_present_hook_t)(uint32_t fb_va, uint32_t pitch, void *user);
void xbox_SetPresentHook(xbox_present_hook_t hook, void *user);

/* Stop (on=1) or resume (on=0) the built-in GDI blit in fb_thread while an
 * external presenter owns the window. Message pumping, input, recording/replay,
 * the F12 dump and the window-title FPS counter all keep running; only the
 * StretchDIBits call is skipped, so a D3D11 presenter does not fight GDI for
 * the same HWND. No-op on non-Windows. */
void xbox_FramebufferWindowSuspend(int on);

#if !defined(_WIN32)
/* POSIX hosts: the framebuffer window needs the process main thread (AppKit
 * on macOS), so a title that wants it calls this from main() with its boot
 * sequence. With RECOMP_FB_WINDOW set it opens the window, runs guest_main on
 * a pthread and pumps events until guest_main returns (its value is returned)
 * or the window is closed (the process exits). Otherwise, or headless, it
 * just calls guest_main. */
int  xbox_HostWindowMain(int (*guest_main)(void));
#endif

#endif /* BURNOUT3_VIDEO_PLAYER_H */
