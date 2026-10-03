/* dump.c -- the values listed in same.h, as `const long abi_<name>` objects.
 * Compiled to assembly against bionic and against glibc by check_abi.sh.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#if defined(__BIONIC__)
#include <asm/hwcap.h>
#else
#include <bits/hwcap.h>
#endif

#define S(name, expr) const long abi_##name = (long)(expr);
#include "same.h"
