/* test_game.c -- the port against the game's own 2.11.311 binary (optional)
 *
 *   test_game GAME_DIR OUT_DIR
 *
 * Needs the user's libGame.so and libc++_shared.so (and scache.txt for the
 * shaders) in GAME_DIR: game files are not part of the repository, so
 * tests/run-tests.sh runs this only when GTASA_GAME_DIR names such a
 * folder. Runs on aarch64 (qemu-aarch64 -cpu cortex-a35 on a PC):
 *   build     the fingerprints the hooks rely on (build_check)
 *   imports   every import of both libraries binds
 *   settings  the game's own first-run settings with Panfrost's caps (the
 *             reason for game_tuning.c), then through game_tuning: the
 *             profile, -1 and out-of-range values, gta_sa.set already read
 *   shaders   every scache.txt variant from the game's own generator with
 *             the LOD bias taken out, written to OUT_DIR (the caller checks
 *             them with glslangValidator)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "so_util.h"

#include "build_check.h"
#include "game_tuning.h"
#include "gl_wrap.h"
#include "import_table.h"
#include "loader.h"
#include "platform_config.h"
#include "platform_util.h"
#include "tls_guard.h"

/* the platform layer's main.c provides these to the rest of the code */
so_module game_mod;
void hard_exit(void) { _exit(0); }

typedef struct {
  void *value_names, *other;
  int32_t value, def, min, max;
  uint8_t flag, pad[3];
  int32_t slider;
} MobileSetting;

enum { VS = 0, DD = 2, SH = 5, CAR = 7 };

static int failures;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      printf("  FAIL line %d: ", __LINE__);       \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

static void report(const char *name, int before) {
  printf("%-10s %s\n", name, failures == before ? "ok" : "FAIL");
}

static so_module donor;
static MobileSetting *ms;
static uint8_t *loaded, *caps;
static void (*ms_initialize)(void), (*ms_renderer_defaults)(void);

static void finalize(void) {
  so_finalize(&donor);
  so_finalize(&game_mod);
  so_flush_caches(&donor);
  so_flush_caches(&game_mod);
}

/* RQCaps as RQ_Command_rqInit leaves it on Panfrost (no DXT, ATC, PVRTC) */
static void panfrost_caps(void) {
  memset(caps, 0, 20);
  caps[0] = caps[1] = 1; /* GL_OES_depth24, GL_OES_packed_depth_stencil */
  caps[13] = 1;          /* none of DXT, ATC, PVRTC */
}

/* the game's settings start: Initialize, gta_sa.set read or not, then the
 * renderer defaults, as psInitialize and OS_ApplicationStartup run them */
static void settings_start(int file_read, const int *file_values) {
  ms_initialize();
  if (file_read) {
    ms[VS].value = file_values[0];
    ms[SH].value = file_values[1];
    ms[DD].value = file_values[2];
    ms[CAR].value = file_values[3];
  }
  *loaded = (uint8_t)file_read;
  ms_renderer_defaults();
}

static int expect(const char *what, int vs, int sh, int dd, int car, int values) {
  const int before = failures;
  CHECK(ms[VS].def == vs && ms[SH].def == sh && ms[DD].def == dd && ms[CAR].def == car,
        "%s: defaults visual FX %d shadows %d distance %d reflections %d, expected %d %d %d %d", what,
        ms[VS].def, ms[SH].def, ms[DD].def, ms[CAR].def, vs, sh, dd, car);
  if (values)
    CHECK(ms[VS].value == vs && ms[SH].value == sh && ms[DD].value == dd && ms[CAR].value == car,
          "%s: values %d %d %d %d, expected the defaults", what, ms[VS].value, ms[SH].value,
          ms[DD].value, ms[CAR].value);
  return failures == before;
}

