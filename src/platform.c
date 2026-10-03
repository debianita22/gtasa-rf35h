/* platform.c -- the game's window on Wayland (sway), EGL plumbing, pads
 *
 * The window is a plain xdg-shell toplevel asking to be fullscreen. The
 * game's eglCreateWindowSurface gets a wl_egl_window on its surface, on the
 * EGL display made here for the same connection. Lakka's SDL2 is built for
 * input only (no video subsystem), and input is all it is used for: game
 * controllers through SDL's mapping database, hotplug included.
 *
 * A render size below the output's (render_scale, an explicit
 * screen_width/height, or the game's own ANativeWindow_setBuffersGeometry)
 * keeps the buffer small and lets the compositor scale it to the output
 * through wp_viewporter, as SurfaceFlinger does on Android: the GPU shades
 * fewer pixels and nothing in the game's GL changes.
 *
 * sway answers the first commit with a 0x0 configure and makes the window
 * fullscreen only when it maps (on the first buffer), so the starting size
 * comes from the output's current mode (wl_output, transform and scale
 * applied); the fullscreen configure that follows normally confirms it.
 *
 * Threads: the main thread dispatches the default event queue in
 * platform_pump(); Mesa reads its own queue from the game's render thread.
 * Reading goes through wl_display_prepare_read(), so the two never steal
 * each other's events. The sizes and the viewport change from the configure
 * handler (main thread) and from setBuffersGeometry (any game thread):
 * size_lock keeps the two apart.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "platform.h"
#include "platform_config.h"
#include "error.h"
#include "util.h"

static struct {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct xdg_wm_base *wm_base;
  struct wp_viewporter *viewporter;
  struct wl_output *output;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct wp_viewport *viewport;
  struct wl_egl_window *egl_window;
  EGLDisplay egl_display;
  int configured;
  int pending_w, pending_h; /* from the last xdg_toplevel.configure */
  int pending_fullscreen, fullscreen;
  int out_w, out_h;         /* what the compositor shows the window at */
  int w, h;                 /* buffer size: what the game renders */
  int base_w, base_h;       /* render size chosen at start; setBuffersGeometry(0, 0) */
  int mode_w, mode_h, mode_mhz, out_transform, out_scale; /* first output */
  int lost;                 /* connection unusable: stop reading it */
  int sdl_ok;
  SDL_GameController *pad;
  SDL_JoystickID pad_id;
  volatile int quit;
} P;

static pthread_mutex_t size_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Wayland ---------------------------------------------------------------------- */

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

/* size_lock held */
static void update_viewport(void) {
  const int scaled = P.w != P.out_w || P.h != P.out_h;
  if (!P.viewporter || !P.surface)
    return;
  if (scaled && !P.viewport)
    P.viewport = wp_viewporter_get_viewport(P.viewporter, P.surface);
  if (P.viewport)
    wp_viewport_set_destination(P.viewport, scaled ? P.out_w : -1, scaled ? P.out_h : -1);
}

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
  (void)data;
  pthread_mutex_lock(&size_lock);
  if (P.pending_w > 0 && P.pending_h > 0 &&
      (P.pending_w != P.out_w || P.pending_h != P.out_h)) {
    if (P.configured)
      debugPrintf("window: compositor size %dx%d -> %dx%d\n", P.out_w, P.out_h, P.pending_w,
                  P.pending_h);
    P.out_w = P.pending_w;
    P.out_h = P.pending_h;
    if (P.configured)
      update_viewport(); /* the buffer keeps its size; the scaling follows */
  }
  pthread_mutex_unlock(&size_lock);
  if (P.pending_fullscreen != P.fullscreen && P.configured)
    debugPrintf("window: %s\n", P.pending_fullscreen ? "fullscreen" : "no longer fullscreen");
  P.fullscreen = P.pending_fullscreen;
  xdg_surface_ack_configure(surface, serial);
  P.configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *states) {
  (void)data; (void)t;
  P.pending_w = w;
  P.pending_h = h;
  P.pending_fullscreen = 0;
  const uint32_t *state;
  wl_array_for_each(state, states)
    if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
      P.pending_fullscreen = 1;
}
static void toplevel_close(void *data, struct xdg_toplevel *t) {
  (void)data; (void)t;
  debugPrintf("window: closed by the compositor\n");
  P.quit = 1;
}
static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
  (void)data; (void)t; (void)w; (void)h;
}
static void toplevel_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps) {
  (void)data; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener toplevel_listener = {
  toplevel_configure, toplevel_close, toplevel_bounds, toplevel_caps,
};

static void output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t mm_w,
                            int32_t mm_h, int32_t subpixel, const char *make, const char *model,
                            int32_t transform) {
  (void)data; (void)o; (void)x; (void)y; (void)mm_w; (void)mm_h; (void)subpixel; (void)make;
  (void)model;
  P.out_transform = transform;
}
static void output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                        int32_t refresh) {
  (void)data; (void)o;
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    P.mode_w = w;
    P.mode_h = h;
    P.mode_mhz = refresh;
  }
}
static void output_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void output_scale(void *data, struct wl_output *o, int32_t factor) {
  (void)data; (void)o;
  P.out_scale = factor;
}
static const struct wl_output_listener output_listener = {
  .geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale,
};

