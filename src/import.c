/* import.c -- install the game from the user's own APK and OBB
 *
 *   gtasa --import [-d DATA_DIR]
 *
 * Looks for *.apk and *.obb in the data folder and in its import/
 * subfolder. From the APK that carries lib/arm64-v8a/libGame.so: that and
 * libc++_shared.so; from every APK: assets/; from main.*.obb, then
 * patch.*.obb: everything (the layout gtasa_nx documents). Files land
 * under the data folder; the archives move to import/originali/ when all
 * went well, so the import never runs twice and nothing is deleted.
 *
 * Progress goes to import-status.txt as one line, "STATE PERCENT MESSAGE"
 * with STATE run, done or fail, rewritten atomically; the launcher core
 * shows it in RetroArch. Messages are Italian: the user reads them.
 *
 * One import at a time per folder: an exclusive flock on .import.lock,
 * held until the process exits. A second import (a launcher restarted
 * mid-import starts one) exits with 3 and leaves the status files to the
 * first. rf35h-gtasa passes its own descriptor in GTASA_LOCK_FD, so the
 * lock also covers the check that follows; the launcher reads "lock free"
 * as "the import and its check are over".
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "import.h"
#include "platform_util.h"
#include "util.h"
#include "zipx.h"

#define LIB_DIR "lib/arm64-v8a/"
#define LIB32_DIR "lib/armeabi-v7a/"

typedef struct {
  char path[1024];
  int is_obb;
  int has_game_lib;  /* lib/arm64-v8a/libGame.so */
  int has_game_lib32;
  uint64_t bytes;    /* what this archive contributes */
} Source;

typedef struct {
  const char *dir;
  Source src[16];
  int nsrc;
  uint64_t total, done, last_report_done;
  uint64_t entry_base; /* bytes done before the current entry */
  uint64_t last_report_ns;
  const char *current;
} Import;

/* ---- status line --------------------------------------------------------------------- */

static void status(Import *im, const char *state, int percent, const char *fmt, ...) {
  char msg[512];
  va_list va;
  va_start(va, fmt);
  vsnprintf(msg, sizeof(msg), fmt, va);
  va_end(va);
  char tmp[1100], final[1100];
  snprintf(tmp, sizeof(tmp), "%s/import-status.txt.tmp", im->dir);
  snprintf(final, sizeof(final), "%s/import-status.txt", im->dir);
  FILE *f = fopen(tmp, "w");
  if (f) {
    fprintf(f, "%s %d %s\n", state, percent, msg);
    fclose(f);
    rename(tmp, final);
  }
  if (strcmp(state, "run") != 0 || percent == 0)
    debugPrintf("import: %s %d %s\n", state, percent, msg);
}

static int fail(Import *im, const char *fmt, ...) {
  char msg[512];
  va_list va;
  va_start(va, fmt);
  vsnprintf(msg, sizeof(msg), fmt, va);
  va_end(va);
  status(im, "fail", 0, "%s", msg);
  char path[1100];
  snprintf(path, sizeof(path), "%s/last-error.txt", im->dir);
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "Importazione non riuscita: %s\n", msg);
    fclose(f);
  }
  fprintf(stderr, "gtasa --import: %s\n", msg);
  return 2;
}

static void progress_cb(uint64_t entry_done, void *arg) {
  Import *im = arg;
  im->done = im->entry_base + entry_done;
  const uint64_t now = now_ns();
  if (now - im->last_report_ns < 250000000ull)
    return;
  im->last_report_ns = now;
  const int pct = im->total ? (int)(im->done * 100 / im->total) : 0;
  status(im, "run", pct > 99 ? 99 : pct, "Estrazione: %s", im->current);
}

/* ---- files ---------------------------------------------------------------------------- */

static int ends_with(const char *s, const char *suffix) {
  const size_t a = strlen(s), b = strlen(suffix);
  return a >= b && !strcasecmp(s + a - b, suffix);
}

static int mkdirs_for(const char *path) {
  char tmp[2048];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
      return -1;
    *p = '/';
  }
  return 0;
}

static void collect(Import *im, const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d)) && im->nsrc < (int)(sizeof(im->src) / sizeof(im->src[0]))) {
    const int apk = ends_with(e->d_name, ".apk"), obb = ends_with(e->d_name, ".obb");
    if (!apk && !obb)
      continue;
    Source *s = &im->src[im->nsrc++];
    memset(s, 0, sizeof(*s));
    snprintf(s->path, sizeof(s->path), "%s/%s", dir, e->d_name);
    s->is_obb = obb;
  }
  closedir(d);
}

/* APKs first, then OBBs by name: main.* before patch.*, so a patch wins */
static int cmp_source(const void *a, const void *b) {
  const Source *x = a, *y = b;
  if (x->is_obb != y->is_obb)
    return x->is_obb - y->is_obb;
  const char *bx = strrchr(x->path, '/'), *by = strrchr(y->path, '/');
  return strcasecmp(bx ? bx + 1 : x->path, by ? by + 1 : y->path);
}