static int dump_shaders(const char *scache, const char *out_dir) {
  int (*build)(unsigned, const char **, const char **) =
      (void *)so_try_find_addr_rx(&game_mod, "_ZN8RQShader11BuildSourceEjPPKcS2_");
  int *max_bones = (int *)so_try_find_addr_rx(&game_mod, "RQMaxBones");
  FILE *list = fopen(scache, "r");
  if (!build || !max_bones || !list) {
    CHECK(0, "no shader generator or no %s", scache);
    if (list)
      fclose(list);
    return -1;
  }
  *max_bones = 128; /* Panfrost: 4096 vertex uniform vectors */
  mkdir(out_dir, 0755);
  char line[64];
  int pairs = 0, biased = 0;
  while (fgets(line, sizeof(line), list)) {
    char *end;
    const unsigned long flags = strtoul(line, &end, 16);
    if (end == line)
      continue;
    const char *pixel = NULL, *vertex = NULL;
    build((unsigned)flags, &pixel, &vertex);
    if (!pixel || !vertex) {
      CHECK(0, "no source for %lx", flags);
      continue;
    }
    char *rewritten = malloc(strlen(pixel) + 1);
    biased += glw_drop_lod_bias(pixel, rewritten) > 0;
    CHECK(!strstr(rewritten, "Out_Tex0, -"), "%lx: a biased sample is left", flags);
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/%08lx.frag", out_dir, flags);
    FILE *f = fopen(path, "w");
    /* the game sends fragment shaders without a #version: GLSL ES 1.00 */
    if (f) {
      fprintf(f, "#version 100\n%s\n", rewritten);
      fclose(f);
    }
    snprintf(path, sizeof(path), "%s/%08lx.vert", out_dir, flags);
    f = fopen(path, "w");
    if (f) {
      fprintf(f, "%s\n", vertex);
      fclose(f);
    }
    free(rewritten);
    pairs++;
  }
  fclose(list);
  printf("           %d shader pairs, %d fragment shaders sampled the diffuse texture with a bias\n",
         pairs, biased);
  CHECK(pairs > 0 && biased > 0, "nothing to check");
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: test_game GAME_DIR OUT_DIR\n");
    return 2;
  }
  char why[128];
  if (tls_guard_check(why, sizeof(why)) < 0) {
    printf("tls_guard: FAIL (%s)\n", why);
    return 1;
  }
  char out_dir[PATH_MAX], game_dir[PATH_MAX], scache[PATH_MAX + 16];
  mkdir(argv[2], 0755);
  if (!realpath(argv[2], out_dir) || !realpath(argv[1], game_dir) || chdir(game_dir) < 0) {
    printf("cannot use %s and %s: %s\n", argv[1], argv[2], strerror(errno));
    return 1;
  }
  snprintf(scache, sizeof(scache), "%s/scache.txt", game_dir);

  int before = failures;
  pconfig_defaults();
  so_set_fallback_resolver(imports_fallback);
  import_table_configure();
  if (so_load(&donor, "libc++_shared.so", NULL, 0) < 0 || so_load(&game_mod, "libGame.so", NULL, 0) < 0) {
    printf("libGame.so and libc++_shared.so do not load from %s\n", argv[1]);
    return 1;
  }
  BuildCheck bc;
  CHECK(build_check(&game_mod, &bc) == 0 && bc.matched == bc.checked,
        "%d of %d fingerprints match: not the 2.11.311 arm64 build", bc.matched, bc.checked);
  report("build", before);

  before = failures;
  so_relocate(&donor);
  so_relocate(&game_mod);
  so_resolve(&donor, import_table, import_table_count, 1);
  int unresolved = so_unresolved_count();
  so_resolve(&game_mod, import_table, import_table_count, 1);
  unresolved += so_unresolved_count();
  CHECK(unresolved == 0, "%d imports left unresolved", unresolved);
  report("imports", before);

  ms = (MobileSetting *)so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings8settingsE");
  loaded = (uint8_t *)so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings6loadedE");
  caps = (uint8_t *)so_try_find_addr_rx(&game_mod, "RQCaps");
  ms_initialize = (void *)so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings10InitializeEv");
  ms_renderer_defaults = (void *)so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings19SetRendererDefaultsEv");
  if (!ms || !loaded || !caps || !ms_initialize || !ms_renderer_defaults) {
    printf("MobileSettings or RQCaps missing\n");
    return 1;
  }

  /* the game alone, in a child: hooks can only go in before finalize */
  before = failures;
  fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    finalize();
    panfrost_caps();
    settings_start(0, NULL);
    _exit(expect("the game alone", 2, 2, 100, 3, 1) ? 0 : 1);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "the game's own first-run settings are not the top ones");
  report("game alone", before);

  before = failures;
  CHECK(game_tuning_install(&game_mod) == 0, "game_tuning_install failed");
  finalize();
  panfrost_caps();
  settings_start(0, NULL);
  expect("profile, first run", 0, 0, 50, 1, 1);
  pconfig.game_visual_fx = -1;      /* the game's */
  pconfig.game_draw_distance = 250; /* out of range: the game's */
  settings_start(0, NULL);
  expect("-1 and out of range", 2, 0, 100, 1, 1);
  pconfig_defaults();
  static const int chosen[4] = { 3, 2, 77, 3 };
  settings_start(1, chosen);
  expect("gta_sa.set read", 0, 0, 50, 1, 0);
  CHECK(ms[VS].value == 3 && ms[SH].value == 2 && ms[DD].value == 77 && ms[CAR].value == 3,
        "the player's settings were overwritten: %d %d %d %d", ms[VS].value, ms[SH].value,
        ms[DD].value, ms[CAR].value);
  report("settings", before);

  before = failures;
  if (access(scache, R_OK) == 0)
    dump_shaders(scache, out_dir);
  else
    printf("           no scache.txt in the game folder: shaders skipped\n");
  report("shaders", before);

  printf("game: %s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
