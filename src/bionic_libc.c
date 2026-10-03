/* bionic_libc.c -- bionic libc behaviours on top of glibc
 *
 * libGame.so and libc++_shared.so were linked against bionic. Where bionic and
 * glibc agree (checked in tests/abi_check.c) imports.c binds glibc directly;
 * this file covers the rest: the fake __sF FILEs, errno, the BSD ctype tables,
 * sysconf numbering, the _FORTIFY_SOURCE entry points, the C-only locale,
 * getaddrinfo's different struct and error codes, and a minimal dl* family.
 * Several wrappers follow gtasa_nx's libc_shim.c (MIT, see upstream/LICENSE).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include "bionic.h"
#include "casefold.h"
#include "import_table.h"
#include "loader.h"
#include "tls_guard.h"
#include "platform_util.h"
#include "util.h"
#include "hooks.h"

/* ==========================================================================
 * stdio: the game's stdin/stdout/stderr are &__sF[0..2], three bionic-sized
 * FILE objects inside our own array. Every FILE * wrapper maps those back to
 * real streams; everything else is a genuine glibc FILE * from bionic_fopen.
 * ======================================================================== */

uint8_t bionic_sF[3][BIONIC_FILE_SIZE] __attribute__((aligned(16)));
FILE *bionic_stdin = (FILE *)bionic_sF[0];
FILE *bionic_stdout = (FILE *)bionic_sF[1];
FILE *bionic_stderr = (FILE *)bionic_sF[2];

static FILE *game_out, *game_err;

void bionic_set_stdio(FILE *out, FILE *err) {
  game_out = out;
  game_err = err;
}

static inline FILE *R(FILE *f) {
  const uintptr_t p = (uintptr_t)f, b = (uintptr_t)bionic_sF;
  if (p >= b && p < b + sizeof(bionic_sF)) {
    switch ((p - b) / BIONIC_FILE_SIZE) {
      case 0: return stdin;
      case 1: return game_out ? game_out : stdout;
      default: return game_err ? game_err : stderr;
    }
  }
  return f;
}

/* a name the game spells differently from the disk (case_resolve) */
#define CASE_RETRY(call, path, failed)                                  \
  do {                                                                  \
    char spelled_[PATH_MAX];                                            \
    const char *on_disk_;                                               \
    if ((failed) && errno == ENOENT &&                                  \
        (on_disk_ = case_resolve(path, spelled_, sizeof(spelled_))))    \
      return call(on_disk_);                                            \
  } while (0)

FILE *bionic_fopen(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  char spelled[PATH_MAX]; /* path may point here from now on */
  if (!f && mode && mode[0] == 'r' && errno == ENOENT) {
    const char *on_disk = case_resolve(path, spelled, sizeof(spelled));
    if (on_disk) {
      f = fopen(on_disk, mode);
      path = on_disk;
    } else {
      errno = ENOENT;
    }
  }
  if (!f) {
    debugPrintf("fopen(%s, %s) failed: %s\n", path ? path : "(null)", mode, strerror(errno));
    return NULL;
  }
  note_data_open(path);
  /* The streaming archives are read in many small chunks: a bigger stdio
   * buffer turns them into fewer, larger reads from the SD card. */
  if (strchr(mode, 'r') && !strchr(mode, '+') && !strchr(mode, 'w')) {
    const char *ext = strrchr(path, '.');
    if (ext && (!strcasecmp(ext, ".img") || !strcasecmp(ext, ".dat") ||
                !strcasecmp(ext, ".ifp") || !strcasecmp(ext, ".mp3")))
      setvbuf(f, NULL, _IOFBF, 64 * 1024);
  }
  return f;
}

FILE *bionic_fdopen(int fd, const char *mode) { return fdopen(fd, mode); }
FILE *bionic_freopen(const char *path, const char *mode, FILE *f) { return freopen(path, mode, R(f)); }

int bionic_fclose(FILE *f) {
  if (R(f) != f)
    return 0; /* the game never owns our process's standard streams */
  return fclose(f);
}

