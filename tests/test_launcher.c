/* test_launcher.c -- the launcher core under a minimal libretro frontend
 *
 *   test_launcher CORE.so WORKDIR
 *
 * Loads the core like RetroArch does for a contentless core and drives it
 * through the situations a user meets: an empty game folder, a first start
 * with the APK and OBB waiting (import, then launch), an error left by the
 * previous run (START to retry), the core options, a failed import and a
 * failed start; and the ones around a detached import: one still running
 * from an earlier session (followed, not restarted), one that got the lock
 * first (exit 3: followed), one that died midway, Restart during an
 * import, a failed import over an installed game (START launches it), and
 * SIGCHLD ignored by the frontend (statuses unknown: the status file
 * decides). The
 * runner it calls is a fake (shell script written here) that logs its
 * arguments and plays its part.
 */

#define _GNU_SOURCE

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../packaging/lakka/gtasa/launcher/libretro.h"

static int failures;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      printf("  FAIL line %d: ", __LINE__);               \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
      failures++;                                         \
    }                                                     \
  } while (0)

static char last_msg[1024];
static int messages, no_game, options_v2, start_button;
static const char *opt_values[16][2];
static int nopts;

static bool env(unsigned cmd, void *data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      no_game = *(bool *)data;
      return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
      *(unsigned *)data = 2;
      return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
      const struct retro_core_options_v2 *o = data;
      options_v2 = 0;
      for (const struct retro_core_option_v2_definition *d = o->definitions; d->key; d++)
        options_v2++;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *v = data;
      v->value = NULL;
      for (int i = 0; i < nopts; i++)
        if (!strcmp(opt_values[i][0], v->key))
          v->value = opt_values[i][1];
      return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *(unsigned *)data = 1;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      snprintf(last_msg, sizeof(last_msg), "%s", ((const struct retro_message_ext *)data)->msg);
      messages++;
      return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE:
      snprintf(last_msg, sizeof(last_msg), "%s", ((const struct retro_message *)data)->msg);
      messages++;
      return true;
    default:
      return false;
  }
}

static int frames_seen;
static void video(const void *d, unsigned w, unsigned h, size_t pitch) {
  (void)d;
  if (w == 320 && h == 240 && pitch == 320 * 4)
    frames_seen++;
}
static size_t audio_batch(const int16_t *d, size_t n) { (void)d; return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)idx;
  return port == 0 && dev == RETRO_DEVICE_JOYPAD && id == RETRO_DEVICE_ID_JOYPAD_START && start_button;
}

static struct {
  void (*set_environment)(retro_environment_t);
  void (*set_video_refresh)(retro_video_refresh_t);
  void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*set_input_poll)(retro_input_poll_t);
  void (*set_input_state)(retro_input_state_t);
  void (*init)(void);
  bool (*load_game)(const struct retro_game_info *);
  void (*run)(void);
  void (*reset)(void);
  void (*get_system_info)(struct retro_system_info *);
  unsigned (*api_version)(void);
} core;

static char work[1024], dir[1100], log_path[1200];

static void writef(const char *path, const char *fmt, ...) {
  FILE *f = fopen(path, "w");
  va_list va;
  va_start(va, fmt);
  vfprintf(f, fmt, va);
  va_end(va);
  fclose(f);
}

static void make_runner(const char *body) {
  char path[1200];
  snprintf(path, sizeof(path), "%s/fake-runner", work);
  writef(path, "#!/bin/sh\necho \"$*\" >> '%s'\nD='%s'\n%s\n", log_path, dir, body);
  chmod(path, 0755);
  setenv("RF35H_GTASA", path, 1);
}

static void new_case(const char *name) {
  printf("-- %s\n", name);
  snprintf(dir, sizeof(dir), "%s/%s", work, name);
  mkdir(dir, 0755);
  snprintf(log_path, sizeof(log_path), "%s/%s.calls", work, name);
  unlink(log_path);
  setenv("GTASA_DIR", dir, 1);
  last_msg[0] = 0;
  nopts = 0;
  start_button = 0;
}

static void touch(const char *rel) {
  char p[1300];
  snprintf(p, sizeof(p), "%s/%s", dir, rel);
  writef(p, "x");
}

static void calls(char *out, size_t len) {
  out[0] = 0;
  FILE *f = fopen(log_path, "r");
  if (!f)
    return;
  size_t n = fread(out, 1, len - 1, f);
  out[n] = 0;
  fclose(f);
}

