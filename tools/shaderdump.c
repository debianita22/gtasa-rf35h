/* shaderdump.c -- the game's GLSL for each shader flag set (development tool)
 *
 *   shaderdump GAME_DIR SCACHE_FILE OUT_DIR [etc]
 *
 * Runs on aarch64 (qemu-aarch64 on a PC). Loads libc++_shared.so and
 * libGame.so from GAME_DIR with the port's loader and calls the game's own
 * RQShader::BuildSource(flags, &pixel, &vertex) for every hexadecimal flag
 * set in SCACHE_FILE (one per line, as in scache.txt), writing
 * OUT_DIR/<flags>.frag and OUT_DIR/<flags>.vert.
 *
 * The generator also reads RQCaps and RQMaxBones, which RQ_Command_rqInit
 * fills from the GL strings. They are set here as on Panfrost: GL_OES_depth24
 * and GL_OES_packed_depth_stencil present, 128 bones (Panfrost's 4096 vertex
 * uniform vectors). "etc" also sets the flag rqInit raises when the driver
 * offers none of DXT, ATC and PVRTC (the game then reads the etc texture
 * database); without it the driver is taken to offer S3TC/DXT.
 *
 * Build (see tools/README or tests/run-tests.sh for the flags):
 *   aarch64-linux-gnu-gcc ... -c tools/shaderdump.c
 *   aarch64-linux-gnu-gcc -o shaderdump build/tls_guard.o shaderdump.o <port objects> <libs>
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

#include "so_util.h"

#include "import_table.h"
#include "loader.h"
#include "platform_config.h"
#include "platform_util.h"
#include "tls_guard.h"

/* the platform layer's main.c provides these to the rest of the code */
so_module game_mod;
void hard_exit(void) { _exit(0); }

/* RQCaps byte offsets, from RQ_Command_rqInit in 2.11.311 */
enum {
  CAP_DEPTH24 = 0,
  CAP_PACKED_DEPTH_STENCIL = 1,
  CAP_DXT = 3,
  CAP_NO_DXT_ATC_PVRTC = 13,
};

static int write_file(const char *dir, const char *name, const char *ext, const char *text) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.%s", dir, name, ext);
  FILE *f = fopen(path, "w");
  if (!f)
    return -1;
  /* the generator writes one statement after another: one per line reads better */
  for (const char *p = text; *p; p++) {
    fputc(*p, f);
    if (*p == ';' || *p == '{' || *p == '}')
      fputc('\n', f);
  }
  fputc('\n', f);
  return fclose(f);
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: shaderdump GAME_DIR SCACHE_FILE OUT_DIR [etc]\n");
    return 2;
  }
  const int etc = argc > 4 && !strcmp(argv[4], "etc");
  char why[128];
  if (tls_guard_check(why, sizeof(why)) < 0) {
    fprintf(stderr, "tls_guard: %s\n", why);
    return 1;
  }
  FILE *list = fopen(argv[2], "r");
  if (!list) {
    fprintf(stderr, "%s: %s\n", argv[2], strerror(errno));
    return 1;
  }
  mkdir(argv[3], 0755);
  char out[PATH_MAX];
  if (!realpath(argv[3], out)) {
    fprintf(stderr, "%s: %s\n", argv[3], strerror(errno));
    return 1;
  }
  if (chdir(argv[1]) < 0) {
    fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
    return 1;
  }

  pconfig_defaults();
  so_set_fallback_resolver(imports_fallback);
  import_table_configure();
  static so_module donor;
  if (so_load(&donor, "libc++_shared.so", NULL, 0) < 0 || so_load(&game_mod, "libGame.so", NULL, 0) < 0) {
    fprintf(stderr, "cannot load libc++_shared.so and libGame.so from %s\n", argv[1]);
    return 1;
  }
  so_relocate(&donor);
  so_relocate(&game_mod);
  so_resolve(&donor, import_table, import_table_count, 1);
  so_resolve(&game_mod, import_table, import_table_count, 1);
  so_finalize(&donor);
  so_finalize(&game_mod);
  so_flush_caches(&donor);
  so_flush_caches(&game_mod);

  uint8_t *caps = (uint8_t *)so_try_find_addr_rx(&game_mod, "RQCaps");
  int *max_bones = (int *)so_try_find_addr_rx(&game_mod, "RQMaxBones");
  int (*build)(unsigned, const char **, const char **) =
      (void *)so_try_find_addr_rx(&game_mod, "_ZN8RQShader11BuildSourceEjPPKcS2_");
  if (!caps || !max_bones || !build) {
    fprintf(stderr, "RQCaps, RQMaxBones or RQShader::BuildSource not found: not the 2.11.311 build?\n");
    return 1;
  }
  caps[CAP_DEPTH24] = 1;
  caps[CAP_PACKED_DEPTH_STENCIL] = 1;
  caps[CAP_DXT] = !etc;
  caps[CAP_NO_DXT_ATC_PVRTC] = (uint8_t)etc;
  *max_bones = 128;

  char line[64];
  int n = 0;
  while (fgets(line, sizeof(line), list)) {
    char *end;
    const unsigned long flags = strtoul(line, &end, 16);
    if (end == line)
      continue;
    const char *pixel = NULL, *vertex = NULL;
    build((unsigned)flags, &pixel, &vertex);
    char name[32];
    snprintf(name, sizeof(name), "%08lx", flags);
    if (!pixel || !vertex || write_file(out, name, "frag", pixel) < 0 ||
        write_file(out, name, "vert", vertex) < 0) {
      fprintf(stderr, "%s: no source or cannot write it\n", name);
      return 1;
    }
    n++;
  }
  fclose(list);
  printf("%d shader pairs written to %s (%s texture path)\n", n, out, etc ? "ETC" : "DXT");
  return 0;
}