int bionic_fflush(FILE *f) { return fflush(f ? R(f) : NULL); }
size_t bionic_fread(void *p, size_t s, size_t n, FILE *f) { return fread(p, s, n, R(f)); }
size_t bionic_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p, s, n, R(f)); }
int bionic_fgetc(FILE *f) { return fgetc(R(f)); }
char *bionic_fgets(char *s, int n, FILE *f) { return fgets(s, n, R(f)); }
int bionic_fputc(int c, FILE *f) { return fputc(c, R(f)); }
int bionic_fputs(const char *s, FILE *f) { return fputs(s, R(f)); }
int bionic_ungetc(int c, FILE *f) { return ungetc(c, R(f)); }
int bionic_vfprintf(FILE *f, const char *fmt, va_list va) { return vfprintf(R(f), fmt, va); }
int bionic_vfscanf(FILE *f, const char *fmt, va_list va) { return vfscanf(R(f), fmt, va); }
int bionic_fseek(FILE *f, long off, int whence) { return fseek(R(f), off, whence); }
int bionic_fseeko(FILE *f, off_t off, int whence) { return fseeko(R(f), off, whence); }
long bionic_ftell(FILE *f) { return ftell(R(f)); }
off_t bionic_ftello(FILE *f) { return ftello(R(f)); }
void bionic_rewind(FILE *f) { rewind(R(f)); }
int bionic_feof(FILE *f) { return feof(R(f)); }
int bionic_ferror(FILE *f) { return ferror(R(f)); }
void bionic_clearerr(FILE *f) { clearerr(R(f)); }
int bionic_fileno(FILE *f) { return fileno(R(f)); }
int bionic_setvbuf(FILE *f, char *buf, int mode, size_t size) { return setvbuf(R(f), buf, mode, size); }
void bionic_setbuf(FILE *f, char *buf) { setbuf(R(f), buf); }
wint_t bionic_fgetwc(FILE *f) { return fgetwc(R(f)); }
wint_t bionic_ungetwc(wint_t c, FILE *f) { return ungetwc(c, R(f)); }
wint_t bionic_fputwc(wchar_t c, FILE *f) { return fputwc(c, R(f)); }
int bionic_fwide(FILE *f, int mode) { return fwide(R(f), mode); }
ssize_t bionic_getline(char **l, size_t *n, FILE *f) { return getline(l, n, R(f)); }
ssize_t bionic_getdelim(char **l, size_t *n, int d, FILE *f) { return getdelim(l, n, d, R(f)); }
void bionic_flockfile(FILE *f) { flockfile(R(f)); }
void bionic_funlockfile(FILE *f) { funlockfile(R(f)); }

int bionic_fprintf(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  const int r = vfprintf(R(f), fmt, va);
  va_end(va);
  return r;
}

int bionic_fscanf(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  const int r = vfscanf(R(f), fmt, va);
  va_end(va);
  return r;
}

int bionic_vprintf(const char *fmt, va_list va) { return vfprintf(R(bionic_stdout), fmt, va); }
int bionic_puts(const char *s) { return fputs(s, R(bionic_stdout)) < 0 ? EOF : fputc('\n', R(bionic_stdout)); }
int bionic_putchar(int c) { return fputc(c, R(bionic_stdout)); }
int bionic_getchar(void) { return fgetc(stdin); }

int bionic_printf(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  const int r = vfprintf(R(bionic_stdout), fmt, va);
  va_end(va);
  return r;
}

/* bionic's fpos_t is a plain off_t; glibc's is a struct */
int bionic_fgetpos(FILE *f, int64_t *pos) {
  const off_t o = ftello(R(f));
  if (o < 0)
    return -1;
  *pos = o;
  return 0;
}

int bionic_fsetpos(FILE *f, const int64_t *pos) {
  return fseeko(R(f), (off_t)*pos, SEEK_SET);
}

/* ==========================================================================
 * errno, ctype, stack protector, process exit
 * ======================================================================== */

int *bionic___errno(void) { return &errno; }

/* BSD ctype flags as used by bionic's inline ctype macros */
#define C_U 0x01
#define C_L 0x02
#define C_N 0x04
#define C_S 0x08
#define C_P 0x10
#define C_C 0x20
#define C_X 0x40
#define C_B 0x80

static char ctype_table[1 + 256];
static short tolower_table[1 + 256];
static short toupper_table[1 + 256];
const char *bionic_ctype_ = ctype_table;
const short *bionic_tolower_tab_ = tolower_table;
const short *bionic_toupper_tab_ = toupper_table;

/* The C-locale table of OpenBSD's ctype_.c, which bionic uses, computed
 * rather than typed out: entry 0 is EOF, bytes >= 0x80 have no class. */
