/* casefold.c -- file names found the way a case-insensitive file system would
 *
 * gtasa_nx (Switch, FAT/exFAT) and gtasa_vita (exFAT) run the game on file
 * systems that ignore case; Lakka's /storage is ext4. When a lookup of a
 * relative path (the game folder) fails with ENOENT, the read-only shims
 * (fopen for reading, open without O_CREAT, stat, lstat, opendir, AAsset)
 * retry with the spelling found on disk, one directory level at a time. A
 * name spelled as on disk costs nothing. Answers are cached (the engine
 * repeats its requests): a miss until the directory where the search
 * stopped changes, so a file created later is still found. The first few
 * differences are logged, so gtasa.log shows whether the real game ever
 * needs this.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "casefold.h"
#include "util.h"

#define CACHE_SLOTS 4096 /* power of two */
#define LOG_FIRST 20

typedef struct {
  char *asked;
  char *found;           /* NULL: no spelling of it exists... */
  char *miss_dir;        /* ...as long as this directory */
  struct timespec mtime; /* keeps this modification time */
} Entry;

static Entry cache[CACHE_SLOTS];
static int cache_used;
static int logged;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hash(const char *s) {
  uint32_t h = 2166136261u; /* FNV-1a */
  while (*s)
    h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

/* lock held; the slot for `asked`, or the empty one where it would go */
static Entry *slot(const char *asked) {
  for (uint32_t i = hash(asked) & (CACHE_SLOTS - 1);; i = (i + 1) & (CACHE_SLOTS - 1))
    if (!cache[i].asked || !strcmp(cache[i].asked, asked))
      return &cache[i];
}

static int join(char *out, size_t len, const char *dir, const char *name) {
  const int n = dir[0] ? snprintf(out, len, "%s/%s", dir, name) : snprintf(out, len, "%s", name);
  return n >= 0 && (size_t)n < len ? 0 : -1;
}

enum { WALK_ERROR = -1, WALK_ABSENT = 0, WALK_FOUND = 1 };

/* Walk `path` from the current directory, taking each component as spelled
 * when it exists, otherwise the first entry of its directory equal to it
 * but for case. WALK_FOUND with the spelling in `out` when some component
 * differed; WALK_ABSENT with the directory where the search stopped in
 * `stop` (also when nothing differed); WALK_ERROR when it could not tell. */
static int walk(const char *path, char *out, size_t len, char *stop, size_t stop_len) {
  char cur[PATH_MAX] = "", next[PATH_MAX], comp[NAME_MAX + 1];
  int changed = 0;
  for (const char *p = path; *p;) {
    while (*p == '/')
      p++;
    const size_t n = strcspn(p, "/");
    if (!n)
      break;
    if (n > NAME_MAX)
      return WALK_ERROR;
    memcpy(comp, p, n);
    comp[n] = 0;
    p += n;
    struct stat st;
    if (join(next, sizeof(next), cur, comp) < 0)
      return WALK_ERROR;
    if (lstat(next, &st) != 0) {
      if (errno != ENOENT)
        return WALK_ERROR; /* ENOTDIR and the like: no spelling helps */
      DIR *d = opendir(cur[0] ? cur : ".");
      if (!d)
        return WALK_ERROR;
      const struct dirent *e;
      int found = 0;
      while (!found && (e = readdir(d)))
        if (!strcasecmp(e->d_name, comp) && join(next, sizeof(next), cur, e->d_name) == 0)
          found = 1;
      closedir(d);
      if (!found) {
        snprintf(stop, stop_len, "%s", cur[0] ? cur : ".");
        return WALK_ABSENT;
      }
      changed = 1;
    }
    memcpy(cur, next, strlen(next) + 1);
  }
  if (!changed) { /* exists as spelled (created meanwhile): not ours to answer */
    snprintf(stop, stop_len, ".");
    return WALK_ERROR;
  }
  /* "dir/" names a directory: keep the slash, so a file still fails */
  const size_t plen = strlen(path);
  if (plen && path[plen - 1] == '/' && join(next, sizeof(next), cur, "") == 0)
    memcpy(cur, next, strlen(next) + 1);
  if (strlen(cur) >= len)
    return WALK_ERROR;
  memcpy(out, cur, strlen(cur) + 1);
  return WALK_FOUND;
}

static int same_time(const struct timespec *a, const struct timespec *b) {
  return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

/* lock held */
static void forget(Entry *e) {
  free(e->found);
  free(e->miss_dir);
  e->found = e->miss_dir = NULL;
}

const char *case_resolve(const char *path, char *buf, size_t len) {
  if (!path || !path[0] || path[0] == '/' || !len)
    return NULL;
  const int saved_errno = errno; /* callers report the original failure */
  const char *r = NULL;
  pthread_mutex_lock(&lock);
  Entry *e = slot(path);
  struct stat st;
  if (e->asked && e->found) {
    if (strlen(e->found) < len)
      r = memcpy(buf, e->found, strlen(e->found) + 1);
    goto out;
  }
  if (e->asked && e->miss_dir && stat(e->miss_dir, &st) == 0 && same_time(&st.st_mtim, &e->mtime))
    goto out; /* still missing: nothing changed where it would be */

  char found[PATH_MAX], stop[PATH_MAX];
  const int w = walk(path, found, sizeof(found), stop, sizeof(stop));
  if (w == WALK_FOUND && strlen(found) < len)
    r = memcpy(buf, found, strlen(found) + 1);
  if (w != WALK_ERROR) { /* errors (no memory, no descriptors) are not remembered */
    if (!e->asked && cache_used < CACHE_SLOTS / 2) { /* sparse table; then no caching */
      e->asked = strdup(path);
      if (e->asked)
        cache_used++;
    }
    if (e->asked) {
      forget(e);
      if (w == WALK_FOUND) {
        e->found = strdup(found);
      } else if (stat(stop, &st) == 0 && (e->miss_dir = strdup(stop))) {
        e->mtime = st.st_mtim;
      }
    }
  }
  if (w == WALK_FOUND && logged < LOG_FIRST) {
    logged++;
    debugPrintf("files: %s found as %s (the case differs)%s\n", path, found,
                logged == LOG_FIRST ? "; further cases not logged" : "");
  }
out:
  pthread_mutex_unlock(&lock);
  errno = saved_errno;
  return r;
}