/* ---- planning: what each archive holds -------------------------------------------------- */

typedef struct {
  Import *im;
  Source *s;
} ScanArg;

static int scan_cb(Zip *z, const ZipEntry *e, void *arg) {
  (void)z;
  ScanArg *a = arg;
  if (a->s->is_obb) {
    a->s->bytes += e->size;
  } else if (!strcmp(e->name, LIB_DIR "libGame.so")) {
    a->s->has_game_lib = 1;
    a->s->bytes += e->size;
  } else if (!strcmp(e->name, LIB_DIR "libc++_shared.so")) {
    a->s->bytes += e->size;
  } else if (!strcmp(e->name, LIB32_DIR "libGame.so")) {
    a->s->has_game_lib32 = 1;
  } else if (!strncmp(e->name, "assets/", 7)) {
    a->s->bytes += e->size;
  }
  return 0;
}

/* ---- extraction -------------------------------------------------------------------------- */

typedef struct {
  Import *im;
  Source *s;
  char why[512];
} ExtractArg;

static int extract_to(Import *im, Zip *z, const ZipEntry *e, const char *rel, char *why, size_t whylen) {
  char dest[2048];
  snprintf(dest, sizeof(dest), "%s/%s", im->dir, rel);
  if (rel[strlen(rel) - 1] == '/') { /* a directory entry */
    mkdirs_for(dest);
    return 0;
  }
  if (mkdirs_for(dest) < 0) {
    snprintf(why, whylen, "cartella per %s: %s", rel, strerror(errno));
    return -1;
  }
  im->current = rel;
  im->entry_base = im->done;
  if (zip_extract(z, e, dest, progress_cb, im, why, whylen) < 0)
    return -1;
  im->done = im->entry_base + e->size;
  return 0;
}

static int extract_cb(Zip *z, const ZipEntry *e, void *arg) {
  ExtractArg *a = arg;
  const char *rel = NULL;
  if (a->s->is_obb) {
    rel = e->name;
  } else if (!strcmp(e->name, LIB_DIR "libGame.so")) {
    rel = "libGame.so";
  } else if (!strcmp(e->name, LIB_DIR "libc++_shared.so")) {
    rel = "libc++_shared.so";
  } else if (!strncmp(e->name, "assets/", 7) && e->name[7]) {
    rel = e->name + 7;
  }
  if (!rel)
    return 0;
  if (!zip_name_safe(rel)) {
    snprintf(a->why, sizeof(a->why), "nome non valido nell'archivio: %.200s", e->name);
    return -1;
  }
  return extract_to(a->im, z, e, rel, a->why, sizeof(a->why)) < 0 ? -1 : 0;
}

/* ---- after the import ------------------------------------------------------------------- */

static int copy_file(const char *from, const char *to) {
  FILE *in = fopen(from, "rb");
  if (!in)
    return -1;
  FILE *out = fopen(to, "wb");
  if (!out) {
    fclose(in);
    return -1;
  }
  char buf[65536];
  size_t n;
  int ok = 1;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    if (fwrite(buf, 1, n, out) != n)
      ok = 0;
  fclose(in);
  if (fclose(out) != 0)
    ok = 0;
  return ok ? 0 : -1;
}

/* gtasa_nx's tip against shader stutter: compile the whole list during the
 * loading screen instead of the short ones (Mesa's disk cache keeps the
 * results, so only the first boot is slower). Originals kept as *.orig. */
static void full_shader_lists(Import *im) {
  char full[1100], small[1100], orig[1200];
  snprintf(full, sizeof(full), "%s/scache.txt", im->dir);
  if (access(full, R_OK) != 0)
    return;
  static const char *const lists[] = { "scache_small.txt", "scache_small_low.txt" };
  for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
    snprintf(small, sizeof(small), "%s/%s", im->dir, lists[i]);
    snprintf(orig, sizeof(orig), "%s.orig", small);
    if (access(small, F_OK) == 0 && access(orig, F_OK) != 0)
      rename(small, orig);
    if (copy_file(full, small) == 0)
      debugPrintf("import: %s = scache.txt (all shaders compiled while loading)\n", lists[i]);
  }
}

static void archive_sources(Import *im) {
  char dir[1100], dest[2200];
  snprintf(dir, sizeof(dir), "%s/import", im->dir);
  mkdir(dir, 0755);
  snprintf(dir, sizeof(dir), "%s/import/originali", im->dir);
  mkdir(dir, 0755);
  for (int i = 0; i < im->nsrc; i++) {
    const char *base = strrchr(im->src[i].path, '/');
    snprintf(dest, sizeof(dest), "%s/%s", dir, base ? base + 1 : im->src[i].path);
    if (rename(im->src[i].path, dest) != 0)
      debugPrintf("import: cannot move %s aside: %s\n", im->src[i].path, strerror(errno));
  }
}

