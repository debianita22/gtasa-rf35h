/* bionic.h -- bionic-ABI entry points the import table binds the game to
 *
 * Everything here takes or returns bionic layouts (see bionic_types.h) and
 * converts to glibc; functions whose ABI is identical are bound directly in
 * imports.c and do not appear here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_BIONIC_H
#define GTASA_BIONIC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <dirent.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>
#include <pthread.h>

#include "bionic_types.h"

/* ---- startup ---------------------------------------------------------------- */

/* where the game's stdout/stderr go (defaults to the process's own) */
void bionic_set_stdio(FILE *out, FILE *err);
/* value reported for _SC_PHYS_PAGES, in MB; 0 = the real amount */
void bionic_set_reported_ram_mb(unsigned mb);
/* network: when off, socket()/getaddrinfo() fail at once instead of timing out */
void bionic_set_network(int enabled);

/* ---- data symbols ----------------------------------------------------------- */

extern uint8_t bionic_sF[3][BIONIC_FILE_SIZE];
extern FILE *bionic_stdin, *bionic_stdout, *bionic_stderr;
extern const char *bionic_ctype_;            /* bionic: const char *_ctype_ */
extern const short *bionic_tolower_tab_;     /* bionic: const short *_tolower_tab_ */
extern const short *bionic_toupper_tab_;
extern uint64_t bionic_stack_chk_guard;

/* ---- libc ------------------------------------------------------------------- */

int *bionic___errno(void);
void bionic___stack_chk_fail(void) __attribute__((noreturn));
void bionic_abort(void) __attribute__((noreturn));
void bionic_exit(int code) __attribute__((noreturn));
long bionic_sysconf(int name);
long bionic_pathconf(const char *path, int name);
long bionic_syscall(long number, ...);
int bionic_gettid(void);
int bionic_strerror_r(int err, char *buf, size_t len);
size_t bionic___ctype_get_mb_cur_max(void);
int bionic___register_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void), void *dso);
int bionic___cxa_finalize(void *dso);

/* fortify */
void *bionic___memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *bionic___memmove_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *bionic___memset_chk(void *s, int c, size_t n, size_t dstlen);
char *bionic___strcat_chk(char *dst, const char *src, size_t dstlen);
char *bionic___strchr_chk(const char *s, int c, size_t slen);
char *bionic___strrchr_chk(const char *s, int c, size_t slen);
char *bionic___strcpy_chk(char *dst, const char *src, size_t dstlen);
char *bionic___stpcpy_chk(char *dst, const char *src, size_t dstlen);
size_t bionic___strlen_chk(const char *s, size_t slen);
char *bionic___strncat_chk(char *dst, const char *src, size_t n, size_t dstlen);
char *bionic___strncpy_chk(char *dst, const char *src, size_t n, size_t dstlen);
char *bionic___strncpy_chk2(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen);
int bionic___vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int bionic___snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...);
int bionic___vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list va);
int bionic___sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...);
ssize_t bionic___read_chk(int fd, void *buf, size_t count, size_t buf_size);
ssize_t bionic___write_chk(int fd, const void *buf, size_t count, size_t buf_size);
size_t bionic___fread_chk(void *ptr, size_t size, size_t n, FILE *f, size_t buf_size);
char *bionic___fgets_chk(char *s, int size, FILE *f, size_t bos);
void bionic___FD_SET_chk(int fd, void *set, size_t setsize);
void bionic___FD_CLR_chk(int fd, void *set, size_t setsize);
int bionic___FD_ISSET_chk(int fd, const void *set, size_t setsize);
int bionic___open_2(const char *path, int flags);
int bionic___openat_2(int dirfd, const char *path, int flags);
/* lookups that retry a name the game spells in another case (casefold.c) */
int bionic_open(const char *path, int flags, ...);
int bionic_openat(int dirfd, const char *path, int flags, ...);
struct stat;
int bionic_stat(const char *path, struct stat *st);
int bionic_lstat(const char *path, struct stat *st);
DIR *bionic_opendir(const char *path);

