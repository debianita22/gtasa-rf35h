/* test_platform.c -- the platform layer on a real Wayland compositor
 *
 * Runs natively under a headless weston (tests/test_platform.sh) and drives
 * the platform code the way the game does:
 *
 *   pacing    the 30 fps pacer holds its cadence and does not rush after a
 *             stall
 *   window    SDL2 fullscreen window on Wayland, wl_egl_window of the output
 *             size, EGL display for SDL's wl_display
 *   egl       from a second thread (the engine's RenderQueue): the game's
 *             EGL sequence through egl_wrap, with EGL_DEFAULT_DISPLAY and the
 *             ANativeWindow token swapped for the platform's; redundant
 *             eglMakeCurrent dropped; the configured swap interval applied
 *             (and enforced over the game's own request): vsync 1 paces to
 *             the compositor, vsync 0 does not
 *   input     an SDL virtual gamepad through platform_pad and gamepad_update:
 *             button numbering, START/SELECT edges, swap_ab, dead zone,
 *             triggers, the exit combination
 *   quit      SDL_QUIT reaches platform_quit_requested
 *   dead connection  the pump returns when the connection dies with an
 *             event still queued (no compositor needed)
 *
 * "scaled" mode (also run under a headless sway with a rotated output whose
 * size is not 640x480, OUT_W x OUT_H): the starting size taken from the
 * output when the compositor's first configure is 0x0, render_scale and
 * setBuffersGeometry checked on screenshots, fullscreen once mapped.
 */

#define _GNU_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#include <SDL2/SDL.h>
#include <wayland-client.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "egl_wrap.h"
#include "gamenative.h"
#include "gl_wrap.h"
#include "jni_fake.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_util.h"
#include "util.h"

/* defined in gamenative.c, read by the gtasa_nx hooks */
extern volatile int g_escape_pressed, g_select_pressed, g_r3_down;
extern volatile float g_right_stick_y;

/* ---- what the rest of the program would provide ------------------------------- */

void *fake_env;
int jni_pop_callback(JniCallback *out) { (void)out; return 0; }
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  (void)mod; (void)symbol;
  return 0;
}
uintptr_t imports_lookup(const char *name) { (void)name; return 0; }
void hard_exit(void) { _exit(0); }

/* count what egl_wrap really sends to EGL (-Wl,--wrap) */
static int real_make_current, real_swap_interval, last_interval = -1;
EGLBoolean __real_eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean __wrap_eglMakeCurrent(EGLDisplay d, EGLSurface w, EGLSurface r, EGLContext c) {
  __atomic_fetch_add(&real_make_current, 1, __ATOMIC_RELAXED);
  return __real_eglMakeCurrent(d, w, r, c);
}
EGLBoolean __real_eglSwapInterval(EGLDisplay, EGLint);
EGLBoolean __wrap_eglSwapInterval(EGLDisplay d, EGLint i) {
  __atomic_fetch_add(&real_swap_interval, 1, __ATOMIC_RELAXED);
  last_interval = i;
  return __real_eglSwapInterval(d, i);
}

static int failures;
#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);            \
      printf(__VA_ARGS__);                                     \
      printf("\n");                                            \
      failures++;                                              \
    }                                                          \
  } while (0)

static void report(const char *name, int before) {
  printf("%-8s %s\n", name, failures == before ? "ok" : "FAIL");
}

/* ---- pacing -------------------------------------------------------------------- */

static void sleep_ns(uint64_t ns) {
  struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
  nanosleep(&ts, NULL);
}