/* the output's size in surface coordinates: mode, rotated, over the scale */
static void output_logical_size(int *w, int *h) {
  int mw = P.mode_w, mh = P.mode_h;
  if (P.out_transform & 1) { /* 90 and 270, flipped or not */
    const int t = mw;
    mw = mh;
    mh = t;
  }
  const int scale = P.out_scale > 1 ? P.out_scale : 1;
  *w = mw / scale;
  *h = mh / scale;
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  (void)data;
  if (!strcmp(iface, wl_compositor_interface.name)) {
    P.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, min_u32(version, 4));
  } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
    P.wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, min_u32(version, 3));
    xdg_wm_base_add_listener(P.wm_base, &wm_base_listener, NULL);
  } else if (!strcmp(iface, wp_viewporter_interface.name)) {
    P.viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  } else if (!strcmp(iface, wl_output_interface.name) && !P.output) {
    P.output = wl_registry_bind(reg, name, &wl_output_interface, min_u32(version, 2));
    wl_output_add_listener(P.output, &output_listener, NULL);
  }
}
static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
  (void)data; (void)reg; (void)name;
}
static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static EGLDisplay make_egl_display(struct wl_display *wl) {
  EGLDisplay dpy = EGL_NO_DISPLAY;
  PFNEGLGETPLATFORMDISPLAYPROC get15 =
      (PFNEGLGETPLATFORMDISPLAYPROC)eglGetProcAddress("eglGetPlatformDisplay");
  if (get15)
    dpy = get15(EGL_PLATFORM_WAYLAND_KHR, wl, NULL);
  if (dpy == EGL_NO_DISPLAY) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC getext =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getext)
      dpy = getext(EGL_PLATFORM_WAYLAND_EXT, wl, NULL);
  }
  if (dpy == EGL_NO_DISPLAY)
    dpy = eglGetDisplay((EGLNativeDisplayType)wl);
  return dpy;
}

static int even(int v) { return v & ~1; }

/* ---- pads (SDL, input only) -------------------------------------------------------- */

static void open_pad(int index) {
  if (P.pad || !SDL_IsGameController(index))
    return;
  P.pad = SDL_GameControllerOpen(index);
  if (!P.pad)
    return;
  P.pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(P.pad));
  char *mapping = SDL_GameControllerMapping(P.pad);
  debugPrintf("pad: %s (%s)\n", SDL_GameControllerName(P.pad), mapping ? mapping : "no mapping");
  SDL_free(mapping);
}

static void log_unmapped_joystick(int index) {
  if (SDL_IsGameController(index))
    return;
  char guid[64];
  SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
  debugPrintf("pad: joystick '%s' (GUID %s) has no gamepad mapping; add one to "
              "gamecontrollerdb.txt in the game folder\n",
              SDL_JoystickNameForIndex(index), guid);
}

static void input_init(void) {
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
    debugPrintf("pad: SDL could not start (%s): no gamepad input\n", SDL_GetError());
    return;
  }
  P.sdl_ok = 1;
  if (access("gamecontrollerdb.txt", R_OK) == 0) {
    const int n = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
    debugPrintf("pad: %d mappings from gamecontrollerdb.txt\n", n);
  }
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    open_pad(i);
    log_unmapped_joystick(i);
  }
}

/* ---- public ------------------------------------------------------------------------- */

