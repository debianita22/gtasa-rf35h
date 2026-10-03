/* gtasa_libretro.c -- GTA: San Andreas in RetroArch's "Contentless Cores"
 *
 * Not an emulator: a launcher. Loaded without content, it checks the game
 * folder and then either
 *   - installs the game from the APK and OBB the user copied there
 *     (rf35h-gtasa import), showing the progress as RetroArch notifications
 *     (archives waiting always come first: a new copy, or a retry);
 *   - or counts down two seconds (time to open the Quick Menu for the
 *     options below) and hands over to rf35h-gtasa.service, which stops
 *     RetroArch, runs the game and brings RetroArch back when it exits.
 * The core's options (Quick Menu > Core Options) become settings in
 * gtasa_nx.cfg at every start. An error left by the previous run is shown
 * first, and START retries.
 *
 * An import keeps running if RetroArch restarts or the core is reloaded
 * (it is detached): the importer holds an flock on .import.lock until it
 * and its check are over, and a launcher that finds the lock taken follows
 * that import through import-status.txt instead of starting another. A
 * failed import does not lock out a game already installed: START then
 * launches it (the next session tries the archives again).
 *
 * Environment (for tests): GTASA_DIR (game folder, default
 * /storage/roms/gtasa) and RF35H_GTASA (the runner, default
 * /usr/bin/rf35h-gtasa).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libretro.h"

#define FPS 60
#define COUNTDOWN_FRAMES (2 * FPS)
#define LAUNCH_TIMEOUT_FRAMES (15 * FPS)
#define W 320
#define H 240

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;

static enum {
  ST_INIT,
  ST_LAST_ERROR, /* the previous run failed: show why, START retries */
  ST_IMPORT,
  ST_COUNTDOWN,
  ST_LAUNCH,
  ST_STOPPED, /* nothing to do: message shown, START re-checks */
} state;
static unsigned frame, state_frame, last_shown, hold_until;
static pid_t child = -1;
static int child_status = -1; /* exit status; -1 unknown (reaped elsewhere) */
static int attached;       /* following an import this core did not start */
static int import_failed;  /* the last import failed; START may skip it */
static int skip_import;    /* until the next session: launch what is installed */
static int start_held;
static char message[512];
static uint32_t pixels[W * H];
static int16_t silence[2 * 44100 / FPS];
static int msg_ext;

/* ---- helpers ---------------------------------------------------------------------- */

static const char *game_dir(void) {
  const char *d = getenv("GTASA_DIR");
  return d && *d ? d : "/storage/roms/gtasa";
}

static const char *runner(void) {
  const char *r = getenv("RF35H_GTASA");
  return r && *r ? r : "/usr/bin/rf35h-gtasa";
}

static void show(unsigned seconds, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vsnprintf(message, sizeof(message), fmt, va);
  va_end(va);
  last_shown = frame;
  if (msg_ext) {
    struct retro_message_ext m = { message, seconds * 1000, 3, RETRO_LOG_INFO,
                                   RETRO_MESSAGE_TARGET_ALL, RETRO_MESSAGE_TYPE_NOTIFICATION, -1 };
    if (env_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m))
      return;
  }
  struct retro_message m = { message, seconds * FPS };
  env_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
}

static void set_state(int s) {
  state = s;
  state_frame = frame;
}

static int file_exists(const char *name) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", game_dir(), name);
  return access(path, F_OK) == 0;
}

static int installed(void) {
  return file_exists("libGame.so") && file_exists("libc++_shared.so") &&
         (file_exists("data") || file_exists("texdb"));
}

static int has_archives_in(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return 0;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    const size_t len = strlen(e->d_name);
    if (len > 4 && (!strcasecmp(e->d_name + len - 4, ".apk") || !strcasecmp(e->d_name + len - 4, ".obb")))
      n++;
  }
  closedir(d);
  return n;
}

/* an import (any process) holds the folder's lock: it and its check run */
static int import_running(void) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/.import.lock", game_dir());
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  const int busy = flock(fd, LOCK_SH | LOCK_NB) < 0 && errno == EWOULDBLOCK;
  close(fd);
  return busy;
}

static int archives_waiting(void) {
  char sub[1024];
  snprintf(sub, sizeof(sub), "%s/import", game_dir());
  return has_archives_in(game_dir()) + has_archives_in(sub);
}

