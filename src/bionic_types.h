/* bionic_types.h -- the bionic (Android LP64/arm64) layouts the shims need
 *
 * Only what differs from glibc, or what we must know the size of to embed a
 * pointer in it, lives here. tests/abi/bionic_types_check.cpp checks every
 * definition below against the real bionic headers, and tests/abi/check_abi.sh
 * compares bionic with glibc for the layouts and constants that pass straight
 * through, so a pass-through in import_table.c is a checked decision rather
 * than an assumption.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef GTASA_BIONIC_TYPES_H
#define GTASA_BIONIC_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ---- stdio --------------------------------------------------------------- */

/* sizeof(FILE) in bionic LP64; stdout is &__sF[1], stderr &__sF[2] */
#define BIONIC_FILE_SIZE 152

/* ---- struct stat / dirent / statvfs (asm-generic layouts) ---------------- */

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

struct bionic_statvfs {
  unsigned long f_bsize;
  unsigned long f_frsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  uint64_t f_favail;
  unsigned long f_fsid;
  unsigned long f_flag;
  unsigned long f_namemax;
  uint32_t __f_reserved[6];
};

/* ---- pthread (only sizes and the fields we read) ------------------------- */

#define BIONIC_MUTEX_SIZE    40
#define BIONIC_COND_SIZE     48
#define BIONIC_RWLOCK_SIZE   56
#define BIONIC_SEM_SIZE      16

/* static initialisers, as they appear in the first 32-bit word */
#define BIONIC_MUTEX_INIT_NORMAL     0x0000u
#define BIONIC_MUTEX_INIT_RECURSIVE  0x4000u
#define BIONIC_MUTEX_INIT_ERRORCHECK 0x8000u

typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
  char __reserved[16];
} bionic_pthread_attr_t;

#define BIONIC_PTHREAD_ATTR_FLAG_DETACHED 0x00000001u
#define BIONIC_PTHREAD_CREATE_JOINABLE 0
#define BIONIC_PTHREAD_CREATE_DETACHED 1

/* pthread_{mutex,cond,rwlock}attr_t are a single long in bionic */
typedef long bionic_pthread_condattr_t;
#define BIONIC_CONDATTR_CLOCK_MONOTONIC 0x2u /* our own encoding inside the long */

/* ---- signals --------------------------------------------------------------- */

typedef unsigned long bionic_sigset_t; /* 64 signals in one word */

/* field names avoid sa_*: glibc's <signal.h> defines sa_handler as a macro */
struct bionic_sigaction {
  int flags;
  void *handler; /* sa_handler or sa_sigaction */
  bionic_sigset_t mask;
  void (*restorer)(void);
};

/* ---- setjmp ------------------------------------------------------------------- */

#define BIONIC_JMPBUF_WORDS 32 /* long[32]; we use 22 of them */

#define BIONIC_SA_RESTORER 0x04000000 /* kernel flag; glibc installs its own restorer */

/* ---- sysconf names that differ from glibc --------------------------------- */

#define BIONIC_SC_PAGESIZE          0x0027
#define BIONIC_SC_PAGE_SIZE         0x0028
#define BIONIC_SC_NPROCESSORS_CONF  0x0060
#define BIONIC_SC_NPROCESSORS_ONLN  0x0061
#define BIONIC_SC_PHYS_PAGES        0x0062
#define BIONIC_SC_AVPHYS_PAGES      0x0063

/* ---- pathconf names (bionic numbers them in another order) --------------- */

#define BIONIC_PC_FILESIZEBITS       0
#define BIONIC_PC_LINK_MAX           1
#define BIONIC_PC_MAX_CANON          2
#define BIONIC_PC_MAX_INPUT          3
#define BIONIC_PC_NAME_MAX           4
#define BIONIC_PC_PATH_MAX           5
#define BIONIC_PC_PIPE_BUF           6
#define BIONIC_PC_2_SYMLINKS         7
#define BIONIC_PC_ALLOC_SIZE_MIN     8
#define BIONIC_PC_REC_INCR_XFER_SIZE 9
#define BIONIC_PC_REC_MAX_XFER_SIZE  10
#define BIONIC_PC_REC_MIN_XFER_SIZE  11
#define BIONIC_PC_REC_XFER_ALIGN     12
#define BIONIC_PC_SYMLINK_MAX        13
#define BIONIC_PC_CHOWN_RESTRICTED   14
#define BIONIC_PC_NO_TRUNC           15
#define BIONIC_PC_VDISABLE           16
#define BIONIC_PC_ASYNC_IO           17
#define BIONIC_PC_PRIO_IO            18
#define BIONIC_PC_SYNC_IO            19

/* ---- netdb: BSD struct order, BSD flag values, positive error codes ------ */

struct bionic_addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  uint32_t ai_addrlen; /* socklen_t */
  char *ai_canonname;  /* before ai_addr, unlike glibc */
  void *ai_addr;       /* struct sockaddr * */
  struct bionic_addrinfo *ai_next;
};

#define BIONIC_AI_PASSIVE      0x0001
#define BIONIC_AI_CANONNAME    0x0002
#define BIONIC_AI_NUMERICHOST  0x0004
#define BIONIC_AI_NUMERICSERV  0x0008
#define BIONIC_AI_ALL          0x0100
#define BIONIC_AI_V4MAPPED_CFG 0x0200
#define BIONIC_AI_ADDRCONFIG   0x0400
#define BIONIC_AI_V4MAPPED     0x0800
/* what bionic's getaddrinfo accepts in hints; anything else is EAI_BADFLAGS */
#define BIONIC_AI_MASK (BIONIC_AI_PASSIVE | BIONIC_AI_CANONNAME | BIONIC_AI_NUMERICHOST | \
                        BIONIC_AI_NUMERICSERV | BIONIC_AI_ADDRCONFIG)

#define BIONIC_EAI_ADDRFAMILY 1
#define BIONIC_EAI_AGAIN      2
#define BIONIC_EAI_BADFLAGS   3
#define BIONIC_EAI_FAIL       4
#define BIONIC_EAI_FAMILY     5
#define BIONIC_EAI_MEMORY     6
#define BIONIC_EAI_NODATA     7
#define BIONIC_EAI_NONAME     8
#define BIONIC_EAI_SERVICE    9
#define BIONIC_EAI_SOCKTYPE   10
#define BIONIC_EAI_SYSTEM     11
#define BIONIC_EAI_OVERFLOW   14

#endif
