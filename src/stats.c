/* stats.c -- per-interval measurements for tuning on the RF35H
 *
 * One line per interval in stats.log, for example:
 *   t=35 fps=29.8 logic=21.4/33.0ms swap=2.1/16.9ms gap=41ms draws=812 state=2210(31%skip)
 *   unif=1730 tex=96 prog=61 fbo=3 sync=2 up=0.4MB cpu[main=78 render=64 drv:gdrv0=48 ...]
 *   gpu[frag=55% vt=21% mem=212MB] ram[avail=94MB rss=402MB swap=61MB] clk[cpu=1296 gpu=600] temp=58
 * which says where the frame goes: game logic (main), GL submission
 * (render), Mesa's worker (drv:...), or the GPU itself. sync counts the GL
 * queries per frame that make the render thread wait for glthread.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gl_wrap.h"
#include "platform_config.h"
#include "platform_util.h"
#include "stats.h"
#include "util.h"

static struct {
  uint64_t logic_frames, logic_ns, logic_max;
  uint64_t presents, swap_ns, swap_max, gap_max, last_present;
} H; /* hot counters, relaxed atomics */

static pthread_mutex_t roles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int tid;
  char role[24];
} roles[32];
static int nroles;

static pthread_t thread;
static volatile int running;

static void atomic_max(uint64_t *slot, uint64_t v) {
  uint64_t cur = __atomic_load_n(slot, __ATOMIC_RELAXED);
  while (v > cur && !__atomic_compare_exchange_n(slot, &cur, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
    ;
}

void stats_logic_frame(uint64_t ns) {
  if (!running)
    return;
  __atomic_fetch_add(&H.logic_frames, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&H.logic_ns, ns, __ATOMIC_RELAXED);
  atomic_max(&H.logic_max, ns);
}

void stats_present(uint64_t swap_ns) {
  if (!running)
    return;
  const uint64_t now = now_ns();
  const uint64_t last = __atomic_exchange_n(&H.last_present, now, __ATOMIC_RELAXED);
  if (last)
    atomic_max(&H.gap_max, now - last);
  __atomic_fetch_add(&H.presents, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&H.swap_ns, swap_ns, __ATOMIC_RELAXED);
  atomic_max(&H.swap_max, swap_ns);
}

void stats_thread_role(int tid, const char *role) {
  pthread_mutex_lock(&roles_lock);
  for (int i = 0; i < nroles; i++)
    if (roles[i].tid == tid) {
      snprintf(roles[i].role, sizeof(roles[i].role), "%s", role);
      pthread_mutex_unlock(&roles_lock);
      return;
    }
  if (nroles < (int)(sizeof(roles) / sizeof(roles[0]))) {
    roles[nroles].tid = tid;
    snprintf(roles[nroles].role, sizeof(roles[nroles].role), "%s", role);
    nroles++;
  }
  pthread_mutex_unlock(&roles_lock);
}

/* ---- sampling ---------------------------------------------------------------- */

#define MAX_THREADS 128

typedef struct {
  int tid;
  uint64_t ticks;
  char name[24];
} ThreadSample;

static int sample_threads(ThreadSample *out) {
  int n = 0;
  DIR *d = opendir("/proc/self/task");
  if (!d)
    return 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < MAX_THREADS) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/self/task/%.24s/stat", e->d_name);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;
    char *open = strchr(buf, '('), *close = strrchr(buf, ')');
    if (!open || !close)
      continue;
    ThreadSample *t = &out[n];
    t->tid = atoi(e->d_name);
    snprintf(t->name, sizeof(t->name), "%.*s", (int)(close - open - 1), open + 1);
    /* after ")": state(3) ... utime(14) stime(15) */
    unsigned long utime = 0, stime = 0;
    if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) == 2) {
      t->ticks = utime + stime;
      n++;
    }
  }
  closedir(d);
  return n;
}

static void label(int tid, const char *comm, char *out, size_t len) {
  pthread_mutex_lock(&roles_lock);
  for (int i = 0; i < nroles; i++)
    if (roles[i].tid == tid) {
      snprintf(out, len, "%s", roles[i].role);
      pthread_mutex_unlock(&roles_lock);
      return;
    }
  pthread_mutex_unlock(&roles_lock);
  snprintf(out, len, "%s", comm);
}

static long meminfo_kb(const char *key) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return -1;
  char line[128];
  long v = -1;
  const size_t klen = strlen(key);
  while (fgets(line, sizeof(line), f))
    if (!strncmp(line, key, klen) && line[klen] == ':') {
      v = atol(line + klen + 1);
      break;
    }
  fclose(f);
  return v;
}

static long status_kb(const char *key) {
  FILE *f = fopen("/proc/self/status", "r");
  if (!f)
    return -1;
  char line[128];
  long v = -1;
  const size_t klen = strlen(key);
  while (fgets(line, sizeof(line), f))
    if (!strncmp(line, key, klen) && line[klen] == ':') {
      v = atol(line + klen + 1);
      break;
    }
  fclose(f);
  return v;
}

