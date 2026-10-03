// bionic_types_check.cpp -- src/bionic_types.h against the real bionic headers.
//
// Compile-only (clang, aarch64-linux-android target, bionic's own headers);
// any definition in bionic_types.h that disagrees with bionic is a compile
// error. C++ so that static initialisers can be inspected with constexpr.
// tests/abi/check_abi.sh runs it.

#include <dirent.h>
#include <netdb.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../../src/bionic_types.h"

#ifndef __BIONIC__
#error "compile against the bionic headers"
#endif
static_assert(sizeof(void *) == 8, "LP64 only");

#define SAME(bt, bf, ot, of) \
  static_assert(offsetof(bt, bf) == offsetof(ot, of) && \
                sizeof(((bt *)0)->bf) == sizeof(((ot *)0)->of), #bt "." #bf)

// ---- struct stat / dirent / statvfs ----
static_assert(sizeof(struct stat) == sizeof(struct bionic_stat), "stat size");
SAME(struct stat, st_dev, struct bionic_stat, st_dev);
SAME(struct stat, st_ino, struct bionic_stat, st_ino);
SAME(struct stat, st_mode, struct bionic_stat, st_mode);
SAME(struct stat, st_nlink, struct bionic_stat, st_nlink);
SAME(struct stat, st_uid, struct bionic_stat, st_uid);
SAME(struct stat, st_gid, struct bionic_stat, st_gid);
SAME(struct stat, st_rdev, struct bionic_stat, st_rdev);
SAME(struct stat, st_size, struct bionic_stat, st_size);
SAME(struct stat, st_blksize, struct bionic_stat, st_blksize);
SAME(struct stat, st_blocks, struct bionic_stat, st_blocks);
SAME(struct stat, st_atim, struct bionic_stat, st_atim);
SAME(struct stat, st_mtim, struct bionic_stat, st_mtim);
SAME(struct stat, st_ctim, struct bionic_stat, st_ctim);

static_assert(sizeof(struct dirent) == sizeof(struct bionic_dirent), "dirent size");
SAME(struct dirent, d_ino, struct bionic_dirent, d_ino);
SAME(struct dirent, d_off, struct bionic_dirent, d_off);
SAME(struct dirent, d_reclen, struct bionic_dirent, d_reclen);
SAME(struct dirent, d_type, struct bionic_dirent, d_type);
SAME(struct dirent, d_name, struct bionic_dirent, d_name);

static_assert(sizeof(struct statvfs) == sizeof(struct bionic_statvfs), "statvfs size");
SAME(struct statvfs, f_bsize, struct bionic_statvfs, f_bsize);
SAME(struct statvfs, f_frsize, struct bionic_statvfs, f_frsize);
SAME(struct statvfs, f_blocks, struct bionic_statvfs, f_blocks);
SAME(struct statvfs, f_bfree, struct bionic_statvfs, f_bfree);
SAME(struct statvfs, f_bavail, struct bionic_statvfs, f_bavail);
SAME(struct statvfs, f_files, struct bionic_statvfs, f_files);
SAME(struct statvfs, f_ffree, struct bionic_statvfs, f_ffree);
SAME(struct statvfs, f_favail, struct bionic_statvfs, f_favail);
SAME(struct statvfs, f_fsid, struct bionic_statvfs, f_fsid);
SAME(struct statvfs, f_flag, struct bionic_statvfs, f_flag);
SAME(struct statvfs, f_namemax, struct bionic_statvfs, f_namemax);

// ---- pthread ----
static_assert(sizeof(pthread_mutex_t) == BIONIC_MUTEX_SIZE, "mutex size");
static_assert(sizeof(pthread_cond_t) == BIONIC_COND_SIZE, "cond size");
static_assert(sizeof(pthread_rwlock_t) == BIONIC_RWLOCK_SIZE, "rwlock size");
static_assert(sizeof(sem_t) == BIONIC_SEM_SIZE, "sem size");
static_assert(sizeof(pthread_condattr_t) == sizeof(bionic_pthread_condattr_t), "condattr");
static_assert(sizeof(pthread_mutexattr_t) == sizeof(long), "mutexattr is a long");
static_assert(sizeof(pthread_rwlockattr_t) == sizeof(long), "rwlockattr is a long");

// the static initialisers, as the game's .data holds them
constexpr pthread_mutex_t m_normal = PTHREAD_MUTEX_INITIALIZER;
constexpr pthread_mutex_t m_recursive = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
constexpr pthread_mutex_t m_errorcheck = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
static_assert((uint32_t)m_normal.__private[0] == BIONIC_MUTEX_INIT_NORMAL, "mutex init");
static_assert((uint32_t)m_recursive.__private[0] == BIONIC_MUTEX_INIT_RECURSIVE, "recursive init");
static_assert((uint32_t)m_errorcheck.__private[0] == BIONIC_MUTEX_INIT_ERRORCHECK, "errorcheck init");
constexpr pthread_cond_t c_init = PTHREAD_COND_INITIALIZER;
static_assert(c_init.__private[0] == 0, "cond init is all zero");
constexpr pthread_rwlock_t rw_init = PTHREAD_RWLOCK_INITIALIZER;
static_assert(rw_init.__private[0] == 0, "rwlock init is all zero");

static_assert(sizeof(pthread_attr_t) == sizeof(bionic_pthread_attr_t), "attr size");
SAME(pthread_attr_t, flags, bionic_pthread_attr_t, flags);
SAME(pthread_attr_t, stack_base, bionic_pthread_attr_t, stack_base);
SAME(pthread_attr_t, stack_size, bionic_pthread_attr_t, stack_size);
SAME(pthread_attr_t, guard_size, bionic_pthread_attr_t, guard_size);
SAME(pthread_attr_t, sched_policy, bionic_pthread_attr_t, sched_policy);
SAME(pthread_attr_t, sched_priority, bionic_pthread_attr_t, sched_priority);
static_assert(PTHREAD_CREATE_JOINABLE == BIONIC_PTHREAD_CREATE_JOINABLE, "joinable");
static_assert(PTHREAD_CREATE_DETACHED == BIONIC_PTHREAD_CREATE_DETACHED, "detached");
// libc/bionic/pthread_internal.h: #define PTHREAD_ATTR_FLAG_DETACHED 0x00000001
static_assert(BIONIC_PTHREAD_ATTR_FLAG_DETACHED == 0x00000001, "attr flag");

// ---- signals ----
static_assert(sizeof(sigset_t) == sizeof(bionic_sigset_t), "sigset size");
static_assert(sizeof(struct sigaction) == sizeof(struct bionic_sigaction), "sigaction size");
SAME(struct sigaction, sa_flags, struct bionic_sigaction, flags);
SAME(struct sigaction, sa_handler, struct bionic_sigaction, handler);
SAME(struct sigaction, sa_sigaction, struct bionic_sigaction, handler);
SAME(struct sigaction, sa_mask, struct bionic_sigaction, mask);
SAME(struct sigaction, sa_restorer, struct bionic_sigaction, restorer);
static_assert(SA_RESTORER == BIONIC_SA_RESTORER, "SA_RESTORER");

// ---- setjmp ----
static_assert(sizeof(jmp_buf) == BIONIC_JMPBUF_WORDS * sizeof(long), "jmp_buf");

// ---- sysconf / pathconf ----
static_assert(_SC_PAGESIZE == BIONIC_SC_PAGESIZE, "_SC_PAGESIZE");
static_assert(_SC_PAGE_SIZE == BIONIC_SC_PAGE_SIZE, "_SC_PAGE_SIZE");
static_assert(_SC_NPROCESSORS_CONF == BIONIC_SC_NPROCESSORS_CONF, "_SC_NPROCESSORS_CONF");
static_assert(_SC_NPROCESSORS_ONLN == BIONIC_SC_NPROCESSORS_ONLN, "_SC_NPROCESSORS_ONLN");
static_assert(_SC_PHYS_PAGES == BIONIC_SC_PHYS_PAGES, "_SC_PHYS_PAGES");
static_assert(_SC_AVPHYS_PAGES == BIONIC_SC_AVPHYS_PAGES, "_SC_AVPHYS_PAGES");
static_assert(_PC_FILESIZEBITS == BIONIC_PC_FILESIZEBITS, "_PC_FILESIZEBITS");
static_assert(_PC_LINK_MAX == BIONIC_PC_LINK_MAX, "_PC_LINK_MAX");
static_assert(_PC_MAX_CANON == BIONIC_PC_MAX_CANON, "_PC_MAX_CANON");
static_assert(_PC_MAX_INPUT == BIONIC_PC_MAX_INPUT, "_PC_MAX_INPUT");
static_assert(_PC_NAME_MAX == BIONIC_PC_NAME_MAX, "_PC_NAME_MAX");
static_assert(_PC_PATH_MAX == BIONIC_PC_PATH_MAX, "_PC_PATH_MAX");
static_assert(_PC_PIPE_BUF == BIONIC_PC_PIPE_BUF, "_PC_PIPE_BUF");
static_assert(_PC_2_SYMLINKS == BIONIC_PC_2_SYMLINKS, "_PC_2_SYMLINKS");
static_assert(_PC_ALLOC_SIZE_MIN == BIONIC_PC_ALLOC_SIZE_MIN, "_PC_ALLOC_SIZE_MIN");
static_assert(_PC_REC_INCR_XFER_SIZE == BIONIC_PC_REC_INCR_XFER_SIZE, "_PC_REC_INCR_XFER_SIZE");
static_assert(_PC_REC_MAX_XFER_SIZE == BIONIC_PC_REC_MAX_XFER_SIZE, "_PC_REC_MAX_XFER_SIZE");
static_assert(_PC_REC_MIN_XFER_SIZE == BIONIC_PC_REC_MIN_XFER_SIZE, "_PC_REC_MIN_XFER_SIZE");
static_assert(_PC_REC_XFER_ALIGN == BIONIC_PC_REC_XFER_ALIGN, "_PC_REC_XFER_ALIGN");
static_assert(_PC_SYMLINK_MAX == BIONIC_PC_SYMLINK_MAX, "_PC_SYMLINK_MAX");
static_assert(_PC_CHOWN_RESTRICTED == BIONIC_PC_CHOWN_RESTRICTED, "_PC_CHOWN_RESTRICTED");
static_assert(_PC_NO_TRUNC == BIONIC_PC_NO_TRUNC, "_PC_NO_TRUNC");
static_assert(_PC_VDISABLE == BIONIC_PC_VDISABLE, "_PC_VDISABLE");
static_assert(_PC_ASYNC_IO == BIONIC_PC_ASYNC_IO, "_PC_ASYNC_IO");
static_assert(_PC_PRIO_IO == BIONIC_PC_PRIO_IO, "_PC_PRIO_IO");
static_assert(_PC_SYNC_IO == BIONIC_PC_SYNC_IO, "_PC_SYNC_IO");

// ---- netdb ----
static_assert(sizeof(struct addrinfo) == sizeof(struct bionic_addrinfo), "addrinfo size");
SAME(struct addrinfo, ai_flags, struct bionic_addrinfo, ai_flags);
SAME(struct addrinfo, ai_family, struct bionic_addrinfo, ai_family);
SAME(struct addrinfo, ai_socktype, struct bionic_addrinfo, ai_socktype);
SAME(struct addrinfo, ai_protocol, struct bionic_addrinfo, ai_protocol);
SAME(struct addrinfo, ai_addrlen, struct bionic_addrinfo, ai_addrlen);
SAME(struct addrinfo, ai_canonname, struct bionic_addrinfo, ai_canonname);
SAME(struct addrinfo, ai_addr, struct bionic_addrinfo, ai_addr);
SAME(struct addrinfo, ai_next, struct bionic_addrinfo, ai_next);
static_assert(AI_PASSIVE == BIONIC_AI_PASSIVE, "AI_PASSIVE");
static_assert(AI_CANONNAME == BIONIC_AI_CANONNAME, "AI_CANONNAME");
static_assert(AI_NUMERICHOST == BIONIC_AI_NUMERICHOST, "AI_NUMERICHOST");
static_assert(AI_NUMERICSERV == BIONIC_AI_NUMERICSERV, "AI_NUMERICSERV");
static_assert(AI_ALL == BIONIC_AI_ALL, "AI_ALL");
static_assert(AI_V4MAPPED_CFG == BIONIC_AI_V4MAPPED_CFG, "AI_V4MAPPED_CFG");
static_assert(AI_ADDRCONFIG == BIONIC_AI_ADDRCONFIG, "AI_ADDRCONFIG");
static_assert(AI_V4MAPPED == BIONIC_AI_V4MAPPED, "AI_V4MAPPED");
static_assert(AI_MASK == BIONIC_AI_MASK, "AI_MASK");
static_assert(EAI_ADDRFAMILY == BIONIC_EAI_ADDRFAMILY, "EAI_ADDRFAMILY");
static_assert(EAI_AGAIN == BIONIC_EAI_AGAIN, "EAI_AGAIN");
static_assert(EAI_BADFLAGS == BIONIC_EAI_BADFLAGS, "EAI_BADFLAGS");
static_assert(EAI_FAIL == BIONIC_EAI_FAIL, "EAI_FAIL");
static_assert(EAI_FAMILY == BIONIC_EAI_FAMILY, "EAI_FAMILY");
static_assert(EAI_MEMORY == BIONIC_EAI_MEMORY, "EAI_MEMORY");
static_assert(EAI_NODATA == BIONIC_EAI_NODATA, "EAI_NODATA");
static_assert(EAI_NONAME == BIONIC_EAI_NONAME, "EAI_NONAME");
static_assert(EAI_SERVICE == BIONIC_EAI_SERVICE, "EAI_SERVICE");
static_assert(EAI_SOCKTYPE == BIONIC_EAI_SOCKTYPE, "EAI_SOCKTYPE");
static_assert(EAI_SYSTEM == BIONIC_EAI_SYSTEM, "EAI_SYSTEM");
static_assert(EAI_OVERFLOW == BIONIC_EAI_OVERFLOW, "EAI_OVERFLOW");