static void test_pacing(void) {
  const int before = failures;
  static volatile sig_atomic_t stop;
  const uint64_t frame = 33333333ull;
  Pacer p;

  /* light, varying work: the period must be the frame time */
  pacer_init(&p, frame);
  uint64_t t0 = now_ns();
  for (int i = 0; i < 30; i++) {
    sleep_ns((uint64_t)(rand() % 20) * 1000000ull);
    pacer_wait(&p, &stop);
  }
  const double avg = (double)(now_ns() - t0) / 30.0 / 1e6;
  CHECK(fabs(avg - 33.33) < 1.5, "30 fps cadence: average frame %.2f ms", avg);

  /* one 100 ms stall: the next frames keep the full period (no burst) */
  sleep_ns(100000000ull);
  pacer_wait(&p, &stop);
  uint64_t prev = now_ns();
  double shortest = 1e9;
  for (int i = 0; i < 5; i++) {
    pacer_wait(&p, &stop);
    const uint64_t t = now_ns();
    shortest = fmin(shortest, (double)(t - prev) / 1e6);
    prev = t;
  }
  CHECK(shortest > 30.0, "after a stall the pacer rushed: a frame of %.2f ms", shortest);

  /* a small overrun is absorbed by the next frame: the cadence keeps its phase */
  pacer_init(&p, frame);
  const uint64_t start = now_ns();
  for (int i = 0; i < 10; i++) {
    sleep_ns(i == 3 ? 40000000ull : 5000000ull); /* frame 3 overruns by ~7 ms */
    pacer_wait(&p, &stop);
  }
  const double total = (double)(now_ns() - start) / 1e6;
  CHECK(fabs(total - 333.3) < 6.0, "10 frames with one small overrun took %.1f ms", total);
  report("pacing", before);
}

/* ---- temporary sysfs settings ------------------------------------------------------- */

static void put(const char *path, const char *v) {
  FILE *f = fopen(path, "w");
  fputs(v, f);
  fclose(f);
}

static int holds(const char *path, const char *v) {
  char buf[64] = "";
  return read_line(path, buf, sizeof(buf)) == 0 && !strcmp(buf, v);
}

static void test_tuning(void) {
  const int before = failures;
  const char *gov = "tests/build/native/fake_governor", *prof = "tests/build/native/fake_profiling";
  put(gov, "ondemand\n");
  put(prof, "0\n");
  CHECK(tune_set(TUNE_PERF, gov, "performance") == 0 && holds(gov, "performance"), "governor not set");
  CHECK(tune_set(TUNE_PERF, gov, "performance") == 0, "setting the same value again failed");
  CHECK(tune_set(TUNE_PERF, gov, "userspace") == 0 && holds(gov, "userspace"), "second value not set");
  CHECK(tune_set(TUNE_STATS, prof, "1") == 0 && holds(prof, "1"), "profiling switch not set");
  CHECK(tune_set(TUNE_PERF, "tests/build/native/missing", "1") < 0, "a missing file was accepted");
  /* perf_mode 1: the boot is over, governors go back, the stats switch stays */
  pconfig.perf_mode = 1;
  cpu_boost(0);
  CHECK(holds(gov, "ondemand"), "perf_mode 1 did not restore the governor found at start");
  CHECK(holds(prof, "1"), "the end of the boot restored the stats switch too");
  /* perf_mode 2 keeps the governor for the whole session */
  pconfig.perf_mode = 2;
  CHECK(tune_set(TUNE_PERF, gov, "performance") == 0, "governor not set again");
  cpu_boost(0);
  CHECK(holds(gov, "performance"), "perf_mode 2 restored the governor before the exit");
  /* every way out puts everything back, with the value found first */
  tune_restore_all();
  CHECK(holds(gov, "ondemand") && holds(prof, "0"), "tune_restore_all left values behind");
  report("tuning", before);
}

/* ---- window + EGL from the engine's render thread ------------------------------------ */

typedef struct {
  int frames;
  double vsync_ms, free_ms;
  int surf_w, surf_h;
  unsigned char pixel[4];
  int make_current_real, make_current_calls;
  int interval_forced;
  int done;
} RenderResult;

static RenderResult rr;

static double run_frames(EGLDisplay dpy, EGLSurface surf, int n, float r, float g, float b) {
  const uint64_t t0 = now_ns();
  for (int i = 0; i < n; i++) {
    glw_glViewport(0, 0, rr.surf_w, rr.surf_h);
    glClearColor(r, g, b, 1.0f);
    glw_glClear(GL_COLOR_BUFFER_BIT);
    if (i == n - 1)
      glReadPixels(rr.surf_w / 2, rr.surf_h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rr.pixel);
    eglw_SwapBuffers(dpy, surf);
  }
  return (double)(now_ns() - t0) / n / 1e6;
}