typedef struct {
  uint64_t frag_ns, vt_ns;
  uint64_t mem_bytes;
  int found;
} GpuSample;

/* panfrost publishes per-client engine time and memory in fdinfo */
static void sample_gpu(GpuSample *g) {
  memset(g, 0, sizeof(*g));
  DIR *d = opendir("/proc/self/fd");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    char link[64], target[128];
    snprintf(link, sizeof(link), "/proc/self/fd/%.24s", e->d_name);
    const ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
      continue;
    target[n] = 0;
    if (strncmp(target, "/dev/dri/", 9) != 0)
      continue;
    char info[64], line[160];
    snprintf(info, sizeof(info), "/proc/self/fdinfo/%.24s", e->d_name);
    FILE *f = fopen(info, "r");
    if (!f)
      continue;
    uint64_t frag = 0, vt = 0, mem = 0;
    int is_client = 0;
    while (fgets(line, sizeof(line), f)) {
      unsigned long long v;
      char unit[8] = "";
      if (sscanf(line, "drm-engine-fragment: %llu", &v) == 1) {
        frag = v;
        is_client = 1;
      } else if (sscanf(line, "drm-engine-vertex-tiler: %llu", &v) == 1) {
        vt = v;
        is_client = 1;
      } else if (sscanf(line, "drm-total-memory: %llu %7s", &v, unit) >= 1) {
        mem = v * (!strcmp(unit, "KiB") ? 1024ull : !strcmp(unit, "MiB") ? 1048576ull : 1ull);
      }
    }
    fclose(f);
    if (is_client && frag + vt >= g->frag_ns + g->vt_ns) {
      g->frag_ns = frag;
      g->vt_ns = vt;
      g->mem_bytes = mem;
      g->found = 1;
    }
  }
  closedir(d);
}

static long read_long(const char *path) {
  char buf[32];
  return read_line(path, buf, sizeof(buf)) == 0 ? atol(buf) : -1;
}