/* first line of a file in the game folder, or "" */
static void read_first_line(const char *name, char *out, size_t len) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", game_dir(), name);
  out[0] = 0;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  if (!fgets(out, (int)len, f))
    out[0] = 0;
  fclose(f);
  out[strcspn(out, "\r\n")] = 0;
}

static void remove_last_error(void) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/last-error.txt", game_dir());
  unlink(path);
}

/* run the runner with arguments, detached from RetroArch's fate; -1 when
 * it could not even start */
static pid_t spawn(char *const argv[]) {
  child_status = -1;
  const pid_t pid = fork();
  if (pid != 0)
    return pid;
  setsid();
  /* the runner's shell needs its children's exit statuses: not RetroArch's
   * ignored SIGCHLD, nor its blocked signals */
  signal(SIGCHLD, SIG_DFL);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, NULL);
  char log[1024];
  snprintf(log, sizeof(log), "%s/launcher.log", game_dir());
  const int fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    dup2(fd, 1);
    dup2(fd, 2);
  }
  execv(argv[0], argv);
  _exit(127);
}

/* the child's exit, once (0 while running). With SIGCHLD ignored (RetroArch
 * does that once its narrator has spoken) the kernel reaps the child itself:
 * waitpid fails with ECHILD and the status stays unknown (-1). */
static int child_done(void) {
  if (child <= 0)
    return 1;
  int st;
  const pid_t r = waitpid(child, &st, WNOHANG);
  if (r == 0)
    return 0;
  if (r == child)
    child_status = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
  else
    child_status = -1;
  child = -1;
  return 1;
}

/* ---- options ------------------------------------------------------------------------- */

static struct retro_core_option_v2_category categories[] = {
  { NULL, NULL, NULL },
};

static struct retro_core_option_v2_definition definitions[] = {
  { "gtasa_fps", "Fotogrammi al secondo", NULL,
    "30 fissi e' la meta' esatta dei 60 Hz del pannello: movimento regolare. 60 solo se il "
    "gioco ci arriva davvero.", NULL, NULL,
    { { "30", NULL }, { "60", NULL }, { NULL, NULL } }, "30" },
  { "gtasa_scale", "Risoluzione di rendering", NULL,
    "Percentuale della risoluzione del pannello (640x480) a cui il gioco disegna; il compositor "
    "ingrandisce l'immagine. Sotto 100 la GPU lavora meno.", NULL, NULL,
    { { "100", "100% (640x480)" }, { "90", "90%" }, { "80", "80%" }, { "75", "75% (480x360)" },
      { "66", "66%" }, { "50", "50% (320x240)" }, { NULL, NULL } }, "100" },
  { "gtasa_perf", "Governor performance", NULL,
    "CPU, GPU e memoria alla frequenza massima: sempre, solo durante il caricamento o mai.",
    NULL, NULL,
    { { "2", "Sempre" }, { "1", "Solo avvio" }, { "0", "No" }, { NULL, NULL } }, "2" },
  { "gtasa_swap_ab", "Scambia A e B", NULL,
    "Se il tasto in basso e quello a destra risultano invertiti.", NULL, NULL,
    { { "0", "No" }, { "1", "Si" }, { NULL, NULL } }, "0" },
  { "gtasa_show_fps", "Contatore FPS", NULL, "Contatore in alto a sinistra.", NULL, NULL,
    { { "0", "No" }, { "1", "Si" }, { NULL, NULL } }, "0" },
  { "gtasa_stats", "Statistiche", NULL,
    "Misure ogni 5 secondi in ROMs/gtasa/stats.log (fps, CPU per thread, GPU, RAM).", NULL, NULL,
    { { "0", "No" }, { "1", "Si" }, { NULL, NULL } }, "0" },
  { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 options_v2 = { categories, definitions };

static const char *option(const char *key, const char *fallback) {
  struct retro_variable v = { key, NULL };
  if (env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value && *v.value)
    return v.value;
  return fallback;
}

/* only values from our own lists go on the runner's command line */
static const char *checked(const char *key, const char *fallback) {
  const char *v = option(key, fallback);
  for (const struct retro_core_option_v2_definition *d = definitions; d->key; d++) {
    if (strcmp(d->key, key) != 0)
      continue;
    for (const struct retro_core_option_value *o = d->values; o->value; o++)
      if (!strcmp(o->value, v))
        return v;
  }
  return fallback;
}

/* ---- libretro API ---------------------------------------------------------------------- */

void retro_set_environment(retro_environment_t cb) {
  env_cb = cb;
  bool no_game = true;
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
  unsigned version = 0;
  if (cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && version >= 2) {
    cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options_v2);
  } else {
    static const struct retro_variable vars[] = {
      { "gtasa_fps", "Fotogrammi al secondo; 30|60" },
      { "gtasa_scale", "Risoluzione di rendering (%); 100|90|80|75|66|50" },
      { "gtasa_perf", "Governor performance (2 sempre, 1 avvio, 0 no); 2|1|0" },
      { "gtasa_swap_ab", "Scambia A e B; 0|1" },
      { "gtasa_show_fps", "Contatore FPS; 0|1" },
      { "gtasa_stats", "Statistiche; 0|1" },
      { NULL, NULL },
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
  }
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info) {
  memset(info, 0, sizeof(*info));
  info->library_name = "GTA San Andreas";
  info->library_version = "1.0";
  info->valid_extensions = "";
  info->need_fullpath = false;
  info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
  memset(info, 0, sizeof(*info));
  info->geometry.base_width = W;
  info->geometry.base_height = H;
  info->geometry.max_width = W;
  info->geometry.max_height = H;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
  info->timing.fps = FPS;
  info->timing.sample_rate = 44100;
}

void retro_init(void) {
  unsigned v = 0;
  msg_ext = env_cb && env_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &v) && v >= 1;
  for (int i = 0; i < W * H; i++)
    pixels[i] = 0xff101418u;
}

void retro_deinit(void) {}
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
/* Restart re-checks the folder; while a child of ours runs, its state goes on */
void retro_reset(void) {
  if (child_done())
    set_state(ST_INIT);
}

bool retro_load_game(const struct retro_game_info *game) {
  (void)game;
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
  child_done(); /* a child of an earlier load: reaped, not followed */
  child = -1;
  frame = 0;
  hold_until = 0;
  skip_import = 0;
  import_failed = 0;
  set_state(ST_INIT);
  return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num) {
  (void)type; (void)info; (void)num;
  return false;
}

void retro_unload_game(void) {}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }
void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