static void *render_thread(void *arg) {
  (void)arg;
  /* what the engine does on Android, through the bindings the import table gives it */
  EGLDisplay dpy = eglw_GetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (!eglw_Initialize(dpy, &major, &minor)) {
    printf("  FAIL eglInitialize: %#x\n", eglGetError());
    failures++;
    rr.done = 1;
    return NULL;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint cfg_attr[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_DEPTH_SIZE, 16, EGL_NONE,
  };
  EGLConfig cfg;
  EGLint n = 0;
  eglw_ChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
  CHECK(n == 1, "no EGL config for a GLES2 window");
  EGLSurface surf = eglw_CreateWindowSurface(dpy, cfg, (EGLNativeWindowType)platform_anative_window(), NULL);
  CHECK(surf != EGL_NO_SURFACE, "eglCreateWindowSurface on the ANativeWindow token failed");
  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext ctx = eglw_CreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext failed");
  if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
    rr.done = 1;
    return NULL;
  }

  /* the engine re-binds the same context over and over */
  const int real0 = real_make_current;
  for (int i = 0; i < 50; i++)
    eglw_MakeCurrent(dpy, surf, surf, ctx);
  rr.make_current_calls = 50;
  rr.make_current_real = real_make_current - real0;

  eglQuerySurface(dpy, surf, EGL_WIDTH, &rr.surf_w);
  eglQuerySurface(dpy, surf, EGL_HEIGHT, &rr.surf_h);

  /* vsync 1 (the default) was applied on the first bind */
  rr.vsync_ms = run_frames(dpy, surf, 60, 0.0f, 1.0f, 0.0f);

  /* vsync 0 in the config wins over the game's request for 1 */
  pconfig.vsync = 0;
  eglw_SwapInterval(dpy, 1);
  rr.interval_forced = last_interval == 0;
  rr.free_ms = run_frames(dpy, surf, 60, 1.0f, 0.0f, 1.0f);
  rr.frames = 120;

  eglw_MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglw_DestroySurface(dpy, surf);
  eglw_DestroyContext(dpy, ctx);
  __atomic_store_n(&rr.done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void test_window_and_egl(void) {
  int before = failures;
  int w = 0, h = 0, ow = 0, oh = 0;
  platform_window_size(&w, &h);
  platform_output_size(&ow, &oh);
  CHECK(ow == 640 && oh == 480, "fullscreen window shown at %dx%d, expected the 640x480 output", ow, oh);
  CHECK(w == 640 && h == 480, "render size %dx%d at render_scale 100", w, h);
  CHECK(!platform_scaled(), "scaling active at render_scale 100");
  CHECK(platform_fullscreen(), "the compositor did not make the window fullscreen");
  CHECK(platform_egl_display() != EGL_NO_DISPLAY, "no EGL display");
  CHECK(platform_egl_window() != 0, "no wl_egl_window");
  report("window", before);

  before = failures;
  pthread_t t;
  pthread_create(&t, NULL, render_thread, NULL);
  /* the main thread keeps pumping events meanwhile, as the game loop does */
  const uint64_t deadline = now_ns() + 30ull * 1000000000ull;
  while (!__atomic_load_n(&rr.done, __ATOMIC_ACQUIRE) && now_ns() < deadline) {
    platform_pump();
    sleep_ns(5000000ull);
  }
  pthread_join(t, NULL);
  CHECK(rr.surf_w == w && rr.surf_h == h, "EGL surface %dx%d, window %dx%d", rr.surf_w, rr.surf_h, w, h);
  CHECK(rr.make_current_real == 1, "%d eglMakeCurrent calls reached EGL for %d identical binds",
        rr.make_current_real, rr.make_current_calls);
  CHECK(rr.pixel[0] == 255 && rr.pixel[1] == 0 && rr.pixel[2] == 255,
        "read back %u,%u,%u instead of the clear colour", rr.pixel[0], rr.pixel[1], rr.pixel[2]);
  CHECK(rr.vsync_ms > 12.0 && rr.vsync_ms < 40.0, "vsync 1: %.2f ms per frame, expected the "
        "compositor's ~16.7", rr.vsync_ms);
  CHECK(rr.free_ms < rr.vsync_ms / 2, "vsync 0: %.2f ms per frame, not faster than vsync 1 (%.2f)",
        rr.free_ms, rr.vsync_ms);
  CHECK(rr.interval_forced, "the game's eglSwapInterval(1) was not overridden by vsync 0");
  printf("         %dx%d surface, vsync 1: %.1f ms/frame, vsync 0: %.1f ms/frame, "
         "%d of %d MakeCurrent reached EGL\n", rr.surf_w, rr.surf_h, rr.vsync_ms, rr.free_ms,
         rr.make_current_real, rr.make_current_calls);
  report("egl", before);
}

/* ---- input ------------------------------------------------------------------------- */

static int downs[16], ups[16], axes_calls;
static float axes[6];
static void fake_connected(void *env, void *thiz, int pad) { (void)env; (void)thiz; (void)pad; }
static void fake_down(void *env, void *thiz, int pad, int b) {
  (void)env; (void)thiz; (void)pad;
  if (b >= 0 && b < 16) downs[b]++;
}
static void fake_up(void *env, void *thiz, int pad, int b) {
  (void)env; (void)thiz; (void)pad;
  if (b >= 0 && b < 16) ups[b]++;
}
static void fake_axes(void *env, void *thiz, int pad, float lx, float ly, float rx, float ry,
                      float lt, float rt) {
  (void)env; (void)thiz; (void)pad;
  axes_calls++;
  axes[0] = lx, axes[1] = ly, axes[2] = rx, axes[3] = ry, axes[4] = lt, axes[5] = rt;
}

static SDL_Joystick *vjoy;

static void settle(void) {
  for (int i = 0; i < 3; i++) {
    platform_pump();
    sleep_ns(2000000ull);
  }
}

/* press one SDL button, run a game frame, release, run a frame */
static int tap(SDL_GameControllerButton b) {
  memset(downs, 0, sizeof(downs));
  memset(ups, 0, sizeof(ups));
  SDL_JoystickSetVirtualButton(vjoy, b, 1);
  settle();
  gamepad_update();
  int pressed = -1;
  for (int i = 0; i < 16; i++)
    if (downs[i])
      pressed = pressed == -1 ? i : 100; /* 100: more than one */
  SDL_JoystickSetVirtualButton(vjoy, b, 0);
  settle();
  gamepad_update();
  if (pressed >= 0 && pressed < 16 && ups[pressed] != 1)
    return 200; /* no matching release */
  return pressed;
}

static void test_input(void) {
  const int before = failures;
  gn.onGamepadConnected = fake_connected;
  gn.onGamepadButtonDown = fake_down;
  gn.onGamepadButtonUp = fake_up;
  gn.onGamepadAxesChanged = fake_axes;

  SDL_VirtualJoystickDesc desc;
  SDL_zero(desc);
  desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  desc.naxes = SDL_CONTROLLER_AXIS_MAX;
  desc.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
  desc.button_mask = (1u << desc.nbuttons) - 1;
  desc.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
  desc.name = "test pad";
  const int index = SDL_JoystickAttachVirtualEx(&desc);
  CHECK(index >= 0, "virtual pad: %s", SDL_GetError());
  if (index < 0)
    return;
  vjoy = SDL_JoystickOpen(index);
  /* a virtual trigger spans the full axis range: -32768 is released */
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
  settle();
  PadState s;
  CHECK(platform_pad(&s) == 1, "the virtual gamepad was not opened as the pad");

  static const struct {
    SDL_GameControllerButton b;
    int engine;
    const char *name;
  } expect[] = {
    { SDL_CONTROLLER_BUTTON_A, 0, "A -> CROSS" },
    { SDL_CONTROLLER_BUTTON_B, 1, "B -> CIRCLE" },
    { SDL_CONTROLLER_BUTTON_X, 2, "X -> SQUARE" },
    { SDL_CONTROLLER_BUTTON_Y, 3, "Y -> TRIANGLE" },
    { SDL_CONTROLLER_BUTTON_START, 4, "START" },
    { SDL_CONTROLLER_BUTTON_BACK, 5, "SELECT" },
    { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 6, "L1" },
    { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 7, "R1" },
    { SDL_CONTROLLER_BUTTON_DPAD_UP, 8, "DPAD_UP" },
    { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 9, "DPAD_DOWN" },
    { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 10, "DPAD_LEFT" },
    { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 11, "DPAD_RIGHT" },
    { SDL_CONTROLLER_BUTTON_LEFTSTICK, 12, "L3" },
    { SDL_CONTROLLER_BUTTON_RIGHTSTICK, 13, "R3" },
  };
  for (size_t i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
    g_escape_pressed = g_select_pressed = 0;
    const int got = tap(expect[i].b);
    CHECK(got == expect[i].engine, "%s: engine button %d", expect[i].name, got);
    if (expect[i].b == SDL_CONTROLLER_BUTTON_START)
      CHECK(g_escape_pressed == 1, "START did not raise the pause edge");
    else if (expect[i].b == SDL_CONTROLLER_BUTTON_BACK)
      CHECK(g_select_pressed == 1, "SELECT did not raise the map edge");
    else
      CHECK(!g_escape_pressed && !g_select_pressed, "%s raised a menu edge", expect[i].name);
  }

  /* R3 held is published for the hydraulics hook */
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_RIGHTSTICK, 1);
  settle();
  gamepad_update();
  CHECK(g_r3_down == 1, "g_r3_down not set while R3 is held");
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0);
  settle();
  gamepad_update();
  CHECK(g_r3_down == 0, "g_r3_down still set after release");

  /* swap_ab for pads labelled the Nintendo way */
  pconfig.swap_ab = 1;
  CHECK(tap(SDL_CONTROLLER_BUTTON_A) == 1, "swap_ab: A should send CIRCLE");
  CHECK(tap(SDL_CONTROLLER_BUTTON_B) == 0, "swap_ab: B should send CROSS");
  pconfig.swap_ab = 0;

  /* dead zone (12 %): a small deflection is nothing, a large one is rescaled */
  pconfig.stick_deadzone = 12;
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_LEFTX, 3000); /* 9 % */
  settle();
  platform_pad(&s);
  CHECK(s.lx == 0.0f, "9%% deflection inside the dead zone gave %.3f", s.lx);
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_LEFTX, 16384); /* 50 % */
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 16384); /* 3/4 pulled */
  settle();
  axes_calls = 0;
  gamepad_update();
  const float want = (16384.0f / 32767.0f - 0.12f) / 0.88f;
  CHECK(axes_calls == 1, "axes not reported once on change (%d)", axes_calls);
  CHECK(fabsf(axes[0] - want) < 0.01f, "lx %.3f, expected %.3f", axes[0], want);
  CHECK(fabsf(axes[3] + 1.0f) < 0.01f, "ry %.3f, expected -1 (up)", axes[3]);
  CHECK(fabsf(axes[5] - 0.75f) < 0.01f, "rt %.3f, expected 0.75", axes[5]);
  CHECK(axes[4] == 0.0f, "lt %.3f, expected 0 (released)", axes[4]);
  CHECK(fabsf(g_right_stick_y + 1.0f) < 0.01f, "g_right_stick_y %.3f", g_right_stick_y);
  platform_pad(&s);
  CHECK(s.buttons & PAD_R2, "right trigger past half did not set PAD_R2");
  axes_calls = 0;
  gamepad_update();
  CHECK(axes_calls == 0, "unchanged axes reported again");
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_LEFTX, 0);
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_RIGHTY, 0);
  SDL_JoystickSetVirtualAxis(vjoy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
  settle();
  gamepad_update();

  /* SELECT+START held for a second quits; shorter does not */
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_BACK, 1);
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_START, 1);
  settle();
  int quit_early = gamepad_update();
  sleep_ns(500000000ull);
  settle();
  quit_early |= gamepad_update();
  sleep_ns(600000000ull);
  settle();
  const int quit_late = gamepad_update();
  CHECK(!quit_early, "exit combination fired before a second");
  CHECK(quit_late, "exit combination held 1.1 s did not fire");
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_BACK, 0);
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_START, 0);
  settle();
  gamepad_update();

  /* pad unplugged: nothing stays held */
  SDL_JoystickSetVirtualButton(vjoy, SDL_CONTROLLER_BUTTON_A, 1);
  settle();
  gamepad_update();
  memset(ups, 0, sizeof(ups));
  SDL_JoystickClose(vjoy);
  SDL_JoystickDetachVirtual(index);
  settle();
  gamepad_update();
  CHECK(ups[0] == 1, "unplugging the pad did not release CROSS");
  CHECK(platform_pad(&s) == 0, "a pad is still reported after unplugging");
  report("input", before);
}

