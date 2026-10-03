/* platform_config.c -- RF35H defaults and parser for the platform settings
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "platform_config.h"

PlatformConfig pconfig;

void pconfig_defaults(void) {
  memset(&pconfig, 0, sizeof(pconfig));
  pconfig.vsync = 1;
  pconfig.glthread = 1;
  pconfig.gl_no_error = 0;
  pconfig.gl_state_cache = 1;
  pconfig.pin_threads = 0;
  pconfig.cpu_logic = 0x1;
  pconfig.cpu_render = 0x2;
  pconfig.cpu_driver = 0x4;
  pconfig.cpu_other = 0x8;
  pconfig.perf_mode = 2;
  pconfig.cpu_turbo = 0;
  pconfig.stats = 0;
  pconfig.stats_interval = 5;
  pconfig.report_ram_mb = 0;
  pconfig.network = 0;
  pconfig.android_log = 0;
  pconfig.exit_combo = 1;
  pconfig.stick_deadzone = 12;
  pconfig.malloc_arenas = 2;
  pconfig.render_scale = 100;
  /* the game's own profile for weak GPUs (it keeps it for Adreno 225 and
   * PowerVR SGX 540 class chips), plus half the draw distance for the A35 */
  pconfig.game_visual_fx = 0;
  pconfig.game_shadows = 0;
  pconfig.game_draw_distance = 50;
  pconfig.game_car_reflections = 1;
  pconfig.texture_lod_bias = 0;
}

/* gtasa_nx's options and the values the RF35H wants for them. Upstream's
 * own defaults (trilinear on, no 30 fps cap, peds kept off-screen) are tuned
 * for the Switch; these apply whenever the file does not say otherwise. */
typedef struct {
  const char *name;
  int *field;
  int rf35h;
  const char *comment;
} UpstreamKey;

static UpstreamKey upstream_keys[] = {
  { "screen_width",          &config.screen_width,          -1, "-1 = risoluzione del pannello (640x480)" },
  { "screen_height",         &config.screen_height,         -1, NULL },
  { "trilinear_filter",      &config.trilinear_filter,       0, "1 = filtro trilineare: piu' banda sul G31" },
  { "show_fps",              &config.show_fps,               0, "1 = contatore FPS (legge lo stato GL: costa con glthread)" },
  { "fps_cap_30",            &config.fps_cap_30,             1, "1 = 30 fps fissi, meta' esatta dei 60 Hz del pannello" },
  { "auto_boot_delay",       &config.auto_boot_delay,        0, NULL },
  { "ps2_corona_rotation",   &config.ps2_corona_rotation,    1, "1 = coronas ruotanti come su PS2" },
  { "ps2_color_filter",      &config.ps2_color_filter,       1, "1 = filtro colore PS2 (nessuna passata in piu')" },
  { "sprint_any_surface",    &config.sprint_any_surface,     0, NULL },
  { "remove_air_resistance", &config.remove_air_resistance,  0, NULL },
  { "show_wanted_stars",     &config.show_wanted_stars,      0, NULL },
  { "disable_ped_spec",      &config.disable_ped_spec,       1, "1 = niente speculare sui pedoni (shader piu' leggero)" },
  { "no_offscreen_despawn",  &config.no_offscreen_despawn,   0, "1 = auto/pedoni restano fuori schermo: CPU e RAM in piu'" },
  { "mobile_widgets",        &config.mobile_widgets,         0, "0 = nasconde i comandi touch" },
  { "fuzzy_seek",            &config.fuzzy_seek,             1, "1 = cambio stazione radio piu' rapido" },
};

typedef struct {
  const char *name;
  int *field;
  const char *comment;
} PlatformKey;