/* the game's own console output (goes to the log) */
int bionic_printf(const char *fmt, ...);
int bionic_vprintf(const char *fmt, va_list va);
int bionic_puts(const char *s);
int bionic_putchar(int c);
int bionic_getchar(void);

/* strings bionic has and glibc may not, or with other semantics */
size_t bionic_strlcpy(char *dst, const char *src, size_t size);
size_t bionic_strlcat(char *dst, const char *src, size_t size);
char *bionic_dirname(const char *path);
char *bionic_basename(const char *path);

/* stdio over the fake __sF */
FILE *bionic_fopen(const char *path, const char *mode);
FILE *bionic_fdopen(int fd, const char *mode);
FILE *bionic_freopen(const char *path, const char *mode, FILE *f);
int bionic_fclose(FILE *f);
int bionic_fflush(FILE *f);
size_t bionic_fread(void *ptr, size_t size, size_t n, FILE *f);
size_t bionic_fwrite(const void *ptr, size_t size, size_t n, FILE *f);
int bionic_fgetc(FILE *f);
char *bionic_fgets(char *s, int n, FILE *f);
int bionic_fputc(int c, FILE *f);
int bionic_fputs(const char *s, FILE *f);
int bionic_ungetc(int c, FILE *f);
int bionic_fprintf(FILE *f, const char *fmt, ...);
int bionic_vfprintf(FILE *f, const char *fmt, va_list va);
int bionic_fscanf(FILE *f, const char *fmt, ...);
int bionic_vfscanf(FILE *f, const char *fmt, va_list va);
int bionic_fseek(FILE *f, long off, int whence);
int bionic_fseeko(FILE *f, off_t off, int whence);
long bionic_ftell(FILE *f);
off_t bionic_ftello(FILE *f);
void bionic_rewind(FILE *f);
int bionic_fgetpos(FILE *f, int64_t *pos);
int bionic_fsetpos(FILE *f, const int64_t *pos);
int bionic_feof(FILE *f);
int bionic_ferror(FILE *f);
void bionic_clearerr(FILE *f);
int bionic_fileno(FILE *f);
int bionic_setvbuf(FILE *f, char *buf, int mode, size_t size);
void bionic_setbuf(FILE *f, char *buf);
wint_t bionic_fgetwc(FILE *f);
wint_t bionic_ungetwc(wint_t c, FILE *f);
wint_t bionic_fputwc(wchar_t c, FILE *f);
int bionic_fwide(FILE *f, int mode);
ssize_t bionic_getline(char **line, size_t *n, FILE *f);
ssize_t bionic_getdelim(char **line, size_t *n, int delim, FILE *f);
void bionic_flockfile(FILE *f);
void bionic_funlockfile(FILE *f);

/* locale: bionic only has the C locale; the _l variants ignore theirs */
void *bionic_newlocale(int mask, const char *locale, void *base);
void bionic_freelocale(void *loc);
void *bionic_uselocale(void *loc);
int bionic_isalpha_l(int c, void *loc);
int bionic_isdigit_l(int c, void *loc);
int bionic_isxdigit_l(int c, void *loc);
int bionic_islower_l(int c, void *loc);
int bionic_isupper_l(int c, void *loc);
int bionic_isspace_l(int c, void *loc);
int bionic_isprint_l(int c, void *loc);
int bionic_ispunct_l(int c, void *loc);
int bionic_iscntrl_l(int c, void *loc);
int bionic_isalnum_l(int c, void *loc);
int bionic_isblank_l(int c, void *loc);
int bionic_isgraph_l(int c, void *loc);
int bionic_toupper_l(int c, void *loc);
int bionic_tolower_l(int c, void *loc);
int bionic_iswalpha_l(wint_t c, void *loc);
int bionic_iswblank_l(wint_t c, void *loc);
int bionic_iswcntrl_l(wint_t c, void *loc);
int bionic_iswdigit_l(wint_t c, void *loc);
int bionic_iswlower_l(wint_t c, void *loc);
int bionic_iswprint_l(wint_t c, void *loc);
int bionic_iswpunct_l(wint_t c, void *loc);
int bionic_iswspace_l(wint_t c, void *loc);
int bionic_iswupper_l(wint_t c, void *loc);
int bionic_iswxdigit_l(wint_t c, void *loc);
wint_t bionic_towlower_l(wint_t c, void *loc);
wint_t bionic_towupper_l(wint_t c, void *loc);
int bionic_strcoll_l(const char *a, const char *b, void *loc);
size_t bionic_strxfrm_l(char *dst, const char *src, size_t n, void *loc);
size_t bionic_strftime_l(char *s, size_t max, const char *fmt, const struct tm *tm, void *loc);
long double bionic_strtold_l(const char *s, char **end, void *loc);
long long bionic_strtoll_l(const char *s, char **end, int base, void *loc);
unsigned long long bionic_strtoull_l(const char *s, char **end, int base, void *loc);
float bionic_strtof_l(const char *s, char **end, void *loc);
double bionic_strtod_l(const char *s, char **end, void *loc);
int bionic_wcscoll_l(const wchar_t *a, const wchar_t *b, void *loc);
size_t bionic_wcsxfrm_l(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
size_t bionic_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps);
size_t bionic_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps);

