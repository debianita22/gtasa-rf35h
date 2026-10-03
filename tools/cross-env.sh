#!/bin/sh
# cross-env.sh -- headers and link stubs for cross-building gtasa on a PC
#
#   tools/cross-env.sh OUTDIR [HOST_LIBDIR]
#
# Makes OUTDIR/include (the target libraries' headers, copied from the
# host's -dev packages without the host's libc headers) and OUTDIR/lib
# (aarch64 shared objects exporting the same names as the host's x86-64
# copies, with empty bodies). Both are enough to compile and link gtasa and
# to run the loader tests under qemu; nothing in the tests calls into the
# stubs. Device builds use the Lakka toolchain's real libraries instead.
#
# Ubuntu 24.04 packages: libsdl2-dev libegl-dev libgles-dev libopenal-dev
# libmpg123-dev libwayland-dev zlib1g-dev gcc-aarch64-linux-gnu
set -eu

OUT=${1:?usage: cross-env.sh OUTDIR [HOST_LIBDIR]}
LIBDIR=${2:-/usr/lib/x86_64-linux-gnu}
TRIPLE=$(basename "$LIBDIR")
CC=${CC:-aarch64-linux-gnu-gcc}
mkdir -p "$OUT/include" "$OUT/lib"

# headers: library headers only; the arch-specific ones are LP64 Linux on
# both x86-64 and aarch64 (64-bit off_t, same type sizes)
for d in AL EGL GLES2 GLES3 KHR SDL2; do
  cp -r "/usr/include/$d" "$OUT/include/"
done
cp /usr/include/wayland-*.h "$OUT/include/"
cp "/usr/include/$TRIPLE/mpg123.h" "/usr/include/$TRIPLE/fmt123.h" "$OUT/include/"
cp "/usr/include/$TRIPLE/SDL2/_real_SDL_config.h" "$OUT/include/SDL2/"
# SDL_config.h includes <SDL2/_real_SDL_config.h>, found via -I.../include/SDL2
mkdir -p "$OUT/include/SDL2/SDL2"
cp "/usr/include/$TRIPLE/SDL2/_real_SDL_config.h" "$OUT/include/SDL2/SDL2/"

# link stubs
while read -r name soname; do
  nm -D --defined-only "$LIBDIR/$soname" |
    awk '$2 ~ /^[TtWi]$/ { print "F", $3 } $2 ~ /^[BbDdRrV]$/ { print "D", $3 }' |
    sed 's/@.*//' | sort -u > "$OUT/lib/$name.syms"
  {
    echo "/* link-only stub of $soname */"
    awk '$1 == "F" { print "void " $2 "(void) {}" } $1 == "D" { print "char " $2 "[256];" }' \
      "$OUT/lib/$name.syms"
  } > "$OUT/lib/$name.c"
  $CC -shared -fPIC -fno-builtin -w -Wl,-soname,"$soname" -o "$OUT/lib/lib$name.so" "$OUT/lib/$name.c"
  ln -sf "lib$name.so" "$OUT/lib/$soname"
done <<EOF
SDL2 libSDL2-2.0.so.0
EGL libEGL.so.1
GLESv2 libGLESv2.so.2
openal libopenal.so.1
mpg123 libmpg123.so.0
wayland-client libwayland-client.so.0
wayland-egl libwayland-egl.so.1
z libz.so.1
EOF
echo "cross environment ready in $OUT"
