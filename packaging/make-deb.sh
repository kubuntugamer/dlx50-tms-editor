#!/usr/bin/env bash
# Build dlx50-tms-editor_<version>_amd64.deb from an already-built Rust binary.
# Usage: packaging/make-deb.sh <version> <path-to-binary> [outdir]
set -euo pipefail
VERSION="${1:?usage: make-deb.sh <version> <binary> [outdir]}"
BIN="${2:?usage: make-deb.sh <version> <binary> [outdir]}"
OUTDIR="${3:-dist}"
PKG=dlx50-tms-editor
ARCH=amd64
[ -f "$BIN" ] || { echo "FATAL: binary not found: $BIN" >&2; exit 1; }
case "$VERSION" in [0-9]*) ;; *) echo "FATAL: Debian version must start with a digit (got: $VERSION)" >&2; exit 1 ;; esac

declare -A pkgs=()
for needed in $(objdump -p "$BIN" | awk '/NEEDED/{print $2}'); do
  path=$(ldd "$BIN" 2>/dev/null | awk -v lib="$needed" '$1==lib{print $3; exit}') || true
  [ -n "$path" ] || { echo "WARN: could not resolve $needed" >&2; continue; }
  path=$(readlink -f "$path")
  pkg=$(dpkg-query -S "$path" 2>/dev/null | head -1 | sed 's/:.*//') || true
  [ -n "$pkg" ] || { echo "WARN: no package owns $path" >&2; continue; }
  pkgs["${pkg%t64}"]=1
done
depends=$(printf '%s\n' "${!pkgs[@]}" | sort | paste -sd ', ' -)
echo "Derived Depends: ${depends:-none}"
ROOT="out/deb-root"
rm -rf "$ROOT"
mkdir -p "$ROOT/opt/$PKG" "$ROOT/usr/bin" "$ROOT/usr/share/applications" "$ROOT/usr/share/doc/$PKG" "$ROOT/DEBIAN"
install -m 755 "$BIN" "$ROOT/opt/$PKG/tms_editor"
ln -s "/opt/$PKG/tms_editor" "$ROOT/usr/bin/$PKG"
install -m 644 packaging/dlx50-tms-editor.desktop "$ROOT/usr/share/applications/$PKG.desktop"
install -m 644 LICENSE "$ROOT/usr/share/doc/$PKG/copyright"
cat > "$ROOT/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: dlx50-tms-editor maintainers <noreply@github.com>
$(if [ -n "$depends" ]; then printf 'Depends: %s\n' "$depends"; fi)Section: utils
Priority: optional
Homepage: https://github.com/kubuntugamer/dlx50-tms-editor
Installed-Size: $(du -sk "$ROOT/opt" | cut -f1)
Description: Rust TMS / DLX-50-style editor for DETECTO scale files
 Cross-platform Rust desktop editor for DETECTO scale .TMS data files.
 Independent clean-room implementation; not affiliated with Detecto.
EOF
mkdir -p "$OUTDIR"
OUT="$OUTDIR/${PKG}_${VERSION}_${ARCH}.deb"
fakeroot dpkg-deb -b -Zxz "$ROOT" "$OUT"
rm -rf "$ROOT"
echo "Built: $OUT"
dpkg-deb -I "$OUT" | sed -n '1,20p'
