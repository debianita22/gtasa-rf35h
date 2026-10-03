/* egl_wrap.c -- the game's own EGL usage, pointed at the platform window
 *
 * The game asks for EGL_DEFAULT_DISPLAY and passes the ANativeWindow token
 * it got from ANativeWindow_fromSurface; both are swapped for the Wayland
 * display and wl_egl_window of the platform. Everything else goes to Mesa,
 * with three additions: the redundant-eglMakeCurrent filter from gtasa_nx,
 * the configured swap interval, and placing the threads Mesa creates for the
 * game's context (the glthread worker) on the driver CPU set.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <EGL/egl.h>

#include "egl_wrap.h"
#include "gl_wrap.h"
#include "import_table.h"
#include "overlay.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_util.h"
#include "stats.h"
#include "util.h"

/* ---- threads Mesa spawns ---------------------------------------------------- */

#define MAX_TASKS 256

static int list_tasks(int *tids) {
  int n = 0;
  DIR *d = opendir("/proc/self/task");
  if (!d)
    return 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < MAX_TASKS)
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
      tids[n++] = atoi(e->d_name);
  closedir(d);
  return n;
}

static void claim_new_threads(const int *before, int nbefore) {
  int after[MAX_TASKS];
  const int nafter = list_tasks(after);
  for (int i = 0; i < nafter; i++) {
    int known = 0;
    for (int j = 0; j < nbefore && !known; j++)
      known = after[i] == before[j];
    if (known)
      continue;
    char path[64], comm[32] = "?";
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", after[i]);
    read_line(path, comm, sizeof(comm));
    debugPrintf("egl: driver thread %d (%s)\n", after[i], comm);
    pin_thread(after[i], CORE_DRIVER);
    char role[48];
    snprintf(role, sizeof(role), "drv:%s", comm);
    stats_thread_role(after[i], role);
  }
}

/* ---- display, configs, surfaces --------------------------------------------- */

EGLDisplay eglw_GetDisplay(EGLNativeDisplayType native) {
  (void)native; /* EGL_DEFAULT_DISPLAY on Android */
  return platform_egl_display();
}

EGLBoolean eglw_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  int before[MAX_TASKS];
  const int n = list_tasks(before);
  const EGLBoolean ok = eglInitialize(dpy, major, minor);
  if (ok) {
    claim_new_threads(before, n);
    static int logged;
    if (!logged++)
      debugPrintf("egl: %s, %s\n", eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
  }
  return ok;
}

EGLBoolean eglw_Terminate(EGLDisplay dpy) {
  (void)dpy; /* the platform owns the display for the whole run */
  return EGL_TRUE;
}

static void log_config(EGLDisplay dpy, EGLConfig cfg, const char *what) {
  EGLint r = 0, g = 0, b = 0, a = 0, d = 0, s = 0, ms = 0;
  eglGetConfigAttrib(dpy, cfg, EGL_RED_SIZE, &r);
  eglGetConfigAttrib(dpy, cfg, EGL_GREEN_SIZE, &g);
  eglGetConfigAttrib(dpy, cfg, EGL_BLUE_SIZE, &b);
  eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &a);
  eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &d);
  eglGetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &s);
  eglGetConfigAttrib(dpy, cfg, EGL_SAMPLES, &ms);
  debugPrintf("egl: %s config R%dG%dB%dA%d depth %d stencil %d samples %d\n", what, r, g, b, a,
              d, s, ms);
}

EGLBoolean eglw_ChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs,
                             EGLint size, EGLint *num) {
  return eglChooseConfig(dpy, attribs, configs, size, num);
}

EGLSurface eglw_CreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
                                    const EGLint *attribs) {
  if ((void *)win == platform_anative_window() || !win)
    win = platform_egl_window();
  log_config(dpy, cfg, "window surface");
  EGLSurface s = eglCreateWindowSurface(dpy, cfg, win, attribs);
  if (s == EGL_NO_SURFACE)
    debugPrintf("egl: eglCreateWindowSurface failed: %#x\n", eglGetError());
  return s;
}

static void forget_current(EGLSurface surf, EGLContext ctx);

EGLBoolean eglw_DestroySurface(EGLDisplay dpy, EGLSurface surf) {
  debugPrintf("egl: surface destroyed\n");
  forget_current(surf, EGL_NO_CONTEXT);
  return eglDestroySurface(dpy, surf);
}

