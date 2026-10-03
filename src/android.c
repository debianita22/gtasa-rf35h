/* android.c -- the slice of the Android NDK the game reaches for
 *
 * AAsset over plain files in the game directory (with gtasa_nx's index of the
 * immutable trees and its silent-MP3 stand-in), ANativeWindow over the
 * platform window, the log functions, system properties, and inert OpenSL ES
 * symbols for imports that must resolve but are never used.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "android.h"
#include "casefold.h"
#include "config.h"
#include "platform.h"
#include "platform_config.h"
#include "platform_util.h"
#include "util.h"

/* ==========================================================================
 * AAsset
 * ======================================================================== */

typedef struct {
  FILE *f;
  long size;
  void *buffer; /* AAsset_getBuffer */
} Asset;

void *AAssetManager_fromJava_fake(void *env, void *mgr) {
  (void)env; (void)mgr;
  return (void *)1; /* any non-NULL token */
}

/* The engine probes the disk for a loose-file override before most archive
 * reads. data/ and es2/ never change while the game runs, so they are indexed
 * once and the probes are answered from memory (from gtasa_nx). */
static char **asset_index;
static int asset_index_count, asset_index_cap;
static pthread_once_t asset_index_once = PTHREAD_ONCE_INIT;

static void index_add(const char *path) {
  if (asset_index_count == asset_index_cap) {
    const int cap = asset_index_cap ? asset_index_cap * 2 : 1024;
    char **grown = realloc(asset_index, (size_t)cap * sizeof(char *));
    if (!grown)
      return;
    asset_index = grown;
    asset_index_cap = cap;
  }
  char *s = strdup(path);
  if (!s)
    return;
  for (char *p = s; *p; p++)
    *p = (char)tolower((unsigned char)*p);
  asset_index[asset_index_count++] = s;
}

static void index_scan_dir(const char *dir) {
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *e;
  char path[1024];
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if (e->d_type == DT_DIR)
      index_scan_dir(path);
    else
      index_add(path);
  }
  closedir(d);
}

static int index_cmp(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int index_key_cmp(const void *key, const void *elem) {
  return strcmp((const char *)key, *(const char *const *)elem);
}

/* built by the first open, whichever thread makes it; the roots too are
 * found whatever their case (a case-insensitive file system would) */
static void index_build(void) {
  static const char *const roots[] = { "data", "es2" };
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    char spelled[64];
    const char *dir = case_resolve(roots[i], spelled, sizeof(spelled));
    index_scan_dir(dir ? dir : roots[i]);
  }
  if (asset_index_count)
    qsort(asset_index, (size_t)asset_index_count, sizeof(char *), index_cmp);
  debugPrintf("AAsset: indexed %d loose files under data/ and es2/\n", asset_index_count);
}

static int index_maybe_exists(const char *path) {
  pthread_once(&asset_index_once, index_build);
  if (strncasecmp(path, "data/", 5) != 0 && strncasecmp(path, "es2/", 4) != 0)
    return 1;
  char lower[1024];
  size_t i;
  for (i = 0; path[i] && i < sizeof(lower) - 1; i++)
    lower[i] = (char)tolower((unsigned char)path[i]);
  lower[i] = 0;
  return bsearch(lower, asset_index, (size_t)asset_index_count, sizeof(char *), index_key_cmp) != NULL;
}

/* as spelled, then as a case-insensitive file system would find it */
static FILE *open_any_case(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f && errno == ENOENT) {
    char spelled[1024];
    const char *on_disk = case_resolve(path, spelled, sizeof(spelled));
    if (on_disk)
      f = fopen(on_disk, "rb");
  }
  return f;
}

static FILE *open_with_fallback(const char *path) {
  FILE *f = open_any_case(path);
  if (f)
    return f;
  const char *as = strstr(path, "assets/");
  if (as) {
    char alt[1024];
    const size_t pre = (size_t)(as - path);
    if (pre < sizeof(alt)) {
      memcpy(alt, path, pre);
      snprintf(alt + pre, sizeof(alt) - pre, "%s", as + 7);
      f = open_any_case(alt);
    }
  }
  return f;
}

extern const unsigned char silent_mp3[];
extern const unsigned char silent_mp3_end[];

