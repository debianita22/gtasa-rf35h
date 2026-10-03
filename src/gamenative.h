/* gamenative.h -- the engine's Java_com_rockstargames_oswrapper_GameNative_* entry points
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_GAMENATIVE_H
#define GTASA_GAMENATIVE_H

#include "so_util.h"

typedef struct {
  void (*onActivityCreated)(void *env, void *thiz, void *activity);
  void (*onActivityDestroyed)(void *env, void *thiz);
  void (*onInitialSetup)(void *env, void *thiz, void *activity, void *apk, void *names, void *paths);
  void (*onSurfaceCreated)(void *env, void *thiz);
  void (*onSurfaceChanged)(void *env, void *thiz, void *surface, int w, int h);
  void (*onSurfaceDestroyed)(void *env, void *thiz);
  void (*onDrawFrame)(void *env, void *thiz, float dt);
  void (*onResume)(void *env, void *thiz);
  void (*onPause)(void *env, void *thiz);
  int (*isInitialized)(void *env, void *thiz);
  void (*onTouchStart)(void *env, void *thiz, int id, float x, float y);
  void (*onTouchMove)(void *env, void *thiz, int id, float x, float y);
  void (*onTouchEnd)(void *env, void *thiz, int id, float x, float y);
  void (*onGamepadConnected)(void *env, void *thiz, int pad);
  void (*onGamepadButtonDown)(void *env, void *thiz, int pad, int button);
  void (*onGamepadButtonUp)(void *env, void *thiz, int pad, int button);
  void (*onGamepadAxesChanged)(void *env, void *thiz, int pad, float lx, float ly, float rx,
                               float ry, float lt, float rt);
  void (*onBackButtonPressed)(void *env, void *thiz);
  /* asynchronous completions the engine waits for during boot */
  void (*onPlaylistOpenComplete)(void *env, void *thiz, int success, int count);
  void (*onRockstarInitialComplete)(void *env, void *thiz);
  void (*onRockstarGateComplete)(void *env, void *thiz, int gate, int success);
  void (*onRockstarSignInComplete)(void *env, void *thiz);
  void (*onRockstarSignOutComplete)(void *env, void *thiz);
  void (*onRockstarStateChanged)(void *env, void *thiz, int state);
} GameNative;

extern GameNative gn;

/* Fill `gn` from the loaded game; fatal if a required entry is missing.
 * `strict` 0 only reports (for --check). Returns the number missing. */
int gamenative_resolve(so_module *game, int strict);

/* Fire the completions the fake JNI queued; main thread, once per frame. */
void gamenative_dispatch_callbacks(void);

/* Feed the pad to the engine and publish the globals the hooks read;
 * returns 1 when the exit combination has been held long enough.
 * (onGamepadConnected is sent once at start-up, as gtasa_nx does.) */
int gamepad_update(void);

#endif