static int lock_held;

int import_lock(const char *dir) {
  if (lock_held)
    return 0;
  int fd = -1;
  const char *inherited = getenv("GTASA_LOCK_FD");
  if (inherited && *inherited) {
    fd = atoi(inherited);
    if (fd < 0 || fcntl(fd, F_GETFD) < 0)
      fd = -1;
  }
  if (fd < 0) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/.import.lock", dir);
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  }
  if (fd < 0)
    return 0; /* an unwritable folder: the import itself will say so */
  while (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    if (errno == EWOULDBLOCK)
      return -1;
    if (errno != EINTR)
      break;
  }
  lock_held = 1;
  return 0; /* the descriptor stays open: the lock lasts as long as the process */
}

int import_pending(const char *data_dir) {
  Import im;
  memset(&im, 0, sizeof(im));
  im.dir = data_dir;
  char sub[1100];
  collect(&im, data_dir);
  snprintf(sub, sizeof(sub), "%s/import", data_dir);
  collect(&im, sub);
  return im.nsrc;
}

int import_run(const char *data_dir, int full_shaders) {
  if (import_lock(data_dir) < 0) {
    fprintf(stderr, "gtasa --import: un'importazione e' gia' in corso in %s\n", data_dir);
    return 3;
  }
  static Import im; /* big: keep it off the stack */
  memset(&im, 0, sizeof(im));
  im.dir = data_dir;
  char sub[1100];
  collect(&im, data_dir);
  snprintf(sub, sizeof(sub), "%s/import", data_dir);
  collect(&im, sub);
  qsort(im.src, (size_t)im.nsrc, sizeof(Source), cmp_source);
  status(&im, "run", 0, "Lettura degli archivi...");

  int apk = -1, obbs = 0, apk32 = 0, napk = 0;
  for (int i = 0; i < im.nsrc; i++) {
    Zip z;
    char why[512];
    if (zip_open(&z, im.src[i].path, why, sizeof(why)) < 0)
      return fail(&im, "%s non si legge: %s", strrchr(im.src[i].path, '/') + 1, why);
    ScanArg a = { &im, &im.src[i] };
    const int rc = zip_foreach(&z, scan_cb, &a);
    zip_close(&z);
    if (rc < 0)
      return fail(&im, "%s e' danneggiato (directory ZIP illeggibile)", strrchr(im.src[i].path, '/') + 1);
    if (im.src[i].is_obb) {
      obbs++;
    } else {
      napk++;
      if (im.src[i].has_game_lib && apk < 0)
        apk = i;
      apk32 |= im.src[i].has_game_lib32;
    }
  }
  if (apk < 0) {
    if (apk32)
      return fail(&im, "l'APK e' a 32 bit (armeabi-v7a): serve la 2.11.311 per arm64-v8a");
    if (!napk)
      return fail(&im, "manca l'APK della 2.11.311 arm64-v8a in ROMs/gtasa");
    return fail(&im, "nessun APK contiene lib/arm64-v8a/libGame.so");
  }
  if (!obbs)
    return fail(&im, "manca l'OBB (main.*.obb) in ROMs/gtasa");

  for (int i = 0; i < im.nsrc; i++)
    im.total += im.src[i].bytes;
  struct statvfs vfs;
  if (statvfs(data_dir, &vfs) == 0) {
    const uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    const uint64_t need = im.total + (64ull << 20);
    if (free_bytes < need)
      return fail(&im, "spazio insufficiente: servono %llu MB, liberi %llu MB",
                  (unsigned long long)(need >> 20), (unsigned long long)(free_bytes >> 20));
  }

  for (int i = 0; i < im.nsrc; i++) {
    Source *s = &im.src[i];
    if (!s->is_obb && s->bytes == 0)
      continue; /* a split APK without libs or assets */
    Zip z;
    ExtractArg a;
    memset(&a, 0, sizeof(a));
    a.im = &im;
    a.s = s;
    if (zip_open(&z, s->path, a.why, sizeof(a.why)) < 0)
      return fail(&im, "%s: %s", strrchr(s->path, '/') + 1, a.why);
    const int rc = zip_foreach(&z, extract_cb, &a);
    zip_close(&z);
    if (rc != 0)
      return fail(&im, "%s", a.why[0] ? a.why : "archivio danneggiato");
  }
  if (full_shaders)
    full_shader_lists(&im);
  archive_sources(&im);
  status(&im, "done", 100, "Importazione completata (%llu MB). Gli archivi sono in ROMs/gtasa/import/originali",
         (unsigned long long)(im.total >> 20));
  return 0;
}
