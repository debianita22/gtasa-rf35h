# SPDX-License-Identifier: MIT
# GTA: San Andreas (Android 2.11.311 arm64-v8a) for the XiFan RF35H.
# The image carries the loader, the launcher core and the scripts only: the
# game files come from the user's own copy (APK + OBB copied to
# ROMs/gtasa, installed by the launcher on first start).

PKG_NAME="gtasa"
PKG_VERSION="1.0.0"
PKG_ARCH="aarch64"
PKG_LICENSE="MIT"
PKG_SITE=""
PKG_URL=""
PKG_DEPENDS_TARGET="toolchain wayland:host wayland wayland-protocols ${OPENGLES} SDL2_input openal-soft mpg123 zlib retroarch"
PKG_LONGDESC="Linux loader for GTA: San Andreas Android 2.11.311 (gtasa_nx hooks), launched from RetroArch's contentless cores"
PKG_TOOLCHAIN="manual"
# sources/ (copied into the build folder by scripts/unpack): src/, upstream/, Makefile

make_target() {
  # the toolchain's CC/CFLAGS/LDFLAGS already target the RK3326 (TUNE= drops
  # the Makefile's own -march/-mtune)
  make VERSION="${PKG_VERSION}" TUNE= \
       PKG_CONFIG="${PKG_CONFIG:-pkg-config}" \
       WAYLAND_SCANNER="${TOOLCHAIN}/bin/wayland-scanner" \
       WAYLAND_PROTOCOLS="${SYSROOT_PREFIX}/usr/share/wayland-protocols" \
       gtasa
  ${CC} ${CFLAGS} -std=gnu11 -fPIC -shared -Wl,-soname,gtasa_libretro.so \
       -o gtasa_libretro.so ${PKG_DIR}/launcher/gtasa_libretro.c ${LDFLAGS}
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin ${INSTALL}/usr/lib/libretro ${INSTALL}/usr/share/gtasa
  cp gtasa ${INSTALL}/usr/bin/gtasa
  cp ${PKG_DIR}/scripts/rf35h-gtasa ${INSTALL}/usr/bin/
  chmod 0755 ${INSTALL}/usr/bin/gtasa ${INSTALL}/usr/bin/rf35h-gtasa
  cp gtasa_libretro.so ${PKG_DIR}/launcher/gtasa_libretro.info ${INSTALL}/usr/lib/libretro/
  cp ${PKG_DIR}/LEGGIMI.md upstream/LICENSE upstream/CHEATS.md upstream/MUSIC.md \
     ${INSTALL}/usr/share/gtasa/
}
