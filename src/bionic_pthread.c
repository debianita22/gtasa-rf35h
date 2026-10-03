/* bionic_pthread.c -- bionic pthread objects backed by glibc ones
 *
 * bionic's mutex, condition, rwlock and semaphore objects have different
 * sizes from glibc's (see bionic_types.h) and are int32 arrays, so only
 * 4-byte aligned: an 8-byte atomic on one is an alignment fault on ARMv8
 * (a libc++ std::mutex after an int is enough). The game's object therefore
 * holds, in its first 32-bit word, a handle: HANDLE_TAG | index into a table
 * of heap-allocated glibc objects. Objects set up with a static initialiser
 * still hold the initialiser value there (always below HANDLE_TAG) and are
 * created on first use, with a 32-bit compare-and-swap so two threads racing
 * on the same static mutex end up sharing one object (gtasa_nx did a plain
 * store of a pointer).
 *
 * Every engine mutex is recursive, as in gtasa_nx: the engine re-locks some
 * mutexes from the same thread during the world load and deadlocks otherwise.
 *
 * Thread attributes are interpreted with bionic's layout, so the stack sizes
 * and detach states the engine asks for are honoured.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "bionic.h"
#include "crash.h"
#include "util.h"

/* ==========================================================================
 * handles: game object word -> glibc object
 * ======================================================================== */

#define HANDLE_TAG 0x80000000u
#define CHUNK_SHIFT 12
#define CHUNK_SIZE (1u << CHUNK_SHIFT)
#define MAX_CHUNKS 512 /* 2M live objects */

static void **chunks[MAX_CHUNKS]; /* each CHUNK_SIZE pointers, published with a CAS */
static pthread_mutex_t free_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_index;
static uint32_t *free_list;
static size_t free_count, free_cap;