int platform_init(const char *title, int w, int h) {
  P.display = wl_display_connect(NULL);
  if (!P.display)
    fatal_error("Nessun compositor Wayland (WAYLAND_DISPLAY=%s):\nsway e' in esecuzione?",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
  P.registry = wl_display_get_registry(P.display);
  wl_registry_add_listener(P.registry, &registry_listener, NULL);
  wl_display_roundtrip(P.display);
  if (!P.compositor || !P.wm_base)
    fatal_error("Il compositor Wayland non offre wl_compositor o xdg_wm_base.");
  if (P.output) {
    wl_display_roundtrip(P.display); /* the output's geometry and mode */
    int lw = 0, lh = 0;
    output_logical_size(&lw, &lh);
    debugPrintf("window: output %dx%d at %d.%03d Hz, transform %d, scale %d -> %dx%d\n", P.mode_w,
                P.mode_h, P.mode_mhz / 1000, P.mode_mhz % 1000, P.out_transform,
                P.out_scale ? P.out_scale : 1, lw, lh);
  }

  P.surface = wl_compositor_create_surface(P.compositor);
  P.xdg_surface = xdg_wm_base_get_xdg_surface(P.wm_base, P.surface);
  xdg_surface_add_listener(P.xdg_surface, &xdg_surface_listener, NULL);
  P.toplevel = xdg_surface_get_toplevel(P.xdg_surface);
  xdg_toplevel_add_listener(P.toplevel, &toplevel_listener, NULL);
  xdg_toplevel_set_title(P.toplevel, title);
  xdg_toplevel_set_app_id(P.toplevel, "gtasa");
  xdg_toplevel_set_fullscreen(P.toplevel, NULL);
  wl_surface_commit(P.surface);
  while (!P.configured)
    if (wl_display_dispatch(P.display) < 0)
      fatal_error("Connessione Wayland persa durante la creazione della finestra.");
  pthread_mutex_lock(&size_lock);
  /* 0x0 = "your choice" (sway, until the window maps): the output's size */
  if (P.out_w <= 0 || P.out_h <= 0)
    output_logical_size(&P.out_w, &P.out_h);
  if (P.out_w <= 0 || P.out_h <= 0) {
    P.out_w = 640;
    P.out_h = 480;
  }

  if (w > 0 && h > 0) {
    P.w = w; /* explicit render size, scaled to the output */
    P.h = h;
  } else {
    int scale = pconfig.render_scale;
    if (scale < 50 || scale > 100)
      scale = 100;
    P.w = even(P.out_w * scale / 100);
    P.h = even(P.out_h * scale / 100);
  }
  P.base_w = P.w;
  P.base_h = P.h;
  if ((P.w != P.out_w || P.h != P.out_h) && !P.viewporter)
    debugPrintf("window: no wp_viewporter; the %dx%d image will not be scaled\n", P.w, P.h);
  update_viewport();
  pthread_mutex_unlock(&size_lock);

  P.egl_window = wl_egl_window_create(P.surface, P.w, P.h);
  if (!P.egl_window)
    fatal_error("wl_egl_window_create(%dx%d) non riuscita.", P.w, P.h);
  P.egl_display = make_egl_display(P.display);
  if (P.egl_display == EGL_NO_DISPLAY)
    fatal_error("Nessun display EGL per la connessione Wayland.");
  debugPrintf("window: %dx%d rendered, shown at %dx%d%s%s\n", P.w, P.h, P.out_w, P.out_h,
              P.viewport ? " (scaled by the compositor)" : "",
              P.fullscreen ? ", fullscreen" : ", fullscreen once it maps");

  input_init();
  return 0;
}

void platform_shutdown(void) {
  if (P.pad)
    SDL_GameControllerClose(P.pad);
  if (P.sdl_ok)
    SDL_Quit();
  if (P.egl_window)
    wl_egl_window_destroy(P.egl_window);
  if (P.viewport)
    wp_viewport_destroy(P.viewport);
  if (P.toplevel)
    xdg_toplevel_destroy(P.toplevel);
  if (P.xdg_surface)
    xdg_surface_destroy(P.xdg_surface);
  if (P.surface)
    wl_surface_destroy(P.surface);
  if (P.viewporter)
    wp_viewporter_destroy(P.viewporter);
  if (P.output)
    wl_output_destroy(P.output);
  if (P.wm_base)
    xdg_wm_base_destroy(P.wm_base);
  if (P.compositor)
    wl_compositor_destroy(P.compositor);
  if (P.registry)
    wl_registry_destroy(P.registry);
  if (P.display)
    wl_display_disconnect(P.display);
  memset(&P, 0, sizeof(P));
}

void platform_window_size(int *w, int *h) {
  pthread_mutex_lock(&size_lock);
  *w = P.w;
  *h = P.h;
  pthread_mutex_unlock(&size_lock);
}

void platform_output_size(int *w, int *h) {
  pthread_mutex_lock(&size_lock);
  *w = P.out_w;
  *h = P.out_h;
  pthread_mutex_unlock(&size_lock);
}

int platform_scaled(void) {
  pthread_mutex_lock(&size_lock);
  const int scaled = P.viewport != NULL && (P.w != P.out_w || P.h != P.out_h);
  pthread_mutex_unlock(&size_lock);
  return scaled;
}
int platform_fullscreen(void) { return P.fullscreen; }

void *platform_anative_window(void) { return &P; }
EGLNativeWindowType platform_egl_window(void) { return (EGLNativeWindowType)P.egl_window; }
EGLDisplay platform_egl_display(void) { return P.egl_display; }

/* ANativeWindow_setBuffersGeometry: the game picks its buffer size (0 x 0 =
 * the window's own, the render size chosen at start); the next frame comes
 * in that size and is scaled to the output */
void platform_resize_buffers(int w, int h) {
  pthread_mutex_lock(&size_lock);
  if (w <= 0 || h <= 0) {
    w = P.base_w;
    h = P.base_h;
  }
  if (P.egl_window && (w != P.w || h != P.h)) {
    debugPrintf("window buffers: %dx%d -> %dx%d\n", P.w, P.h, w, h);
    wl_egl_window_resize(P.egl_window, w, h, 0, 0);
    P.w = w;
    P.h = h;
    update_viewport();
  }
  pthread_mutex_unlock(&size_lock);
}

int platform_wayland_pump(struct wl_display *d) {
  /* a dead connection keeps prepare_read failing while events stay queued,
   * and dispatch_pending then fails without consuming them: stop there */
  while (wl_display_prepare_read(d) != 0)
    if (wl_display_dispatch_pending(d) < 0)
      return -1;
  wl_display_flush(d);
  struct pollfd pfd = { wl_display_get_fd(d), POLLIN, 0 };
  if (poll(&pfd, 1, 0) > 0)
    wl_display_read_events(d);
  else
    wl_display_cancel_read(d);
  return wl_display_dispatch_pending(d) < 0 ? -1 : 0;
}

void platform_pump(void) {
  if (P.display && !P.lost && platform_wayland_pump(P.display) < 0) {
    debugPrintf("window: Wayland connection lost (%s)\n", strerror(errno));
    P.lost = 1;
    P.quit = 1;
  }
  if (!P.sdl_ok)
    return;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        P.quit = 1;
        break;
      case SDL_CONTROLLERDEVICEADDED:
        open_pad(e.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        if (P.pad && e.cdevice.which == P.pad_id) {
          SDL_GameControllerClose(P.pad);
          P.pad = NULL;
          for (int i = 0; i < SDL_NumJoysticks(); i++)
            open_pad(i);
        }
        break;
      case SDL_JOYDEVICEADDED:
        log_unmapped_joystick(e.jdevice.which);
        break;
      default:
        break;
    }
  }
}

