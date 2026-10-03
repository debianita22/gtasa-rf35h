/* bionic_lib.c -- a stand-in for an NDK-built arm64 library
 *
 * Built with -nostdlib and Android's stack-protector convention (canary at
 * TPIDR_EL0+0x28), it imports libc by bionic names with bionic layouts, the
 * way libGame.so does, and exports test_* functions the driver calls after
 * loading it with the real loader and import table. Each returns 0 on
 * success or a line number on failure.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned long uintptr_t;
typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef struct __sFILE FILE;

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

/* ---- bionic data and errno ---- */
extern char __sF[];                 /* FILE __sF[3], 152 bytes each */
extern FILE *stdout;
extern const char *_ctype_;
extern int *__errno(void);
#define errno (*__errno())

/* ---- libc by bionic names ---- */
extern int fprintf(FILE *f, const char *fmt, ...);
extern int fputs(const char *s, FILE *f);
extern int fflush(FILE *f);
extern int fileno(FILE *f);
extern int snprintf(char *s, size_t n, const char *fmt, ...);
extern size_t strlen(const char *s);
extern int strcmp(const char *a, const char *b);
extern void *memset(void *s, int c, size_t n);
extern void *malloc(size_t n);
extern void free(void *p);
extern int close(int fd);
extern int open(const char *path, int flags, ...);
extern ssize_t read(int fd, void *buf, size_t n);
extern long sysconf(int name);
extern int usleep(unsigned us);
extern int sched_yield(void);
extern char *strerror(int e);
extern FILE *fopen(const char *path, const char *mode);
extern int fclose(FILE *f);
extern size_t fread(void *p, size_t size, size_t n, FILE *f);
extern int openat(int dirfd, const char *path, int flags, ...);
extern int __open_2(const char *path, int flags);

/* ---- Android NDK, as the game calls it ---- */
extern void *AAssetManager_open(void *mgr, const char *name, int mode);
extern int AAsset_read(void *asset, void *buf, size_t count);
extern long AAsset_getLength(void *asset);
extern void AAsset_close(void *asset);
extern int ANativeWindow_setBuffersGeometry(void *win, int w, int h, int format);

/* ---- fortified entry points (bionic's own signatures) ---- */
extern char *__fgets_chk(char *dst, int size, FILE *stream, size_t bos);
extern size_t __fread_chk(void *buf, size_t size, size_t count, FILE *stream, size_t bos);
extern ssize_t __read_chk(int fd, void *buf, size_t count, size_t bos);
extern size_t __strlen_chk(const char *s, size_t bos);
extern int __snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...);
extern char *__strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len, size_t src_len);

/* ---- stat / dirent with bionic arm64 layouts ---- */
struct bionic_stat {
  uint64_t st_dev, st_ino;
  uint32_t st_mode, st_nlink, st_uid, st_gid;
  uint64_t st_rdev, __pad1;
  long st_size;
  int st_blksize, __pad2;
  long st_blocks;
  long st_atim[2], st_mtim[2], st_ctim[2];
  uint32_t __unused4, __unused5;
};
extern int stat(const char *path, struct bionic_stat *st);
extern int lstat(const char *path, struct bionic_stat *st);
struct bionic_dirent {
  uint64_t d_ino;
  long d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256];
};
extern void *opendir(const char *path);
extern struct bionic_dirent *readdir(void *dir);
extern int closedir(void *dir);

