#!/bin/sh
# test_platform.sh -- platform-layer tests on the PC, under a headless weston
#
#   tests/test_platform.sh
#
# Builds the platform sources natively (portable C over Wayland, EGL and
# SDL2 input) and runs:
#   test_platform         window, EGL from a render thread, vsync, input, quit
#   test_platform scaled  render_scale and setBuffersGeometry, checked on
#                         screenshots of the compositor's output
#   mutate_platform.sh    broken platform code that the above must catch
#   test_glcache          GL state cache vs direct GL, in lockstep (llvmpipe)
#   mutate_glcache.sh     broken caches that test_glcache must catch
#   test_platform scaled  again under a headless sway (the compositor Lakka
#                         runs: 0x0 first configure, fullscreen when mapped)
#                         on a rotated 600x800 output, i.e. 800x600, plus
#                         its mutants; skipped without sway and grim
# Needs weston, wayland-scanner, wayland-protocols, gcc, python3 with PIL and
# the SDL2/EGL/GLES/wayland development packages.
set -eu

cd "$(dirname "$0")/.."
B=tests/build/native
mkdir -p "$B"
CFLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
  -Wno-missing-field-initializers -Isrc -Isrc/compat -Iupstream/source -I$B"
fail=0

P=$(pkg-config --variable=pkgdatadir wayland-protocols)
for proto in stable/xdg-shell/xdg-shell stable/viewporter/viewporter; do
  name=$(basename $proto)
  wayland-scanner client-header "$P/$proto.xml" "$B/$name-client-protocol.h"
  wayland-scanner private-code "$P/$proto.xml" "$B/$name-protocol.c"
done

PLATFORM_SRCS="src/platform.c src/platform_config.c src/egl_wrap.c src/gl_wrap.c src/gamenative.c \
  src/stats.c src/util.c src/tls_guard.c src/error.c upstream/source/config.c upstream/source/overlay.c \
  $B/xdg-shell-protocol.c $B/viewporter-protocol.c"
export PLATFORM_SRCS CFLAGS
gcc $CFLAGS -o "$B/test_platform" tests/test_platform.c $PLATFORM_SRCS \
  -Wl,--wrap=eglMakeCurrent,--wrap=eglSwapInterval \
  -lSDL2 -lwayland-egl -lwayland-client -lEGL -lGLESv2 -lm -lpthread
gcc $CFLAGS -o "$B/test_glcache" tests/test_glcache.c src/gl_wrap.c -lEGL -lGLESv2 -lpthread

# a private compositor: 640x480 like the RF35H panel, software rendering,
# no panel (sway has none over a fullscreen window), --debug for screenshots
RUNDIR=$(mktemp -d)
chmod 700 "$RUNDIR"
export XDG_RUNTIME_DIR="$RUNDIR"
printf '[core]\nidle-time=0\n[shell]\npanel-position=none\n' > "$B/weston.ini"
weston --backend=headless --renderer=pixman --width=640 --height=480 --socket=gtasa-test \
  --config="$PWD/$B/weston.ini" --debug --log="$B/weston.log" &
WESTON=$!
SWAY=
SWAYDIR=
trap 'kill $WESTON $SWAY 2>/dev/null; rm -rf "$RUNDIR" "$SWAYDIR"' EXIT
for _ in $(seq 50); do [ -S "$RUNDIR/gtasa-test" ] && break; sleep 0.1; done
export WAYLAND_DISPLAY=gtasa-test LIBGL_ALWAYS_SOFTWARE=1

echo "-- test_platform"
"$B/test_platform" || fail=1
"$B/test_platform" scaled || fail=1

echo "-- mutate_platform"
tests/mutate_platform.sh "$B" || fail=1

echo "-- test_glcache"
"$B/test_glcache" 20000 || fail=1
out=$("$B/test_glcache" 20000 7) || fail=1
echo "$out" | tail -1

echo "-- mutate_glcache"
tests/mutate_glcache.sh "$B" || fail=1

echo "-- test_platform scaled under sway (600x800 panel rotated: 800x600)"
if command -v sway > /dev/null && command -v grim > /dev/null; then
  SWAYDIR=$(mktemp -d)
  chmod 700 "$SWAYDIR"
  printf 'output HEADLESS-1 mode 600x800 transform 90\ndefault_border none\n' > "$B/sway.conf"
  XDG_RUNTIME_DIR="$SWAYDIR" WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$B/sway.conf" > "$B/sway.log" 2>&1 &
  SWAY=$!
  for _ in $(seq 50); do ls "$SWAYDIR"/sway-ipc.*.sock > /dev/null 2>&1 && break; sleep 0.1; done
  sleep 0.5
  SWAY_ENV="XDG_RUNTIME_DIR=$SWAYDIR WAYLAND_DISPLAY=$(ls "$SWAYDIR" | grep -m1 '^wayland-[0-9]*$') \
    SWAYSOCK=$(ls "$SWAYDIR"/sway-ipc.*.sock) OUT_W=800 OUT_H=600"
  export SWAY_ENV
  env $SWAY_ENV "$B/test_platform" scaled || fail=1
  tests/mutate_platform.sh "$B" sway || fail=1
else
  echo "skipped: sway and grim are not installed"
fi

exit $fail
