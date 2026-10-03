/* game_tuning.c -- the game's own graphics settings, started where this
 * hardware can run them
 *
 * On first run the game picks its settings from the GPU it detects
 * (MobileSettings::SetRendererDefaults, after RQ_Command_rqInit has filled
 * RQCaps from the GL extension list). Only old Adreno 225 / PowerVR SGX 540
 * class GPUs get a light profile; anything else, Panfrost included, starts
 * at the top: Visual FX High, real-time shadows, draw distance 100, car
 * reflections 3 (a 1024x512 sphere-map render of the scene, every frame).
 * Measured by running the game's own code on its 2.11.311 binary
 * (tests/test_game.c).
 *
 * The replacement below does what the original does and then puts the
 * defaults from gtasa_nx.cfg on top (game_visual_fx, game_shadows,
 * game_draw_distance, game_car_reflections; -1 keeps the game's). They are
 * defaults: they apply on the first run, before gta_sa.set exists, and when
 * the game's menu resets its settings. Whatever the player chooses in the
 * menu is saved in gta_sa.set and left alone.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stddef.h>
#include <stdint.h>

#include "game_tuning.h"
#include "loader.h"
#include "platform_config.h"
#include "util.h"

/* MobileSettings::settings[] in 2.11.311: 38 entries of 40 bytes */
typedef struct {
  void *value_names;
  void *other;
  int32_t value;
  int32_t def;
  int32_t min, max;
  uint8_t flag, pad[3];
  int32_t slider;
} MobileSetting;

_Static_assert(sizeof(MobileSetting) == 40, "MobileSettings entry layout");

enum {
  MS_VISUAL_FX = 0,       /* MOB_VS: 0 low .. 3 very high */
  MS_DRAW_DISTANCE = 2,   /* MOB_DD: 0..100 */
  MS_SHADOWS = 5,         /* MOB_SH: 0 off, 1 classic, 2 real-time */
  MS_CAR_REFLECTIONS = 7, /* CAR_REF: 0 off, 1 static, 2-3 rendered */
  MS_FRAME_LIMITER = 30,  /* FEM_FRM */
  MS_COUNT = 38,
};

/* RQCaps bytes read by the original (see RQ_Command_rqInit) */
enum { CAP_ATC = 4, CAP_PVRTC = 5, CAP_OLD_GPU = 14 };

static MobileSetting *settings;
static const uint8_t *loaded, *caps;
static int logged;

static void set_default(int index, int value, const char *what) {
  if (value < 0)
    return; /* -1: the game's own default */
  MobileSetting *s = &settings[index];
  if (value < s->min || value > s->max) {
    debugPrintf("game settings: %s %d out of %d..%d, the game's %d kept\n", what, value, s->min,
                s->max, s->def);
    return;
  }
  s->def = value;
}

static void SetRendererDefaults_tuned(void) {
  /* the original, as disassembled from 2.11.311 */
  if (caps[CAP_OLD_GPU] == 1) {
    settings[MS_VISUAL_FX].def = 0;
    settings[MS_SHADOWS].def = 0;
    settings[MS_CAR_REFLECTIONS].def = 1;
    if (caps[CAP_PVRTC] == 1)
      settings[MS_FRAME_LIMITER].def = 1;
  } else if ((caps[CAP_ATC] & 1) || caps[CAP_PVRTC] == 1) {
    settings[MS_VISUAL_FX].def = 1;
  }

  /* the layout check needs MobileSettings::Initialize to have run, as it has
   * whenever the game calls this */
  const int layout_ok = settings[MS_VISUAL_FX].max == 3 && settings[MS_DRAW_DISTANCE].max == 100 &&
                        settings[MS_SHADOWS].max == 2 && settings[MS_CAR_REFLECTIONS].max == 3;
  if (layout_ok) {
    set_default(MS_VISUAL_FX, pconfig.game_visual_fx, "game_visual_fx");
    set_default(MS_SHADOWS, pconfig.game_shadows, "game_shadows");
    set_default(MS_DRAW_DISTANCE, pconfig.game_draw_distance, "game_draw_distance");
    set_default(MS_CAR_REFLECTIONS, pconfig.game_car_reflections, "game_car_reflections");
  }
  if (!(*loaded & 1))
    for (int i = 0; i < MS_COUNT; i++)
      settings[i].value = settings[i].def;

  if (!logged) {
    logged = 1;
    if (!layout_ok)
      debugPrintf("game settings: unexpected MobileSettings layout, the game's defaults kept\n");
    debugPrintf("game settings: %s visual FX %d, shadows %d, draw distance %d, car reflections %d\n",
                (*loaded & 1) ? "gta_sa.set in use; menu defaults" : "first run:",
                settings[MS_VISUAL_FX].def, settings[MS_SHADOWS].def,
                settings[MS_DRAW_DISTANCE].def, settings[MS_CAR_REFLECTIONS].def);
  }
}

int game_tuning_install(so_module *game) {
  const uintptr_t fn = so_try_find_addr_rx(game, "_ZN14MobileSettings19SetRendererDefaultsEv");
  settings = (MobileSetting *)so_try_find_addr_rx(game, "_ZN14MobileSettings8settingsE");
  loaded = (const uint8_t *)so_try_find_addr_rx(game, "_ZN14MobileSettings6loadedE");
  caps = (const uint8_t *)so_try_find_addr_rx(game, "RQCaps");
  if (!fn || !settings || !loaded || !caps) {
    debugPrintf("game settings: MobileSettings not found, the game's defaults kept\n");
    return -1;
  }
  hook_arm64(fn, (uintptr_t)SetRendererDefaults_tuned);
  return 0;
}
