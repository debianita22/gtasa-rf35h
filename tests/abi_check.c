/* abi_check.c -- compile-time proof that the layouts imports.c passes straight
 * through are identical between bionic arm64 and glibc aarch64. Build with the
 * aarch64 compiler; any mismatch is a compile error, not a runtime surprise.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <wchar.h>
#include <pthread.h>
#include <semaphore.h>
#include "../src/bionic_types.h"

#define SAME(gt, bt, f) static_assert(offsetof(gt, f) == offsetof(bt, f) && \
  sizeof(((gt *)0)->f) == sizeof(((bt *)0)->f), #gt "." #f)

/* struct stat: asm-generic layout on both */
static_assert(sizeof(struct stat) == sizeof(struct bionic_stat), "stat size");
SAME(struct stat, struct bionic_stat, st_dev);
SAME(struct stat, struct bionic_stat, st_ino);
SAME(struct stat, struct bionic_stat, st_mode);
SAME(struct stat, struct bionic_stat, st_nlink);
SAME(struct stat, struct bionic_stat, st_uid);
SAME(struct stat, struct bionic_stat, st_gid);
SAME(struct stat, struct bionic_stat, st_rdev);
SAME(struct stat, struct bionic_stat, st_size);
SAME(struct stat, struct bionic_stat, st_blksize);
SAME(struct stat, struct bionic_stat, st_blocks);
SAME(struct stat, struct bionic_stat, st_atim);
SAME(struct stat, struct bionic_stat, st_mtim);
SAME(struct stat, struct bionic_stat, st_ctim);

/* struct dirent */
static_assert(sizeof(struct dirent) == sizeof(struct bionic_dirent), "dirent size");
SAME(struct dirent, struct bionic_dirent, d_ino);
SAME(struct dirent, struct bionic_dirent, d_off);
SAME(struct dirent, struct bionic_dirent, d_reclen);
SAME(struct dirent, struct bionic_dirent, d_type);
SAME(struct dirent, struct bionic_dirent, d_name);

/* struct statvfs */
static_assert(sizeof(struct statvfs) == sizeof(struct bionic_statvfs), "statvfs size");
SAME(struct statvfs, struct bionic_statvfs, f_bsize);
SAME(struct statvfs, struct bionic_statvfs, f_frsize);
SAME(struct statvfs, struct bionic_statvfs, f_blocks);
SAME(struct statvfs, struct bionic_statvfs, f_bfree);
SAME(struct statvfs, struct bionic_statvfs, f_bavail);
SAME(struct statvfs, struct bionic_statvfs, f_files);
SAME(struct statvfs, struct bionic_statvfs, f_ffree);
SAME(struct statvfs, struct bionic_statvfs, f_favail);
SAME(struct statvfs, struct bionic_statvfs, f_fsid);
SAME(struct statvfs, struct bionic_statvfs, f_flag);
SAME(struct statvfs, struct bionic_statvfs, f_namemax);

/* struct tm: 9 ints, then tm_gmtoff and tm_zone on both */
static_assert(sizeof(struct tm) == 56, "tm size");
static_assert(offsetof(struct tm, tm_gmtoff) == 40 && offsetof(struct tm, tm_zone) == 48, "tm tail");

/* mbstate_t is opaque but must fit in bionic's 8 bytes */
static_assert(sizeof(mbstate_t) == 8, "mbstate_t");

/* things that are NOT the same and therefore must be wrapped */
static_assert(sizeof(pthread_mutex_t) != BIONIC_MUTEX_SIZE, "mutex differs: wrapped");
static_assert(sizeof(pthread_attr_t) != sizeof(bionic_pthread_attr_t), "attr differs: wrapped");
static_assert(sizeof(sigset_t) != sizeof(bionic_sigset_t), "sigset differs: wrapped");
static_assert(sizeof(sem_t) != BIONIC_SEM_SIZE, "sem differs: wrapped");
/* sizes we rely on when storing a pointer in the bionic object */
static_assert(sizeof(bionic_pthread_attr_t) == 56, "bionic attr size");
static_assert(sizeof(void *) == 8, "LP64");

int main(void) { return 0; }
