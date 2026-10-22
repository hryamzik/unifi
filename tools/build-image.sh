#!/bin/sh
# Build a full firmware image for the UniFi Travel Router (ubnt,utr / ipq40xx generic) with the
# LCD packages baked in, using the official OpenWrt ImageBuilder for snapshots:
#
#   tools/build-image.sh [--skip-packages]
#
# The two packages are built first (tools/build-package.sh, apk/snapshot flavour) and copied
# into the ImageBuilder's local package directory; the image is assembled with luci added so
# the panel and its LuCI page work out of the box. Output: build/image/*ubnt_utr*.bin|*.itb
# (whatever the profile emits). First run downloads the ImageBuilder (~600 MB) into
# build/imagebuilder/ and caches it there.
#
# NOT flashed anywhere by this script: flash the sysupgrade image from the router's LuCI
# (System > Backup / Flash Firmware) or sysupgrade, keeping kernel0/recovery untouched.
set -eu
cd "$(dirname "$0")/.."

IB_DIR=build/imagebuilder
IB_TARBALL=$IB_DIR/openwrt-imagebuilder-ipq40xx-generic.Linux-x86_64.tar.zst
IB_URL=https://downloads.openwrt.org/snapshots/targets/ipq40xx/generic/openwrt-imagebuilder-ipq40xx-generic.Linux-x86_64.tar.zst
PROFILE=ubnt_utr

if [ "${1:-}" != "--skip-packages" ]; then
	tools/build-package.sh
fi

mkdir -p "$IB_DIR" build/image
if [ ! -d "$IB_DIR/ib" ]; then
	[ -f "$IB_TARBALL" ] || curl -fSL -o "$IB_TARBALL" "$IB_URL"
	mkdir -p "$IB_DIR/ib"
	tar -xf "$IB_TARBALL" -C "$IB_DIR/ib" --strip-components=1
fi

for pkg in build/sdk-out/ipq40xx-generic-master/utr-lcd-*.apk build/sdk-out/ipq40xx-generic-master/luci-app-utr-lcd-*.apk; do
	[ -f "$pkg" ] || { echo "missing $pkg (run without --skip-packages first)" >&2; exit 1; }
	cp "$pkg" "$IB_DIR/ib/packages/"
done

# the ImageBuilder's host tools are Linux binaries: assemble inside a throwaway debian container
docker run --rm -v "$PWD":/w -w /w/"$IB_DIR"/ib debian:bookworm-slim sh -c '
	apt-get update -qq && 	apt-get install -y -qq --no-install-recommends \
		make perl python3 python3-distutils file zstd unzip rsync git wget ca-certificates patch gawk bzip2 >/dev/null
	make image PROFILE='"$PROFILE"' PACKAGES="luci utr-lcd luci-app-utr-lcd" EXTRA_IMAGE_NAME="utr-lcd"'

find "$IB_DIR/ib/bin/targets" -name "*${PROFILE}*" -type f | while read -r f; do
	cp "$f" build/image/
done
ls -la build/image/
