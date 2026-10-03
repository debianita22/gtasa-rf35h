/* egl_wrap.h -- the EGL calls the game makes, bound to the platform window
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_EGL_WRAP_H
#define GTASA_EGL_WRAP_H

#include <EGL/egl.h>

EGLDisplay eglw_GetDisplay(EGLNativeDisplayType native);
EGLBoolean eglw_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor);
EGLBoolean eglw_Terminate(EGLDisplay dpy);
EGLBoolean eglw_ChooseConfig(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs,
                             EGLint size, EGLint *num);
EGLSurface eglw_CreateWindowSurface(EGLDisplay dpy, EGLConfig cfg, EGLNativeWindowType win,
                                    const EGLint *attribs);
EGLBoolean eglw_DestroySurface(EGLDisplay dpy, EGLSurface surf);
EGLContext eglw_CreateContext(EGLDisplay dpy, EGLConfig cfg, EGLContext share,
                              const EGLint *attribs);
EGLBoolean eglw_DestroyContext(EGLDisplay dpy, EGLContext ctx);
EGLBoolean eglw_MakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
EGLBoolean eglw_SwapBuffers(EGLDisplay dpy, EGLSurface surf);
EGLBoolean eglw_SwapInterval(EGLDisplay dpy, EGLint interval);
void *eglw_GetProcAddress(const char *name);

#endif
