#!/bin/sh
# check_abi.sh -- bionic vs glibc, from the real headers of both
#
#   tests/abi/check_abi.sh BIONIC_DIR
#
# BIONIC_DIR is a checkout of https://android.googlesource.com/platform/bionic
# (or its GitHub mirror aosp-mirror/platform_bionic); only libc/include,
# libc/kernel, libc/stdio, libc/private and libc/async_safe/include are read.
# The LP64 ABI is frozen, so any recent revision describes what the game was
# built against.
#
# 1. src/bionic_types.h must agree with bionic (bionic_types_check.cpp).
# 2. Everything in same.h must have the same value under both C libraries.
# 3. sizeof(FILE) must be the 152 bytes the fake __sF array is built with.
set -eu

B=${1:?usage: check_abi.sh BIONIC_DIR}
cd "$(dirname "$0")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
RES=$(clang -print-resource-dir)
BINC="-nostdinc -isystem $RES/include -isystem $B/libc/include -isystem $B/libc/kernel/uapi \
  -isystem $B/libc/kernel/uapi/asm-arm64 -isystem $B/libc/kernel/android/uapi"
fail=0

# 1
if clang++ --target=aarch64-linux-android29 -std=c++17 -nostdinc++ $BINC -fsyntax-only \
     bionic_types_check.cpp; then
  echo "bionic_types.h agrees with bionic"
else
  echo "bionic_types.h DISAGREES with bionic"
  fail=1
fi

# 2
clang --target=aarch64-linux-android29 $BINC -O0 -S -o "$T/bionic.s" dump.c
aarch64-linux-gnu-gcc -O0 -S -o "$T/glibc.s" dump.c
python3 - "$T/bionic.s" "$T/glibc.s" <<'EOF' || fail=1
import re, sys

def values(path):
    out, name = {}, None
    for line in open(path):
        m = re.match(r'^(abi_\w+):', line)
        if m:
            name = m.group(1)[4:]
            continue
        m = re.match(r'^\s+\.(xword|quad|8byte)\s+(-?\w+)', line)
        if name and m:
            out[name] = int(m.group(2), 0)
            name = None
        elif name and re.match(r'^\s+\.zero\s+8', line):
            out[name] = 0
            name = None
    return out

b, g = values(sys.argv[1]), values(sys.argv[2])
# differences that are known and harmless for what the game does with them
known = {
    # glibc defines O_LARGEFILE as 0 on LP64; the kernel implies it on arm64
    # anyway, so bionic's bit passed through to open() changes nothing
    'O_LARGEFILE': (0o400000, 0),
    # only the typedef differs: struct stat declares st_blksize as int in
    # both (stat_st_blksize* above compare equal), and no imported function
    # takes a blksize_t
    'blksize_t': (8, 4),
}
bad = 0
for k in sorted(set(b) | set(g)):
    if k not in b or k not in g:
        print(f"  missing on {'bionic' if k not in b else 'glibc'}: {k}")
        bad += 1
    elif b[k] != g[k]:
        if known.get(k) == (b[k], g[k]):
            print(f"  known: {k} bionic {b[k]:#x} glibc {g[k]:#x}")
        else:
            print(f"  DIFF {k}: bionic {b[k]:#x} glibc {g[k]:#x}")
            bad += 1
print(f"{len(b)} layouts and constants compared, {bad} unexpected differences")
sys.exit(1 if bad else 0)
EOF

# 3: stdio/local.h is bionic's private FILE definition; it pulls in
# <android/log.h> (liblog, not part of bionic) only for a priority enum
mkdir -p "$T/android"
printf 'typedef enum android_LogPriority { ANDROID_LOG_FATAL = 7 } android_LogPriority;\n' \
  > "$T/android/log.h"
cat > "$T/file.cpp" <<'EOF'
#include <stdio.h>
#include "local.h"
static_assert(sizeof(struct __sFILE) == 152, "bionic FILE is not 152 bytes");
EOF
if clang++ --target=aarch64-linux-android29 -std=c++17 -nostdinc++ $BINC -I"$B/libc/stdio" \
     -I"$B/libc" -I"$B/libc/async_safe/include" -I"$T" -fsyntax-only "$T/file.cpp" 2>"$T/file.err"; then
  echo "bionic FILE is 152 bytes"
else
  echo "FILE size check failed:"; head -5 "$T/file.err"
  fail=1
fi

[ $fail = 0 ] && echo "abi: ok" || { echo "abi: FAILED"; exit 1; }