/* the table slot of an index, its chunk allocated on demand */
static void **slot_of(uint32_t idx) {
  const uint32_t c = idx >> CHUNK_SHIFT;
  if (c >= MAX_CHUNKS)
    return NULL;
  void **chunk = __atomic_load_n(&chunks[c], __ATOMIC_ACQUIRE);
  if (!chunk) {
    void **fresh = calloc(CHUNK_SIZE, sizeof(void *));
    if (!fresh)
      return NULL;
    void **expected = NULL;
    if (__atomic_compare_exchange_n(&chunks[c], &expected, fresh, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
      chunk = fresh;
    } else {
      free(fresh);
      chunk = expected;
    }
  }
  return &chunk[idx & (CHUNK_SIZE - 1)];
}

/* register a glibc object; 0 when the table is full */
static uint32_t handle_new(void *obj) {
  uint32_t idx;
  pthread_mutex_lock(&free_lock);
  if (free_count)
    idx = free_list[--free_count];
  else
    idx = next_index < MAX_CHUNKS * CHUNK_SIZE ? next_index++ : UINT32_MAX;
  pthread_mutex_unlock(&free_lock);
  void **slot = idx != UINT32_MAX ? slot_of(idx) : NULL;
  if (!slot)
    return 0;
  __atomic_store_n(slot, obj, __ATOMIC_RELEASE);
  return HANDLE_TAG | idx;
}

static void *handle_get(uint32_t h) {
  void **slot = slot_of(h & ~HANDLE_TAG);
  return slot ? __atomic_load_n(slot, __ATOMIC_ACQUIRE) : NULL;
}

static void handle_free(uint32_t h) {
  const uint32_t idx = h & ~HANDLE_TAG;
  void **slot = slot_of(idx);
  if (slot)
    __atomic_store_n(slot, NULL, __ATOMIC_RELEASE);
  pthread_mutex_lock(&free_lock);
  if (free_count == free_cap) {
    const size_t cap = free_cap ? free_cap * 2 : 256;
    uint32_t *grown = realloc(free_list, cap * sizeof(*grown));
    if (grown) {
      free_list = grown;
      free_cap = cap;
    }
  }
  if (free_count < free_cap)
    free_list[free_count++] = idx; /* else the index is simply not reused */
  pthread_mutex_unlock(&free_lock);
}

static inline uint32_t word_load(void *obj) {
  return __atomic_load_n((uint32_t *)obj, __ATOMIC_ACQUIRE);
}

/* The glibc object behind a game object, created on first use from the
 * static initialiser in its first word (passed to `make`). */
static void *object_get(void *obj, void *(*make)(uint32_t init), void (*kill)(void *)) {
  for (;;) {
    uint32_t w = word_load(obj);
    if (w & HANDLE_TAG)
      return handle_get(w);
    void *o = make(w);
    if (!o)
      return NULL;
    const uint32_t h = handle_new(o);
    if (!h) {
      kill(o);
      return NULL;
    }
    if (__atomic_compare_exchange_n((uint32_t *)obj, &w, h, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return o;
    handle_free(h); /* another thread published first: use its object */
    kill(o);
  }
}

/* explicit *_init: a fresh object, whatever the word held before */
static int object_init(void *obj, size_t size, void *o, void (*kill)(void *)) {
  if (!o)
    return ENOMEM;
  const uint32_t h = handle_new(o);
  if (!h) {
    kill(o);
    return ENOMEM;
  }
  memset((uint8_t *)obj + 4, 0, size - 4);
  __atomic_store_n((uint32_t *)obj, h, __ATOMIC_RELEASE);
  return 0;
}

static void object_destroy(void *obj, void (*kill)(void *)) {
  const uint32_t w = __atomic_exchange_n((uint32_t *)obj, 0, __ATOMIC_ACQ_REL);
  if (!(w & HANDLE_TAG))
    return;
  void *o = handle_get(w);
  handle_free(w);
  if (o)
    kill(o);
}

/* ==========================================================================
 * threads
 * ======================================================================== */

typedef struct {
  void *(*fn)(void *);
  void *arg;
} ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart s = *(ThreadStart *)p;
  free(p);
  crash_thread_init(); /* an alternate stack, so a stack overflow is reported */
  /* OS_ThreadLaunch threads (streaming, audio) share the "other" core class,
   * as in gtasa_nx; set_thread_core maps it to the configured CPU set */
  set_thread_core(2);
  thread_registry_add();
  if (!game_tls_install())
    return (void *)-1;
  return s.fn(s.arg);
}

int bionic_pthread_create(pthread_t *t, const bionic_pthread_attr_t *battr,
                          void *(*fn)(void *), void *arg) {
  ThreadStart *s = malloc(sizeof(*s));
  if (!s)
    return EAGAIN;
  s->fn = fn;
  s->arg = arg;

  pthread_attr_t a;
  pthread_attr_init(&a);
  /* bionic's default is 1 MiB; glibc's would be RLIMIT_STACK (8 MiB) */
  size_t stack = 1024 * 1024;
  if (battr) {
    if (battr->stack_size)
      stack = battr->stack_size;
    if (battr->flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED)
      pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  }
  if (stack < PTHREAD_STACK_MIN) /* glibc's (128 KiB here): it keeps TLS there too */
    stack = PTHREAD_STACK_MIN;
  pthread_attr_setstacksize(&a, stack);

  const int rc = pthread_create(t, &a, thread_trampoline, s);
  pthread_attr_destroy(&a);
  if (rc)
    free(s);
  return rc;
}

int bionic_pthread_attr_init(bionic_pthread_attr_t *a) {
  memset(a, 0, sizeof(*a));
  a->stack_size = 1024 * 1024;
  a->guard_size = 4096;
  return 0;
}

int bionic_pthread_attr_destroy(bionic_pthread_attr_t *a) {
  memset(a, 0x42, sizeof(*a));
  return 0;
}

int bionic_pthread_attr_setdetachstate(bionic_pthread_attr_t *a, int state) {
  if (state == BIONIC_PTHREAD_CREATE_DETACHED)
    a->flags |= BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
  else if (state == BIONIC_PTHREAD_CREATE_JOINABLE)
    a->flags &= ~BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
  else
    return EINVAL;
  return 0;
}

int bionic_pthread_attr_getdetachstate(const bionic_pthread_attr_t *a, int *state) {
  *state = (a->flags & BIONIC_PTHREAD_ATTR_FLAG_DETACHED) ? BIONIC_PTHREAD_CREATE_DETACHED
                                                          : BIONIC_PTHREAD_CREATE_JOINABLE;
  return 0;
}

/* bionic's minimum (16 KiB); pthread_create rounds up to glibc's */
#define BIONIC_STACK_MIN 16384

int bionic_pthread_attr_setstacksize(bionic_pthread_attr_t *a, size_t size) {
  if (size < BIONIC_STACK_MIN)
    return EINVAL;
  a->stack_size = size;
  return 0;
}

int bionic_pthread_attr_getstacksize(const bionic_pthread_attr_t *a, size_t *size) {
  *size = a->stack_size;
  return 0;
}

int bionic_pthread_attr_setstack(bionic_pthread_attr_t *a, void *base, size_t size) {
  /* caller-provided stacks are not used: the size is kept, the memory is not */
  a->stack_base = base;
  a->stack_size = size;
  return 0;
}

int bionic_pthread_attr_getstack(const bionic_pthread_attr_t *a, void **base, size_t *size) {
  *base = a->stack_base;
  *size = a->stack_size;
  return 0;
}

int bionic_pthread_attr_setguardsize(bionic_pthread_attr_t *a, size_t size) { a->guard_size = size; return 0; }
int bionic_pthread_attr_getguardsize(const bionic_pthread_attr_t *a, size_t *size) { *size = a->guard_size; return 0; }
int bionic_pthread_attr_setschedpolicy(bionic_pthread_attr_t *a, int p) { a->sched_policy = p; return 0; }
int bionic_pthread_attr_getschedpolicy(const bionic_pthread_attr_t *a, int *p) { *p = a->sched_policy; return 0; }
int bionic_pthread_attr_setschedparam(bionic_pthread_attr_t *a, const int *param) { a->sched_priority = *param; return 0; }
int bionic_pthread_attr_getschedparam(const bionic_pthread_attr_t *a, int *param) { *param = a->sched_priority; return 0; }
int bionic_pthread_attr_setscope(bionic_pthread_attr_t *a, int scope) { (void)a; return scope == PTHREAD_SCOPE_SYSTEM ? 0 : ENOTSUP; }
int bionic_pthread_attr_getscope(const bionic_pthread_attr_t *a, int *scope) { (void)a; *scope = PTHREAD_SCOPE_SYSTEM; return 0; }
int bionic_pthread_attr_setinheritsched(bionic_pthread_attr_t *a, int inherit) { (void)a; (void)inherit; return 0; }

int bionic_pthread_getattr_np(pthread_t t, bionic_pthread_attr_t *a) {
  pthread_attr_t ga;
  const int rc = pthread_getattr_np(t, &ga);
  if (rc)
    return rc;
  bionic_pthread_attr_init(a);
  void *base = NULL;
  size_t size = 0;
  pthread_attr_getstack(&ga, &base, &size);
  a->stack_base = base;
  a->stack_size = size;
  int detach = 0;
  pthread_attr_getdetachstate(&ga, &detach);
  if (detach == PTHREAD_CREATE_DETACHED)
    a->flags |= BIONIC_PTHREAD_ATTR_FLAG_DETACHED;
  pthread_attr_destroy(&ga);
  return 0;
}

/* Scheduling changes need privileges the game never had on Android either;
 * report success so it does not take a fallback path. */
int bionic_pthread_setschedparam(pthread_t t, int policy, const void *param) {
  (void)t; (void)policy; (void)param;
  return 0;
}

int bionic_pthread_getschedparam(pthread_t t, int *policy, void *param) {
  (void)t;
  *policy = SCHED_OTHER;
  memset(param, 0, sizeof(int));
  return 0;
}

int bionic_pthread_setname_np(pthread_t t, const char *name) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%s", name ? name : "");
  return pthread_setname_np(t, buf);
}

pid_t bionic_pthread_gettid_np(pthread_t t) {
  if (pthread_equal(t, pthread_self()))
    return (pid_t)syscall(SYS_gettid);
  return -1;
}

/* ==========================================================================
 * mutexes
 * ======================================================================== */

int bionic_pthread_mutexattr_init(long *a) { *a = 0; return 0; }
int bionic_pthread_mutexattr_destroy(long *a) { *a = -1; return 0; }
int bionic_pthread_mutexattr_settype(long *a, int type) { *a = (*a & ~0xfL) | (type & 0xf); return 0; }
int bionic_pthread_mutexattr_gettype(const long *a, int *type) { *type = (int)(*a & 0xf); return 0; }
int bionic_pthread_mutexattr_setpshared(long *a, int pshared) { (void)a; return pshared ? ENOTSUP : 0; }

static void *mutex_make(uint32_t init) {
  (void)init; /* normal, recursive or errorcheck: all recursive, see above */
  pthread_mutex_t *m = malloc(sizeof(*m));
  if (!m)
    return NULL;
  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &ma);
  pthread_mutexattr_destroy(&ma);
  return m;
}

