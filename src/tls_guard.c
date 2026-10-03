/* tls_guard.c -- bionic's stack-guard slot on a glibc thread pointer
 *
 * Code built by the Android NDK for arm64 loads its stack canary from
 * TPIDR_EL0 + 0x28 (bionic TLS_SLOT_STACK_GUARD) in the prologue and epilogue
 * of every protected function. On glibc, TPIDR_EL0 points at a 16-byte TCB
 * that is followed by the static TLS of the executable and then of the shared
 * libraries. Left alone, +0x28 falls inside somebody's live TLS variable (libc's
 * errno block, for instance); its value changes between prologue and epilogue
 * and the game dies in __stack_chk_fail for no reason.
 *
 * So the executable's own TLS block starts with an array that stands in for
 * bionic slots 2..7, slot 5 holding a constant. glibc places the executable's
 * block right after the TCB (TLS variant I, block alignment <= 16), and every
 * thread's copy is initialised from .tdata, so the canary is the same constant
 * on every thread for its whole life, threads created by Mesa or SDL included.
 *
 * Placement: the linker puts initialised TLS (.tdata) before zeroed TLS
 * (.tbss), and this array is the executable's only initialised TLS, so it
 * lands first today whatever the link order; keeping this object first on
 * the link line keeps it first if another object ever gains .tdata.
 * tls_guard_check() verifies the layout at run time, and
 * tests/test_package.sh on the linked binary.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdint.h>

#include "tls_guard.h"

#if defined(__aarch64__)

/* bionic slots 2..7: APP, OPENGL, OPENGL_API, STACK_GUARD, SANITIZER, ART */
__thread uint64_t gtasa_bionic_tls[6] __attribute__((aligned(16), used)) = {
  0, 0, 0, BIONIC_STACK_GUARD, 0, 0,
};

static inline char *thread_pointer(void) {
  char *tp;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
  return tp;
}

int tls_guard_check(char *why, unsigned whylen) {
  char *tp = thread_pointer();
  const char *block = (const char *)gtasa_bionic_tls;
  if (block != tp + 16) {
    snprintf(why, whylen, "executable TLS block at tp%+ld, expected tp+16",
             (long)(block - tp));
    return -1;
  }
  const uint64_t slot = *(const volatile uint64_t *)(tp + BIONIC_STACK_GUARD_OFFSET);
  if (slot != BIONIC_STACK_GUARD) {
    snprintf(why, whylen, "TPIDR_EL0+0x28 holds %#llx, expected %#llx",
             (unsigned long long)slot, (unsigned long long)BIONIC_STACK_GUARD);
    return -1;
  }
  return 0;
}

void *tls_guard_block(void) {
  return gtasa_bionic_tls;
}

#else /* host builds of the platform tests: there is no bionic code to protect */

int tls_guard_check(char *why, unsigned whylen) {
  (void)why; (void)whylen;
  return 0;
}

void *tls_guard_block(void) {
  static uint64_t dummy[6];
  return dummy;
}

#endif