/* ---- quit ---------------------------------------------------------------------------- */

static void test_quit(void) {
  const int before = failures;
  CHECK(!platform_quit_requested(), "quit requested before any SDL_QUIT");
  SDL_Event e;
  SDL_zero(e);
  e.type = SDL_QUIT;
  SDL_PushEvent(&e);
  platform_pump();
  CHECK(platform_quit_requested(), "SDL_QUIT did not request a quit");
  report("quit", before);
}

static int log_has(const char *needle) {
  char line[512];
  int found = 0;
  FILE *f = fopen("tests/build/native/test_platform.log", "r");
  while (f && !found && fgets(line, sizeof(line), f))
    found = strstr(line, needle) != NULL;
  if (f)
    fclose(f);
  return found;
}

/* ---- a connection that dies with an event still queued ----------------------------------- */

static void sync_done(void *d, struct wl_callback *cb, uint32_t t) {
  (void)d; (void)t;
  wl_callback_destroy(cb);
}
static const struct wl_callback_listener sync_listener = { sync_done };

/* Mesa's thread reads the socket on its own queue: an event for the
 * default queue can be waiting there when the connection dies */
static void test_dead_connection(void) {
  const int before = failures;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
    CHECK(0, "socketpair");
    return;
  }
  struct wl_display *d = wl_display_connect_to_fd(sv[0]);
  struct wl_callback *cb = wl_display_sync(d); /* object 2, default queue */
  wl_callback_add_listener(cb, &sync_listener, NULL);
  wl_display_flush(d);
  char junk[64];
  CHECK(read(sv[1], junk, sizeof(junk)) > 0, "the sync request did not arrive");
  const uint32_t done_event[3] = { 2, (12u << 16) | 0, 123 }; /* wl_callback.done */
  CHECK(write(sv[1], done_event, sizeof(done_event)) == sizeof(done_event), "write");
  struct wl_event_queue *q = wl_display_create_queue(d);
  wl_display_prepare_read_queue(d, q);
  wl_display_read_events(d); /* 'done' is now queued on the default queue */
  close(sv[1]);              /* the compositor goes away */
  wl_display_prepare_read_queue(d, q);
  wl_display_read_events(d);
  alarm(5); /* a pump that never returns ends the test here */
  const int r = platform_wayland_pump(d);
  alarm(0);
  CHECK(r < 0, "the pump returned %d on a dead connection", r);
  wl_event_queue_destroy(q);
  wl_display_disconnect(d);
  report("dead connection", before);
}

