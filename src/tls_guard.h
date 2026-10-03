/* tls_guard.h -- bionic AArch64 TLS slots on top of a glibc thread pointer
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_TLS_GUARD_H
#define GTASA_TLS_GUARD_H

#include <stdint.h>

/* The constant every NDK-built arm64 function compares its stack canary with.
 * Same value gtasa_nx uses, so the global __stack_chk_guard import and the
 * TPIDR_EL0+0x28 slot agree. */
#define BIONIC_STACK_GUARD UINT64_C(0x4242424242424242)

/* Offset of bionic's TLS_SLOT_STACK_GUARD (slot 5) from TPIDR_EL0. */
#define BIONIC_STACK_GUARD_OFFSET 0x28

/* Returns 0 when the calling thread's TPIDR_EL0+0x28 holds the guard and the
 * executable's TLS block sits where the layout expects it; otherwise writes a
 * one-line reason into `why` and returns -1. Cheap enough to run per thread. */
int tls_guard_check(char *why, unsigned whylen);

/* Address of the calling thread's bionic-slot block (for game_tls_install). */
void *tls_guard_block(void);

#endif