static PlatformKey platform_keys[] = {
  { "render_scale",   &pconfig.render_scale,   "risoluzione di rendering in % di quella del pannello (50-100):\n# sotto 100 il compositor ingrandisce l'immagine e la GPU lavora meno" },
  { "game_visual_fx", &pconfig.game_visual_fx, "impostazioni del gioco al primo avvio (e quando nel menu le reimposti);\n"
                                               "# poi valgono quelle scelte nel menu, salvate in gta_sa.set. -1 = quelle del gioco,\n"
                                               "# che su questa GPU partirebbe dal massimo (FX alto, ombre in tempo reale,\n"
                                               "# distanza 100, riflessi delle auto 3).\n"
                                               "# effetti visivi: 0 bassi .. 3 molto alti" },
  { "game_shadows",   &pconfig.game_shadows,   "ombre: 0 no, 1 classiche, 2 in tempo reale" },
  { "game_draw_distance", &pconfig.game_draw_distance, "distanza visiva: 0..100" },
  { "game_car_reflections", &pconfig.game_car_reflections, "riflessi delle auto: 0 no, 1 statici, 2-3 scena ridisegnata in una texture 1024x512" },
  { "texture_lod_bias", &pconfig.texture_lod_bias, "1 = texture piu' nitide come nel gioco (bias -0.5), 0 = nessun bias:\n# meno banda e meno sfarfallio a 640x480" },
  { "vsync",          &pconfig.vsync,          "1 = attende il vblank allo swap, 0 = no, -1 = decide il gioco" },
  { "glthread",       &pconfig.glthread,       "1 = driver GL di Mesa su un thread separato" },
  { "gl_no_error",    &pconfig.gl_no_error,    "1 = MESA_NO_ERROR: niente controlli d'errore GL (sperimentale)" },
  { "gl_state_cache", &pconfig.gl_state_cache, "1 = scarta le chiamate GL che non cambiano stato" },
  { "pin_threads",    &pconfig.pin_threads,    "1 = applica i cpu_* qui sotto (maschere: bit n = CPU n)" },
  { "cpu_logic",      &pconfig.cpu_logic,      NULL },
  { "cpu_render",     &pconfig.cpu_render,     NULL },
  { "cpu_driver",     &pconfig.cpu_driver,     NULL },
  { "cpu_other",      &pconfig.cpu_other,      NULL },
  { "perf_mode",      &pconfig.perf_mode,      "governor performance per CPU, GPU e RAM: 0 = no, 1 = solo avvio, 2 = sempre" },
  { "cpu_turbo",      &pconfig.cpu_turbo,      "1 = frequenze turbo della CPU (boost di cpufreq) insieme a perf_mode: piu' calore" },
  { "stats",          &pconfig.stats,          "1 = misure in stats.log ogni stats_interval secondi" },
  { "stats_interval", &pconfig.stats_interval, NULL },
  { "report_ram_mb",  &pconfig.report_ram_mb,  "RAM dichiarata al gioco in MB, 0 = quella reale" },
  { "network",        &pconfig.network,        "0 = rete chiusa al gioco (Social Club)" },
  { "android_log",    &pconfig.android_log,    "1 = copia i log Android del gioco in gtasa.log" },
  { "exit_combo",     &pconfig.exit_combo,     "1 = SELECT+START per un secondo: esce" },
  { "stick_deadzone", &pconfig.stick_deadzone, "zona morta degli stick, in percento" },
  { "swap_ab",        &pconfig.swap_ab,        "1 = scambia i tasti in basso e a destra (se A/B risultano invertiti)" },
  { "swap_xy",        &pconfig.swap_xy,        "1 = scambia i tasti a sinistra e in alto" },
  { "malloc_arenas",  &pconfig.malloc_arenas,  "arene di malloc: meno = meno RAM sprecata" },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

int pconfig_load(const char *path) {
  int seen[COUNT(upstream_keys)] = { 0 };
  FILE *f = fopen(path, "r");
  if (f) {
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      char *name = line;
      while (*name && isspace((unsigned char)*name))
        name++;
      if (*name == '#' || !*name)
        continue;
      char *end = name;
      while (*end && !isspace((unsigned char)*end))
        end++;
      if (!*end)
        continue;
      *end++ = 0;
      const int value = atoi(end);
      for (size_t i = 0; i < COUNT(platform_keys); i++)
        if (!strcmp(name, platform_keys[i].name))
          *platform_keys[i].field = value;
      for (size_t i = 0; i < COUNT(upstream_keys); i++)
        if (!strcmp(name, upstream_keys[i].name))
          seen[i] = 1;
    }
    fclose(f);
  }
  /* upstream read_config() has already filled `config`; give the keys the
   * file does not mention the RF35H value instead of upstream's default */
  for (size_t i = 0; i < COUNT(upstream_keys); i++)
    if (!seen[i])
      *upstream_keys[i].field = upstream_keys[i].rf35h;
  if (pconfig.stats_interval < 1)
    pconfig.stats_interval = 1;
  if (pconfig.stick_deadzone < 0 || pconfig.stick_deadzone > 60)
    pconfig.stick_deadzone = 12;
  if (pconfig.render_scale < 50 || pconfig.render_scale > 100)
    pconfig.render_scale = 100;
  /* game_* ranges are checked against the game's own table when applied */
  return f ? 0 : -1;
}

int pconfig_write_alsoft(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  fputs("# OpenAL Soft per GTA: San Andreas sulla RF35H (letto tramite ALSOFT_CONF).\n"
        "# Audio del gioco a 22 kHz portato a 44,1 kHz: un resampler lineare basta\n"
        "# per gli altoparlanti e lascia CPU al gioco.\n"
        "[general]\n"
        "drivers = alsa\n"
        "channels = stereo\n"
        "stereo-encoding = basic\n"
        "frequency = 44100\n"
        "sample-type = int16\n"
        "resampler = linear\n"
        "period_size = 1024\n"
        "periods = 3\n"
        "\n[alsa]\n"
        "device = default\n", f);
  return fclose(f) == 0 ? 0 : -1;
}

int pconfig_write_default(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  fputs("# GTA: San Andreas sulla XiFan RF35H.\n"
        "# Una riga per opzione: NOME VALORE. Le righe che iniziano con # sono commenti.\n"
        "\n# --- gioco (opzioni di gtasa_nx) ---\n", f);
  for (size_t i = 0; i < COUNT(upstream_keys); i++) {
    if (upstream_keys[i].comment)
      fprintf(f, "# %s\n", upstream_keys[i].comment);
    fprintf(f, "%s %d\n", upstream_keys[i].name, upstream_keys[i].rf35h);
  }
  fputs("\n# --- piattaforma RF35H ---\n", f);
  PlatformConfig saved = pconfig;
  pconfig_defaults();
  for (size_t i = 0; i < COUNT(platform_keys); i++) {
    if (platform_keys[i].comment)
      fprintf(f, "# %s\n", platform_keys[i].comment);
    fprintf(f, "%s %d\n", platform_keys[i].name, *platform_keys[i].field);
  }
  pconfig = saved;
  return fclose(f) == 0 ? 0 : -1;
}