/* ---- scaled rendering (render_scale, setBuffersGeometry) ---------------------------------- */

static volatile int scaled_stop;
static volatile int scaled_w, scaled_h; /* EGL surface size seen by the render thread */

static void *scaled_render_thread(void *arg) {
  (void)arg;
  EGLDisplay dpy = eglw_GetDisplay(EGL_DEFAULT_DISPLAY);
  eglw_Initialize(dpy, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);
  static const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                                     EGL_OPENGL_ES2_BIT, EGL_NONE };
  EGLConfig cfg;
  EGLint n = 0;
  eglw_ChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
  EGLSurface surf = eglw_CreateWindowSurface(dpy, cfg, (EGLNativeWindowType)platform_anative_window(), NULL);
  static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext ctx = eglw_CreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  eglw_MakeCurrent(dpy, surf, surf, ctx);
  while (!scaled_stop) {
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    glw_glViewport(0, 0, w, h);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glw_glClear(GL_COLOR_BUFFER_BIT);
    eglw_SwapBuffers(dpy, surf);
    scaled_w = w;
    scaled_h = h;
  }
  eglw_MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return NULL;
}

static void pump_for(uint64_t ns) {
  const uint64_t end = now_ns() + ns;
  while (now_ns() < end) {
    platform_pump();
    sleep_ns(5000000ull);
  }
}