__attribute__((constructor)) static void build_ctype_tables(void) {
  ctype_table[0] = 0;
  tolower_table[0] = toupper_table[0] = -1;
  for (int c = 0; c < 256; c++) {
    int f = 0;
    if (c < 0x80) {
      if (c < 0x20 || c == 0x7f) f |= C_C;
      if (c >= 0x09 && c <= 0x0d) f |= C_S;
      if (c == ' ') f |= C_S | C_B;
      if (c >= '0' && c <= '9') f |= C_N;
      if (c >= 'A' && c <= 'Z') f |= C_U;
      if (c >= 'a' && c <= 'z') f |= C_L;
      if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= C_X;
      if (c > 0x20 && c < 0x7f && !(f & (C_N | C_U | C_L))) f |= C_P;
    }
    ctype_table[1 + c] = (char)f;
    tolower_table[1 + c] = (short)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    toupper_table[1 + c] = (short)((c >= 'a' && c <= 'z') ? c - 32 : c);
  }
}

uint64_t bionic_stack_chk_guard = BIONIC_STACK_GUARD;

void bionic___stack_chk_fail(void) {
  const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  const so_module *m = so_module_at(caller);
  if (m)
    debugPrintf("FATAL: stack corruption detected in %s+%#lx\n", m->name,
                (unsigned long)(caller - (uintptr_t)m->load_base));
  else
    debugPrintf("FATAL: stack corruption detected at %p\n", (void *)caller);
  abort();
}

void bionic_abort(void) {
  const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  const so_module *m = so_module_at(caller);
  if (m)
    debugPrintf("FATAL: abort() called from %s+%#lx\n", m->name,
                (unsigned long)(caller - (uintptr_t)m->load_base));
  abort();
}

/* The engine's static destructors crash when run at exit (see gtasa_nx's
 * hard_exit); leave the way the pause menu does. */
void bionic_exit(int code) {
  debugPrintf("game called exit(%d)\n", code);
  hard_exit();
  __builtin_unreachable();
}

/* ==========================================================================
 * sysconf, syscall, misc
 * ======================================================================== */

static unsigned reported_ram_mb;

void bionic_set_reported_ram_mb(unsigned mb) { reported_ram_mb = mb; }

long bionic_sysconf(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return sysconf(_SC_PAGESIZE);
    case BIONIC_SC_NPROCESSORS_CONF:
      return sysconf(_SC_NPROCESSORS_CONF);
    case BIONIC_SC_NPROCESSORS_ONLN:
      return sysconf(_SC_NPROCESSORS_ONLN);
    case BIONIC_SC_PHYS_PAGES:
      if (reported_ram_mb)
        return (long)reported_ram_mb * (1024 * 1024 / sysconf(_SC_PAGESIZE));
      return sysconf(_SC_PHYS_PAGES);
    case BIONIC_SC_AVPHYS_PAGES:
      return sysconf(_SC_AVPHYS_PAGES);
    default: {
      static uint64_t warned[4]; /* log each unknown name once (racy, log only) */
      if (name >= 0 && name < 256 && !(warned[name / 64] & (1ull << (name % 64)))) {
        warned[name / 64] |= 1ull << (name % 64);
        debugPrintf("sysconf: bionic name %#x is not mapped\n", name);
      }
      errno = EINVAL;
      return -1;
    }
  }
}