/* run frames until `until` returns true or `max` frames pass; 1 if it did */
static int run_until(int max, int (*until)(void)) {
  for (int i = 0; i < max; i++) {
    core.run();
    if (until && until())
      return 1;
    usleep(2000);
  }
  return until ? 0 : 1;
}

static int started(void) {
  char c[4096];
  calls(c, sizeof(c));
  return strstr(c, "start ") != NULL;
}
static int msg_has_restart(void) { return strstr(last_msg, "START per riprovare") != NULL; }
static int msg_has_installed(void) { return strstr(last_msg, "START avvia il gioco") != NULL; }

static int count_calls(const char *what) {
  char c[4096];
  calls(c, sizeof(c));
  int n = 0;
  for (const char *p = c; (p = strstr(p, what)); p += strlen(what))
    n++;
  return n;
}

/* the folder's import lock, held the way a running importer holds it */
static int hold_import_lock(void) {
  char p[1300];
  snprintf(p, sizeof(p), "%s/.import.lock", dir);
  const int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd >= 0 && flock(fd, LOCK_EX) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void install_game(void) {
  char p[1300];
  touch("libGame.so");
  touch("libc++_shared.so");
  snprintf(p, sizeof(p), "%s/data", dir);
  mkdir(p, 0755);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: test_launcher CORE.so WORKDIR\n");
    return 2;
  }
  snprintf(work, sizeof(work), "%s", argv[2]);
  mkdir(work, 0755);
  void *h = dlopen(argv[1], RTLD_NOW);
  if (!h) {
    printf("dlopen: %s\n", dlerror());
    return 1;
  }
#define SYM(field, name) *(void **)&core.field = dlsym(h, name)
  SYM(set_environment, "retro_set_environment");
  SYM(set_video_refresh, "retro_set_video_refresh");
  SYM(set_audio_sample_batch, "retro_set_audio_sample_batch");
  SYM(set_input_poll, "retro_set_input_poll");
  SYM(set_input_state, "retro_set_input_state");
  SYM(init, "retro_init");
  SYM(load_game, "retro_load_game");
  SYM(run, "retro_run");
  SYM(reset, "retro_reset");
  SYM(get_system_info, "retro_get_system_info");
  SYM(api_version, "retro_api_version");

  struct retro_system_info info;
  core.get_system_info(&info);
  CHECK(core.api_version() == RETRO_API_VERSION, "API version");
  CHECK(!strcmp(info.library_name, "GTA San Andreas"), "library name %s", info.library_name);
  core.set_environment(env);
  CHECK(no_game, "contentless operation not declared");
  CHECK(options_v2 == 6, "%d core options", options_v2);
  core.set_video_refresh(video);
  core.set_audio_sample_batch(audio_batch);
  core.set_input_poll(poll);
  core.set_input_state(input);
  core.init();

  char c[4096];

  new_case("empty");
  make_runner("exit 0");
  CHECK(core.load_game(NULL), "load without content");
  run_until(200, NULL);
  CHECK(strstr(last_msg, "Copia l'APK") != NULL, "empty folder message: %s", last_msg);
  calls(c, sizeof(c));
  CHECK(c[0] == 0, "runner called with nothing to do: %s", c);
  CHECK(frames_seen > 0, "no video frames");

  new_case("first-start");
  touch("base.apk");
  touch("main.8.com.rockstargames.gtasa.obb");
  make_runner("case \"$1\" in\n"
              "import) echo 'run 10 Estrazione: texdb/gta3/gta3.etc.dat' > \"$D/import-status.txt\"; sleep 1\n"
              "  touch \"$D/libGame.so\" \"$D/libc++_shared.so\"; mkdir -p \"$D/data\" \"$D/import/originali\"\n"
              "  mv \"$D\"/*.apk \"$D\"/*.obb \"$D/import/originali/\"\n"
              "  echo 'done 100 Importazione completata (2400 MB)' > \"$D/import-status.txt\"; exit 0 ;;\n"
              "start) exit 0 ;;\nesac");
  core.load_game(NULL);
  int saw_progress = 0;
  for (int i = 0; i < 2000 && !started(); i++) {
    core.run();
    saw_progress |= strstr(last_msg, "Installazione 10%") != NULL;
    usleep(2000);
  }
  calls(c, sizeof(c));
  CHECK(!strncmp(c, "import\n", 7), "import first: %s", c);
  CHECK(saw_progress, "import progress never shown");
  CHECK(strstr(c, "start fps=30 scale=100 perf=2 swap_ab=0 show_fps=0 stats=0") != NULL,
        "launch with the default options: %s", c);

  new_case("partial-install");
  touch("libGame.so");
  touch("libc++_shared.so");
  touch("data");
  touch("main.8.com.rockstargames.gtasa.obb"); /* an import stopped halfway */
  make_runner("[ \"$1\" = import ] && { mv \"$D/main.8.com.rockstargames.gtasa.obb\" \"$D/moved\"; "
              "echo 'done 100 ok' > \"$D/import-status.txt\"; }; exit 0");
  core.load_game(NULL);
  CHECK(run_until(2000, started), "never launched");
  calls(c, sizeof(c));
  CHECK(!strncmp(c, "import\n", 7), "archives waiting must be imported before launching: %s", c);

  new_case("last-error");
  touch("libGame.so");
  touch("libc++_shared.so");
  mkdir(strcat(strcpy(c, dir), "/data"), 0755);
  writef(strcat(strcpy(c, dir), "/last-error.txt"), "Mancano i dati del gioco\n");
  make_runner("exit 0");
  core.load_game(NULL);
  run_until(400, NULL);
  CHECK(strstr(last_msg, "Mancano i dati del gioco") && msg_has_restart(), "last error shown: %s", last_msg);
  CHECK(!started(), "started despite the last error");
  start_button = 1;
  run_until(3, NULL);
  start_button = 0;
  CHECK(run_until(400, started), "START did not retry");
  CHECK(access(strcat(strcpy(c, dir), "/last-error.txt"), F_OK) != 0, "last-error.txt kept");

  new_case("options");
  touch("libGame.so");
  touch("libc++_shared.so");
  touch("texdb");
  make_runner("exit 0");
  opt_values[0][0] = "gtasa_fps", opt_values[0][1] = "60";
  opt_values[1][0] = "gtasa_scale", opt_values[1][1] = "75";
  opt_values[2][0] = "gtasa_perf", opt_values[2][1] = "9; rm -rf /"; /* not in the list */
  opt_values[3][0] = "gtasa_stats", opt_values[3][1] = "1";
  nopts = 4;
  core.load_game(NULL);
  CHECK(run_until(400, started), "no launch");
  calls(c, sizeof(c));
  CHECK(strstr(c, "start fps=60 scale=75 perf=2 swap_ab=0 show_fps=0 stats=1") != NULL,
        "options passed (unknown value replaced by the default): %s", c);

  new_case("import-fails");
  touch("gtasa.apk");
  make_runner("echo 'fail 0 manca l OBB (main.*.obb) in ROMs/gtasa' > \"$D/import-status.txt\"; exit 2");
  core.load_game(NULL);
  CHECK(run_until(400, msg_has_restart), "no failure message");
  CHECK(strstr(last_msg, "manca l OBB") != NULL, "import failure reason: %s", last_msg);
  CHECK(!started(), "launched after a failed import");

  new_case("start-fails");
  touch("libGame.so");
  touch("libc++_shared.so");
  touch("data");
  make_runner("[ \"$1\" = start ] && exit 3; exit 0");
  core.load_game(NULL);
  CHECK(run_until(600, msg_has_restart), "no message after a failed start");
  CHECK(strstr(last_msg, "codice 3") != NULL, "start failure: %s", last_msg);

  new_case("import-attach");
  touch("base.apk");
  touch("main.8.com.rockstargames.gtasa.obb");
  writef(strcat(strcpy(c, dir), "/import-status.txt"), "run 40 Estrazione: models/gta3.img\n");
  int lock = hold_import_lock(); /* an import from an earlier session */
  CHECK(lock >= 0, "lock");
  make_runner("exit 0");
  core.load_game(NULL);
  int saw_attach_progress = 0;
  for (int i = 0; i < 300; i++) {
    core.run();
    saw_attach_progress |= strstr(last_msg, "Installazione 40%") != NULL;
    usleep(2000);
  }
  CHECK(count_calls("import") == 0, "a second import started while one ran");
  CHECK(saw_attach_progress, "the running import's progress not shown: %s", last_msg);
  install_game(); /* the other import ends */
  mkdir(strcat(strcpy(c, dir), "/import"), 0755);
  rename(strcat(strcpy(c, dir), "/base.apk"), strcat(strcpy(c + 2048, dir), "/import/base.apk.done"));
  rename(strcat(strcpy(c, dir), "/main.8.com.rockstargames.gtasa.obb"),
         strcat(strcpy(c + 2048, dir), "/import/main.obb.done"));
  writef(strcat(strcpy(c, dir), "/import-status.txt"), "done 100 Importazione completata (2400 MB)\n");
  close(lock);
  CHECK(run_until(2000, started), "never launched after the followed import");
  CHECK(count_calls("import") == 0, "an import was started anyway");

  new_case("import-race");
  touch("base.apk");
  touch("main.8.com.rockstargames.gtasa.obb");
  /* the other importer takes the lock first; ours gives up with 3 */
  make_runner("case \"$1\" in\n"
              "import) ( flock -x 9; echo 'run 55 Estrazione: audio' > \"$D/import-status.txt\"; sleep 1.5\n"
              "  touch \"$D/libGame.so\" \"$D/libc++_shared.so\"; mkdir -p \"$D/data\" \"$D/import/originali\"\n"
              "  mv \"$D\"/*.apk \"$D\"/*.obb \"$D/import/originali/\"\n"
              "  echo 'done 100 ok' > \"$D/import-status.txt\" ) 9>> \"$D/.import.lock\" &\n"
              "  sleep 0.3; exit 3 ;;\n"
              "start) exit 0 ;;\nesac");
  core.load_game(NULL);
  CHECK(run_until(3000, started), "never launched after losing the race");
  CHECK(count_calls("import") == 1, "%d imports started", count_calls("import"));

  new_case("import-interrupted");
  touch("base.apk");
  writef(strcat(strcpy(c, dir), "/import-status.txt"), "run 70 Estrazione: texdb\n");
  lock = hold_import_lock();
  make_runner("exit 0");
  core.load_game(NULL);
  run_until(200, NULL);
  close(lock); /* it died without a last word */
  CHECK(run_until(400, msg_has_restart), "no message after an interrupted import: %s", last_msg);
  CHECK(strstr(last_msg, "interrotta") != NULL, "interrupted import: %s", last_msg);

  new_case("reset-during-import");
  touch("base.apk");
  touch("main.8.com.rockstargames.gtasa.obb");
  make_runner("[ \"$1\" = import ] && { echo 'run 5 Lettura' > \"$D/import-status.txt\"; sleep 1; "
              "mv \"$D/base.apk\" \"$D/main.8.com.rockstargames.gtasa.obb\" \"$D/..\"/ 2>/dev/null; "
              "echo 'fail 0 test' > \"$D/import-status.txt\"; exit 2; }; exit 0");
  core.load_game(NULL);
  run_until(20, NULL);
  core.reset(); /* Quick Menu > Restart while the import runs */
  CHECK(run_until(1000, msg_has_restart), "the import's end never shown: %s", last_msg);
  CHECK(count_calls("import") == 1, "Restart started another import (%d)", count_calls("import"));

  new_case("import-fails-installed");
  install_game();
  touch("patch.8.com.rockstargames.gtasa.obb"); /* alone: the import refuses it */
  make_runner("[ \"$1\" = import ] && { echo \"fail 0 manca l'APK\" > \"$D/import-status.txt\"; "
              "echo \"Importazione non riuscita: manca l'APK\" > \"$D/last-error.txt\"; exit 2; }; exit 0");
  core.load_game(NULL);
  CHECK(run_until(400, msg_has_installed), "no way past a failed import: %s", last_msg);
  CHECK(!started(), "launched before START");
  start_button = 1;
  run_until(3, NULL);
  start_button = 0;
  CHECK(run_until(600, started), "START did not launch the installed game: %s", last_msg);
  CHECK(count_calls("import") == 1, "the import ran again (%d)", count_calls("import"));
  CHECK(access(strcat(strcpy(c, dir), "/last-error.txt"), F_OK) != 0, "the import's error kept");

  new_case("sigchld-ignored");
  touch("base.apk");
  touch("main.8.com.rockstargames.gtasa.obb");
  make_runner("case \"$1\" in\n"
              "import) sleep 0.5; touch \"$D/libGame.so\" \"$D/libc++_shared.so\"\n"
              "  mkdir -p \"$D/data\" \"$D/import/originali\"\n"
              "  mv \"$D\"/*.apk \"$D\"/*.obb \"$D/import/originali/\"\n"
              "  echo 'done 100 ok' > \"$D/import-status.txt\"; exit 0 ;;\n"
              "start) exit 0 ;;\nesac");
  signal(SIGCHLD, SIG_IGN); /* RetroArch, once its narrator has spoken */
  core.load_game(NULL);
  CHECK(run_until(2000, started), "an import with an unknown status never led to the launch: %s", last_msg);
  run_until(20 * 60, NULL); /* the 15 s launch timeout, with the start's status unknown */
  CHECK(strstr(last_msg, "Avvio non riuscito") == NULL, "a start with an unknown status reported failed: %s",
        last_msg);
  signal(SIGCHLD, SIG_DFL);

  printf("launcher: %s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
