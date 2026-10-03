#!/bin/sh
# run-tests.sh -- build and run the test suite on a PC (x86-64 Ubuntu 24.04)
#
#   tests/run-tests.sh
#
# Environment (defaults in brackets):
#   CROSSENV  headers + aarch64 link stubs, made by tools/cross-env.sh when
#             missing [tests/build/cross]
#   BIONIC    bionic source checkout for the ABI check; skipped if unset
#   LAKKA     Lakka-LibreELEC checkout for the package build; skipped if unset
#   BUSYBOX   busybox binary: the runner script is also tested under its ash
#
# Needs: aarch64-linux-gnu-gcc, clang + lld, qemu-aarch64-static, python3
# with PIL; for the native parts gcc, weston, wayland-scanner and the
# SDL2/EGL/GLES/wayland/zlib development packages.
set -eu

cd "$(dirname "$0")/.."
B=tests/build
mkdir -p "$B"
CROSSENV=${CROSSENV:-$B/cross}
STUBS=$CROSSENV/lib
XINC=$CROSSENV/include
BIONIC=${BIONIC:-}
CROSS=aarch64-linux-gnu-
XF="-I$XINC -I$XINC/SDL2 -DSDL_DISABLE_IMMINTRIN_H"
LIBS="-L$STUBS -Wl,-rpath-link,$STUBS -lSDL2 -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lopenal -lmpg123 \
  -Wl,--no-as-needed -lz -lm -Wl,--as-needed -lpthread -ldl"
QEMU="qemu-aarch64-static -cpu cortex-a35 -L /usr/aarch64-linux-gnu -E LD_LIBRARY_PATH=$STUBS"
fail=0
step() { printf '\n== %s\n' "$*"; }

[ -e "$STUBS/libSDL2-2.0.so.0" ] || tools/cross-env.sh "$CROSSENV" >/dev/null

step "vendored gtasa_nx files"
tools/verify-upstream.sh || fail=1

step "bionic ABI vs glibc (real headers)"
if [ -n "$BIONIC" ] && [ -d "$BIONIC/libc/include" ]; then
  tests/abi/check_abi.sh "$BIONIC" || fail=1
else
  echo "skipped: set BIONIC to a bionic checkout"
  ${CROSS}gcc -std=gnu11 -Wall -c -o "$B/abi_check.o" tests/abi_check.c && echo "fallback abi_check.c: ok"
fi

step "build fingerprints vs GNU as encodings"
python3 tests/check_fingerprints.py || fail=1

step "cross build (aarch64, warnings are failures)"
if make -s CROSS=$CROSS PKG_CFLAGS="$XF" \
     PKG_LIBS="-L$STUBS -Wl,-rpath-link,$STUBS -lSDL2 -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lopenal -lmpg123 -lz" \
     gtasa > "$B/make.log" 2>&1; then
  if grep -E 'warning|error' "$B/make.log"; then fail=1; else echo "ok"; fi
else
  cat "$B/make.log"
  fail=1
fi

step "test libraries (GNU ld RELA; clang/lld Android with APS2 + RELR)"
G="-mstack-protector-guard=sysreg -mstack-protector-guard-reg=tpidr_el0 -mstack-protector-guard-offset=40"
${CROSS}gcc -O2 -g -fPIC -shared -nostdlib -fno-builtin -fstack-protector-all $G \
  -o "$B/bionic_lib.so" tests/bionic_lib.c
clang --target=aarch64-linux-android29 -O2 -g -fPIC -shared -nostdlib -fno-builtin \
  -fstack-protector-all -fuse-ld=lld -Wl,--pack-dyn-relocs=android+relr \
  -o "$B/bionic_lib_ndk.so" tests/bionic_lib.c
echo "ok"