/* bionic numbers _PC_* differently; the table is indexed by bionic's value */
long bionic_pathconf(const char *path, int name) {
  static const int to_glibc[] = {
    [BIONIC_PC_FILESIZEBITS] = _PC_FILESIZEBITS,
    [BIONIC_PC_LINK_MAX] = _PC_LINK_MAX,
    [BIONIC_PC_MAX_CANON] = _PC_MAX_CANON,
    [BIONIC_PC_MAX_INPUT] = _PC_MAX_INPUT,
    [BIONIC_PC_NAME_MAX] = _PC_NAME_MAX,
    [BIONIC_PC_PATH_MAX] = _PC_PATH_MAX,
    [BIONIC_PC_PIPE_BUF] = _PC_PIPE_BUF,
    [BIONIC_PC_2_SYMLINKS] = _PC_2_SYMLINKS,
    [BIONIC_PC_ALLOC_SIZE_MIN] = _PC_ALLOC_SIZE_MIN,
    [BIONIC_PC_REC_INCR_XFER_SIZE] = _PC_REC_INCR_XFER_SIZE,
    [BIONIC_PC_REC_MAX_XFER_SIZE] = _PC_REC_MAX_XFER_SIZE,
    [BIONIC_PC_REC_MIN_XFER_SIZE] = _PC_REC_MIN_XFER_SIZE,
    [BIONIC_PC_REC_XFER_ALIGN] = _PC_REC_XFER_ALIGN,
    [BIONIC_PC_SYMLINK_MAX] = _PC_SYMLINK_MAX,
    [BIONIC_PC_CHOWN_RESTRICTED] = _PC_CHOWN_RESTRICTED,
    [BIONIC_PC_NO_TRUNC] = _PC_NO_TRUNC,
    [BIONIC_PC_VDISABLE] = _PC_VDISABLE,
    [BIONIC_PC_ASYNC_IO] = _PC_ASYNC_IO,
    [BIONIC_PC_PRIO_IO] = _PC_PRIO_IO,
    [BIONIC_PC_SYNC_IO] = _PC_SYNC_IO,
  };
  if (name < 0 || name >= (int)(sizeof(to_glibc) / sizeof(to_glibc[0]))) {
    errno = EINVAL;
    return -1;
  }
  return pathconf(path, to_glibc[name]);
}

/* Linux syscall numbers are the same for bionic and glibc on arm64. */
long bionic_syscall(long number, ...) {
  va_list va;
  va_start(va, number);
  long a[6];
  for (int i = 0; i < 6; i++)
    a[i] = va_arg(va, long);
  va_end(va);
  return syscall(number, a[0], a[1], a[2], a[3], a[4], a[5]);
}

int bionic_gettid(void) { return (int)syscall(SYS_gettid); }

extern int __xpg_strerror_r(int err, char *buf, size_t len);
int bionic_strerror_r(int err, char *buf, size_t len) { return __xpg_strerror_r(err, buf, len); }

size_t bionic___ctype_get_mb_cur_max(void) { return 1; }

int bionic___register_atfork(void (*prepare)(void), void (*parent)(void),
                             void (*child)(void), void *dso) {
  (void)prepare; (void)parent; (void)child; (void)dso;
  return 0; /* the game never forks */
}

int bionic___cxa_finalize(void *dso) {
  (void)dso;
  return 0;
}

/* ==========================================================================
 * _FORTIFY_SOURCE entry points: the object-size checks are dropped, as in
 * gtasa_nx; the game was built against these sizes and never trips them.
 * ======================================================================== */

void *bionic___memcpy_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memcpy(d, s, n); }
void *bionic___memmove_chk(void *d, const void *s, size_t n, size_t dl) { (void)dl; return memmove(d, s, n); }
void *bionic___memset_chk(void *s, int c, size_t n, size_t dl) { (void)dl; return memset(s, c, n); }
char *bionic___strcat_chk(char *d, const char *s, size_t dl) { (void)dl; return strcat(d, s); }
char *bionic___strchr_chk(const char *s, int c, size_t sl) { (void)sl; return strchr(s, c); }
char *bionic___strrchr_chk(const char *s, int c, size_t sl) { (void)sl; return strrchr(s, c); }
char *bionic___strcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return strcpy(d, s); }
char *bionic___stpcpy_chk(char *d, const char *s, size_t dl) { (void)dl; return stpcpy(d, s); }
size_t bionic___strlen_chk(const char *s, size_t sl) { (void)sl; return strlen(s); }
char *bionic___strncat_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncat(d, s, n); }
char *bionic___strncpy_chk(char *d, const char *s, size_t n, size_t dl) { (void)dl; return strncpy(d, s, n); }
char *bionic___strncpy_chk2(char *d, const char *s, size_t n, size_t dl, size_t sl) {
  (void)dl; (void)sl;
  return strncpy(d, s, n);
}

int bionic___vsnprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int bionic___snprintf_chk(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va;
  va_start(va, fmt);
  const int r = vsnprintf(s, maxlen, fmt, va);
  va_end(va);
  return r;
}

