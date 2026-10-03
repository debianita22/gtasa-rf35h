/* bionic_signal.c -- bionic signal sets and sigaction on glibc
 *
 * bionic's sigset_t is a single 64-bit word and its struct sigaction puts
 * sa_flags first; glibc uses a 1024-bit set and another field order. The
 * wrappers convert both ways. Handlers for crash signals are not installed:
 * the platform layer keeps its own crash reporter on those, which is what
 * makes a remote crash diagnosable (module + offset in the log).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>

#include "bionic.h"
#include "util.h"

static int crash_signal(int sig) {
  return sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE ||
         sig == SIGABRT || sig == SIGTRAP || sig == SIGSYS;
}

static void to_glibc(const bionic_sigset_t *in, sigset_t *out) {
  sigemptyset(out);
  for (int sig = 1; sig <= 64; sig++)
    if (*in & (1ul << (sig - 1)))
      sigaddset(out, sig);
}

static void to_bionic(const sigset_t *in, bionic_sigset_t *out) {
  *out = 0;
  for (int sig = 1; sig <= 64; sig++)
    if (sigismember(in, sig) == 1)
      *out |= 1ul << (sig - 1);
}

int bionic_sigemptyset(bionic_sigset_t *set) { *set = 0; return 0; }
int bionic_sigfillset(bionic_sigset_t *set) { *set = ~0ul; return 0; }

int bionic_sigaddset(bionic_sigset_t *set, int sig) {
  if (sig < 1 || sig > 64) {
    errno = EINVAL;
    return -1;
  }
  *set |= 1ul << (sig - 1);
  return 0;
}

int bionic_sigdelset(bionic_sigset_t *set, int sig) {
  if (sig < 1 || sig > 64) {
    errno = EINVAL;
    return -1;
  }
  *set &= ~(1ul << (sig - 1));
  return 0;
}

int bionic_sigismember(const bionic_sigset_t *set, int sig) {
  if (sig < 1 || sig > 64) {
    errno = EINVAL;
    return -1;
  }
  return (*set >> (sig - 1)) & 1;
}

int bionic_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old) {
  struct sigaction gold;
  memset(&gold, 0, sizeof(gold));
  if (act && crash_signal(sig)) {
    debugPrintf("sigaction(%d) from the game ignored: crash reporting stays ours\n", sig);
    act = NULL;
  }
  int rc;
  if (act) {
    struct sigaction gact;
    memset(&gact, 0, sizeof(gact));
    gact.sa_handler = (void (*)(int))act->handler;
    gact.sa_flags = act->flags & ~BIONIC_SA_RESTORER;
    to_glibc(&act->mask, &gact.sa_mask);
    rc = sigaction(sig, &gact, old ? &gold : NULL);
  } else {
    rc = sigaction(sig, NULL, old ? &gold : NULL);
  }
  if (rc == 0 && old) {
    memset(old, 0, sizeof(*old));
    old->handler = (void *)gold.sa_handler;
    old->flags = gold.sa_flags;
    to_bionic(&gold.sa_mask, &old->mask);
  }
  return rc;
}

void *bionic_signal(int sig, void *handler) {
  if (crash_signal(sig)) {
    debugPrintf("signal(%d) from the game ignored: crash reporting stays ours\n", sig);
    return SIG_DFL;
  }
  return (void *)signal(sig, (void (*)(int))handler);
}

static int mask_common(int how, const bionic_sigset_t *set, bionic_sigset_t *old, int threaded) {
  sigset_t gset, gold;
  if (set)
    to_glibc(set, &gset);
  const int rc = threaded ? pthread_sigmask(how, set ? &gset : NULL, old ? &gold : NULL)
                          : sigprocmask(how, set ? &gset : NULL, old ? &gold : NULL);
  if ((threaded ? rc == 0 : rc >= 0) && old)
    to_bionic(&gold, old);
  return rc;
}

int bionic_sigprocmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old) {
  return mask_common(how, set, old, 0);
}

int bionic_pthread_sigmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old) {
  return mask_common(how, set, old, 1);
}