int platform_quit_requested(void) { return P.quit; }
void platform_request_quit(void) { P.quit = 1; }

static void deadzone(float *x, float *y) {
  const float dz = (float)pconfig.stick_deadzone / 100.0f;
  const float mag = sqrtf(*x * *x + *y * *y);
  if (mag <= dz) {
    *x = *y = 0.0f;
    return;
  }
  /* rescale so the output still spans 0..1 outside the dead zone */
  const float scale = fminf(1.0f, (mag - dz) / (1.0f - dz)) / mag;
  *x *= scale;
  *y *= scale;
}

int platform_pad(PadState *s) {
  memset(s, 0, sizeof(*s));
  if (!P.pad)
    return 0;
  static const struct {
    SDL_GameControllerButton b;
    uint32_t bit;
  } map[] = {
    { SDL_CONTROLLER_BUTTON_A, PAD_A },
    { SDL_CONTROLLER_BUTTON_B, PAD_B },
    { SDL_CONTROLLER_BUTTON_X, PAD_X },
    { SDL_CONTROLLER_BUTTON_Y, PAD_Y },
    { SDL_CONTROLLER_BUTTON_BACK, PAD_BACK },
    { SDL_CONTROLLER_BUTTON_GUIDE, PAD_GUIDE },
    { SDL_CONTROLLER_BUTTON_START, PAD_START },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_L3 },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_R3 },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_L1 },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_R1 },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT },
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (SDL_GameControllerGetButton(P.pad, map[i].b))
      s->buttons |= map[i].bit;

  const float k = 1.0f / 32767.0f;
  s->lx = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_LEFTX) * k;
  s->ly = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_LEFTY) * k;
  s->rx = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_RIGHTX) * k;
  s->ry = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_RIGHTY) * k;
  deadzone(&s->lx, &s->ly);
  deadzone(&s->rx, &s->ry);
  s->lt = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) * k;
  s->rt = SDL_GameControllerGetAxis(P.pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) * k;
  if (s->lt > 0.5f)
    s->buttons |= PAD_L2;
  if (s->rt > 0.5f)
    s->buttons |= PAD_R2;
  return 1;
}
