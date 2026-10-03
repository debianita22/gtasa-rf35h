/* switch.h -- the handful of libnx names the vendored gtasa_nx sources use.
 *
 * upstream/source/{overlay.c,jni_fake.c,hooks/game.c} include <switch.h> for
 * the fixed-width typedefs and the system tick; nothing else from libnx is
 * referenced by the files we build. Keeping this shim lets those files compile
 * unmodified on Linux.
 */

#ifndef GTASA_COMPAT_SWITCH_H
#define GTASA_COMPAT_SWITCH_H

#include <stdint.h>
#include <time.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

/* armGetSystemTick() counts in nanoseconds here (monotonic clock). */
static inline u64 armGetSystemTickFreq(void) { return 1000000000ull; }

static inline u64 armGetSystemTick(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

#endif