OBJS=$(ls build/*.o | grep -v -E 'tls_guard|/main.o|up_game|_stub')
link_test() {
  ${CROSS}gcc -std=gnu11 -O2 -g -Wall -Isrc -Isrc/compat -Iupstream/source $XF \
    -c -o "$B/$1.o" "tests/$1.c"
  ${CROSS}gcc -o "$B/$1" build/tls_guard.o "$B/$1.o" $OBJS $LIBS
}

step "loader and bionic shims under qemu"
link_test test_loader
for lib in bionic_lib.so bionic_lib_ndk.so; do
  echo "-- $lib"
  $QEMU "$B/test_loader" "$B/$lib" 2>/dev/null || fail=1
done
QEMU="$QEMU" XF="$XF" LIBS="$LIBS" CROSS="$CROSS" tests/mutate_loader.sh "$B" || fail=1

step "gtasa --import while another import runs (qemu)"
D="$PWD/$B/import-busy"
rm -rf "$D"
mkdir -p "$D"
echo "the running import's log" > "$D/gtasa-import.log"
(flock -x "$D/.import.lock" sleep 5) &
holder=$!
sleep 0.5
rc=0
$QEMU ./gtasa -d "$D" --import 2>/dev/null || rc=$?
kill $holder 2>/dev/null || :
wait $holder 2>/dev/null || :
if [ $rc -eq 3 ] && [ "$(cat "$D/gtasa-import.log")" = "the running import's log" ] && [ ! -e "$D/gtasa-import.log.1" ] &&
   [ ! -e "$D/import-status.txt" ]; then
  echo "ok: exit 3, the running import's log and status untouched"
else
  echo "FAIL: exit $rc, files: $(ls "$D" | tr '\n' ' ')"
  fail=1
fi

step "the game's own 2.11.311 binary (optional: GTASA_GAME_DIR)"
if [ -n "${GTASA_GAME_DIR:-}" ] && [ -f "$GTASA_GAME_DIR/libGame.so" ]; then
  link_test test_game
  rm -rf "$B/game-shaders"
  $QEMU "$B/test_game" "$GTASA_GAME_DIR" "$B/game-shaders" 2>/dev/null || fail=1
  if command -v glslangValidator > /dev/null; then
    n=0 bad=0
    for f in "$B"/game-shaders/*.frag "$B"/game-shaders/*.vert; do
      [ -f "$f" ] || continue
      n=$((n + 1))
      glslangValidator "$f" > /dev/null 2>&1 || { echo "  invalid GLSL ES: $f"; bad=$((bad + 1)); }
    done
    if [ $bad = 0 ] && [ $n -gt 0 ]; then echo "glslang: $n shaders valid GLSL ES 1.00"; else fail=1; fi
  else
    echo "glslangValidator not installed: shaders not validated"
  fi
  QEMU="$QEMU" XF="$XF" LIBS="$LIBS" CROSS="$CROSS" tests/mutate_game.sh "$B" "$GTASA_GAME_DIR" || fail=1
else
  echo "skipped: set GTASA_GAME_DIR to a folder with your libGame.so, libc++_shared.so and scache.txt"
fi

step "binding of every import gtasa_nx 3460c75 knows of"
link_test import_coverage
$QEMU "$B/import_coverage" tests/upstream-imports.txt || fail=1

step "platform layer (x86-64, headless weston)"
tests/test_platform.sh || fail=1

N=tests/build/native
NF="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Isrc -Isrc/compat -Iupstream/source"
step "import of the APK and OBB (x86-64)"
gcc $NF -o "$N/test_import" tests/test_import.c src/import.c src/zipx.c src/util.c \
  src/platform_config.c src/tls_guard.c upstream/source/config.c -lz &&
  python3 tests/test_import.py "$N" && tests/mutate_import.sh "$N" || fail=1

step "launcher core (x86-64, fake frontend)"
gcc $NF -fPIC -shared -o "$N/gtasa_libretro.so" packaging/lakka/gtasa/launcher/gtasa_libretro.c &&
  gcc $NF -o "$N/test_launcher" tests/test_launcher.c -ldl &&
  rm -rf "$N/launcher-work" && "$N/test_launcher" "$N/gtasa_libretro.so" "$N/launcher-work" &&
  tests/mutate_launcher.sh "$N" || fail=1

step "shader source rewrite, texture_lod_bias (x86-64)"
gcc $NF -o "$N/test_shader_rewrite" tests/test_shader_rewrite.c src/gl_wrap.c src/util.c src/tls_guard.c \
  src/platform_config.c upstream/source/config.c -Wl,--wrap=glShaderSource -lGLESv2 -lpthread &&
  "$N/test_shader_rewrite" && tests/mutate_rewrite.sh "$N" || fail=1

step "runner script rf35h-gtasa"
BUSYBOX= tests/test_runner.sh "$N" || fail=1 # the system's sh (dash on Debian/Ubuntu)
if [ -n "${BUSYBOX:-}" ]; then BUSYBOX="$BUSYBOX" tests/test_runner.sh "$N" || fail=1; fi
tests/mutate_runner.sh "$N" || fail=1

step "Lakka package, built as LibreELEC does"
if [ -n "${LAKKA:-}" ] && [ -d "$LAKKA/projects/Rockchip/devices/RK3326" ]; then
  tests/test_package.sh "$LAKKA" "$CROSSENV" > "$B/package.log" 2>&1 || fail=1
  grep -v -E '^(aarch64-linux-gnu-gcc|wayland-scanner) ' "$B/package.log"
else
  echo "skipped: set LAKKA to a Lakka-LibreELEC checkout"
fi

[ $fail = 0 ] && printf '\nALL TESTS PASSED\n' || { printf '\nSOME TESTS FAILED\n'; exit 1; }
