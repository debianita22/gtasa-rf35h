/* stats.h -- measurements for tuning on the device
 *
 * With `stats 1` a background thread writes one line per interval to
 * stats.log: frame rates and times, GL calls per frame, CPU per thread,
 * GPU engine time and memory, RAM, clocks and temperature. The hooks below
 * are cheap enough to stay in place when stats are off.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_STATS_H
#define GTASA_STATS_H

#include <stdint.h>

void stats_start(void);
void stats_stop(void);

/* main thread: one call per implOnDrawFrame, with its duration */
void stats_logic_frame(uint64_t ns);

/* render thread: one call per eglSwapBuffers, with the time spent inside */
void stats_present(uint64_t swap_ns);

/* label a thread for the per-thread CPU columns ("main", "render", ...) */
void stats_thread_role(int tid, const char *role);

#endif
