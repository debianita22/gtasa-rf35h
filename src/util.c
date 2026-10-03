/* util.c -- upstream util.h on Linux, plus logging and CPU placement
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "util.h"
#include "platform_util.h"
#include "platform_config.h"
#include "tls_guard.h"

static int log_descriptor = 2;
static uint64_t log_epoch;

uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void log_open(const char *path) {
  char old[512];
  snprintf(old, sizeof(old), "%s.1", path);
  rename(path, old);
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0)
    log_descriptor = fd;
  if (!log_epoch)
    log_epoch = now_ns();
}

int log_fd(void) { return log_descriptor; }

void log_raw(const char *msg, size_t len) {
  while (len) {
    const ssize_t n = write(log_descriptor, msg, len);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return;
    msg += n;
    len -= (size_t)n;
  }
}

static void vlog(const char *fmt, va_list va) {
  char line[1024];
  if (!log_epoch)
    log_epoch = now_ns();
  const uint64_t t = now_ns() - log_epoch;
  int n = snprintf(line, sizeof(line), "[%5u.%03u %5d] ", (unsigned)(t / 1000000000ull),
                   (unsigned)(t / 1000000ull % 1000), (int)syscall(SYS_gettid));
  int m = vsnprintf(line + n, sizeof(line) - n, fmt, va);
  if (m < 0)
    return;
  n += m;
  if (n > (int)sizeof(line) - 2)
    n = (int)sizeof(line) - 2;
  if (line[n - 1] != '\n')
    line[n++] = '\n';
  log_raw(line, (size_t)n);
}

int debugPrintf(char *text, ...) {
  va_list va;
  va_start(va, text);
  vlog(text, va);
  va_end(va);
  return 0;
}

int read_line(const char *path, char *out, size_t len) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  const char *r = fgets(out, (int)len, f);
  fclose(f);
  if (!r)
    return -1;
  out[strcspn(out, "\r\n")] = 0;
  return 0;
}

int write_line(const char *path, const char *value) {
  const int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC); /* sysfs ignores O_TRUNC */
  if (fd < 0)
    return -1;
  const ssize_t n = write(fd, value, strlen(value));
  close(fd);
  return n == (ssize_t)strlen(value) ? 0 : -1;
}

void note_data_open(const char *path) {
  const char *t = path ? strstr(path, "texdb/") : NULL;
  const size_t n = path ? strlen(path) : 0;
  if (!t || n < 4 || strcasecmp(path + n - 4, ".dat") != 0)
    return;
  static char seen[24][96];
  static int nseen;
  for (int i = 0; i < nseen; i++)
    if (!strcmp(seen[i], t))
      return;
  if (nseen < 24)
    snprintf(seen[nseen++], sizeof(seen[0]), "%s", t);
  debugPrintf("data: texture database %s\n", t);
}

/* ---- temporary sysfs settings ----------------------------------------------- */

#define MAX_TUNES 16
static struct {
  char path[128];
  char old[48];
  int group;
} tunes[MAX_TUNES];
static volatile int ntunes;

int tune_set(int group, const char *path, const char *value) {
  char cur[48];
  if (read_line(path, cur, sizeof(cur)) < 0)
    return -1;
  if (!strcmp(cur, value))
    return 0; /* already so: nothing to put back */
  for (int i = 0; i < ntunes; i++)
    if (!strcmp(tunes[i].path, path))
      return write_line(path, value); /* the first old value is the one to restore */
  if (ntunes == MAX_TUNES || strlen(path) >= sizeof(tunes[0].path))
    return -1;
  if (write_line(path, value) < 0) {
    debugPrintf("sysfs: cannot set %s to %s\n", path, value);
    return -1;
  }
  const int i = ntunes;
  snprintf(tunes[i].path, sizeof(tunes[i].path), "%s", path);
  snprintf(tunes[i].old, sizeof(tunes[i].old), "%s", cur);
  tunes[i].group = group;
  ntunes = i + 1;
  debugPrintf("sysfs: %s %s -> %s\n", path, cur, value);
  return 0;
}

/* async-signal-safe (the crash handler calls it): open/write/close only */
static void restore_where(int group) {
  for (int i = ntunes - 1; i >= 0; i--) {
    if (group >= 0 && tunes[i].group != group)
      continue;
    const int fd = open(tunes[i].path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd >= 0) {
      if (write(fd, tunes[i].old, strlen(tunes[i].old)) < 0) {
        /* nothing more to try */
      }
      close(fd);
    }
    tunes[i].path[0] = 0;
    tunes[i].group = -2; /* done */
  }
  if (group < 0)
    ntunes = 0;
}