static void *stats_main(void *arg) {
  (void)arg;
  char name[] = "gtasa-stats";
  pthread_setname_np(pthread_self(), name);
  FILE *out = fopen("stats.log", "w");
  if (!out) {
    debugPrintf("stats: cannot write stats.log\n");
    return NULL;
  }
  setvbuf(out, NULL, _IOLBF, 0);
  fprintf(out, "# vsync=%d glthread=%d no_error=%d state_cache=%d pin=%d perf=%d scale=%d "
              "interval=%ds\n", pconfig.vsync, pconfig.glthread, pconfig.gl_no_error,
              pconfig.gl_state_cache, pconfig.pin_threads, pconfig.perf_mode, pconfig.render_scale,
              pconfig.stats_interval);

  const long hz = sysconf(_SC_CLK_TCK);
  static ThreadSample prev_t[MAX_THREADS], cur_t[MAX_THREADS];
  int nprev = sample_threads(prev_t);
  GlCounters prev_gl, cur_gl;
  glw_counters(&prev_gl);
  GpuSample prev_gpu, cur_gpu;
  sample_gpu(&prev_gpu);
  uint64_t prev_logic_frames = 0, prev_logic_ns = 0, prev_presents = 0, prev_swap_ns = 0;
  uint64_t t_prev = now_ns();
  const uint64_t t0 = t_prev;

  while (running) {
    for (int s = 0; s < pconfig.stats_interval * 10 && running; s++)
      usleep(100000);
    const uint64_t t = now_ns();
    const double dt = (double)(t - t_prev) / 1e9;
    t_prev = t;

    const uint64_t lf = __atomic_load_n(&H.logic_frames, __ATOMIC_RELAXED);
    const uint64_t ln = __atomic_load_n(&H.logic_ns, __ATOMIC_RELAXED);
    const uint64_t pr = __atomic_load_n(&H.presents, __ATOMIC_RELAXED);
    const uint64_t sn = __atomic_load_n(&H.swap_ns, __ATOMIC_RELAXED);
    const uint64_t lmax = __atomic_exchange_n(&H.logic_max, 0, __ATOMIC_RELAXED);
    const uint64_t smax = __atomic_exchange_n(&H.swap_max, 0, __ATOMIC_RELAXED);
    const uint64_t gmax = __atomic_exchange_n(&H.gap_max, 0, __ATOMIC_RELAXED);
    const uint64_t dlf = lf - prev_logic_frames, dpr = pr - prev_presents;
    const double logic_avg = dlf ? (double)(ln - prev_logic_ns) / dlf / 1e6 : 0;
    const double swap_avg = dpr ? (double)(sn - prev_swap_ns) / dpr / 1e6 : 0;
    prev_logic_frames = lf;
    prev_logic_ns = ln;
    prev_presents = pr;
    prev_swap_ns = sn;

    glw_counters(&cur_gl);
    const double per = dpr ? (double)dpr : 1.0;
#define D(f) ((double)(cur_gl.f - prev_gl.f) / per)
    const double state = D(state_calls), skipped = D(state_skipped);
    char line[1024];
    /* appends never run past the buffer, whatever the fields expand to */
#define APPEND(...) \
  do { if (n < (int)sizeof(line)) n += snprintf(line + n, sizeof(line) - (size_t)n, __VA_ARGS__); } while (0)
    int n = snprintf(line, sizeof(line),
                     "t=%.0f fps=%.1f logic=%.1f/%.1fms swap=%.1f/%.1fms gap=%.0fms draws=%.0f "
                     "verts=%.0f state=%.0f(%.0f%%skip) unif=%.0f tex=%.0f prog=%.0f fbo=%.0f "
                     "sync=%.0f up=%.2fMB",
                     (double)(t - t0) / 1e9, dpr / dt, logic_avg, lmax / 1e6, swap_avg, smax / 1e6,
                     gmax / 1e6, D(draws), D(vertices), state, state > 0 ? 100.0 * skipped / state : 0.0,
                     D(uniforms), D(tex_binds), D(prog_binds), D(fbo_binds), D(syncs),
                     (double)((cur_gl.tex_upload - prev_gl.tex_upload) + (cur_gl.buf_upload - prev_gl.buf_upload)) / 1048576.0);
#undef D
    prev_gl = cur_gl;

    const int ncur = sample_threads(cur_t);
    /* busiest threads first */
    int order[MAX_THREADS];
    double pct[MAX_THREADS];
    int nshow = 0;
    for (int i = 0; i < ncur; i++) {
      uint64_t before = cur_t[i].ticks;
      for (int j = 0; j < nprev; j++)
        if (prev_t[j].tid == cur_t[i].tid) {
          before = prev_t[j].ticks;
          break;
        }
      pct[i] = 100.0 * (double)(cur_t[i].ticks - before) / (double)hz / dt;
      order[nshow++] = i;
    }
    for (int i = 1; i < nshow; i++)
      for (int j = i; j > 0 && pct[order[j]] > pct[order[j - 1]]; j--) {
        const int tmp = order[j];
        order[j] = order[j - 1];
        order[j - 1] = tmp;
      }
    APPEND(" cpu[");
    for (int k = 0; k < nshow && k < 6 && pct[order[k]] >= 1.0; k++) {
      char lab[32];
      label(cur_t[order[k]].tid, cur_t[order[k]].name, lab, sizeof(lab));
      APPEND("%s%s=%.0f", k ? " " : "", lab, pct[order[k]]);
    }
    APPEND("]");
    memcpy(prev_t, cur_t, sizeof(ThreadSample) * (size_t)ncur);
    nprev = ncur;

    sample_gpu(&cur_gpu);
    if (cur_gpu.found && prev_gpu.found) {
      const double ns = dt * 1e9;
      APPEND(" gpu[frag=%.0f%% vt=%.0f%% mem=%.0fMB]",
                    100.0 * (double)(cur_gpu.frag_ns - prev_gpu.frag_ns) / ns,
                    100.0 * (double)(cur_gpu.vt_ns - prev_gpu.vt_ns) / ns,
                    (double)cur_gpu.mem_bytes / 1048576.0);
    }
    prev_gpu = cur_gpu;

    const long avail = meminfo_kb("MemAvailable");
    const long swap = meminfo_kb("SwapTotal") - meminfo_kb("SwapFree");
    APPEND(" ram[avail=%ldMB rss=%ldMB hwm=%ldMB swap=%ldMB]",
                  avail / 1024, status_kb("VmRSS") / 1024, status_kb("VmHWM") / 1024, swap / 1024);
    APPEND(" clk[cpu=%ld gpu=%ld] temp=%ld",
                  read_long("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq") / 1000,
                  read_long("/sys/class/devfreq/ff400000.gpu/cur_freq") / 1000000,
                  read_long("/sys/class/thermal/thermal_zone0/temp") / 1000);
    fprintf(out, "%.*s\n", (int)sizeof(line) - 1, line);
#undef APPEND
  }
  fclose(out);
  return NULL;
}

/* panfrost counts per-client GPU time in fdinfo only with profiling on
 * (sysfs switch since Linux 6.7; put back at exit like the governors) */
static void panfrost_profiling(void) {
  DIR *d = opendir("/sys/bus/platform/drivers/panfrost");
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    char path[192];
    snprintf(path, sizeof(path), "/sys/bus/platform/drivers/panfrost/%.100s/profiling", e->d_name);
    if (access(path, W_OK) == 0)
      tune_set(TUNE_STATS, path, "1");
  }
  closedir(d);
}

void stats_start(void) {
  if (!pconfig.stats || running)
    return;
  panfrost_profiling();
  running = 1;
  if (pthread_create(&thread, NULL, stats_main, NULL) != 0)
    running = 0;
}

void stats_stop(void) {
  if (!running)
    return;
  running = 0;
  pthread_join(thread, NULL);
}
