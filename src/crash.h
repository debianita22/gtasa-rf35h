/* crash.h -- crash reporter for remote diagnosis
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_CRASH_H
#define GTASA_CRASH_H

/* Install the handler for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP
 * (main thread alternate stack included). */
void crash_install(void);

/* Give the calling thread its own alternate signal stack (game threads get
 * one from the pthread_create wrapper). */
void crash_thread_init(void);

#endif