EGLContext eglw_CreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
                              const EGLint *attribs) {
  int version = 1;
  for (const EGLint *a = attribs; a && *a != EGL_NONE; a += 2)
    if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
      version = a[1];
  int before[MAX_TASKS];
  const int n = list_tasks(before);
  EGLContext ctx = eglCreateContext(dpy, cfg, share, attribs);
  if (ctx == EGL_NO_CONTEXT) {
    debugPrintf("egl: eglCreateContext (GLES %d) failed: %#x\n", version, eglGetError());
    return ctx;
  }
  debugPrintf("egl: GLES %d context%s\n", version, share != EGL_NO_CONTEXT ? " (shared)" : "");
  claim_new_threads(before, n);
  return ctx;
}

EGLBoolean eglw_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  debugPrintf("egl: context destroyed\n");
  forget_current(EGL_NO_SURFACE, ctx);
  return eglDestroyContext(dpy, ctx);
}

/* ---- current context ----------------------------------------------------------- */

/* The engine re-binds the same context and surface many times per frame and
 * Mesa revalidates on each call; per EGL the repeat is a no-op, so drop it. */
static __thread struct {
  int valid;
  EGLDisplay dpy;
  EGLSurface draw, read;
  EGLContext ctx;
} current;

/* the surface our swap interval was last applied to (the interval is
 * per surface, and the game may never set one itself) */
static EGLSurface interval_surface = EGL_NO_SURFACE;

/* a destroyed handle can come back as a new object: never match it again */
static void forget_current(EGLSurface surf, EGLContext ctx) {
  if (surf != EGL_NO_SURFACE && (current.draw == surf || current.read == surf))
    current.valid = 0;
  if (ctx != EGL_NO_CONTEXT && current.ctx == ctx)
    current.valid = 0;
  if (surf != EGL_NO_SURFACE && interval_surface == surf)
    interval_surface = EGL_NO_SURFACE;
}

EGLBoolean eglw_MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  if (current.valid && current.dpy == dpy && current.draw == draw && current.read == read &&
      current.ctx == ctx)
    return EGL_TRUE;
  const EGLBoolean ok = eglMakeCurrent(dpy, draw, read, ctx);
  if (!ok) {
    current.valid = 0;
    return ok;
  }
  glw_reset(); /* new context or surface: nothing cached is known to hold */
  current.valid = 1;
  current.dpy = dpy;
  current.draw = draw;
  current.read = read;
  current.ctx = ctx;
  if (ctx != EGL_NO_CONTEXT) {
    static int logged;
    const int tid = (int)syscall(SYS_gettid);
    stats_thread_role(tid, "render");
    if (draw != EGL_NO_SURFACE && pconfig.vsync >= 0 && draw != interval_surface) {
      eglSwapInterval(dpy, pconfig.vsync);
      interval_surface = draw;
    }
    if (!logged++) {
      debugPrintf("gl: %s | %s | %s\n", (const char *)glGetString(GL_VENDOR),
                  (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
      debugPrintf("gl: extensions: %s\n", (const char *)glGetString(GL_EXTENSIONS));
    }
  }
  return ok;
}

/* ---- presentation ---------------------------------------------------------------- */

EGLBoolean eglw_SwapBuffers(EGLDisplay dpy, EGLSurface surf) {
  const uint64_t t0 = now_ns();
  /* upstream's hook draws the optional FPS counter, then presents */
  const EGLBoolean ok = eglSwapBuffersHook(dpy, surf);
  glw_reset(); /* the counter drew behind the cache */
  stats_present(now_ns() - t0);
  return ok;
}

EGLBoolean eglw_SwapInterval(EGLDisplay dpy, EGLint interval) {
  if (pconfig.vsync >= 0 && interval != pconfig.vsync) {
    static int logged;
    if (!logged++)
      debugPrintf("egl: swap interval %d requested, using %d\n", interval, pconfig.vsync);
    interval = pconfig.vsync;
  }
  return eglSwapInterval(dpy, interval);
}

/* Functions fetched at run time must go through the same wrappers as linked
 * ones, or a bypassed glBindTexture would leave the state cache stale. */
void *eglw_GetProcAddress(const char *name) {
  const uintptr_t mine = imports_lookup(name);
  if (mine)
    return (void *)mine;
  return (void *)eglGetProcAddress(name);
}