/* network gate */
int bionic_socket(int domain, int type, int protocol);
int bionic_getaddrinfo(const char *node, const char *service, const void *hints, void **res);
void bionic_freeaddrinfo(void *res);
const char *bionic_gai_strerror(int code);
void *bionic_gethostbyname(const char *name);
int bionic_gethostname(char *name, size_t len);

/* dynamic linking */
void *bionic_dlopen(const char *name, int flags);
void *bionic_dlsym(void *handle, const char *name);
int bionic_dlclose(void *handle);
char *bionic_dlerror(void);
int bionic_dladdr(const void *addr, void *info);

/* ---- pthread (bionic_pthread.c) -------------------------------------------- */

int bionic_pthread_create(pthread_t *t, const bionic_pthread_attr_t *attr,
                          void *(*fn)(void *), void *arg);
int bionic_pthread_attr_init(bionic_pthread_attr_t *a);
int bionic_pthread_attr_destroy(bionic_pthread_attr_t *a);
int bionic_pthread_attr_setdetachstate(bionic_pthread_attr_t *a, int state);
int bionic_pthread_attr_getdetachstate(const bionic_pthread_attr_t *a, int *state);
int bionic_pthread_attr_setstacksize(bionic_pthread_attr_t *a, size_t size);
int bionic_pthread_attr_getstacksize(const bionic_pthread_attr_t *a, size_t *size);
int bionic_pthread_attr_setstack(bionic_pthread_attr_t *a, void *base, size_t size);
int bionic_pthread_attr_getstack(const bionic_pthread_attr_t *a, void **base, size_t *size);
int bionic_pthread_attr_setguardsize(bionic_pthread_attr_t *a, size_t size);
int bionic_pthread_attr_getguardsize(const bionic_pthread_attr_t *a, size_t *size);
int bionic_pthread_attr_setschedpolicy(bionic_pthread_attr_t *a, int policy);
int bionic_pthread_attr_getschedpolicy(const bionic_pthread_attr_t *a, int *policy);
int bionic_pthread_attr_setschedparam(bionic_pthread_attr_t *a, const int *param);
int bionic_pthread_attr_getschedparam(const bionic_pthread_attr_t *a, int *param);
int bionic_pthread_attr_setscope(bionic_pthread_attr_t *a, int scope);
int bionic_pthread_attr_getscope(const bionic_pthread_attr_t *a, int *scope);
int bionic_pthread_attr_setinheritsched(bionic_pthread_attr_t *a, int inherit);
int bionic_pthread_getattr_np(pthread_t t, bionic_pthread_attr_t *a);
int bionic_pthread_setschedparam(pthread_t t, int policy, const void *param);
int bionic_pthread_getschedparam(pthread_t t, int *policy, void *param);
int bionic_pthread_setname_np(pthread_t t, const char *name);
pid_t bionic_pthread_gettid_np(pthread_t t);