/* ---- pthread with bionic sizes ---- */
typedef long pthread_t;
typedef struct { int __private[10]; } pthread_mutex_t;   /* 40 bytes */
typedef struct { int __private[12]; } pthread_cond_t;    /* 48 bytes */
typedef struct { int __private[14]; } pthread_rwlock_t;  /* 56 bytes */
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int sched_policy, sched_priority;
  char __reserved[16];
} pthread_attr_t;
typedef long pthread_mutexattr_t;
typedef struct { unsigned int count; int __reserved[3]; } sem_t; /* 16 bytes */
struct timespec { long tv_sec, tv_nsec; };
#define PTHREAD_MUTEX_INITIALIZER { { 0 } }
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP { { 0x4000 } }
#define PTHREAD_COND_INITIALIZER { { 0 } }
#define PTHREAD_COND_INITIALIZER_MONOTONIC_NP { { 1 << 1 } }
#define PTHREAD_RWLOCK_INITIALIZER { { 0 } }
extern int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
extern int pthread_join(pthread_t t, void **ret);
extern int pthread_attr_init(pthread_attr_t *a);
extern int pthread_attr_setstacksize(pthread_attr_t *a, size_t s);
extern int pthread_attr_getstacksize(const pthread_attr_t *a, size_t *s);
extern int pthread_attr_destroy(pthread_attr_t *a);
extern int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
extern int pthread_mutex_lock(pthread_mutex_t *m);
extern int pthread_mutex_trylock(pthread_mutex_t *m);
extern int pthread_mutex_unlock(pthread_mutex_t *m);
extern int pthread_mutex_destroy(pthread_mutex_t *m);
extern int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
extern int pthread_cond_signal(pthread_cond_t *c);
extern int pthread_cond_broadcast(pthread_cond_t *c);
extern int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t);
extern int pthread_rwlock_rdlock(pthread_rwlock_t *rw);
extern int pthread_rwlock_wrlock(pthread_rwlock_t *rw);
extern int pthread_rwlock_unlock(pthread_rwlock_t *rw);
extern int pthread_once(int *once, void (*fn)(void));
extern int sem_init(sem_t *s, int pshared, unsigned v);
extern int sem_post(sem_t *s);
extern int sem_wait(sem_t *s);
extern int sem_trywait(sem_t *s);
extern int sem_getvalue(sem_t *s, int *v);
extern int sem_destroy(sem_t *s);
extern int clock_gettime(int clk, struct timespec *ts);

/* ---- signals and jumps, bionic layouts ---- */
typedef unsigned long sigset_t;
struct bionic_sigaction {
  int sa_flags;
  void (*sa_handler)(int);
  sigset_t sa_mask;
  void (*sa_restorer)(void);
};
extern int sigaction(int sig, const struct bionic_sigaction *a, struct bionic_sigaction *o);
extern int sigemptyset(sigset_t *s);
extern int sigaddset(sigset_t *s, int sig);
extern int sigismember(const sigset_t *s, int sig);
extern int sigprocmask(int how, const sigset_t *set, sigset_t *old);
extern int raise(int sig);
typedef long jmp_buf[32];
extern int setjmp(jmp_buf env);
extern void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* ---- a weak import nobody provides: must bind to NULL ---- */
extern int gtasa_test_nonexistent_symbol(void) __attribute__((weak));

/* ======================================================================= */

#define STDOUT ((FILE *)(__sF + 152))
#define STDERR ((FILE *)(__sF + 2 * 152))

static int constructed;
__attribute__((constructor)) static void ctor(void) { constructed = 42; }

/* relocated data: function pointers and pointers to data */
static int answer = 7;
int *answer_ptr = &answer;
static int twice(int x) { return 2 * x; }
int (*twice_ptr)(int) = twice;
const char *const strings[] = { "alpha", "beta", "gamma" };

int test_relocs(void) {
  CHECK(constructed == 42);
  CHECK(*answer_ptr == 7);
  CHECK(twice_ptr(21) == 42);
  CHECK(strcmp(strings[2], "gamma") == 0);
  CHECK(gtasa_test_nonexistent_symbol == 0);
  return 0;
}

int test_errno(void) {
  errno = 0;
  CHECK(open("/nonexistent/path", 0) < 0);
  CHECK(errno == 2); /* ENOENT */
  errno = 0;
  CHECK(close(-1) < 0 && errno == 9); /* EBADF */
  return 0;
}

int test_stdio(void) {
  CHECK(fprintf(STDERR, "bionic_lib: hello through __sF[2]\n") > 0);
  CHECK(fputs("bionic_lib: hello through stdout\n", stdout) >= 0);
  CHECK(fflush(STDOUT) == 0);
  CHECK(fileno(STDERR) == 2);
  CHECK(stdout == STDOUT); /* the stdout variable points into __sF */
  return 0;
}