static int is_mp3(const char *p) {
  const size_t n = p ? strlen(p) : 0;
  return n >= 4 && !strcasecmp(p + n - 4, ".mp3");
}

void *AAssetManager_open_fake(void *mgr, const char *path, int mode) {
  (void)mgr; (void)mode;
  if (!path || !index_maybe_exists(path))
    return NULL;
  FILE *f = open_with_fallback(path);
  long size = 0;
  if (!f && is_mp3(path)) {
    /* the engine retries a failed .mp3 open forever: hand it silence */
    f = fmemopen((void *)silent_mp3, (size_t)(silent_mp3_end - silent_mp3), "rb");
    if (f)
      debugPrintf("AAsset: %s missing, silent stand-in\n", path);
  }
  if (!f) {
    debugPrintf("AAsset: open(%s) -> missing\n", path);
    return NULL;
  }
  note_data_open(path);
  setvbuf(f, NULL, _IOFBF, 16 * 1024);
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  Asset *a = calloc(1, sizeof(*a));
  if (!a) {
    fclose(f);
    return NULL;
  }
  a->f = f;
  a->size = size;
  return a;
}

void AAsset_close_fake(void *asset) {
  Asset *a = asset;
  if (!a)
    return;
  fclose(a->f);
  free(a->buffer);
  free(a);
}

int AAsset_read_fake(void *asset, void *buf, size_t count) {
  Asset *a = asset;
  return a ? (int)fread(buf, 1, count, a->f) : -1;
}

long AAsset_seek_fake(void *asset, long off, int whence) {
  Asset *a = asset;
  if (!a || fseek(a->f, off, whence) < 0)
    return -1;
  return ftell(a->f);
}

int64_t AAsset_seek64_fake(void *asset, int64_t off, int whence) {
  return AAsset_seek_fake(asset, (long)off, whence);
}

long AAsset_getLength_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size : 0;
}

int64_t AAsset_getLength64_fake(void *asset) {
  return AAsset_getLength_fake(asset);
}

long AAsset_getRemainingLength_fake(void *asset) {
  Asset *a = asset;
  return a ? a->size - ftell(a->f) : 0;
}

int64_t AAsset_getRemainingLength64_fake(void *asset) {
  return AAsset_getRemainingLength_fake(asset);
}

const void *AAsset_getBuffer_fake(void *asset) {
  Asset *a = asset;
  if (!a)
    return NULL;
  if (!a->buffer) {
    a->buffer = malloc((size_t)a->size + 1);
    if (!a->buffer)
      return NULL;
    const long pos = ftell(a->f);
    fseek(a->f, 0, SEEK_SET);
    const size_t got = fread(a->buffer, 1, (size_t)a->size, a->f);
    fseek(a->f, pos, SEEK_SET);
    if (got != (size_t)a->size) {
      free(a->buffer);
      a->buffer = NULL;
    }
  }
  return a->buffer;
}

int AAsset_openFileDescriptor_fake(void *asset, long *start, long *length) {
  Asset *a = asset;
  if (!a)
    return -1;
  const int fd = dup(fileno(a->f));
  if (fd < 0)
    return -1;
  lseek(fd, 0, SEEK_SET);
  *start = 0;
  *length = a->size;
  return fd;
}

int AAsset_isAllocated_fake(void *asset) {
  (void)asset;
  return 0;
}

typedef struct {
  DIR *d;
  char name[256];
} AssetDir;

void *AAssetManager_openDir_fake(void *mgr, const char *dir) {
  (void)mgr;
  AssetDir *ad = calloc(1, sizeof(*ad));
  if (!ad)
    return NULL;
  ad->d = opendir(dir && *dir ? dir : ".");
  return ad;
}

const char *AAssetDir_getNextFileName_fake(void *dir) {
  AssetDir *ad = dir;
  if (!ad || !ad->d)
    return NULL;
  struct dirent *e;
  while ((e = readdir(ad->d))) {
    if (e->d_type == DT_DIR)
      continue; /* Android lists files only */
    snprintf(ad->name, sizeof(ad->name), "%s", e->d_name);
    return ad->name;
  }
  return NULL;
}

void AAssetDir_rewind_fake(void *dir) {
  AssetDir *ad = dir;
  if (ad && ad->d)
    rewinddir(ad->d);
}