/* the whole output must show the frame: screenshot through the compositor
 * (grim under sway, weston-screenshooter under weston) */
static int screen_is_magenta(const char *tag) {
  char cmd[512];
  if (getenv("SWAYSOCK"))
    snprintf(cmd, sizeof(cmd),
             "cd tests/build/native && grim shot-sway-%s.png && "
             "python3 ../../check_screenshot.py shot-sway-%s.png 255,0,255", tag, tag);
  else
    snprintf(cmd, sizeof(cmd),
             "cd tests/build/native && rm -f wayland-screenshot*.png && weston-screenshooter >/dev/null 2>&1 && "
             "mv wayland-screenshot*.png shot-%s.png && python3 ../../check_screenshot.py shot-%s.png 255,0,255",
             tag, tag);
  return system(cmd) == 0;
}

static int env_int(const char *name, int def) {
  const char *v = getenv(name);
  return v && *v ? atoi(v) : def;
}

static int run_scaled(void) {
  /* the output's size in surface coordinates, as the compositor runs it */
  const int out_w = env_int("OUT_W", 640), out_h = env_int("OUT_H", 480);
  const int sw = (out_w * 75 / 100) & ~1, sh = (out_h * 75 / 100) & ~1;
  pconfig.render_scale = 75;
  platform_init("gtasa scaling test", 0, 0);
  int before = failures;
  int w, h, ow, oh;
  platform_window_size(&w, &h);
  platform_output_size(&ow, &oh);
  CHECK(ow == out_w && oh == out_h, "starting output size %dx%d, expected %dx%d", ow, oh, out_w, out_h);
  CHECK(w == sw && h == sh, "render_scale 75 gave a %dx%d buffer, expected %dx%d", w, h, sw, sh);
  CHECK(platform_scaled(), "no compositor scaling at render_scale 75");

  pthread_t t;
  pthread_create(&t, NULL, scaled_render_thread, NULL);
  pump_for(800000000ull);
  CHECK(platform_fullscreen(), "the window is not fullscreen once mapped");
  platform_output_size(&ow, &oh);
  CHECK(ow == out_w && oh == out_h, "output size %dx%d once mapped, expected %dx%d", ow, oh, out_w, out_h);
  CHECK(scaled_w == sw && scaled_h == sh, "EGL surface %dx%d, expected %dx%d", scaled_w, scaled_h, sw, sh);
  CHECK(screen_is_magenta("scale75"), "the %dx%d frame does not fill the output", sw, sh);
  report("scale", before);

  /* the game's ANativeWindow_setBuffersGeometry: smaller, back to the
   * window's own size (0 x 0 keeps render_scale), then the output's */
  before = failures;
  platform_resize_buffers(320, 240);
  pump_for(500000000ull);
  CHECK(scaled_w == 320 && scaled_h == 240, "after setBuffersGeometry(320, 240) the surface is %dx%d",
        scaled_w, scaled_h);
  CHECK(platform_scaled(), "320x240 buffer not scaled");
  CHECK(screen_is_magenta("geometry320"), "the 320x240 frame does not fill the output");
  platform_resize_buffers(0, 0);
  pump_for(500000000ull);
  CHECK(scaled_w == sw && scaled_h == sh, "after setBuffersGeometry(0, 0) the surface is %dx%d, expected %dx%d",
        scaled_w, scaled_h, sw, sh);
  CHECK(platform_scaled(), "setBuffersGeometry(0, 0) dropped render_scale");
  CHECK(screen_is_magenta("geometry0"), "the %dx%d frame does not fill the output", sw, sh);
  platform_resize_buffers(out_w, out_h);
  pump_for(500000000ull);
  CHECK(scaled_w == out_w && scaled_h == out_h, "after setBuffersGeometry(%d, %d) the surface is %dx%d",
        out_w, out_h, scaled_w, scaled_h);
  CHECK(!platform_scaled(), "scaling left on at the output size");
  CHECK(screen_is_magenta("native"), "the native frame does not fill the output");
  report("geometry", before);

  scaled_stop = 1;
  pthread_join(t, NULL);
  platform_shutdown();
  printf("scaling: %s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}

int main(int argc, char **argv) {
  log_open("tests/build/native/test_platform.log");
  pconfig_defaults();
  if (argc > 1 && !strcmp(argv[1], "scaled"))
    return run_scaled();
  pconfig.vsync = 1;
  glw_configure(1, 1);

  test_pacing();
  test_tuning();
  test_dead_connection();
  platform_init("gtasa platform test", 0, 0);
  CHECK(log_has("window: output 640x480 at "), "the output's mode was not read (wl_output)");
  test_window_and_egl();
  test_input();
  test_quit();
  platform_shutdown();
  printf("platform: %s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
