#!/bin/sh
# test_package.sh -- the Lakka package, built the way LibreELEC builds it
#
#   tests/test_package.sh LAKKA_TREE [CROSSENV]
#
# Installs the overlay into a copy of the tree's RK3326 device folder, then
# runs package.mk's make_target and makeinstall_target under bash with the
# variables LibreELEC's setup_toolchain exports (cross gcc, a pkg-config that
# answers for the target, wayland-scanner, the sysroot's wayland-protocols)
# and checks what lands in INSTALL. LibreELEC itself is not needed.
set -eu

cd "$(dirname "$0")/.."
TREE=${1:?usage: test_package.sh LAKKA_TREE [CROSSENV]}
CROSSENV=${2:-tests/build/cross}
B="$PWD/tests/build/package"
rm -rf "$B"
mkdir -p "$B"
fail=0
check() { if eval "$1"; then :; else echo "  FAIL: $2"; fail=1; fi; }
[ -e "$CROSSENV/lib/libSDL2-2.0.so.0" ] || tools/cross-env.sh "$CROSSENV" > /dev/null
CROSSENV=$(cd "$CROSSENV" && pwd)

echo "-- overlay into a copy of the RK3326 folder"
tools/make-overlay.sh "$B/overlay.tar.gz" > /dev/null
tar -C "$B" -xzf "$B/overlay.tar.gz"
mkdir -p "$B/tree/projects/Rockchip/devices"
cp -r "$TREE/projects/Rockchip/devices/RK3326" "$B/tree/projects/Rockchip/devices/"
sh "$B/gtasa-rf35h-overlay/install.sh" "$B/tree" > /dev/null
sh "$B/gtasa-rf35h-overlay/install.sh" "$B/tree" > /dev/null # twice: idempotent
OPTS="$B/tree/projects/Rockchip/devices/RK3326/options"
check '[ $(grep -c "ADDITIONAL_PACKAGES+=\" gtasa\"" "$OPTS") -eq 1 ]' "options line added exactly once"
check 'bash -n "$OPTS"' "options no longer parses"
for u in "" rf35h; do
  pkgs=$(bash -c 'UBOOT_SYSTEM='"$u"'; ADDITIONAL_PACKAGES=""; . "'"$OPTS"'" >/dev/null 2>&1; echo "$ADDITIONAL_PACKAGES"')
  if [ "$u" = rf35h ]; then
    check 'echo "$pkgs" | grep -qw gtasa' "gtasa not in ADDITIONAL_PACKAGES for rf35h: $pkgs"
  else
    check '! echo "$pkgs" | grep -qw gtasa' "gtasa added for other RK3326 devices: $pkgs"
  fi
done

echo "-- package.mk as LibreELEC runs it"
PKG_DIR="$B/tree/projects/Rockchip/devices/RK3326/packages/gtasa"
check 'bash -n "$PKG_DIR/package.mk"' "package.mk syntax"
BUILD_DIR="$B/build/gtasa-1.0.0"
mkdir -p "$BUILD_DIR"
cp -PRf "$PKG_DIR/sources/"* "$BUILD_DIR/" # what scripts/unpack does with sources/
mkdir -p "$B/toolchain/bin" "$B/sysroot/usr/share"
ln -sf "$(command -v wayland-scanner)" "$B/toolchain/bin/wayland-scanner"
ln -sf "$(pkg-config --variable=pkgdatadir wayland-protocols)" "$B/sysroot/usr/share/wayland-protocols"
cat > "$B/toolchain/bin/pkg-config" <<EOF
#!/bin/sh
# answers for the target sysroot, as LibreELEC's pkg-config does
for a in "\$@"; do
  case "\$a" in
    --cflags) echo "-I$CROSSENV/include -I$CROSSENV/include/SDL2 -DSDL_DISABLE_IMMINTRIN_H"; exit 0 ;;
    --libs) echo "-L$CROSSENV/lib -Wl,-rpath-link,$CROSSENV/lib -lSDL2 -lwayland-client -lwayland-egl -lEGL -lGLESv2 -lopenal -lmpg123 -lz"; exit 0 ;;
  esac
