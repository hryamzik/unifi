#!/bin/sh
# Pack package/utr-lcd for OpenWrt PKG_SOURCE: top-level directory must be utr-lcd-$VER/
# (not the monorepo name "unifi"). Upload the result to the matching GitHub release.
#
#   tools/make-utr-lcd-tarball.sh [VERSION]   default: PKG_VERSION from package/utr-lcd/Makefile
set -eu
cd "$(dirname "$0")/.."
VER=${1:-$(sed -n 's/^PKG_VERSION:=//p' package/utr-lcd/Makefile | head -1)}
[ -n "$VER" ] || { echo "usage: $0 VERSION" >&2; exit 2; }
OUT=utr-lcd-${VER}.tar.gz
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/utr-lcd-$VER"
cp -r package/utr-lcd/src package/utr-lcd/files package/utr-lcd/LICENSE "$STAGE/utr-lcd-$VER/"
tar -C "$STAGE" -czf "$OUT" "utr-lcd-$VER"
echo "wrote $OUT"
sha256sum "$OUT"
