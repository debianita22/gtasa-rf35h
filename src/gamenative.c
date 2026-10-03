/* gamenative.c -- driving the engine through its GameNative JNI entry points
 *
 * Replaces Android's GameActivity: resolves the entry points, fires the
 * asynchronous completions the engine waits for, and feeds the pad. The
 * button numbering, the pause/map edges and the globals the hooks read are
 * gtasa_nx's (main.c); only the input source is new.
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (original main.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gamenative.h"
#include "jni_fake.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_util.h"
#include "util.h"
#include "error.h"

GameNative gn;

/* globals the gtasa_nx hooks (hooks/game.c) read */
volatile int g_escape_pressed = 0;
volatile int g_select_pressed = 0;
volatile float g_right_stick_y = 0.0f;
volatile int g_r3_down = 0;
volatile int g_l3_down = 0;
volatile int g_dpad_down = 0;
volatile int g_l1_down = 0;
volatile int g_r1_down = 0;

#define GN_PREFIX "Java_com_rockstargames_oswrapper_GameNative_"

int gamenative_resolve(so_module *game, int strict) {
  static const struct {
    const char *name;
    size_t offset;
    int required;
  } entries[] = {
#define E(field, sym, req) { GN_PREFIX sym, offsetof(GameNative, field), req }
    E(onActivityCreated, "implOnActivityCreated", 1),
    E(onActivityDestroyed, "implOnActivityDestroyed", 1),
    E(onInitialSetup, "implOnInitialSetup", 1),
    E(onSurfaceCreated, "implOnSurfaceCreated", 1),
    E(onSurfaceChanged, "implOnSurfaceChanged", 1),
    E(onSurfaceDestroyed, "implOnSurfaceDestroyed", 1),
    E(onDrawFrame, "implOnDrawFrame", 1),
    E(onResume, "implOnResume", 1),
    E(onPause, "implOnPause", 1),
    E(isInitialized, "implIsInitialized", 1),
    E(onTouchStart, "implOnTouchStart", 1),
    E(onTouchMove, "implOnTouchMove", 1),
    E(onTouchEnd, "implOnTouchEnd", 1),
    E(onGamepadConnected, "implOnGamepadConnected", 1),
    E(onGamepadButtonDown, "implOnGamepadButtonDown", 1),
    E(onGamepadButtonUp, "implOnGamepadButtonUp", 1),
    E(onGamepadAxesChanged, "implOnGamepadAxesChanged", 1),
    E(onBackButtonPressed, "implOnBackButtonPressed", 1),
    E(onPlaylistOpenComplete, "implOnPlaylistOpenComplete", 0),
    E(onRockstarInitialComplete, "implOnRockstarInitialComplete", 0),
    E(onRockstarGateComplete, "implOnRockstarGateComplete", 0),
    E(onRockstarSignInComplete, "implOnRockstarSignInComplete", 0),
    E(onRockstarSignOutComplete, "implOnRockstarSignOutComplete", 0),
    E(onRockstarStateChanged, "implOnRockstarStateChanged", 0),
#undef E
  };
  int missing = 0;
  for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
    const uintptr_t addr = so_try_find_addr_rx(game, entries[i].name);
    memcpy((uint8_t *)&gn + entries[i].offset, &addr, sizeof(addr));
    if (!addr && entries[i].required) {
      missing++;
      debugPrintf("GameNative: missing %s\n", entries[i].name);
    }
  }
  if (missing && strict)
    fatal_error("This libGame.so lacks %d GameNative entry points:\nnot the 2.11.311 arm64 build?",
                missing);
  return missing;
}

void gamenative_dispatch_callbacks(void) {
  JniCallback cb;
  int n = 0;
  while (n++ < 16 && jni_pop_callback(&cb)) {
    switch (cb.type) {
      case JNI_CB_PLAYLIST_OPEN_COMPLETE:
        if (gn.onPlaylistOpenComplete)
          gn.onPlaylistOpenComplete(fake_env, NULL, cb.arg0, cb.arg1);
        break;
      case JNI_CB_ROCKSTAR_INITIAL_COMPLETE:
        if (gn.onRockstarInitialComplete)
          gn.onRockstarInitialComplete(fake_env, NULL);
        break;
      case JNI_CB_ROCKSTAR_GATE_COMPLETE:
        if (gn.onRockstarGateComplete)
          gn.onRockstarGateComplete(fake_env, NULL, cb.arg0, cb.arg1);
        break;
      case JNI_CB_ROCKSTAR_SIGNIN_COMPLETE:
        if (gn.onRockstarSignInComplete)
          gn.onRockstarSignInComplete(fake_env, NULL);
        break;
      case JNI_CB_ROCKSTAR_SIGNOUT_COMPLETE:
        if (gn.onRockstarSignOutComplete)
          gn.onRockstarSignOutComplete(fake_env, NULL);
        break;
      default:
        break;
    }
  }
}

