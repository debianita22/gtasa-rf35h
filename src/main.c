/* main.c -- GTA: San Andreas (Android 2.11.311 arm64) on Linux handhelds
 *
 * Loads libc++_shared.so and libGame.so from the user's own copy of the
 * game, binds them to glibc/Mesa/SDL2/OpenAL through the import table,
 * applies gtasa_nx's game patches, and drives the engine through its
 * GameNative entry points the way Android's GameActivity would. The flow
 * follows gtasa_nx's main.c; the platform pieces are Linux.
 *
 *   gtasa [-d DATA_DIR] [--check | --import | --write-config]
 *
 * --check loads and validates the game files without opening a window and
 * exits 0 when they are usable; --import installs them from the user's APK
 * and OBB (import.c); --write-config writes the default gtasa_nx.cfg if
 * there is none.
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (original main.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <locale.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "config.h"
#include "error.h"
#include "hooks.h"
#include "jni_fake.h"
#include "so_util.h"
#include "util.h"

#include "bionic.h"
#include "build_check.h"
#include "crash.h"
#include "gamenative.h"
#include "gl_wrap.h"
#include "game_tuning.h"
#include "import.h"
#include "import_table.h"
#include "loader.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_util.h"
#include "stats.h"
#include "tls_guard.h"

#ifndef GTASA_VERSION
#define GTASA_VERSION "dev"
#endif

so_module donor_mod; /* libc++_shared.so, the C++ runtime the game was built with */
so_module game_mod;  /* libGame.so */

static volatile sig_atomic_t term_requested;

/* SIGTERM (systemd stopping the service) or SIGINT: the main loop leaves at
 * its next frame. If it is stuck inside the engine (a long load), the alarm
 * ends the process anyway after a few seconds, settings put back. */
static void on_alarm(int sig) {
  (void)sig;
  tune_restore_all();
  sync();
  _exit(0);
}

static void on_term(int sig) {
  (void)sig;
  term_requested = 1;
  alarm(5);
}

/* The engine's static destructors crash at exit (gtasa_nx found the same on
 * the Switch), so nothing is torn down: saves are flushed and the process
 * ends, every thread with it. */
void hard_exit(void) {
  debugPrintf("exit\n");
  tune_restore_all();
  sync();
  _exit(0);
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Manca %s nella cartella del gioco.\nVa estratto da lib/arm64-v8a dell'APK 2.11.311.",
                SO_NAME);
  if (stat(CXX_DONOR_SO_NAME, &st) < 0)
    fatal_error("Manca %s nella cartella del gioco.\nVa estratto da lib/arm64-v8a dell'APK 2.11.311.",
                CXX_DONOR_SO_NAME);
  int dirs = 0;
  const char *expected[] = { "data", "texdb", "models", "audio", "anim", "text" };
  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
    if (stat(expected[i], &st) == 0 && S_ISDIR(st.st_mode))
      dirs++;
    else
      debugPrintf("data: %s/ is missing\n", expected[i]);
  if (dirs < 3)
    fatal_error("Mancano i dati del gioco (data/, texdb/, models/, audio/...).\n"
                "Estrai assets e OBB della 2.11.311 nella cartella del gioco.");
}

static void load_modules(void) {
  if (so_load(&donor_mod, CXX_DONOR_SO_NAME, NULL, 0) < 0)
    fatal_error("%s non si carica: file danneggiato o non arm64.", CXX_DONOR_SO_NAME);
  if (so_load(&game_mod, SO_NAME, NULL, 0) < 0)
    fatal_error("%s non si carica: file danneggiato o non arm64.", SO_NAME);

  BuildCheck bc;
  if (build_check(&game_mod, &bc) == 0) {
    debugPrintf("build: 2.11.311 arm64 confirmed (%d/%d checks)\n", bc.matched, bc.checked);
  } else {
    debugPrintf("build: %d/%d checks match; first difference: %s\n", bc.matched, bc.checked,
                bc.first_mismatch);
    if (!getenv("GTASA_SKIP_BUILD_CHECK"))
      fatal_error("Questo libGame.so non e' la build 2.11.311 arm64-v8a\n"
                  "su cui sono scritte le patch (%d/%d controlli).\n%s",
                  bc.matched, bc.checked, bc.first_mismatch);
  }

  so_set_fallback_resolver(imports_fallback);
  import_table_configure();
  so_relocate(&donor_mod);
  so_relocate(&game_mod);
  so_resolve(&donor_mod, import_table, import_table_count, 1);
  int unresolved = so_unresolved_count();
  so_resolve(&game_mod, import_table, import_table_count, 1);
  unresolved += so_unresolved_count();
  debugPrintf("imports: %d left unresolved (they trap with their name if called)\n", unresolved);
}