static void mutex_kill(void *m) {
  pthread_mutex_destroy(m);
  free(m);
}

static pthread_mutex_t *mutex_get(void *obj) { return object_get(obj, mutex_make, mutex_kill); }

int bionic_pthread_mutex_init(void *obj, const long *attr) {
  (void)attr; /* every engine mutex is recursive, see the header comment */
  return object_init(obj, BIONIC_MUTEX_SIZE, mutex_make(0), mutex_kill);
}

int bionic_pthread_mutex_destroy(void *obj) {
  object_destroy(obj, mutex_kill);
  return 0;
}

int bionic_pthread_mutex_lock(void *obj) {
  pthread_mutex_t *m = mutex_get(obj);
  return m ? pthread_mutex_lock(m) : ENOMEM;
}

int bionic_pthread_mutex_trylock(void *obj) {
  pthread_mutex_t *m = mutex_get(obj);
  return m ? pthread_mutex_trylock(m) : ENOMEM;
}

int bionic_pthread_mutex_unlock(void *obj) {
  pthread_mutex_t *m = mutex_get(obj);
  return m ? pthread_mutex_unlock(m) : ENOMEM;
}

int bionic_pthread_mutex_timedlock(void *obj, const struct timespec *abstime) {
  pthread_mutex_t *m = mutex_get(obj);
  return m ? pthread_mutex_timedlock(m, abstime) : ENOMEM;
}