void AAssetDir_close_fake(void *dir) {
  AssetDir *ad = dir;
  if (!ad)
    return;
  if (ad->d)
    closedir(ad->d);
  free(ad);
}

/* ==========================================================================
 * ANativeWindow: one window, the platform's
 * ======================================================================== */

void *ANativeWindow_fromSurface_fake(void *env, void *surface) {
  (void)env; (void)surface;
  int w = 0, h = 0;
  platform_window_size(&w, &h);
  debugPrintf("ANativeWindow_fromSurface -> %dx%d\n", w, h);
  return platform_anative_window();
}

int ANativeWindow_getWidth_fake(void *win) {
  (void)win;
  int w = 0, h = 0;
  platform_window_size(&w, &h);
  return w;
}

int ANativeWindow_getHeight_fake(void *win) {
  (void)win;
  int w = 0, h = 0;
  platform_window_size(&w, &h);
  return h;
}

void ANativeWindow_acquire_fake(void *win) { (void)win; }
void ANativeWindow_release_fake(void *win) { (void)win; }

int ANativeWindow_setBuffersGeometry_fake(void *win, int w, int h, int format) {
  (void)win; (void)format;
  debugPrintf("ANativeWindow_setBuffersGeometry(%d, %d, %d)\n", w, h, format);
  if (w < 0 || h < 0 || (w == 0) != (h == 0))
    return -22; /* BAD_VALUE, as Android answers */
  platform_resize_buffers(w, h); /* 0 x 0: back to the window's own size */
  /* the engine sizes its render targets from OS_ScreenGetWidth/Height */
  platform_window_size(&screen_width, &screen_height);
  return 0;
}

/* ==========================================================================
 * log, properties, abort message
 * ======================================================================== */

enum { ANDROID_LOG_WARN = 5 };

static void android_log(int prio, const char *tag, const char *text) {
  if (pconfig.android_log || prio >= ANDROID_LOG_WARN)
    debugPrintf("%s: %s", tag ? tag : "?", text);
}

int __android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  if (!pconfig.android_log && prio < ANDROID_LOG_WARN)
    return 0;
  char buf[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);
  android_log(prio, tag, buf);
  return 0;
}

int __android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
  if (!pconfig.android_log && prio < ANDROID_LOG_WARN)
    return 0;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, va);
  android_log(prio, tag, buf);
  return 0;
}

int __android_log_write_fake(int prio, const char *tag, const char *text) {
  android_log(prio, tag, text ? text : "");
  return 0;
}

void __android_log_assert_fake(const char *cond, const char *tag, const char *fmt, ...) {
  char buf[1024];
  if (fmt) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
  } else {
    snprintf(buf, sizeof(buf), "assertion \"%s\" failed", cond ? cond : "");
  }
  debugPrintf("FATAL %s: %s\n", tag ? tag : "", buf);
  abort();
}

void __assert2_fake(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("FATAL: assertion failed: %s:%d (%s): %s\n", file, line, func, expr);
  abort();
}

void android_set_abort_message_fake(const char *msg) {
  debugPrintf("abort message: %s\n", msg ? msg : "(null)");
}

int __system_property_get_fake(const char *name, char *value) {
  static const struct {
    const char *name, *value;
  } props[] = {
    { "ro.build.version.sdk", "30" },
    { "ro.build.version.release", "11" },
    { "ro.product.manufacturer", "XiFan" },
    { "ro.product.model", "RF35H" },
    { "ro.product.brand", "XiFan" },
    { "ro.product.device", "rf35h" },
    { "ro.hardware", "rk3326" },
    { "ro.board.platform", "rk3326" },
    { "ro.product.cpu.abi", "arm64-v8a" },
  };
  for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++)
    if (name && !strcmp(name, props[i].name)) {
      strcpy(value, props[i].value);
      return (int)strlen(value);
    }
  value[0] = 0;
  return 0;
}

/* ==========================================================================
 * OpenSL ES: imported, never driven (audio is OpenAL)
 * ======================================================================== */

void *SL_IID_fake;

unsigned slCreateEngine_fake(void) {
  return 0x0000000C; /* SL_RESULT_FEATURE_UNSUPPORTED */
}