int bionic_pthread_mutexattr_init(long *a);
int bionic_pthread_mutexattr_destroy(long *a);
int bionic_pthread_mutexattr_settype(long *a, int type);
int bionic_pthread_mutexattr_gettype(const long *a, int *type);
int bionic_pthread_mutexattr_setpshared(long *a, int pshared);
int bionic_pthread_mutex_init(void *m, const long *attr);
int bionic_pthread_mutex_destroy(void *m);
int bionic_pthread_mutex_lock(void *m);
int bionic_pthread_mutex_trylock(void *m);
int bionic_pthread_mutex_unlock(void *m);
int bionic_pthread_mutex_timedlock(void *m, const struct timespec *abstime);
int bionic_pthread_mutex_lock_timeout_np(void *m, unsigned ms);

int bionic_pthread_condattr_init(bionic_pthread_condattr_t *a);
int bionic_pthread_condattr_destroy(bionic_pthread_condattr_t *a);
int bionic_pthread_condattr_setclock(bionic_pthread_condattr_t *a, clockid_t clock);
int bionic_pthread_condattr_getclock(const bionic_pthread_condattr_t *a, clockid_t *clock);
int bionic_pthread_condattr_setpshared(bionic_pthread_condattr_t *a, int pshared);
int bionic_pthread_cond_init(void *c, const bionic_pthread_condattr_t *attr);
int bionic_pthread_cond_destroy(void *c);
int bionic_pthread_cond_signal(void *c);
int bionic_pthread_cond_broadcast(void *c);
int bionic_pthread_cond_wait(void *c, void *m);
int bionic_pthread_cond_timedwait(void *c, void *m, const struct timespec *abstime);
int bionic_pthread_cond_clockwait(void *c, void *m, clockid_t clock, const struct timespec *abstime);
int bionic_pthread_cond_timedwait_monotonic_np(void *c, void *m, const struct timespec *abstime);
int bionic_pthread_cond_timedwait_relative_np(void *c, void *m, const struct timespec *rel);

int bionic_pthread_rwlockattr_init(long *a);
int bionic_pthread_rwlockattr_destroy(long *a);
int bionic_pthread_rwlock_init(void *rw, const long *attr);
int bionic_pthread_rwlock_destroy(void *rw);
int bionic_pthread_rwlock_rdlock(void *rw);
int bionic_pthread_rwlock_wrlock(void *rw);
int bionic_pthread_rwlock_tryrdlock(void *rw);
int bionic_pthread_rwlock_trywrlock(void *rw);
int bionic_pthread_rwlock_timedrdlock(void *rw, const struct timespec *abstime);
int bionic_pthread_rwlock_timedwrlock(void *rw, const struct timespec *abstime);
int bionic_pthread_rwlock_unlock(void *rw);

int bionic_sem_init(void *s, int pshared, unsigned value);
int bionic_sem_destroy(void *s);
int bionic_sem_post(void *s);
int bionic_sem_wait(void *s);
int bionic_sem_trywait(void *s);
int bionic_sem_timedwait(void *s, const struct timespec *abstime);
int bionic_sem_getvalue(void *s, int *value);
void *bionic_sem_open(const char *name, int oflag, ...);
int bionic_sem_close(void *s);
int bionic_sem_unlink(const char *name);

/* ---- signals (bionic_signal.c) --------------------------------------------- */

int bionic_sigaction(int sig, const struct bionic_sigaction *act, struct bionic_sigaction *old);
void *bionic_signal(int sig, void *handler);
int bionic_sigemptyset(bionic_sigset_t *set);
int bionic_sigfillset(bionic_sigset_t *set);
int bionic_sigaddset(bionic_sigset_t *set, int sig);
int bionic_sigdelset(bionic_sigset_t *set, int sig);
int bionic_sigismember(const bionic_sigset_t *set, int sig);
int bionic_sigprocmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old);
int bionic_pthread_sigmask(int how, const bionic_sigset_t *set, bionic_sigset_t *old);

/* ---- setjmp (bionic_setjmp.S): jmp_buf is long[32] in bionic ---------------- */

int bionic_setjmp(long *buf);
void bionic_longjmp(long *buf, int val) __attribute__((noreturn));

#endif