done
exit 1
EOF
chmod +x "$B/toolchain/bin/pkg-config"
INSTALL="$B/install"
(
  cd "$BUILD_DIR"
  export PKG_DIR INSTALL TOOLCHAIN="$B/toolchain" SYSROOT_PREFIX="$B/sysroot"
  export CC=aarch64-linux-gnu-gcc CFLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a35 -O2 -fomit-frame-pointer"
  export LDFLAGS="-Wl,--as-needed" PKG_CONFIG="$B/toolchain/bin/pkg-config" OPENGLES=mesa
  bash -c 'set -e; . "$PKG_DIR/package.mk"; make_target; makeinstall_target'
) > "$B/build.log" 2>&1 || { tail -20 "$B/build.log"; fail=1; }
check '! grep -E "warning:|error:" "$B/build.log"' "compiler warnings in the package build"

echo "-- what lands in the image"
check 'file "$INSTALL/usr/bin/gtasa" | grep -q "ARM aarch64"' "gtasa is not an aarch64 executable"
check 'file "$INSTALL/usr/lib/libretro/gtasa_libretro.so" | grep -q "ARM aarch64"' "launcher core is not aarch64"
check '[ $(aarch64-linux-gnu-nm -D --defined-only "$INSTALL/usr/lib/libretro/gtasa_libretro.so" | grep -c " T retro_") -eq 25 ]' "launcher core does not export the 25 retro_ functions"
check 'grep -qx "supports_no_game = \"true\"" "$INSTALL/usr/lib/libretro/gtasa_libretro.info"' "info file: supports_no_game"
check 'grep -qx "single_purpose = \"true\"" "$INSTALL/usr/lib/libretro/gtasa_libretro.info"' "info file: single_purpose"
check '[ -x "$INSTALL/usr/bin/rf35h-gtasa" ]' "runner script not executable"
check '[ -f "$INSTALL/usr/share/gtasa/LEGGIMI.md" ] && [ -f "$INSTALL/usr/share/gtasa/LICENSE" ]' "documentation missing"
check '[ -f "$PKG_DIR/system.d/rf35h-gtasa.service" ]' "service unit missing (scripts/install copies system.d/)"
# the game runs with RetroArch's environment (Wayland socket, ALSA card, user settings)
RA_UNIT="$TREE/packages/lakka/retroarch_base/retroarch/system.d/retroarch.service"
if [ -f "$RA_UNIT" ]; then
  for ef in $(grep '^EnvironmentFile=' "$RA_UNIT"); do
    check 'grep -qxF "$ef" "$PKG_DIR/system.d/rf35h-gtasa.service"' "the service lacks RetroArch's $ef"
  done
else
  echo "  (retroarch.service not in the tree: environment files not compared)"
fi
check 'aarch64-linux-gnu-readelf -d "$INSTALL/usr/bin/gtasa" | grep -q "libSDL2-2.0.so.0"' "gtasa not linked against SDL2"
check 'aarch64-linux-gnu-readelf -lW "$INSTALL/usr/bin/gtasa" | grep -q "TLS"' "no TLS segment (stack-guard slot)"
check 'aarch64-linux-gnu-objdump -d "$INSTALL/usr/bin/gtasa" | grep -q "<tls_guard_check>"' "tls_guard_check missing"
# the guard array must be the first thing in the executable's TLS block
first_tls=$(aarch64-linux-gnu-readelf -sW "$INSTALL/usr/bin/gtasa" | awk '$4 == "TLS" && $3 > 0 { print $2, $8 }' | sort | head -1)
check 'echo "$first_tls" | grep -q "gtasa_bionic_tls"' "first TLS symbol is not the guard array: $first_tls"

[ $fail = 0 ] && echo "package: ok" || { echo "package: FAILED"; exit 1; }