int test_fortify(const char *path) {
  FILE *f = fopen(path, "w");
  CHECK(f != 0);
  CHECK(fputs("first line\nsecond\n", f) >= 0);
  CHECK(fclose(f) == 0);
  f = fopen(path, "r");
  CHECK(f != 0);
  char line[32], rest[8];
  memset(rest, 0, sizeof(rest));
  CHECK(__fgets_chk(line, sizeof(line), f, sizeof(line)) == line);
  CHECK(strcmp(line, "first line\n") == 0);
  CHECK(__fread_chk(rest, 1, 7, f, sizeof(rest)) == 7);
  CHECK(strcmp(rest, "second\n") == 0);
  CHECK(fclose(f) == 0);
  const int fd = open(path, 0 /* O_RDONLY */);
  CHECK(fd >= 0);
  CHECK(__read_chk(fd, line, 5, sizeof(line)) == 5 && line[4] == 't');
  close(fd);
  CHECK(__strlen_chk("abc", 4) == 3);
  CHECK(__snprintf_chk(line, sizeof(line), 0, sizeof(line), "%d-%s", 42, "x") == 4);
  CHECK(strcmp(line, "42-x") == 0);
  CHECK(__strncpy_chk2(line, "copy", 5, sizeof(line), 5) == line && strcmp(line, "copy") == 0);
  return 0;
}

/* names spelled in another case than on disk (the game ran on FAT/exFAT):
 * the driver made tests/build/CaseDir/Sub/MixedName.txt holding "abc" */
int test_casefold(void) {
  char buf[8];
  memset(buf, 0, sizeof(buf));
  FILE *f = fopen("tests/build/casedir/SUB/mixedname.TXT", "r");
  CHECK(f != 0);
  CHECK(fread(buf, 1, 3, f) == 3 && strcmp(buf, "abc") == 0);
  fclose(f);
  struct bionic_stat st;
  CHECK(stat("tests/build/CASEDIR/sub/MIXEDNAME.txt", &st) == 0 && st.st_size == 3);
  const int fd = open("tests/build/casedir/sub/mixedname.txt", 0 /* O_RDONLY */);
  CHECK(fd >= 0);
  memset(buf, 0, sizeof(buf));
  CHECK(read(fd, buf, 3) == 3 && strcmp(buf, "abc") == 0);
  close(fd);
  void *d = opendir("tests/build/casedir/SUB");
  CHECK(d != 0);
  closedir(d);
  CHECK(lstat("tests/build/casedir/Sub/MIXEDname.txt", &st) == 0 && st.st_size == 3);
  const int fd2 = openat(-100 /* AT_FDCWD */, "tests/build/CaseDir/SUB/mixedname.txt", 0);
  CHECK(fd2 >= 0);
  close(fd2);
  const int fd3 = __open_2("tests/build/casedir/sub/MIXEDNAME.TXT", 0);
  CHECK(fd3 >= 0);
  close(fd3);
  /* a trailing slash names a directory, whatever the case */
  CHECK(stat("tests/build/casedir/sub/MIXEDNAME.txt/", &st) < 0);
  CHECK(fopen("tests/build/casedir/sub/missing.txt", "r") == 0 && errno == 2); /* ENOENT */
  CHECK(stat("tests/build/casedir/sub/missing.txt", &st) < 0 && errno == 2);
  /* a miss is remembered only until its directory changes */
  CHECK(stat("tests/build/casedir/sub/Later.txt", &st) < 0 && errno == 2);
  FILE *w = fopen("tests/build/CaseDir/Sub/later.TXT", "w");
  CHECK(w != 0);
  fclose(w);
  CHECK(stat("tests/build/casedir/sub/Later.txt", &st) == 0);
  return 0;
}

/* AAsset in a folder whose data/ is spelled DATA/ (the driver made it and
 * runs this from there): the index finds the root, the open the file */