static int run_check(void) {
  check_data();
  load_modules();
  const int missing = gamenative_resolve(&game_mod, 0);
  if (!so_try_find_addr_rx(&game_mod, "JNI_OnLoad")) {
    printf("FAIL: JNI_OnLoad missing\n");
    return 1;
  }
  if (missing) {
    printf("FAIL: %d GameNative entry points missing\n", missing);
    return 1;
  }
  printf("OK: game files usable (log: gtasa.log)\n");
  return 0;
}

static void setup_environment(const char *data_dir) {
  /* Mesa reads these when the game creates its context */
  setenv("mesa_glthread", pconfig.glthread ? "true" : "false", 1);
  if (pconfig.gl_no_error)
    setenv("MESA_NO_ERROR", "1", 1);
  char cache[1024];
  snprintf(cache, sizeof(cache), "%s/shadercache", data_dir);
  mkdir(cache, 0755);
  setenv("MESA_SHADER_CACHE_DIR", cache, 1);
  setenv("MESA_SHADER_CACHE_DISABLE", "false", 1);

  if (pconfig.malloc_arenas > 0)
    mallopt(M_ARENA_MAX, pconfig.malloc_arenas);

  /* OpenAL Soft reads its settings from the game folder */
  if (!getenv("ALSOFT_CONF")) {
    if (access("alsoft.conf", R_OK) != 0)
      pconfig_write_alsoft("alsoft.conf");
    char conf[1100];
    snprintf(conf, sizeof(conf), "%s/alsoft.conf", data_dir);
    setenv("ALSOFT_CONF", conf, 1);
  }

  /* bionic's multibyte functions are UTF-8; glibc's follow LC_CTYPE */
  if (!setlocale(LC_CTYPE, "C.UTF-8"))
    setlocale(LC_CTYPE, "C");

  bionic_set_reported_ram_mb((unsigned)pconfig.report_ram_mb);
  bionic_set_network(pconfig.network);
  FILE *game_out = fdopen(dup(log_fd()), "w");
  if (game_out) {
    setvbuf(game_out, NULL, _IOLBF, 0);
    bionic_set_stdio(game_out, game_out);
  }
  glw_configure(pconfig.gl_state_cache, pconfig.stats);
  glw_skip_get_error(pconfig.gl_no_error);
  glw_keep_lod_bias(pconfig.texture_lod_bias);
}

static void start_engine(void) {
  int (*JNI_OnLoad)(void *vm, void *reserved) = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");

  patch_game();
  game_tuning_install(&game_mod); /* first-run graphics settings for this hardware */
  gamenative_resolve(&game_mod, 1);

  so_finalize(&donor_mod);
  so_finalize(&game_mod);
  so_flush_caches(&donor_mod);
  so_flush_caches(&game_mod);

  /* the C++ runtime's static constructors must run before the game's */
  so_execute_init_array(&donor_mod);
  so_execute_init_array(&game_mod);
  so_free_temp(&donor_mod);
  so_free_temp(&game_mod);

  jni_init();
  debugPrintf("JNI_OnLoad\n");
  JNI_OnLoad(fake_vm, NULL);

  void *gn_class = jni_make_object("com/rockstargames/oswrapper/GameNative");
  void *activity = jni_make_object("GameActivity");
  void *asset_mgr = jni_make_object("AssetManager");
  void *surface = jni_make_object("Surface");
  void *names = jni_make_string_array(0, NULL);
  void *paths = jni_make_string_array(0, NULL);
  int w = 0, h = 0;
  platform_window_size(&w, &h);

  gn.onActivityCreated(fake_env, gn_class, activity);
  gn.onInitialSetup(fake_env, gn_class, activity, asset_mgr, names, paths);
  gn.onSurfaceCreated(fake_env, gn_class);
  gn.onSurfaceChanged(fake_env, gn_class, surface, w, h);
  gn.onResume(fake_env, gn_class);
  /* as gtasa_nx: the built-in controls are always there, so the engine is
   * told about a pad up front (it then hides the touch widgets) */
  gn.onGamepadConnected(fake_env, gn_class, 0);
}

