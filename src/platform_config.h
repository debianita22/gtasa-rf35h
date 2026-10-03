/* platform_config.h -- settings of the Linux platform layer
 *
 * They live in the same file as gtasa_nx's own options (upstream config.c
 * reads that file too and ignores the keys it does not know, and so does
 * this parser). Defaults are tuned for the XiFan RF35H.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_PLATFORM_CONFIG_H
#define GTASA_PLATFORM_CONFIG_H

typedef struct {
  int vsync;           /* 1: swaps wait for vblank, 0: never, -1: the game decides */
  int glthread;        /* Mesa's GL worker thread (mesa_glthread) */
  int gl_no_error;     /* MESA_NO_ERROR: skip GL error checking */
  int gl_state_cache;  /* drop redundant GL state calls before they reach Mesa */
  int pin_threads;     /* apply the cpu_* sets below */
  int cpu_logic;       /* CPU masks per thread class (bit n = CPU n) */
  int cpu_render;
  int cpu_driver;
  int cpu_other;
  int perf_mode;       /* performance governors (CPU, GPU, DDR): 0 off, 1 boot, 2 always */
  int cpu_turbo;       /* with perf_mode: cpufreq boost (turbo OPPs, where the kernel has them) */
  int stats;           /* write per-interval measurements to stats.log */
  int stats_interval;  /* seconds */
  int report_ram_mb;   /* _SC_PHYS_PAGES as seen by the game; 0 = real */
  int network;         /* let the Social Club/cloud code open sockets */
  int android_log;     /* copy the game's __android_log output to the log */
  int exit_combo;      /* hold SELECT+START for a second to quit */
  int stick_deadzone;  /* percent */
  int swap_ab;         /* swap the bottom and right face buttons */
  int swap_xy;         /* swap the left and top face buttons */
  int malloc_arenas;   /* glibc malloc arenas; fewer = less RSS overhead */
  int render_scale;    /* percent of the output resolution the game renders at */
  /* the game's own settings (MobileSettings) on the first run and on a menu
   * reset; -1 keeps the game's (see game_tuning.c) */
  int game_visual_fx;       /* 0 low .. 3 very high */
  int game_shadows;         /* 0 off, 1 classic, 2 real-time */
  int game_draw_distance;   /* 0..100 */
  int game_car_reflections; /* 0 off, 1 static, 2-3 rendered every frame */
  int texture_lod_bias;     /* 1: the game's -0.5 bias on diffuse textures, 0: none */
} PlatformConfig;

extern PlatformConfig pconfig;

void pconfig_defaults(void);
int pconfig_load(const char *path);

/* Write a complete config: gtasa_nx's options with the RF35H values, then
 * ours, commented. Returns 0 on success. */
int pconfig_write_default(const char *path);

/* OpenAL Soft settings for the device (stereo, 44.1 kHz, linear resampler,
 * ALSA); main.c points ALSOFT_CONF at the file. Returns 0 on success. */
int pconfig_write_alsoft(const char *path);

#endif