int test_aasset(void) {
  void *a = AAssetManager_open((void *)1, "data/models/thing.dat", 2);
  CHECK(a != 0);
  char buf[8];
  memset(buf, 0, sizeof(buf));
  CHECK(AAsset_getLength(a) == 3 && AAsset_read(a, buf, 3) == 3 && strcmp(buf, "xyz") == 0);
  AAsset_close(a);
  a = AAssetManager_open((void *)1, "es2/SHADER.txt", 2);
  CHECK(a != 0);
  AAsset_close(a);
  CHECK(AAssetManager_open((void *)1, "data/models/none.dat", 2) == 0);
  /* setBuffersGeometry: both sizes or neither, as on Android */
  CHECK(ANativeWindow_setBuffersGeometry((void *)1, 0, 240, 4) == -22);
  CHECK(ANativeWindow_setBuffersGeometry((void *)1, -1, -1, 4) == -22);
  return 0;
}

int test_ctype(void) {
  CHECK(_ctype_ != 0);
  CHECK((_ctype_ + 1)['A'] & 0x01);  /* _U */
  CHECK((_ctype_ + 1)['z'] & 0x02);  /* _L */
  CHECK((_ctype_ + 1)['7'] & 0x04);  /* _N */
  CHECK((_ctype_ + 1)[' '] & 0x08);  /* _S */
  CHECK((_ctype_ + 1)['!'] & 0x10);  /* _P */
  CHECK((_ctype_ + 1)['\n'] & 0x20); /* _C */
  CHECK((_ctype_ + 1)['f'] & 0x40);  /* _X */
  CHECK(!((_ctype_ + 1)['g'] & 0x40));
  CHECK((_ctype_ + 1)[-1] == 0);     /* EOF entry */
  return 0;
}

int test_sysconf(void) {
  CHECK(sysconf(0x27) == 4096 || sysconf(0x27) == 16384 || sysconf(0x27) == 65536);
  CHECK(sysconf(0x61) >= 1);  /* _SC_NPROCESSORS_ONLN */
  CHECK(sysconf(0x62) > 0);   /* _SC_PHYS_PAGES */
  CHECK(sysconf(0x7fff) == -1);
  return 0;
}

int test_stat(const char *path) {
  struct bionic_stat st;
  memset(&st, 0xa5, sizeof(st));
  CHECK(stat(path, &st) == 0);
  CHECK((st.st_mode & 0170000) == 0100000); /* S_IFREG */
  CHECK(st.st_size > 1000);
  CHECK(st.st_nlink >= 1);
  CHECK(st.st_mtim[0] > 1000000000);
  return 0;
}

int test_readdir(const char *dir, const char *want) {
  void *d = opendir(dir);
  CHECK(d != 0);
  int found = 0;
  struct bionic_dirent *e;
  while ((e = readdir(d)))
    if (strcmp(e->d_name, want) == 0)
      found = 1;
  closedir(d);
  CHECK(found);
  return 0;
}

static pthread_mutex_t static_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t static_rmutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

int test_mutex(void) {
  CHECK(pthread_mutex_lock(&static_mutex) == 0);
  CHECK(pthread_mutex_lock(&static_mutex) == 0); /* every engine mutex is recursive */
  CHECK(pthread_mutex_unlock(&static_mutex) == 0);
  CHECK(pthread_mutex_unlock(&static_mutex) == 0);
  CHECK(pthread_mutex_trylock(&static_rmutex) == 0);
  CHECK(pthread_mutex_unlock(&static_rmutex) == 0);
  pthread_mutex_t m;
  memset(&m, 0x5a, sizeof(m));
  CHECK(pthread_mutex_init(&m, 0) == 0);
  CHECK(pthread_mutex_lock(&m) == 0);
  CHECK(pthread_mutex_unlock(&m) == 0);
  CHECK(pthread_mutex_destroy(&m) == 0);
  return 0;
}

/* --- threads: counter under a static mutex, a condition and a semaphore --- */

static pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t count_cond = PTHREAD_COND_INITIALIZER;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static long counter, rw_value;
static sem_t done;

/* stack-protected and busy: runs on every worker while errno and libc TLS
 * change underneath; a moving canary would abort the process */