int bionic___vsprintf_chk(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

int bionic___sprintf_chk(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va;
  va_start(va, fmt);
  const int r = vsprintf(s, fmt, va);
  va_end(va);
  return r;
}

ssize_t bionic___read_chk(int fd, void *buf, size_t count, size_t bs) { (void)bs; return read(fd, buf, count); }
ssize_t bionic___write_chk(int fd, const void *buf, size_t count, size_t bs) { (void)bs; return write(fd, buf, count); }
size_t bionic___fread_chk(void *p, size_t s, size_t n, FILE *f, size_t bs) { (void)bs; return fread(p, s, n, R(f)); }
char *bionic___fgets_chk(char *s, int size, FILE *f, size_t bos) { (void)bos; return fgets(s, size, R(f)); }
void bionic___FD_SET_chk(int fd, void *set, size_t ss) { (void)ss; FD_SET(fd, (fd_set *)set); }
void bionic___FD_CLR_chk(int fd, void *set, size_t ss) { (void)ss; FD_CLR(fd, (fd_set *)set); }
int bionic___FD_ISSET_chk(int fd, const void *set, size_t ss) { (void)ss; return FD_ISSET(fd, (const fd_set *)set); }
/* glibc's __OPEN_NEEDS_MODE: only then is there a mode argument */
#define NEEDS_MODE(flags) (((flags) & O_CREAT) || ((flags) & O_TMPFILE) == O_TMPFILE)

static int open_retry(int dirfd, const char *path, int flags, mode_t mode) {
  const int fd = openat(dirfd, path, flags, mode);
#define OPENAT_AGAIN(p) openat(dirfd, p, flags, mode)
  if (dirfd == AT_FDCWD && !(flags & O_CREAT))
    CASE_RETRY(OPENAT_AGAIN, path, fd < 0);
  return fd;
}

int bionic_open(const char *path, int flags, ...) {
  mode_t mode = 0;
  if (NEEDS_MODE(flags)) {
    va_list va;
    va_start(va, flags);
    mode = (mode_t)va_arg(va, int);
    va_end(va);
  }
  return open_retry(AT_FDCWD, path, flags, mode);
}

int bionic_openat(int dirfd, const char *path, int flags, ...) {
  mode_t mode = 0;
  if (NEEDS_MODE(flags)) {
    va_list va;
    va_start(va, flags);
    mode = (mode_t)va_arg(va, int);
    va_end(va);
  }
  return open_retry(dirfd, path, flags, mode);
}

/* the fortified forms, used when there is no mode (bionic aborts if the
 * flags would need one; here it is 0) */
int bionic___open_2(const char *path, int flags) { return open_retry(AT_FDCWD, path, flags, 0); }
int bionic___openat_2(int dirfd, const char *path, int flags) { return open_retry(dirfd, path, flags, 0); }

/* struct stat is the same on bionic and glibc for arm64 (tests/abi) */
int bionic_stat(const char *path, struct stat *st) {
  const int rc = stat(path, st);
#define STAT_AGAIN(p) stat(p, st)
  CASE_RETRY(STAT_AGAIN, path, rc < 0);
  return rc;
}

int bionic_lstat(const char *path, struct stat *st) {
  const int rc = lstat(path, st);
#define LSTAT_AGAIN(p) lstat(p, st)
  CASE_RETRY(LSTAT_AGAIN, path, rc < 0);
  return rc;
}

DIR *bionic_opendir(const char *path) {
  DIR *d = opendir(path);
  CASE_RETRY(opendir, path, !d);
  return d;
}

/* ==========================================================================
 * locale: bionic ships only C/C.UTF-8 and libc++ just round-trips the handle,
 * so a token stands in for locale_t and the _l variants ignore it (gtasa_nx
 * does the same). Classification uses the bionic C-locale table above.
 * ======================================================================== */

#define FAKE_LOCALE ((void *)(uintptr_t)1)

void *bionic_newlocale(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return FAKE_LOCALE;
}
void bionic_freelocale(void *loc) { (void)loc; }
void *bionic_uselocale(void *loc) { (void)loc; return FAKE_LOCALE; }

static inline int ct(int c, int mask) {
  return (c >= -1 && c < 256) ? (ctype_table[1 + c] & mask) : 0;
}

int bionic_isalpha_l(int c, void *l) { (void)l; return ct(c, C_U | C_L); }
int bionic_isdigit_l(int c, void *l) { (void)l; return ct(c, C_N); }
int bionic_isxdigit_l(int c, void *l) { (void)l; return ct(c, C_N | C_X); }
int bionic_islower_l(int c, void *l) { (void)l; return ct(c, C_L); }
int bionic_isupper_l(int c, void *l) { (void)l; return ct(c, C_U); }
int bionic_isspace_l(int c, void *l) { (void)l; return ct(c, C_S); }
int bionic_isprint_l(int c, void *l) { (void)l; return ct(c, C_P | C_U | C_L | C_N | C_B); }
int bionic_ispunct_l(int c, void *l) { (void)l; return ct(c, C_P); }
int bionic_iscntrl_l(int c, void *l) { (void)l; return ct(c, C_C); }
int bionic_isalnum_l(int c, void *l) { (void)l; return ct(c, C_U | C_L | C_N); }
int bionic_isblank_l(int c, void *l) { (void)l; return c == ' ' || c == '\t'; }
int bionic_isgraph_l(int c, void *l) { (void)l; return ct(c, C_P | C_U | C_L | C_N); }
int bionic_toupper_l(int c, void *l) { (void)l; return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int bionic_tolower_l(int c, void *l) { (void)l; return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

#define WIDE_L(fn) int bionic_##fn##_l(wint_t c, void *l) { (void)l; return fn(c); }
WIDE_L(iswalpha)
WIDE_L(iswblank)
WIDE_L(iswcntrl)
WIDE_L(iswdigit)
WIDE_L(iswlower)
WIDE_L(iswprint)
WIDE_L(iswpunct)
WIDE_L(iswspace)
WIDE_L(iswupper)
WIDE_L(iswxdigit)
wint_t bionic_towlower_l(wint_t c, void *l) { (void)l; return towlower(c); }
wint_t bionic_towupper_l(wint_t c, void *l) { (void)l; return towupper(c); }

int bionic_strcoll_l(const char *a, const char *b, void *l) { (void)l; return strcmp(a, b); }
size_t bionic_strxfrm_l(char *d, const char *s, size_t n, void *l) { (void)l; return strxfrm(d, s, n); }
size_t bionic_strftime_l(char *s, size_t max, const char *fmt, const struct tm *tm, void *l) {
  (void)l;
  return strftime(s, max, fmt, tm);
}
long double bionic_strtold_l(const char *s, char **e, void *l) { (void)l; return strtold(s, e); }
long long bionic_strtoll_l(const char *s, char **e, int b, void *l) { (void)l; return strtoll(s, e, b); }
unsigned long long bionic_strtoull_l(const char *s, char **e, int b, void *l) { (void)l; return strtoull(s, e, b); }
float bionic_strtof_l(const char *s, char **e, void *l) { (void)l; return strtof(s, e); }
double bionic_strtod_l(const char *s, char **e, void *l) { (void)l; return strtod(s, e); }
int bionic_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { (void)l; return wcscmp(a, b); }
size_t bionic_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { (void)l; return wcsxfrm(d, s, n); }

/* Byte-per-character conversions, as gtasa_nx ships them, with the POSIX
 * contract for *src: NULL after a converted terminator, else just past the
 * last converted unit (only when dst is non-NULL). */
size_t bionic_mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  const char *s = *src;
  size_t i = 0, n = 0;
  while (i < nms && (!dst || n < len)) {
    const unsigned char c = (unsigned char)s[i];
    if (dst)
      dst[n] = c;
    if (c == 0) {
      if (dst)
        *src = NULL;
      return n;
    }
    i++;
    n++;
  }
  if (dst)
    *src = s + i;
  return n;
}

size_t bionic_wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  const wchar_t *s = *src;
  size_t i = 0, n = 0;
  while (i < nwc && (!dst || n < len)) {
    const wchar_t c = s[i];
    if ((uint32_t)c > 0xff) {
      errno = EILSEQ;
      if (dst)
        *src = s + i;
      return (size_t)-1;
    }
    if (dst)
      dst[n] = (char)c;
    if (c == 0) {
      if (dst)
        *src = NULL;
      return n;
    }
    i++;
    n++;
  }
  if (dst)
    *src = s + i;
  return n;
}

/* ==========================================================================
 * network: off by default, so the Social Club probes fail at once instead of
 * waiting on DNS. getaddrinfo also needs translating when on: bionic swaps
 * ai_addr and ai_canonname, numbers the AI_* flags the BSD way and uses
 * positive EAI_* codes.
 * ======================================================================== */

static int network_enabled;
void bionic_set_network(int enabled) { network_enabled = enabled; }

static int eai_to_bionic(int e) {
  switch (e) {
    case 0:              return 0;
    case EAI_ADDRFAMILY: return BIONIC_EAI_ADDRFAMILY;
    case EAI_AGAIN:      return BIONIC_EAI_AGAIN;
    case EAI_BADFLAGS:   return BIONIC_EAI_BADFLAGS;
    case EAI_FAIL:       return BIONIC_EAI_FAIL;
    case EAI_FAMILY:     return BIONIC_EAI_FAMILY;
    case EAI_MEMORY:     return BIONIC_EAI_MEMORY;
    case EAI_NODATA:     return BIONIC_EAI_NODATA;
    case EAI_NONAME:     return BIONIC_EAI_NONAME;
    case EAI_SERVICE:    return BIONIC_EAI_SERVICE;
    case EAI_SOCKTYPE:   return BIONIC_EAI_SOCKTYPE;
    case EAI_SYSTEM:     return BIONIC_EAI_SYSTEM;
    case EAI_OVERFLOW:   return BIONIC_EAI_OVERFLOW;
    default:             return BIONIC_EAI_FAIL;
  }
}

static const struct { int bionic, glibc; } ai_flags_map[] = {
  { BIONIC_AI_PASSIVE, AI_PASSIVE },         { BIONIC_AI_CANONNAME, AI_CANONNAME },
  { BIONIC_AI_NUMERICHOST, AI_NUMERICHOST }, { BIONIC_AI_NUMERICSERV, AI_NUMERICSERV },
  { BIONIC_AI_ALL, AI_ALL },                 { BIONIC_AI_ADDRCONFIG, AI_ADDRCONFIG },
  { BIONIC_AI_V4MAPPED, AI_V4MAPPED },
};

static int ai_flags_convert(int flags, int to_glibc) {
  int out = 0;
  for (size_t i = 0; i < sizeof(ai_flags_map) / sizeof(ai_flags_map[0]); i++)
    if (flags & (to_glibc ? ai_flags_map[i].bionic : ai_flags_map[i].glibc))
      out |= to_glibc ? ai_flags_map[i].glibc : ai_flags_map[i].bionic;
  return out;
}

int bionic_socket(int domain, int type, int protocol) {
  if (!network_enabled) {
    errno = EACCES;
    return -1;
  }
  return socket(domain, type, protocol);
}

int bionic_getaddrinfo(const char *node, const char *service, const void *hints_b, void **res) {
  *res = NULL;
  if (!network_enabled)
    return BIONIC_EAI_FAIL;
  struct addrinfo hints, *gres = NULL;
  memset(&hints, 0, sizeof(hints));
  if (hints_b) {
    const struct bionic_addrinfo *h = hints_b;
    if (h->ai_flags & ~BIONIC_AI_MASK)
      return BIONIC_EAI_BADFLAGS; /* as bionic itself answers */
    hints.ai_flags = ai_flags_convert(h->ai_flags, 1);
    hints.ai_family = h->ai_family;
    hints.ai_socktype = h->ai_socktype;
    hints.ai_protocol = h->ai_protocol;
  }
  const int rc = getaddrinfo(node, service, hints_b ? &hints : NULL, &gres);
  if (rc != 0)
    return eai_to_bionic(rc);
  struct bionic_addrinfo *head = NULL, **tail = &head;
  for (const struct addrinfo *a = gres; a; a = a->ai_next) {
    /* one allocation per entry: struct, address, name; freeaddrinfo = free */
    const size_t name_len = a->ai_canonname ? strlen(a->ai_canonname) + 1 : 0;
    struct bionic_addrinfo *b = calloc(1, sizeof(*b) + a->ai_addrlen + name_len);
    if (!b)
      break;
    b->ai_flags = ai_flags_convert(a->ai_flags, 0);
    b->ai_family = a->ai_family;
    b->ai_socktype = a->ai_socktype;
    b->ai_protocol = a->ai_protocol;
    b->ai_addrlen = a->ai_addrlen;
    b->ai_addr = b + 1;
    memcpy(b->ai_addr, a->ai_addr, a->ai_addrlen);
    if (name_len) {
      b->ai_canonname = (char *)b->ai_addr + a->ai_addrlen;
      memcpy(b->ai_canonname, a->ai_canonname, name_len);
    }
    *tail = b;
    tail = &b->ai_next;
  }
  freeaddrinfo(gres);
  *res = head;
  return head ? 0 : BIONIC_EAI_MEMORY;
}

void bionic_freeaddrinfo(void *res) {
  struct bionic_addrinfo *a = res;
  while (a) {
    struct bionic_addrinfo *next = a->ai_next;
    free(a);
    a = next;
  }
}

const char *bionic_gai_strerror(int code) {
  return code == 0 ? "Success" : "Name resolution failed";
}

void *bionic_gethostbyname(const char *name) {
  if (!network_enabled) {
    h_errno = HOST_NOT_FOUND;
    return NULL;
  }
  return gethostbyname(name);
}

int bionic_gethostname(char *name, size_t len) {
  snprintf(name, len, "%s", "rf35h");
  return 0;
}

/* strlcpy/strlcat are bionic staples; glibc only has them from 2.38 */
size_t bionic_strlcpy(char *dst, const char *src, size_t size) {
  const size_t len = strlen(src);
  if (size) {
    const size_t n = len < size - 1 ? len : size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
  }
  return len;
}

size_t bionic_strlcat(char *dst, const char *src, size_t size) {
  const size_t dlen = strnlen(dst, size);
  if (dlen == size)
    return size + strlen(src);
  return dlen + bionic_strlcpy(dst + dlen, src, size - dlen);
}

/* bionic's dirname/basename never modify their argument */
char *bionic_dirname(const char *path) {
  static __thread char buf[1024];
  if (!path || !*path || !strchr(path, '/'))
    return strcpy(buf, ".");
  snprintf(buf, sizeof(buf), "%s", path);
  size_t n = strlen(buf);
  while (n > 1 && buf[n - 1] == '/')
    buf[--n] = 0;
  char *slash = strrchr(buf, '/');
  if (!slash)
    return strcpy(buf, ".");
  while (slash > buf && *slash == '/')
    slash--;
  slash[1] = 0;
  if (!buf[0])
    strcpy(buf, "/");
  return buf;
}

char *bionic_basename(const char *path) {
  static __thread char buf[1024];
  if (!path || !*path)
    return strcpy(buf, ".");
  snprintf(buf, sizeof(buf), "%s", path);
  size_t n = strlen(buf);
  while (n > 1 && buf[n - 1] == '/')
    buf[--n] = 0;
  const char *slash = strrchr(buf, '/');
  if (slash && slash[1])
    memmove(buf, slash + 1, strlen(slash + 1) + 1);
  return buf;
}

/* ==========================================================================
 * dl*: the game links everything at load time; these only answer the odd
 * runtime probe through the same resolution the loader uses.
 * ======================================================================== */

static char dl_error[256];
static int dl_pseudo_handle;

void *bionic_dlopen(const char *name, int flags) {
  (void)flags;
  debugPrintf("dlopen(%s) -> pseudo handle\n", name ? name : "(null)");
  return &dl_pseudo_handle;
}

void *bionic_dlsym(void *handle, const char *name) {
  (void)handle;
  uintptr_t addr = imports_resolve(name);
  if (!addr) {
    snprintf(dl_error, sizeof(dl_error), "undefined symbol: %s", name);
    debugPrintf("dlsym(%s) -> not found\n", name);
  }
  return (void *)addr;
}

int bionic_dlclose(void *handle) {
  (void)handle;
  return 0;
}

char *bionic_dlerror(void) {
  if (!dl_error[0])
    return NULL;
  static char out[sizeof(dl_error)];
  memcpy(out, dl_error, sizeof(out));
  dl_error[0] = 0;
  return out;
}

int bionic_dladdr(const void *addr, void *info_out) {
  Dl_info *info = info_out; /* same layout in bionic and glibc */
  const so_module *m = so_module_at((uintptr_t)addr);
  if (!m)
    return dladdr(addr, info);
  uintptr_t off = 0;
  info->dli_fname = m->name;
  info->dli_fbase = m->load_base;
  info->dli_sname = so_symbol_at(m, (uintptr_t)addr, &off);
  info->dli_saddr = info->dli_sname ? (void *)((uintptr_t)addr - off) : NULL;
  return 1;
}