int bionic_pthread_mutex_lock_timeout_np(void *obj, unsigned ms) {
  pthread_mutex_t *m = mutex_get(obj);
  if (!m)
    return ENOMEM;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  const int rc = pthread_mutex_timedlock(m, &ts);
  return rc == ETIMEDOUT ? EBUSY : rc;
}

/* ==========================================================================
 * condition variables
 * ======================================================================== */

int bionic_pthread_condattr_init(bionic_pthread_condattr_t *a) { *a = 0; return 0; }
int bionic_pthread_condattr_destroy(bionic_pthread_condattr_t *a) { *a = -1; return 0; }
int bionic_pthread_condattr_setpshared(bionic_pthread_condattr_t *a, int p) { (void)a; return p ? ENOTSUP : 0; }

int bionic_pthread_condattr_setclock(bionic_pthread_condattr_t *a, clockid_t clock) {
  if (clock == CLOCK_MONOTONIC)
    *a |= BIONIC_CONDATTR_CLOCK_MONOTONIC;
  else if (clock == CLOCK_REALTIME)
    *a &= ~(long)BIONIC_CONDATTR_CLOCK_MONOTONIC;
  else
    return EINVAL;
  return 0;
}

int bionic_pthread_condattr_getclock(const bionic_pthread_condattr_t *a, clockid_t *clock) {
  *clock = (*a & BIONIC_CONDATTR_CLOCK_MONOTONIC) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
  return 0;
}