static int __attribute__((noinline)) protected_work(int seed) {
  char buf[64];
  snprintf(buf, sizeof(buf), "seed %d", seed);
  errno = seed & 0xff;
  close(-1); /* sets libc's errno through our __errno */
  sched_yield();
  return (int)strlen(buf);
}

static void *worker(void *arg) {
  const long id = (long)arg;
  for (int i = 0; i < 2000; i++) {
    protected_work((int)(id * 1000 + i));
    pthread_mutex_lock(&count_lock);
    counter++;
    if (counter % 1000 == 0)
      pthread_cond_broadcast(&count_cond);
    pthread_mutex_unlock(&count_lock);
    if (i % 100 == 0) {
      pthread_rwlock_wrlock(&rw);
      rw_value++;
      pthread_rwlock_unlock(&rw);
    }
  }
  sem_post(&done);
  return (void *)(id + 100);
}

static int once_flag;
static int once_runs;
static void once_fn(void) { once_runs++; }

int test_threads(int nthreads) {
  CHECK(sem_init(&done, 0, 0) == 0);
  pthread_attr_t attr;
  CHECK(pthread_attr_init(&attr) == 0);
  CHECK(pthread_attr_setstacksize(&attr, 256 * 1024) == 0);
  size_t ss = 0;
  CHECK(pthread_attr_getstacksize(&attr, &ss) == 0 && ss == 256 * 1024);
  pthread_t t[8];
  for (long i = 0; i < nthreads; i++)
    CHECK(pthread_create(&t[i], &attr, worker, (void *)i) == 0);
  pthread_attr_destroy(&attr);

  pthread_mutex_lock(&count_lock);
  while (counter < 1000)
    pthread_cond_wait(&count_cond, &count_lock);
  pthread_mutex_unlock(&count_lock);

  for (int i = 0; i < nthreads; i++)
    CHECK(sem_wait(&done) == 0);
  int v = -1;
  CHECK(sem_getvalue(&done, &v) == 0 && v == 0);
  CHECK(sem_trywait(&done) == -1);
  for (long i = 0; i < nthreads; i++) {
    void *ret = 0;
    CHECK(pthread_join(t[i], &ret) == 0);
    CHECK((long)ret == i + 100);
  }
  CHECK(counter == 2000L * nthreads);
  pthread_rwlock_rdlock(&rw);
  CHECK(rw_value == 20L * nthreads);
  pthread_rwlock_unlock(&rw);
  CHECK(sem_destroy(&done) == 0);

  for (int i = 0; i < 5; i++)
    CHECK(pthread_once(&once_flag, once_fn) == 0);
  CHECK(once_runs == 1);
  return 0;
}

/* bionic's minimum stack is 16 KiB, glibc's much more: an Android size is
 * accepted and the thread still runs */
static void *small_stack_worker(void *arg) { return (void *)((long)arg + 1); }

int test_small_stack(void) {
  pthread_attr_t a;
  CHECK(pthread_attr_init(&a) == 0);
  CHECK(pthread_attr_setstacksize(&a, 8 * 1024) == 22); /* EINVAL below bionic's minimum */
  CHECK(pthread_attr_setstacksize(&a, 16 * 1024) == 0);
  size_t ss = 0;
  CHECK(pthread_attr_getstacksize(&a, &ss) == 0 && ss == 16 * 1024);
  pthread_t t;
  CHECK(pthread_create(&t, &a, small_stack_worker, (void *)41) == 0);
  void *ret = 0;
  CHECK(pthread_join(t, &ret) == 0 && (long)ret == 42);
  pthread_attr_destroy(&a);
  return 0;
}

/* a condition initialised with PTHREAD_COND_INITIALIZER_MONOTONIC_NP takes
 * a CLOCK_MONOTONIC deadline: 30 ms from now must mean 30 ms (read against
 * the realtime clock, it is decades past and returns at once) */
int test_timedwait_monotonic(void) {
  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t c = PTHREAD_COND_INITIALIZER_MONOTONIC_NP;
  struct timespec t0, ts, t1;
  clock_gettime(1 /* CLOCK_MONOTONIC */, &t0);
  ts = t0;
  ts.tv_nsec += 30 * 1000 * 1000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&m);
  CHECK(pthread_cond_timedwait(&c, &m, &ts) == 110); /* ETIMEDOUT */
  pthread_mutex_unlock(&m);
  clock_gettime(1, &t1);
  const long waited_ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  CHECK(waited_ms >= 25);
  return 0;
}

