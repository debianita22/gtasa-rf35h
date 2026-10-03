/* crash.c -- crash reports that name the module, offset and symbol
 *
 * On a fault the log gets the signal, the faulting address, pc/lr/sp and a
 * frame-pointer backtrace, each address as module+offset (symbol+offset when
 * the game exports one). libGame.so keeps frame records on arm64, so the
 * chain is usually complete. Only async-signal-safe calls in here: no stdio,
 * no malloc; text is formatted by hand and written with write().
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include "crash.h"
#include "loader.h"
#include "platform_util.h"

typedef struct {
  char buf[2048];
  size_t len;
} Out;

static void put(Out *o, const char *s) {
  while (*s && o->len < sizeof(o->buf) - 1)
    o->buf[o->len++] = *s++;
}

static void put_hex(Out *o, uint64_t v) {
  char tmp[19] = "0x";
  int n = 2;
  int started = 0;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const int d = (int)((v >> shift) & 0xf);
    if (d || started || shift == 0) {
      tmp[n++] = "0123456789abcdef"[d];
      started = 1;
    }
  }
  tmp[n] = 0;
  put(o, tmp);
}

static void put_dec(Out *o, long v) {
  char tmp[24];
  int n = 0;
  if (v < 0) {
    put(o, "-");
    v = -v;
  }
  do {
    tmp[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && n < (int)sizeof(tmp));
  while (n) {
    n--;
    if (o->len < sizeof(o->buf) - 1)
      o->buf[o->len++] = tmp[n];
  }
}

static void put_addr(Out *o, uintptr_t a) {
  put_hex(o, a);
  const so_module *m = so_module_at(a);
  if (!m)
    return;
  put(o, " (");
  put(o, m->name);
  put(o, "+");
  put_hex(o, a - (uintptr_t)m->load_base);
  uintptr_t off = 0;
  const char *sym = so_symbol_at(m, a, &off);
  if (sym) {
    put(o, " ");
    put(o, sym);
    put(o, "+");
    put_hex(o, off);
  }
  put(o, ")");
}

static const char *signame(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    default:      return "signal";
  }
}

/* A probe that cannot fault: the kernel copies what is written to a pipe
 * and answers EFAULT for unreadable memory (writing to /dev/null would not
 * look at the bytes at all). Drained after every probe. */
static int probe_pipe[2] = { -1, -1 };

static int readable(uintptr_t p) {
  char tmp[16];
  if (!p || (p & 7) || probe_pipe[1] < 0)
    return 0;
  if (write(probe_pipe[1], (const void *)p, sizeof(tmp)) != (ssize_t)sizeof(tmp))
    return 0;
  while (read(probe_pipe[0], tmp, sizeof(tmp)) > 0)
    ;
  return 1;
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
  /* One report per process: a second thread faulting meanwhile waits for
   * the first report to end the process; a fault inside the handler ends
   * it at once. */
  static int reporter;
  const int tid = (int)syscall(SYS_gettid);
  int expected = 0;
  if (!__atomic_compare_exchange_n(&reporter, &expected, tid, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    if (expected == tid)
      _exit(128 + sig);
    for (;;)
      pause();
  }

  Out o = { .len = 0 };
  put(&o, "\n*** CRASH: ");
  put(&o, signame(sig));
  put(&o, " on thread ");
  put_dec(&o, (long)tid);
  char comm[20] = "";
  const int cfd = open("/proc/thread-self/comm", O_RDONLY | O_CLOEXEC);
  if (cfd >= 0) {
    const ssize_t n = read(cfd, comm, sizeof(comm) - 1);
    close(cfd);
    if (n > 0)
      comm[n - 1] = 0;
  }
  put(&o, " (");
  put(&o, comm);
  put(&o, ") fault address ");
  put_hex(&o, (uintptr_t)si->si_addr);
  put(&o, "\n");

#if defined(__aarch64__)
  const ucontext_t *uc = ctx;
  const uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
  const uintptr_t lr = (uintptr_t)uc->uc_mcontext.regs[30];
  uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
  put(&o, "  pc ");
  put_addr(&o, pc);
  put(&o, "\n  lr ");
  put_addr(&o, lr);
  put(&o, "\n  sp ");
  put_hex(&o, (uintptr_t)uc->uc_mcontext.sp);
  put(&o, "\n");
  /* what is known so far reaches the log before the walk, which reads
   * memory the crash may have damaged */
  log_raw(o.buf, o.len);
  o.len = 0;
  for (int depth = 0; depth < 24 && readable(fp); depth++) {
    const uintptr_t next = ((const uintptr_t *)fp)[0];
    const uintptr_t ret = ((const uintptr_t *)fp)[1];
    if (!ret)
      break;
    put(&o, "  #");
    put_dec(&o, depth);
    put(&o, " ");
    put_addr(&o, ret);
    put(&o, "\n");
    if (next <= fp)
      break;
    fp = next;
  }
#else
  (void)ctx;
#endif
  log_raw(o.buf, o.len);

  const int efd = open("last-error.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (efd >= 0) {
    static const char msg[] = "Il gioco si e' chiuso per un errore: dettagli in gtasa.log\n";
    if (write(efd, msg, sizeof(msg) - 1) < 0) {
      /* nothing else to try from a signal handler */
    }
    close(efd);
  }
  tune_restore_all();
  _exit(128 + sig);
}

#define ALTSTACK_SIZE (64 * 1024)

static pthread_key_t altstack_key;
static pthread_once_t altstack_once = PTHREAD_ONCE_INIT;

static void altstack_free(void *mem) {
  const stack_t off = { .ss_sp = NULL, .ss_size = 0, .ss_flags = SS_DISABLE };
  sigaltstack(&off, NULL);
  munmap(mem, ALTSTACK_SIZE);
}

static void altstack_key_create(void) { pthread_key_create(&altstack_key, altstack_free); }

/* sigaltstack is per thread: without one, a stack overflow on a game
 * thread kills the process before the handler can run */
void crash_thread_init(void) {
  pthread_once(&altstack_once, altstack_key_create);
  if (pthread_getspecific(altstack_key))
    return;
  void *mem = mmap(NULL, ALTSTACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
    return;
  const stack_t ss = { .ss_sp = mem, .ss_size = ALTSTACK_SIZE, .ss_flags = 0 };
  if (sigaltstack(&ss, NULL) == 0)
    pthread_setspecific(altstack_key, mem);
  else
    munmap(mem, ALTSTACK_SIZE);
}

void crash_install(void) {
  if (pipe2(probe_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
    probe_pipe[0] = probe_pipe[1] = -1;
  crash_thread_init();
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
  for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
    sigaction(sigs[i], &sa, NULL);
}
