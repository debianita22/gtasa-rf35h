/* platform.h -- window and EGL plumbing on Wayland, gamepads through SDL2
 *
 * The game creates and drives its own EGL context from its RenderQueue
 * thread (as it does on Android); the platform only supplies the display,
 * the native window behind ANativeWindow, and the input.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_PLATFORM_H
#define GTASA_PLATFORM_H

#include <stdint.h>
#include <EGL/egl.h>

/* Connect to the compositor, open a fullscreen window and start SDL's game
 * controller support. The render size is the output's scaled by
 * pconfig.render_scale, or w x h when both are > 0; anything smaller than
 * the output is scaled up by the compositor. Returns 0 or fails fatally. */
int platform_init(const char *title, int w, int h);
void platform_shutdown(void);

void platform_window_size(int *w, int *h); /* render (buffer) size */
void platform_output_size(int *w, int *h); /* size shown on the output */
int platform_scaled(void);                 /* 1 when the compositor scales the image */
int platform_fullscreen(void);             /* 1 once the compositor made it fullscreen */
void *platform_anative_window(void);      /* token the game sees as ANativeWindow */
EGLNativeWindowType platform_egl_window(void);
EGLDisplay platform_egl_display(void);
/* ANativeWindow_setBuffersGeometry: new buffer size; 0 x 0 = back to the
 * render size chosen by platform_init (render_scale is kept) */
void platform_resize_buffers(int w, int h);

/* Dispatch window events and pad hotplug; main thread, every frame. */
void platform_pump(void);
/* The Wayland part of platform_pump() (exposed for tests): reads and
 * dispatches the default queue without blocking; -1 once the connection is
 * unusable. */
struct wl_display;
int platform_wayland_pump(struct wl_display *d);
int platform_quit_requested(void);
void platform_request_quit(void);

enum {
  PAD_A = 1u << 0,      /* south face button */
  PAD_B = 1u << 1,      /* east */
  PAD_X = 1u << 2,      /* west */
  PAD_Y = 1u << 3,      /* north */
  PAD_BACK = 1u << 4,   /* SELECT */
  PAD_GUIDE = 1u << 5,
  PAD_START = 1u << 6,
  PAD_L3 = 1u << 7,
  PAD_R3 = 1u << 8,
  PAD_L1 = 1u << 9,
  PAD_R1 = 1u << 10,
  PAD_UP = 1u << 11,
  PAD_DOWN = 1u << 12,
  PAD_LEFT = 1u << 13,
  PAD_RIGHT = 1u << 14,
  PAD_L2 = 1u << 15,
  PAD_R2 = 1u << 16,
};

typedef struct {
  uint32_t buttons;
  float lx, ly, rx, ry; /* -1..1, y down, dead zone applied */
  float lt, rt;         /* 0..1 */
} PadState;

/* Current state of the first connected pad; returns 0 when there is none. */
int platform_pad(PadState *out);

#endif