int test_timedwait(void) {
  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t c = PTHREAD_COND_INITIALIZER;
  struct timespec ts;
  clock_gettime(0 /* CLOCK_REALTIME */, &ts);
  ts.tv_nsec += 20 * 1000 * 1000;
  if (ts.tv_nsec >= 1000000000) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&m);
  CHECK(pthread_cond_timedwait(&c, &m, &ts) == 110); /* ETIMEDOUT */
  pthread_mutex_unlock(&m);
  return 0;
}

/* --- objects at 4-byte alignment, which bionic's int arrays allow: the
 * engine embeds them in structures, so any 8-byte atomic on them faults
 * (qemu enforces it, as the Cortex-A35 does) --- */
struct packed_objects {
  int pad;
  pthread_mutex_t m;   /* offset 4 */
  pthread_cond_t c;    /* offset 44 */
  pthread_rwlock_t rw; /* offset 92 */
  sem_t s;             /* offset 148 */
  pthread_mutex_t m2;  /* offset 164 */
};
static struct packed_objects po __attribute__((aligned(8))) = {
  0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_RWLOCK_INITIALIZER, { 0, { 0 } },
  PTHREAD_MUTEX_INITIALIZER,
};
static long race_counter;

/* first use of a static mutex from several threads at once: every thread
 * must end up on the same lock */
static void *racer(void *arg) {
  (void)arg;
  for (int i = 0; i < 500; i++) {
    pthread_mutex_lock(&po.m2);
    race_counter++;
    pthread_mutex_unlock(&po.m2);
  }
  return 0;
}

int test_misaligned(void) {
  CHECK(((uintptr_t)&po.m & 7) == 4 && ((uintptr_t)&po.c & 7) == 4);
  CHECK(((uintptr_t)&po.rw & 7) == 4 && ((uintptr_t)&po.s & 7) == 4);
  CHECK(pthread_mutex_lock(&po.m) == 0);
  struct timespec past = { 0, 0 };
  CHECK(pthread_cond_timedwait(&po.c, &po.m, &past) == 110); /* ETIMEDOUT */
  CHECK(pthread_cond_signal(&po.c) == 0);
  CHECK(pthread_mutex_unlock(&po.m) == 0);
  CHECK(pthread_rwlock_rdlock(&po.rw) == 0 && pthread_rwlock_unlock(&po.rw) == 0);
  CHECK(pthread_rwlock_wrlock(&po.rw) == 0 && pthread_rwlock_unlock(&po.rw) == 0);
  CHECK(sem_init(&po.s, 0, 1) == 0);
  CHECK(sem_wait(&po.s) == 0 && sem_post(&po.s) == 0);
  int v = -1;
  CHECK(sem_getvalue(&po.s, &v) == 0 && v == 1);
  CHECK(sem_destroy(&po.s) == 0);
  /* explicit init over garbage, at the same misaligned place */
  memset(&po.m, 0x5a, sizeof(po.m));
  CHECK(pthread_mutex_init(&po.m, 0) == 0);
  CHECK(pthread_mutex_trylock(&po.m) == 0 && pthread_mutex_unlock(&po.m) == 0);
  CHECK(pthread_mutex_destroy(&po.m) == 0);

  pthread_t t[4];
  for (long i = 0; i < 4; i++)
    CHECK(pthread_create(&t[i], 0, racer, 0) == 0);
  for (int i = 0; i < 4; i++)
    CHECK(pthread_join(t[i], 0) == 0);
  CHECK(race_counter == 2000);
  return 0;
}

static jmp_buf jb;
static volatile int jumped;

static void __attribute__((noinline)) deep(int n) {
  char pad[128];
  memset(pad, n, sizeof(pad));
  if (n == 0)
    longjmp(jb, 0); /* longjmp(.., 0) must return 1 */
  deep(n - 1);
}