void tune_restore_all(void) { restore_where(-1); }

/* ---- performance mode: CPU, GPU and memory-controller governors ------------ */

static int has_governor(const char *dir, const char *gov) {
  char path[192], list[256];
  snprintf(path, sizeof(path), "%s/available_governors", dir);
  return read_line(path, list, sizeof(list)) == 0 && strstr(list, gov) != NULL;
}

static void performance_governors(void) {
  char path[192];
  if (pconfig.cpu_turbo && access("/sys/devices/system/cpu/cpufreq/boost", W_OK) == 0)
    tune_set(TUNE_PERF, "/sys/devices/system/cpu/cpufreq/boost", "1");
  for (int p = 0; p < 8; p++) {
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor", p);
    if (access(path, W_OK) == 0)
      tune_set(TUNE_PERF, path, "performance");
  }
  /* devfreq: the Mali (ff400000.gpu) and, where the kernel drives it, DDR (dmc) */
  DIR *d = opendir("/sys/class/devfreq");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strstr(e->d_name, "gpu") && !strstr(e->d_name, "dmc"))
      continue;
    char dir[160];
    snprintf(dir, sizeof(dir), "/sys/class/devfreq/%.100s", e->d_name);
    snprintf(path, sizeof(path), "%s/governor", dir);
    if (has_governor(dir, "performance"))
      tune_set(TUNE_PERF, path, "performance");
  }
  closedir(d);
}

/* upstream calls cpu_boost(0) when the loading splash goes away; the main
 * loop also does once the menu has been up for a while */
void cpu_boost(int on) {
  if (pconfig.perf_mode <= 0)
    return;
  if (on) {
    performance_governors();
  } else if (pconfig.perf_mode == 1) {
    static int logged;
    if (!logged++)
      debugPrintf("perf: boot over, governors restored\n");
    restore_where(TUNE_PERF);
  }
}

/* ---- frame pacing --------------------------------------------------------- */

void pacer_init(Pacer *p, uint64_t frame_ns) {
  p->frame_ns = frame_ns;
  p->next = now_ns() + frame_ns;
}

void pacer_wait(Pacer *p, const volatile sig_atomic_t *stop) {
  const uint64_t end = now_ns();
  if (end < p->next) {
    const struct timespec ts = { (time_t)(p->next / 1000000000ull),
                                 (long)(p->next % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !*stop)
      ;
  } else if (end - p->next >= p->frame_ns) {
    p->next = end; /* a frame or more behind: do not try to catch up */
  }
  p->next += p->frame_ns;
}

/* ---- thread placement ----------------------------------------------------- */

void pin_thread(int tid, int cls) {
  if (!pconfig.pin_threads)
    return;
  int mask;
  switch (cls) {
    case CORE_LOGIC:  mask = pconfig.cpu_logic; break;
    case CORE_RENDER: mask = pconfig.cpu_render; break;
    case CORE_DRIVER: mask = pconfig.cpu_driver; break;
    default:          mask = pconfig.cpu_other; break;
  }
  if (mask <= 0)
    return;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu = 0; cpu < 16; cpu++)
    if (mask & (1 << cpu))
      CPU_SET(cpu, &set);
  if (sched_setaffinity(tid, sizeof(set), &set) < 0)
    debugPrintf("affinity: thread %d class %d mask %#x failed: %s\n", tid, cls, mask, strerror(errno));
}

/* gtasa_nx: 0 = game logic, 1 = the "RenderQueue" GL thread, 2 = the rest */
void set_thread_core(int core) {
  pin_thread(0, core);
}

/* The process leaves with _exit(), which stops every thread at once; nothing
 * to freeze first, unlike the Switch. */
void thread_registry_add(void) {}
void thread_registry_pause_others(void) {}

void *game_tls_install(void) {
  char why[128];
  if (tls_guard_check(why, sizeof(why)) < 0) {
    debugPrintf("FATAL: stack-guard slot not usable on thread %d: %s\n",
                (int)syscall(SYS_gettid), why);
    return NULL;
  }
  return tls_guard_block();
}

int ret0(void) { return 0; }
int retm1(void) { return -1; }