static pthread_cond_t *cond_new(int monotonic) {
  pthread_cond_t *c = malloc(sizeof(*c));
  if (!c)
    return NULL;
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  if (monotonic)
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(c, &ca);
  pthread_condattr_destroy(&ca);
  return c;
}

/* PTHREAD_COND_INITIALIZER_MONOTONIC_NP sets bionic's clock bit (0x2) */
static void *cond_make(uint32_t init) { return cond_new((init & 0x2) != 0); }

static void cond_kill(void *c) {
  pthread_cond_destroy(c);
  free(c);
}

static pthread_cond_t *cond_get(void *obj) { return object_get(obj, cond_make, cond_kill); }

int bionic_pthread_cond_init(void *obj, const bionic_pthread_condattr_t *attr) {
  return object_init(obj, BIONIC_COND_SIZE,
                     cond_new(attr && (*attr & BIONIC_CONDATTR_CLOCK_MONOTONIC)), cond_kill);
}

int bionic_pthread_cond_destroy(void *obj) {
  object_destroy(obj, cond_kill);
  return 0;
}

int bionic_pthread_cond_signal(void *obj) {
  pthread_cond_t *c = cond_get(obj);
  return c ? pthread_cond_signal(c) : ENOMEM;
}

int bionic_pthread_cond_broadcast(void *obj) {
  pthread_cond_t *c = cond_get(obj);
  return c ? pthread_cond_broadcast(c) : ENOMEM;
}

int bionic_pthread_cond_wait(void *cobj, void *mobj) {
  pthread_cond_t *c = cond_get(cobj);
  pthread_mutex_t *m = mutex_get(mobj);
  return (c && m) ? pthread_cond_wait(c, m) : ENOMEM;
}

int bionic_pthread_cond_timedwait(void *cobj, void *mobj, const struct timespec *abstime) {
  pthread_cond_t *c = cond_get(cobj);
  pthread_mutex_t *m = mutex_get(mobj);
  return (c && m) ? pthread_cond_timedwait(c, m, abstime) : ENOMEM;
}

int bionic_pthread_cond_clockwait(void *cobj, void *mobj, clockid_t clock,
                                  const struct timespec *abstime) {
  pthread_cond_t *c = cond_get(cobj);
  pthread_mutex_t *m = mutex_get(mobj);
  return (c && m) ? pthread_cond_clockwait(c, m, clock, abstime) : ENOMEM;
}

int bionic_pthread_cond_timedwait_monotonic_np(void *cobj, void *mobj, const struct timespec *abstime) {
  return bionic_pthread_cond_clockwait(cobj, mobj, CLOCK_MONOTONIC, abstime);
}

int bionic_pthread_cond_timedwait_relative_np(void *cobj, void *mobj, const struct timespec *rel) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  ts.tv_sec += rel->tv_sec;
  ts.tv_nsec += rel->tv_nsec;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  return bionic_pthread_cond_clockwait(cobj, mobj, CLOCK_MONOTONIC, &ts);
}

/* ==========================================================================
 * rwlocks
 * ======================================================================== */

int bionic_pthread_rwlockattr_init(long *a) { *a = 0; return 0; }
int bionic_pthread_rwlockattr_destroy(long *a) { *a = -1; return 0; }

static void *rwlock_make(uint32_t init) {
  (void)init;
  pthread_rwlock_t *rw = malloc(sizeof(*rw));
  if (rw)
    pthread_rwlock_init(rw, NULL);
  return rw;
}

static void rwlock_kill(void *rw) {
  pthread_rwlock_destroy(rw);
  free(rw);
}

static pthread_rwlock_t *rwlock_get(void *obj) { return object_get(obj, rwlock_make, rwlock_kill); }

int bionic_pthread_rwlock_init(void *obj, const long *attr) {
  (void)attr;
  return object_init(obj, BIONIC_RWLOCK_SIZE, rwlock_make(0), rwlock_kill);
}

int bionic_pthread_rwlock_destroy(void *obj) {
  object_destroy(obj, rwlock_kill);
  return 0;
}