/* ---- pad --------------------------------------------------------------------- */

/* Engine ButtonIDs (CHIDJoystick numbering, see gtasa_nx main.c):
 * CROSS=0 CIRCLE=1 SQUARE=2 TRIANGLE=3 START=4 SELECT=5 L1=6 R1=7
 * DPAD_UP=8 DPAD_DOWN=9 DPAD_LEFT=10 DPAD_RIGHT=11 L3=12 R3=13.
 * Positions as on the Switch port: bottom face = CROSS, right = CIRCLE,
 * left = SQUARE, top = TRIANGLE. L2/R2 travel as analog triggers. */
static const struct {
  uint32_t pad;
  int button;
} button_map[] = {
  { PAD_A, 0 }, { PAD_B, 1 }, { PAD_X, 2 }, { PAD_Y, 3 },
  { PAD_START, 4 }, { PAD_BACK, 5 }, { PAD_L1, 6 }, { PAD_R1, 7 },
  { PAD_UP, 8 }, { PAD_DOWN, 9 }, { PAD_LEFT, 10 }, { PAD_RIGHT, 11 },
  { PAD_L3, 12 }, { PAD_R3, 13 },
};

static uint32_t swap_bits(uint32_t v, uint32_t a, uint32_t b) {
  const int ha = (v & a) != 0, hb = (v & b) != 0;
  v &= ~(a | b);
  if (ha) v |= b;
  if (hb) v |= a;
  return v;
}

int gamepad_update(void) {
  static uint32_t prev;
  static float prev_axes[6];
  static uint64_t combo_since;

  PadState s;
  platform_pad(&s); /* all zero when no pad: everything held is released below */
  if (pconfig.swap_ab)
    s.buttons = swap_bits(s.buttons, PAD_A, PAD_B);
  if (pconfig.swap_xy)
    s.buttons = swap_bits(s.buttons, PAD_X, PAD_Y);

  const uint32_t changed = s.buttons ^ prev;
  for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
    if (!(changed & button_map[i].pad))
      continue;
    if (s.buttons & button_map[i].pad)
      gn.onGamepadButtonDown(fake_env, NULL, 0, button_map[i].button);
    else
      gn.onGamepadButtonUp(fake_env, NULL, 0, button_map[i].button);
  }
  /* START opens/closes the pause menu, SELECT opens the map (hooks/game.c) */
  if ((changed & PAD_START) && (s.buttons & PAD_START))
    g_escape_pressed = 1;
  if ((changed & PAD_BACK) && (s.buttons & PAD_BACK))
    g_select_pressed = 1;
  prev = s.buttons;

  g_r3_down = (s.buttons & PAD_R3) != 0;
  g_l3_down = (s.buttons & PAD_L3) != 0;
  g_dpad_down = (s.buttons & PAD_DOWN) != 0;
  g_l1_down = (s.buttons & PAD_L1) != 0;
  g_r1_down = (s.buttons & PAD_R1) != 0;
  g_right_stick_y = s.ry;

  const float axes[6] = { s.lx, s.ly, s.rx, s.ry, s.lt, s.rt };
  if (memcmp(axes, prev_axes, sizeof(axes)) != 0) {
    memcpy(prev_axes, axes, sizeof(axes));
    gn.onGamepadAxesChanged(fake_env, NULL, 0, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt);
  }

  /* SELECT+START (or the guide/function key) held for a second: quit */
  const int combo = (s.buttons & (PAD_BACK | PAD_START)) == (PAD_BACK | PAD_START) ||
                    (s.buttons & PAD_GUIDE);
  if (!pconfig.exit_combo || !combo) {
    combo_since = 0;
    return 0;
  }
  const uint64_t now = now_ns();
  if (!combo_since)
    combo_since = now;
  return now - combo_since >= 1000000000ull;
}
