/* platform_util.h -- logging and small helpers of the Linux platform layer
 *
 * The functions declared by upstream/source/util.h (debugPrintf, cpu_boost,
 * set_thread_core, ...) are implemented in util.c too; this header carries
 * the extras the rest of src/ uses.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_PLATFORM_UTIL_H
#define GTASA_PLATFORM_UTIL_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

/* Open the log, keeping the previous run's as <path>.1. Before this call,
 * messages go to stderr. */
void log_open(const char *path);
int log_fd(void);

/* Async-signal-safe: one write() of an already formatted message. */
void log_raw(const char *msg, size_t len);

/* Monotonic clock in nanoseconds. */
uint64_t now_ns(void);

/* First line of a sysfs/procfs file, trimmed; returns 0 on success. */
int read_line(const char *path, char *out, size_t len);
int write_line(const char *path, const char *value);

/* Log which texture database format the game loads (texdb/<name>.<fmt>.dat:
 * dxt, etc, pvr or unc), once per file: it tells which variants of the
 * game data are needed on the card. */
void note_data_open(const char *path);

/* Thread classes, matching gtasa_nx's set_thread_core() numbering plus the
 * Mesa driver thread, which gtasa_nx never had to place. */
enum { CORE_LOGIC = 0, CORE_RENDER = 1, CORE_OTHER = 2, CORE_DRIVER = 3 };

/* Apply the configured CPU set of a class to a thread (0 = calling thread). */
void pin_thread(int tid, int cls);

/* Temporary sysfs settings (governors, the panfrost profiling switch): the
 * value found is saved the first time a path is set and written back by
 * tune_restore_all() on every way out (exit, fatal error, crash, SIGTERM).
 * cpu_boost(1) from upstream's util.h sets the performance governors of the
 * CPU, the GPU and the memory controller, as pconfig.perf_mode asks. */
enum { TUNE_PERF = 0, TUNE_STATS = 1 };
int tune_set(int group, const char *path, const char *value);
void tune_restore_all(void); /* async-signal-safe */

/* Frame pacing on absolute deadlines (gtasa_nx's 30 fps cap): a frame that
 * ends early sleeps until its deadline; one that overruns by less than a
 * frame shortens the next wait so the cadence holds; one that overruns by a
 * frame or more restarts the cadence instead of rushing to catch up. */
typedef struct {
  uint64_t frame_ns;
  uint64_t next; /* deadline of the frame in progress */
} Pacer;

void pacer_init(Pacer *p, uint64_t frame_ns);
/* Call at the end of each frame; returns early if *stop becomes nonzero. */
void pacer_wait(Pacer *p, const volatile sig_atomic_t *stop);

#endif
