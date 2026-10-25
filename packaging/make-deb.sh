#!/usr/bin/env bash
# Build dlx50-tms-editor_<version>_amd64.deb from an already-built binary.
#
# usage: packaging/make-deb.sh <version> <path-to-binary> [outdir]
#
# Depends are derived from the BINARY, not hand-guessed:
#   objdump NEEDED  ->  ldd resolves path  ->  dpkg -S gives the owning
#   package  ->  trailing "t64" stripped.
# Why the t64 strip: noble (24.04) ships libqt6core6t64 etc., while 26.10
# renamed them back to libqt6core6. Noble's t64 packages *Provide* the plain
# names (verified), so plain names resolve on BOTH releases — t64 names do not.
# qt6-qpa-plugins is added explicitly because libqt6gui6 only *Recommends* it,
# and without that package there is no xcb platform plugin and no window.
set -euo pipefail

VERSION="${1:?usage: make-deb.sh <version> <binary> [outdir]}"
BIN="${2:?usage: make-deb.sh <version> <binary> [outdir]}"
OUTDIR="${3:-dist}"

PKG=dlx50-tms-editor
ARCH=amd64
DESC_NAME="TMS / DLX-50-style Editor"

[ -f "$BIN" ] || { echo "FATAL: binary not found: $BIN" >&2; exit 1; }
case "$VERSION" in
  [0-9]*) ;;
  *) echo "FATAL: Debian version must start with a digit (got: $VERSION)" >&2; exit 1 ;;
esac

# ---- derive Depends from the actual link closure -------------------------
declare -A pkgs=()
for needed in $(objdump -p "$BIN" | awk '/NEEDED/{print $2}'); do
  path=$(ldd "$BIN" 2>/dev/null | awk -v lib="$needed" '$1==lib{print $3; exit}') || true
  [ -n "$path" ] || { echo "WARN: could not resolve $needed" >&2; continue; }
  pkg=$(dpkg-query -S "$path" 2>/dev/null | head -1 | sed 's/:.*//') || true
  [ -n "$pkg" ] || { echo "WARN: no package owns $path" >&2; continue; }
  pkg="${pkg%t64}"          # cross-release-safe plain name (see header)
  pkgs["$pkg"]=1
done
pkgs["qt6-qpa-plugins"]=1   # QPA plugin package (xcb); Recommends-only upstream
depends=$(printf '%s\n' "${!pkgs[@]}" | sort | tr '\n' ',' | sed 's/,$//; s/,/, /g')
echo "Derived Depends: $depends"

# ---- stage the filesystem tree ------------------------------------------
ROOT="out/deb-root"
rm -rf "$ROOT"
mkdir -p \
  "$ROOT/opt/$PKG" \
  "$ROOT/usr/bin" \
  "$ROOT/usr/share/applications" \
  "$ROOT/usr/share/doc/$PKG"

install -m 755 "$BIN" "$ROOT/opt/$PKG/tms_editor"
ln -s "/opt/$PKG/tms_editor" "$ROOT/usr/bin/$PKG"
install -m 644 packaging/dlx50-tms-editor.desktop \
  "$ROOT/usr/share/applications/$PKG.desktop"
install -m 644 LICENSE "$ROOT/usr/share/doc/$PKG/copyright"

# ---- control file --------------------------------------------------------
mkdir -p out/deb-root/DEBIAN
cat > out/deb-root/DEBIAN/control <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: dlx50-tms-editor maintainers <noreply@github.com>
Depends: $depends
Section: utils
Priority: optional
Homepage: https://github.com/kubuntugamer/dlx50-tms-editor
Installed-Size: $(du -sk "$ROOT/opt" | cut -f1)
Description: TMS / DLX-50-style editor for DETECTO scale .TMS files
 Clean-room Qt6 desktop editor for the .TMS product/PLU data files used by
 DLX-50-style retail scales. Browse, search and edit entries with the same
 backup-then-overwrite save behaviour as the original vendor tool.
 .
 Independent clean-room implementation developed without access to the
 original DLX-50 software. Not affiliated with Detecto.
EOF

# ---- build ---------------------------------------------------------------
mkdir -p "$OUTDIR"
OUT="$OUTDIR/${PKG}_${VERSION}_${ARCH}.deb"
fakeroot dpkg-deb -b -Zxz "$ROOT" "$OUT"
rm -rf "$ROOT"

echo "Built: $OUT"
dpkg-deb -I "$OUT" | sed -n '1,20p'