int test_setjmp(void) {
  volatile int stage = 0;
  const int r = setjmp(jb);
  if (r == 0) {
    stage = 1;
    deep(10);
    return __LINE__; /* not reached */
  }
  CHECK(r == 1 && stage == 1);
  jumped = 1;
  return 0;
}

static volatile int got_signal;
static void on_usr1(int sig) { got_signal = sig; }

int test_signal(void) {
  struct bionic_sigaction sa, old;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_usr1;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, 12);
  CHECK(sigismember(&sa.sa_mask, 12) == 1);
  CHECK(sigaction(10 /* SIGUSR1 */, &sa, &old) == 0);
  CHECK(raise(10) == 0);
  CHECK(got_signal == 10);
  struct bionic_sigaction now;
  CHECK(sigaction(10, 0, &now) == 0 && now.sa_handler == on_usr1);
  CHECK(sigismember(&now.sa_mask, 12) == 1);
  /* crash signals stay with the platform's reporter */
  CHECK(sigaction(11 /* SIGSEGV */, &sa, 0) == 0);
  CHECK(sigaction(11, 0, &now) == 0 && now.sa_handler != on_usr1);
  sigset_t block = 0, prev = 0;
  sigaddset(&block, 12);
  CHECK(sigprocmask(0 /* SIG_BLOCK */, &block, &prev) == 0);
  CHECK(sigprocmask(2 /* SIG_SETMASK */, &prev, 0) == 0);
  return 0;
}

/* the canary check itself: a protected function returning normally after
 * libc has written its TLS (errno) many times must not trip */
int test_canary(void) {
  int total = 0;
  for (int i = 0; i < 1000; i++)
    total += protected_work(i);
  CHECK(total > 0);
  return 0;
}

/* ---- many non-relative relocations, so packed tables (APS2) get groups
 * sharing an offset delta, groups sharing r_info, and mixed ones ---- */
typedef void (*fnptr)(void);
fnptr abs_imports[] = {
  (fnptr)strlen, (fnptr)strcmp, (fnptr)memset, (fnptr)malloc, (fnptr)free,
  (fnptr)close, (fnptr)open, (fnptr)read, (fnptr)sysconf, (fnptr)usleep,
  (fnptr)strlen, (fnptr)strlen, (fnptr)strlen, (fnptr)strlen, (fnptr)strlen,
  (fnptr)strlen, (fnptr)strlen, (fnptr)strlen, (fnptr)free, (fnptr)malloc,
  (fnptr)pthread_mutex_lock, (fnptr)pthread_mutex_unlock, (fnptr)sem_post,
  (fnptr)sem_wait, (fnptr)stat, (fnptr)readdir, (fnptr)opendir, (fnptr)closedir,
};
const char *abs_data[] = {
  (const char *)&_ctype_, (const char *)&stdout, __sF, __sF + 152, __sF + 304,
};

int test_abs_relocs(void) {
  /* the same functions through the GOT (GLOB_DAT) must match the ABS64 copies */
  fnptr (*volatile got[])(void) = { 0 };
  (void)got;
  fnptr expect[] = {
    (fnptr)&strlen, (fnptr)&strcmp, (fnptr)&memset, (fnptr)&malloc, (fnptr)&free,
    (fnptr)&close, (fnptr)&open, (fnptr)&read, (fnptr)&sysconf, (fnptr)&usleep,
  };
  for (int i = 0; i < 10; i++)
    CHECK(abs_imports[i] == expect[i]);
  for (int i = 10; i < 18; i++)
    CHECK(abs_imports[i] == (fnptr)&strlen);
  CHECK(abs_imports[18] == (fnptr)&free && abs_imports[19] == (fnptr)&malloc);
  CHECK(abs_imports[20] == (fnptr)&pthread_mutex_lock);
  CHECK(abs_imports[27] == (fnptr)&closedir);
  CHECK(abs_data[0] == (const char *)&_ctype_ && abs_data[1] == (const char *)&stdout);
  CHECK(abs_data[3] == __sF + 152 && abs_data[4] == __sF + 304);
  return 0;
}