static int start_pressed(void) {
  const int held = input_cb && (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START) ||
                                input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A));
  const int edge = held && !start_held;
  start_held = held;
  return edge;
}

static void begin_countdown(void) {
  show(2, "GTA: San Andreas parte tra 2 secondi (Menu rapido > Opzioni per le impostazioni)");
  set_state(ST_COUNTDOWN);
}

static void step(void) {
  const int start = start_pressed();
  char line[512];
  switch (state) {
    case ST_INIT:
      if (frame < hold_until)
        break; /* let the last message be read */
      import_failed = 0;
      if (import_running()) {
        /* started by an earlier session of the core: follow it */
        attached = 1;
        show(3, "Installazione di GTA: San Andreas in corso...");
        set_state(ST_IMPORT);
      } else if (archives_waiting() && !skip_import) {
        /* archives waiting come first: a new copy, or the retry of an
         * import that stopped halfway (a successful one moves them aside) */
        char *argv[] = { (char *)runner(), "import", NULL };
        attached = 0;
        child = spawn(argv);
        if (child < 0) {
          show(60, "Installazione non avviata (%s)  -  START per riprovare", strerror(errno));
          set_state(ST_STOPPED);
          break;
        }
        show(3, "Installazione di GTA: San Andreas dall'APK e dall'OBB...");
        set_state(ST_IMPORT);
      } else if (installed()) {
        read_first_line("last-error.txt", line, sizeof(line));
        if (line[0]) {
          show(30, "Ultima partita chiusa per un errore: %s  -  START per riprovare", line);
          set_state(ST_LAST_ERROR);
        } else {
          begin_countdown();
        }
      } else {
        show(60, "Copia l'APK 2.11.311 arm64-v8a e l'OBB in ROMs/gtasa (vedi LEGGIMI), poi START");
        set_state(ST_STOPPED);
      }
      break;

    case ST_LAST_ERROR:
      if (start) {
        remove_last_error();
        begin_countdown();
      } else if (frame - last_shown > 25 * FPS) {
        show(30, "%s", message);
      }
      break;

    case ST_IMPORT: {
      /* ours: the child's exit; attached: the lock let go */
      const int done = attached ? !import_running() : child_done();
      if (!done && frame % FPS != 0)
        break;
      read_first_line("import-status.txt", line, sizeof(line));
      /* another import got there first (3, or an unknown status while the
       * lock is taken): follow that one */
      if (done && !attached && (child_status == 3 || (child_status < 0 && import_running()))) {
        attached = 1;
        break;
      }
      if (done) {
        if ((attached || child_status <= 0) && !strncmp(line, "done ", 5)) {
          const char *msg = strchr(line + 5, ' ');
          show(5, "%s", msg ? msg + 1 : "Installazione completata");
          set_state(ST_INIT); /* now installed: on to the countdown */
          hold_until = frame + 4 * FPS;
        } else {
          const char *msg = !strncmp(line, "fail ", 5) ? strchr(line + 5, ' ') : NULL;
          const char *why = msg ? msg + 1
                            : !strncmp(line, "run ", 4) ? "interrotta prima della fine"
                                                        : "vedi ROMs/gtasa/gtasa-import.log";
          import_failed = 1;
          if (installed())
            show(60, "Installazione non riuscita: %s  -  START avvia il gioco gia' installato", why);
          else
            show(60, "Installazione non riuscita: %s  -  START per riprovare", why);
          set_state(ST_STOPPED);
        }
      } else if (!strncmp(line, "run ", 4)) {
        int pct = 0;
        char what[400] = "";
        sscanf(line + 4, "%d %399[^\n]", &pct, what);
        show(2, "Installazione %d%%: %s", pct, what);
      }
      break;
    }

    case ST_COUNTDOWN:
      if (frame - state_frame >= COUNTDOWN_FRAMES) {
        char fps[16], scale[16], perf[16], swap[16], showfps[16], stats[16];
        snprintf(fps, sizeof(fps), "fps=%s", checked("gtasa_fps", "30"));
        snprintf(scale, sizeof(scale), "scale=%s", checked("gtasa_scale", "100"));
        snprintf(perf, sizeof(perf), "perf=%s", checked("gtasa_perf", "2"));
        snprintf(swap, sizeof(swap), "swap_ab=%s", checked("gtasa_swap_ab", "0"));
        snprintf(showfps, sizeof(showfps), "show_fps=%s", checked("gtasa_show_fps", "0"));
        snprintf(stats, sizeof(stats), "stats=%s", checked("gtasa_stats", "0"));
        char *argv[] = { (char *)runner(), "start", fps, scale, perf, swap, showfps, stats, NULL };
        child = spawn(argv);
        if (child < 0) {
          show(60, "Avvio non riuscito (%s)  -  START per riprovare", strerror(errno));
          set_state(ST_STOPPED);
          break;
        }
        set_state(ST_LAUNCH);
      }
      break;

    case ST_LAUNCH:
      /* the service stops RetroArch: normally nothing after this runs (an
       * unknown status waits for the timeout) */
      if (child_done() && child_status > 0) {
        show(60, "Avvio non riuscito (codice %d): vedi ROMs/gtasa/launcher.log  -  START per riprovare",
             child_status);
        set_state(ST_STOPPED);
      } else if (frame - state_frame > LAUNCH_TIMEOUT_FRAMES) {
        show(60, "Il gioco non e' partito: vedi ROMs/gtasa/launcher.log  -  START per riprovare");
        set_state(ST_STOPPED);
      }
      break;

    case ST_STOPPED:
      if (start) {
        /* an import that keeps failing must not lock out what is installed */
        skip_import = import_failed && installed();
        if (skip_import)
          remove_last_error(); /* the import's own error, just read */
        set_state(ST_INIT);
      }
      else if (frame - last_shown > 50 * FPS)
        show(60, "%s", message);
      break;
  }
}

void retro_run(void) {
  if (poll_cb)
    poll_cb();
  frame++;
  step();
  if (video_cb)
    video_cb(pixels, W, H, W * sizeof(uint32_t));
  if (audio_batch_cb)
    audio_batch_cb(silence, sizeof(silence) / sizeof(silence[0]) / 2);
}
