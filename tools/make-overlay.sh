#!/bin/sh
# make-overlay.sh -- the Lakka package as an overlay for the RF35H build tree
#
#   tools/make-overlay.sh [OUT.tar.gz]
#
# The tarball holds projects/Rockchip/devices/RK3326/packages/gtasa (the
# package, with this project's sources under sources/) and install.sh, which
# copies it into a Lakka-LibreELEC tree and adds the package to the RK3326
# options for UBOOT_SYSTEM=rf35h. The game files are not part of it.
set -eu

cd "$(dirname "$0")/.."
OUT=${1:-gtasa-rf35h-overlay.tar.gz}
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
R="$T/gtasa-rf35h-overlay"
P="$R/projects/Rockchip/devices/RK3326/packages/gtasa"
mkdir -p "$P/sources"

cp -r packaging/lakka/gtasa/. "$P/"
cp Makefile "$P/sources/"
cp -r src upstream "$P/sources/"
rm -f "$P/sources/upstream/FILES.git-blobs"

cat > "$R/install.sh" <<'EOF'
#!/bin/sh
# install.sh LAKKA_TREE -- add the gtasa package to a Lakka-LibreELEC tree
set -eu
TREE=${1:?usage: install.sh /path/to/Lakka-LibreELEC}
HERE=$(cd "$(dirname "$0")" && pwd)
OPTS="$TREE/projects/Rockchip/devices/RK3326/options"
[ -f "$OPTS" ] || { echo "not a Lakka tree with RK3326: $TREE" >&2; exit 1; }
DEST="$TREE/projects/Rockchip/devices/RK3326/packages/gtasa"
rm -rf "$DEST"
cp -r "$HERE/projects/Rockchip/devices/RK3326/packages/gtasa" "$DEST"
if ! grep -q 'ADDITIONAL_PACKAGES+=" gtasa"' "$OPTS"; then
  cat >> "$OPTS" <<'OPT'

  # GTA: San Andreas launcher for the RF35H (game files from the user's own copy)
    if [ "${UBOOT_SYSTEM}" = "rf35h" ]; then
      ADDITIONAL_PACKAGES+=" gtasa"
    fi
OPT
  echo "options: gtasa added for UBOOT_SYSTEM=rf35h"
fi
echo "package in $DEST"
echo "build: UBOOT_SYSTEM=rf35h PROJECT=Rockchip DEVICE=RK3326 ARCH=aarch64 scripts/build gtasa (or make image)"
EOF
chmod 0755 "$R/install.sh" "$P/scripts/rf35h-gtasa"

tar -C "$T" --owner=0 --group=0 --sort=name -czf "$OUT" gtasa-rf35h-overlay
echo "$OUT ($(du -h "$OUT" | cut -f1))"