static void main_loop(void) {
  Pacer pacer;
  pacer_init(&pacer, config.fps_cap_30 ? 33333333ull : 16666667ull);
  uint64_t last = now_ns();
  /* the boot boost lasts until the menu has been up for two seconds, or
   * two minutes at most */
  const uint64_t boot_deadline = last + 120ull * 1000000000ull;
  uint64_t menu_since = 0;
  int boosting = 1;

  stats_thread_role((int)syscall(SYS_gettid), "main");
  while (!term_requested && !platform_quit_requested() && !jni_quit_requested) {
    gamenative_dispatch_callbacks();
    platform_pump();
    if (gamepad_update()) {
      debugPrintf("exit combination held\n");
      break;
    }

    const uint64_t now = now_ns();
    float dt = (float)(now - last) / 1e9f;
    last = now;
    /* as gtasa_nx: never feed a near-zero or huge step to the engine timers */
    if (dt < 1.0f / 120.0f || dt > 0.5f)
      dt = config.fps_cap_30 ? 1.0f / 30.0f : 1.0f / 60.0f;

    gn.onDrawFrame(fake_env, NULL, dt);
    if (config.fps_cap_30)
      keep_game_frame_limiter_off();
    stats_logic_frame(now_ns() - now);

    if (boosting) {
      if (jni_frontend_ready && !menu_since)
        menu_since = now;
      if ((menu_since && now - menu_since > 2000000000ull) || now > boot_deadline) {
        cpu_boost(0);
        boosting = 0;
      }
    }

    pacer_wait(&pacer, &term_requested);
  }
}

static void usage(void) {
  fprintf(stderr, "usage: gtasa [-d DATA_DIR] [--check | --import | --write-config | --version]\n");
}

int main(int argc, char **argv) {
  const char *data_dir = getenv("GTASA_DATA");
  int check_only = 0, import_only = 0, write_config = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-d") && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (!strcmp(argv[i], "--check")) {
      check_only = 1;
    } else if (!strcmp(argv[i], "--import")) {
      import_only = 1;
    } else if (!strcmp(argv[i], "--write-config")) {
      write_config = 1;
    } else if (!strcmp(argv[i], "--version")) {
      printf("gtasa %s (gtasa_nx 3460c75 hooks, libGame.so 2.11.311 arm64-v8a)\n", GTASA_VERSION);
      return 0;
    } else {
      usage();
      return 2;
    }
  }
  if (data_dir && chdir(data_dir) < 0) {
    fprintf(stderr, "gtasa: cannot enter %s: %s\n", data_dir, strerror(errno));
    return 2;
  }
  char cwd[1024];
  if (!getcwd(cwd, sizeof(cwd)))
    strcpy(cwd, ".");

  if (write_config) { /* for the launcher, before it edits options */
    pconfig_defaults();
    return access(CONFIG_NAME, F_OK) == 0 || pconfig_write_default(CONFIG_NAME) == 0 ? 0 : 1;
  }
  if (import_only) {
    /* before the log: a refused import must not rotate the running one's */
    if (import_lock(cwd) < 0) {
      fprintf(stderr, "gtasa --import: un'importazione e' gia' in corso in %s\n", cwd);
      return 3;
    }
    log_open("gtasa-import.log");
    debugPrintf("gtasa %s: import in %s\n", GTASA_VERSION, cwd);
    pconfig_defaults();
    return import_run(cwd, 1);
  }

  log_open(check_only ? "gtasa-check.log" : "gtasa.log");
  debugPrintf("gtasa %s in %s\n", GTASA_VERSION, cwd);

  char why[128];
  if (tls_guard_check(why, sizeof(why)) < 0)
    fatal_error("Layout TLS inatteso: %s", why);
  crash_install();
  signal(SIGALRM, on_alarm);
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  signal(SIGHUP, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);

  pconfig_defaults();
  if (access(CONFIG_NAME, R_OK) != 0 && pconfig_write_default(CONFIG_NAME) == 0)
    debugPrintf("config: wrote defaults to %s\n", CONFIG_NAME);
  read_config(CONFIG_NAME);  /* gtasa_nx's options */
  pconfig_load(CONFIG_NAME); /* ours, and RF35H values for missing game keys */
  setup_environment(cwd);

  if (check_only)
    return run_check();

  check_data();
  cpu_boost(1);
  platform_init("GTA: San Andreas", config.screen_width, config.screen_height);
  /* gtasa_nx's OS_ScreenGetWidth/Height hooks and the overlay read these */
  platform_window_size(&screen_width, &screen_height);
  load_modules();
  stats_start();
  start_engine();
  main_loop();
  hard_exit();
  return 0;
}