#define RW_OP(name, call)                         \
  int bionic_pthread_rwlock_##name(void *obj) {   \
    pthread_rwlock_t *rw = rwlock_get(obj);       \
    return rw ? call(rw) : ENOMEM;                \
  }
RW_OP(rdlock, pthread_rwlock_rdlock)
RW_OP(wrlock, pthread_rwlock_wrlock)
RW_OP(tryrdlock, pthread_rwlock_tryrdlock)
RW_OP(trywrlock, pthread_rwlock_trywrlock)
RW_OP(unlock, pthread_rwlock_unlock)

int bionic_pthread_rwlock_timedrdlock(void *obj, const struct timespec *abstime) {
  pthread_rwlock_t *rw = rwlock_get(obj);
  return rw ? pthread_rwlock_timedrdlock(rw, abstime) : ENOMEM;
}

int bionic_pthread_rwlock_timedwrlock(void *obj, const struct timespec *abstime) {
  pthread_rwlock_t *rw = rwlock_get(obj);
  return rw ? pthread_rwlock_timedwrlock(rw, abstime) : ENOMEM;
}

/* ==========================================================================
 * semaphores: bionic's sem_t is 16 bytes, glibc's 32. sem_* report errors
 * through errno and -1, unlike the pthread family.
 * ======================================================================== */

static sem_t *sem_of(void *obj) {
  const uint32_t w = word_load(obj);
  sem_t *s = (w & HANDLE_TAG) ? handle_get(w) : NULL;
  if (!s)
    errno = EINVAL;
  return s;
}

static void sem_kill(void *s) {
  sem_destroy(s);
  free(s);
}

int bionic_sem_init(void *obj, int pshared, unsigned value) {
  (void)pshared;
  sem_t *s = malloc(sizeof(*s));
  if (!s) {
    errno = ENOMEM;
    return -1;
  }
  if (sem_init(s, 0, value) < 0) {
    const int e = errno;
    free(s);
    errno = e;
    return -1;
  }
  const int rc = object_init(obj, BIONIC_SEM_SIZE, s, sem_kill);
  if (rc) {
    errno = rc;
    return -1;
  }
  return 0;
}

int bionic_sem_destroy(void *obj) {
  object_destroy(obj, sem_kill);
  return 0;
}

int bionic_sem_post(void *obj) {
  sem_t *s = sem_of(obj);
  return s ? sem_post(s) : -1;
}

int bionic_sem_wait(void *obj) {
  sem_t *s = sem_of(obj);
  if (!s)
    return -1;
  int rc;
  while ((rc = sem_wait(s)) < 0 && errno == EINTR)
    ;
  return rc;
}

int bionic_sem_trywait(void *obj) {
  sem_t *s = sem_of(obj);
  return s ? sem_trywait(s) : -1;
}

int bionic_sem_timedwait(void *obj, const struct timespec *abstime) {
  sem_t *s = sem_of(obj);
  return s ? sem_timedwait(s, abstime) : -1;
}

int bionic_sem_getvalue(void *obj, int *value) {
  sem_t *s = sem_of(obj);
  return s ? sem_getvalue(s, value) : -1;
}

/* Named semaphores become private unnamed ones: the game uses them only
 * within the process. The handle is a 16-byte bionic sem_t of our own. */
void *bionic_sem_open(const char *name, int oflag, ...) {
  (void)name;
  unsigned value = 0;
  if (oflag & O_CREAT) {
    va_list va;
    va_start(va, oflag);
    (void)va_arg(va, int); /* mode */
    value = va_arg(va, unsigned);
    va_end(va);
  }
  void *holder = calloc(1, BIONIC_SEM_SIZE);
  if (!holder) {
    errno = ENOMEM;
    return SEM_FAILED;
  }
  if (bionic_sem_init(holder, 0, value) < 0) {
    free(holder);
    return SEM_FAILED;
  }
  return holder;
}

int bionic_sem_close(void *obj) {
  if (obj) {
    bionic_sem_destroy(obj);
    free(obj);
  }
  return 0;
}

int bionic_sem_unlink(const char *name) {
  (void)name;
  return 0;
}
